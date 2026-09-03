/*
 * wasm-test.mjs — WASM 経路(契約 2)のオフライン数値検証
 *
 * 位置づけ: Web デモの **本番経路** は web/prism.wasm(C++ 正本 prism::PitchShifter を
 * Emscripten でビルドしたもの)であり、PitchShifterJS はフォールバックである。
 * 本ファイルはブラウザを起動せずに WASM 経路そのものを数値で検証する。
 * 判定式・閾値は verify/verify.cpp と web/test/pitch-shifter-test.mjs に揃えてある。
 * team.md「Testing Posture」の test-after に従い、実装後に書いて実行する。
 *
 * 実行: node web/test/wasm-test.mjs
 * 依存: Node 18+ のみ(外部パッケージなし)。事前に web/wasm/build.sh を通して
 *       web/prism.wasm を生成しておく(生成物はリポジトリにコミット済み)。
 *
 * ローダーは本番と同一のもの(prism-worklet.js の PrismWasmShifter)を使う。
 * テスト専用の別ローダーは書かない —— 検証したいのは本番経路そのものだから。
 *
 * 周波数推定は検証側の離散フーリエ変換(自前実装)で行う。
 * 音声経路には FFT・位相ボコーダを一切使っていない(CLAUDE.md 最重要制約)。
 *
 * C++ 正本の実測値(README.md「実測値」):
 *   遅延 228 サンプル @44.1k / 248 サンプル @48k = いずれも 5.17ms
 *   グリッチ 0 件
 * WASM 経路がこれと食い違う場合はブリッジかビルド構成の誤りであり、
 * 判定を緩めてはならない。
 */

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import vm from 'node:vm';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const WORKLET_PATH = path.join(HERE, '..', 'prism-worklet.js');
const WASM_PATH = path.join(HERE, '..', 'prism.wasm');

/* ---- prism-worklet.js を classic script として評価して両実装を取り出す ---- */
function loadWorklet() {
    const source = readFileSync(WORKLET_PATH, 'utf8');
    const context = vm.createContext({ console, WebAssembly, ArrayBuffer, Float32Array, Error });
    new vm.Script(source, { filename: 'prism-worklet.js' }).runInContext(context);
    for (const name of ['PrismWasmShifter', 'PitchShifterJS']) {
        if (typeof context[name] !== 'function') {
            throw new Error(`${name} を prism-worklet.js から取得できませんでした`);
        }
    }
    return {
        PrismWasmShifter: context.PrismWasmShifter,
        PitchShifterJS: context.PitchShifterJS,
        PS_PARAM: context.PS_PARAM
    };
}

const { PrismWasmShifter, PitchShifterJS, PS_PARAM } = loadWorklet();
const WASM_BYTES = readFileSync(WASM_PATH);

/* ---------------------------- 判定閾値 ---------------------------- */
/* verify.cpp / pitch-shifter-test.mjs の名前付き定数と 1:1(SR-3.1)。 */

const EXPECTED_RATIO = 418 / 440; // = 0.95(CLAUDE.md の実測値)
const PITCH_REL_TOLERANCE = 0.005; // BR2.1: ±0.5%
const LATENCY_BUDGET_MS = 10.0; // BR2.2 / NFR-1
const LATENCY_THRESHOLD = 0.05; // BR2.2: -26 dBFS
const LATENCY_SLACK_SAMPLES = 8.0; // BR2.2: 設計値一致許容差の定数項
const GLITCH_SLOPE_FACTOR = 3.0; // BR2.3: k = 3.0
const GLITCH_WARMUP_SEC = 0.25; // BR2.3: warmup 250ms 除外
const GLITCH_DURATION_SEC = 5.0;
const SIGNAL_AMPLITUDE = 0.5;
const BASE_OFFSET_SAMPLES = 8; // PitchShifter.h kBaseOffsetSamples
const SWEEP_MS = 9.5; // PitchShifter.h kSweepMs
const DEFAULT_CROSSFADE_MS = 50;
const SAMPLE_RATES = [44100, 48000];
const PITCH_FREQS = [110, 440, 3520];
const BLOCK = 128; // Web Audio のレンダ量子
/** C++ 正本 verify.cpp の実測遅延(サンプル)。README.md「実測値」。 */
const CPP_LATENCY_SAMPLES = { 44100: 228, 48000: 248 };

/* ---------------------------- テスト基盤 ---------------------------- */

let failures = 0;
let passes = 0;

function check(name, ok, detail) {
    if (ok) {
        passes++;
        console.log(`  PASS  ${name}${detail ? ' — ' + detail : ''}`);
    } else {
        failures++;
        console.log(`  FAIL  ${name}${detail ? ' — ' + detail : ''}`);
    }
}

function section(title) {
    console.log(`\n[${title}]`);
}

/* ------------------------- エンジン生成 ------------------------- */

/** 生の WASM インスタンス(契約 2 を C API として直接叩くテスト用)。 */
function rawInstance() {
    const module = new WebAssembly.Module(WASM_BYTES);
    const instance = new WebAssembly.Instance(module, {});
    if (typeof instance.exports._initialize === 'function') {
        instance.exports._initialize();
    }
    return instance.exports;
}

