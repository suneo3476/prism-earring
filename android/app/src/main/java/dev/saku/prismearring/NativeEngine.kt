package dev.saku.prismearring

/**
 * libprism.so への唯一の入口。
 *
 * ネイティブ側 (PrismEngine) がストリームの生存管理と DSP を持ち、ここは
 * ハンドルを持ち回すだけの薄い層に留める。パラメータのセッターは
 * PitchShifter 内部の std::atomic に直接書くので、動作中に呼んでよい。
 */
class NativeEngine private constructor(private var handle: Long) {

    /** 入力 / 出力 / DSP それぞれの遅延(ミリ秒)。[valid] が false なら測定不能。 */
    data class Latency(
        val inputMs: Double,
        val outputMs: Double,
        val dspMs: Double,
        val totalMs: Double,
        val valid: Boolean,
    )

    /** 開いているストリームの実際の素性。UI の診断表示に使う。 */
    data class StreamInfo(
        val sampleRate: Int,
        val inputChannels: Int,
        val outputChannels: Int,
        val framesPerBurst: Int,
        val underruns: Int,
        val inputErrors: Int,
        val exclusive: Boolean,
        val unprocessedInput: Boolean,
    )

    val isValid: Boolean get() = handle != 0L

    /** 成功したら true。失敗理由は [lastError] に入る。 */
    fun start(): Boolean = if (handle == 0L) false else nativeStart(handle)

    fun stop() {
        if (handle != 0L) nativeStop(handle)
    }

    fun isRunning(): Boolean = handle != 0L && nativeIsRunning(handle)

    /** 起動同期(古い入力の破棄とクッション確保)が終わり、実際に音を通しているか。 */
    fun isSynced(): Boolean = handle != 0L && nativeIsSynced(handle)

    /** [channel] は [CHANNEL_LEFT] / [CHANNEL_RIGHT] / [CHANNEL_BOTH]。 */
    fun setShiftCents(channel: Int, cents: Float) {
        if (handle != 0L) nativeSetShiftCents(handle, channel, cents)
    }

    fun setDryWet(mix: Float) {
        if (handle != 0L) nativeSetDryWet(handle, mix)
    }

    fun setCrossfadeMs(ms: Float) {
        if (handle != 0L) nativeSetCrossfadeMs(handle, ms)
    }

    /** [gain] は倍率(0.5〜4.0)。dB からの変換は [Params] 側の責務。 */
    fun setOutputGain(gain: Float) {
        if (handle != 0L) nativeSetOutputGain(handle, gain)
    }

    fun latency(): Latency {
        if (handle == 0L) return Latency(0.0, 0.0, 0.0, 0.0, false)
        val v = nativeGetLatency(handle) ?: return Latency(0.0, 0.0, 0.0, 0.0, false)
        if (v.size < 5) return Latency(0.0, 0.0, 0.0, 0.0, false)
        return Latency(v[0], v[1], v[2], v[3], v[4] != 0.0)
    }

    fun streamInfo(): StreamInfo {
        val empty = StreamInfo(0, 0, 0, 0, 0, 0, false, false)
        if (handle == 0L) return empty
        val v = nativeGetStreamInfo(handle) ?: return empty
        if (v.size < 8) return empty
        return StreamInfo(v[0], v[1], v[2], v[3], v[4], v[5], v[6] != 0, v[7] != 0)
    }

    fun lastError(): String =
        if (handle == 0L) "エンジンが初期化されていません" else nativeGetLastError(handle) ?: ""

    /** 二度呼んでも安全。以後このインスタンスは使えない。 */
    fun release() {
        val h = handle
        handle = 0L
        if (h != 0L) nativeDestroy(h)
    }

    companion object {
        const val CHANNEL_LEFT = 0
        const val CHANNEL_RIGHT = 1
        const val CHANNEL_BOTH = 2

        /** .so のロードに失敗した場合(ABI 不一致など)は null を返す。 */
        @Volatile
        private var libraryLoaded: Boolean? = null

        private fun ensureLibrary(): Boolean {
            libraryLoaded?.let { return it }
            synchronized(this) {
                libraryLoaded?.let { return it }
                val ok = try {
                    System.loadLibrary("prism")
                    true
                } catch (e: UnsatisfiedLinkError) {
                    android.util.Log.e("prism", "libprism.so をロードできません", e)
                    false
                }
                libraryLoaded = ok
                return ok
            }
        }

        /** 生成に失敗したら null。呼び出し側は必ず null チェックすること。 */
        fun create(): NativeEngine? {
            if (!ensureLibrary()) return null
            val handle = nativeCreate()
            if (handle == 0L) {
                android.util.Log.e("prism", "ネイティブエンジンを確保できません")
                return null
            }
            return NativeEngine(handle)
        }

        @JvmStatic private external fun nativeCreate(): Long
        @JvmStatic private external fun nativeDestroy(handle: Long)
        @JvmStatic private external fun nativeStart(handle: Long): Boolean
        @JvmStatic private external fun nativeStop(handle: Long)
        @JvmStatic private external fun nativeIsRunning(handle: Long): Boolean
        @JvmStatic private external fun nativeIsSynced(handle: Long): Boolean
        @JvmStatic private external fun nativeSetShiftCents(handle: Long, channel: Int, cents: Float)
        @JvmStatic private external fun nativeSetDryWet(handle: Long, mix: Float)
        @JvmStatic private external fun nativeSetCrossfadeMs(handle: Long, ms: Float)
        @JvmStatic private external fun nativeSetOutputGain(handle: Long, gain: Float)
        @JvmStatic private external fun nativeGetLatency(handle: Long): DoubleArray?
        @JvmStatic private external fun nativeGetStreamInfo(handle: Long): IntArray?
        @JvmStatic private external fun nativeGetLastError(handle: Long): String?
    }
}
