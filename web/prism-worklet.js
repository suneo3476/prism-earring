/*
 * prism-worklet.js — PrismEarring Web デモの AudioWorklet 層
 *
 * 構成(team.md「レイヤ分離」に従う):
 *   PitchShifterJS  … DSP コア。プラットフォーム非依存。C++ 正本
 *                     dsp/include/prism/PitchShifter.h の直訳(アルゴリズム・
 *                     数式・定数・処理順を 1:1 で対応させてある)。将来 WASM
 *                     ビルド(契約 2 の extern "C" API)へ差し替えられるよう、
 *                     メソッド名を ps_* に 1:1 対応させてある。
 *   PrismProcessor  … 接着層。AudioWorkletProcessor と契約 3 の postMessage。
 *
 * 本ファイルは classic script として書く(import/export を使わない)。
 * AudioWorklet.addModule で読み込め、Node の vm でもそのまま評価できる。
 *
 * 重要: process() 経路ではヒープ確保・オブジェクト生成・ロック・I/O・例外を
 * 行わない(CLAUDE.md「リアルタイムオーディオの鉄則」/ BR1.5)。
 * 音声経路に FFT・位相ボコーダは一切使わない(CLAUDE.md 最重要制約)。
 *
 * ---- WASM 差し替え時の対応表(契約 2) ----
 *   ps_create      -> PitchShifterJS.create()
 *   ps_destroy     -> instance.destroy()
 *   ps_prepare     -> instance.prepare(sampleRate, maxBlockFrames)
 *   ps_io_ptr      -> instance.ioPtr(channel)
 *   ps_process     -> instance.process(numFrames)
 *   ps_set_param   -> instance.setParam(id, value)
 *   ps_latency_ms  -> instance.latencyMs()
 */

'use strict';

/* ------------------------------------------------------------------ *
 * 定数 — C++ 正本 prism::PitchShifter の public constexpr と同値
 * ------------------------------------------------------------------ */

/** パラメータ ID。契約 2 の ps_set_param と同一。 */
const PS_PARAM_SHIFT_CENTS_L = 0;
const PS_PARAM_SHIFT_CENTS_R = 1;
const PS_PARAM_DRY_WET = 2;
const PS_PARAM_CROSSFADE_MS = 3;

/** パラメータ定義域(BR1.2 / 契約 1)。 */
const SHIFT_CENTS_MIN = -150;
const SHIFT_CENTS_MAX = 0;
const SHIFT_CENTS_DEFAULT = -89;
const DRY_WET_MIN = 0;
const DRY_WET_MAX = 1;
const DRY_WET_DEFAULT = 1;
const CROSSFADE_MS_MIN = 10;
const CROSSFADE_MS_MAX = 100;
const CROSSFADE_MS_DEFAULT = 50;

/** prepare の妥当域(契約 1 / WF-1.1)。 */
const SAMPLE_RATE_MIN = 8000;
const SAMPLE_RATE_MAX = 192000;

/** 基準読み出しオフセット(サンプル)。線形補間ガード + 安全余裕。遅延式の定数項。 */
const BASE_OFFSET_SAMPLES = 8;

/**
 * 遅延スイープ幅(ms)。読み出しヘッドは baseOffset から baseOffset+sweep まで
 * 遅れを蓄積し、そこで波形同期跳躍で前方へ戻る。クロスフェード窓長とは独立。
 * 上限は NFR-1 の 10ms 予算(最大遅れ = 8 サンプル + 9.5ms = 9.68ms @48kHz)、
 * 下限は「正しくシフトできる最低周波数 ≈ 1/9.5ms ≈ 105Hz」。README の D-A 参照。
 */
const SWEEP_MS = 9.5;

/** per-sample 平滑の時定数(BR1.3 / D-03: 20ms)。 */
const SMOOTHING_TIME_CONSTANT_S = 0.020;

