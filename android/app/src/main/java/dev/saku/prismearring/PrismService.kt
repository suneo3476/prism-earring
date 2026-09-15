package dev.saku.prismearring

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log

/**
 * 常駐オーディオ処理。
 *
 * 画面を消しても、他のアプリに切り替えてもストリームを維持する必要があるため
 * ForegroundService にする。Activity は bind して状態を読むだけで、
 * エンジンの所有者はこの Service。
 *
 * foregroundServiceType は microphone + mediaPlayback + mediaProjection。マイクを開き、
 * 音を鳴らし続け、v0.4.0 からは他アプリの再生音も捕獲しうるので 3 つとも該当する
 * (宣言に無い型だと Android 14 以降で ForegroundServiceTypeException になりうる)。
 *
 * ただし実際に `startForeground()` へ渡す型は常に microphone + mediaPlayback だけ
 * ([startForegroundCompat] 参照)。mediaProjection 型は、新しい MediaProjection の
 * 同意結果(`resultCode`/`data`)を受け取った直後にだけ
 * [startForegroundWithCaptureType] で足す。Android 14 以降は `android:project_media`
 * app-op が同意結果でしか付与されないため、**同意 → startForeground(mediaProjection
 * 込み) → getMediaProjection()** の順序を守らないと `startForeground` が
 * `SecurityException`(`missing permissions: ... FOREGROUND_SERVICE_MEDIA_PROJECTION ...`)
 * で失敗する。設定変更などによる Service の自動再起動(stop → start)では、
 * 直前に同意を得ていても [CaptureController.stop] 経由で MediaProjection 自体を
 * 破棄しているため、起動直後の `startForeground` に mediaProjection 型を含めては
 * いけない(v0.4.1 で修正: 以前は [Params.captureEnabled] を見て型を決めていたため、
 * 再起動のたびにこの順序が崩れて起動が落ちていた)。
 */
class PrismService : Service() {

    /** UI 向けの観測可能な状態。すべてメインスレッドから読む。 */
    data class State(
        val running: Boolean = false,
        val synced: Boolean = false,
        val latency: NativeEngine.Latency = NativeEngine.Latency(0.0, 0.0, 0.0, 0.0, false),
        val info: NativeEngine.StreamInfo = NativeEngine.StreamInfo(0, 0, 0, 0, 0, 0, false, false),
        val error: String = "",
        /** 他アプリの音を実際に捕獲中(MediaProjection 取得済み・録音スレッド稼働中)か。 */
        val captureActive: Boolean = false,
    )

    fun interface StateListener {
        fun onState(state: State)
    }

    inner class LocalBinder : Binder() {
        val service: PrismService get() = this@PrismService
    }

    private val binder = LocalBinder()
    private val handler = Handler(Looper.getMainLooper())

    private var engine: NativeEngine? = null
    private var params: Params = Params()
    private var listener: StateListener? = null
    private var polling = false

    /** 他アプリの音を拾う(AudioPlaybackCapture)係。null なら捕獲していない。 */
    private var captureController: CaptureController? = null

    @Volatile
    var state: State = State()
        private set

    // 1 秒ごとに遅延と診断値を取り直す。オーディオスレッドには一切触らない。
    private val poller = object : Runnable {
        override fun run() {
            publishState()
            if (polling) handler.postDelayed(this, POLL_INTERVAL_MS)
        }
    }

