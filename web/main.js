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

/** レンダ量子。遅延内訳の「ブロック」計算に使う。 */
const RENDER_QUANTUM = 128;

/** Worklet の ready / error を待つ上限。無反応で固まらせない。 */
const READY_TIMEOUT_MS = 8000;

const WORKLET_URL = './prism-worklet.js';
const PROCESSOR_NAME = 'prism-processor';

/* ---------------------------- DOM 参照 ---------------------------- */

const el = {
    status: document.getElementById('status'),
    toggle: document.getElementById('toggle'),
    controls: document.getElementById('controls'),
    shiftL: document.getElementById('shiftL'),
    shiftR: document.getElementById('shiftR'),
    shiftLOut: document.getElementById('shiftLOut'),
    shiftROut: document.getElementById('shiftROut'),
    link: document.getElementById('link'),
    dryWet: document.getElementById('dryWet'),
    dryWetOut: document.getElementById('dryWetOut'),
    xfade: document.getElementById('xfade'),
    xfadeOut: document.getElementById('xfadeOut'),
    latTotal: document.getElementById('latTotal'),
    latOut: document.getElementById('latOut'),
    latBlock: document.getElementById('latBlock'),
    latDsp: document.getElementById('latDsp'),
    error: document.getElementById('error')
};

/* ---------------------------- UIState ---------------------------- */

const ui = {
    state: State.STOPPED,
    linkLR: el.link.checked,
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
    [State.STOPPED]: '状態: 停止中',
    [State.RUNNING]: '状態: 動作中',
    [State.DENIED]: '状態: マイクが許可されていません',
    [State.UNSUPPORTED]: '状態: 非対応ブラウザ'
};

const TOGGLE_TEXT = {
    [State.STOPPED]: '▶ 開始',
    [State.RUNNING]: '■ 停止',
    [State.DENIED]: '▶ 再試行',
    [State.UNSUPPORTED]: '▶ 開始'
};

function render() {
    el.status.textContent = STATUS_TEXT[ui.state];
    el.toggle.textContent = TOGGLE_TEXT[ui.state];
    el.toggle.disabled = ui.state === State.UNSUPPORTED;

    const controlsEnabled = ui.state === State.RUNNING;
    el.controls.setAttribute('aria-disabled', String(!controlsEnabled));
    for (const input of [el.shiftL, el.shiftR, el.link, el.dryWet, el.xfade]) {
        input.disabled = !controlsEnabled;
    }
    renderLatency();
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
    renderLatency();
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
    // 現行実装は WASM を使わないが、将来の WASM ビルドへの差し替えを
    // 前提に functional-spec WF-4 どおり検出しておく。
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
    el.shiftLOut.textContent = `${el.shiftL.value} cent`;
    el.shiftROut.textContent = `${el.shiftR.value} cent`;
    el.dryWetOut.textContent = Number(el.dryWet.value).toFixed(2);
    el.xfadeOut.textContent = `${el.xfade.value} ms`;
}

/* -------------------- Worklet メッセージ受信 -------------------- */

function handleWorkletMessage(data) {
    if (!data || typeof data !== 'object') {
        return;
    }
    if (data.type === 'latency' && Number.isFinite(data.dspLatencyMs)) {
        ui.latency.dspMs = data.dspLatencyMs;
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

async function start() {
    clearError();

    let stream;
    try {
        stream = await navigator.mediaDevices.getUserMedia({
            audio: {
                // 生の外界音を通す。加工系は全て無効(FR-4.3)
                echoCancellation: false,
                noiseSuppression: false,
                autoGainControl: false,
                // 低遅延ヒント(非対応ブラウザでは無視される)
                latency: 0
            },
            video: false
        });
    } catch (err) {
        handleGetUserMediaError(err);
        return;
    }

    // 以降で失敗しても teardown() が確実にトラックを止められるよう先に保持する
    audio.stream = stream;

    try {
        const ctx = new AudioContext({ latencyHint: 'interactive' });
        audio.context = ctx;

        await ctx.audioWorklet.addModule(WORKLET_URL);

        const node = new AudioWorkletNode(ctx, PROCESSOR_NAME, {
            numberOfInputs: 1,
            numberOfOutputs: 1,
            outputChannelCount: [2],
            processorOptions: { maxBlockFrames: RENDER_QUANTUM }
        });
        audio.node = node;

        await waitForReady(node);

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

        const source = ctx.createMediaStreamSource(stream);
        audio.source = source;
        source.connect(node);
        node.connect(ctx.destination);

        if (ctx.state === 'suspended') {
            await ctx.resume();
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
 * error または時間切れは reject し、呼び出し側で後始末する。
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
                resolve();
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

el.link.addEventListener('change', () => {
    ui.linkLR = el.link.checked;
    if (ui.linkLR) {
        // 連動 ON にした瞬間に L の値へ揃える
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

window.addEventListener('pagehide', () => {
    // タブを閉じる/離れるときにマイクを確実に解放する
    teardown();
});

/* ------------------------------ 起動 ------------------------------ */

function init() {
    updateOutputs();
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
