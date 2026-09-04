/*
 * main.js — PrismEarring Web デモの UI 層(DemoUI)
 *
 * 素の ES module。外部ライブラリ・CDN・ビルドツールは使わない(SR-2.1)。
 * 契約 3(postMessage)経由でのみ Worklet と会話し、DSP には直接触れない。
 *
 * 対応する設計: u3-web-demo functional-spec WF-1〜WF-6 / frontend-components.md /
 * refined-mockups(mockups.md, interaction-spec.md, accessibility-checklist.md)
 */

import { encodeQr, toSvgPathData } from './qr.js';

/* ------------------------------ 定数 ------------------------------ */

/** パラメータ ID(契約 2 / 契約 3 で共通)。 */
const PARAM_SHIFT_CENTS_L = 0;
const PARAM_SHIFT_CENTS_R = 1;
const PARAM_DRY_WET = 2;
const PARAM_CROSSFADE_MS = 3;

/** 画面状態(functional-spec の状態機械)。 */
const State = {
    STOPPED: 'stopped',
    RUNNING: 'running',
    DENIED: 'denied',
    UNSUPPORTED: 'unsupported'
};

/** 入力ソース(WF-1 の取得経路だけが分岐する。以降の経路は共通)。 */
const Source = {
    MIC: 'mic',
    TAB: 'tab',
    FILE: 'file'
};

/** タブ音声の共有手順。取得失敗のたびに同じ案内を出すため定数化する。 */
const TAB_SHARE_HOWTO = [
    '共有ダイアログで「Chrome のタブ」(Edge/Vivaldi も同様)を選び、',
    '同じブラウザ内のタブをクリックしてから、左下の「タブの音声を共有」にチェックを入れてください。',
    '画面全体・ウィンドウを選んだ場合、音声は取得できません。'
];

/** レンダ量子。遅延内訳の「ブロック」計算に使う。 */
const RENDER_QUANTUM = 128;

/** Worklet の ready / error を待つ上限。無反応で固まらせない。 */
const READY_TIMEOUT_MS = 8000;

/**
 * プリセットが設定する窓長(ms)。dry_wet は触らない。
 * 遅延はディレイライン長で決まり窓長に依存しない(処理部で約 5ms 固定)ので、
 * ここで変わるのは音の滑らかさ(うねり・にじみ)だけ。説明文もそう書いてある。
 * 上限は Android 版(v0.4.0)に合わせて 200ms まで拡張してある。
 */
const PRESET_XFADE_MS = [20, 50, 100, 200];

/** シフト量スライダーの範囲(セント)。既定は微調整の ±150。 */
const DEFAULT_RANGE_CENTS = 150;

/** ±1200 の範囲では −/+ の刻みを 10 セントにする(1 セントでは遠すぎる)。 */
const COARSE_RANGE_CENTS = 1200;
const COARSE_STEP_CENTS = 10;

/** モーダル内でフォーカスを回す対象。 */
const FOCUSABLE = 'button, [href], input:not([type="hidden"]), select, textarea, [tabindex]:not([tabindex="-1"])';

const WORKLET_URL = './prism-worklet.js';
const WASM_URL = './prism.wasm';
const PROCESSOR_NAME = 'prism-processor';

/* ---------------------------- DOM 参照 ---------------------------- */

const el = {
    status: document.getElementById('status'),
    statusPill: document.getElementById('statusPill'),
    toggle: document.getElementById('toggle'),
    controls: document.getElementById('controls'),
    sourceSection: document.getElementById('source'),
    sourceRadios: Array.from(document.querySelectorAll('input[name="inputSource"]')),
    fileRow: document.getElementById('fileRow'),
    audioFile: document.getElementById('audioFile'),
    sourceHint: document.getElementById('sourceHint'),
    shiftL: document.getElementById('shiftL'),
    shiftR: document.getElementById('shiftR'),
    shiftLOut: document.getElementById('shiftLOut'),
    shiftROut: document.getElementById('shiftROut'),
    split: document.getElementById('split'),
    advParams: document.getElementById('advParams'),
    dryWet: document.getElementById('dryWet'),
    dryWetOut: document.getElementById('dryWetOut'),
    xfade: document.getElementById('xfade'),
    xfadeOut: document.getElementById('xfadeOut'),
    latTotal: document.getElementById('latTotal'),
    latOut: document.getElementById('latOut'),
    latBlock: document.getElementById('latBlock'),
    latDsp: document.getElementById('latDsp'),
    engine: document.getElementById('engine'),
    error: document.getElementById('error'),
    srcMic: document.getElementById('srcMic'),
    srcTab: document.getElementById('srcTab'),
    tabNote: document.getElementById('tabNote'),
    // 連動時にラベルから L を落とす対象(下記 renderShiftLabels を参照)
    stepLDown: document.getElementById('stepLDown'),
    stepLUp: document.getElementById('stepLUp'),
    stepRDown: document.getElementById('stepRDown'),
    stepRUp: document.getElementById('stepRUp'),
    // スライダ両脇の − / + ボタン。data-target / data-delta をマークアップから読む
    stepButtons: Array.from(document.querySelectorAll('.bigstep, .ministep')),
    // シフト量側だけ。範囲切替で刻み(data-delta)を書き換える対象
    shiftStepButtons: Array.from(document.querySelectorAll('.bigstep')),

    // プリセット / 範囲切替
    presetRadios: Array.from(document.querySelectorAll('input[name="preset"]')),
    rangeRadios: Array.from(document.querySelectorAll('input[name="shiftRange"]')),

    // 説明シートと QR モーダル
    hdr: document.getElementById('hdr'),
    appMain: document.getElementById('app'),
    dock: document.getElementById('dock'),
    infoButtons: Array.from(document.querySelectorAll('.infobtn')),
    infoTexts: document.getElementById('infoTexts'),
    infoSheet: document.getElementById('infoSheet'),
    infoTitle: document.getElementById('infoTitle'),
    infoBody: document.getElementById('infoBody'),
    infoClose: document.getElementById('infoClose'),
    qrBtn: document.getElementById('qrBtn'),
    qrModal: document.getElementById('qrModal'),
    qrFrame: document.getElementById('qrFrame'),
    qrUrl: document.getElementById('qrUrl'),
    qrNote: document.getElementById('qrNote'),
    qrClose: document.getElementById('qrClose'),
    scrims: Array.from(document.querySelectorAll('.overlay-scrim'))
};