    override fun onCreate() {
        super.onCreate()
        params = Params.load(this)
        engine = NativeEngine.create()
        if (engine == null) {
            state = state.copy(error = "ネイティブエンジンを初期化できませんでした")
        } else {
            params.applyTo(engine!!)
        }
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> startProcessing()
            ACTION_STOP -> {
                stopProcessing()
                stopSelf()
            }
            else -> {
                // システムによる再作成。処理は再開せず、待機のまま終わる。
                if (!isRunning()) stopSelf()
            }
        }
        // 明示的に開始/停止する設計なので、殺されたら黙って終わる方が安全
        // (勝手にマイクを開き直さない)。
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        stopProcessing()
        engine?.release()
        engine = null
        super.onDestroy()
    }

    // ---- 制御 --------------------------------------------------------------

    fun isRunning(): Boolean = engine?.isRunning() == true

    /** 成功したら true。失敗理由は [State.error]。 */
    fun startProcessing(): Boolean {
        val e = engine
        if (e == null) {
            state = state.copy(running = false, error = "ネイティブエンジンがありません")
            publishState()
            return false
        }
        if (e.isRunning()) return true

        // 先に前景化する。startForeground より前に長い処理を挟むと
        // ForegroundServiceDidNotStartInTimeException になる。
        try {
            startForegroundCompat()
        } catch (t: Throwable) {
            Log.e(TAG, "startForeground に失敗", t)
            state = state.copy(running = false, error = "前景サービスを開始できません: ${t.message}")
            publishState()
            return false
        }

        params.applyTo(e)
        val ok = e.start()
        if (!ok) {
            val message = e.lastError().ifEmpty { "オーディオストリームを開けませんでした" }
            Log.e(TAG, "engine.start 失敗: $message")
            state = state.copy(running = false, error = message)
            stopForegroundCompat()
            publishState()
            return false
        }

        state = state.copy(running = true, error = "")
        startPolling()
        publishState()
        return true
    }

    fun stopProcessing() {
        teardownCaptureRuntime()
        engine?.stop()
        stopPolling()
        state = state.copy(running = false, synced = false)
        stopForegroundCompat()
        publishState()
    }

    // ---- 捕獲(他アプリの再生音)----------------------------------------------
    // AudioPlaybackCapture は Oboe に無いため Java 側(AudioRecord)で完結させる。
    // 実体は CaptureController。ここは start/stop を呼ぶだけ。
    //
    // [Params.captureEnabled] を触るのは [startCapture] / [stopCapture](ユーザーの
    // 明示的な意図)だけ。エンジン全体の停止([stopProcessing] 経由の
    // [teardownCaptureRuntime])や外部からの取り消しでは、次回開始時に同じ設定で
    // 再同意を求められるよう、この設定値自体はそのまま残す。

    /**
     * 実行中の捕獲(コントローラ + エンジン側フラグ)を畳む。[Params.captureEnabled] は変えない。
     * [CaptureController.stop] が MediaProjection 自体を破棄するため、エンジンがまだ
     * 動作中なら前景化の型も microphone + mediaPlayback だけへ縮退させておく
     * (mediaProjection 型のまま残すと、次に同意なしで `startForeground` を呼んだときに
     * 落ちる余地を残してしまう)。
     */
    private fun teardownCaptureRuntime() {
        captureController?.stop()
        captureController = null
        engine?.setCaptureEnabled(false)
        if (isRunning() && Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            try {
                startForegroundCompat()
            } catch (t: Throwable) {
                Log.e(TAG, "捕獲終了後の前景化縮退に失敗", t)
            }
        }
    }

    /**
     * 捕獲を開始する。[resultCode] / [data] は
     * `MediaProjectionManager.createScreenCaptureIntent()` の同意結果。
     * エンジンが動作中でなければ失敗する(捕獲用ストリームのサンプルレートが決まらないため)。
     * 成功したら [Params.captureEnabled] を true にして保存する。
     */
    fun startCapture(resultCode: Int, data: Intent): Boolean {
        val e = engine
        if (e == null || !e.isRunning()) return false
        teardownCaptureRuntime()

        params = params.copy(captureEnabled = true)
        params.save(this)

        // Android 14 以降は、getMediaProjection の **前** に mediaProjection 型を
        // 含めて前景化し直す必要がある(順序を逆にすると SecurityException)。
        // [resultCode]/[data] は呼び出し側(MainActivity)が直前に得た同意結果そのものなので、
        // ここで初めて mediaProjection 型を足すのが安全(startProcessing() 側は常に
        // microphone + mediaPlayback のみで、型を先取りしない)。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            try {
                startForegroundWithCaptureType()
            } catch (t: Throwable) {
                // SecurityException / ForegroundServiceStartNotAllowedException 等。
                // 落とさず、捕獲を OFF に戻してマイク経路だけで続行する。
                Log.e(TAG, "捕獲用の前景化に失敗", t)
                params = params.copy(captureEnabled = false)
                params.save(this)
                state = state.copy(error = "捕獲を開始できません: ${t.message}")
                publishState()
                return false
            }
        }

        e.setCaptureEnabled(true)
        val controller = CaptureController(
            context = this,
            engine = e,
            onError = { message ->
                handler.post {
                    state = state.copy(error = message)
                    listener?.onState(state)
                }
            },
            onProjectionStopped = {
                handler.post {
                    teardownCaptureRuntime()
                    publishState()
                }
            },
        )
        captureController = controller
        controller.start(resultCode, data)
        publishState()
        return true
    }

    /** ユーザーが明示的に捕獲を止める。[Params.captureEnabled] も false にして保存する。 */
    fun stopCapture() {
        teardownCaptureRuntime()
        if (params.captureEnabled) {
            params = params.copy(captureEnabled = false)
            params.save(this)
        }
        publishState()
    }

    fun isCapturing(): Boolean = captureController?.isActive == true

    // ---- パラメータ ---------------------------------------------------------

    fun currentParams(): Params = params

    /** 動作中でも呼んでよい。内部は std::atomic 経由。 */
    fun updateParams(next: Params) {
        params = next
        engine?.let { next.applyTo(it) }
        next.save(this)
    }

    // ---- 状態通知 -----------------------------------------------------------

    fun setListener(l: StateListener?) {
        listener = l
        if (l != null) publishState()
    }

    private fun startPolling() {
        if (polling) return
        polling = true
        handler.post(poller)
    }

    private fun stopPolling() {
        polling = false
        handler.removeCallbacks(poller)
    }

    private fun publishState() {
        val e = engine
        state = if (e == null) {
            state.copy(running = false, synced = false)
        } else {
            state.copy(
                running = e.isRunning(),
                synced = e.isSynced(),
                latency = e.latency(),
                info = e.streamInfo(),
                captureActive = captureController?.isActive == true,
            )
        }
        listener?.onState(state)
    }

    // ---- 通知 ---------------------------------------------------------------

    private fun createNotificationChannel() {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (manager.getNotificationChannel(CHANNEL_ID) != null) return
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.notif_channel_name),
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = getString(R.string.notif_channel_desc)
            setShowBadge(false)
            enableVibration(false)
            setSound(null, null)
        }
        manager.createNotificationChannel(channel)
    }

    private fun buildNotification(): Notification {
        val openIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val stopIntent = PendingIntent.getService(
            this,
            1,
            Intent(this, PrismService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.notif_title))
            .setContentText(getString(R.string.notif_text))
            .setSmallIcon(R.drawable.ic_notification)
            .setContentIntent(openIntent)
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .addAction(
                Notification.Action.Builder(
                    null,
                    getString(R.string.notif_action_stop),
                    stopIntent,
                ).build()
            )
            .build()
    }

    /**
     * 通常の前景化。**常に** microphone + mediaPlayback の 2 型だけ
     * (mediaProjection 型は含めない)。エンジンの開始・再起動はすべてここを通るため、
     * [Params.captureEnabled] の値(＝ユーザーの意図。実際の同意の有無とは別)で
     * 型を変えてはいけない — 変えると、同意が無い(または破棄済みの)状態で
     * mediaProjection 型を宣言することになり、Android 14 以降で
     * `startForeground` が `SecurityException` になる。
     */
    private fun startForegroundCompat() {
        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            val types = ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE or
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            startForeground(NOTIFICATION_ID, notification, types)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    /**
     * mediaProjection 型を含めて前景化し直す。呼び出し前提: 直前に新しい
     * `MediaProjectionManager.createScreenCaptureIntent()` の同意結果(`resultCode`/`data`)
     * を受け取っていること。[startCapture] からのみ、`getMediaProjection()` を呼ぶ
     * **直前**に呼ぶ。SDK 34 未満では呼ばない(マニフェストの宣言だけで足りる)。
     */
    private fun startForegroundWithCaptureType() {
        val notification = buildNotification()
        val types = ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE or
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK or
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
        startForeground(NOTIFICATION_ID, notification, types)
    }

    private fun stopForegroundCompat() {
        stopForeground(STOP_FOREGROUND_REMOVE)
    }

    /**
     * 捕獲 ON のまま設定変更で再起動したが、Activity が前面になく同意画面を
     * 出せなかった場合に呼ぶ(MainActivity から)。捕獲は OFF のままとし
     * (mediaProjection 型では前景化しない)、通知で「アプリを開いて再開してください」
     * と案内するだけにとどめる。
     */
    fun notifyReopenToResumeCapture() {
        try {
            val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            val openIntent = PendingIntent.getActivity(
                this,
                REOPEN_REQUEST_CODE,
                Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
            val notification = Notification.Builder(this, CHANNEL_ID)
                .setContentTitle(getString(R.string.notif_title))
                .setContentText(getString(R.string.capture_reopen_hint))
                .setSmallIcon(R.drawable.ic_notification)
                .setContentIntent(openIntent)
                .setAutoCancel(true)
                .build()
            manager.notify(REOPEN_NOTIFICATION_ID, notification)
        } catch (t: Throwable) {
            Log.e(TAG, "再開案内の通知に失敗", t)
        }
    }

    companion object {
        private const val TAG = "prism"
        private const val CHANNEL_ID = "prism_processing"
        private const val NOTIFICATION_ID = 1
        private const val REOPEN_NOTIFICATION_ID = 2
        private const val REOPEN_REQUEST_CODE = 2
        private const val POLL_INTERVAL_MS = 1000L

        const val ACTION_START = "dev.saku.prismearring.action.START"
        const val ACTION_STOP = "dev.saku.prismearring.action.STOP"

        fun startIntent(context: Context): Intent =
            Intent(context, PrismService::class.java).setAction(ACTION_START)

        fun stopIntent(context: Context): Intent =
            Intent(context, PrismService::class.java).setAction(ACTION_STOP)
    }
}
