package dev.saku.prismearring

import android.content.Context
import android.content.SharedPreferences
import kotlin.math.log10
import kotlin.math.pow

/**
 * 実行時パラメータ。範囲は prism::PitchShifter の公開定数と一致させること
 * (dsp/include/prism/PitchShifter.h の kShiftCentsMin など)。
 * ここを変えるなら DSP 側も変える。片方だけ動かすと UI と実際の音がずれる。
 */
data class Params(
    val shiftCentsL: Int = DEFAULT_SHIFT_CENTS,
    val shiftCentsR: Int = DEFAULT_SHIFT_CENTS,
    val splitChannels: Boolean = false,
    val dryWet: Float = DEFAULT_DRY_WET,
    val crossfadeMs: Int = DEFAULT_CROSSFADE_MS,
    /** −/+ ボタン 1 回ぶんの刻み(セント)。全音 200 / 半音 100 / 1/4 音 50 / 1/8 音 25。 */
    val stepCents: Int = DEFAULT_STEP_CENTS,
    /** 出力ゲイン(dB)。−6dB〜+12dB。エンジンへは倍率に変換して渡す([dbToGain])。 */
    val outputGainDb: Float = DEFAULT_OUTPUT_GAIN_DB,
    /** テーマ設定。[THEME_SYSTEM] / [THEME_LIGHT] / [THEME_DARK]。エンジンには渡さない。 */
    val themeMode: Int = THEME_SYSTEM,
    // --- ユーザープリセット(P1〜P3)。値は [PRESET_UNSET] なら未登録。エンジンには渡さない。 ---
    val preset1L: Int = PRESET_UNSET,
    val preset1R: Int = PRESET_UNSET,
    val preset2L: Int = PRESET_UNSET,
    val preset2R: Int = PRESET_UNSET,
    val preset3L: Int = PRESET_UNSET,
    val preset3R: Int = PRESET_UNSET,
    // --- 音源(v0.4.0)。マイクと捕獲(他アプリの再生音)の 2 系統。 ---
    /** マイク音量(dB)。−30〜+12、既定 0。UI 最小位置は「OFF」(倍率 0.0)。 */
    val micGainDb: Float = DEFAULT_MIC_GAIN_DB,
    /** 他アプリの音を拾う(AudioPlaybackCapture)か。既定 false。 */
    val captureEnabled: Boolean = false,
    /** 捕獲音の音量(dB)。−12〜+12、既定 0。 */
    val captureGainDb: Float = DEFAULT_CAPTURE_GAIN_DB,
    /** 出力先デバイス ID。[NativeEngine.DEVICE_AUTO] = 自動。次の start() から有効。 */
    val outputDeviceId: Int = NativeEngine.DEVICE_AUTO,
    /** 入力元デバイス ID。[NativeEngine.DEVICE_AUTO] = 自動。次の start() から有効。 */
    val inputDeviceId: Int = NativeEngine.DEVICE_AUTO,
    /** 出力用途。[NativeEngine.USAGE_MEDIA] / [NativeEngine.USAGE_ACCESSIBILITY]。次の start() から有効。 */
    val outputUsage: Int = NativeEngine.USAGE_MEDIA,
    /** マイク経路の走査幅(ms)。[SWEEP_PRESETS] のいずれか。次の start() から有効。 */
    val micSweepMs: Float = DEFAULT_MIC_SWEEP_MS,
) {
    companion object {
        // --- DSP が実際に受け付ける範囲(PitchShifter の clamp と同じ) ---
        const val DSP_SHIFT_CENTS_MIN = -1200
        const val DSP_SHIFT_CENTS_MAX = 1200
        const val DEFAULT_SHIFT_CENTS = -89

        // スライダは常にこの全域を表示する(範囲切替は廃止し、刻み幅の切替に置き換えた)。
        const val SLIDER_MIN = DSP_SHIFT_CENTS_MIN
        const val SLIDER_MAX = DSP_SHIFT_CENTS_MAX

        const val DRY_WET_MIN = 0.0f
        const val DRY_WET_MAX = 1.0f
        const val DEFAULT_DRY_WET = 1.0f

        const val CROSSFADE_MS_MIN = 10
        // v0.4.0: 200ms まで拡張(既定 −89 セントでは跳躍間隔が約 190ms のため、
        // その付近まで伸ばすと常時クロスフェード状態になる。詳細は README)。
        const val CROSSFADE_MS_MAX = 200
        const val DEFAULT_CROSSFADE_MS = 50

        // --- 刻み幅プリセット(4 分割。−/+ ボタン 1 回ぶんのセント数) ---
        const val STEP_WHOLE_TONE = 200  // 全音
        const val STEP_SEMITONE = 100    // 半音
        const val STEP_QUARTER_TONE = 50 // 1/4 音
        const val STEP_EIGHTH_TONE = 25  // 1/8 音(既定)
        const val DEFAULT_STEP_CENTS = STEP_EIGHTH_TONE

        val STEP_PRESETS = intArrayOf(
            STEP_WHOLE_TONE,
            STEP_SEMITONE,
            STEP_QUARTER_TONE,
            STEP_EIGHTH_TONE,
        )

        // --- 窓長(なめらかさ)プリセット(v0.4.0 で 4 分割に拡張) ---
        val CROSSFADE_PRESETS = intArrayOf(20, 50, 100, 200)

        // --- 出力ゲイン(dB)。倍率換算で 0.5〜4.0 = −6dB〜+12dB(AudioBridge と一致)。 ---
        const val OUTPUT_GAIN_DB_MIN = -6.0f
        const val OUTPUT_GAIN_DB_MAX = 12.0f
        const val DEFAULT_OUTPUT_GAIN_DB = 0.0f

        /** dB -> 倍率。AudioBridge::kOutputGainMin/Max で最終的に clamp される。 */
        fun dbToGain(db: Float): Float = 10.0f.pow(db / 20.0f)

        /** 倍率 -> dB。診断表示や逆算に使う。 */
        fun gainToDb(gain: Float): Float = 20.0f * log10(gain.coerceAtLeast(1e-6f))

        // --- マイク音量(dB)。最小位置は「OFF」扱いで倍率 0.0 を渡す([micGainDbToGain])。 ---
        const val MIC_GAIN_DB_MIN = -30.0f
        const val MIC_GAIN_DB_MAX = 12.0f
        const val DEFAULT_MIC_GAIN_DB = 0.0f

        /** dB -> 倍率(マイク)。最小値ちょうどは完全ミュート(0.0)。 */
        fun micGainDbToGain(db: Float): Float =
            if (!db.isFinite() || db <= MIC_GAIN_DB_MIN) 0.0f else dbToGain(db)

        // --- 捕獲音量(dB)。 ---
        const val CAPTURE_GAIN_DB_MIN = -12.0f
        const val CAPTURE_GAIN_DB_MAX = 12.0f
        const val DEFAULT_CAPTURE_GAIN_DB = 0.0f

        // --- マイク経路の走査幅(ms)。聴感ヒアリング用のプリセット。 ---
        // FloatArray は contains/indexOf を持たない(浮動小数の等値比較を意図的に外して
        // いるため)。List<Float> にしておくと Collection の contains/indexOf がそのまま使える。
        val SWEEP_PRESETS: List<Float> = listOf(5f, 9.5f, 15f, 20f, 30f, 40f, 60f)
        val DEFAULT_MIC_SWEEP_MS = NativeEngine.MIC_SWEEP_MS_DEFAULT.toFloat()

        // --- テーマ ---
        const val THEME_SYSTEM = 0
        const val THEME_LIGHT = 1
        const val THEME_DARK = 2

        // --- ユーザープリセット ---
        /** プリセットスロット未登録を表す番兵値。DSP の有効範囲(±1200)の外側であること。 */
        const val PRESET_UNSET = Int.MIN_VALUE

        private const val PREFS_NAME = "prism_params"
        private const val KEY_SHIFT_L = "shift_cents_l"
        private const val KEY_SHIFT_R = "shift_cents_r"
        private const val KEY_SPLIT = "split_channels"
        private const val KEY_DRY_WET = "dry_wet"
        private const val KEY_CROSSFADE = "crossfade_ms"
        private const val KEY_STEP_CENTS = "step_cents"
        private const val KEY_OUTPUT_GAIN_DB = "output_gain_db"
        private const val KEY_THEME_MODE = "theme_mode"
        private const val KEY_PRESET1_L = "preset1_l"
        private const val KEY_PRESET1_R = "preset1_r"
        private const val KEY_PRESET2_L = "preset2_l"
        private const val KEY_PRESET2_R = "preset2_r"
        private const val KEY_PRESET3_L = "preset3_l"
        private const val KEY_PRESET3_R = "preset3_r"
        private const val KEY_MIC_GAIN_DB = "mic_gain_db"
        private const val KEY_CAPTURE_ENABLED = "capture_enabled"
        private const val KEY_CAPTURE_GAIN_DB = "capture_gain_db"
        private const val KEY_OUTPUT_DEVICE_ID = "output_device_id"
        private const val KEY_INPUT_DEVICE_ID = "input_device_id"
        private const val KEY_OUTPUT_USAGE = "output_usage"
        private const val KEY_MIC_SWEEP_MS = "mic_sweep_ms"

        fun prefs(context: Context): SharedPreferences =
            context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

        fun load(context: Context): Params {
            val p = prefs(context)
            return Params(
                shiftCentsL = p.getInt(KEY_SHIFT_L, DEFAULT_SHIFT_CENTS),
                shiftCentsR = p.getInt(KEY_SHIFT_R, DEFAULT_SHIFT_CENTS),
                splitChannels = p.getBoolean(KEY_SPLIT, false),
                dryWet = p.getFloat(KEY_DRY_WET, DEFAULT_DRY_WET),
                crossfadeMs = p.getInt(KEY_CROSSFADE, DEFAULT_CROSSFADE_MS),
                stepCents = p.getInt(KEY_STEP_CENTS, DEFAULT_STEP_CENTS),
                outputGainDb = p.getFloat(KEY_OUTPUT_GAIN_DB, DEFAULT_OUTPUT_GAIN_DB),
                themeMode = p.getInt(KEY_THEME_MODE, THEME_SYSTEM),
                preset1L = p.getInt(KEY_PRESET1_L, PRESET_UNSET),
                preset1R = p.getInt(KEY_PRESET1_R, PRESET_UNSET),
                preset2L = p.getInt(KEY_PRESET2_L, PRESET_UNSET),
                preset2R = p.getInt(KEY_PRESET2_R, PRESET_UNSET),
                preset3L = p.getInt(KEY_PRESET3_L, PRESET_UNSET),
                preset3R = p.getInt(KEY_PRESET3_R, PRESET_UNSET),
                micGainDb = p.getFloat(KEY_MIC_GAIN_DB, DEFAULT_MIC_GAIN_DB),
                captureEnabled = p.getBoolean(KEY_CAPTURE_ENABLED, false),
                captureGainDb = p.getFloat(KEY_CAPTURE_GAIN_DB, DEFAULT_CAPTURE_GAIN_DB),
                outputDeviceId = p.getInt(KEY_OUTPUT_DEVICE_ID, NativeEngine.DEVICE_AUTO),
                inputDeviceId = p.getInt(KEY_INPUT_DEVICE_ID, NativeEngine.DEVICE_AUTO),
                outputUsage = p.getInt(KEY_OUTPUT_USAGE, NativeEngine.USAGE_MEDIA),
                micSweepMs = p.getFloat(KEY_MIC_SWEEP_MS, DEFAULT_MIC_SWEEP_MS),
            ).sanitized()
        }

        /**
         * プリセットの L/R ペアの丸め。片方だけが有効範囲外(番兵値含む)だと
         * L だけ登録済み・R は範囲外の値、という壊れた組み合わせになりうるため、
         * どちらか一方でも無効なら両方まとめて「未登録」に戻す。
         */
        private fun sanitizePresetPair(l: Int, r: Int): Pair<Int, Int> {
            val validL = l in DSP_SHIFT_CENTS_MIN..DSP_SHIFT_CENTS_MAX
            val validR = r in DSP_SHIFT_CENTS_MIN..DSP_SHIFT_CENTS_MAX
            return if (validL && validR) l to r else PRESET_UNSET to PRESET_UNSET
        }

        /**
         * DSP に渡す前の丸め。SharedPreferences は前バージョンの値や
         * 手で書き換えた値を返しうるので、境界で必ず通す。
         */
        private fun Params.sanitized(): Params {
            val step = if (STEP_PRESETS.contains(stepCents)) stepCents else DEFAULT_STEP_CENTS
            val theme = if (themeMode in THEME_SYSTEM..THEME_DARK) themeMode else THEME_SYSTEM
            val sweep = if (SWEEP_PRESETS.contains(micSweepMs)) micSweepMs else DEFAULT_MIC_SWEEP_MS
            val usage = if (outputUsage == NativeEngine.USAGE_MEDIA ||
                outputUsage == NativeEngine.USAGE_ACCESSIBILITY
            ) outputUsage else NativeEngine.USAGE_MEDIA
            return copy(
                shiftCentsL = shiftCentsL.coerceIn(DSP_SHIFT_CENTS_MIN, DSP_SHIFT_CENTS_MAX),
                shiftCentsR = shiftCentsR.coerceIn(DSP_SHIFT_CENTS_MIN, DSP_SHIFT_CENTS_MAX),
                dryWet = if (dryWet.isFinite()) dryWet.coerceIn(DRY_WET_MIN, DRY_WET_MAX)
                else DEFAULT_DRY_WET,
                crossfadeMs = crossfadeMs.coerceIn(CROSSFADE_MS_MIN, CROSSFADE_MS_MAX),
                stepCents = step,
                outputGainDb = if (outputGainDb.isFinite())
                    outputGainDb.coerceIn(OUTPUT_GAIN_DB_MIN, OUTPUT_GAIN_DB_MAX)
                else DEFAULT_OUTPUT_GAIN_DB,
                themeMode = theme,
                micGainDb = if (micGainDb.isFinite())
                    micGainDb.coerceIn(MIC_GAIN_DB_MIN, MIC_GAIN_DB_MAX)
                else DEFAULT_MIC_GAIN_DB,
                captureGainDb = if (captureGainDb.isFinite())
                    captureGainDb.coerceIn(CAPTURE_GAIN_DB_MIN, CAPTURE_GAIN_DB_MAX)
                else DEFAULT_CAPTURE_GAIN_DB,
                outputDeviceId = if (outputDeviceId >= 0) outputDeviceId else NativeEngine.DEVICE_AUTO,
                inputDeviceId = if (inputDeviceId >= 0) inputDeviceId else NativeEngine.DEVICE_AUTO,
                outputUsage = usage,
                micSweepMs = sweep,
            ).let {
                val (p1l, p1r) = sanitizePresetPair(preset1L, preset1R)
                val (p2l, p2r) = sanitizePresetPair(preset2L, preset2R)
                val (p3l, p3r) = sanitizePresetPair(preset3L, preset3R)
                it.copy(
                    preset1L = p1l, preset1R = p1r,
                    preset2L = p2l, preset2R = p2r,
                    preset3L = p3l, preset3R = p3r,
                )
            }
        }
    }

    fun save(context: Context) {
        prefs(context).edit()
            .putInt(KEY_SHIFT_L, shiftCentsL)
            .putInt(KEY_SHIFT_R, shiftCentsR)
            .putBoolean(KEY_SPLIT, splitChannels)
            .putFloat(KEY_DRY_WET, dryWet)
            .putInt(KEY_CROSSFADE, crossfadeMs)
            .putInt(KEY_STEP_CENTS, stepCents)
            .putFloat(KEY_OUTPUT_GAIN_DB, outputGainDb)
            .putInt(KEY_THEME_MODE, themeMode)
            .putInt(KEY_PRESET1_L, preset1L)
            .putInt(KEY_PRESET1_R, preset1R)
            .putInt(KEY_PRESET2_L, preset2L)
            .putInt(KEY_PRESET2_R, preset2R)
            .putInt(KEY_PRESET3_L, preset3L)
            .putInt(KEY_PRESET3_R, preset3R)
            .putFloat(KEY_MIC_GAIN_DB, micGainDb)
            .putBoolean(KEY_CAPTURE_ENABLED, captureEnabled)
            .putFloat(KEY_CAPTURE_GAIN_DB, captureGainDb)
            .putInt(KEY_OUTPUT_DEVICE_ID, outputDeviceId)
            .putInt(KEY_INPUT_DEVICE_ID, inputDeviceId)
            .putInt(KEY_OUTPUT_USAGE, outputUsage)
            .putFloat(KEY_MIC_SWEEP_MS, micSweepMs)
            .apply()
    }

    /** L/R 独立が OFF のときは L の値を両チャンネルに使う。 */
    val effectiveRight: Int get() = if (splitChannels) shiftCentsR else shiftCentsL

    /** スライダの実効下限 / 上限(セント)。常に DSP の全域([SLIDER_MIN], [SLIDER_MAX])。 */
    val sliderMin: Int get() = SLIDER_MIN
    val sliderMax: Int get() = SLIDER_MAX

    /** −/+ ボタン 1 回ぶんの刻み(セント)。[stepCents] をそのまま使う。 */
    val shiftStepCents: Int get() = stepCents

    /** スロット(1〜3)の L 値。未登録なら [PRESET_UNSET]。 */
    fun presetL(slot: Int): Int = when (slot) {
        1 -> preset1L
        2 -> preset2L
        else -> preset3L
    }

    /** スロット(1〜3)の R 値。未登録なら [PRESET_UNSET]。 */
    fun presetR(slot: Int): Int = when (slot) {
        1 -> preset1R
        2 -> preset2R
        else -> preset3R
    }

    /** スロットが登録済みかどうか。 */
    fun isPresetSet(slot: Int): Boolean = presetL(slot) != PRESET_UNSET

    /** 指定スロットに現在の L/R ペアを登録した新しい [Params] を返す。 */
    fun withPresetSet(slot: Int, shiftL: Int, shiftR: Int): Params = when (slot) {
        1 -> copy(preset1L = shiftL, preset1R = shiftR)
        2 -> copy(preset2L = shiftL, preset2R = shiftR)
        else -> copy(preset3L = shiftL, preset3R = shiftR)
    }

    /**
     * エンジンへ一括反映する。動作中に呼んでよい(内部は atomic)。
     * ただし [outputDeviceId] / [inputDeviceId] / [outputUsage] / [micSweepMs] は
     * 呼んでも即時には効かず、**次の `start()` から**反映される([requiresRestart] 参照)。
     */
    fun applyTo(engine: NativeEngine) {
        engine.setShiftCents(NativeEngine.CHANNEL_LEFT, shiftCentsL.toFloat())
        engine.setShiftCents(NativeEngine.CHANNEL_RIGHT, effectiveRight.toFloat())
        engine.setDryWet(dryWet)
        engine.setCrossfadeMs(crossfadeMs.toFloat())
        engine.setOutputGain(dbToGain(outputGainDb))
        engine.setMicGain(micGainDbToGain(micGainDb))
        engine.setCaptureEnabled(captureEnabled)
        engine.setCaptureGain(dbToGain(captureGainDb))
        engine.setOutputDeviceId(outputDeviceId)
        engine.setInputDeviceId(inputDeviceId)
        engine.setOutputUsage(outputUsage)
        engine.setMicSweepMs(micSweepMs.toDouble())
    }

    /**
     * この設定と比べて、動作中に変えた場合に Service の stop → start が要る項目
     * (デバイス指定 / 出力用途 / マイク走査幅)のいずれかが違うか。
     */
    fun requiresRestart(other: Params): Boolean =
        outputDeviceId != other.outputDeviceId ||
            inputDeviceId != other.inputDeviceId ||
            outputUsage != other.outputUsage ||
            micSweepMs != other.micSweepMs
}