/* ---------------------------- UIState ---------------------------- */

const ui = {
    state: State.STOPPED,
    /** L/R 連動。既定は連動(「左右を別々に設定」スイッチが OFF)。 */
    linkLR: !el.split.checked,
    /** 現在選択中の入力ソース(Source のいずれか)。 */
    source: Source.MIC,
    /** シフト量スライダーの範囲(±セント)。値はクランプせず min/max だけを動かす。 */
    rangeCents: DEFAULT_RANGE_CENTS,
    /** 開いているモーダル(null なら無し)。 */
    overlay: null,
    /** モーダルを開く直前のフォーカス位置。閉じたら必ずここへ戻す。 */
    lastFocus: null,
    /** getDisplayMedia が使えるか。スマホの Chrome は非対応なのでタブ音声を出せない。 */
    tabSupported: true,
    /** 'wasm'(本番)/ 'js'(フォールバック)/ null(停止中)。契約 3 の ready・latency から。 */
    engine: null,
    engineNote: '',
    latency: {
        outputMs: null,
        blockMs: null,
        dspMs: null,
        approximate: false
    }
};

/** オーディオグラフ。停止時は全て null に戻す。 */
const audio = {
    context: null,
    stream: null,
    source: null,
    node: null
};

/* -------------------------- 表示・描画 -------------------------- */

const STATUS_TEXT = {
    [State.STOPPED]: '停止中',
    [State.RUNNING]: '動作中',
    [State.DENIED]: 'マイク不許可',
    [State.UNSUPPORTED]: '非対応'
};

const TOGGLE_TEXT = {
    [State.STOPPED]: '▶ 開始',
    [State.RUNNING]: '■ 停止',
    [State.DENIED]: '▶ 再試行',
    [State.UNSUPPORTED]: '▶ 開始'
};

function render() {
    el.status.textContent = STATUS_TEXT[ui.state];
    // ピルは色でも状態を示す(CSS の [data-state] が拾う)
    el.statusPill.dataset.state = ui.state;
    el.toggle.textContent = TOGGLE_TEXT[ui.state];
    el.toggle.disabled = ui.state === State.UNSUPPORTED;
    // 文言だけでなく色でも状態が分かるようにする(CSS の [data-state] が拾う)
    el.toggle.dataset.state = ui.state;

    const controlsEnabled = ui.state === State.RUNNING;
    el.controls.setAttribute('aria-disabled', String(!controlsEnabled));
    el.advParams.setAttribute('aria-disabled', String(!controlsEnabled));
    for (const input of [el.shiftL, el.shiftR, el.split, el.dryWet, el.xfade]) {
        input.disabled = !controlsEnabled;
    }
    for (const button of el.stepButtons) {
        button.disabled = !controlsEnabled;
    }

    // ソース切替は停止中のみ。動作中に切り替えられると経路の整合が取れない
    const sourceEnabled = ui.state === State.STOPPED || ui.state === State.DENIED;
    el.sourceSection.setAttribute('aria-disabled', String(!sourceEnabled));
    for (const radio of el.sourceRadios) {
        radio.disabled = !sourceEnabled;
    }
    if (!ui.tabSupported) {
        // 非対応端末では停止中でもタブ音声を選ばせない(理由は #tabNote に出す)
        el.srcTab.disabled = true;
    }
    el.audioFile.disabled = !sourceEnabled;

    renderLatency();
    renderEngine();
}

const SOURCE_HINT = {
    [Source.MIC]: 'マイクで拾った外界の音を処理します。イヤホンを使ってください。',
    [Source.TAB]:
        '共有ダイアログで同じブラウザ内のタブを選び、「タブの音声を共有」にチェックを入れてください。' +
        '共有中は共有元タブの音を自動で止めるので、処理音だけが聞こえます' +
        '(止まらないブラウザでは共有元タブをミュートしてください)。',
    [Source.FILE]: '選んだ音声ファイルをループ再生して処理します(ファイルは端末外へ出ません)。'
};

/** 選択中ソースに応じて、ヒント文とファイル選択欄の出し入れを行う。 */
function renderSource() {
    el.sourceHint.textContent = SOURCE_HINT[ui.source];
    el.fileRow.hidden = ui.source !== Source.FILE;
}

/** ラジオの現在値を読む(未選択はあり得ないが、その場合はマイク扱い)。 */
function selectedSource() {
    for (const radio of el.sourceRadios) {
        if (radio.checked) {
            return radio.value;
        }
    }
    return Source.MIC;
}

/** 実際に動いている DSP エンジンを表示する(wasm が本番、js はフォールバック)。 */
function renderEngine() {
    if (ui.engine === null) {
        el.engine.textContent = 'engine: --';
        return;
    }
    if (ui.engine === 'wasm') {
        el.engine.textContent = 'engine: wasm(prism.wasm)';
        return;
    }
    el.engine.textContent =
        'engine: js(フォールバック' + (ui.engineNote ? ': ' + ui.engineNote : '') + ')';
}

function formatMs(value) {
    return value === null ? '--' : `${value.toFixed(2)} ms`;
}

