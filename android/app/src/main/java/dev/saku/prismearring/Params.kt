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
    /** スライダの下限(セント)。UI の粒度切替であって DSP の範囲ではない。 */
    val sliderFloor: Int = SLIDER_FLOOR_FINE,
) {
    companion object {
        // --- DSP が実際に受け付ける範囲(PitchShifter の clamp と同じ) ---
        const val DSP_SHIFT_CENTS_MIN = -150
        const val DSP_SHIFT_CENTS_MAX = 0
        const val DEFAULT_SHIFT_CENTS = -89

        const val DRY_WET_MIN = 0.0f
        const val DRY_WET_MAX = 1.0f
        const val DEFAULT_DRY_WET = 1.0f

        const val CROSSFADE_MS_MIN = 10
        const val CROSSFADE_MS_MAX = 100
        const val DEFAULT_CROSSFADE_MS = 50

        // --- スライダの下限プリセット(範囲切替の 4 分割) ---
        const val SLIDER_FLOOR_FINE = -150      // 微調整
        const val SLIDER_FLOOR_SEMITONE = -100  // 半音
        const val SLIDER_FLOOR_TONE = -200      // 全音
        const val SLIDER_FLOOR_OCTAVE = -1200   // オクターブ

        val SLIDER_FLOORS = intArrayOf(
            SLIDER_FLOOR_FINE,
            SLIDER_FLOOR_SEMITONE,
            SLIDER_FLOOR_TONE,
            SLIDER_FLOOR_OCTAVE,
        )

        // --- 窓長プリセット(3 分割) ---
        val CROSSFADE_PRESETS = intArrayOf(20, 50, 100)

        private const val PREFS_NAME = "prism_params"
        private const val KEY_SHIFT_L = "shift_cents_l"
        private const val KEY_SHIFT_R = "shift_cents_r"
        private const val KEY_SPLIT = "split_channels"
        private const val KEY_DRY_WET = "dry_wet"
        private const val KEY_CROSSFADE = "crossfade_ms"
        private const val KEY_SLIDER_FLOOR = "slider_floor"

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
                sliderFloor = p.getInt(KEY_SLIDER_FLOOR, SLIDER_FLOOR_FINE),
            ).sanitized()
        }

        /**
         * DSP に渡す前の丸め。SharedPreferences は前バージョンの値や
         * 手で書き換えた値を返しうるので、境界で必ず通す。
         */
        private fun Params.sanitized(): Params {
            val floor = if (SLIDER_FLOORS.contains(sliderFloor)) sliderFloor else SLIDER_FLOOR_FINE
            return copy(
                shiftCentsL = shiftCentsL.coerceIn(floor, DSP_SHIFT_CENTS_MAX),
                shiftCentsR = shiftCentsR.coerceIn(floor, DSP_SHIFT_CENTS_MAX),
                dryWet = if (dryWet.isFinite()) dryWet.coerceIn(DRY_WET_MIN, DRY_WET_MAX)
                else DEFAULT_DRY_WET,
                crossfadeMs = crossfadeMs.coerceIn(CROSSFADE_MS_MIN, CROSSFADE_MS_MAX),
                sliderFloor = floor,
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
            .putInt(KEY_SLIDER_FLOOR, sliderFloor)
            .apply()
    }

    /** L/R 独立が OFF のときは L の値を両チャンネルに使う。 */
    val effectiveRight: Int get() = if (splitChannels) shiftCentsR else shiftCentsL

    /**
     * DSP に渡る実効値。PitchShifter は [DSP_SHIFT_CENTS_MIN] で clamp するため、
     * スライダの下限がそれより低い(全音 / オクターブ)場合はここで頭打ちになる。
     * UI はこの差を隠さず注記として出す。
     */
    val clampedLeft: Int get() = shiftCentsL.coerceIn(DSP_SHIFT_CENTS_MIN, DSP_SHIFT_CENTS_MAX)
    val clampedRight: Int get() = effectiveRight.coerceIn(DSP_SHIFT_CENTS_MIN, DSP_SHIFT_CENTS_MAX)

    /** スライダ値が DSP の下限を下回っている(= 頭打ちになっている)か。 */
    val isClamped: Boolean
        get() = shiftCentsL < DSP_SHIFT_CENTS_MIN || effectiveRight < DSP_SHIFT_CENTS_MIN

    /** エンジンへ一括反映する。動作中に呼んでよい(内部は atomic)。 */
    fun applyTo(engine: NativeEngine) {
        engine.setShiftCents(NativeEngine.CHANNEL_LEFT, clampedLeft.toFloat())
        engine.setShiftCents(NativeEngine.CHANNEL_RIGHT, clampedRight.toFloat())
        engine.setDryWet(dryWet)
        engine.setCrossfadeMs(crossfadeMs.toFloat())
    }
}
