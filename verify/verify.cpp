// prism オフライン数値検証ランナー(u2-verification)
//
// 判定ルールは construction/u2-verification/functional-design/rules.md(BR2.1..BR2.5)、
// 走査手順は同 functional-spec.md(WF-1..WF-6)、
// 非機能要件は同 nfr-requirements/{security-requirements,tech-stack-decisions}.md(SR-1..SR-4)。
//
// 外部依存ゼロ(C++17 標準ライブラリのみ)。FFT は自前 radix-2(検証側限定 —
// 音声経路には FFT を置かない、FR-3.1)。ファイル I/O・ネットワーク・乱数は使わない。

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "prism/PitchShifter.h"

// ============================================================================
// SR-4.2 確保カウンタ: グローバル operator new/delete を差し替え、計測区間の
// 境界で差分 0 を検証する(逐次ランナーのため非 atomic な単一カウンタで足りる)。
// ============================================================================
namespace {
std::size_t g_allocCount = 0;
}  // namespace

void* operator new(std::size_t n) {
    ++g_allocCount;
    if (n == 0) {
        n = 1;
    }
    void* p = std::malloc(n);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

// ============================================================================
// SR-3.1 判定閾値は名前付き定数として単一定義する(リテラルの散在を禁止)
// ============================================================================
constexpr double kExpectedRatio = 0.95;          // BR2.1: 418/440
// BR2.1 の ±0.5% は拡張した定義域(±1200 セント)の全点に同じ厳しさで適用する。
// ±1200 でも実測誤差は 2 cents 未満(= 0.1%)で、緩和する必要がなかった。
constexpr double kPitchRelTolerance = 0.005;     // BR2.1: ±0.5%
constexpr double kLatencyBudgetMs = 10.0;        // BR2.2 / NFR-1
constexpr double kLatencyThreshold = 0.05;       // BR2.2: -26 dBFS
constexpr double kLatencyDesignSlackSamples = 8.0;  // BR2.2: 設計値一致許容差の定数項
constexpr double kGlitchSlopeFactor = 3.0;       // BR2.3: k = 3.0
constexpr double kGlitchWarmupSec = 0.250;       // BR2.3: warmup 250ms 除外
constexpr int kFftSize = 32768;                  // tech-stack-decisions: N=32768
constexpr int kBlockFrames = 512;                // BR2.4: 512 フレームブロック
constexpr double kSignalAmplitude = 0.5;
constexpr double kGlitchFreqHz = 440.0;
constexpr double kGlitchDurationSec = 5.0;
constexpr double kCpuDurationSec = 5.0;
constexpr double kPi = 3.14159265358979323846;

const double kPitchFreqs[3] = {110.0, 440.0, 3520.0};
const double kSampleRates[2] = {44100.0, 48000.0};

// 拡張したシフト定義域(±1200 セント)の走査点。440Hz で全点、110/3520Hz で
// kBandCents の 3 点を測る(BR2.1 の判定式・許容差はそのまま適用する)。
const double kShiftMatrixCents[6] = {-1200.0, -200.0, -100.0, 100.0, 200.0, 1200.0};
const double kBandCents[3] = {100.0, -1200.0, 1200.0};
const double kBandFreqs[2] = {110.0, 3520.0};

// ============================================================================
// 自前 radix-2 FFT(オフライン限定、FR-3.1)
// ============================================================================
void fftRadix2(std::vector<double>& re, std::vector<double>& im) {
    const std::size_t n = re.size();
    // ビット反転並べ替え
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const double wr = std::cos(ang);
        const double wi = std::sin(ang);
        for (std::size_t i = 0; i < n; i += len) {
            double cr = 1.0;
            double ci = 0.0;
            for (std::size_t k = 0; k < len / 2; ++k) {
                const double ur = re[i + k];
                const double ui = im[i + k];
                const double vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const double vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr;
                im[i + k + len / 2] = ui - vi;
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

// ハン窓 + FFT + 放物線補間による支配ピーク周波数(Hz)
double dominantFrequency(const std::vector<float>& signal, std::size_t offset, double fs) {
    std::vector<double> re(static_cast<std::size_t>(kFftSize), 0.0);
    std::vector<double> im(static_cast<std::size_t>(kFftSize), 0.0);
    for (int i = 0; i < kFftSize; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / static_cast<double>(kFftSize));
        re[static_cast<std::size_t>(i)] =
            static_cast<double>(signal[offset + static_cast<std::size_t>(i)]) * w;
    }
    fftRadix2(re, im);

    std::size_t peak = 1;
    double peakMag = 0.0;
    for (std::size_t k = 1; k < static_cast<std::size_t>(kFftSize) / 2; ++k) {
        const double mag = std::sqrt(re[k] * re[k] + im[k] * im[k]);
        if (mag > peakMag) {
            peakMag = mag;
            peak = k;
        }
    }
    // 放物線補間(ピークは常に 1 <= peak < N/2 なので前後の bin は必ず存在する)
    const double magAt = peakMag;
    const double magPrev = std::sqrt(re[peak - 1] * re[peak - 1] + im[peak - 1] * im[peak - 1]);
    const double magNext = std::sqrt(re[peak + 1] * re[peak + 1] + im[peak + 1] * im[peak + 1]);
    double delta = 0.0;
    const double denom = magPrev - 2.0 * magAt + magNext;
    if (std::fabs(denom) > 0.0) {
        delta = 0.5 * (magPrev - magNext) / denom;
    }
    if (delta > 0.5) {
        delta = 0.5;
    } else if (delta < -0.5) {
        delta = -0.5;
    }
    return (static_cast<double>(peak) + delta) * fs / static_cast<double>(kFftSize);
}

// ============================================================================
// VerificationReport(entities.md)
// ============================================================================
struct Report {
    std::string caseName;
    std::string metric;
    double measured = 0.0;
    double expected = 0.0;
    double tolerance = 0.0;
    bool hasTolerance = true;
    bool passed = true;
    bool countsTowardExit = true;
    std::size_t allocDelta = 0;
    std::string note;
};

std::vector<Report> g_reports;

// 出力蓄積バッファ(計測区間の外で最終サイズまで確保する、SR-4.1)
struct Scratch {
    std::vector<float> inL, inR, outL, outR;

    void allocate(std::size_t frames) {
        inL.assign(frames, 0.0f);
        inR.assign(frames, 0.0f);
        outL.assign(frames, 0.0f);
        outR.assign(frames, 0.0f);
    }
};

// 計測区間: ブロック単位で process を回す。区間内で確保が起きないことを assert(SR-4.2)。
std::size_t runBlocks(prism::PitchShifter& ps, Scratch& s, std::size_t frames,
                      const char* caseName, double* elapsedSecOut) {
    const float* inPtrs[2] = {s.inL.data(), s.inR.data()};
    float* outPtrs[2] = {s.outL.data(), s.outR.data()};
    const std::size_t before = g_allocCount;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t pos = 0; pos < frames; pos += static_cast<std::size_t>(kBlockFrames)) {
        const int n = static_cast<int>(
            std::min(static_cast<std::size_t>(kBlockFrames), frames - pos));
        const float* in[2] = {inPtrs[0] + pos, inPtrs[1] + pos};
        float* out[2] = {outPtrs[0] + pos, outPtrs[1] + pos};
        ps.process(in, out, n);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const std::size_t delta = g_allocCount - before;
    if (elapsedSecOut != nullptr) {
        *elapsedSecOut = std::chrono::duration<double>(t1 - t0).count();
    }
    if (delta != 0) {
        std::cerr << "FATAL: allocation inside process() region: case=" << caseName
                  << " allocations=" << delta << "\n";
    }
    assert(delta == 0 && "process() must not allocate (SR-4.2)");
    return delta;
}

void fillSine(std::vector<float>& buf, double freq, double fs, double amp) {
    const double w = 2.0 * kPi * freq / fs;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<float>(amp * std::sin(w * static_cast<double>(i)));
    }
}

bool preparedOrReport(prism::PitchShifter& ps, double fs, const std::string& caseName) {
    if (ps.isPrepared()) {
        return true;
    }
    std::cerr << "FATAL: prepare() failed for fs=" << fs << " (case " << caseName << ")\n";
    return false;
}

// ---------------------------------------------------------------------------
// BR2.1 ピッチ精度
// ---------------------------------------------------------------------------
void testPitch(double fs) {
    const std::size_t warmup = static_cast<std::size_t>(fs * 0.6);
    const std::size_t frames = warmup + static_cast<std::size_t>(kFftSize);
    Scratch s;
    s.allocate(frames);

    for (int fi = 0; fi < 3; ++fi) {
        const double f = kPitchFreqs[fi];
        char name[64];
        std::snprintf(name, sizeof(name), "pitch@%.0f/%.0fHz", fs, f);
        Report r;
        r.caseName = name;
        r.metric = "ratio";
        r.expected = kExpectedRatio;
        r.tolerance = kExpectedRatio * kPitchRelTolerance;

        prism::PitchShifter ps;
        if (!ps.prepare(fs, kBlockFrames) || !preparedOrReport(ps, fs, r.caseName)) {
            r.passed = false;
            r.note = "prepare failed";
            g_reports.push_back(r);
            continue;
        }
        fillSine(s.inL, f, fs, kSignalAmplitude);
        s.inR = s.inL;  // BR1.8: モノラル信号の L=R 複製は呼び出し側の責務
        r.allocDelta = runBlocks(ps, s, frames, r.caseName.c_str(), nullptr);

        const double fOut = dominantFrequency(s.outL, warmup, fs);
        r.measured = fOut / f;
        r.passed = std::fabs(r.measured - kExpectedRatio) <= r.tolerance;
        char note[96];
        std::snprintf(note, sizeof(note), "f_out=%.3fHz", fOut);
        r.note = note;
        g_reports.push_back(r);
    }
}

// ---------------------------------------------------------------------------
// BR2.1 拡張: 任意のシフト量でのピッチ精度(上げ方向を含む)
// 期待比は常に 2^(cents/1200)。半音 = 100 セントの決め打ちはしない(BR1.1)。
// ---------------------------------------------------------------------------
void testPitchAt(double fs, double f, double cents, Scratch& s, std::size_t warmup,
                 std::size_t frames) {
    char name[64];
    std::snprintf(name, sizeof(name), "pitch@%.0f/%.0fHz/%+.0fc", fs, f, cents);
    Report r;
    r.caseName = name;
    r.metric = "ratio";
    r.expected = std::exp2(cents / 1200.0);
    r.tolerance = r.expected * kPitchRelTolerance;

    prism::PitchShifter ps;
    if (!ps.prepare(fs, kBlockFrames) || !preparedOrReport(ps, fs, r.caseName)) {
        r.passed = false;
        r.note = "prepare failed";
        g_reports.push_back(r);
        return;
    }
    ps.setShiftCentsL(static_cast<float>(cents));
    ps.setShiftCentsR(static_cast<float>(cents));
    ps.reset();  // 平滑器を整定させ、走査域の中央から始める

    fillSine(s.inL, f, fs, kSignalAmplitude);
    s.inR = s.inL;  // BR1.8: モノラル信号の L=R 複製は呼び出し側の責務
    r.allocDelta = runBlocks(ps, s, frames, r.caseName.c_str(), nullptr);

    const double fOut = dominantFrequency(s.outL, warmup, fs);
    r.measured = fOut / f;
    r.passed = std::fabs(r.measured - r.expected) <= r.tolerance;
    char note[96];
    std::snprintf(note, sizeof(note), "f_out=%.3fHz err=%+.2f cents", fOut,
                  1200.0 * std::log2(r.measured / r.expected));
    r.note = note;
    g_reports.push_back(r);
}

void testShiftRange(double fs) {
    const std::size_t warmup = static_cast<std::size_t>(fs * 0.6);
    const std::size_t frames = warmup + static_cast<std::size_t>(kFftSize);
    Scratch s;
    s.allocate(frames);

    for (int i = 0; i < 6; ++i) {
        testPitchAt(fs, kPitchFreqs[1], kShiftMatrixCents[i], s, warmup, frames);  // 440Hz
    }
    for (int fi = 0; fi < 2; ++fi) {
        for (int ci = 0; ci < 3; ++ci) {
            testPitchAt(fs, kBandFreqs[fi], kBandCents[ci], s, warmup, frames);
        }
    }
}

// ---------------------------------------------------------------------------
// BR2.3 拡張: 上げ方向のグリッチ
// 閾値は出力側の最大スロープ基準(= 3.0 x 2pi x f x ratio x A / fs)。既定 -89 の
// 既存ケース(testGlitch)は入力側基準の式のまま据え置き、判定を緩めも締めもしない。
// ---------------------------------------------------------------------------
void testGlitchAt(double fs, double cents) {
    const std::size_t frames = static_cast<std::size_t>(fs * kGlitchDurationSec);
    Scratch s;
    s.allocate(frames);

    char name[64];
    std::snprintf(name, sizeof(name), "glitch@%.0f/%+.0fc", fs, cents);
    Report r;
    r.caseName = name;
    r.metric = "discontinuities";
    r.expected = 0.0;
    r.tolerance = 0.0;

    prism::PitchShifter ps;
    if (!ps.prepare(fs, kBlockFrames) || !preparedOrReport(ps, fs, r.caseName)) {
        r.passed = false;
        r.note = "prepare failed";
        g_reports.push_back(r);
        return;
    }
    ps.setShiftCentsL(static_cast<float>(cents));
    ps.setShiftCentsR(static_cast<float>(cents));
    ps.reset();
    fillSine(s.inL, kGlitchFreqHz, fs, kSignalAmplitude);
    s.inR = s.inL;
    r.allocDelta = runBlocks(ps, s, frames, r.caseName.c_str(), nullptr);

    const double ratio = std::exp2(cents / 1200.0);
    const double maxSlope = 2.0 * kPi * kGlitchFreqHz * ratio * kSignalAmplitude / fs;
    const double limit = kGlitchSlopeFactor * maxSlope;
    const std::size_t skip = static_cast<std::size_t>(fs * kGlitchWarmupSec);
    long count = 0;
    double worst = 0.0;
    for (std::size_t i = skip + 1; i < frames; ++i) {
        const double d = std::fabs(static_cast<double>(s.outL[i]) -
                                   static_cast<double>(s.outL[i - 1]));
        if (d > worst) {
            worst = d;
        }
        if (d > limit) {
            ++count;
        }
    }
    r.measured = static_cast<double>(count);
    r.passed = (count == 0);
    char note[96];
    std::snprintf(note, sizeof(note), "max|dy|=%.5f limit=%.5f", worst, limit);
    r.note = note;
    g_reports.push_back(r);
}

// ---------------------------------------------------------------------------
// BR2.2 レイテンシ
// ---------------------------------------------------------------------------
void testLatency(double fs) {
    const std::size_t frames = static_cast<std::size_t>(fs * 0.2);
    Scratch s;
    s.allocate(frames);

    char name[64];
    std::snprintf(name, sizeof(name), "latency@%.0f", fs);
    Report r;
    r.caseName = name;
    r.metric = "samples";

    prism::PitchShifter ps;
    if (!ps.prepare(fs, kBlockFrames) || !preparedOrReport(ps, fs, r.caseName)) {
        r.passed = false;
        r.note = "prepare failed";
        g_reports.push_back(r);
        return;
    }
    ps.reset();
    std::fill(s.inL.begin(), s.inL.end(), 0.0f);
    s.inL[0] = 1.0f;  // 振幅 1.0 の単位インパルス(sample 0)
    s.inR = s.inL;

    const double designSamples = ps.getLatencySamples();
    const double window = static_cast<double>(ps.getWindowSamples());
    const double ratio = std::exp2(static_cast<double>(prism::PitchShifter::kShiftCentsDefault) /
                                  1200.0);
    r.allocDelta = runBlocks(ps, s, frames, r.caseName.c_str(), nullptr);

    long firstIndex = -1;
    for (std::size_t i = 0; i < frames; ++i) {
        if (std::fabs(static_cast<double>(s.outL[i])) > kLatencyThreshold) {
            firstIndex = static_cast<long>(i);
            break;
        }
    }
    const double tolerance = (1.0 - ratio) * window * 0.5 + kLatencyDesignSlackSamples;
    r.expected = designSamples;
    r.tolerance = tolerance;
    if (firstIndex < 0) {
        r.measured = -1.0;
        r.passed = false;
        r.note = "no output above threshold";
    } else {
        r.measured = static_cast<double>(firstIndex);
        const double ms = r.measured / fs * 1000.0;
        const bool withinBudget = ms <= kLatencyBudgetMs;
        const bool matchesDesign = std::fabs(r.measured - designSamples) <= tolerance;
        r.passed = withinBudget && matchesDesign;
        char note[96];
        std::snprintf(note, sizeof(note), "%.3fms (budget %.1fms), design=%.1f", ms,
                      kLatencyBudgetMs, designSamples);
        r.note = note;
    }
    g_reports.push_back(r);
}

// ---------------------------------------------------------------------------
// BR2.3 グリッチ
// ---------------------------------------------------------------------------
void testGlitch(double fs) {
    const std::size_t frames = static_cast<std::size_t>(fs * kGlitchDurationSec);
    Scratch s;
    s.allocate(frames);

    char name[64];
    std::snprintf(name, sizeof(name), "glitch@%.0f", fs);
    Report r;
    r.caseName = name;
    r.metric = "discontinuities";
    r.expected = 0.0;
    r.tolerance = 0.0;

    prism::PitchShifter ps;
    if (!ps.prepare(fs, kBlockFrames) || !preparedOrReport(ps, fs, r.caseName)) {
        r.passed = false;
        r.note = "prepare failed";
        g_reports.push_back(r);
        return;
    }
    fillSine(s.inL, kGlitchFreqHz, fs, kSignalAmplitude);
    s.inR = s.inL;
    r.allocDelta = runBlocks(ps, s, frames, r.caseName.c_str(), nullptr);

    const double maxSlope = 2.0 * kPi * kGlitchFreqHz * kSignalAmplitude / fs;
    const double limit = kGlitchSlopeFactor * maxSlope;
    const std::size_t skip = static_cast<std::size_t>(fs * kGlitchWarmupSec);
    long count = 0;
    double worst = 0.0;
    for (std::size_t i = skip + 1; i < frames; ++i) {
        const double d = std::fabs(static_cast<double>(s.outL[i]) -
                                   static_cast<double>(s.outL[i - 1]));
        if (d > worst) {
            worst = d;
        }
        if (d > limit) {
            ++count;
        }
    }
    r.measured = static_cast<double>(count);
    r.passed = (count == 0);
    char note[96];
    std::snprintf(note, sizeof(note), "max|dy|=%.5f limit=%.5f", worst, limit);
    r.note = note;
    g_reports.push_back(r);
}

// ---------------------------------------------------------------------------
// BR2.4 CPU 比(報告のみ、終了コードに影響しない)
// ---------------------------------------------------------------------------
void testCpu(double fs, double cents, bool named) {
    const std::size_t frames = static_cast<std::size_t>(fs * kCpuDurationSec);
    Scratch s;
    s.allocate(frames);

    char name[64];
    if (named) {
        std::snprintf(name, sizeof(name), "cpu@%.0f/%+.0fc", fs, cents);
    } else {
        std::snprintf(name, sizeof(name), "cpu@%.0f", fs);
    }
    Report r;
    r.caseName = name;
    r.metric = "cpuRatio";
    r.hasTolerance = false;
    r.countsTowardExit = true;  // prepare 失敗時のみ false になりうる

    prism::PitchShifter ps;
    if (!ps.prepare(fs, kBlockFrames) || !preparedOrReport(ps, fs, r.caseName)) {
        r.passed = false;
        r.note = "prepare failed";
        g_reports.push_back(r);
        return;
    }
    ps.setShiftCentsL(static_cast<float>(cents));
    ps.setShiftCentsR(static_cast<float>(cents));
    ps.reset();
    fillSine(s.inL, kGlitchFreqHz, fs, kSignalAmplitude);
    s.inR = s.inL;
    double elapsed = 0.0;
    r.allocDelta = runBlocks(ps, s, frames, r.caseName.c_str(), &elapsed);

    const double realTime = static_cast<double>(frames) / fs;
    r.measured = elapsed / realTime;
    r.passed = true;  // BR2.4: 報告のみ
    r.countsTowardExit = false;
    char note[128];
    std::snprintf(note, sizeof(note),
                  "report only / environment dependent (no threshold), %.0f blocks of %d",
                  std::ceil(static_cast<double>(frames) / kBlockFrames), kBlockFrames);
    r.note = note;
    g_reports.push_back(r);
}

// ---------------------------------------------------------------------------
// 契約・境界条件(construction ガードレール「happy path + エラー/境界 2 件以上」)
// BR2.1..BR2.5 の判定式は変更せず、横断的な追加検査としてのみ加える。
// ---------------------------------------------------------------------------
double measureRatio(prism::PitchShifter& ps, Scratch& s, double f, double fs,
                    std::size_t warmup, std::size_t frames, const char* caseName) {
    fillSine(s.inL, f, fs, kSignalAmplitude);
    s.inR = s.inL;
    runBlocks(ps, s, frames, caseName, nullptr);
    return dominantFrequency(s.outL, warmup, fs) / f;
}

void testContract(double fs) {
    const std::size_t warmup = static_cast<std::size_t>(fs * 0.6);
    const std::size_t frames = warmup + static_cast<std::size_t>(kFftSize);
    Scratch s;
    s.allocate(frames);
    const double f = kPitchFreqs[1];  // 440 Hz

    // C1: prepare() が不正な引数を拒否すること(fs 範囲外 / 非有限 / ブロック長 0)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "contract@%.0f/prepare-rejects", fs);
        Report r;
        r.caseName = name;
        r.metric = "rejected/4";
        r.expected = 4.0;
        r.tolerance = 0.0;
        int rejected = 0;
        prism::PitchShifter a;
        prism::PitchShifter b;
        prism::PitchShifter c;
        prism::PitchShifter d;
        rejected += a.prepare(1000.0, kBlockFrames) ? 0 : 1;                    // fs 下限未満
        rejected += b.prepare(200000.0, kBlockFrames) ? 0 : 1;                  // fs 上限超過
        rejected += c.prepare(std::nan(""), kBlockFrames) ? 0 : 1;              // 非有限 fs
        rejected += d.prepare(fs, 0) ? 0 : 1;                                   // ブロック長 0
        r.measured = static_cast<double>(rejected);
        r.passed = (rejected == 4);
        r.note = "invalid prepare() args must return false";
        g_reports.push_back(r);
    }

    // C2: 定義域外のシフト量が下限 -1200 セントへクランプされること(BR1.2)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "contract@%.0f/clamp-shift", fs);
        Report r;
        r.caseName = name;
        r.metric = "ratio";
        r.expected = std::exp2(static_cast<double>(prism::PitchShifter::kShiftCentsMin) / 1200.0);
        r.tolerance = r.expected * kPitchRelTolerance;
        prism::PitchShifter ps;
        if (!ps.prepare(fs, kBlockFrames) || !preparedOrReport(ps, fs, r.caseName)) {
            r.passed = false;
            r.note = "prepare failed";
            g_reports.push_back(r);
        } else {
            ps.setShiftCentsL(-5000.0f);  // 範囲外 -> kShiftCentsMin にクランプ
            ps.setShiftCentsR(-5000.0f);
            ps.reset();
            r.measured = measureRatio(ps, s, f, fs, warmup, frames, r.caseName.c_str());
            r.passed = std::fabs(r.measured - r.expected) <= r.tolerance;
            char note[96];
            std::snprintf(note, sizeof(note), "setShiftCents(-5000) -> %.0f cents",
                          static_cast<double>(prism::PitchShifter::kShiftCentsMin));
            r.note = note;
            g_reports.push_back(r);
        }
    }

    // C2b: 定義域外のシフト量が上限 +1200 セントへクランプされること(BR1.2、上げ方向)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "contract@%.0f/clamp-shift-up", fs);
        Report r;
        r.caseName = name;
        r.metric = "ratio";
        r.expected = std::exp2(static_cast<double>(prism::PitchShifter::kShiftCentsMax) / 1200.0);
        r.tolerance = r.expected * kPitchRelTolerance;
        prism::PitchShifter ps;
        if (!ps.prepare(fs, kBlockFrames) || !preparedOrReport(ps, fs, r.caseName)) {
            r.passed = false;
            r.note = "prepare failed";
            g_reports.push_back(r);
        } else {
            ps.setShiftCentsL(5000.0f);  // 範囲外 -> kShiftCentsMax にクランプ
            ps.setShiftCentsR(5000.0f);
            ps.reset();
            r.measured = measureRatio(ps, s, f, fs, warmup, frames, r.caseName.c_str());
            r.passed = std::fabs(r.measured - r.expected) <= r.tolerance;
            char note[96];
            std::snprintf(note, sizeof(note), "setShiftCents(+5000) -> %.0f cents",
                          static_cast<double>(prism::PitchShifter::kShiftCentsMax));
            r.note = note;
            g_reports.push_back(r);
        }
    }

    // C6: 上げ方向でも設計値遅延が NFR-1 の 10ms 予算に収まること(ガード帯を含む)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "contract@%.0f/latency-budget-up", fs);
        Report r;
        r.caseName = name;
        r.metric = "ms";
        r.expected = 0.0;
        r.hasTolerance = false;
        prism::PitchShifter ps;
        if (!ps.prepare(fs, kBlockFrames) || !preparedOrReport(ps, fs, r.caseName)) {
            r.passed = false;
            r.note = "prepare failed";
            g_reports.push_back(r);
        } else {
            ps.setShiftCentsL(prism::PitchShifter::kShiftCentsMax);
            ps.setShiftCentsR(prism::PitchShifter::kShiftCentsMax);
            const double ms = ps.getLatencySamples() / fs * 1000.0;
            r.measured = ms;
            r.passed = (ms > 0.0) && (ms <= kLatencyBudgetMs);
            char note[96];
            std::snprintf(note, sizeof(note), "+1200c design latency %.1f samples (budget %.1fms)",
                          ps.getLatencySamples(), kLatencyBudgetMs);
            r.note = note;
            g_reports.push_back(r);
        }
    }

    // C3: 非有限値のセッター入力が無視されること(SR-3.2)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "contract@%.0f/nan-ignored", fs);
        Report r;
        r.caseName = name;
        r.metric = "ratio";
        r.expected = kExpectedRatio;
        r.tolerance = kExpectedRatio * kPitchRelTolerance;
        prism::PitchShifter ps;
        if (!ps.prepare(fs, kBlockFrames) || !preparedOrReport(ps, fs, r.caseName)) {
            r.passed = false;
            r.note = "prepare failed";
            g_reports.push_back(r);
        } else {
            ps.setShiftCentsL(std::nan(""));  // 無視され既定値 -89 のまま
            ps.setShiftCentsR(std::numeric_limits<float>::infinity());
            ps.reset();
            r.measured = measureRatio(ps, s, f, fs, warmup, frames, r.caseName.c_str());
            r.passed = std::fabs(r.measured - r.expected) <= r.tolerance;
            r.note = "NaN/Inf setter input must keep the previous value";
            g_reports.push_back(r);
        }
    }

    // C4: dryWet=0 で原音そのままが出ること(FR-1.3)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "contract@%.0f/drywet-zero", fs);
        Report r;
        r.caseName = name;
        r.metric = "max|out-in|";
        r.expected = 0.0;
        r.tolerance = 1.0e-6;
        prism::PitchShifter ps;
        if (!ps.prepare(fs, kBlockFrames) || !preparedOrReport(ps, fs, r.caseName)) {
            r.passed = false;
            r.note = "prepare failed";
            g_reports.push_back(r);
        } else {
            ps.setDryWet(0.0f);
            ps.reset();
            fillSine(s.inL, f, fs, kSignalAmplitude);
            s.inR = s.inL;
            runBlocks(ps, s, frames, r.caseName.c_str(), nullptr);
            double worst = 0.0;
            for (std::size_t i = warmup; i < frames; ++i) {
                const double d = std::fabs(static_cast<double>(s.outL[i]) -
                                           static_cast<double>(s.inL[i]));
                if (d > worst) {
                    worst = d;
                }
            }
            r.measured = worst;
            r.passed = worst <= r.tolerance;
            r.note = "dryWet=0 must pass the input through unchanged";
            g_reports.push_back(r);
        }
    }
}