function renderLatency() {
    const { outputMs, blockMs, dspMs, approximate } = ui.latency;
    el.latOut.textContent = formatMs(outputMs);
    el.latBlock.textContent = formatMs(blockMs);
    el.latDsp.textContent = formatMs(dspMs);
    if (outputMs === null || blockMs === null || dspMs === null) {
        el.latTotal.textContent = '-- ms';
        return;
    }
    const total = outputMs + blockMs + dspMs;
    el.latTotal.textContent = `${total.toFixed(2)} ms${approximate ? '(概算)' : ''}`;
}

function clearLatency() {
    ui.latency.outputMs = null;
    ui.latency.blockMs = null;
    ui.latency.dspMs = null;
    ui.latency.approximate = false;
    ui.engine = null;
    ui.engineNote = '';
    renderLatency();
    renderEngine();
}

/**
 * エラー領域を表示する。role="alert" なので挿入時に読み上げられる。
 * 表示後はフォーカスを #toggle(= 再試行)へ戻す(a11y チェックリスト 6)。
 */
function showError(lines, focusToggle = true) {
    el.error.replaceChildren();
    for (const line of lines) {
        const p = document.createElement('p');
        p.textContent = line;
        el.error.append(p);
    }
    el.error.hidden = false;
    if (focusToggle && !el.toggle.disabled) {
        el.toggle.focus();
    }
}

function clearError() {
    el.error.hidden = true;
    el.error.replaceChildren();
}

/* ------------------------ 機能検出(WF-4) ------------------------ */

function detectSupport() {
    const missing = [];
    if (typeof AudioWorkletNode === 'undefined' || typeof AudioContext === 'undefined') {
        missing.push('AudioWorklet');
    }
    // WASM(prism.wasm)が本番経路。functional-spec WF-4 どおり検出する。
    if (typeof WebAssembly === 'undefined') {
        missing.push('WebAssembly');
    }
    if (!navigator.mediaDevices || typeof navigator.mediaDevices.getUserMedia !== 'function') {
        missing.push('getUserMedia');
    }
    return missing;
}

/* ------------------------ パラメータ送信 ------------------------ */

function postParam(id, value) {
    if (!audio.node) {
        return;
    }
    audio.node.port.postMessage({ type: 'param', id, value });
}

/** UI の現在値をまとめて送る(起動直後の同期用)。 */
function pushAllParams() {
    postParam(PARAM_SHIFT_CENTS_L, Number(el.shiftL.value));
    postParam(PARAM_SHIFT_CENTS_R, Number(el.shiftR.value));
    postParam(PARAM_DRY_WET, Number(el.dryWet.value));
    postParam(PARAM_CROSSFADE_MS, Number(el.xfade.value));
}

/**
 * シフト量を符号付きで書式化する。プラス側も設定できるので、
 * 符号を必ず添えて「下げているのか上げているのか」を一目で分かるようにする。
 * ちょうど 0 は「±0」(単なる 0 だと符号の欠落と紛らわしい)。
 * 記号は U+2212 MINUS SIGN。読み上げでも「マイナス」と読まれる。
 */
function formatCents(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) {
        return '--';
    }
    if (n === 0) {
        return '\u00b10';
    }
    return (n > 0 ? '+' : '\u2212') + Math.abs(n);
}

function updateOutputs() {
    // 単位はマークアップ側の「セント」が担うので、ここは数値だけを出す
    el.shiftLOut.textContent = formatCents(el.shiftL.value);
    el.shiftROut.textContent = formatCents(el.shiftR.value);
    el.dryWetOut.textContent = Number(el.dryWet.value).toFixed(2);
    el.xfadeOut.textContent = `${el.xfade.value} ms`;
}

/**
 * L/R 独立の ON/OFF と −/+ の刻みを画面へ反映する。表示の切り替えは CSS が
 * .hero[data-split] を見て行う。連動中は L 側のラベルから「L」を落とし、
 * 「シフト量」1 本として読み上げられるようにする。刻みは範囲切替で変わるので、
 * 読み上げラベルも実際の刻みに追随させる。
 */
function renderShiftLabels() {
    const split = el.split.checked;
    el.controls.dataset.split = split ? 'on' : 'off';
    const step = Math.abs(Number(el.stepLUp.dataset.delta)) || 1;
    const name = split ? 'シフト量 L' : 'シフト量';
    el.shiftL.setAttribute('aria-label', `${name}(セント)`);
    el.shiftR.setAttribute('aria-label', 'シフト量 R(セント)');
    el.stepLDown.setAttribute('aria-label', `${name}を ${step} セント下げる`);
    el.stepLUp.setAttribute('aria-label', `${name}を ${step} セント上げる`);
    el.stepRDown.setAttribute('aria-label', `シフト量 R を ${step} セント下げる`);
    el.stepRUp.setAttribute('aria-label', `シフト量 R を ${step} セント上げる`);
}

/**
 * シフト量スライダーの可動範囲を切り替える。
 * 現在値はクランプしない —— 範囲外にいるときは min/max のほうを現在値まで広げる
 * (せっかく合わせ込んだ値を、表示の都合で勝手に動かさないため)。
 * スライダの step は 1 のまま据え置く。step を 10 にすると min からの格子に
 * 吸着して -89 が -90 に化けてしまうため、粗い刻みは −/+ ボタン側だけで表現する。
 */
function applyShiftRange(limit) {
    ui.rangeCents = limit;
    const values = [Number(el.shiftL.value), Number(el.shiftR.value)];
    const min = Math.min(-limit, ...values);
    const max = Math.max(limit, ...values);
    for (const input of [el.shiftL, el.shiftR]) {
        const keep = input.value;
        input.min = String(min);
        input.max = String(max);
        input.value = keep;     // min/max 変更時のブラウザ側の丸めを打ち消す
    }
    const delta = limit >= COARSE_RANGE_CENTS ? COARSE_STEP_CENTS : 1;
    for (const button of el.shiftStepButtons) {
        const sign = Number(button.dataset.delta) < 0 ? -1 : 1;
        button.dataset.delta = String(sign * delta);
    }
    renderShiftLabels();
    updateOutputs();
}