/** 'wasm' | 'js' のどちらかの実装を prepare 済みで返す。面は共通。 */
function makeEngine(kind, fs, { cents = -89, centsR = null, dryWet = 1, xfade = DEFAULT_CROSSFADE_MS } = {}) {
    const s = kind === 'wasm' ? PrismWasmShifter.instantiate(WASM_BYTES) : PitchShifterJS.create();
    if (!s.prepare(fs, BLOCK)) {
        throw new Error(`prepare が失敗しました (engine=${kind}, fs=${fs})`);
    }
    s.setParam(PS_PARAM.SHIFT_CENTS_L, cents);
    s.setParam(PS_PARAM.SHIFT_CENTS_R, centsR === null ? cents : centsR);
    s.setParam(PS_PARAM.DRY_WET, dryWet);
    s.setParam(PS_PARAM.CROSSFADE_MS, xfade);
    s.reset(); // 平滑器を整定済みにして測定条件を安定させる
    return s;
}

/* ------------------------- 信号処理ヘルパ ------------------------- */

/** 正弦波を通して出力を得る(共有 I/O 領域を 128 フレームずつ in-place 処理)。 */
function runSine(kind, { freq, fs, seconds, cents = -89, centsR = null, dryWet = 1, xfade = DEFAULT_CROSSFADE_MS }) {
    const s = makeEngine(kind, fs, { cents, centsR, dryWet, xfade });
    const total = Math.floor(fs * seconds);
    const left = new Float64Array(total);
    const right = new Float64Array(total);
    const io0 = s.ioPtr(0);
    const io1 = s.ioPtr(1);
    const omega = (2 * Math.PI * freq) / fs;

    for (let start = 0; start < total; start += BLOCK) {
        const n = Math.min(BLOCK, total - start);
        for (let i = 0; i < n; i++) {
            const v = SIGNAL_AMPLITUDE * Math.sin(omega * (start + i));
            io0[i] = v;
            io1[i] = v;
        }
        s.process(n);
        for (let i = 0; i < n; i++) {
            left[start + i] = io0[i];
            right[start + i] = io1[i];
        }
    }
    const result = {
        left,
        right,
        latencyMs: s.latencyMs(),
        latencySamples: s.latencySamples(),
        windowSamples: s.windowSamples(),
        sweepSamples: s.sweepSamples()
    };
    s.destroy();
    return result;
}

/** 単位インパルス(sample 0、振幅 1.0)を入れて出力を返す(verify.cpp testLatency)。 */
function runImpulse(kind, fs, seconds, xfade = DEFAULT_CROSSFADE_MS) {
    const s = makeEngine(kind, fs, { xfade });
    const total = Math.floor(fs * seconds);
    const out = new Float64Array(total);
    const io0 = s.ioPtr(0);
    const io1 = s.ioPtr(1);
    for (let start = 0; start < total; start += BLOCK) {
        const n = Math.min(BLOCK, total - start);
        for (let i = 0; i < n; i++) {
            const v = start + i === 0 ? 1 : 0;
            io0[i] = v;
            io1[i] = v;
        }
        s.process(n);
        for (let i = 0; i < n; i++) {
            out[start + i] = io0[i];
        }
    }
    const result = {
        out,
        designSamples: s.latencySamples(),
        windowSamples: s.windowSamples(),
        sweepSamples: s.sweepSamples()
    };
    s.destroy();
    return result;
}

/** Hann 窓つき任意周波数 DFT の振幅(自前実装。検証側のみ)。 */
function dftMagnitude(x, from, len, fs, freq) {
    const w = (2 * Math.PI * freq) / fs;
    let re = 0;
    let im = 0;
    for (let i = 0; i < len; i++) {
        const hann = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (len - 1));
        const v = x[from + i] * hann;
        const p = w * i;
        re += v * Math.cos(p);
        im -= v * Math.sin(p);
    }
    return Math.sqrt(re * re + im * im) / len;
}

/** 入力周波数に対する出力周波数比のピークを探索する(粗 → 微 + 放物線補間)。 */
function estimateRatio(x, fs, inputFreq, { from, len, lo = 0.8, hi = 1.2 }) {
    const coarseStep = 0.002;
    let bestRatio = lo;
    let bestMag = -1;
    for (let r = lo; r <= hi + 1e-12; r += coarseStep) {
        const m = dftMagnitude(x, from, len, fs, inputFreq * r);
        if (m > bestMag) {
            bestMag = m;
            bestRatio = r;
        }
    }
    const fineStep = 0.00002;
    let fineBest = bestRatio;
    let fineBestMag = -1;
    const mags = [];
    const ratios = [];
    for (let r = bestRatio - coarseStep; r <= bestRatio + coarseStep + 1e-12; r += fineStep) {
        const m = dftMagnitude(x, from, len, fs, inputFreq * r);
        ratios.push(r);
        mags.push(m);
        if (m > fineBestMag) {
            fineBestMag = m;
            fineBest = r;
        }
    }
    const k = mags.indexOf(fineBestMag);
    if (k > 0 && k < mags.length - 1) {
        const denom = mags[k - 1] - 2 * fineBestMag + mags[k + 1];
        if (denom !== 0) {
            const delta = (0.5 * (mags[k - 1] - mags[k + 1])) / denom;
            if (Math.abs(delta) <= 1) {
                return { ratio: ratios[k] + delta * fineStep, magnitude: fineBestMag };
            }
        }
    }
    return { ratio: fineBest, magnitude: fineBestMag };
}