void printMatrix() {
    std::printf("\n");
    std::printf("%-6s %-30s %-16s %14s %14s %14s %8s  %s\n", "result", "case", "metric",
                "measured", "expected", "tolerance", "alloc", "note");
    std::printf(
        "------ ------------------------------ ---------------- -------------- -------------- "
        "-------------- --------  ----\n");
    for (const Report& r : g_reports) {
        char expected[32];
        char tolerance[32];
        if (r.hasTolerance) {
            std::snprintf(expected, sizeof(expected), "%.6f", r.expected);
            std::snprintf(tolerance, sizeof(tolerance), "%.6f", r.tolerance);
        } else {
            std::snprintf(expected, sizeof(expected), "%s", "-");
            std::snprintf(tolerance, sizeof(tolerance), "%s", "null");
        }
        char alloc[16];
        std::snprintf(alloc, sizeof(alloc), "alloc=%zu", r.allocDelta);
        std::printf("%-6s %-30s %-16s %14.6f %14s %14s %8s  %s\n", r.passed ? "PASS" : "FAIL",
                    r.caseName.c_str(), r.metric.c_str(), r.measured, expected, tolerance, alloc,
                    r.note.c_str());
    }
}

}  // namespace

int main() {
    std::printf("prism verification harness (u2-verification)\n");
    std::printf("fs matrix: 44100 / 48000 Hz, tests: pitch / latency / glitch / cpu (+ contract edge cases)\n");
    std::printf("shift matrix: %.0f..%.0f cents (ratio = 2^(cents/1200)), tolerance +/-0.5%% everywhere\n",
                static_cast<double>(prism::PitchShifter::kShiftCentsMin),
                static_cast<double>(prism::PitchShifter::kShiftCentsMax));
    std::printf("defaults: shift=%.0f cents, dryWet=%.1f, crossfade=%.0f ms, block=%d frames\n",
                static_cast<double>(prism::PitchShifter::kShiftCentsDefault),
                static_cast<double>(prism::PitchShifter::kDryWetDefault),
                static_cast<double>(prism::PitchShifter::kCrossfadeMsDefault), kBlockFrames);

    for (int i = 0; i < 2; ++i) {
        const double fs = kSampleRates[i];
        testPitch(fs);
        testShiftRange(fs);
        testLatency(fs);
        testGlitch(fs);
        testGlitchAt(fs, 100.0);
        testGlitchAt(fs, 1200.0);
        testCpu(fs, static_cast<double>(prism::PitchShifter::kShiftCentsDefault), false);
        // 跳躍頻度は |1-ratio| に比例するため、相関探索の負荷は定義域の端が最悪になる。
        testCpu(fs, static_cast<double>(prism::PitchShifter::kShiftCentsMin), true);
        testCpu(fs, static_cast<double>(prism::PitchShifter::kShiftCentsMax), true);
        testContract(fs);
    }

    printMatrix();

    int failures = 0;
    for (const Report& r : g_reports) {
        if (r.countsTowardExit && !r.passed) {
            ++failures;
        }
    }
    std::printf("\n%d/%zu checks passed (%d failing, cpu cases are report-only)\n",
                static_cast<int>(g_reports.size()) - failures, g_reports.size(), failures);
    if (failures == 0) {
        std::printf("RESULT: ALL GREEN\n");
        return 0;
    }
    std::printf("RESULT: FAILED\n");
    return 1;
}
