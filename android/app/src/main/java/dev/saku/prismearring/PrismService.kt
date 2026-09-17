package dev.saku.prismearring

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import kotlin.math.roundToInt

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

    /** 診断用の 10 秒録音。null なら録音していない。1 回使い切りで、完了/失敗のたびに null へ戻す。 */
    private var diagnosticRecorder: DiagnosticRecorder? = null

    private val audioManager: AudioManager by lazy {
        getSystemService(Context.AUDIO_SERVICE) as AudioManager
    }

    /**
     * 「原音の抑え込み」で STREAM_MUSIC を直接絞っている間の、絞る前の index。
     * null なら絞っていない(= 復元済み、または元々絞っていない)。
     */
    private var duckedOriginalVolumeIndex: Int? = null

    /**
     * 出力用途がユーザー補助のときに STREAM_ACCESSIBILITY を最大へ上げる前の index。
     * null なら上げていない(= 復元済み、または上げようとして失敗した)。
     */
    private var accessibilityOriginalVolumeIndex: Int? = null

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
        pushBluetoothDeviceIds(e)
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
        applyAccessibilityVolumeState()
        applyDuckState()
        publishState()
        return true
    }

    /**
     * 通知の「停止」・エラーでの停止・[onDestroy] のいずれもここを通る単一の停止経路。
     * 「原音の抑え込み」で絞ったメディア音量は、どの経路で止まってもここで必ず復元する。
     */
    fun stopProcessing() {
        diagnosticRecorder?.cancel()
        diagnosticRecorder = null
        teardownCaptureRuntime()
        engine?.stop()
        stopPolling()
        state = state.copy(running = false, synced = false)
        stopForegroundCompat()
        restoreDuckedVolume()
        restoreAccessibilityVolume()
        publishState()
    }

    // ---- 出力バッファの初期サイズ(Bluetooth 判定)-------------------------------
    // Bluetooth(A2DP / LE / SCO)はコールバックの起床ジッタが大きく、バースト
    // 2 個ぶんのバッファで開くと xrun が出続ける。エンジンが「実際に開いた出力
    // デバイスが Bluetooth かどうか」を判定できるよう、現在見えている Bluetooth
    // 出力デバイスの ID 一覧を start() の直前に渡しておく(出力先が「自動」でも、
    // 実際に開いた ID と突き合わせられる)。

    private fun isBluetoothOutputType(type: Int): Boolean = when (type) {
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
        AudioDeviceInfo.TYPE_BLE_HEADSET,
        AudioDeviceInfo.TYPE_BLE_SPEAKER,
        AudioDeviceInfo.TYPE_BLE_BROADCAST,
        -> true

        else -> false
    }

    private fun pushBluetoothDeviceIds(e: NativeEngine) {
        val ids = try {
            audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
                .filter { isBluetoothOutputType(it.type) }
                .map { it.id }
                .toIntArray()
        } catch (t: Throwable) {
            Log.e(TAG, "Bluetooth 出力デバイスの列挙に失敗", t)
            IntArray(0)
        }
        e.setBluetoothOutputDeviceIds(ids)
    }

    // ---- 診断用の 10 秒録音 ---------------------------------------------------
    // 捕獲 in/out・マイク in/out を同時刻に開始して 10 秒間 WAV へ保存する。
    // 実体は [DiagnosticRecorder]。ここは 1 回使い切りのインスタンスを生成して
    // 開始するだけ(完了・失敗のどちらでも [diagnosticRecorder] を null へ戻す)。

    fun isDiagnosticRecording(): Boolean = diagnosticRecorder?.isActive == true

    /**
     * 成功したら true(結果は [onSaved]/[onError] へ非同期に届く)。動作中でない・
     * 既に録音中・ネイティブ側の確保に失敗、のいずれかなら false
     * (この場合コールバックは呼ばれない)。
     */
    fun startDiagnosticRecording(onSaved: (String) -> Unit, onError: (String) -> Unit): Boolean {
        val e = engine ?: return false
        if (!e.isRunning()) return false
        if (diagnosticRecorder?.isActive == true) return false

        val info = e.streamInfo()
        val latency = e.latency()
        val meta = DiagnosticRecorder.Meta(
            framesPerBurst = info.framesPerBurst,
            outputDeviceId = info.outputDeviceId,
            inputDeviceId = info.inputDeviceId,
            inputPreset = params.inputPreset,
            micSweepMs = latency.micSweepMs,
            captureSweepMs = latency.captureSweepMs,
            crossfadeMs = params.crossfadeMs,
            shiftCentsL = params.shiftCentsL,
            shiftCentsR = params.effectiveRight,
            outputGainDb = params.outputGainDb,
            micGainDb = params.micGainDb,
            captureGainDb = params.captureGainDb,
            captureEnabled = params.captureEnabled,
            outputXRunCount = info.outputXRunCount,
            inputXRunCount = info.inputXRunCount,
            micShortfallCount = info.underruns,
            micShortfallFrames = info.micShortfallFrames,
            captureUnderrunCount = info.captureUnderruns,
            captureOverrunCount = info.captureOverruns,
            captureShortfallFrames = info.captureShortfallFrames,
        )

        val recorder = DiagnosticRecorder(this, e)
        val started = recorder.start(
            meta = meta,
            onSaved = { path ->
                diagnosticRecorder = null
                onSaved(path)
            },
            onError = { message ->
                diagnosticRecorder = null
                onError(message)
            },
        )
        if (started) diagnosticRecorder = recorder
        return started
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
                applyDuckState()
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
        applyDuckState()
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
        applyDuckState()
        publishState()
    }

    fun isCapturing(): Boolean = captureController?.isActive == true

    // ---- 原音の抑え込み(メディア音量 STREAM_MUSIC の直接制御)-----------------------
    // 逆相打ち消しではなく、原音の出力経路そのもの(メディア音量)を絞ることで消音する
    // (詳しくは README「捕獲音のミックス」節)。[Params.duckAvailable] の条件
    // (動作中 + 捕獲 ON + 出力用途がユーザー補助)が崩れた瞬間に必ず元の音量へ戻す。

    /**
     * 現在の [params] と実行状態から、抑え込みを効かせるべきかどうかを判定し、
     * 必要なら STREAM_MUSIC の音量を書き換える(または元へ戻す)。
     * [startProcessing] / [stopProcessing] / [startCapture] / [stopCapture] /
     * [updateParams] の末尾から呼ぶ(状態が変わりうる箇所すべて)。副作用は
     * 冪等 — 条件も値も変わっていなければ何もしない。
     */
    private fun applyDuckState() {
        if (params.duckAvailable(isRunning())) {
            duckToward(params.duckPercent)
        } else {
            restoreDuckedVolume()
        }
    }

    private fun duckToward(percent: Int) {
        val original = duckedOriginalVolumeIndex ?: try {
            audioManager.getStreamVolume(AudioManager.STREAM_MUSIC).also {
                duckedOriginalVolumeIndex = it
            }
        } catch (t: Throwable) {
            Log.e(TAG, "現在のメディア音量を取得できません", t)
            return
        }
        val clamped = percent.coerceIn(Params.DUCK_PERCENT_MIN, Params.DUCK_PERCENT_MAX)
        // 100% = 0、0% = 絞る前の index のまま。
        val target = (original - (original * (clamped / 100.0)).roundToInt())
            .coerceIn(0, original)
        try {
            audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, target, 0)
        } catch (t: Throwable) {
            Log.e(TAG, "メディア音量の変更に失敗", t)
        }
    }

    /** 絞る前の音量へ戻す。絞っていなければ何もしない。 */
    private fun restoreDuckedVolume() {
        val original = duckedOriginalVolumeIndex ?: return
        duckedOriginalVolumeIndex = null
        try {
            audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, original, 0)
        } catch (t: Throwable) {
            Log.e(TAG, "メディア音量の復元に失敗", t)
        }
    }

    // ---- パラメータ ---------------------------------------------------------

    fun currentParams(): Params = params

    /** 動作中でも呼んでよい。内部は std::atomic 経由。 */
    fun updateParams(next: Params) {
        params = next
        engine?.let { next.applyTo(it) }
        next.save(this)
        applyAccessibilityVolumeState()
        applyDuckState()
    }

    // ---- ユーザー補助系統の音量(STREAM_ACCESSIBILITY)---------------------------
    // 出力用途をユーザー補助にすると、本アプリの音はメディアではなくユーザー補助の
    // 音量つまみに乗る。この系統は既定値が小さいことがあり、実機(Pixel 9a +
    // Bluetooth)では「アプリ側の音量を最大にしてやっと聞こえる」状態になった。
    // そこで開始時に一度だけ最大へ上げ(元の値を保存)、停止時に必ず戻す。
    //
    // 注意: STREAM_ACCESSIBILITY の音量変更は、ユーザー補助サービスでないアプリには
    // OS が黙って無視する(AudioService の CHANGE_ACCESSIBILITY_VOLUME 判定。例外は
    // 飛ばず、ログが出るだけ)。そのため [setAccessibilityVolume] は書き込んだあとに
    // 読み戻し、反映されたかどうかを返す。反映されない端末では、Activity 側が
    // volumeControlStream をこの系統へ向けてあるので、端末の音量キーで操作できる。

    private fun applyAccessibilityVolumeState() {
        if (isRunning() && params.outputUsage == NativeEngine.USAGE_ACCESSIBILITY) {
            boostAccessibilityVolumeOnce()
        } else {
            restoreAccessibilityVolume()
        }
    }

    /** 一度だけ最大へ上げる。以後の音量は利用者(スライダ・音量キー)に委ねる。 */
    private fun boostAccessibilityVolumeOnce() {
        if (accessibilityOriginalVolumeIndex != null) return
        try {
            accessibilityOriginalVolumeIndex =
                audioManager.getStreamVolume(AudioManager.STREAM_ACCESSIBILITY)
            audioManager.setStreamVolume(
                AudioManager.STREAM_ACCESSIBILITY,
                audioManager.getStreamMaxVolume(AudioManager.STREAM_ACCESSIBILITY),
                0,
            )
        } catch (t: Throwable) {
            Log.e(TAG, "ユーザー補助系統の音量を最大にできません", t)
        }
    }

    private fun restoreAccessibilityVolume() {
        val original = accessibilityOriginalVolumeIndex ?: return
        accessibilityOriginalVolumeIndex = null
        try {
            audioManager.setStreamVolume(AudioManager.STREAM_ACCESSIBILITY, original, 0)
        } catch (t: Throwable) {
            Log.e(TAG, "ユーザー補助系統の音量を復元できません", t)
        }
    }

    fun accessibilityVolumeMax(): Int = try {
        audioManager.getStreamMaxVolume(AudioManager.STREAM_ACCESSIBILITY)
    } catch (t: Throwable) {
        Log.e(TAG, "ユーザー補助系統の最大音量を取得できません", t)
        0
    }

    fun accessibilityVolumeIndex(): Int = try {
        audioManager.getStreamVolume(AudioManager.STREAM_ACCESSIBILITY)
    } catch (t: Throwable) {
        Log.e(TAG, "ユーザー補助系統の音量を取得できません", t)
        0
    }

    /**
     * ユーザー補助系統の音量を直接設定する。
     * @return 実際に反映されたら true(OS に無視された場合は false)。
     */
    fun setAccessibilityVolume(index: Int): Boolean {
        val max = accessibilityVolumeMax()
        if (max <= 0) return false
        val target = index.coerceIn(0, max)
        return try {
            audioManager.setStreamVolume(AudioManager.STREAM_ACCESSIBILITY, target, 0)
            audioManager.getStreamVolume(AudioManager.STREAM_ACCESSIBILITY) == target
        } catch (t: Throwable) {
            Log.e(TAG, "ユーザー補助系統の音量を変更できません", t)
            false
        }
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
