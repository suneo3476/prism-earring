package dev.saku.prismearring

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.view.View
import android.widget.SeekBar
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import dev.saku.prismearring.databinding.ActivityMainBinding
import kotlin.math.roundToInt

/**
 * 画面。エンジンは持たず、[PrismService] に bind して状態を読み、
 * パラメータの変更を Service 経由で流し込むだけ。
 *
 * 意匠は web/index.html + web/styles.css を踏襲する(ダーク / 大きな数値 /
 * 太いスライダ / 72dp の −+ / セグメント / 固定フッタ)。
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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, true)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        applySystemBarInsets()

        params = Params.load(this)
        setUpSliders()
        setUpSteppers()
        setUpSegments()
        setUpSplitSwitch()
        setUpAdvancedToggle()
        setUpDock()

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

    // ---- レイアウト --------------------------------------------------------

    /** ジェスチャバー / ノッチの下に操作要素が潜り込まないようにする。 */
    private fun applySystemBarInsets() {
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            view.updatePadding(left = bars.left, right = bars.right, bottom = bars.bottom)
            insets
        }
    }

    // ---- 入力ハンドラ ------------------------------------------------------

    private fun setUpSliders() {
        // SeekBar は 0 始まりの整数しか扱えないので、値 = 下限 + progress で写像する。
        binding.shiftLSlider.setOnSeekBarChangeListener(simpleListener { progress ->
            updateParams(params.copy(shiftCentsL = params.sliderFloor + progress))
        })
        binding.shiftRSlider.setOnSeekBarChangeListener(simpleListener { progress ->
            updateParams(params.copy(shiftCentsR = params.sliderFloor + progress))
        })
        binding.dryWetSlider.max = 100
        binding.dryWetSlider.setOnSeekBarChangeListener(simpleListener { progress ->
            updateParams(params.copy(dryWet = progress / 100.0f))
        })
        binding.crossfadeSlider.max = Params.CROSSFADE_MS_MAX - Params.CROSSFADE_MS_MIN
        binding.crossfadeSlider.setOnSeekBarChangeListener(simpleListener { progress ->
            updateParams(params.copy(crossfadeMs = Params.CROSSFADE_MS_MIN + progress))
        })
    }

    private fun setUpSteppers() {
        binding.stepLDown.setOnClickListener { nudgeLeft(-1) }
        binding.stepLUp.setOnClickListener { nudgeLeft(1) }
        binding.stepRDown.setOnClickListener { nudgeRight(-1) }
        binding.stepRUp.setOnClickListener { nudgeRight(1) }

        binding.dryWetDown.setOnClickListener { nudgeDryWet(-0.05f) }
        binding.dryWetUp.setOnClickListener { nudgeDryWet(0.05f) }
        binding.crossfadeDown.setOnClickListener { nudgeCrossfade(-1) }
        binding.crossfadeUp.setOnClickListener { nudgeCrossfade(1) }
    }

    private fun nudgeLeft(delta: Int) = updateParams(
        params.copy(
            shiftCentsL = (params.shiftCentsL + delta)
                .coerceIn(params.sliderFloor, Params.DSP_SHIFT_CENTS_MAX)
        )
    )

    private fun nudgeRight(delta: Int) = updateParams(
        params.copy(
            shiftCentsR = (params.effectiveRight + delta)
                .coerceIn(params.sliderFloor, Params.DSP_SHIFT_CENTS_MAX)
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

    private fun setUpSegments() {
        val presets = listOf(binding.preset0, binding.preset1, binding.preset2)
        presets.forEachIndexed { index, view ->
            view.setOnClickListener {
                updateParams(params.copy(crossfadeMs = Params.CROSSFADE_PRESETS[index]))
            }
        }

        val ranges = listOf(binding.range0, binding.range1, binding.range2, binding.range3)
        ranges.forEachIndexed { index, view ->
            view.setOnClickListener {
                val floor = Params.SLIDER_FLOORS[index]
                // 範囲を狭めたときは、はみ出した現在値を新しい下限へ引き上げる。
                updateParams(
                    params.copy(
                        sliderFloor = floor,
                        shiftCentsL = params.shiftCentsL.coerceIn(floor, Params.DSP_SHIFT_CENTS_MAX),
                        shiftCentsR = params.shiftCentsR.coerceIn(floor, Params.DSP_SHIFT_CENTS_MAX),
                    )
                )
            }
        }
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
            binding.advToggle.setText(if (opening) R.string.adv_close else R.string.adv_open)
        }
    }

    private fun setUpDock() {
        binding.toggleButton.setOnClickListener {
            if (service?.isRunning() == true) stopProcessing() else requestStart()
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
        }
    }

    private fun stopProcessing() {
        service?.stopProcessing()
        startService(PrismService.stopIntent(this))
        renderState(service?.state ?: PrismService.State())
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
        params = next
        service?.updateParams(next) ?: next.save(this)
        suppressListeners = true
        bindParamsToViews()
        suppressListeners = false
    }

    /** params -> View。リスナーを止めた状態でのみ呼ぶこと。 */
    private fun bindParamsToViews() {
        val span = Params.DSP_SHIFT_CENTS_MAX - params.sliderFloor

        binding.shiftLSlider.max = span
        binding.shiftLSlider.progress = params.shiftCentsL - params.sliderFloor
        binding.shiftLValue.text = formatCents(params.shiftCentsL)

        binding.shiftRSlider.max = span
        binding.shiftRSlider.progress = params.effectiveRight - params.sliderFloor
        binding.shiftRValue.text = formatCents(params.effectiveRight)

        binding.splitSwitch.isChecked = params.splitChannels
        binding.chanR.visibility = if (params.splitChannels) View.VISIBLE else View.GONE
        binding.tagL.visibility = if (params.splitChannels) View.VISIBLE else View.GONE
        // L/R 独立時は 2 段になるので数値を縮める(web の .hero[data-split="on"])。
        binding.shiftLValue.textSize = if (params.splitChannels) 40f else 56f

        binding.dryWetSlider.progress = (params.dryWet * 100f).roundToInt()
        binding.dryWetValue.text = String.format("%.2f", params.dryWet)

        binding.crossfadeSlider.progress = params.crossfadeMs - Params.CROSSFADE_MS_MIN
        binding.crossfadeValue.text = getString(R.string.crossfade_format, params.crossfadeMs)

        selectSegment(
            listOf(binding.preset0, binding.preset1, binding.preset2),
            Params.CROSSFADE_PRESETS.indexOf(params.crossfadeMs),
        )
        selectSegment(
            listOf(binding.range0, binding.range1, binding.range2, binding.range3),
            Params.SLIDER_FLOORS.indexOf(params.sliderFloor),
        )

        binding.clampNote.visibility = if (params.isClamped) View.VISIBLE else View.GONE
    }

    private fun selectSegment(views: List<TextView>, selectedIndex: Int) {
        views.forEachIndexed { index, view -> view.isSelected = index == selectedIndex }
    }

    /** 符号つきで、マイナスは U+2212(意匠上の「−」)にする。 */
    private fun formatCents(value: Int): String =
        if (value < 0) "−${-value}" else value.toString()

    private fun renderState(state: PrismService.State) {
        val running = state.running
        binding.toggleButton.setText(if (running) R.string.action_stop else R.string.action_start)
        binding.toggleButton.backgroundTintList =
            ContextCompat.getColorStateList(this, if (running) R.color.stop else R.color.primary)

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
            )
        }

        if (state.error.isNotEmpty()) showError(state.error) else hideError()
    }

    private fun showError(message: String) {
        binding.errorBox.text = message
        binding.errorBox.visibility = View.VISIBLE
    }

    private fun hideError() {
        binding.errorBox.visibility = View.GONE
    }

    /** ユーザー操作(fromUser)のときだけ発火する SeekBar リスナー。 */
    private fun simpleListener(onChange: (Int) -> Unit) =
        object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar?, progress: Int, fromUser: Boolean) {
                if (fromUser && !suppressListeners) onChange(progress)
            }

            override fun onStartTrackingTouch(bar: SeekBar?) = Unit
            override fun onStopTrackingTouch(bar: SeekBar?) = Unit
        }
}
