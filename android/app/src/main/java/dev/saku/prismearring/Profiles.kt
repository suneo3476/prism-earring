package dev.saku.prismearring

import android.content.Context
import android.content.SharedPreferences
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import java.util.UUID

/**
 * プロファイル機能(v0.7.0)。環境(USB 有線 / Bluetooth / 実験用など)ごとに
 * [ProfileSnapshot] を登録し、[MainActivity] から呼び出して一括適用する。
 *
 * リアルタイム経路には一切関わらない — ここで扱うのは UI スレッドの通常経路
 * (SharedPreferences への JSON 文字列の読み書き)のみ。
 */

/** プロファイルの対象デバイス種別。自動切替の判定と保存ダイアログの既定選択に使う。 */
object DeviceType {
    const val UNSPECIFIED = 0
    const val USB = 1
    const val WIRED = 2
    const val BLUETOOTH = 3
    const val SPEAKER = 4

    /** 自動切替の優先順(spec.md §5): USB → 有線 → Bluetooth → 本体スピーカー。 */
    val AUTO_SWITCH_PRIORITY = listOf(USB, WIRED, BLUETOOTH, SPEAKER)
}

/**
 * [Params] のスナップショット。themeMode と preset1〜3 を除く全フィールドを持つ
 * (テーマとユーザープリセットは端末全体で共通のグローバル設定なので、プロファイルには含めない)。
 */
data class ProfileSnapshot(
    val shiftCentsL: Int,
    val shiftCentsR: Int,
    val splitChannels: Boolean,
    val dryWet: Float,
    val crossfadeMs: Int,
    val stepCents: Int,
    val outputGainDb: Float,
    val micGainDb: Float,
    val captureEnabled: Boolean,
    val captureGainDb: Float,
    val outputDeviceId: Int,
    val inputDeviceId: Int,
    val outputUsage: Int,
    val micSweepMs: Float,
    val inputPreset: Int,
    val duckPercent: Int,
    val micMethod: Int,
    val captureMethod: Int,
) {
    /** このスナップショットを [p] に被せた新しい [Params] を返す(themeMode / P1〜P3 はそのまま)。 */
    fun applyTo(p: Params): Params = p.copy(
        shiftCentsL = shiftCentsL,
        shiftCentsR = shiftCentsR,
        splitChannels = splitChannels,
        dryWet = dryWet,
        crossfadeMs = crossfadeMs,
        stepCents = stepCents,
        outputGainDb = outputGainDb,
        micGainDb = micGainDb,
        captureEnabled = captureEnabled,
        captureGainDb = captureGainDb,
        outputDeviceId = outputDeviceId,
        inputDeviceId = inputDeviceId,
        outputUsage = outputUsage,
        micSweepMs = micSweepMs,
        inputPreset = inputPreset,
        duckPercent = duckPercent,
        micMethod = micMethod,
        captureMethod = captureMethod,
    )

    /** 「使用中」判定: 現在の [p] がこのスナップショットと(対象フィールドについて)一致するか。 */
    fun matches(p: Params): Boolean = this == fromParams(p)

    companion object {
        fun fromParams(p: Params): ProfileSnapshot = ProfileSnapshot(
            shiftCentsL = p.shiftCentsL,
            shiftCentsR = p.shiftCentsR,
            splitChannels = p.splitChannels,
            dryWet = p.dryWet,
            crossfadeMs = p.crossfadeMs,
            stepCents = p.stepCents,
            outputGainDb = p.outputGainDb,
            micGainDb = p.micGainDb,
            captureEnabled = p.captureEnabled,
            captureGainDb = p.captureGainDb,
            outputDeviceId = p.outputDeviceId,
            inputDeviceId = p.inputDeviceId,
            outputUsage = p.outputUsage,
            micSweepMs = p.micSweepMs,
            inputPreset = p.inputPreset,
            duckPercent = p.duckPercent,
            micMethod = p.micMethod,
            captureMethod = p.captureMethod,
        )

        fun fromJson(o: JSONObject): ProfileSnapshot = ProfileSnapshot(
            shiftCentsL = o.optInt("shiftCentsL", Params.DEFAULT_SHIFT_CENTS),
            shiftCentsR = o.optInt("shiftCentsR", Params.DEFAULT_SHIFT_CENTS),
            splitChannels = o.optBoolean("splitChannels", false),
            dryWet = o.optDouble("dryWet", Params.DEFAULT_DRY_WET.toDouble()).toFloat(),
            crossfadeMs = o.optInt("crossfadeMs", Params.DEFAULT_CROSSFADE_MS),
            stepCents = o.optInt("stepCents", Params.DEFAULT_STEP_CENTS),
            outputGainDb = o.optDouble("outputGainDb", Params.DEFAULT_OUTPUT_GAIN_DB.toDouble()).toFloat(),
            micGainDb = o.optDouble("micGainDb", Params.DEFAULT_MIC_GAIN_DB.toDouble()).toFloat(),
            captureEnabled = o.optBoolean("captureEnabled", false),
            captureGainDb = o.optDouble("captureGainDb", Params.DEFAULT_CAPTURE_GAIN_DB.toDouble()).toFloat(),
            outputDeviceId = o.optInt("outputDeviceId", NativeEngine.DEVICE_AUTO),
            inputDeviceId = o.optInt("inputDeviceId", NativeEngine.DEVICE_AUTO),
            outputUsage = o.optInt("outputUsage", NativeEngine.USAGE_MEDIA),
            micSweepMs = o.optDouble("micSweepMs", Params.DEFAULT_MIC_SWEEP_MS.toDouble()).toFloat(),
            inputPreset = o.optInt("inputPreset", NativeEngine.INPUT_PRESET_NOISE_SUPPRESSION),
            duckPercent = o.optInt("duckPercent", Params.DEFAULT_DUCK_PERCENT),
            micMethod = o.optInt("micMethod", NativeEngine.MIC_METHOD_DEFAULT),
            captureMethod = o.optInt("captureMethod", NativeEngine.CAPTURE_METHOD_DEFAULT),
        )
    }

    fun toJson(): JSONObject = JSONObject().apply {
        put("shiftCentsL", shiftCentsL)
        put("shiftCentsR", shiftCentsR)
        put("splitChannels", splitChannels)
        put("dryWet", dryWet.toDouble())
        put("crossfadeMs", crossfadeMs)
        put("stepCents", stepCents)
        put("outputGainDb", outputGainDb.toDouble())
        put("micGainDb", micGainDb.toDouble())
        put("captureEnabled", captureEnabled)
        put("captureGainDb", captureGainDb.toDouble())
        put("outputDeviceId", outputDeviceId)
        put("inputDeviceId", inputDeviceId)
        put("outputUsage", outputUsage)
        put("micSweepMs", micSweepMs.toDouble())
        put("inputPreset", inputPreset)
        put("duckPercent", duckPercent)
        put("micMethod", micMethod)
        put("captureMethod", captureMethod)
    }
}