/** 現在の窓長に一致するプリセットだけを選択状態にする(無ければ全て非選択)。 */
function renderPreset() {
    const current = Number(el.xfade.value);
    for (const radio of el.presetRadios) {
        radio.checked = Number(radio.value) === current;
    }
}

/* -------------------- Worklet メッセージ受信 -------------------- */

function handleWorkletMessage(data) {
    if (!data || typeof data !== 'object') {
        return;
    }
    if (data.type === 'latency' && Number.isFinite(data.dspLatencyMs)) {
        ui.latency.dspMs = data.dspLatencyMs;
        if (data.engine === 'wasm' || data.engine === 'js') {
            ui.engine = data.engine;
            renderEngine();
        }
        refreshContextLatency();
        renderLatency();
        return;
    }
    if (data.type === 'error') {
        // 起動後の異常(レンダ量子超過など)もここへ来る
        showError(['オーディオ処理でエラーが発生しました。', String(data.message || '')], false);
    }
}

/** WebAudio 側の遅延(出力・ブロック)を取り直す(WF-6)。 */
function refreshContextLatency() {
    const ctx = audio.context;
    if (!ctx) {
        return;
    }
    const base = Number.isFinite(ctx.baseLatency) ? ctx.baseLatency : 0;
    const hasOutputLatency = Number.isFinite(ctx.outputLatency);
    const output = hasOutputLatency ? ctx.outputLatency : 0;
    ui.latency.outputMs = (base + output) * 1000;
    ui.latency.blockMs = ((RENDER_QUANTUM * 2) / ctx.sampleRate) * 1000;
    // outputLatency 未実装(Safari 等)では baseLatency のみ → 概算表示
    ui.latency.approximate = !hasOutputLatency;
}

/* ------------------------- 起動(WF-1) ------------------------- */

/**
 * prism.wasm を取得する(WF-1 の WASM ロード)。
 *
 * AudioWorklet のグローバルスコープには fetch が無いため、バイト列の取得は
 * メインスレッドの責務。取得したものは processorOptions で Worklet へ渡す。
 * 失敗しても致命ではない —— Worklet が JS 実装へフォールバックする。
 * @returns {Promise<{bytes: ArrayBuffer|null, error: string}>}
 */
async function loadWasmBytes() {
    if (typeof fetch !== 'function') {
        return { bytes: null, error: 'fetch が使えません' };
    }
    try {
        const response = await fetch(WASM_URL, { cache: 'no-cache' });
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        const bytes = await response.arrayBuffer();
        if (bytes.byteLength === 0) {
            throw new Error('prism.wasm が空です');
        }
        return { bytes, error: '' };
    } catch (err) {
        // file:// で開いた場合・配信漏れの場合はここに来る(JS 実装で動き続ける)
        return { bytes: null, error: String((err && err.message) || err) };
    }
}

/**
 * 案内文を自分で持つ取得エラー。DOMException と区別して扱う。
 */
class InputSourceError extends Error {
    constructor(lines) {
        super(lines[0]);
        this.name = 'InputSourceError';
        this.lines = lines;
    }
}

/** マイク(既定)。生の外界音を通すため加工系は全て無効(FR-4.3)。 */
function acquireMicStream() {
    return navigator.mediaDevices.getUserMedia({
        audio: {
            echoCancellation: false,
            noiseSuppression: false,
            autoGainControl: false,
            // 低遅延ヒント(非対応ブラウザでは無視される)
            latency: 0
        },
        video: false
    });
}

/**
 * ブラウザのタブ音声。
 *
 * Chromium は `video: true` を指定しないと共有ダイアログを出さないため映像も要求するが、
 * 映像は一切使わないので取得直後に停止して MediaStream から外す。
 * 「タブの音声を共有」が未チェック、あるいは画面/ウィンドウを選んだ場合は音声トラックが
 * 付いてこないので、手順を案内して中止する。
 *
 * `suppressLocalAudioPlayback: true`(Chromium 109+)で、共有中は共有元タブの音を
 * ブラウザ側で止めてもらう。これで「原音 + 処理音」の二重聞こえがなくなり、処理音だけが
 * イヤホンに届く。非対応ブラウザでは無視されるだけなので、その場合は共有元タブを手動で
 * ミュートすれば同じ状態になる(タブのミュートはキャプチャには影響しない)。
 */
async function acquireTabStream() {
    if (!navigator.mediaDevices || typeof navigator.mediaDevices.getDisplayMedia !== 'function') {
        throw new InputSourceError([
            'このブラウザはタブ音声の取得(getDisplayMedia)に対応していません。',
            'Chrome / Edge / Vivaldi の最新安定版でお試しください。'
        ]);
    }

    const stream = await navigator.mediaDevices.getDisplayMedia({
        video: true,
        audio: {
            echoCancellation: false,
            noiseSuppression: false,
            autoGainControl: false,
            suppressLocalAudioPlayback: true
        },
        // 自分自身(このデモのタブ)を選ぶとフィードバックループになるので候補から外す
        selfBrowserSurface: 'exclude'
    });

    for (const track of stream.getVideoTracks()) {
        track.stop(); // 画面キャプチャのインジケータを最小限にする
        if (typeof stream.removeTrack === 'function') {
            stream.removeTrack(track);
        }
    }

    if (stream.getAudioTracks().length === 0) {
        for (const track of stream.getTracks()) {
            track.stop();
        }
        throw new InputSourceError(['タブの音声が共有されませんでした。'].concat(TAB_SHARE_HOWTO));
    }
    return stream;
}