/** 定常部の比を測る(起動過渡 0.5 秒を除外し 1.4 秒ぶんを解析)。 */
function steadyRatio(x, fs, freq, bounds) {
    return estimateRatio(x, fs, freq, {
        from: Math.floor(fs * 0.5),
        len: Math.floor(fs * 1.4),
        ...bounds
    });
}

function centsError(ratio) {
    return 1200 * Math.log2(ratio / EXPECTED_RATIO);
}

function rms(x, from, to) {
    let s = 0;
    for (let i = from; i < to; i++) {
        s += x[i] * x[i];
    }
    return Math.sqrt(s / (to - from));
}

/** インパルス応答が閾値を超える最初のサンプル(-1 = 見つからない)。 */
function firstOnset(out) {
    for (let i = 0; i < out.length; i++) {
        if (Math.abs(out[i]) > LATENCY_THRESHOLD) {
            return i;
        }
    }
    return -1;
}

/* ============================ テスト本体 ============================ */

console.log('PrismEarring — WASM 経路(契約 2)のオフライン検証');
console.log(`Node ${process.version} / prism.wasm ${WASM_BYTES.length} bytes`);

/* --- W1: モジュールの形(import ゼロ・契約 2 の export 一式) --- */
section('W1 モジュールの形 — import ゼロで instantiate できる');
{
    const module = new WebAssembly.Module(WASM_BYTES);
    const imports = WebAssembly.Module.imports(module);
    check(
        'import が 0 本(JS グルー不要・Worklet で直接 instantiate できる)',
        imports.length === 0,
        imports.length === 0 ? 'なし' : JSON.stringify(imports)
    );

    const exports = WebAssembly.Module.exports(module);
    const names = new Set(exports.map((e) => e.name));
    const contractNames = [
        'ps_create',
        'ps_destroy',
        'ps_prepare',
        'ps_io_ptr',
        'ps_process',
        'ps_set_param',
        'ps_latency_ms'
    ];
    const missing = contractNames.filter((n) => !names.has(n));
    check('契約 2 の 7 関数がエクスポートされている', missing.length === 0, missing.length === 0 ? contractNames.join(', ') : '欠落: ' + missing.join(', '));
    check('memory がエクスポートされている(HEAPF32 共有)', names.has('memory'));

    const s = PrismWasmShifter.instantiate(WASM_BYTES);
    check('PrismWasmShifter.instantiate が成功する(ps_create > 0)', s.isPrepared() === false && s.prepare(48000, BLOCK));
    check('ps_io_ptr が HEAPF32 上のビューを返す', s.ioPtr(0) instanceof Float32Array && s.ioPtr(1) instanceof Float32Array, `長さ ${s.ioPtr(0) ? s.ioPtr(0).length : 'null'}`);
    check('範囲外 channel は null', s.ioPtr(2) === null && s.ioPtr(-1) === null);
    s.destroy();
}

/* --- W2: ピッチ精度(BR2.1) --- */
section('W2 ピッチ精度 — shift = -89 cents, 期待比 0.95 ± 0.5%(WASM 経路)');
for (const fs of SAMPLE_RATES) {
    for (const freq of PITCH_FREQS) {
        const res = runSine('wasm', { freq, fs, seconds: 2.0 });
        const est = steadyRatio(res.left, fs, freq);
        const err = (est.ratio - EXPECTED_RATIO) / EXPECTED_RATIO;
        check(
            `fs=${fs} f=${freq}Hz`,
            Math.abs(err) <= PITCH_REL_TOLERANCE,
            `ratio=${est.ratio.toFixed(6)} (期待 ${EXPECTED_RATIO.toFixed(6)}), ` +
                `誤差 ${(err * 100).toFixed(4)}% = ${centsError(est.ratio).toFixed(2)} cents`
        );
    }
}

/* --- W3: セント → 比の一般性(-89 決め打ち禁止、BR1.1) --- */
section('W3 セント→比の一般性 — 半音決め打ちでないこと(WASM 経路)');
for (const cents of [-150, -100, -50, 0]) {
    const fs = 48000;
    const expected = Math.pow(2, cents / 1200);
    const res = runSine('wasm', { freq: 440, fs, seconds: 2.0, cents });
    const est = steadyRatio(res.left, fs, 440, { lo: expected - 0.05, hi: expected + 0.05 });
    const err = (est.ratio - expected) / expected;
    check(
        `cents=${cents}`,
        Math.abs(err) <= PITCH_REL_TOLERANCE,
        `ratio=${est.ratio.toFixed(6)} (期待 ${expected.toFixed(6)}), 誤差 ${(err * 100).toFixed(4)}%`
    );
}

