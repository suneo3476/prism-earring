/*
 * main.js — PrismEarring Web デモの UI 層(DemoUI)
 *
 * 素の ES module。外部ライブラリ・CDN・ビルドツールは使わない(SR-2.1)。
 * 契約 3(postMessage)経由でのみ Worklet と会話し、DSP には直接触れない。
 *
 * 対応する設計: u3-web-demo functional-spec WF-1〜WF-6 / frontend-components.md /
 * refined-mockups(mockups.md, interaction-spec.md, accessibility-checklist.md)
 */

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
    // 連動時にラベルから L を落とす対象(下記 renderSplit を参照)
    stepLDown: document.getElementById('stepLDown'),
    stepLUp: document.getElementById('stepLUp'),
    // スライダ両脇の − / + ボタン。data-target / data-delta をマークアップから読む
    stepButtons: Array.from(document.querySelectorAll('.bigstep, .ministep'))
};

/* ---------------------------- UIState ---------------------------- */

const ui = {
    state: State.STOPPED,
    /** L/R 連動。既定は連動(「左右を別々に設定」スイッチが OFF)。 */
    linkLR: !el.split.checked,
    /** 現在選択中の入力ソース(Source のいずれか)。 */
    source: Source.MIC,
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
        '共有元タブをミュートすると、処理音だけが聞こえて聞き比べやすくなります。',
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

function updateOutputs() {
    // 単位はマークアップ側の「セント」が担うので、ここは数値だけを出す
    el.shiftLOut.textContent = el.shiftL.value;
    el.shiftROut.textContent = el.shiftR.value;
    el.dryWetOut.textContent = Number(el.dryWet.value).toFixed(2);
    el.xfadeOut.textContent = `${el.xfade.value} ms`;
}

/**
 * L/R 独立の ON/OFF を画面へ反映する。表示の切り替えは CSS が
 * .hero[data-split] を見て行う。連動中は L 側のラベルから「L」を落とし、
 * 「シフト量」1 本として読み上げられるようにする。
 */
function renderSplit() {
    const split = el.split.checked;
    el.controls.dataset.split = split ? 'on' : 'off';
    el.shiftL.setAttribute('aria-label', split ? 'シフト量 L(セント)' : 'シフト量(セント)');
    el.stepLDown.setAttribute(
        'aria-label',
        split ? 'シフト量 L を 1 セント下げる' : 'シフト量を 1 セント下げる'
    );
    el.stepLUp.setAttribute(
        'aria-label',
        split ? 'シフト量 L を 1 セント上げる' : 'シフト量を 1 セント上げる'
    );
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
            autoGainControl: false
        }
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
    renderSplit();
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
});

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
    const delta = Number(button.dataset.delta);
    if (!input || !Number.isFinite(delta)) {
        // マークアップ側の指定漏れ。黙って無視せず開発者に見えるようにする
        console.error('prism: 微調整ボタンの data-target / data-delta が不正です', button);
        return;
    }

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
        nudge(input, delta);
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
                nudge(input, delta);
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
        nudge(input, delta);        // キーボード操作
    });
}

for (const button of el.stepButtons) {
    bindStepButton(button);
}

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
    updateOutputs();
    renderSplit();
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