/** 音声ファイル。decodeAudioData は AudioContext 生成後に行うのでここでは読むだけ。 */
async function readSelectedFile() {
    const file = el.audioFile.files && el.audioFile.files[0];
    if (!file) {
        throw new InputSourceError([
            '音声ファイルが選ばれていません。',
            '「ファイル」から音声ファイルを選んでから開始してください。'
        ]);
    }
    try {
        return await file.arrayBuffer();
    } catch (err) {
        throw new InputSourceError([
            '音声ファイルを読み込めませんでした。',
            String((err && err.message) || err)
        ]);
    }
}

async function start() {
    clearError();

    ui.source = selectedSource();

    let stream = null;
    let fileBytes = null;
    try {
        if (ui.source === Source.TAB) {
            stream = await acquireTabStream();
        } else if (ui.source === Source.FILE) {
            fileBytes = await readSelectedFile();
        } else {
            stream = await acquireMicStream();
        }
    } catch (err) {
        handleAcquireError(err);
        return;
    }

    // 以降で失敗しても teardown() が確実にトラックを止められるよう先に保持する
    audio.stream = stream;

    try {
        const ctx = new AudioContext({ latencyHint: 'interactive' });
        audio.context = ctx;

        // WASM の取得と Worklet モジュールのロードは独立なので並行させる
        const [wasm] = await Promise.all([loadWasmBytes(), ctx.audioWorklet.addModule(WORKLET_URL)]);

        const node = new AudioWorkletNode(ctx, PROCESSOR_NAME, {
            numberOfInputs: 1,
            numberOfOutputs: 1,
            outputChannelCount: [2],
            processorOptions: {
                maxBlockFrames: RENDER_QUANTUM,
                // ArrayBuffer は構造化複製で Worklet へ渡る(転送はしない)
                wasmBytes: wasm.bytes
            }
        });
        audio.node = node;

        const ready = await waitForReady(node);
        ui.engine = ready.engine === 'wasm' ? 'wasm' : 'js';
        ui.engineNote = ui.engine === 'js' ? ready.fallbackReason || wasm.error : '';
        renderEngine();

        node.port.onmessage = (event) => handleWorkletMessage(event.data);
        node.onprocessorerror = () => {
            // 起動後に processor が死んだら黙って無音にせず、必ず知らせる
            stop().then(() => {
                showError([
                    'オーディオ処理が停止しました(AudioWorkletProcessor エラー)。',
                    'もう一度「▶ 開始」を押してください。'
                ]);
            });
        };

        const source = stream
            ? ctx.createMediaStreamSource(stream)
            : await createFileSource(ctx, fileBytes);
        audio.source = source;
        source.connect(node);
        node.connect(ctx.destination);

        if (ctx.state === 'suspended') {
            await ctx.resume();
        }

        // AudioBufferSourceNode のときだけ再生を始める(MediaStream 側は常時流れている)
        if (typeof source.start === 'function') {
            source.start();
        }

        if (ui.source === Source.TAB) {
            watchShareEnd(stream);
        }

        pushAllParams();
        refreshContextLatency();

        ui.state = State.RUNNING;
        render();
    } catch (err) {
        await teardown();
        ui.state = State.STOPPED;
        render();
        showError([
            'オーディオの初期化に失敗しました。',
            String((err && err.message) || err),
            'ページを再読み込みしてから、もう一度お試しください。'
        ]);
    }
}

/**
 * Worklet の ready / error を待つ(契約 3)。
 * ready は `{engine, fallbackReason}` を伴う(契約 3 への追加フィールド)。
 * error または時間切れは reject し、呼び出し側で後始末する。
 * @returns {Promise<{engine: string, fallbackReason: string}>}
 */
function waitForReady(node) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            node.port.onmessage = null;
            reject(new Error('Worklet が応答しません(タイムアウト)'));
        }, READY_TIMEOUT_MS);

        node.port.onmessage = (event) => {
            const data = event.data;
            if (!data || typeof data !== 'object') {
                return;
            }
            if (data.type === 'ready') {
                clearTimeout(timer);
                node.port.onmessage = null;
                resolve({
                    engine: typeof data.engine === 'string' ? data.engine : 'js',
                    fallbackReason:
                        typeof data.fallbackReason === 'string' ? data.fallbackReason : ''
                });
            } else if (data.type === 'error') {
                clearTimeout(timer);
                node.port.onmessage = null;
                reject(new Error(String(data.message || 'Worklet の初期化に失敗しました')));
            }
        };

        node.onprocessorerror = () => {
            clearTimeout(timer);
            reject(new Error('AudioWorkletProcessor が異常終了しました'));
        };
    });
}

/**
 * 音声ファイルをデコードして、ループ再生するソースノードを作る。
 * decodeAudioData は ArrayBuffer を切り離すため、再開時は毎回読み直す。
 */
async function createFileSource(ctx, fileBytes) {
    let buffer;
    try {
        buffer = await ctx.decodeAudioData(fileBytes);
    } catch (err) {
        throw new Error('音声ファイルをデコードできませんでした(未対応の形式かもしれません)');
    }
    const source = ctx.createBufferSource();
    source.buffer = buffer;
    source.loop = true;
    return source;
}

/**
 * 共有元タブでの「共有を停止」を検知して後始末する。
 * `track.stop()`(こちらからの停止)では ended は発火しないので、
 * ここに来るのはユーザーが共有を終了したときだけ。
 */
function watchShareEnd(stream) {
    for (const track of stream.getAudioTracks()) {
        track.addEventListener('ended', async () => {
            if (ui.state !== State.RUNNING) {
                return;
            }
            await stop();
            showError(['タブ音声の共有が終了したため、処理を停止しました。']);
        });
    }
}