/* --- W4: L/R 独立(FR-1.2) --- */
section('W4 L/R 独立 — shiftCentsL と shiftCentsR が別の比を生む(WASM 経路)');
{
    const fs = 48000;
    const res = runSine('wasm', { freq: 440, fs, seconds: 2.0, cents: -89, centsR: 0 });
    const l = steadyRatio(res.left, fs, 440);
    const r = steadyRatio(res.right, fs, 440);
    check('L = -89 cents', Math.abs((l.ratio - EXPECTED_RATIO) / EXPECTED_RATIO) <= PITCH_REL_TOLERANCE, `ratio=${l.ratio.toFixed(6)}`);
    check('R = 0 cents', Math.abs(r.ratio - 1) <= PITCH_REL_TOLERANCE, `ratio=${r.ratio.toFixed(6)}`);
}

/* --- W5: 設計値遅延と実測遅延(BR2.2 / NFR-1) --- */
section('W5 遅延 — 設計値 = (baseOffset + sweep/2)/fs、実測は 10ms 以下');
{
    const ratio = Math.pow(2, -89 / 1200);
    for (const fs of SAMPLE_RATES) {
        const sweep = Math.max(4, Math.floor((SWEEP_MS * fs) / 1000 + 0.5));
        const expectedSamples = BASE_OFFSET_SAMPLES + 0.5 * sweep;
        const r = runImpulse('wasm', fs, 0.2);
        check(
            `fs=${fs} — ps_latency_samples が設計式と一致`,
            Math.abs(r.designSamples - expectedSamples) < 1e-9,
            `${r.designSamples} サンプル(期待 ${expectedSamples})`
        );
        check(
            `fs=${fs} — 設計値遅延が 10ms 以下`,
            (r.designSamples / fs) * 1000 <= LATENCY_BUDGET_MS,
            `${((r.designSamples / fs) * 1000).toFixed(3)}ms`
        );

        const first = firstOnset(r.out);
        const tolerance = (1 - ratio) * r.windowSamples * 0.5 + LATENCY_SLACK_SAMPLES;
        const ms = first < 0 ? Infinity : (first / fs) * 1000;
        check(
            `fs=${fs} — 実測遅延が 10ms 以下かつ設計値と許容差内`,
            ms <= LATENCY_BUDGET_MS && first >= 0 && Math.abs(first - r.designSamples) <= tolerance,
            `実測 ${first} サンプル = ${ms.toFixed(3)}ms / 設計値 ${r.designSamples} (許容差 ±${tolerance.toFixed(1)})`
        );
        check(
            `fs=${fs} — 実測遅延が C++ 正本の実測値 ${CPP_LATENCY_SAMPLES[fs]} サンプルと一致(±2)`,
            Math.abs(first - CPP_LATENCY_SAMPLES[fs]) <= 2,
            `WASM ${first} サンプル = ${ms.toFixed(3)}ms(C++ 実測 5.17ms)`
        );
    }

    // 窓長を変えても設計値遅延は変わらない(D-D)
    const fs = 48000;
    const a = runImpulse('wasm', fs, 0.05, 10);
    const b = runImpulse('wasm', fs, 0.05, 100);
    check(
        '設計値遅延は crossfadeMs に依存しない(10ms vs 100ms)',
        a.designSamples === b.designSamples,
        `${a.designSamples} サンプル(窓長 ${a.windowSamples} / ${b.windowSamples})`
    );
}

/* --- W6: グリッチ(BR2.3) --- */
section('W6 グリッチ — 5 秒の連続正弦波で不連続点 0 件(先頭 250ms 除外)');
for (const fs of SAMPLE_RATES) {
    const res = runSine('wasm', { freq: 440, fs, seconds: GLITCH_DURATION_SEC });
    const skip = Math.floor(fs * GLITCH_WARMUP_SEC);
    const limit = (GLITCH_SLOPE_FACTOR * 2 * Math.PI * 440 * SIGNAL_AMPLITUDE) / fs;
    let count = 0;
    let worst = 0;
    for (let i = skip + 1; i < res.left.length; i++) {
        const d = Math.abs(res.left[i] - res.left[i - 1]);
        if (d > worst) {
            worst = d;
        }
        if (d > limit) {
            count++;
        }
    }
    check(`fs=${fs} — 不連続点 0 件`, count === 0, `件数 ${count}, max|Δy|=${worst.toFixed(5)} / 閾値 ${limit.toFixed(5)}`);

    const level = rms(res.left, skip, res.left.length);
    const inputRms = SIGNAL_AMPLITUDE / Math.SQRT2;
    check(
        `fs=${fs} — 振幅が維持されている(RMS が入力の 90%〜110%)`,
        level > inputRms * 0.9 && level < inputRms * 1.1,
        `RMS=${level.toFixed(4)} (入力 RMS=${inputRms.toFixed(4)})`
    );
}

