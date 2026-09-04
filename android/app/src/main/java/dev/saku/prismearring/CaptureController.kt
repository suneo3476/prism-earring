package dev.saku.prismearring

import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Process
import android.util.Log

/**
 * 他アプリの再生音(AudioPlaybackCapture)を捕獲し、[NativeEngine.pushCapture] へ
 * 流し込む係。MediaProjection / AudioRecord / 録音スレッドの生存管理をここに閉じ込め、
 * [PrismService] は [start] / [stop] を呼ぶだけにする。
 *
 * Oboe / AAudio にはこの API が無いため、ここだけは Java 側(AudioRecord)で完結する。
 * ネイティブ側は「サンプルを受け取る口」(pushCapture)だけを持つ。
 */
class CaptureController(
    private val context: Context,
    private val engine: NativeEngine,
    /** 捕獲を開始・継続できなかったときに呼ばれる(UI へのエラー表示用)。 */
    private val onError: (String) -> Unit,
    /** MediaProjection がシステム側(通知の「停止」等)から止められたときに呼ばれる。 */
    private val onProjectionStopped: () -> Unit,
) {
    private var projection: MediaProjection? = null
    private var projectionCallback: MediaProjection.Callback? = null
    private var audioRecord: AudioRecord? = null
    private var thread: Thread? = null

    @Volatile
    private var running = false

    val isActive: Boolean get() = running

    /**
     * 捕獲を開始する。[resultCode] / [data] は
     * `MediaProjectionManager.createScreenCaptureIntent()` の同意結果そのもの。
     * 呼ぶ前提として [NativeEngine] は既に `start()` 済みであること
     * (捕獲用 AudioRecord をエンジンの出力サンプルレートに合わせて開くため)。
     */
    fun start(resultCode: Int, data: Intent) {
        stop() // 二重起動しない

        val sampleRate = engine.streamInfo().sampleRate
        if (sampleRate <= 0) {
            onError(context.getString(R.string.capture_error_engine_not_running))
            return
        }

        try {
            val manager =
                context.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
            val proj = manager.getMediaProjection(resultCode, data)
            projection = proj
            val callback = object : MediaProjection.Callback() {
                override fun onStop() {
                    onProjectionStopped()
                    stop()
                }
            }
            projectionCallback = callback
            proj.registerCallback(callback, null)

            val config = AudioPlaybackCaptureConfiguration.Builder(proj)
                .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                .addMatchingUsage(AudioAttributes.USAGE_GAME)
                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                .build()

            val channelMask = AudioFormat.CHANNEL_IN_STEREO
            var useFloat = true
            var minBuf = AudioRecord.getMinBufferSize(
                sampleRate,
                channelMask,
                AudioFormat.ENCODING_PCM_FLOAT,
            )
            var encoding = AudioFormat.ENCODING_PCM_FLOAT
            if (minBuf <= 0) {
                useFloat = false
                encoding = AudioFormat.ENCODING_PCM_16BIT
                minBuf = AudioRecord.getMinBufferSize(sampleRate, channelMask, encoding)
            }
            if (minBuf <= 0) {
                onError(context.getString(R.string.capture_error_buffer))
                stop()
                return
            }

            val format = AudioFormat.Builder()
                .setEncoding(encoding)
                .setSampleRate(sampleRate)
                .setChannelMask(channelMask)
                .build()

            val record = AudioRecord.Builder()
                .setAudioFormat(format)
                .setBufferSizeInBytes(minBuf * 2)
                .setAudioPlaybackCaptureConfig(config)
                .build()

            if (record.state != AudioRecord.STATE_INITIALIZED) {
                record.release()
                onError(context.getString(R.string.capture_error_open))
                stop()
                return
            }

            audioRecord = record
            record.startRecording()
            running = true

            // 約 10ms ぶんずつ読む。
            val chunkFrames = (sampleRate / 100).coerceAtLeast(64)
            thread = Thread({ runLoop(record, useFloat, chunkFrames) }, THREAD_NAME).apply {
                start()
            }
        } catch (t: Throwable) {
            Log.e(TAG, "捕獲の開始に失敗しました", t)
            onError(
                context.getString(R.string.capture_error_generic, t.message ?: t.toString()),
            )
            stop()
        }
    }

    private fun runLoop(record: AudioRecord, useFloat: Boolean, chunkFrames: Int) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        val channels = 2
        val floatBuf = FloatArray(chunkFrames * channels)
        val shortBuf = if (useFloat) null else ShortArray(chunkFrames * channels)
        try {
            while (running) {
                val read = if (useFloat) {
                    record.read(floatBuf, 0, floatBuf.size, AudioRecord.READ_BLOCKING)
                } else {
                    val n = record.read(shortBuf!!, 0, shortBuf.size)
                    if (n > 0) {
                        for (i in 0 until n) {
                            floatBuf[i] = shortBuf[i] / 32768.0f
                        }
                    }
                    n
                }
                if (read > 0) {
                    engine.pushCapture(floatBuf, read / channels, channels)
                } else if (read < 0) {
                    // AudioRecord のエラーコード(ERROR_INVALID_OPERATION 等)。継続しない。
                    Log.e(TAG, "捕獲の read が失敗しました: $read")
                    break
                }
            }
        } catch (t: Throwable) {
            Log.e(TAG, "捕獲スレッドで例外が発生しました", t)
        }
    }

    /** 何度呼んでも安全。 */
    fun stop() {
        running = false
        thread?.let {
            try {
                it.join(200)
            } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }
        thread = null
        audioRecord?.let {
            try {
                it.stop()
            } catch (e: IllegalStateException) {
                // 未開始 / 既に停止済み。無視して解放へ進む。
            }
            it.release()
        }
        audioRecord = null
        // 自分で止めるときはコールバックを先に外す。外さないと projection.stop() が
        // onStop をメインスレッドへ遅れて配送し、直後に始めた新しい捕獲を巻き込む。
        projection?.let { proj ->
            projectionCallback?.let { proj.unregisterCallback(it) }
            proj.stop()
        }
        projectionCallback = null
        projection = null
    }

    companion object {
        private const val TAG = "prism-capture"
        private const val THREAD_NAME = "prism-capture"
    }
}