/** 取得段のエラーをソース別に案内へ落とす。 */
function handleAcquireError(err) {
    if (err instanceof InputSourceError) {
        ui.state = State.STOPPED;
        render();
        showError(err.lines);
        return;
    }
    if (ui.source === Source.TAB) {
        handleGetDisplayMediaError(err);
        return;
    }
    if (ui.source === Source.FILE) {
        ui.state = State.STOPPED;
        render();
        showError(['音声ファイルを読み込めませんでした。', String((err && err.message) || err)]);
        return;
    }
    handleGetUserMediaError(err);
}

function handleGetDisplayMediaError(err) {
    const name = (err && err.name) || '';
    ui.state = State.STOPPED;
    render();
    if (name === 'NotAllowedError' || name === 'PermissionDeniedError') {
        // 共有ダイアログのキャンセルもここに来る(拒否と区別できない)
        showError(['タブ音声の共有がキャンセル、または拒否されました。'].concat(TAB_SHARE_HOWTO));
        return;
    }
    if (name === 'NotFoundError' || name === 'NotSupportedError' || name === 'TypeError') {
        showError([
            'このブラウザ・OS ではタブ音声を取得できません。',
            'macOS では画面全体・ウィンドウの音声は取得できず、タブ音声のみ対応しています。'
        ]);
        return;
    }
    if (name === 'NotReadableError') {
        showError(['共有元の取得に失敗しました。対象のタブを開き直してから再試行してください。']);
        return;
    }
    showError(['タブ音声を取得できませんでした。', String((err && err.message) || err)]);
}

function handleGetUserMediaError(err) {
    const name = (err && err.name) || '';
    if (name === 'NotAllowedError' || name === 'PermissionDeniedError' || name === 'SecurityError') {
        ui.state = State.DENIED;
        render();
        showError([
            'ブラウザのサイト設定でマイクを許可してから、再試行を押してください。',
            'localhost 以外(LAN の IP など)では、HTTPS でないとマイクを使えません。' +
                ' README の「Pixel から開く」を参照してください。'
        ]);
        return;
    }
    ui.state = State.STOPPED;
    render();
    if (name === 'NotFoundError' || name === 'DevicesNotFoundError') {
        showError(['マイクが見つかりません。入力デバイスを接続してから再試行してください。']);
        return;
    }
    if (name === 'NotReadableError') {
        showError(['マイクを他のアプリが使用中です。そちらを閉じてから再試行してください。']);
        return;
    }
    showError(['マイクを取得できませんでした。', String((err && err.message) || err)]);
}

/* ------------------------- 停止(WF-2) ------------------------- */

async function teardown() {
    if (audio.node) {
        audio.node.port.onmessage = null;
        try {
            audio.node.disconnect();
        } catch (err) {
            // 既に切断済み。停止処理は続行する
        }
    }
    if (audio.source) {
        if (typeof audio.source.stop === 'function') {
            try {
                audio.source.stop(); // AudioBufferSourceNode(ファイル再生)を止める
            } catch (err) {
                // 未開始・停止済み。停止処理は続行する
            }
        }
        try {
            audio.source.disconnect();
        } catch (err) {
            // 同上
        }
    }
    if (audio.stream) {
        for (const track of audio.stream.getTracks()) {
            track.stop(); // マイクインジケータを確実に消す(SR-3.1)
        }
    }
    if (audio.context && audio.context.state !== 'closed') {
        try {
            await audio.context.close();
        } catch (err) {
            // クローズ済み扱いで続行
        }
    }
    audio.node = null;
    audio.source = null;
    audio.stream = null;
    audio.context = null;
}

async function stop() {
    await teardown();
    ui.state = State.STOPPED;
    clearError();
    clearLatency();
    render();
}

/* ---------------------------- イベント ---------------------------- */

el.toggle.addEventListener('click', async () => {
    if (ui.state === State.RUNNING) {
        await stop();
        return;
    }
    if (ui.state === State.STOPPED || ui.state === State.DENIED) {
        el.toggle.disabled = true;
        try {
            await start();
        } finally {
            el.toggle.disabled = ui.state === State.UNSUPPORTED;
        }
    }
});

for (const radio of el.sourceRadios) {
    radio.addEventListener('change', () => {
        if (!radio.checked) {
            return;
        }
        ui.source = radio.value;
        clearError();
        renderSource();
    });
}

el.audioFile.addEventListener('change', () => {
    clearError();
});

el.shiftL.addEventListener('input', () => {
    const value = Number(el.shiftL.value);
    if (ui.linkLR) {
        el.shiftR.value = el.shiftL.value;
        postParam(PARAM_SHIFT_CENTS_L, value);
        postParam(PARAM_SHIFT_CENTS_R, value);
    } else {
        postParam(PARAM_SHIFT_CENTS_L, value);
    }
    updateOutputs();
});

el.shiftR.addEventListener('input', () => {
    const value = Number(el.shiftR.value);
    if (ui.linkLR) {
        el.shiftL.value = el.shiftR.value;
        postParam(PARAM_SHIFT_CENTS_L, value);
        postParam(PARAM_SHIFT_CENTS_R, value);
    } else {
        postParam(PARAM_SHIFT_CENTS_R, value);
    }
    updateOutputs();
});

el.split.addEventListener('change', () => {
    // スイッチは「別々に設定」。ON = 独立、OFF = 連動。
    ui.linkLR = !el.split.checked;
    renderShiftLabels();
    if (ui.linkLR) {
        // 連動へ戻した瞬間に R を L の値へ揃える
        el.shiftR.value = el.shiftL.value;
        postParam(PARAM_SHIFT_CENTS_R, Number(el.shiftR.value));
        updateOutputs();
    }
});

el.dryWet.addEventListener('input', () => {
    postParam(PARAM_DRY_WET, Number(el.dryWet.value));
    updateOutputs();
});

el.xfade.addEventListener('input', () => {
    postParam(PARAM_CROSSFADE_MS, Number(el.xfade.value));
    updateOutputs();
    renderPreset();     // 手で動かしてプリセット値から外れたら非選択に戻す
});

