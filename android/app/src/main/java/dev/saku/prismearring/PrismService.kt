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
 * foregroundServiceType は microphone + mediaPlayback。マイクを開き、かつ
 * 音を鳴らし続けるので両方に該当する(片方だけの宣言だと Android 14 以降で
 * ForegroundServiceTypeException になりうる)。
 */
class PrismService : Service() {

    /** UI 向けの観測可能な状態。すべてメインスレッドから読む。 */
    data class State(
        val running: Boolean = false,
        val synced: Boolean = false,
        val latency: NativeEngine.Latency = NativeEngine.Latency(0.0, 0.0, 0.0, 0.0, false),
        val info: NativeEngine.StreamInfo = NativeEngine.StreamInfo(0, 0, 0, 0, 0, 0, false, false),
        val error: String = "",
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
        engine?.stop()
        stopPolling()
        state = state.copy(running = false, synced = false)
        stopForegroundCompat()
        publishState()
    }

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

    private fun startForegroundCompat() {
        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE or
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK,
            )
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
