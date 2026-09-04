package dev.saku.prismearring

import android.content.Context
import android.content.SharedPreferences

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
    /**
     * スライダの対称半幅(セント)。実効範囲は [-sliderHalfRange, +sliderHalfRange]。
     * UI の粒度切替であって DSP の範囲ではない(web/main.js の rangeCents と同じ役割)。
     */
    val sliderHalfRange: Int = SLIDER_HALF_RANGE_FINE,
) {
    companion object {
        // --- DSP が実際に受け付ける範囲(PitchShifter の clamp と同じ) ---
        const val DSP_SHIFT_CENTS_MIN = -1200
        const val DSP_SHIFT_CENTS_MAX = 1200
        const val DEFAULT_SHIFT_CENTS = -89

        const val DRY_WET_MIN = 0.0f
        const val DRY_WET_MAX = 1.0f
        const val DEFAULT_DRY_WET = 1.0f

        const val CROSSFADE_MS_MIN = 10
        const val CROSSFADE_MS_MAX = 100
        const val DEFAULT_CROSSFADE_MS = 50

        // --- スライダの半幅プリセット(範囲切替の 4 分割。対称範囲 [-N, +N]) ---
        const val SLIDER_HALF_RANGE_FINE = 150      // 微調整
        const val SLIDER_HALF_RANGE_SEMITONE = 100  // 半音
        const val SLIDER_HALF_RANGE_TONE = 200      // 全音
        const val SLIDER_HALF_RANGE_OCTAVE = 1200   // オクターブ

        val SLIDER_HALF_RANGES = intArrayOf(
            SLIDER_HALF_RANGE_FINE,
            SLIDER_HALF_RANGE_SEMITONE,
            SLIDER_HALF_RANGE_TONE,
            SLIDER_HALF_RANGE_OCTAVE,
        )

        /** オクターブ範囲のときだけ −/+ ボタンの刻みを 10 セントにする(web/main.js と同じ)。 */
        const val SHIFT_STEP_COARSE_CENTS = 10
        const val SHIFT_STEP_FINE_CENTS = 1

        // --- 窓長プリセット(3 分割) ---
        val CROSSFADE_PRESETS = intArrayOf(20, 50, 100)

        private const val PREFS_NAME = "prism_params"
        private const val KEY_SHIFT_L = "shift_cents_l"
        private const val KEY_SHIFT_R = "shift_cents_r"
        private const val KEY_SPLIT = "split_channels"
        private const val KEY_DRY_WET = "dry_wet"
        private const val KEY_CROSSFADE = "crossfade_ms"
        // 新キー。旧バージョンの "slider_floor"(下限そのものを負値で保持)とは
        // 意味が違う(半幅)ので、キー名も変えて共存させる。旧キーが SharedPreferences
        // に残っていても読まないので落ちない — 単に無視され、既定値が使われる。
        private const val KEY_SLIDER_HALF_RANGE = "slider_half_range"

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
                sliderHalfRange = p.getInt(KEY_SLIDER_HALF_RANGE, SLIDER_HALF_RANGE_FINE),
            ).sanitized()
        }

        /**
         * DSP に渡す前の丸め。SharedPreferences は前バージョンの値や
         * 手で書き換えた値を返しうるので、境界で必ず通す。
         */
        private fun Params.sanitized(): Params {
            val halfRange =
                if (SLIDER_HALF_RANGES.contains(sliderHalfRange)) sliderHalfRange
                else SLIDER_HALF_RANGE_FINE
            return copy(
                shiftCentsL = shiftCentsL.coerceIn(DSP_SHIFT_CENTS_MIN, DSP_SHIFT_CENTS_MAX),
                shiftCentsR = shiftCentsR.coerceIn(DSP_SHIFT_CENTS_MIN, DSP_SHIFT_CENTS_MAX),
                dryWet = if (dryWet.isFinite()) dryWet.coerceIn(DRY_WET_MIN, DRY_WET_MAX)
                else DEFAULT_DRY_WET,
                crossfadeMs = crossfadeMs.coerceIn(CROSSFADE_MS_MIN, CROSSFADE_MS_MAX),
                sliderHalfRange = halfRange,
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
            .putInt(KEY_SLIDER_HALF_RANGE, sliderHalfRange)
            .apply()
    }

    /** L/R 独立が OFF のときは L の値を両チャンネルに使う。 */
    val effectiveRight: Int get() = if (splitChannels) shiftCentsR else shiftCentsL

    /**
     * スライダの実効下限 / 上限(セント)。プリセット半幅を基準にした対称範囲だが、
     * 現在値がそれより外にあるときはクランプせず範囲側を現在値まで広げる
     * (web/main.js の applyShiftRange と同じ規約)。L/R 共通の 1 本の範囲を使う。
     */
    val sliderMin: Int get() = minOf(-sliderHalfRange, shiftCentsL, effectiveRight)
    val sliderMax: Int get() = maxOf(sliderHalfRange, shiftCentsL, effectiveRight)

    /** −/+ ボタン 1 回ぶんの刻み(セント)。オクターブ範囲のときだけ粗くする。 */
    val shiftStepCents: Int
        get() = if (sliderHalfRange >= SLIDER_HALF_RANGE_OCTAVE) SHIFT_STEP_COARSE_CENTS
        else SHIFT_STEP_FINE_CENTS

    /** エンジンへ一括反映する。動作中に呼んでよい(内部は atomic)。 */
    fun applyTo(engine: NativeEngine) {
        engine.setShiftCents(NativeEngine.CHANNEL_LEFT, shiftCentsL.toFloat())
        engine.setShiftCents(NativeEngine.CHANNEL_RIGHT, effectiveRight.toFloat())
        engine.setDryWet(dryWet)
        engine.setCrossfadeMs(crossfadeMs.toFloat())
    }
}