/* --------------------- プリセット / 範囲切替 --------------------- */

for (const radio of el.presetRadios) {
    radio.addEventListener('change', () => {
        if (!radio.checked) {
            return;
        }
        const value = Number(radio.value);
        if (!PRESET_XFADE_MS.includes(value)) {
            console.error('prism: 未知のプリセット値です', radio.value);
            return;
        }
        // 窓長だけを動かす。dry_wet は 1.0 のまま触らない。
        el.xfade.value = String(value);
        postParam(PARAM_CROSSFADE_MS, value);
        updateOutputs();
    });
}

for (const radio of el.rangeRadios) {
    radio.addEventListener('change', () => {
        if (!radio.checked) {
            return;
        }
        const limit = Number(radio.value);
        if (!Number.isFinite(limit) || limit <= 0) {
            console.error('prism: 範囲切替の値が不正です', radio.value);
            return;
        }
        applyShiftRange(limit);
    });
}

/** step 属性の小数桁数。0.05 刻みの加算で浮動小数の端数が出るのを丸めるために使う。 */
function decimalsOf(step) {
    const text = String(step);
    const dot = text.indexOf('.');
    return dot < 0 ? 0 : text.length - dot - 1;
}

/**
 * スライダの値を delta だけ動かす。値の反映は input イベントを合成して
 * 既存の更新経路(input リスナ → postParam → Worklet)へ流す。
 * ここで postParam を直接呼ばないのは、連動 L/R の扱いを二重に持たないため。
 */
function nudge(input, delta) {
    const min = Number(input.min);
    const max = Number(input.max);
    const step = Number(input.step) || 1;
    const before = Number(input.value);
    let next = Math.round((before + delta) / step) * step;
    next = Math.min(max, Math.max(min, next));
    const text = next.toFixed(decimalsOf(input.step || '1'));
    if (Number(text) === before) {
        return; // 端に達している。無駄な postMessage を出さない
    }
    input.value = text;
    input.dispatchEvent(new Event('input', { bubbles: true }));
}

/** 長押しの判定時間と、連続変化の間隔。 */
const HOLD_DELAY_MS = 400;
const HOLD_INTERVAL_MS = 150;

/**
 * − / + ボタンを 1 個ぶん結線する。
 * タップ/クリックで 1 ステップ、押しっぱなしで 150ms ごとに連続変化する。
 * ポインタ操作は pointerdown で処理し、続く click は握りつぶす
 * (キーボードの Enter / Space は pointerdown を伴わないので click 側が拾う)。
 */
function bindStepButton(button) {
    const input = document.getElementById(button.dataset.target);
    if (!input || !Number.isFinite(Number(button.dataset.delta))) {
        // マークアップ側の指定漏れ。黙って無視せず開発者に見えるようにする
        console.error('prism: 微調整ボタンの data-target / data-delta が不正です', button);
        return;
    }
    // 範囲切替で data-delta が書き換わるので、束縛時ではなく押すたびに読む。
    const delta = () => Number(button.dataset.delta);

    let holdTimer = null;
    let repeatTimer = null;
    let fromPointer = false;

    function stopRepeat() {
        if (holdTimer !== null) {
            clearTimeout(holdTimer);
            holdTimer = null;
        }
        if (repeatTimer !== null) {
            clearInterval(repeatTimer);
            repeatTimer = null;
        }
    }

    button.addEventListener('pointerdown', (event) => {
        if (button.disabled || (event.pointerType === 'mouse' && event.button !== 0)) {
            return;
        }
        fromPointer = true;
        stopRepeat();
        nudge(input, delta());
        // 指を離す前にボタン外へ動いても pointerup を受け取れるようにする
        if (typeof button.setPointerCapture === 'function') {
            try {
                button.setPointerCapture(event.pointerId);
            } catch {
                // 捕捉できない環境(古い実装)では pointercancel / pointerleave で止める
            }
        }
        holdTimer = setTimeout(() => {
            holdTimer = null;
            repeatTimer = setInterval(() => {
                if (button.disabled) {
                    stopRepeat();
                    return;
                }
                nudge(input, delta());
            }, HOLD_INTERVAL_MS);
        }, HOLD_DELAY_MS);
    });

    for (const type of ['pointerup', 'pointercancel', 'pointerleave']) {
        button.addEventListener(type, stopRepeat);
    }
    window.addEventListener('blur', stopRepeat);

    button.addEventListener('click', () => {
        if (fromPointer) {
            fromPointer = false;    // pointerdown で処理済み
            return;
        }
        if (button.disabled) {
            return;
        }
        nudge(input, delta());        // キーボード操作
    });
}

for (const button of el.stepButtons) {
    bindStepButton(button);
}

/* ------------------ モーダル(QR / 説明シート) ------------------ */

/**
 * モーダルを開く。背後は inert で操作不能にし、スクロールも止める。
 * inert 非対応のブラウザ向けに、下の keydown で Tab も閉じ込める。
 */
function openOverlay(overlay) {
    if (ui.overlay === overlay) {
        return;
    }
    if (ui.overlay) {
        closeOverlay();
    }
    ui.lastFocus = document.activeElement;
    overlay.hidden = false;
    document.body.classList.add('is-locked');
    for (const node of [el.hdr, el.appMain, el.dock]) {
        node.inert = true;
    }
    ui.overlay = overlay;
    const close = overlay.querySelector('.overlay-close');
    if (close) {
        close.focus();
    }
}

/** モーダルを閉じ、開く前のフォーカス位置へ必ず戻す(a11y チェックリスト 6)。 */
function closeOverlay() {
    const overlay = ui.overlay;
    if (!overlay) {
        return;
    }
    overlay.hidden = true;
    ui.overlay = null;
    document.body.classList.remove('is-locked');
    for (const node of [el.hdr, el.appMain, el.dock]) {
        node.inert = false;
    }
    const back = ui.lastFocus;
    ui.lastFocus = null;
    if (back && typeof back.focus === 'function' && document.contains(back)) {
        back.focus();
    }
}

