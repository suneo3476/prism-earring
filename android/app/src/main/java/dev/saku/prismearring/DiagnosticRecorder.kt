package dev.saku.prismearring

import android.content.ContentValues
import android.content.Context
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import android.util.Log
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.roundToInt

/**
 * 診断用の 10 秒録音。捕獲経路(シフト前/シフト後)とマイク経路(シフト前/シフト後)の
 * 計 4 本を同時刻に開始して 10 秒間 WAV として `Downloads/katsuo/` へ保存する。
 *
 * ネイティブ側([NativeEngine])が音声スレッドで固定バッファへ [memcpy] するだけで、
 * ファイル I/O(WAV エンコード・MediaStore 保存)はすべてここ(Kotlin 側、
 * ワーカースレッド)の責務にする — 音声スレッドはヒープ確保・ロック・I/O を
 * 一切行わない(CLAUDE.md「リアルタイムオーディオの鉄則」)。
 *
 * ライフサイクルは 1 回使い切り: [start] を呼んだらポーリングで完了を待ち、
 * 完了したらワーカースレッドで保存して [onSaved] / [onError] のどちらかを
 * 一度だけ呼ぶ。同じインスタンスを再利用しない。
 */
class DiagnosticRecorder(
    private val context: Context,
    private val engine: NativeEngine,
) {
    private val handler = Handler(Looper.getMainLooper())
    private var pollRunnable: Runnable? = null
    private var worker: Thread? = null

    @Volatile
    var isActive = false
        private set

    /**
     * 開始できたら true。開始できなかった場合([onSaved]/[onError] のどちらも
     * 呼ばれない)は、動作中でない・ネイティブ側の確保に失敗した・既に録音中、
     * のいずれか。
     */
    fun start(
        meta: Meta,
        onSaved: (String) -> Unit,
        onError: (String) -> Unit,
    ): Boolean {
        if (isActive) return false
        if (!engine.startDiagnosticRecording()) return false
        isActive = true
        pollForCompletion(meta, onSaved, onError)
        return true
    }

    private fun pollForCompletion(meta: Meta, onSaved: (String) -> Unit, onError: (String) -> Unit) {
        val check = object : Runnable {
            override fun run() {
                if (!isActive) return
                when {
                    engine.isDiagnosticRecordingDone() -> {
                        isActive = false
                        pollRunnable = null
                        saveInBackground(meta, onSaved, onError)
                    }
                    !engine.isRunning() -> {
                        // 処理が止まった。ネイティブ側のバッファには触れず
                        // (音声スレッドがどこまで書いたか確定できないため)、
                        // 次回の start() が自然に片付けるのに任せる。
                        isActive = false
                        pollRunnable = null
                        engine.cancelDiagnosticRecording()
                        onError(context.getString(R.string.diag_record_error_stopped))
                    }
                    else -> handler.postDelayed(this, POLL_INTERVAL_MS)
                }
            }
        }
        pollRunnable = check
        handler.postDelayed(check, POLL_INTERVAL_MS)
    }

    private fun saveInBackground(meta: Meta, onSaved: (String) -> Unit, onError: (String) -> Unit) {
        val thread = Thread({
            try {
                val label = writeFiles(meta)
                handler.post { onSaved(label) }
            } catch (t: Throwable) {
                Log.e(TAG, "診断録音の保存に失敗しました", t)
                val message = context.getString(
                    R.string.diag_record_error_save,
                    t.message ?: t.toString(),
                )
                handler.post { onError(message) }
            } finally {
                // 録音は自然完了済み(isDiagnosticRecordingDone() を確認してから
                // ここへ来ている)なので、音声スレッドはもう触らない。解放してよい。
                engine.releaseDiagnosticRecording()
            }
        }, THREAD_NAME)
        worker = thread
        thread.start()
    }

    private fun writeFiles(meta: Meta): String {
        val sampleRate = engine.streamInfo().sampleRate
        val inputChannels = engine.diagnosticInputChannels().coerceAtLeast(1)
        val totalFrames = engine.diagnosticTotalFrames()
        if (sampleRate <= 0 || totalFrames <= 0) {
            throw IllegalStateException(context.getString(R.string.diag_record_error_no_data))
        }

        val captureIn = engine.fetchDiagnosticCaptureIn()
            ?: throw IllegalStateException(context.getString(R.string.diag_record_error_no_data))
        val captureOut = engine.fetchDiagnosticCaptureOut()
            ?: throw IllegalStateException(context.getString(R.string.diag_record_error_no_data))
        val micIn = engine.fetchDiagnosticMicIn()
            ?: throw IllegalStateException(context.getString(R.string.diag_record_error_no_data))
        val micOut = engine.fetchDiagnosticMicOut()
            ?: throw IllegalStateException(context.getString(R.string.diag_record_error_no_data))

        val base = SimpleDateFormat(TIMESTAMP_PATTERN, Locale.US).format(Date())
        val relDir = "${Environment.DIRECTORY_DOWNLOADS}/$SUBDIR"

        saveWav("$base-capture-in.wav", captureIn, sampleRate, 2, relDir)
        saveWav("$base-capture-out.wav", captureOut, sampleRate, 2, relDir)
        saveWav("$base-mic-in.wav", micIn, sampleRate, inputChannels, relDir)
        saveWav("$base-mic-out.wav", micOut, sampleRate, 2, relDir)
        saveText("$base.txt", meta, sampleRate, inputChannels, totalFrames, relDir)

        return "$relDir/$base-*.wav"
    }

    private fun saveWav(
        fileName: String,
        samples: FloatArray,
        sampleRate: Int,
        channels: Int,
        relDir: String,
    ) {
        val resolver = context.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, fileName)
            put(MediaStore.Downloads.MIME_TYPE, "audio/wav")
            put(MediaStore.Downloads.RELATIVE_PATH, relDir)
            put(MediaStore.Downloads.IS_PENDING, 1)
        }
        val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
            ?: throw IllegalStateException(
                context.getString(R.string.diag_record_error_mediastore, fileName),
            )
        resolver.openOutputStream(uri)?.use { out ->
            writeWav(out, samples, sampleRate, channels)
        } ?: throw IllegalStateException(
            context.getString(R.string.diag_record_error_mediastore, fileName),
        )
        values.clear()
        values.put(MediaStore.Downloads.IS_PENDING, 0)
        resolver.update(uri, values, null, null)
    }

    /** 16-bit PCM WAV。[samples] は既にインタリーブ済み(呼び出し側の責務)。 */
    private fun writeWav(out: OutputStream, samples: FloatArray, sampleRate: Int, channels: Int) {
        val bytesPerSample = 2
        val dataSize = samples.size * bytesPerSample
        val byteRate = sampleRate * channels * bytesPerSample
        val blockAlign = channels * bytesPerSample

        val header = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN)
        header.put("RIFF".toByteArray(Charsets.US_ASCII))
        header.putInt(36 + dataSize)
        header.put("WAVE".toByteArray(Charsets.US_ASCII))
        header.put("fmt ".toByteArray(Charsets.US_ASCII))
        header.putInt(16)
        header.putShort(1) // PCM
        header.putShort(channels.toShort())
        header.putInt(sampleRate)
        header.putInt(byteRate)
        header.putShort(blockAlign.toShort())
        header.putShort(16) // bits per sample
        header.put("data".toByteArray(Charsets.US_ASCII))
        header.putInt(dataSize)
        out.write(header.array())

        // まとめて 1 回で書く(チャンク分割は不要な規模: 10 秒 x 2ch x 48kHz で ~1.9MB)。
        val body = ByteBuffer.allocate(dataSize).order(ByteOrder.LITTLE_ENDIAN)
        for (s in samples) {
            val clamped = if (s.isFinite()) s.coerceIn(-1.0f, 1.0f) else 0.0f
            val v = (clamped * 32767.0f).roundToInt().coerceIn(-32768, 32767)
            body.putShort(v.toShort())
        }
        out.write(body.array())
    }

    private fun saveText(
        fileName: String,
        meta: Meta,
        sampleRate: Int,
        inputChannels: Int,
        totalFrames: Int,
        relDir: String,
    ) {
        val seconds = totalFrames.toFloat() / sampleRate
        val content = buildString {
            appendLine("prism 診断録音")
            appendLine("sampleRate=$sampleRate")
            appendLine("frames=$totalFrames (%.1fs)".format(Locale.US, seconds))
            appendLine("micInputChannels=$inputChannels")
            appendLine("framesPerBurst=${meta.framesPerBurst}")
            appendLine("outputDeviceId=${meta.outputDeviceId}")
            appendLine("inputDeviceId=${meta.inputDeviceId}")
            appendLine("inputPreset=${meta.inputPreset}")
            appendLine("micSweepMs=${meta.micSweepMs}")
            appendLine("captureSweepMs=${meta.captureSweepMs}")
            appendLine("crossfadeMs=${meta.crossfadeMs}")
            appendLine("shiftCentsL=${meta.shiftCentsL}")
            appendLine("shiftCentsR=${meta.shiftCentsR}")
            appendLine("outputGainDb=${meta.outputGainDb}")
            appendLine("micGainDb=${meta.micGainDb}")
            appendLine("captureGainDb=${meta.captureGainDb}")
            appendLine("captureEnabled=${meta.captureEnabled}")
            appendLine("outputXRunCount=${meta.outputXRunCount}")
            appendLine("inputXRunCount=${meta.inputXRunCount}")
            appendLine("micShortfallCount=${meta.micShortfallCount}")
            appendLine("micShortfallFrames=${meta.micShortfallFrames}")
            appendLine("captureUnderrunCount=${meta.captureUnderrunCount}")
            appendLine("captureOverrunCount=${meta.captureOverrunCount}")
            appendLine("captureShortfallFrames=${meta.captureShortfallFrames}")
        }
        val resolver = context.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, fileName)
            put(MediaStore.Downloads.MIME_TYPE, "text/plain")
            put(MediaStore.Downloads.RELATIVE_PATH, relDir)
        }
        val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
            ?: throw IllegalStateException(
                context.getString(R.string.diag_record_error_mediastore, fileName),
            )
        resolver.openOutputStream(uri)?.use { it.write(content.toByteArray(Charsets.UTF_8)) }
            ?: throw IllegalStateException(
                context.getString(R.string.diag_record_error_mediastore, fileName),
            )
    }

    /**
     * 途中で打ち切る(Activity/Service が畳まれる場合など)。ポーリングを止め、
     * ネイティブ側には `cancelDiagnosticRecording()`(atomic フラグのみ、いつ呼んでも
     * 安全)だけを伝える。保存ワーカーが走っていれば、次の操作(エンジンの停止など)
     * より前に確実に終わらせておくため、ここで join する。
     */
    fun cancel() {
        isActive = false
        pollRunnable?.let { handler.removeCallbacks(it) }
        pollRunnable = null
        engine.cancelDiagnosticRecording()
        worker?.let {
            try {
                it.join(JOIN_TIMEOUT_MS)
            } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }
        worker = null
    }

    /** 録音時のメタ情報。テキストサイドカーにそのまま書き出す。 */
    data class Meta(
        val framesPerBurst: Int,
        val outputDeviceId: Int,
        val inputDeviceId: Int,
        val inputPreset: Int,
        val micSweepMs: Double,
        val captureSweepMs: Double,
        val crossfadeMs: Int,
        val shiftCentsL: Int,
        val shiftCentsR: Int,
        val outputGainDb: Float,
        val micGainDb: Float,
        val captureGainDb: Float,
        val captureEnabled: Boolean,
        val outputXRunCount: Int,
        val inputXRunCount: Int,
        val micShortfallCount: Int,
        val micShortfallFrames: Int,
        val captureUnderrunCount: Int,
        val captureOverrunCount: Int,
        val captureShortfallFrames: Int,
    )

    companion object {
        private const val TAG = "prism-diag"
        private const val THREAD_NAME = "prism-diag-save"
        private const val SUBDIR = "katsuo"
        private const val TIMESTAMP_PATTERN = "yyyyMMdd-HHmmss"
        private const val POLL_INTERVAL_MS = 300L
        private const val JOIN_TIMEOUT_MS = 2000L
    }
}
