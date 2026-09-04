package dev.saku.prismearring

/**
 * libprism.so への唯一の入口。
 *
 * ネイティブ側 (PrismEngine) がストリームの生存管理と DSP を持ち、ここは
 * ハンドルを持ち回すだけの薄い層に留める。パラメータのセッターは
 * PitchShifter 内部の std::atomic に直接書くので、動作中に呼んでよい。
 */
class NativeEngine private constructor(private var handle: Long) {

    /**
     * 入力 / 出力 / DSP それぞれの遅延(ミリ秒)。[valid] が false なら測定不能。
     *
     * [dspMs] は **マイク経路** の値。捕獲経路(AudioPlaybackCapture)は生音の
     * 漏れ込みが無いため遅延制約の対象外で、[captureSweepMs] のとおり広い走査幅で動く。
     */
    data class Latency(
        val inputMs: Double,
        val outputMs: Double,
        val dspMs: Double,
        val totalMs: Double,
        val valid: Boolean,
        /** マイク経路の遅延スイープ幅(ms)。実際に採用された値。 */
        val micSweepMs: Double = 0.0,
        /** 捕獲経路の遅延スイープ幅(ms)。実際に採用された値。 */
        val captureSweepMs: Double = 0.0,
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
        /** 捕獲リングが空でコールバックを埋めきれなかった累計回数。 */
        val captureUnderruns: Int = 0,
        /** 捕獲リングが満杯で [pushCapture] が取りこぼした累計回数。 */
        val captureOverruns: Int = 0,
        /** 捕獲リングの現在の滞留フレーム数(遅延の目安 = これ / [sampleRate])。 */
        val captureFillFrames: Int = 0,
        /** 実際に開いた出力デバイス ID。0 は未取得。 */
        val outputDeviceId: Int = 0,
        /** 実際に開いた入力デバイス ID。0 は未取得。 */
        val inputDeviceId: Int = 0,
        /** 指定した出力デバイスで開けず、自動に落ちたなら true。 */
        val outputDeviceFallback: Boolean = false,
        /** 指定した入力デバイスで開けず、自動に落ちたなら true。 */
        val inputDeviceFallback: Boolean = false,
        /** 出力の用途。[USAGE_MEDIA] または [USAGE_ACCESSIBILITY]。 */
        val outputUsage: Int = USAGE_MEDIA,
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

    // ---- 捕獲音(AudioPlaybackCapture)------------------------------------
    // 捕獲経路はマイク経路と独立したピッチシフタを持ち、走査幅だけが違う
    // (既定 40ms。生音の漏れ込みが無いので遅延制約の対象外)。
    // シフト量 / dry-wet / クロスフェードは上のセッターが両経路へ流す。

    /**
     * 捕獲音を投入する。録音スレッドから直接呼ぶこと(1 秒に 50〜100 回想定)。
     *
     * [data] はインタリーブ float、[channels] ch、[frames] フレーム。
     * mono なら L/R に複製され、3ch 以上なら先頭 2ch だけ使う。
     * サンプルレートは **エンジンの出力と同じ** であること([streamInfo] の
     * `sampleRate`)。違うと再生速度がずれる。
     *
     * @return 実際に書けたフレーム数。リングが満杯だと [frames] より少なくなる。
     */
    fun pushCapture(data: FloatArray, frames: Int, channels: Int): Int =
        if (handle == 0L) 0 else nativePushCapture(handle, data, frames, channels)

    /**
     * false のあいだ捕獲経路は無音で、[pushCapture] は何も書かずに 0 を返す。
     * 次に true にしたとき、リングは空・シフタは初期状態から始まる。
     */
    fun setCaptureEnabled(enabled: Boolean) {
        if (handle != 0L) nativeSetCaptureEnabled(handle, enabled)
    }

    /** [gain] は倍率(0.0〜2.0、既定 1.0)。0.0 でマイクを完全ミュートする。 */
    fun setMicGain(gain: Float) {
        if (handle != 0L) nativeSetMicGain(handle, gain)
    }

    /** [gain] は倍率(0.0〜4.0、既定 1.0)。 */
    fun setCaptureGain(gain: Float) {
        if (handle != 0L) nativeSetCaptureGain(handle, gain)
    }

    // ---- 次の start() から効く設定 ----------------------------------------

    /**
     * 出力先デバイス。[DEVICE_AUTO] で OS 任せ。
     * 指定した ID で開けなかった場合は自動で開き直し、
     * [StreamInfo.outputDeviceFallback] が true になる。
     */
    fun setOutputDeviceId(id: Int) {
        if (handle != 0L) nativeSetOutputDeviceId(handle, id)
    }

    /** 入力元デバイス。[DEVICE_AUTO] で OS 任せ。挙動は [setOutputDeviceId] と同じ。 */
    fun setInputDeviceId(id: Int) {
        if (handle != 0L) nativeSetInputDeviceId(handle, id)
    }

    /** [USAGE_MEDIA](既定)または [USAGE_ACCESSIBILITY]。 */
    fun setOutputUsage(usage: Int) {
        if (handle != 0L) nativeSetOutputUsage(handle, usage)
    }

    /**
     * マイク経路の遅延スイープ幅(ms、既定 9.5)。範囲はネイティブ側で 2.0〜100.0 に
     * clamp される。広げるほど低域のピッチ精度と跳躍間隔が改善する代わりに遅延が増える
     * (設計値遅延 ≈ 8 サンプル + 走査幅の半分)。採用値は [Latency.micSweepMs]。
     */
    fun setMicSweepMs(ms: Double) {
        if (handle != 0L) nativeSetMicSweepMs(handle, ms)
    }

    fun latency(): Latency {
        val empty = Latency(0.0, 0.0, 0.0, 0.0, false)
        if (handle == 0L) return empty
        val v = nativeGetLatency(handle) ?: return empty
        if (v.size < 7) return empty
        return Latency(v[0], v[1], v[2], v[3], v[4] != 0.0, v[5], v[6])
    }

    fun streamInfo(): StreamInfo {
        val empty = StreamInfo(0, 0, 0, 0, 0, 0, false, false)
        if (handle == 0L) return empty
        val v = nativeGetStreamInfo(handle) ?: return empty
        if (v.size < 16) return empty
        return StreamInfo(
            sampleRate = v[0],
            inputChannels = v[1],
            outputChannels = v[2],
            framesPerBurst = v[3],
            underruns = v[4],
            inputErrors = v[5],
            exclusive = v[6] != 0,
            unprocessedInput = v[7] != 0,
            captureUnderruns = v[8],
            captureOverruns = v[9],
            captureFillFrames = v[10],
            outputDeviceId = v[11],
            inputDeviceId = v[12],
            outputDeviceFallback = v[13] != 0,
            inputDeviceFallback = v[14] != 0,
            outputUsage = v[15],
        )
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

        /** デバイス ID の「指定なし」(OS 既定のデバイスを使う)。 */
        const val DEVICE_AUTO = 0

        /** 出力の用途: メディア(既定)。メディア音量に乗る。 */
        const val USAGE_MEDIA = 0

        /**
         * 出力の用途: アクセシビリティ。ContentType は Speech になる。
         * メディア音量を絞っても本アプリの出力だけ鳴らせるかを試すための選択肢。
         */
        const val USAGE_ACCESSIBILITY = 1

        /** マイク経路の走査幅の既定値(ms)。ネイティブ側の `kSweepMs` と同じ。 */
        const val MIC_SWEEP_MS_DEFAULT = 9.5

        /** 捕獲経路の走査幅(ms)。ネイティブ側の `kCaptureSweepMsDefault` と同じ。 */
        const val CAPTURE_SWEEP_MS_DEFAULT = 40.0

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
        @JvmStatic private external fun nativePushCapture(
            handle: Long,
            data: FloatArray,
            frames: Int,
            channels: Int,
        ): Int
        @JvmStatic private external fun nativeSetCaptureEnabled(handle: Long, enabled: Boolean)
        @JvmStatic private external fun nativeSetMicGain(handle: Long, gain: Float)
        @JvmStatic private external fun nativeSetCaptureGain(handle: Long, gain: Float)
        @JvmStatic private external fun nativeSetOutputDeviceId(handle: Long, id: Int)
        @JvmStatic private external fun nativeSetInputDeviceId(handle: Long, id: Int)
        @JvmStatic private external fun nativeSetOutputUsage(handle: Long, usage: Int)
        @JvmStatic private external fun nativeSetMicSweepMs(handle: Long, ms: Double)
        @JvmStatic private external fun nativeGetLatency(handle: Long): DoubleArray?
        @JvmStatic private external fun nativeGetStreamInfo(handle: Long): IntArray?
        @JvmStatic private external fun nativeGetLastError(handle: Long): String?
    }
}