/* --- W7: WASM と JS フォールバックの一致 --- */
section('W7 WASM ↔ JS 一致 — 同じ入力で同じ結論になる');
{
    const fs = 48000;
    for (const freq of PITCH_FREQS) {
        const w = runSine('wasm', { freq, fs, seconds: 2.0 });
        const j = runSine('js', { freq, fs, seconds: 2.0 });
        const rw = steadyRatio(w.left, fs, freq).ratio;
        const rj = steadyRatio(j.left, fs, freq).ratio;
        check(
            `f=${freq}Hz — 推定比の差が 0.1% 未満`,
            Math.abs(rw - rj) / rj < 0.001,
            `wasm=${rw.toFixed(6)} / js=${rj.toFixed(6)} (差 ${(((rw - rj) / rj) * 100).toFixed(4)}%)`
        );
    }
    const w = runImpulse('wasm', fs, 0.05);
    const j = runImpulse('js', fs, 0.05);
    check('設計値遅延が一致', w.designSamples === j.designSamples, `${w.designSamples} サンプル`);
    check('窓長・スイープ幅が一致', w.windowSamples === j.windowSamples && w.sweepSamples === j.sweepSamples, `window=${w.windowSamples} / sweep=${w.sweepSamples}`);
    check('インパルス実測遅延が一致(±2 サンプル)', Math.abs(firstOnset(w.out) - firstOnset(j.out)) <= 2, `wasm=${firstOnset(w.out)} / js=${firstOnset(j.out)}`);
}

/* --- W8: エッジケース — prepare の妥当性検証(WF-1.1 / 契約 2) --- */
section('W8 エッジケース — ps_prepare が不正引数で 0 を返す');
{
    const cases = [
        ['fs=0', 0, BLOCK],
        ['fs=7999(下限未満)', 7999, BLOCK],
        ['fs=192001(上限超過)', 192001, BLOCK],
        ['fs=NaN', NaN, BLOCK],
        ['maxBlock=0', 48000, 0],
        ['maxBlock=-1', 48000, -1],
        ['maxBlock=2048(共有 I/O 領域超過)', 48000, 2048]
    ];
    for (const [label, fs, block] of cases) {
        const s = PrismWasmShifter.instantiate(WASM_BYTES);
        const ok = s.prepare(fs, block);
        check(`${label} で prepare が false`, ok === false, `戻り値 ${ok}`);
        check(`${label} の後は ioPtr が null(未 prepare)`, s.ioPtr(0) === null);
        s.destroy();
    }

    const s = PrismWasmShifter.instantiate(WASM_BYTES);
    check('境界値 fs=8000 は成功', s.prepare(8000, BLOCK) === true);
    check('境界値 fs=192000 は成功', s.prepare(192000, BLOCK) === true);
    s.destroy();
}

/* --- W9: エッジケース — ps_set_param のクランプ / 無効値無視(BR1.2) --- */
section('W9 エッジケース — ps_set_param のクランプ・無効値無視');
{
    const fs = 48000;
    // 範囲外の -1000 cents は -150 にクランプされる(= -150 と同じ比になる)
    const clamped = runSine('wasm', { freq: 440, fs, seconds: 2.0, cents: -1000 });
    const atLimit = runSine('wasm', { freq: 440, fs, seconds: 2.0, cents: -150 });
    const rc = steadyRatio(clamped.left, fs, 440, { lo: 0.86, hi: 0.96 }).ratio;
    const rl = steadyRatio(atLimit.left, fs, 440, { lo: 0.86, hi: 0.96 }).ratio;
    check('cents=-1000 は -150 にクランプされる', Math.abs(rc - rl) / rl < 0.001, `ratio -1000: ${rc.toFixed(6)} / -150: ${rl.toFixed(6)}`);

    // 非有限値は無視され、直前の値が保たれる
    const s = makeEngine('wasm', fs, { cents: -89 });
    s.setParam(PS_PARAM.SHIFT_CENTS_L, NaN);
    s.setParam(PS_PARAM.SHIFT_CENTS_L, Infinity);
    const io0 = s.ioPtr(0);
    const io1 = s.ioPtr(1);
    const total = Math.floor(fs * 2.0);
    const out = new Float64Array(total);
    const omega = (2 * Math.PI * 440) / fs;
    for (let start = 0; start < total; start += BLOCK) {
        const n = Math.min(BLOCK, total - start);
        for (let i = 0; i < n; i++) {
            const v = SIGNAL_AMPLITUDE * Math.sin(omega * (start + i));
            io0[i] = v;
            io1[i] = v;
        }
        s.process(n);
        for (let i = 0; i < n; i++) {
            out[start + i] = io0[i];
        }
    }
    const rNaN = steadyRatio(out, fs, 440).ratio;
    check('NaN / Infinity は無視され -89 cents のまま', Math.abs((rNaN - EXPECTED_RATIO) / EXPECTED_RATIO) <= PITCH_REL_TOLERANCE, `ratio=${rNaN.toFixed(6)}`);
    s.destroy();

    // 未知 id は無視される(処理が壊れない)
    const s2 = makeEngine('wasm', fs, { cents: -89 });
    s2.setParam(99, 1.0);
    s2.setParam(-1, 1.0);
    const io = s2.ioPtr(0);
    io[0] = 0.5;
    s2.process(BLOCK);
    check('未知 id を渡しても処理が継続する', Number.isFinite(io[0]), `y[0]=${io[0]}`);
    s2.destroy();
}

