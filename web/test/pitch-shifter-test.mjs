/*
 * pitch-shifter-test.mjs — PitchShifterJS のオフライン数値検証(u3 の JS 実装)
 *
 * 位置づけ: u2-verification(C++ 正本 verify/verify.cpp)とは別物。ブラウザを
 * 起動できない環境で、JS 移植が C++ 正本と同一アルゴリズムであることを数値で
 * 確認する。判定式・閾値は verify.cpp と揃えてある(BR2.1〜BR2.4)。
 * team.md「Testing Posture」の test-after に従い、実装後に書いて実行する。
 *
 * 実行: node web/test/pitch-shifter-test.mjs
 * 依存: Node 18+ のみ(外部パッケージなし)
 *
 * 周波数推定は検証側の離散フーリエ変換(自前実装、Hann 窓 + 任意周波数の
 * Goertzel 相当)で行う。音声経路には FFT を一切使っていない(CLAUDE.md)。
 *
 * C++ 正本の実測値(Apple M2 / clang / -O2、README.md「実測値」):
 *   遅延 228 サンプル @44.1k / 248 サンプル @48k = いずれも 5.17ms
 *   110Hz の誤差 +0.93 cents @44.1k / -1.55 cents @48k(許容差 ±8.66 cents)
 *   グリッチ 0 件(max|Δy| = 0.0298 @44.1k / 0.0274 @48k)
 * JS 側がこれらと食い違う場合は移植ミスであり、判定を緩めてはならない。
 */

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import vm from 'node:vm';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const WORKLET_PATH = path.join(HERE, '..', 'prism-worklet.js');

/* ---- prism-worklet.js を classic script として評価してコアを取り出す ---- */
function loadCore() {
    const source = readFileSync(WORKLET_PATH, 'utf8');
    const context = vm.createContext({ console });
    new vm.Script(source, { filename: 'prism-worklet.js' }).runInContext(context);
    if (typeof context.PitchShifterJS !== 'function') {
        throw new Error('PitchShifterJS を prism-worklet.js から取得できませんでした');
    }
    return { PitchShifterJS: context.PitchShifterJS, PS_PARAM: context.PS_PARAM };
}

const { PitchShifterJS, PS_PARAM } = loadCore();

/* ---------------------------- 判定閾値 ---------------------------- */
/* verify.cpp の名前付き定数と 1:1(SR-3.1: 閾値は単一定義)。 */

const EXPECTED_RATIO = 418 / 440;      // = 0.95(CLAUDE.md の実測値)
const PITCH_REL_TOLERANCE = 0.005;     // BR2.1: ±0.5%
const LATENCY_BUDGET_MS = 10.0;        // BR2.2 / NFR-1
const LATENCY_THRESHOLD = 0.05;        // BR2.2: -26 dBFS
const LATENCY_SLACK_SAMPLES = 8.0;     // BR2.2: 設計値一致許容差の定数項
const GLITCH_SLOPE_FACTOR = 3.0;       // BR2.3: k = 3.0
const GLITCH_WARMUP_SEC = 0.250;       // BR2.3: warmup 250ms 除外
const GLITCH_DURATION_SEC = 5.0;
const SIGNAL_AMPLITUDE = 0.5;
const BASE_OFFSET_SAMPLES = 8;         // PitchShifter.h kBaseOffsetSamples
const SWEEP_MS = 9.5;                  // PitchShifter.h kSweepMs
const DEFAULT_CROSSFADE_MS = 50;
const SAMPLE_RATES = [44100, 48000];
const PITCH_FREQS = [110, 440, 3520];

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

/* ------------------------- 信号処理ヘルパ ------------------------- */

const BLOCK = 128;