document.addEventListener('keydown', (event) => {
    if (!ui.overlay) {
        return;
    }
    if (event.key === 'Escape') {
        event.preventDefault();
        closeOverlay();
        return;
    }
    if (event.key !== 'Tab') {
        return;
    }
    const panel = ui.overlay.querySelector('.overlay-panel');
    const items = Array.from(panel.querySelectorAll(FOCUSABLE)).filter((node) => !node.disabled);
    if (items.length === 0) {
        return;
    }
    const first = items[0];
    const last = items[items.length - 1];
    if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
    }
});

for (const scrim of el.scrims) {
    scrim.addEventListener('click', closeOverlay);
}
el.infoClose.addEventListener('click', closeOverlay);
el.qrClose.addEventListener('click', closeOverlay);

/* ------------------------ 説明のボトムシート ------------------------ */

/**
 * data-info のキーに対応する説明を #infoTexts から複製して開く。
 * 文面は index.html の <template> にまとめてあり、ここでは組み立てない。
 */
function showInfo(key) {
    const source = el.infoTexts.content.querySelector(`[data-key="${key}"]`);
    if (!source) {
        // マークアップ側の指定漏れ。無言で開かないよりは開発者に見せる
        console.error('prism: 説明文が見つかりません', key);
        return;
    }
    el.infoTitle.textContent = source.dataset.title || '説明';
    el.infoBody.replaceChildren(...Array.from(source.cloneNode(true).children));
    openOverlay(el.infoSheet);
}

for (const button of el.infoButtons) {
    button.addEventListener('click', () => {
        showInfo(button.dataset.info);
    });
}

/* --------------------------- QR コード --------------------------- */

/** いま開いている画面の URL。ハッシュとクエリは落として素の場所だけを渡す。 */
function shareUrl() {
    const url = new URL(window.location.href);
    url.hash = '';
    url.search = '';
    return url.href;
}

/**
 * QR コードの SVG を組み立てる。エンコードは web/qr.js(自前実装・外部依存なし)。
 * 白の下地を SVG 内に描くのは、読み取り機が明暗の比を見るため。
 * ゆとり(quiet zone)4 モジュールは規格の要求。
 */
function buildQrSvg(text) {
    const qr = encodeQr(text);          // バイトモード / 誤り訂正 M / 版とマスクは自動
    const border = 4;
    const span = qr.size + border * 2;
    const NS = 'http://www.w3.org/2000/svg';

    const svg = document.createElementNS(NS, 'svg');
    svg.setAttribute('xmlns', NS);
    svg.setAttribute('viewBox', `0 0 ${span} ${span}`);
    svg.setAttribute('shape-rendering', 'crispEdges');
    svg.setAttribute('role', 'img');
    svg.setAttribute('aria-label', `${text} を開く QR コード`);

    const background = document.createElementNS(NS, 'rect');
    background.setAttribute('width', String(span));
    background.setAttribute('height', String(span));
    background.setAttribute('fill', '#ffffff');

    const path = document.createElementNS(NS, 'path');
    path.setAttribute('d', toSvgPathData(qr.modules, border));
    path.setAttribute('fill', '#000000');

    svg.append(background, path);
    return svg;
}

function openQr() {
    const text = shareUrl();
    el.qrUrl.textContent = text;
    try {
        el.qrFrame.replaceChildren(buildQrSvg(text));
        el.qrNote.hidden = true;
        el.qrNote.textContent = '';
    } catch (err) {
        // URL が長すぎる等。QR を出せなくても URL 文字列は読めるようにしておく。
        el.qrFrame.replaceChildren();
        el.qrNote.textContent = `QR コードを作れませんでした: ${err.message}`;
        el.qrNote.hidden = false;
        console.error('prism: QR コードの生成に失敗しました', err);
    }
    openOverlay(el.qrModal);
}

el.qrBtn.addEventListener('click', openQr);

window.addEventListener('pagehide', () => {
    // タブを閉じる/離れるときにマイクを確実に解放する
    teardown();
});

/* ------------------------------ 起動 ------------------------------ */

/**
 * タブ音声(getDisplayMedia)の可否を判定する。Android/iOS のブラウザは非対応なので、
 * 選択肢を無効化して理由を短く出す。判定は起動時の 1 回だけで足りる。
 */
function detectTabSupport() {
    ui.tabSupported = Boolean(
        navigator.mediaDevices && typeof navigator.mediaDevices.getDisplayMedia === 'function'
    );
    if (ui.tabSupported) {
        return;
    }
    el.srcTab.disabled = true;
    el.tabNote.textContent = 'この端末では「タブ音声」を使えません(タブ音声の共有に非対応)。';
    el.tabNote.hidden = false;
    if (el.srcTab.checked) {
        // 復元(bfcache 等)でタブが選ばれていた場合はマイクへ戻す
        el.srcMic.checked = true;
    }
}

function init() {
    applyShiftRange(DEFAULT_RANGE_CENTS);   // updateOutputs / renderShiftLabels も呼ばれる
    renderPreset();
    detectTabSupport();
    ui.source = selectedSource();
    renderSource();
    const missing = detectSupport();
    if (missing.length > 0) {
        ui.state = State.UNSUPPORTED;
        render();
        showError(
            [
                `このブラウザは次の機能に対応していません: ${missing.join(' / ')}`,
                '対応ブラウザ: Chrome 最新安定版 / Safari 17 以降。',
                'また、localhost 以外では HTTPS でないとマイクを利用できません。'
            ],
            false
        );
        return;
    }
    render();
    el.toggle.focus(); // 初期フォーカス(a11y チェックリスト 2)
}

init();