/* --- W10: エッジケース — dryWet=0 は完全バイパス(FR-1.3) --- */
section('W10 エッジケース — dryWet=0 で出力が入力と一致する');
{
    const fs = 48000;
    const s = makeEngine('wasm', fs, { cents: -89, dryWet: 0 });
    const io0 = s.ioPtr(0);
    const io1 = s.ioPtr(1);
    const omega = (2 * Math.PI * 440) / fs;
    let worst = 0;
    for (let start = 0; start < fs; start += BLOCK) {
        const expect = new Float32Array(BLOCK);
        for (let i = 0; i < BLOCK; i++) {
            const v = Math.fround(SIGNAL_AMPLITUDE * Math.sin(omega * (start + i)));
            expect[i] = v;
            io0[i] = v;
            io1[i] = v;
        }
        s.process(BLOCK);
        for (let i = 0; i < BLOCK; i++) {
            const d = Math.abs(io0[i] - expect[i]);
            if (d > worst) {
                worst = d;
            }
        }
    }
    check('dryWet=0 で出力が入力とビット一致', worst === 0, `max|y-x| = ${worst}`);
    s.destroy();
}

/* --- W11: エッジケース — 無音入力と可変ブロック長 --- */
section('W11 エッジケース — 無音入力・可変ブロック長・numFrames<=0');
{
    const fs = 48000;
    const s = makeEngine('wasm', fs);
    const io0 = s.ioPtr(0);
    const io1 = s.ioPtr(1);
    let allZero = true;
    for (let k = 0; k < 200; k++) {
        io0.fill(0);
        io1.fill(0);
        s.process(BLOCK);
        for (let i = 0; i < BLOCK; i++) {
            if (io0[i] !== 0 || io1[i] !== 0) {
                allZero = false;
            }
        }
    }
    check('無音入力は無音出力(非有限値・DC を出さない)', allZero);

    // 可変ブロック長(1..128)でも有限値を出し続ける
    const omega = (2 * Math.PI * 440) / fs;
    let finite = true;
    let t = 0;
    for (let k = 0; k < 500; k++) {
        const n = 1 + (k % BLOCK);
        for (let i = 0; i < n; i++) {
            const v = SIGNAL_AMPLITUDE * Math.sin(omega * t++);
            io0[i] = v;
            io1[i] = v;
        }
        s.process(n);
        for (let i = 0; i < n; i++) {
            if (!Number.isFinite(io0[i]) || !Number.isFinite(io1[i])) {
                finite = false;
            }
        }
    }
    check('可変ブロック長でも出力が有限', finite);

    // numFrames <= 0 は何もしない(入力バッファが書き換わらない)
    io0[0] = 0.25;
    s.process(0);
    s.process(-5);
    check('numFrames<=0 は no-op', io0[0] === 0.25, `io0[0]=${io0[0]}`);
    s.destroy();
}

/* --- W12: エッジケース — ローダーの失敗経路(JS フォールバックの引き金) --- */
section('W12 エッジケース — 不正なバイト列では instantiate が例外を投げる');
{
    let threw = false;
    try {
        PrismWasmShifter.instantiate(null);
    } catch (err) {
        threw = true;
    }
    check('bytes=null で例外(→ PrismProcessor は JS へフォールバック)', threw);

    threw = false;
    try {
        PrismWasmShifter.instantiate(new Uint8Array([0, 1, 2, 3, 4, 5, 6, 7]));
    } catch (err) {
        threw = true;
    }
    check('壊れたバイト列で例外', threw);

    // 破損検出: マジックナンバーだけ正しいゴミ
    threw = false;
    try {
        const bad = new Uint8Array(WASM_BYTES.slice(0, 64));
        PrismWasmShifter.instantiate(bad);
    } catch (err) {
        threw = true;
    }
    check('途中で切れた wasm で例外', threw);
}