/** デノーマル対策(SR-3.3): 微小 DC 加算と到達スナップ閾値。 */
const DENORMAL_GUARD = 1e-20;
const SNAP_CENTS = 1e-4;
const SNAP_UNIT = 1e-6;

/** 波形同期跳躍(WSOLA 相当)の探索パラメータ(README の D-B)。 */
const CORRELATION_LENGTH = 512;
const JUMP_MIN_FRACTION = 0.35;

function clamp(v, lo, hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/** C++ roundToInt: floor(v + 0.5)。 */
function roundToInt(v) {
    return Math.floor(v + 0.5);
}

/** セント → 速度比。半音 = 100 セントの決め打ちを禁止(BR1.1)。 */
function centsToRatio(cents) {
    return Math.pow(2, cents / 1200);
}

/**
 * 折り返し。C++ wrapIndex と同じく ±1 周ぶんのみ補正する(呼び出し側で
 * 入力が [-cap, 2cap) に収まることを保証している)。
 */
function wrapIndex(i, cap) {
    let n = i;
    if (n >= cap) {
        n -= cap;
    }
    if (n < 0) {
        n += cap;
    }
    return n;
}

/**
 * LC-3 ParameterSmoother の 1 サンプル分(BR1.3 / SD-4.2)。
 * 到達スナップでデノーマル源を構造的に断ち、それ以外は微小 DC を加算する。
 */
function smoothTick(current, target, a, snapEps) {
    const diff = target - current;
    if (diff < snapEps && diff > -snapEps) {
        return target;
    }
    return a * current + (1 - a) * target + DENORMAL_GUARD;
}

/* ------------------------------------------------------------------ *
 * PitchShifterJS — ディレイライン型ピッチシフタ
 * ------------------------------------------------------------------ *
 *
 * C++ 正本 dsp/include/prism/PitchShifter.h の直訳。アルゴリズムの要点:
 *
 *  - リングバッファに入力を書き込み、読み出しヘッドを rate = 2^(cents/1200)
 *    倍速で走らせる(時間領域のリサンプリング、線形補間)。
 *  - rate <= 1 なので遅れ lag は毎サンプル (1-rate) ずつ単調増加する。
 *    lag が baseOffset + sweep に達したら、前方へ「波形同期跳躍」する。
 *  - 跳躍量は直近 512 サンプルの正規化相互相関が最大になる値を
 *    [0.35×最大, 最大] から選ぶ(WSOLA 相当。README の D-B)。これにより
 *    跳躍量が局所周期の整数倍に近づき、跳躍時の位相再同期が消える。
 *  - 跳躍したヘッドと旧ヘッドを定振幅(線形)クロスフェードで繋ぐ。2 本は
 *    波形同期して相関しているため、等パワーではなく定振幅が正しい(D-C)。
 *  - 設計値遅延 = baseOffset + sweep/2(遅れがスイープ区間を一様に走査する
 *    ためその平均。D-D)。クロスフェード窓長には依存しない。
 *  - クロスフェード位相・跳躍スケジュールは ch 独立(D-E)。
 */
class PitchShifterJS {
    /** ps_create 相当。失敗しない(確保は prepare で行う)。 */
    static create() {
        return new PitchShifterJS();
    }

    constructor() {
        this._prepared = false;
        this._fs = 0;
        this._maxBlockFrames = 0;
        this._capacity = 0;
        this._windowMaxSamples = 0;
        this._sweepSamples = 0;
        this._window = 2;
        this._writeIndex = 0;
        this._smoothCoeff = 0;

        // LC-4 RingBuffer(channel-major。prepare で確保、以後サイズ不変)
        this._data0 = null;
        this._data1 = null;

        // 契約 2 の共有 I/O 領域(in-place 処理)
        this._io0 = null;
        this._io1 = null;

        // LC-2 ParameterGateway。JS の worklet は単一スレッドなので atomic は
        // 不要だが、ブロック頭で 1 回だけ load する規律は C++ と同じにする。
        this._paramCentsL = SHIFT_CENTS_DEFAULT;
        this._paramCentsR = SHIFT_CENTS_DEFAULT;
        this._paramDryWet = DRY_WET_DEFAULT;
        this._paramCrossfadeMs = CROSSFADE_MS_DEFAULT;

        // LC-3 ParameterSmoother(target / current)
        this._centsLTarget = SHIFT_CENTS_DEFAULT;
        this._centsLCurrent = SHIFT_CENTS_DEFAULT;
        this._centsRTarget = SHIFT_CENTS_DEFAULT;
        this._centsRCurrent = SHIFT_CENTS_DEFAULT;
        this._dryWetTarget = DRY_WET_DEFAULT;
        this._dryWetCurrent = DRY_WET_DEFAULT;

        // LC-5 ReadHead x2 + LC-6 Crossfader を ch ごとに保持(C++ Voice[2])。
        // 型付き配列は構築時に確保する(process 内では触るだけ)。
        this._lag = new Float64Array(4);   // [ch*2 + head]
        this._active = new Int32Array(2);
        this._fadePos = new Int32Array(2); // -1 = クロスフェードなし
        this._fadeLen = new Int32Array(2);
        this._fadePos[0] = -1;
        this._fadePos[1] = -1;
    }

    /** ps_destroy 相当。JS では GC 任せだが参照を切って再利用を防ぐ。 */
    destroy() {
        this._prepared = false;
        this._data0 = null;
        this._data1 = null;
        this._io0 = null;
        this._io1 = null;
    }

    /**
     * ps_prepare 相当(WF-1)。ヒープ確保はここだけ。
     * @param {number} sampleRate 8000..192000
     * @param {number} maxBlockFrames >= 1
     * @returns {boolean} 成功で true
     */
    prepare(sampleRate, maxBlockFrames) {
        this._prepared = false;
        if (!Number.isFinite(sampleRate) || !Number.isFinite(maxBlockFrames)) {
            return false;
        }
        if (!(sampleRate >= SAMPLE_RATE_MIN) || !(sampleRate <= SAMPLE_RATE_MAX)) {
            return false;
        }
        const maxBlock = Math.floor(maxBlockFrames);
        if (maxBlock < 1) {
            return false;
        }

        this._fs = sampleRate;
        this._maxBlockFrames = maxBlock;

        this._windowMaxSamples = roundToInt((CROSSFADE_MS_MAX * sampleRate) / 1000);
        if (this._windowMaxSamples < 2) {
            this._windowMaxSamples = 2;
        }
        this._sweepSamples = roundToInt((SWEEP_MS * sampleRate) / 1000);
        if (this._sweepSamples < 4) {
            this._sweepSamples = 4;
        }
        // 容量: 最大遅れ + クロスフェード中の追い越し分 + 相関窓 + 最大ブロック長 + 補間余裕
        this._capacity =
            BASE_OFFSET_SAMPLES +
            this._sweepSamples +
            this._windowMaxSamples +
            CORRELATION_LENGTH +
            maxBlock +
            2;

        try {
            this._data0 = new Float32Array(this._capacity);
            this._data1 = new Float32Array(this._capacity);
            this._io0 = new Float32Array(maxBlock);
            this._io1 = new Float32Array(maxBlock);
        } catch (e) {
            // SD-3: 確保失敗は例外を漏らさず false へ変換する。
            this._data0 = null;
            this._data1 = null;
            this._io0 = null;
            this._io1 = null;
            return false;
        }

        this._smoothCoeff = Math.exp(-1 / (SMOOTHING_TIME_CONSTANT_S * sampleRate));

        this._prepared = true;
        this.reset();
        return true;
    }

    /** 状態クリア(WF-3)。確保済みバッファは保持する。 */
    reset() {
        if (!this._prepared) {
            return;
        }
        this._data0.fill(0);
        this._data1.fill(0);
        this._io0.fill(0);
        this._io1.fill(0);
        this._writeIndex = 0;

        // 平滑器は整定済み扱い(current = target = 現在のパラメータ値)
        this._centsLTarget = this._paramCentsL;
        this._centsLCurrent = this._paramCentsL;
        this._centsRTarget = this._paramCentsR;
        this._centsRCurrent = this._paramCentsR;
        this._dryWetTarget = this._paramDryWet;
        this._dryWetCurrent = this._paramDryWet;
        this._window = this._latchWindowSamples();

        // スイープ中央(= 設計値遅延)から開始する。
        const midLag = BASE_OFFSET_SAMPLES + 0.5 * this._sweepSamples;
        for (let ch = 0; ch < 2; ch++) {
            this._lag[ch * 2] = midLag;
            this._lag[ch * 2 + 1] = midLag;
            this._active[ch] = 0;
            this._fadePos[ch] = -1;
            this._fadeLen[ch] = 0;
        }
    }

    /**
     * ps_io_ptr 相当。共有 I/O 領域(in-place 処理)。
     * @param {number} channel 0 = L, 1 = R
     * @returns {Float32Array|null}
     */
    ioPtr(channel) {
        if (!this._prepared) {
            return null;
        }
        if (channel === 0) {
            return this._io0;
        }
        if (channel === 1) {
            return this._io1;
        }
        return null;
    }

    /**
     * ps_set_param 相当(LC-2 / BR1.2)。非有限値は無視、値はクランプ。
     * @param {number} id 0..3
     * @param {number} value
     */
    setParam(id, value) {
        if (!Number.isFinite(value)) {
            return; // SR-3.2: 非有限値は無視(clamp より前に検査する)
        }
        switch (id) {
            case PS_PARAM_SHIFT_CENTS_L:
                this._paramCentsL = clamp(value, SHIFT_CENTS_MIN, SHIFT_CENTS_MAX);
                return;
            case PS_PARAM_SHIFT_CENTS_R:
                this._paramCentsR = clamp(value, SHIFT_CENTS_MIN, SHIFT_CENTS_MAX);
                return;
            case PS_PARAM_DRY_WET:
                this._paramDryWet = clamp(value, DRY_WET_MIN, DRY_WET_MAX);
                return;
            case PS_PARAM_CROSSFADE_MS:
                this._paramCrossfadeMs = clamp(value, CROSSFADE_MS_MIN, CROSSFADE_MS_MAX);
                return;
            default:
                return; // 未知 id は無視(契約 2)
        }
    }

    /**
     * 設計値遅延(サンプル)。C++ getLatencySamples() と同一式。
     * 遅れはスイープ区間 [baseOffset, baseOffset+sweep] を一様に走査するため、
     * 設計値(平均遅れ)= baseOffset + sweep/2。窓長には依存しない(D-D)。
     */
    latencySamples() {
        if (!this._prepared) {
            return 0;
        }
        return BASE_OFFSET_SAMPLES + 0.5 * this._sweepSamples;
    }

    /** ps_latency_ms 相当。設計値遅延をミリ秒で返す。 */
    latencyMs() {
        if (!this._prepared) {
            return 0;
        }
        return (this.latencySamples() / this._fs) * 1000;
    }

    /** 現在ラッチ済みのクロスフェード窓長(サンプル)。C++ getWindowSamples()。 */
    windowSamples() {
        return this._window;
    }

    /** 遅延スイープ幅(サンプル)。最大遅れ = baseOffset + これ。C++ getSweepSamples()。 */
    sweepSamples() {
        return this._sweepSamples;
    }

    /** C++ isPrepared()。 */
    isPrepared() {
        return this._prepared;
    }

    /**
     * ps_process 相当(WF-2)。共有 I/O 領域を in-place 処理する。
     * 確保・オブジェクト生成・例外なし(BR1.5)。
     * @param {number} numFrames 1..maxBlockFrames(超過分はクランプ)
     */
    process(numFrames) {
        if (!(numFrames > 0)) {
            return;
        }
        if (!this._prepared) {
            return;
        }
        const frames = numFrames > this._maxBlockFrames ? this._maxBlockFrames : numFrames;

        // 1. ブロック頭でパラメータを各 1 回 load(FR-1.5 / D-03)
        this._centsLTarget = this._paramCentsL;
        this._centsRTarget = this._paramCentsR;
        this._dryWetTarget = this._paramDryWet;
        this._window = this._latchWindowSamples();

        const maxLag = BASE_OFFSET_SAMPLES + this._sweepSamples;
        const a = this._smoothCoeff;
        const cap = this._capacity;
        const d0 = this._data0;
        const d1 = this._data1;
        const io0 = this._io0;
        const io1 = this._io1;
        const lag = this._lag;
        const active = this._active;
        const fadePos = this._fadePos;
        const fadeLen = this._fadeLen;

        const targetCentsL = this._centsLTarget;
        const targetCentsR = this._centsRTarget;
        const targetDryWet = this._dryWetTarget;

        let cL = this._centsLCurrent;
        let cR = this._centsRCurrent;
        let mix = this._dryWetCurrent;
        let w = this._writeIndex;

        for (let i = 0; i < frames; i++) {
            // 2.1 平滑(BR1.3)
            cL = smoothTick(cL, targetCentsL, a, SNAP_CENTS);
            cR = smoothTick(cR, targetCentsR, a, SNAP_CENTS);
            mix = smoothTick(mix, targetDryWet, a, SNAP_UNIT);

            // 2.2 rate 更新(ch 独立、BR1.1)
            const rateL = centsToRatio(cL);
            const rateR = centsToRatio(cR);

            // 2.3 書き込み
            const xL = io0[i];
            const xR = io1[i];
            d0[w] = xL;
            d1[w] = xR;

            for (let ch = 0; ch < 2; ch++) {
                const data = ch === 0 ? d0 : d1;
                const x = ch === 0 ? xL : xR;
                const rate = ch === 0 ? rateL : rateR;
                const base = ch * 2;
                const act = active[ch];
                const drift = 1 - rate; // rate<=1 なので遅れは単調増加

                // 2.4 読み出し(線形補間、D-02)+ 2.5 クロスフェード合成(LC-6)
                let wet;
                if (fadePos[ch] < 0) {
                    wet = this._readAt(data, lag[base + act], w);
                } else {
                    const u = fadePos[ch] / fadeLen[ch];
                    const yOld = this._readAt(data, lag[base + (1 - act)], w);
                    const yNew = this._readAt(data, lag[base + act], w);
                    // 波形同期跳躍により 2 本のヘッドは相関しているため、定振幅
                    // (線形)クロスフェードを用いる(等パワーだと +3dB のこぶが出る)。
                    wet = (1 - u) * yOld + u * yNew;
                    fadePos[ch] = fadePos[ch] + 1;
                    if (fadePos[ch] >= fadeLen[ch]) {
                        fadePos[ch] = -1; // 旧ヘッドを解放
                    }
                }

                // 2.7 dry/wet ミックス(FR-1.3)
                const y = (1 - mix) * x + mix * wet;
                if (ch === 0) {
                    io0[i] = y;
                } else {
                    io1[i] = y;
                }

                // 遅れの前進(両ヘッド)
                lag[base] = lag[base] + drift;
                lag[base + 1] = lag[base + 1] + drift;

                // 2.6 スイープ端に達したら波形同期跳躍 + クロスフェード開始(WF-4 / BR1.4)
                if (fadePos[ch] < 0 && lag[base + active[ch]] >= maxLag) {
                    this._startJump(ch, data, w);
                }
            }

            // 2.8 writeIndex 前進(全 ch 共有、フレームごとに 1 回)
            w = w + 1 >= cap ? 0 : w + 1;
        }

        this._writeIndex = w;
        this._centsLCurrent = cL;
        this._centsRCurrent = cR;
        this._dryWetCurrent = mix;
    }

    /** ラッチする窓長(サンプル)。C++ latchWindowSamples()。 */
    _latchWindowSamples() {
        let n = roundToInt((this._paramCrossfadeMs * this._fs) / 1000);
        if (n < 2) {
            n = 2;
        }
        if (n > this._windowMaxSamples) {
            n = this._windowMaxSamples;
        }
        return n;
    }

    /** LC-5: 遅れ lag(サンプル、実数)の位置を線形補間で読む(D-02)。 */
    _readAt(data, lagSamples, writeIndex) {
        const cap = this._capacity;
        let pos = writeIndex - lagSamples;
        while (pos < 0) {
            pos += cap;
        }
        const fl = Math.floor(pos);
        let n = fl;
        if (n >= cap) {
            n -= cap;
        }
        const f = pos - fl;
        const va = data[n];
        const vb = data[wrapIndex(n + 1, cap)];
        return va + f * (vb - va);
    }

    /**
     * 波形同期跳躍(WSOLA 相当。C++ startJump と同一)。旧ヘッド直近
     * CORRELATION_LENGTH サンプルと最も相関する位置へ新ヘッドを置く。
     */
    _startJump(ch, data, writeIndex) {
        const lag = this._lag;
        const active = this._active;
        const cap = this._capacity;
        const base = ch * 2;
        const act = active[ch];
        const oldLag = lag[base + act];
        const jumpMax = oldLag - BASE_OFFSET_SAMPLES;
        if (jumpMax <= 1) {
            return;
        }
        let jMax = Math.trunc(jumpMax);
        let jMin = Math.trunc(jumpMax * JUMP_MIN_FRACTION);
        if (jMin < 1) {
            jMin = 1;
        }
        if (jMax <= jMin) {
            jMax = jMin;
        }

        const oldBase = Math.floor(writeIndex - oldLag + cap);
        let bestScore = -1.0e30;
        let bestJump = jMax;
        for (let j = jMax; j >= jMin; j--) {
            let dot = 0;
            let energy = 0;
            for (let k = 0; k < CORRELATION_LENGTH; k++) {
                const va = data[wrapIndex(oldBase - k, cap)];
                const vb = data[wrapIndex(oldBase - k + j, cap)];
                dot += va * vb;
                energy += vb * vb;
            }
            const score = dot / Math.sqrt(energy + 1.0e-12);
            if (score > bestScore) {
                bestScore = score;
                bestJump = j; // 同点なら大きい j(= 長い走行)を採る
            }
        }

        const next = 1 - act;
        lag[base + next] = oldLag - bestJump;
        active[ch] = next;
        let len = this._window;
        // クロスフェードは走行長を超えてはならない(跳躍間隔の 1/2 を上限にする)
        const runLimit = bestJump * 8;
        if (len > runLimit) {
            len = runLimit;
        }
        if (len < 2) {
            len = 2;
        }
        this._fadeLen[ch] = len;
        this._fadePos[ch] = 0;
    }
}

/* ------------------------------------------------------------------ *
 * PrismProcessor — 接着層(契約 3: postMessage プロトコル)
 * ------------------------------------------------------------------ */

if (typeof registerProcessor === 'function' && typeof AudioWorkletProcessor === 'function') {
    /** レンダ量子の既定値。Web Audio の仕様値。 */
    const DEFAULT_RENDER_QUANTUM = 128;

    class PrismProcessor extends AudioWorkletProcessor {
        constructor(options) {
            super();
            const opts = (options && options.processorOptions) || {};
            const maxBlock = Math.max(
                DEFAULT_RENDER_QUANTUM,
                Number.isFinite(opts.maxBlockFrames) ? Math.floor(opts.maxBlockFrames) : 0
            );

            this._shifter = PitchShifterJS.create();
            this._ready = this._shifter.prepare(sampleRate, maxBlock);
            this._io0 = this._ready ? this._shifter.ioPtr(0) : null;
            this._io1 = this._ready ? this._shifter.ioPtr(1) : null;

            // 1 秒ごとの遅延報告(D-04)。メッセージ本体は再利用して確保を抑える
            this._reportIntervalFrames = Math.max(1, Math.round(sampleRate));
            this._framesSinceReport = 0;
            this._latencyMessage = { type: 'latency', dspLatencyMs: 0 };
            this._overflowReported = false;

            this.port.onmessage = (event) => {
                this._handleMessage(event.data);
            };

            if (!this._ready) {
                // 初期化失敗は必ず UI へ上げる(契約 3 の error トリガ)
                this.port.postMessage({
                    type: 'error',
                    message:
                        'PitchShifter の初期化に失敗しました(sampleRate=' +
                        sampleRate +
                        ')。対応サンプリングレートは 8000〜192000 Hz です。'
                });
            } else {
                this.port.postMessage({ type: 'ready' });
                this._postLatency();
            }
        }

        /** UI → Worklet(契約 3)。未知の型・不正値は無視する。 */
        _handleMessage(data) {
            if (!data || typeof data !== 'object') {
                return;
            }
            if (data.type === 'param') {
                if (this._ready) {
                    this._shifter.setParam(data.id, data.value);
                }
                return;
            }
            if (data.type === 'reset') {
                if (this._ready) {
                    this._shifter.reset();
                }
                return;
            }
            if (data.type === 'requestLatency') {
                if (this._ready) {
                    this._postLatency();
                }
            }
        }

        _postLatency() {
            this._latencyMessage.dspLatencyMs = this._shifter.latencyMs();
            this.port.postMessage(this._latencyMessage);
        }

        process(inputs, outputs) {
            const output = outputs[0];
            if (!output || output.length === 0) {
                return true;
            }
            const outL = output[0];
            const outR = output.length > 1 ? output[1] : null;
            const n = outL.length;

            if (!this._ready) {
                outL.fill(0);
                if (outR) {
                    outR.fill(0);
                }
                return true;
            }

            const io0 = this._io0;
            const io1 = this._io1;
            if (n > io0.length) {
                // レンダ量子が想定を超えた: 無音を出し、一度だけ UI へ通知する
                outL.fill(0);
                if (outR) {
                    outR.fill(0);
                }
                if (!this._overflowReported) {
                    this._overflowReported = true;
                    this.port.postMessage({
                        type: 'error',
                        message:
                            'レンダ量子 ' + n + ' フレームが想定(' + io0.length + ')を超えました。'
                    });
                }
                return true;
            }

            const input = inputs[0];
            if (input && input.length > 0) {
                const inL = input[0];
                // モノラル入力の L=R 複製は呼び出し側(Worklet)の責務(D-06 / BR1.8)
                const inR = input.length > 1 ? input[1] : inL;
                for (let i = 0; i < n; i++) {
                    io0[i] = inL[i];
                    io1[i] = inR[i];
                }
            } else {
                for (let i = 0; i < n; i++) {
                    io0[i] = 0;
                    io1[i] = 0;
                }
            }

            this._shifter.process(n);

            for (let i = 0; i < n; i++) {
                outL[i] = io0[i];
            }
            if (outR) {
                for (let i = 0; i < n; i++) {
                    outR[i] = io1[i];
                }
            }

            this._framesSinceReport += n;
            if (this._framesSinceReport >= this._reportIntervalFrames) {
                this._framesSinceReport = 0;
                this._postLatency();
            }
            return true;
        }
    }

    registerProcessor('prism-processor', PrismProcessor);
}

// Node(オフラインロジックテスト)から classic script として評価したときの
// 公開面。AudioWorklet では未使用。
if (typeof globalThis !== 'undefined') {
    globalThis.PitchShifterJS = PitchShifterJS;
    globalThis.PS_PARAM = {
        SHIFT_CENTS_L: PS_PARAM_SHIFT_CENTS_L,
        SHIFT_CENTS_R: PS_PARAM_SHIFT_CENTS_R,
        DRY_WET: PS_PARAM_DRY_WET,
        CROSSFADE_MS: PS_PARAM_CROSSFADE_MS
    };
}
