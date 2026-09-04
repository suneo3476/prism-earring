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
        const val CROSSFADE_MS_MAX = 100
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

        // --- 窓長(なめらかさ)プリセット(3 分割) ---
        val CROSSFADE_PRESETS = intArrayOf(20, 50, 100)

        // --- 出力ゲイン(dB)。倍率換算で 0.5〜4.0 = −6dB〜+12dB(AudioBridge と一致)。 ---
        const val OUTPUT_GAIN_DB_MIN = -6.0f
        const val OUTPUT_GAIN_DB_MAX = 12.0f
        const val DEFAULT_OUTPUT_GAIN_DB = 0.0f

        /** dB -> 倍率。AudioBridge::kOutputGainMin/Max で最終的に clamp される。 */
        fun dbToGain(db: Float): Float = 10.0f.pow(db / 20.0f)

        /** 倍率 -> dB。診断表示や逆算に使う。 */
        fun gainToDb(gain: Float): Float = 20.0f * log10(gain.coerceAtLeast(1e-6f))

        // --- テーマ ---
        const val THEME_SYSTEM = 0
        const val THEME_LIGHT = 1
        const val THEME_DARK = 2

        private const val PREFS_NAME = "prism_params"
        private const val KEY_SHIFT_L = "shift_cents_l"
        private const val KEY_SHIFT_R = "shift_cents_r"
        private const val KEY_SPLIT = "split_channels"
        private const val KEY_DRY_WET = "dry_wet"
        private const val KEY_CROSSFADE = "crossfade_ms"
        private const val KEY_STEP_CENTS = "step_cents"
        private const val KEY_OUTPUT_GAIN_DB = "output_gain_db"
        private const val KEY_THEME_MODE = "theme_mode"

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
            ).sanitized()
        }

        /**
         * DSP に渡す前の丸め。SharedPreferences は前バージョンの値や
         * 手で書き換えた値を返しうるので、境界で必ず通す。
         */
        private fun Params.sanitized(): Params {
            val step = if (STEP_PRESETS.contains(stepCents)) stepCents else DEFAULT_STEP_CENTS
            val theme = if (themeMode in THEME_SYSTEM..THEME_DARK) themeMode else THEME_SYSTEM
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
            )
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
            .apply()
    }

    /** L/R 独立が OFF のときは L の値を両チャンネルに使う。 */
    val effectiveRight: Int get() = if (splitChannels) shiftCentsR else shiftCentsL

    /** スライダの実効下限 / 上限(セント)。常に DSP の全域([SLIDER_MIN], [SLIDER_MAX])。 */
    val sliderMin: Int get() = SLIDER_MIN
    val sliderMax: Int get() = SLIDER_MAX

    /** −/+ ボタン 1 回ぶんの刻み(セント)。[stepCents] をそのまま使う。 */
    val shiftStepCents: Int get() = stepCents

    /** エンジンへ一括反映する。動作中に呼んでよい(内部は atomic)。 */
    fun applyTo(engine: NativeEngine) {
        engine.setShiftCents(NativeEngine.CHANNEL_LEFT, shiftCentsL.toFloat())
        engine.setShiftCents(NativeEngine.CHANNEL_RIGHT, effectiveRight.toFloat())
        engine.setDryWet(dryWet)
        engine.setCrossfadeMs(crossfadeMs.toFloat())
        engine.setOutputGain(dbToGain(outputGainDb))
    }
}
