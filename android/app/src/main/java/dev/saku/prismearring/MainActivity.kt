package dev.saku.prismearring

import android.Manifest
import android.app.Activity
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.text.InputType
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.snackbar.Snackbar
import dev.saku.prismearring.databinding.ActivityMainBinding
import dev.saku.prismearring.databinding.SheetInfoBinding
import kotlin.math.roundToInt

/**
 * 画面。エンジンは持たず、[PrismService] に bind して状態を読み、
 * パラメータの変更を Service 経由で流し込むだけ。
 *
 * 意匠は web/index.html + web/styles.css を踏襲する(ライト/ダーク両対応 /
 * 大きな数値 / 太いスライダ / 72dp の −+ / セグメント / 固定フッタ)。
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    private var service: PrismService? = null
    private var bound = false
    private var params = Params()

    /** リスナー由来の setProgress で再帰的にコールバックが走るのを防ぐ。 */
    private var suppressListeners = false

    /** 権限許可の直後に開始したい、というユーザーの意図を覚えておく。 */
    private var startAfterPermission = false

    // ---- 音源(v0.4.0): デバイス一覧 / MediaProjection --------------------------

    private data class DeviceEntry(val id: Int, val label: String)

    private var outputDevices: List<DeviceEntry> = emptyList()
    private var inputDevices: List<DeviceEntry> = emptyList()

    private val audioManager: AudioManager by lazy {
        getSystemService(AudioManager::class.java)
    }

    private val projectionManager: MediaProjectionManager by lazy {
        getSystemService(MediaProjectionManager::class.java)
    }

    /** 抜き差しのたびにデバイス一覧(出力先 / 入力元スピナー)を作り直す。 */
    private val audioDeviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>) {
            refreshDeviceLists()
        }

        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>) {
            refreshDeviceLists()
        }
    }

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binderIn: IBinder?) {
            val binder = binderIn as? PrismService.LocalBinder ?: return
            val s = binder.service
            service = s
            bound = true
            // Service 側が持っている値を正とする(通知から停止された場合などに追従)。
            params = s.currentParams()
            suppressListeners = true
            bindParamsToViews()
            suppressListeners = false
            s.setListener { state -> runOnUiThread { renderState(state) } }
            renderState(s.state)
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            service?.setListener(null)
            service = null
            bound = false
        }
    }

    private val micPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) {
                if (startAfterPermission) startProcessing()
            } else {
                showError(getString(R.string.perm_mic_required))
            }
            startAfterPermission = false
        }

    private val notificationPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { /* 任意 */ }

    /**
     * 「他アプリの音を拾う」の同意画面(画面キャプチャの許可ダイアログ)。
     * OK なら resultCode / data を Service へ渡して捕獲を開始する。
     * キャンセル・拒否なら「捕獲 ON」の意図を下ろし、マイクのみで動かし続ける。
     */
    private val projectionLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            val data = result.data
            val s = service
            if (result.resultCode == Activity.RESULT_OK && data != null && s != null) {
                if (s.startCapture(result.resultCode, data)) {
                    params = s.currentParams()
                    suppressListeners = true
                    bindParamsToViews()
                    suppressListeners = false
                } else {
                    Snackbar.make(binding.root, R.string.capture_denied_hint, Snackbar.LENGTH_LONG)
                        .show()
                    revertCaptureSwitch()
                }
            } else {
                Snackbar.make(binding.root, R.string.capture_denied_hint, Snackbar.LENGTH_LONG).show()
                revertCaptureSwitch()
            }
        }

    /** 同意が得られなかった場合に、スイッチとパラメータを OFF へ戻す。 */
    private fun revertCaptureSwitch() {
        val s = service
        s?.stopCapture()
        params = (s?.currentParams() ?: params).copy(captureEnabled = false)
        service?.updateParams(params) ?: params.save(this)
        suppressListeners = true
        bindParamsToViews()
        suppressListeners = false
    }

    private fun launchProjectionConsent() {
        projectionLauncher.launch(projectionManager.createScreenCaptureIntent())
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        // テーマは setContentView より前に確定させる必要がある。SharedPreferences は
        // Context さえあれば super.onCreate() 前でも読める(attachBaseContext は
        // performCreate の中で onCreate より先に走る)。
        AppCompatDelegate.setDefaultNightMode(nightModeFor(Params.load(this).themeMode))
        super.onCreate(savedInstanceState)

        // Edge-to-edge。fitsSystemWindows には頼らず、inset は自分で配る。
        WindowCompat.setDecorFitsSystemWindows(window, false)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        applySystemBarInsets()
        applyStatusBarAppearance()

        params = Params.load(this)
        setUpSliders()
        setUpSteppers()
        setUpSegments()
        setUpSplitSwitch()
        setUpAdvancedToggle()
        setUpDock()
        setUpInfoButtons()
        setUpShiftValueInputs()
        setUpJumpRow()
        setUpPresetButtons()
        setUpSourceControls()
        setUpDeviceSpinners()
        setUpSweepSpinner()
        setUpOutputUsageSwitch()

        refreshDeviceLists()
        audioManager.registerAudioDeviceCallback(audioDeviceCallback, null)
        applyVolumeControlStream(params.outputUsage)

        suppressListeners = true
        bindParamsToViews()
        suppressListeners = false

        requestNotificationPermissionIfNeeded()
    }

    override fun onStart() {
        super.onStart()
        // Service は開始していなくても bind できる(BIND_AUTO_CREATE)。
        bindService(Intent(this, PrismService::class.java), connection, Context.BIND_AUTO_CREATE)
    }

    override fun onStop() {
        service?.setListener(null)
        if (bound) {
            unbindService(connection)
            bound = false
        }
        service = null
        super.onStop()
    }

    override fun onDestroy() {
        audioManager.unregisterAudioDeviceCallback(audioDeviceCallback)
        super.onDestroy()
    }

    /** 出力用途がユーザー補助のときは端末の音量キーもユーザー補助音量に切り替える。 */
    private fun applyVolumeControlStream(outputUsage: Int) {
        setVolumeControlStream(
            if (outputUsage == NativeEngine.USAGE_ACCESSIBILITY) {
                AudioManager.STREAM_ACCESSIBILITY
            } else {
                AudioManager.STREAM_MUSIC
            }
        )
    }

    // ---- レイアウト --------------------------------------------------------

    /**
     * ジェスチャバー / ノッチ / ディスプレイカットアウトの下にコントロールが
     * 潜り込まないようにする。fitsSystemWindows には頼らず、上端の inset は
     * ヘッダの paddingTop に、下端の inset は固定フッタの paddingBottom に、
     * それぞれ既存の余白へ加算する形で適用する。左右は画面全体(root)へ。
     */
    private fun applySystemBarInsets() {
        val header = binding.headerRow
        val footer = binding.footer
        val headerInitialTop = header.paddingTop
        val footerInitialBottom = footer.paddingBottom
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { view, insets ->
            val bars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()
            )
            view.updatePadding(left = bars.left, right = bars.right)
            header.updatePadding(top = headerInitialTop + bars.top)
            footer.updatePadding(bottom = footerInitialBottom + bars.bottom)
            insets
        }
        ViewCompat.requestApplyInsets(binding.root)
    }

    /** ステータスバー / ナビゲーションバーのアイコン色を、現在解決されているテーマに合わせる。 */
    private fun applyStatusBarAppearance() {
        val isNight = (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES
        val controller = WindowCompat.getInsetsController(window, window.decorView)
        controller.isAppearanceLightStatusBars = !isNight
        controller.isAppearanceLightNavigationBars = !isNight
    }

    private fun nightModeFor(themeMode: Int): Int = when (themeMode) {
        Params.THEME_LIGHT -> AppCompatDelegate.MODE_NIGHT_NO
        Params.THEME_DARK -> AppCompatDelegate.MODE_NIGHT_YES
        else -> AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM
    }

    // ---- 入力ハンドラ ------------------------------------------------------

    private fun setUpSliders() {
        // Slider は Float。値は Params の型(Int/Float)に応じて丸めて反映する。
        binding.shiftLSlider.addOnChangeListener { _, value, fromUser ->
            if (fromUser && !suppressListeners) {
                updateParams(params.copy(shiftCentsL = value.roundToInt()))
            }
        }
        binding.shiftRSlider.addOnChangeListener { _, value, fromUser ->
            if (fromUser && !suppressListeners) {
                updateParams(params.copy(shiftCentsR = value.roundToInt()))
            }
        }
        binding.dryWetSlider.addOnChangeListener { _, value, fromUser ->
            if (fromUser && !suppressListeners) {
                updateParams(params.copy(dryWet = value))
            }
        }
        binding.crossfadeSlider.addOnChangeListener { _, value, fromUser ->
            if (fromUser && !suppressListeners) {
                updateParams(params.copy(crossfadeMs = value.roundToInt()))
            }
        }
        binding.volumeSlider.addOnChangeListener { _, value, fromUser ->
            if (fromUser && !suppressListeners) {
                updateParams(params.copy(outputGainDb = value))
            }
        }
    }

    private fun setUpSteppers() {
        // sign は方向だけ(-1/+1)。実際の刻み幅は現在の刻み幅プリセットから毎回引く。
        binding.stepLDown.setOnClickListener { nudgeLeft(-1) }
        binding.stepLUp.setOnClickListener { nudgeLeft(1) }
        binding.stepRDown.setOnClickListener { nudgeRight(-1) }
        binding.stepRUp.setOnClickListener { nudgeRight(1) }

        binding.dryWetDown.setOnClickListener { nudgeDryWet(-0.05f) }
        binding.dryWetUp.setOnClickListener { nudgeDryWet(0.05f) }
        binding.crossfadeDown.setOnClickListener { nudgeCrossfade(-1) }
        binding.crossfadeUp.setOnClickListener { nudgeCrossfade(1) }
        binding.volumeDown.setOnClickListener { nudgeVolume(-1.0f) }
        binding.volumeUp.setOnClickListener { nudgeVolume(1.0f) }
    }

    private fun nudgeLeft(sign: Int) = updateParams(
        params.copy(
            shiftCentsL = (params.shiftCentsL + sign * params.shiftStepCents)
                .coerceIn(params.sliderMin, params.sliderMax)
        )
    )

    private fun nudgeRight(sign: Int) = updateParams(
        params.copy(
            shiftCentsR = (params.effectiveRight + sign * params.shiftStepCents)
                .coerceIn(params.sliderMin, params.sliderMax)
        )
    )

    private fun nudgeDryWet(delta: Float) = updateParams(
        params.copy(
            dryWet = (params.dryWet + delta).coerceIn(Params.DRY_WET_MIN, Params.DRY_WET_MAX)
        )
    )

    private fun nudgeCrossfade(delta: Int) = updateParams(
        params.copy(
            crossfadeMs = (params.crossfadeMs + delta)
                .coerceIn(Params.CROSSFADE_MS_MIN, Params.CROSSFADE_MS_MAX)
        )
    )

    private fun nudgeVolume(delta: Float) = updateParams(
        params.copy(
            outputGainDb = (params.outputGainDb + delta)
                .coerceIn(Params.OUTPUT_GAIN_DB_MIN, Params.OUTPUT_GAIN_DB_MAX)
        )
    )

    /**
     * シフト量を指定値へジャンプさせる。左右独立が ON でも常に L/R 両方を同じ値にする
     * (仕様はシンプルさ優先。片方だけ動かしたい場合はスライダ / ステッパを使う)。
     */
    private fun jumpShiftTo(value: Int) = updateParams(
        params.copy(shiftCentsL = value, shiftCentsR = value)
    )

    private fun setUpJumpRow() {
        binding.jumpMin.setOnClickListener { jumpShiftTo(Params.DSP_SHIFT_CENTS_MIN) }
        binding.jumpZero.setOnClickListener { jumpShiftTo(0) }
        binding.jumpDefault.setOnClickListener { jumpShiftTo(Params.DEFAULT_SHIFT_CENTS) }
        binding.jumpMax.setOnClickListener { jumpShiftTo(Params.DSP_SHIFT_CENTS_MAX) }
    }

    // ---- ユーザープリセット(P1〜P3)------------------------------------------

    private fun setUpPresetButtons() {
        val slots = listOf(1 to binding.presetSlot1, 2 to binding.presetSlot2, 3 to binding.presetSlot3)
        slots.forEach { (slot, view) ->
            view.setOnClickListener { onPresetTap(slot) }
            view.setOnLongClickListener { onPresetLongPress(slot); true }
        }
    }

    /** タップ: 登録済みならその値へジャンプ、未登録なら登録方法を案内する。 */
    private fun onPresetTap(slot: Int) {
        if (!params.isPresetSet(slot)) {
            Snackbar.make(binding.root, R.string.preset_unset_hint, Snackbar.LENGTH_SHORT).show()
            return
        }
        updateParams(
            params.copy(shiftCentsL = params.presetL(slot), shiftCentsR = params.presetR(slot))
        )
    }

    /** 長押し: 現在の L/R ペアをそのスロットへ上書き登録する。 */
    private fun onPresetLongPress(slot: Int) {
        val l = params.shiftCentsL
        val r = params.effectiveRight
        updateParams(params.withPresetSet(slot, l, r))
        val message = if (l == r) {
            getString(R.string.preset_saved_single, slot, formatCents(l))
        } else {
            getString(R.string.preset_saved_pair, slot, formatCents(l), formatCents(r))
        }
        Snackbar.make(binding.root, message, Snackbar.LENGTH_SHORT).show()
    }

    private fun updatePresetLabel(view: TextView, slot: Int) {
        val name = getString(R.string.preset_slot_name, slot)
        val valueText = if (params.isPresetSet(slot)) {
            val l = params.presetL(slot)
            val r = params.presetR(slot)
            if (l == r) formatCents(l) else "${formatCents(l)}/${formatCents(r)}"
        } else {
            getString(R.string.preset_slot_unset)
        }
        view.text = "$name\n$valueText"
        view.contentDescription = getString(R.string.desc_preset_slot, slot, valueText)
    }

    // ---- 音源(v0.4.0): マイク音量 / 他アプリの音を拾う ---------------------------

    private fun setUpSourceControls() {
        binding.micGainSlider.addOnChangeListener { _, value, fromUser ->
            if (fromUser && !suppressListeners) {
                updateParams(params.copy(micGainDb = value))
            }
        }
        binding.micGainDown.setOnClickListener { nudgeMicGain(-1.0f) }
        binding.micGainUp.setOnClickListener { nudgeMicGain(1.0f) }

        binding.captureGainSlider.addOnChangeListener { _, value, fromUser ->
            if (fromUser && !suppressListeners) {
                updateParams(params.copy(captureGainDb = value))
            }
        }
        binding.captureGainDown.setOnClickListener { nudgeCaptureGain(-1.0f) }
        binding.captureGainUp.setOnClickListener { nudgeCaptureGain(1.0f) }

        binding.captureSwitch.setOnCheckedChangeListener { _, checked ->
            if (suppressListeners) return@setOnCheckedChangeListener
            if (checked) requestCaptureEnable() else disableCapture()
        }
    }

    private fun nudgeMicGain(delta: Float) = updateParams(
        params.copy(
            micGainDb = (params.micGainDb + delta).coerceIn(Params.MIC_GAIN_DB_MIN, Params.MIC_GAIN_DB_MAX)
        )
    )

    private fun nudgeCaptureGain(delta: Float) = updateParams(
        params.copy(
            captureGainDb = (params.captureGainDb + delta)
                .coerceIn(Params.CAPTURE_GAIN_DB_MIN, Params.CAPTURE_GAIN_DB_MAX)
        )
    )

    /**
     * 捕獲 ON への切り替え。動作中でなければ設定を保存するだけ(次の開始時に同意を求める)。
     * 動作中なら、その場で MediaProjection の同意画面を出す。
     */
    private fun requestCaptureEnable() {
        if (service?.isRunning() != true) {
            updateParams(params.copy(captureEnabled = true))
            return
        }
        launchProjectionConsent()
    }

    private fun disableCapture() {
        service?.stopCapture()
        updateParams(params.copy(captureEnabled = false))
    }

    private fun micGainLabel(db: Float): String =
        if (db <= Params.MIC_GAIN_DB_MIN) getString(R.string.mic_gain_off) else getString(R.string.volume_format, db)

    /** 「状態の一行」の文言。実際に捕獲が動いているかは [PrismService.State.captureActive] を見る。 */
    private fun captureStatusText(params: Params, running: Boolean, captureActive: Boolean, fillFrames: Int): String =
        when {
            !params.captureEnabled -> getString(R.string.capture_status_stopped)
            !running -> getString(R.string.capture_status_stopped)
            !captureActive -> getString(R.string.capture_status_waiting)
            fillFrames <= 0 -> getString(R.string.capture_status_silence)
            else -> getString(R.string.capture_status_capturing)
        }

    // ---- 音源(v0.4.0): 出力先 / 入力元デバイス -------------------------------------

    private fun deviceTypeLabel(type: Int): String = when (type) {
        AudioDeviceInfo.TYPE_USB_HEADSET -> getString(R.string.device_type_usb_headset)
        AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_ACCESSORY ->
            getString(R.string.device_type_usb_device)
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> getString(R.string.device_type_wired_headset)
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> getString(R.string.device_type_wired_headphones)
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP, AudioDeviceInfo.TYPE_BLUETOOTH_SCO ->
            getString(R.string.device_type_bluetooth)
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> getString(R.string.device_type_speaker)
        AudioDeviceInfo.TYPE_BUILTIN_MIC -> getString(R.string.device_type_builtin_mic)
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> getString(R.string.device_type_earpiece)
        else -> getString(R.string.device_type_other)
    }

    private fun deviceLabel(info: AudioDeviceInfo): String {
        val type = deviceTypeLabel(info.type)
        val name = info.productName?.toString()?.trim().orEmpty()
        return if (name.isEmpty() || name == "?") type else "$type $name"
    }

    private fun setUpDeviceSpinners() {
        binding.outputDeviceSpinner.onItemSelectedListener =
            object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    if (suppressListeners) return
                    val entry = outputDevices.getOrNull(position) ?: return
                    if (entry.id == params.outputDeviceId) return
                    updateParams(params.copy(outputDeviceId = entry.id))
                }

                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            }
        binding.inputDeviceSpinner.onItemSelectedListener =
            object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    if (suppressListeners) return
                    val entry = inputDevices.getOrNull(position) ?: return
                    if (entry.id == params.inputDeviceId) return
                    updateParams(params.copy(inputDeviceId = entry.id))
                }

                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            }
    }

    private fun spinnerAdapter(labels: List<String>): ArrayAdapter<String> =
        ArrayAdapter(this, android.R.layout.simple_spinner_item, labels).apply {
            setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        }

    /**
     * 出力先 / 入力元の一覧を作り直す。抜き差しのたびに [audioDeviceCallback] から呼ばれる。
     * 選んでいたデバイスが無くなっていた場合は「自動」を選択状態にする(設定値そのものは
     * 変えない。次に同じデバイスが挿さればまた選ばれる)。
     */
    private fun refreshDeviceLists() {
        val outputs = mutableListOf(DeviceEntry(NativeEngine.DEVICE_AUTO, getString(R.string.device_auto)))
        audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).forEach {
            outputs += DeviceEntry(it.id, deviceLabel(it))
        }
        val inputs = mutableListOf(DeviceEntry(NativeEngine.DEVICE_AUTO, getString(R.string.device_auto)))
        audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS).forEach {
            inputs += DeviceEntry(it.id, deviceLabel(it))
        }
        outputDevices = outputs
        inputDevices = inputs

        val wasSuppressed = suppressListeners
        suppressListeners = true
        binding.outputDeviceSpinner.adapter = spinnerAdapter(outputs.map { it.label })
        binding.inputDeviceSpinner.adapter = spinnerAdapter(inputs.map { it.label })
        binding.outputDeviceSpinner.setSelection(
            outputs.indexOfFirst { it.id == params.outputDeviceId }.takeIf { it >= 0 } ?: 0
        )
        binding.inputDeviceSpinner.setSelection(
            inputs.indexOfFirst { it.id == params.inputDeviceId }.takeIf { it >= 0 } ?: 0
        )
        suppressListeners = wasSuppressed
    }

    // ---- 音源(v0.4.0): 走査幅 / 出力用途(詳細設定) -----------------------------

    private fun sweepLabel(ms: Float): String =
        if (ms == ms.toInt().toFloat()) "${ms.toInt()} ms" else "$ms ms"

    private fun setUpSweepSpinner() {
        binding.sweepSpinner.adapter = spinnerAdapter(Params.SWEEP_PRESETS.map { sweepLabel(it) })
        binding.sweepSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                if (suppressListeners) return
                val value = Params.SWEEP_PRESETS.getOrNull(position) ?: return
                if (value == params.micSweepMs) return
                updateParams(params.copy(micSweepMs = value))
            }

            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }
    }

    private fun setUpOutputUsageSwitch() {
        binding.outputUsageSwitch.setOnCheckedChangeListener { _, checked ->
            if (suppressListeners) return@setOnCheckedChangeListener
            val usage = if (checked) NativeEngine.USAGE_ACCESSIBILITY else NativeEngine.USAGE_MEDIA
            updateParams(params.copy(outputUsage = usage))
        }
    }

    // ---- 数値の直接入力 --------------------------------------------------------

    private fun setUpShiftValueInputs() {
        binding.shiftLValue.setOnClickListener { showShiftInputDialog(isRight = false) }
        binding.shiftRValue.setOnClickListener { showShiftInputDialog(isRight = true) }
    }

    private fun showShiftInputDialog(isRight: Boolean) {
        val current = if (isRight) params.effectiveRight else params.shiftCentsL
        val density = resources.displayMetrics.density
        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED
            setText(current.toString())
            setSelection(text.length)
            hint = getString(R.string.shift_input_hint)
            imeOptions = EditorInfo.IME_ACTION_DONE
        }
        val container = FrameLayout(this).apply {
            val horizontal = (20 * density).roundToInt()
            setPadding(horizontal, 0, horizontal, 0)
            addView(input)
        }
        val titleRes = if (isRight) R.string.shift_input_title_r else R.string.shift_input_title_l
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(titleRes)
            .setView(container)
            .setPositiveButton(R.string.dialog_ok) { _, _ -> applyShiftInput(input.text.toString(), isRight) }
            .setNegativeButton(R.string.dialog_cancel, null)
            .create()
        input.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_DONE) {
                applyShiftInput(input.text.toString(), isRight)
                dialog.dismiss()
                true
            } else {
                false
            }
        }
        dialog.show()
    }

    /** 空・非数は無視する。範囲外は DSP の有効範囲(±1200)へ丸める。 */
    private fun applyShiftInput(text: String, isRight: Boolean) {
        val value = text.trim().toIntOrNull() ?: return
        val coerced = value.coerceIn(Params.DSP_SHIFT_CENTS_MIN, Params.DSP_SHIFT_CENTS_MAX)
        updateParams(
            if (isRight) params.copy(shiftCentsR = coerced) else params.copy(shiftCentsL = coerced)
        )
    }

    private fun setUpSegments() {
        val presets = listOf(binding.preset0, binding.preset1, binding.preset2, binding.preset3)
        presets.forEachIndexed { index, view ->
            view.setOnClickListener {
                updateParams(params.copy(crossfadeMs = Params.CROSSFADE_PRESETS[index]))
            }
        }

        val steps = listOf(binding.step0, binding.step1, binding.step2, binding.step3)
        steps.forEachIndexed { index, view ->
            view.setOnClickListener {
                updateParams(params.copy(stepCents = Params.STEP_PRESETS[index]))
            }
        }

        val themeViews = listOf(
            Params.THEME_SYSTEM to binding.themeSystem,
            Params.THEME_LIGHT to binding.themeLight,
            Params.THEME_DARK to binding.themeDark,
        )
        themeViews.forEach { (mode, view) ->
            view.setOnClickListener { setThemeMode(mode) }
        }
    }

    /** 保存してから夜間モードを切り替える。実際に値が変わればアクティビティが再生成される。 */
    private fun setThemeMode(mode: Int) {
        if (params.themeMode == mode) return
        updateParams(params.copy(themeMode = mode))
        AppCompatDelegate.setDefaultNightMode(nightModeFor(mode))
    }

    private fun setUpSplitSwitch() {
        binding.splitSwitch.setOnCheckedChangeListener { _, checked ->
            if (suppressListeners) return@setOnCheckedChangeListener
            // OFF -> ON にした瞬間は R を L に揃える(いきなり左右がずれない)。
            val next = if (checked) {
                params.copy(splitChannels = true, shiftCentsR = params.shiftCentsL)
            } else {
                params.copy(splitChannels = false)
            }
            updateParams(next)
        }
    }

    private fun setUpAdvancedToggle() {
        binding.advToggle.setOnClickListener {
            val opening = binding.advBody.visibility != View.VISIBLE
            binding.advBody.visibility = if (opening) View.VISIBLE else View.GONE
            binding.advChevron.setImageResource(
                if (opening) R.drawable.ic_chevron_up else R.drawable.ic_chevron_down
            )
        }
    }

    private fun setUpDock() {
        binding.toggleButton.setOnClickListener {
            if (service?.isRunning() == true) stopProcessing() else requestStart()
        }
    }

    private fun setUpInfoButtons() {
        infoButton(binding.presetInfoButton, R.string.info_presets_title, R.string.info_presets_body)
        infoButton(binding.shiftInfoButton, R.string.info_shift_title, R.string.info_shift_body)
        infoButton(binding.stepInfoButton, R.string.info_step_title, R.string.info_step_body)
        infoButton(binding.splitInfoButton, R.string.info_split_title, R.string.info_split_body)
        infoButton(binding.volumeInfoButton, R.string.info_volume_title, R.string.info_volume_body)
        infoButton(binding.dryWetInfoButton, R.string.info_drywet_title, R.string.info_drywet_body)
        infoButton(
            binding.crossfadeInfoButton, R.string.info_crossfade_title, R.string.info_crossfade_body
        )
        infoButton(binding.latencyInfoButton, R.string.info_latency_title, R.string.info_latency_body)
        infoButton(
            binding.jumpPresetInfoButton, R.string.info_jump_preset_title, R.string.info_jump_preset_body
        )
        infoButton(binding.micGainInfoButton, R.string.info_mic_gain_title, R.string.info_mic_gain_body)
        infoButton(binding.captureInfoButton, R.string.info_capture_title, R.string.info_capture_body)
        infoButton(
            binding.outputDeviceInfoButton, R.string.info_output_device_title, R.string.info_output_device_body
        )
        infoButton(
            binding.inputDeviceInfoButton, R.string.info_input_device_title, R.string.info_input_device_body
        )
        infoButton(binding.sweepInfoButton, R.string.info_sweep_title, R.string.info_sweep_body)
        infoButton(
            binding.outputUsageInfoButton, R.string.info_output_usage_title, R.string.info_output_usage_body
        )
    }

    private fun infoButton(view: ImageView, titleRes: Int, bodyRes: Int) {
        view.contentDescription = getString(R.string.info_button_desc_format, getString(titleRes))
        view.setOnClickListener { showInfoSheet(titleRes, bodyRes) }
    }

    /** 「どう変えるとどう聞こえるか」を書いた説明を BottomSheetDialog で見せる。 */
    private fun showInfoSheet(titleRes: Int, bodyRes: Int) {
        val sheet = SheetInfoBinding.inflate(layoutInflater)
        sheet.sheetTitle.text = getString(titleRes)
        sheet.sheetBody.text = getString(bodyRes)
        BottomSheetDialog(this).apply {
            setContentView(sheet.root)
            show()
        }
    }

    // ---- 開始 / 停止 -------------------------------------------------------

    private fun requestStart() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            startAfterPermission = true
            micPermissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
            return
        }
        startProcessing()
    }

    private fun startProcessing() {
        hideError()
        // 常駐させるため、bind とは別に startForegroundService でも起こす。
        // これをしないと Activity が終わった時点で Service が落ちる。
        ContextCompat.startForegroundService(this, PrismService.startIntent(this))
        val s = service
        if (s == null) {
            // まだ bind が済んでいない。Service 側が ACTION_START で自力起動する。
            return
        }
        if (!s.startProcessing()) {
            showError(s.state.error.ifEmpty { getString(R.string.perm_mic_required) })
            return
        }
        // 「他アプリの音を拾う」が ON のまま開始したので、ここで同意画面を出す。
        // 拒否されたらマイクのみで動作を続ける([projectionLauncher] 側で処理)。
        if (params.captureEnabled) {
            launchProjectionConsent()
        }
    }

    private fun stopProcessing() {
        service?.stopProcessing()
        startService(PrismService.stopIntent(this))
        renderState(service?.state ?: PrismService.State())
    }

    /**
     * 再起動が要る設定(出力先 / 入力元 / 出力用途 / 走査幅)を動作中に変えたときに呼ぶ。
     * Service を stop → start し直し、捕獲 ON なら同意画面が再び出ることを案内する。
     */
    private fun restartForSettings() {
        val s = service ?: return
        if (params.captureEnabled) {
            Snackbar.make(binding.root, R.string.restart_capture_consent_hint, Snackbar.LENGTH_LONG).show()
        }
        s.stopProcessing()
        if (!s.startProcessing()) {
            showError(s.state.error.ifEmpty { getString(R.string.perm_mic_required) })
            renderState(s.state)
            return
        }
        if (params.captureEnabled) {
            launchProjectionConsent()
        }
        renderState(s.state)
    }

    private fun requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
            == PackageManager.PERMISSION_GRANTED
        ) return
        notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
    }

    // ---- 表示 --------------------------------------------------------------

    private fun updateParams(next: Params) {
        if (suppressListeners) return
        val prev = params
        params = next
        service?.updateParams(next) ?: next.save(this)
        suppressListeners = true
        bindParamsToViews()
        suppressListeners = false

        if (prev.outputUsage != next.outputUsage) {
            applyVolumeControlStream(next.outputUsage)
        }

        // デバイス指定 / 出力用途 / 走査幅は次の start() からしか効かないため、動作中に
        // 変えた場合はここで stop → start する(捕獲 ON なら同意画面が再び出る)。
        if (service?.isRunning() == true && prev.requiresRestart(next)) {
            restartForSettings()
        }
    }

    /** params -> View。リスナーを止めた状態でのみ呼ぶこと。 */
    private fun bindParamsToViews() {
        binding.shiftLSlider.value = params.shiftCentsL.toFloat()
        binding.shiftLValue.text = formatCents(params.shiftCentsL)

        binding.shiftRSlider.value = params.effectiveRight.toFloat()
        binding.shiftRValue.text = formatCents(params.effectiveRight)

        val hint = stepHintText(params.stepCents)
        binding.shiftStepHintL.text = hint
        binding.shiftStepHintR.text = hint

        binding.splitSwitch.isChecked = params.splitChannels
        binding.chanR.visibility = if (params.splitChannels) View.VISIBLE else View.GONE
        binding.tagL.visibility = if (params.splitChannels) View.VISIBLE else View.GONE
        // L/R 独立時は 2 段になるので数値を縮める(web の .hero[data-split="on"])。
        binding.shiftLValue.textSize = if (params.splitChannels) 40f else 56f

        binding.dryWetSlider.value = params.dryWet.coerceIn(Params.DRY_WET_MIN, Params.DRY_WET_MAX)
        binding.dryWetValue.text = String.format("%.2f", params.dryWet)

        binding.crossfadeSlider.value =
            params.crossfadeMs.toFloat().coerceIn(Params.CROSSFADE_MS_MIN.toFloat(), Params.CROSSFADE_MS_MAX.toFloat())
        binding.crossfadeValue.text = getString(R.string.crossfade_format, params.crossfadeMs)

        binding.volumeSlider.value =
            params.outputGainDb.coerceIn(Params.OUTPUT_GAIN_DB_MIN, Params.OUTPUT_GAIN_DB_MAX)
        binding.volumeValue.text = getString(R.string.volume_format, params.outputGainDb)

        selectSegment(
            listOf(binding.preset0, binding.preset1, binding.preset2, binding.preset3),
            Params.CROSSFADE_PRESETS.indexOf(params.crossfadeMs),
        )
        selectSegment(
            listOf(binding.step0, binding.step1, binding.step2, binding.step3),
            Params.STEP_PRESETS.indexOf(params.stepCents),
        )
        selectSegment(
            listOf(binding.themeSystem, binding.themeLight, binding.themeDark),
            params.themeMode,
        )

        updatePresetLabel(binding.presetSlot1, 1)
        updatePresetLabel(binding.presetSlot2, 2)
        updatePresetLabel(binding.presetSlot3, 3)

        // --- 音源(v0.4.0) ---
        binding.micGainSlider.value = params.micGainDb.coerceIn(Params.MIC_GAIN_DB_MIN, Params.MIC_GAIN_DB_MAX)
        binding.micGainValue.text = micGainLabel(params.micGainDb)

        binding.captureSwitch.isChecked = params.captureEnabled
        binding.captureGainSlider.value =
            params.captureGainDb.coerceIn(Params.CAPTURE_GAIN_DB_MIN, Params.CAPTURE_GAIN_DB_MAX)
        val captureActive = service?.state?.captureActive == true
        val captureFillFrames = service?.state?.info?.captureFillFrames ?: 0
        binding.captureStatusText.text = getString(
            R.string.capture_status_with_gain,
            captureStatusText(params, service?.isRunning() == true, captureActive, captureFillFrames),
            params.captureGainDb,
        )

        val outIndex = outputDevices.indexOfFirst { it.id == params.outputDeviceId }.takeIf { it >= 0 } ?: 0
        binding.outputDeviceSpinner.setSelection(outIndex)
        val inIndex = inputDevices.indexOfFirst { it.id == params.inputDeviceId }.takeIf { it >= 0 } ?: 0
        binding.inputDeviceSpinner.setSelection(inIndex)

        val sweepIndex = Params.SWEEP_PRESETS.indexOf(params.micSweepMs).takeIf { it >= 0 } ?: 0
        binding.sweepSpinner.setSelection(sweepIndex)

        binding.outputUsageSwitch.isChecked = params.outputUsage == NativeEngine.USAGE_ACCESSIBILITY
    }

    private fun selectSegment(views: List<TextView>, selectedIndex: Int) {
        views.forEachIndexed { index, view -> view.isSelected = index == selectedIndex }
    }

    private fun stepName(stepCents: Int): String = when (stepCents) {
        Params.STEP_WHOLE_TONE -> getString(R.string.step_name_whole_tone)
        Params.STEP_SEMITONE -> getString(R.string.step_name_semitone)
        Params.STEP_QUARTER_TONE -> getString(R.string.step_name_quarter_tone)
        else -> getString(R.string.step_name_eighth_tone)
    }

    /** 「半音 下げる / 上げる」のように、現在の刻み幅を −/+ ボタン付近に表示する文言。 */
    private fun stepHintText(stepCents: Int): String {
        val name = stepName(stepCents)
        val down = getString(R.string.step_hint_down_format, name)
        val up = getString(R.string.step_hint_up_format, name)
        return "$down / $up"
    }

    /**
     * 符号つきで書式化する。プラス側も設定できるようになったため、上げているのか
     * 下げているのか一目で分かるよう符号を必ず添える。ちょうど 0 は「±0」
     * (単なる 0 だと符号の欠落と紛らわしい)。マイナスは U+2212(意匠上の「−」)。
     * web/main.js の formatCents と同じ規約。
     */
    private fun formatCents(value: Int): String = when {
        value > 0 -> "+$value"
        value < 0 -> "−${-value}"
        else -> "±0"
    }

    private fun renderState(state: PrismService.State) {
        val running = state.running
        binding.toggleButton.text = getString(if (running) R.string.action_stop else R.string.action_start)
        binding.toggleButton.setIconResource(if (running) R.drawable.ic_stop_icon else R.drawable.ic_play)
        binding.toggleButton.contentDescription =
            getString(if (running) R.string.desc_toggle_stop else R.string.desc_toggle_start)
        binding.toggleButton.backgroundTintList =
            ContextCompat.getColorStateList(this, if (running) R.color.stop else R.color.primary)

        binding.usageText.text = getString(if (running) R.string.usage_running else R.string.usage_stopped)

        val statusRes = when {
            !running -> R.string.status_stopped
            !state.synced -> R.string.status_starting
            else -> R.string.status_running
        }
        binding.statusText.setText(statusRes)

        val pillColor = if (running && state.synced) R.color.ok else R.color.idle
        val textColor = if (running && state.synced) R.color.ok_fg else R.color.idle_fg
        binding.statusPill.backgroundTintList = ContextCompat.getColorStateList(this, pillColor)
        binding.statusText.setTextColor(ContextCompat.getColor(this, textColor))
        binding.statusDot.backgroundTintList =
            ContextCompat.getColorStateList(this, if (running) R.color.ok_fg else R.color.muted)
        binding.latencyText.setTextColor(ContextCompat.getColor(this, textColor))

        binding.latencyText.text = when {
            !running -> getString(R.string.latency_unknown)
            !state.latency.valid -> getString(R.string.latency_measuring)
            else -> getString(R.string.latency_format, state.latency.totalMs)
        }

        binding.diagText.text = if (state.info.sampleRate <= 0) {
            getString(R.string.diag_placeholder)
        } else {
            getString(
                R.string.diag_format,
                state.info.sampleRate,
                state.info.inputChannels,
                state.info.outputChannels,
                state.info.framesPerBurst,
                getString(if (state.info.exclusive) R.string.yes else R.string.no),
                getString(if (state.info.unprocessedInput) R.string.yes else R.string.no),
                state.latency.inputMs,
                state.latency.outputMs,
                state.latency.dspMs,
                state.info.underruns,
                state.info.inputErrors,
                state.info.captureFillFrames,
                state.info.captureUnderruns,
                state.info.captureOverruns,
                state.latency.captureSweepMs,
                state.info.outputDeviceId,
                getString(
                    if (state.info.outputDeviceFallback) R.string.device_fallback_yes
                    else R.string.device_fallback_no
                ),
                state.info.inputDeviceId,
                getString(
                    if (state.info.inputDeviceFallback) R.string.device_fallback_yes
                    else R.string.device_fallback_no
                ),
                getString(
                    if (state.info.outputUsage == NativeEngine.USAGE_ACCESSIBILITY) {
                        R.string.usage_accessibility_short
                    } else {
                        R.string.usage_media_short
                    }
                ),
                state.latency.micSweepMs,
            )
        }

        // 状態(動作中か・実際に捕獲できているか)が変わるたびに更新する。
        binding.captureStatusText.text = getString(
            R.string.capture_status_with_gain,
            captureStatusText(params, running, state.captureActive, state.info.captureFillFrames),
            params.captureGainDb,
        )

        if (state.error.isNotEmpty()) showError(state.error) else hideError()
    }

    private fun showError(message: String) {
        binding.errorBox.text = message
        binding.errorBox.visibility = View.VISIBLE
    }

    private fun hideError() {
        binding.errorBox.visibility = View.GONE
    }
}