/** 1 件のプロファイル。 */
data class Profile(
    val id: String,
    val name: String,
    val abbrev: String,
    val deviceType: Int,
    val snapshot: ProfileSnapshot,
)

/**
 * プロファイル一覧・自動切替設定・いま適用中の ID をまとめて保持する不変の状態。
 * 永続化形式は SharedPreferences に 1 本の JSON 文字列(README「プロファイルの永続化」節参照)。
 */
data class ProfileState(
    val profiles: List<Profile> = emptyList(),
    val autoSwitch: Boolean = false,
    val activeProfileId: String? = null,
)

object ProfileStore {
    private const val PREFS_NAME = "prism_profiles"
    private const val KEY_DATA = "profiles_json"

    private const val FIELD_VERSION = "version"
    private const val FIELD_PROFILES = "profiles"
    private const val FIELD_AUTO_SWITCH = "autoSwitch"
    private const val FIELD_ACTIVE_ID = "activeProfileId"
    private const val CURRENT_VERSION = 1

    private fun prefs(context: Context): SharedPreferences =
        context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun load(context: Context): ProfileState {
        val raw = prefs(context).getString(KEY_DATA, null) ?: return ProfileState()
        return try {
            val root = JSONObject(raw)
            val array = root.optJSONArray(FIELD_PROFILES) ?: JSONArray()
            val profiles = (0 until array.length()).mapNotNull { i ->
                val o = array.optJSONObject(i) ?: return@mapNotNull null
                val id = o.optString("id").takeIf { it.isNotEmpty() } ?: return@mapNotNull null
                val snapshotObj = o.optJSONObject("snapshot") ?: return@mapNotNull null
                Profile(
                    id = id,
                    name = o.optString("name", "?"),
                    abbrev = o.optString("abbrev", "?"),
                    deviceType = o.optInt("deviceType", DeviceType.UNSPECIFIED),
                    snapshot = ProfileSnapshot.fromJson(snapshotObj),
                )
            }
            val activeId = root.optString(FIELD_ACTIVE_ID, "").takeIf { it.isNotEmpty() }
            ProfileState(
                profiles = profiles,
                autoSwitch = root.optBoolean(FIELD_AUTO_SWITCH, false),
                // 保存されていた ID が一覧に無ければ(削除済みなど)無視する。
                activeProfileId = activeId?.takeIf { id -> profiles.any { it.id == id } },
            )
        } catch (e: JSONException) {
            ProfileState()
        }
    }

    fun save(context: Context, state: ProfileState) {
        val root = JSONObject()
        root.put(FIELD_VERSION, CURRENT_VERSION)
        val array = JSONArray()
        state.profiles.forEach { profile ->
            val o = JSONObject()
            o.put("id", profile.id)
            o.put("name", profile.name)
            o.put("abbrev", profile.abbrev)
            o.put("deviceType", profile.deviceType)
            o.put("snapshot", profile.snapshot.toJson())
            array.put(o)
        }
        root.put(FIELD_PROFILES, array)
        root.put(FIELD_AUTO_SWITCH, state.autoSwitch)
        if (state.activeProfileId != null) root.put(FIELD_ACTIVE_ID, state.activeProfileId)
        prefs(context).edit().putString(KEY_DATA, root.toString()).apply()
    }

    fun newId(): String = UUID.randomUUID().toString()
}