function makeShifter(fs, cents, xfade, dryWet = 1, block = BLOCK) {
    const s = PitchShifterJS.create();
    if (!s.prepare(fs, block)) {
        throw new Error(`prepare が失敗しました (fs=${fs})`);
    }
    s.setParam(PS_PARAM.SHIFT_CENTS_L, cents);
    s.setParam(PS_PARAM.SHIFT_CENTS_R, cents);
    s.setParam(PS_PARAM.DRY_WET, dryWet);
    s.setParam(PS_PARAM.CROSSFADE_MS, xfade);
    s.reset(); // 平滑器を整定済みにして測定条件を安定させる
    return s;
}

/**
 * 正弦波を通して出力を得る。
 * @returns {{left: Float64Array, right: Float64Array, input: Float64Array}}
 */
function runSine({
    freq,
    fs,
    seconds,
    centsL,
    centsR,
    dryWet,
    crossfadeMs,
    amplitude = SIGNAL_AMPLITUDE
}) {
    const shifter = PitchShifterJS.create();
    if (!shifter.prepare(fs, BLOCK)) {
        throw new Error('prepare が失敗しました');
    }
    shifter.setParam(PS_PARAM.SHIFT_CENTS_L, centsL);
    shifter.setParam(PS_PARAM.SHIFT_CENTS_R, centsR);
    shifter.setParam(PS_PARAM.DRY_WET, dryWet);
    shifter.setParam(PS_PARAM.CROSSFADE_MS, crossfadeMs);
    shifter.reset();

    const total = Math.floor(fs * seconds);
    const left = new Float64Array(total);
    const right = new Float64Array(total);
    const input = new Float64Array(total);
    const io0 = shifter.ioPtr(0);
    const io1 = shifter.ioPtr(1);
    const omega = (2 * Math.PI * freq) / fs;

    for (let start = 0; start < total; start += BLOCK) {
        const n = Math.min(BLOCK, total - start);
        for (let i = 0; i < n; i++) {
            const s = amplitude * Math.sin(omega * (start + i));
            input[start + i] = s;
            io0[i] = s;
            io1[i] = s;
        }
        shifter.process(n);
        for (let i = 0; i < n; i++) {
            left[start + i] = io0[i];
            right[start + i] = io1[i];
        }
    }
    return {
        left,
        right,
        input,
        latencyMs: shifter.latencyMs(),
        latencySamples: shifter.latencySamples(),
        windowSamples: shifter.windowSamples()
    };
}

/** 単位インパルス(sample 0、振幅 1.0)を入れて出力を返す(verify.cpp testLatency)。 */
function runImpulse(fs, seconds, crossfadeMs) {
    const s = makeShifter(fs, -89, crossfadeMs);
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
    return {
        out,
        designSamples: s.latencySamples(),
        windowSamples: s.windowSamples(),
        sweepSamples: s.sweepSamples()
    };
}

/** Hann 窓つき任意周波数 DFT の振幅(自前実装)。 */
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