/* --- W13: 契約 2 を C API として直接叩く(ハンドル管理) --- */
section('W13 契約 2 の C API — ハンドルの生成・破棄・枯渇');
{
    const ex = rawInstance();
    check('ps_create が 1 以上のハンドルを返す', ex.ps_create() >= 1);

    // インスタンス上限(4)まで作ると 5 本目は 0(契約 2: 0 = 失敗)
    const ex2 = rawInstance();
    const handles = [];
    for (let i = 0; i < 4; i++) {
        handles.push(ex2.ps_create());
    }
    check('4 本まで作れる', handles.every((h) => h >= 1), `handles=${handles.join(',')}`);
    check('5 本目は 0 を返す(枯渇)', ex2.ps_create() === 0);
    ex2.ps_destroy(handles[0]);
    check('destroy 後は再び作れる', ex2.ps_create() >= 1);

    // 無効ハンドルは黙って無視され、例外も異常終了もしない
    const ex3 = rawInstance();
    check('無効ハンドルの ps_prepare は 0', ex3.ps_prepare(0, 48000, BLOCK) === 0 && ex3.ps_prepare(99, 48000, BLOCK) === 0);
    check('無効ハンドルの ps_io_ptr は 0', ex3.ps_io_ptr(0, 0) === 0 && ex3.ps_io_ptr(99, 0) === 0);
    check('無効ハンドルの ps_latency_ms は 0', ex3.ps_latency_ms(0) === 0);
    ex3.ps_destroy(99); // 例外なく戻ること
    ex3.ps_process(99, BLOCK);
    ex3.ps_set_param(99, 0, -89);
    check('無効ハンドルへの destroy/process/set_param が無害', true);

    // 未 prepare での process は no-op、prepare 後は ps_latency_ms が >0
    const h = ex3.ps_create();
    ex3.ps_process(h, BLOCK);
    check('未 prepare の ps_process は no-op', ex3.ps_latency_ms(h) === 0);
    // 設計値遅延 = (baseOffset + sweep/2) / fs。48kHz では sweep=456 → 236 サンプル。
    const expectedMs = ((BASE_OFFSET_SAMPLES + 0.5 * Math.round((SWEEP_MS * 48000) / 1000)) / 48000) * 1000;
    check(
        'prepare 後の ps_latency_ms が設計値と一致',
        ex3.ps_prepare(h, 48000, BLOCK) === 1 && Math.abs(ex3.ps_latency_ms(h) - expectedMs) < 1e-9,
        `${ex3.ps_latency_ms(h).toFixed(4)}ms(期待 ${expectedMs.toFixed(4)}ms)`
    );
}

/* --- W14: 処理コスト(PR-1 の目安) --- */
section('W14 処理コスト — 128 フレームの処理時間(WASM / JS 比較)');
{
    const fs = 48000;
    const quantumMs = (BLOCK / fs) * 1000;
    const results = {};
    for (const kind of ['wasm', 'js']) {
        const s = makeEngine(kind, fs);
        const io0 = s.ioPtr(0);
        const io1 = s.ioPtr(1);
        const omega = (2 * Math.PI * 440) / fs;
        for (let i = 0; i < BLOCK; i++) {
            const v = SIGNAL_AMPLITUDE * Math.sin(omega * i);
            io0[i] = v;
            io1[i] = v;
        }
        for (let k = 0; k < 2000; k++) {
            s.process(BLOCK); // ウォームアップ
        }
        const blocks = 20000;
        const t0 = process.hrtime.bigint();
        for (let k = 0; k < blocks; k++) {
            s.process(BLOCK);
        }
        const elapsedMs = Number(process.hrtime.bigint() - t0) / 1e6;
        results[kind] = elapsedMs / blocks;
        s.destroy();
    }
    const load = (results.wasm / quantumMs) * 100;
    check(
        `WASM の 1 ブロック処理がレンダ量子時間の 50% 以内(PR-1)`,
        load < 50,
        `${results.wasm.toFixed(4)}ms / ${quantumMs.toFixed(3)}ms = ${load.toFixed(2)}% ` +
            `(JS フォールバックは ${results.js.toFixed(4)}ms = ${((results.js / quantumMs) * 100).toFixed(2)}%)`
    );
}

/* --- W15: process 経路の確保ゼロ(BR1.5) --- */
section('W15 リアルタイム安全性 — process 経路に確保がない');
{
    const fs = 48000;
    const s = makeEngine('wasm', fs);
    const io0 = s.ioPtr(0);
    const io1 = s.ioPtr(1);
    const omega = (2 * Math.PI * 440) / fs;
    for (let i = 0; i < BLOCK; i++) {
        const v = SIGNAL_AMPLITUDE * Math.sin(omega * i);
        io0[i] = v;
        io1[i] = v;
    }
    for (let k = 0; k < 5000; k++) {
        s.process(BLOCK); // ウォームアップ
    }
    const blocks = 100000;
    const before = process.memoryUsage().heapUsed;
    for (let k = 0; k < blocks; k++) {
        s.process(BLOCK);
    }
    const delta = process.memoryUsage().heapUsed - before;
    const budget = 256 * 1024;
    check(
        `${blocks} ブロック連続処理で JS ヒープが増えない`,
        delta < budget,
        `Δ heapUsed = ${(delta / 1024).toFixed(1)} KiB (閾値 ${(budget / 1024).toFixed(0)} KiB)`
    );

    // wasm 線形メモリが伸びない(ALLOW_MEMORY_GROWTH=0 の担保)。
    // 伸びると ArrayBuffer が detach してビューが無効になる。
    const view = s.ioPtr(0);
    check('共有 I/O ビューが detach していない', view.length === BLOCK && !view.buffer.detached, `length=${view.length}`);
    s.destroy();
}

