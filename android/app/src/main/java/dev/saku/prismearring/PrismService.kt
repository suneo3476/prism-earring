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
 * 実際に startForeground() へ渡す型は [Params.captureEnabled] に応じて動的に絞る
 * ([startForegroundCompat] 参照)。
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

    /** 実行中の捕獲(コントローラ + エンジン側フラグ)を畳む。[Params.captureEnabled] は変えない。 */
    private fun teardownCaptureRuntime() {
        captureController?.stop()
        captureController = null
        engine?.setCaptureEnabled(false)
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
        // 起動直後の startProcessing() 経由ならここは型の変化なし(既に含めて呼ばれている)、
        // 動作中に捕獲だけを ON にした場合はここで初めて型が追加される。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            try {
                startForegroundCompat()
            } catch (t: Throwable) {
                Log.e(TAG, "捕獲用の前景化に失敗", t)
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
     * Android 14 以降は、`getMediaProjection` を呼ぶ **前** に mediaProjection 型で
     * 前景化しておく必要がある(順序を逆にすると SecurityException)。捕獲が無効な
     * ときは従来どおり microphone + mediaPlayback の 2 型だけで前景化する。
     */
    private fun startForegroundCompat() {
        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            var types = ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE or
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            if (params.captureEnabled) {
                types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            }
            startForeground(NOTIFICATION_ID, notification, types)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun stopForegroundCompat() {
        stopForeground(STOP_FOREGROUND_REMOVE)
    }

    companion object {
        private const val TAG = "prism"
        private const val CHANNEL_ID = "prism_processing"
        private const val NOTIFICATION_ID = 1
        private const val POLL_INTERVAL_MS = 1000L

        const val ACTION_START = "dev.saku.prismearring.action.START"
        const val ACTION_STOP = "dev.saku.prismearring.action.STOP"

        fun startIntent(context: Context): Intent =
            Intent(context, PrismService::class.java).setAction(ACTION_START)

        fun stopIntent(context: Context): Intent =
            Intent(context, PrismService::class.java).setAction(ACTION_STOP)
    }
}