/**
 * 入力周波数に対する出力周波数比のピークを探索する。
 * 粗探索 → 放物線補間つき微細探索の 2 段。
 */
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
    // 微細探索(粗ステップ ±1 の範囲)
    const fineStep = 0.00002;
    let fineBest = bestRatio;
    let fineBestMag = -1;
    const fineLo = bestRatio - coarseStep;
    const fineHi = bestRatio + coarseStep;
    const mags = [];
    const ratios = [];
    for (let r = fineLo; r <= fineHi + 1e-12; r += fineStep) {
        const m = dftMagnitude(x, from, len, fs, inputFreq * r);
        ratios.push(r);
        mags.push(m);
        if (m > fineBestMag) {
            fineBestMag = m;
            fineBest = r;
        }
    }
    // 放物線補間でピーク位置を細分化する
    const k = mags.indexOf(fineBestMag);
    if (k > 0 && k < mags.length - 1) {
        const magMinus = mags[k - 1];
        const magPlus = mags[k + 1];
        const denom = magMinus - 2 * fineBestMag + magPlus;
        if (denom !== 0) {
            const delta = (0.5 * (magMinus - magPlus)) / denom;
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

function maxAbsDiff(x, from, to) {
    let m = 0;
    for (let i = from + 1; i < to; i++) {
        const d = Math.abs(x[i] - x[i - 1]);
        if (d > m) {
            m = d;
        }
    }
    return m;
}

function rms(x, from, to) {
    let s = 0;
    for (let i = from; i < to; i++) {
        s += x[i] * x[i];
    }
    return Math.sqrt(s / (to - from));
}

/* ============================ テスト本体 ============================ */

console.log('PrismEarring — PitchShifterJS オフライン検証(C++ 正本の直訳を数値で確認)');
console.log(`Node ${process.version}`);

/* --- T1: ピッチ精度(BR2.1) --- */
section('T1 ピッチ精度 — shift = -89 cents, 期待比 0.95 ± 0.5%');

for (const fs of SAMPLE_RATES) {
    for (const freq of PITCH_FREQS) {
        const res = runSine({
            freq,
            fs,
            seconds: 2.0,
            centsL: -89,
            centsR: -89,
            dryWet: 1,
            crossfadeMs: DEFAULT_CROSSFADE_MS
        });
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

/* --- T2: cents → 比の対応(-89 決め打ち禁止の確認) --- */
section('T2 セント→比の一般性 — 半音決め打ちでないこと(BR1.1)');

for (const cents of [-150, -100, -50, 0]) {
    const fs = 48000;
    const expected = Math.pow(2, cents / 1200);
    const res = runSine({
        freq: 440,
        fs,
        seconds: 2.0,
        centsL: cents,
        centsR: cents,
        dryWet: 1,
        crossfadeMs: DEFAULT_CROSSFADE_MS
    });
    const est = steadyRatio(res.left, fs, 440, { lo: expected - 0.05, hi: expected + 0.05 });
    const err = (est.ratio - expected) / expected;
    check(
        `cents=${cents}`,
        Math.abs(err) <= PITCH_REL_TOLERANCE,
        `ratio=${est.ratio.toFixed(6)} (期待 ${expected.toFixed(6)}), 誤差 ${(err * 100).toFixed(4)}%`
    );
}

/* --- T3: L/R 独立(FR-1.2 / D-E) --- */
section('T3 L/R 独立 — shiftCentsL と shiftCentsR が別の比を生む');
{
    const fs = 48000;
    const res = runSine({
        freq: 440,
        fs,
        seconds: 2.0,
        centsL: -89,
        centsR: 0,
        dryWet: 1,
        crossfadeMs: DEFAULT_CROSSFADE_MS
    });
    const l = steadyRatio(res.left, fs, 440);
    const r = steadyRatio(res.right, fs, 440);
    check(
        'L = -89 cents',
        Math.abs((l.ratio - EXPECTED_RATIO) / EXPECTED_RATIO) <= PITCH_REL_TOLERANCE,
        `ratio=${l.ratio.toFixed(6)}`
    );
    check(
        'R = 0 cents',
        Math.abs(r.ratio - 1) <= PITCH_REL_TOLERANCE,
        `ratio=${r.ratio.toFixed(6)}`
    );
}

/* --- T4a: 設計値遅延の式(D-D、C++ getLatencySamples() と同一) --- */
section('T4a 設計値遅延 — latency = (baseOffset + sweep/2) / fs、窓長に依存しない');
{
    for (const fs of SAMPLE_RATES) {
        const sweep = Math.max(4, Math.floor((SWEEP_MS * fs) / 1000 + 0.5));
        const expectedSamples = BASE_OFFSET_SAMPLES + sweep / 2;
        const expectedMs = (expectedSamples / fs) * 1000;
        const observed = [];
        for (const xfade of [10, 20, 50, 100]) {
            const s = makeShifter(fs, -89, xfade);
            observed.push(s.latencyMs());
            check(
                `fs=${fs} crossfade=${xfade}ms — 式と一致`,
                Math.abs(s.latencyMs() - expectedMs) < 1e-9 &&
                    Math.abs(s.latencySamples() - expectedSamples) < 1e-9 &&
                    s.sweepSamples() === sweep,
                `実測 ${s.latencyMs().toFixed(4)}ms (${s.latencySamples()} サンプル) / 式 ${expectedMs.toFixed(4)}ms`
            );
        }
        const spread = Math.max(...observed) - Math.min(...observed);
        check(
            `fs=${fs} — crossfadeMs を変えても遅延が変わらない`,
            spread < 1e-12,
            `ばらつき ${spread.toExponential(2)}ms`
        );
    }
}

/* --- T4b: NFR-1(処理部 10ms 以下)を全パラメータ域で満たす --- */
section('T4b NFR-1 — 設計値遅延が全パラメータ域で 10ms 以下');
{
    for (const fs of SAMPLE_RATES) {
        for (const xfade of [10, 50, 100]) {
            for (const cents of [-150, -89, 0]) {
                const s = makeShifter(fs, cents, xfade);
                const ms = s.latencyMs();
                check(
                    `fs=${fs} cents=${cents} crossfade=${xfade}ms`,
                    ms > 0 && ms <= LATENCY_BUDGET_MS,
                    `dspLatency=${ms.toFixed(3)}ms`
                );
            }
        }
    }
}

/* --- T4c: 実測遅延(インパルス応答、BR2.2 / verify.cpp testLatency) --- */
section('T4c 実測遅延 — インパルスが |y|>0.05 を超える最初のサンプル');
{
    const ratio = Math.pow(2, -89 / 1200);
    for (const fs of SAMPLE_RATES) {
        const r = runImpulse(fs, 0.2, DEFAULT_CROSSFADE_MS);
        let first = -1;
        for (let i = 0; i < r.out.length; i++) {
            if (Math.abs(r.out[i]) > LATENCY_THRESHOLD) {
                first = i;
                break;
            }
        }
        const tolerance = (1 - ratio) * r.windowSamples * 0.5 + LATENCY_SLACK_SAMPLES;
        const ms = first < 0 ? Infinity : (first / fs) * 1000;
        const withinBudget = ms <= LATENCY_BUDGET_MS;
        const matchesDesign = first >= 0 && Math.abs(first - r.designSamples) <= tolerance;
        check(
            `fs=${fs} — 10ms 以下かつ設計値と許容差内`,
            withinBudget && matchesDesign,
            `実測 ${first} サンプル = ${ms.toFixed(3)}ms / 設計値 ${r.designSamples} ` +
                `(許容差 ±${tolerance.toFixed(1)} サンプル)`
        );
    }
}

/* --- T5: グリッチ(BR2.3 / verify.cpp testGlitch) --- */
section('T5 グリッチ — 5 秒の連続正弦波で不連続点 0 件(先頭 250ms 除外)');
{
    for (const fs of SAMPLE_RATES) {
        const res = runSine({
            freq: 440,
            fs,
            seconds: GLITCH_DURATION_SEC,
            centsL: -89,
            centsR: -89,
            dryWet: 1,
            crossfadeMs: DEFAULT_CROSSFADE_MS
        });
        const skip = Math.floor(fs * GLITCH_WARMUP_SEC);
        const limit =
            (GLITCH_SLOPE_FACTOR * 2 * Math.PI * 440 * SIGNAL_AMPLITUDE) / fs;
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
        check(
            `fs=${fs} — 不連続点 0 件`,
            count === 0,
            `件数 ${count}, max|Δy|=${worst.toFixed(5)} / 閾値 ${limit.toFixed(5)}`
        );
        // 定振幅クロスフェード(D-C)なので、長時間 RMS は入力とほぼ等しくなる。
        // 等パワー合成に戻ると +3dB のこぶが出るため、この検査で退行を捕まえる。
        const level = rms(res.left, skip, res.left.length);
        const inputRms = SIGNAL_AMPLITUDE / Math.SQRT2;
        check(
            `fs=${fs} — 振幅が維持されている(RMS が入力の 90%〜110%)`,
            level > inputRms * 0.9 && level < inputRms * 1.1,
            `RMS=${level.toFixed(4)} (入力 RMS=${inputRms.toFixed(4)})`
        );
    }
}

/* --- T6: エッジケース — prepare の不正引数 --- */
section('T6 エッジケース — prepare の妥当性検証(WF-1.1)');
{
    const cases = [
        ['fs=1000(下限未満)', 1000, 128, false],
        ['fs=200000(上限超)', 200000, 128, false],
        ['maxBlockFrames=0', 48000, 0, false],
        ['fs=NaN', NaN, 128, false],
        ['fs=8000(下限)', 8000, 1, true],
        ['fs=192000(上限)', 192000, 128, true]
    ];
    for (const [name, fs, block, expected] of cases) {
        const s = PitchShifterJS.create();
        const ok = s.prepare(fs, block);
        check(name, ok === expected, `prepare -> ${ok}`);
    }
    // prepare 前の process/ioPtr は安全に何もしない
    const unprepared = PitchShifterJS.create();
    let threw = false;
    try {
        unprepared.process(128);
    } catch (e) {
        threw = true;
    }
    check('prepare 前の process が例外を投げない', !threw);
    check('prepare 前の ioPtr が null', unprepared.ioPtr(0) === null);
    check('prepare 前の latencyMs が 0', unprepared.latencyMs() === 0);
}

/* --- T7: エッジケース — setParam のクランプと無視 --- */
section('T7 エッジケース — setParam のクランプ/無効値無視(BR1.2)');
{
    const fs = 48000;
    // cents は [-150, 0] にクランプされる: -400 は -150 として振る舞う
    const clamped = runSine({
        freq: 440,
        fs,
        seconds: 2.0,
        centsL: -400,
        centsR: -400,
        dryWet: 1,
        crossfadeMs: DEFAULT_CROSSFADE_MS
    });
    const expectedClamp = Math.pow(2, -150 / 1200);
    const estClamp = steadyRatio(clamped.left, fs, 440, {
        lo: expectedClamp - 0.05,
        hi: expectedClamp + 0.05
    });
    check(
        'shiftCents=-400 は -150 にクランプされる',
        Math.abs((estClamp.ratio - expectedClamp) / expectedClamp) <= PITCH_REL_TOLERANCE,
        `ratio=${estClamp.ratio.toFixed(6)} (期待 ${expectedClamp.toFixed(6)})`
    );

    // crossfadeMs は [10, 100] にクランプされる(窓長サンプル数で検算)
    const sLow = makeShifter(fs, -89, 1);
    const sHigh = makeShifter(fs, -89, 999);
    check(
        'crossfadeMs=1 は 10ms に、999 は 100ms にクランプされる',
        sLow.windowSamples() === Math.round((10 * fs) / 1000) &&
            sHigh.windowSamples() === Math.round((100 * fs) / 1000),
        `window=${sLow.windowSamples()} / ${sHigh.windowSamples()} サンプル`
    );

    // 非有限値・未知 id は無視され、直前の値が保たれる
    const s = makeShifter(fs, -89, 50);
    const before = s.windowSamples();
    s.setParam(PS_PARAM.CROSSFADE_MS, NaN);
    s.setParam(PS_PARAM.CROSSFADE_MS, Infinity);
    s.reset();
    check('NaN / Infinity を無視する', s.windowSamples() === before, `window=${s.windowSamples()}`);
    s.setParam(-1, 10);
    s.setParam(4, 10);
    s.setParam(99, 10);
    s.reset();
    check('未知の id を無視する', s.windowSamples() === before, `window=${s.windowSamples()}`);
}

/* --- T8: エッジケース — dryWet=0 は完全バイパス --- */
section('T8 エッジケース — dryWet=0 で入力と一致(FR-1.3)');
{
    const fs = 48000;
    const res = runSine({
        freq: 440,
        fs,
        seconds: 0.5,
        centsL: -89,
        centsR: -89,
        dryWet: 0,
        crossfadeMs: DEFAULT_CROSSFADE_MS
    });
    let maxErr = 0;
    for (let i = 0; i < res.left.length; i++) {
        const e = Math.abs(res.left[i] - res.input[i]);
        if (e > maxErr) {
            maxErr = e;
        }
    }
    check('出力 == 入力(誤差 < 1e-6)', maxErr < 1e-6, `max誤差 ${maxErr.toExponential(3)}`);
}

/* --- T9: エッジケース — 無音入力・可変ブロック長 --- */
section('T9 エッジケース — 無音入力・可変ブロック長');
{
    const fs = 48000;
    const s = makeShifter(fs, -89, DEFAULT_CROSSFADE_MS, 1, 512);
    const io0 = s.ioPtr(0);
    const io1 = s.ioPtr(1);
    let maxOut = 0;
    let sawNaN = false;
    // 1, 7, 128, 512, 1024(上限超 → 上限にクランプ), 0, -5 を混ぜる
    for (const n of [1, 7, 128, 512, 1024, 0, -5]) {
        io0.fill(0);
        io1.fill(0);
        s.process(n);
        const len = Math.min(Math.max(n, 0), 512);
        for (let i = 0; i < len; i++) {
            if (!Number.isFinite(io0[i]) || !Number.isFinite(io1[i])) {
                sawNaN = true;
            }
            maxOut = Math.max(maxOut, Math.abs(io0[i]), Math.abs(io1[i]));
        }
    }
    check('無音入力で NaN/Inf が出ない', !sawNaN);
    check('無音入力で出力が 0', maxOut === 0, `max|out|=${maxOut}`);
}

/* --- T10: crossfadeMs 変更中もグリッチが出ない(境界ラッチ BR1.4) --- */
section('T10 crossfadeMs のライブ変更 — 不連続なし');
{
    const fs = 48000;
    const freq = 440;
    const amp = SIGNAL_AMPLITUDE;
    const s = makeShifter(fs, -89, DEFAULT_CROSSFADE_MS);
    const io0 = s.ioPtr(0);
    const io1 = s.ioPtr(1);
    const total = fs * 2;
    const out = new Float64Array(total);
    const omega = (2 * Math.PI * freq) / fs;
    let blockIndex = 0;
    for (let start = 0; start < total; start += BLOCK) {
        const n = Math.min(BLOCK, total - start);
        // 10ms ⇄ 100ms を約 50ms ごとに往復させる
        const ms = blockIndex % 40 < 20 ? 10 : 100;
        s.setParam(PS_PARAM.CROSSFADE_MS, ms);
        for (let i = 0; i < n; i++) {
            const v = amp * Math.sin(omega * (start + i));
            io0[i] = v;
            io1[i] = v;
        }
        s.process(n);
        for (let i = 0; i < n; i++) {
            out[start + i] = io0[i];
        }
        blockIndex++;
    }
    const from = Math.floor(fs * GLITCH_WARMUP_SEC);
    const observed = maxAbsDiff(out, from, total);
    const limit = (GLITCH_SLOPE_FACTOR * 2 * Math.PI * freq * amp) / fs;
    check(
        'crossfadeMs 往復中の max|Δ| < 3× 理論スロープ',
        observed < limit,
        `観測 ${observed.toExponential(3)} / 閾値 ${limit.toExponential(3)}`
    );
}

/* --- T11: パラメータ平滑(ズィッパーノイズなし) --- */
section('T11 パラメータ平滑 — cents ステップ変化で不連続が出ない(BR1.3)');
{
    const fs = 48000;
    const freq = 440;
    const amp = SIGNAL_AMPLITUDE;
    const s = makeShifter(fs, 0, DEFAULT_CROSSFADE_MS);
    const io0 = s.ioPtr(0);
    const io1 = s.ioPtr(1);
    const total = fs * 1;
    const out = new Float64Array(total);
    const omega = (2 * Math.PI * freq) / fs;
    for (let start = 0; start < total; start += BLOCK) {
        const n = Math.min(BLOCK, total - start);
        if (start >= fs * 0.5 && start < fs * 0.5 + BLOCK) {
            s.setParam(PS_PARAM.SHIFT_CENTS_L, -150); // 0 → -150 の急変
            s.setParam(PS_PARAM.SHIFT_CENTS_R, -150);
        }
        for (let i = 0; i < n; i++) {
            const v = amp * Math.sin(omega * (start + i));
            io0[i] = v;
            io1[i] = v;
        }
        s.process(n);
        for (let i = 0; i < n; i++) {
            out[start + i] = io0[i];
        }
    }
    const observed = maxAbsDiff(out, Math.floor(fs * 0.2), total);
    const limit = (GLITCH_SLOPE_FACTOR * 2 * Math.PI * freq * amp) / fs;
    check(
        'ステップ変化直後も max|Δ| < 3× 理論スロープ',
        observed < limit,
        `観測 ${observed.toExponential(3)} / 閾値 ${limit.toExponential(3)}`
    );
}

/* --- T12: レンダ量子あたりの処理時間(PR-1 の目安) --- */
section('T12 処理コスト — 128 フレームの処理時間');
{
    const fs = 48000;
    const s = makeShifter(fs, -89, DEFAULT_CROSSFADE_MS);
    s.setParam(PS_PARAM.SHIFT_CENTS_R, -91); // L/R で別 rate(最悪寄り)
    const io0 = s.ioPtr(0);
    const io1 = s.ioPtr(1);
    const omega = (2 * Math.PI * 440) / fs;
    for (let i = 0; i < BLOCK; i++) {
        const v = SIGNAL_AMPLITUDE * Math.sin(omega * i);
        io0[i] = v;
        io1[i] = v;
    }
    // ウォームアップ(跳躍探索も含めて JIT を暖める)
    for (let k = 0; k < 5000; k++) {
        s.process(BLOCK);
    }
    const iterations = 20000;
    const t0 = process.hrtime.bigint();
    for (let k = 0; k < iterations; k++) {
        s.process(BLOCK);
    }
    const t1 = process.hrtime.bigint();
    const perBlockMs = Number(t1 - t0) / 1e6 / iterations;
    const quantumMs = (BLOCK / fs) * 1000;
    const load = (perBlockMs / quantumMs) * 100;
    check(
        'Node 上の平均処理時間がレンダ量子の 50% 未満',
        load < 50,
        `${perBlockMs.toFixed(4)}ms / ${quantumMs.toFixed(3)}ms = ${load.toFixed(2)}%`
    );
}

/* --- T13: process 経路にヒープ確保がない(BR1.5 / SR-4.2 相当) --- */
section('T13 リアルタイム安全性 — process 経路に確保がない');
{
    // (a) ソース検査: process とそこから呼ぶヘルパに確保を生む構文がないこと。
    //     C++ 側の SR-1.x ソース検査に対応する静的チェック。
    const hot = [
        ['process', PitchShifterJS.prototype.process],
        ['_readAt', PitchShifterJS.prototype._readAt],
        ['_startJump', PitchShifterJS.prototype._startJump],
        ['_latchWindowSamples', PitchShifterJS.prototype._latchWindowSamples]
    ];
    const forbidden = [
        [/\bnew\b/, 'new(オブジェクト確保)'],
        [/\bArray\b/, 'Array'],
        [/=>/, 'アロー関数(クロージャ確保)'],
        [/`/, 'テンプレートリテラル'],
        [/\.(push|slice|concat|map|filter|join|split)\s*\(/, '配列メソッド'],
        [/\bJSON\b/, 'JSON'],
        [/(=|return)\s*\{/, 'オブジェクトリテラル'],
        [/(=|return)\s*\[/, '配列リテラル'],
        [/\bthrow\b/, 'throw(例外)']
    ];
    let sourceOk = true;
    const found = [];
    for (const [name, fn] of hot) {
        const src = fn.toString();
        for (const [re, label] of forbidden) {
            if (re.test(src)) {
                sourceOk = false;
                found.push(`${name}: ${label}`);
            }
        }
    }
    check(
        'process/_readAt/_startJump のソースに確保構文がない',
        sourceOk,
        sourceOk ? '検出なし' : '検出: ' + found.join(', ')
    );

    // (b) 実行時検査: 連続処理でヒープ使用量が増えないこと。
    //     1 ブロックあたり 1 オブジェクト(最小 16B)でも確保すれば閾値を超える。
    const fs = 48000;
    const s = makeShifter(fs, -89, DEFAULT_CROSSFADE_MS);
    const io0 = s.ioPtr(0);
    const io1 = s.ioPtr(1);
    const omega = (2 * Math.PI * 440) / fs;
    for (let i = 0; i < BLOCK; i++) {
        const v = SIGNAL_AMPLITUDE * Math.sin(omega * i);
        io0[i] = v;
        io1[i] = v;
    }
    const blocks = 100000; // 跳躍(約 190ms ごと)を 1000 回以上含む
    for (let k = 0; k < 5000; k++) {
        s.process(BLOCK); // ウォームアップ
    }
    const before = process.memoryUsage().heapUsed;
    for (let k = 0; k < blocks; k++) {
        s.process(BLOCK);
    }
    const delta = process.memoryUsage().heapUsed - before;
    const budget = 256 * 1024; // 1 ブロック 1 オブジェクトなら 1.6MB 以上増える
    check(
        `${blocks} ブロック連続処理でヒープが増えない`,
        delta < budget,
        `Δ heapUsed = ${(delta / 1024).toFixed(1)} KiB (閾値 ${(budget / 1024).toFixed(0)} KiB)`
    );
}

/* --- T14: C++ 正本との数値一致(移植ミスの検出) --- */
section('T14 C++ 正本との一致 — 遅延サンプル数と 110Hz 精度');
{
    // README.md「実測値」: 228 サンプル @44.1k / 248 サンプル @48k(いずれも 5.17ms)
    const expectedFirst = { 44100: 228, 48000: 248 };
    for (const fs of SAMPLE_RATES) {
        const r = runImpulse(fs, 0.2, DEFAULT_CROSSFADE_MS);
        let first = -1;
        for (let i = 0; i < r.out.length; i++) {
            if (Math.abs(r.out[i]) > LATENCY_THRESHOLD) {
                first = i;
                break;
            }
        }
        check(
            `fs=${fs} — 実測遅延が C++ 実測値 ${expectedFirst[fs]} サンプルと一致(±2)`,
            Math.abs(first - expectedFirst[fs]) <= 2,
            `JS ${first} サンプル = ${((first / fs) * 1000).toFixed(3)}ms`
        );
    }
    // README.md: 110Hz の誤差は C++ で +0.93 / -1.55 cents。移植ミスなら数十 cents ずれる。
    for (const fs of SAMPLE_RATES) {
        const res = runSine({
            freq: 110,
            fs,
            seconds: 2.0,
            centsL: -89,
            centsR: -89,
            dryWet: 1,
            crossfadeMs: DEFAULT_CROSSFADE_MS
        });
        const est = steadyRatio(res.left, fs, 110);
        const cents = centsError(est.ratio);
        check(
            `fs=${fs} f=110Hz — 誤差が C++ と同程度(|誤差| ≤ 3 cents)`,
            Math.abs(cents) <= 3,
            `${cents.toFixed(2)} cents (ratio=${est.ratio.toFixed(6)})`
        );
    }
}

/* ------------------------------ 集計 ------------------------------ */

console.log(`\n合計: ${passes} PASS / ${failures} FAIL`);
if (failures > 0) {
    process.exitCode = 1;
}