/* --- W16: 接着層の統合(契約 3 + エンジン選択) --- */
section('W16 統合 — PrismProcessor が WASM を選び、失敗時は JS へ落ちる');
{
    /**
     * AudioWorklet 環境を模して PrismProcessor を 1 個起動し、送信メッセージを集める。
     * ready / error はコンストラクタ内で送られるため、postMessage は
     * AudioWorkletProcessor スタブの側で最初から差し替えておく。
     */
    function spawnCapturing(fs, processorOptions) {
        const source = readFileSync(WORKLET_PATH, 'utf8');
        const messages = [];
        let registered = null;
        const context = vm.createContext({
            console,
            WebAssembly,
            ArrayBuffer,
            Float32Array,
            Error,
            sampleRate: fs,
            currentTime: 0,
            AudioWorkletProcessor: class {
                constructor() {
                    this.port = {
                        onmessage: null,
                        postMessage: (msg) => messages.push(msg)
                    };
                }
            },
            registerProcessor: (name, cls) => {
                registered = cls;
            }
        });
        new vm.Script(source, { filename: 'prism-worklet.js' }).runInContext(context);
        if (typeof registered !== 'function') {
            throw new Error('registerProcessor が呼ばれませんでした');
        }
        const proc = new registered({ processorOptions });
        return { proc, messages };
    }

    const fs = 48000;
    const quantum = (n) => [new Float32Array(n), new Float32Array(n)];

    // (a) wasm バイト列あり → engine: wasm
    {
        const { proc, messages } = spawnCapturing(fs, {
            maxBlockFrames: BLOCK,
            wasmBytes: WASM_BYTES
        });
        const ready = messages.find((m) => m.type === 'ready');
        check('ready メッセージが engine=wasm を報告する', !!ready && ready.engine === 'wasm', JSON.stringify(ready));
        const latency = messages.find((m) => m.type === 'latency');
        const expectedMs =
            ((BASE_OFFSET_SAMPLES + 0.5 * Math.round((SWEEP_MS * fs) / 1000)) / fs) * 1000;
        check(
            'latency メッセージが engine と dspLatencyMs を持つ',
            !!latency && latency.engine === 'wasm' && Math.abs(latency.dspLatencyMs - expectedMs) < 1e-9,
            JSON.stringify(latency)
        );

        // 実際にレンダしてみる(入力 = 440Hz、出力が無音でないこと)
        const inputs = [quantum(BLOCK)];
        const outputs = [quantum(BLOCK)];
        const omega = (2 * Math.PI * 440) / fs;
        let t = 0;
        let peak = 0;
        for (let k = 0; k < 400; k++) {
            for (let i = 0; i < BLOCK; i++) {
                const v = SIGNAL_AMPLITUDE * Math.sin(omega * t++);
                inputs[0][0][i] = v;
                inputs[0][1][i] = v;
            }
            proc.process(inputs, outputs);
            for (let i = 0; i < BLOCK; i++) {
                peak = Math.max(peak, Math.abs(outputs[0][0][i]));
            }
        }
        check('process() が音を出す(無音でない)', peak > 0.1, `peak=${peak.toFixed(4)}`);

        // 契約 3: param メッセージでパラメータが変わる(例外なく処理される)
        proc.port.onmessage({ data: { type: 'param', id: PS_PARAM.DRY_WET, value: 0 } });
        proc.port.onmessage({ data: { type: 'param', id: 99, value: 1 } });
        proc.port.onmessage({ data: { type: 'reset' } });
        proc.port.onmessage({ data: { type: 'requestLatency' } });
        proc.port.onmessage({ data: null });
        const latencies = messages.filter((m) => m.type === 'latency');
        check('requestLatency で latency が再送される', latencies.length >= 2, `${latencies.length} 通`);
        check('不正メッセージでも落ちない', true);
    }

    // (b) wasm バイト列なし → JS フォールバック(engine: js + 理由つき)
    {
        const { messages } = spawnCapturing(fs, { maxBlockFrames: BLOCK, wasmBytes: null });
        const ready = messages.find((m) => m.type === 'ready');
        check('bytes が無いと engine=js へフォールバックする', !!ready && ready.engine === 'js', JSON.stringify(ready));
        check('フォールバック理由が付く', !!ready && typeof ready.fallbackReason === 'string' && ready.fallbackReason.length > 0, ready ? ready.fallbackReason : '');
    }

    // (c) 壊れた wasm → JS フォールバック
    {
        const { messages } = spawnCapturing(fs, {
            maxBlockFrames: BLOCK,
            wasmBytes: new Uint8Array([1, 2, 3, 4])
        });
        const ready = messages.find((m) => m.type === 'ready');
        check('壊れた wasm でも engine=js で動く', !!ready && ready.engine === 'js', ready ? ready.fallbackReason : '(ready なし)');
    }

    // (d) 両エンジンとも prepare できない sampleRate → 契約 3 の error
    {
        const { messages } = spawnCapturing(1000, { maxBlockFrames: BLOCK, wasmBytes: WASM_BYTES });
        const error = messages.find((m) => m.type === 'error');
        check('sampleRate=1000 では error を送る(契約 3)', !!error, error ? error.message : '(error なし)');
        check('ready は送らない', !messages.some((m) => m.type === 'ready'));
    }
}

/* ------------------------------ 集計 ------------------------------ */

console.log(`\n合計: ${passes} PASS / ${failures} FAIL`);
process.exit(failures === 0 ? 0 : 1);
