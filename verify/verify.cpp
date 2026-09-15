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

#include "prism/PhaseVocoderShifter.h"
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
// 捕獲経路(Android の再生音キャプチャ)の走査幅。生音の漏れ込みが無いため
// NFR-1 の 10ms 予算は適用されず、跳躍間隔(= sweep / |1-比|)を優先して広く取る。
constexpr double kCaptureSweepMs = 40.0;

const double kPitchFreqs[3] = {110.0, 440.0, 3520.0};
const double kSampleRates[2] = {44100.0, 48000.0};

// 拡張したシフト定義域(±1200 セント)の走査点。440Hz で全点、110/3520Hz で
// kBandCents の 3 点を測る(BR2.1 の判定式・許容差はそのまま適用する)。
const double kShiftMatrixCents[6] = {-1200.0, -200.0, -100.0, 100.0, 200.0, 1200.0};
const double kBandCents[3] = {100.0, -1200.0, 1200.0};
const double kBandFreqs[2] = {110.0, 3520.0};

// ---------------------------------------------------------------------------
// 第 2 方式(prism::PhaseVocoderShifter)専用の閾値・信号定義。
// 既存の BR2.1..BR2.5 の定数には一切触れない。
// ---------------------------------------------------------------------------
// 捕獲経路の遅延予算。生音の漏れ込みが無いので NFR-1 の 10ms ではなく 100ms。
constexpr double kPvLatencyBudgetMs = 100.0;
// 遅延の実測許容差は hop/4(= N/16 サンプル)。
constexpr int kPvLatencyToleranceDiv = 4;
// 遅延実測用のトーンバースト(無音 -> 440Hz -> 無音)。出力と入力のエネルギー重心の
// 差を群遅延とみなす。インパルスだと位相ボコーダの分散で重心がずれるため使わない。
constexpr double kPvBurstLeadSec = 0.100;
constexpr double kPvBurstToneSec = 0.150;
constexpr double kPvBurstRampSec = 0.001;
constexpr double kPvBurstTotalSec = 0.500;
// 品質指標(report only)。3 音の和音と直線チャープ。
const double kChordFreqs[3] = {220.0, 277.0, 330.0};
constexpr double kChordPartialAmp = 0.5 / 3.0;
// 和音は長い窓(2^18 = 約 6 秒)で見る。ディレイライン型の跳躍・クロスフェードは
// 跳躍間隔(-89 セント / 走査幅 40ms で約 760ms)の遅い変調として出るため、
// 既存検査の 32768 点(約 0.7 秒)では搬送波の主ローブに埋もれて検出できない。
constexpr int kQualityChordFft = 1 << 18;
// チャープは期待位相で複素復調(dechirp)してから 2^16 点で見る。時間-帯域幅の
// 制約で「期待帯域」を直接切ると広くなりすぎるため、期待信号を DC に畳んでから測る。
constexpr int kQualityChirpFft = 1 << 16;
// 期待ピーク周辺として許す bin 幅(ハン窓の主ローブは片側 2 bin)。
constexpr int kQualityGuardBins = 4;
constexpr double kChirpLoHz = 200.0;
constexpr double kChirpHiHz = 2000.0;
constexpr double kChirpSec = 2.0;
constexpr double kChirpAnalysisStartSec = 0.3;
// 復調後に「搬送波近傍」とみなす帯域(これより外は和周波数成分なので分母から除く)。
constexpr double kChirpNearBandHz = 200.0;
// 第 2 方式の走査点。
const double kPvCents[4] = {-89.0, -1200.0, 100.0, 1200.0};
const double kPvLatencyCents[3] = {-89.0, -1200.0, 1200.0};

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
                 std::size_t frames, double sweepMs = prism::PitchShifter::kSweepMs,
                 double crossfadeMs =
                     static_cast<double>(prism::PitchShifter::kCrossfadeMsDefault)) {
    char name[64];
    if (crossfadeMs != static_cast<double>(prism::PitchShifter::kCrossfadeMsDefault)) {
        std::snprintf(name, sizeof(name), "pitch@%.0f/%.0fHz/%+.0fc/cf%.0f", fs, f, cents,
                      crossfadeMs);
    } else if (sweepMs == prism::PitchShifter::kSweepMs) {
        std::snprintf(name, sizeof(name), "pitch@%.0f/%.0fHz/%+.0fc", fs, f, cents);
    } else {
        std::snprintf(name, sizeof(name), "pitch@%.0f/%.0fHz/%+.0fc/sw%.0f", fs, f, cents, sweepMs);
    }
    Report r;
    r.caseName = name;
    r.metric = "ratio";
    r.expected = std::exp2(cents / 1200.0);
    r.tolerance = r.expected * kPitchRelTolerance;

    prism::PitchShifter ps;
    if (!ps.prepare(fs, kBlockFrames, sweepMs) || !preparedOrReport(ps, fs, r.caseName)) {
        r.passed = false;
        r.note = "prepare failed";
        g_reports.push_back(r);
        return;
    }
    ps.setShiftCentsL(static_cast<float>(cents));
    ps.setShiftCentsR(static_cast<float>(cents));
    ps.setCrossfadeMs(static_cast<float>(crossfadeMs));
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
// 走査幅のパラメータ化(捕獲経路 = 40ms)
// prepare() の第 3 引数で走査幅を広げても、BR2.1 の判定式・許容差(±0.5%)を
// そのまま満たすこと、および遅延式 baseOffset + sweep/2 が追従することを確認する。
// ---------------------------------------------------------------------------
void testSweepDesign(double fs, double sweepMs) {
    char name[64];
    std::snprintf(name, sizeof(name), "sweep@%.0f/sw%.0f/latency-design", fs, sweepMs);
    Report r;
    r.caseName = name;
    r.metric = "samples";
    r.tolerance = 0.0;

    prism::PitchShifter ps;
    if (!ps.prepare(fs, kBlockFrames, sweepMs) || !preparedOrReport(ps, fs, r.caseName)) {
        r.passed = false;
        r.note = "prepare failed";
        g_reports.push_back(r);
        return;
    }
    const double sweepSamples = static_cast<double>(ps.getSweepSamples());
    r.expected = static_cast<double>(prism::PitchShifter::kBaseOffsetSamples) + 0.5 * sweepSamples;
    r.measured = ps.getLatencySamples();  // 既定 -89 セント = 下げ方向(ガード帯なし)
    const bool sweepMsMatches = (ps.getSweepMs() == sweepMs);
    r.passed = (r.measured == r.expected) && sweepMsMatches;
    char note[128];
    std::snprintf(note, sizeof(note), "sweep=%.0f samples (%.2fms), getSweepMs=%.1f, %.2fms",
                  sweepSamples, sweepSamples / fs * 1000.0, ps.getSweepMs(),
                  r.measured / fs * 1000.0);
    r.note = note;
    g_reports.push_back(r);
}

void testCaptureSweep(double fs) {
    const std::size_t warmup = static_cast<std::size_t>(fs * 0.6);
    const std::size_t frames = warmup + static_cast<std::size_t>(kFftSize);
    Scratch s;
    s.allocate(frames);

    // (a) 既定 -89 セントのピッチ精度が 110 / 440 / 3520 Hz で保たれること。
    const double defaultCents = static_cast<double>(prism::PitchShifter::kShiftCentsDefault);
    for (int fi = 0; fi < 3; ++fi) {
        testPitchAt(fs, kPitchFreqs[fi], defaultCents, s, warmup, frames, kCaptureSweepMs);
    }
    // (b) 遅延式が走査幅に追従すること。
    testSweepDesign(fs, kCaptureSweepMs);
    // (c) 定義域の端(比 2.0 / 0.5)でも ±0.5% に収まること。
    testPitchAt(fs, kPitchFreqs[1], static_cast<double>(prism::PitchShifter::kShiftCentsMax), s,
                warmup, frames, kCaptureSweepMs);
    testPitchAt(fs, kPitchFreqs[1], static_cast<double>(prism::PitchShifter::kShiftCentsMin), s,
                warmup, frames, kCaptureSweepMs);
}

// ---------------------------------------------------------------------------
// BR2.3 拡張: 上げ方向のグリッチ
// 閾値は出力側の最大スロープ基準(= 3.0 x 2pi x f x ratio x A / fs)。既定 -89 の
// 既存ケース(testGlitch)は入力側基準の式のまま据え置き、判定を緩めも締めもしない。
// ---------------------------------------------------------------------------
void testGlitchAt(double fs, double cents,
                  double crossfadeMs =
                      static_cast<double>(prism::PitchShifter::kCrossfadeMsDefault)) {
    const std::size_t frames = static_cast<std::size_t>(fs * kGlitchDurationSec);
    Scratch s;
    s.allocate(frames);

    char name[64];
    if (crossfadeMs != static_cast<double>(prism::PitchShifter::kCrossfadeMsDefault)) {
        std::snprintf(name, sizeof(name), "glitch@%.0f/%+.0fc/cf%.0f", fs, cents, crossfadeMs);
    } else {
        std::snprintf(name, sizeof(name), "glitch@%.0f/%+.0fc", fs, cents);
    }
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
    ps.setCrossfadeMs(static_cast<float>(crossfadeMs));
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
// クロスフェード窓長の上限(200ms)
// startJump() の runLimit(跳躍量 x 8)を撤廃したことで、既定 -89 セントでも
// 窓長が跳躍間隔(sweep / |1-比| ≈ 190ms)いっぱいまで伸びる。その極端でも
// ピッチ精度(BR2.1)とグリッチゼロ(BR2.3)が保たれることを確認する。
// ---------------------------------------------------------------------------
void testCrossfadeMax(double fs) {
    const std::size_t warmup = static_cast<std::size_t>(fs * 0.6);
    const std::size_t frames = warmup + static_cast<std::size_t>(kFftSize);
    Scratch s;
    s.allocate(frames);

    const double cf = static_cast<double>(prism::PitchShifter::kCrossfadeMsMax);
    const double defaultCents = static_cast<double>(prism::PitchShifter::kShiftCentsDefault);
    for (int fi = 0; fi < 3; ++fi) {
        testPitchAt(fs, kPitchFreqs[fi], defaultCents, s, warmup, frames,
                    prism::PitchShifter::kSweepMs, cf);
    }
    testGlitchAt(fs, defaultCents, cf);
    // 上げ方向はガード帯が予算になるため、窓長を伸ばしても実効長は別式で決まる。
    testGlitchAt(fs, static_cast<double>(prism::PitchShifter::kShiftCentsMax), cf);
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

// ============================================================================
// 第 2 方式: 位相ロック付き位相ボコーダ(prism::PhaseVocoderShifter)
//
// 遅延制約の緩い経路(他アプリ音の捕獲)向けの選択式の方式。判定の骨格は既存と
// 同じ(比 ±0.5% / 遅延の設計値一致 / グリッチゼロ / 確保ゼロ / 決定論)で、
// 遅延予算だけ 10ms -> 100ms に置き換わる。既存の検査には一切触れていない。
// ============================================================================

// 任意のシフタ型でブロック処理を回す。約束は runBlocks と同じ(区間内で確保ゼロ)。
template <class Shifter>
std::size_t runBlocksOn(Shifter& ps, Scratch& s, std::size_t frames, const char* caseName,
                        double* elapsedSecOut) {
    const float* inPtrs[2] = {s.inL.data(), s.inR.data()};
    float* outPtrs[2] = {s.outL.data(), s.outR.data()};
    const std::size_t before = g_allocCount;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t pos = 0; pos < frames; pos += static_cast<std::size_t>(kBlockFrames)) {
        const int n =
            static_cast<int>(std::min(static_cast<std::size_t>(kBlockFrames), frames - pos));
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

bool pvPreparedOrReport(prism::PhaseVocoderShifter& ps, double fs, const std::string& caseName) {
    if (ps.isPrepared()) {
        return true;
    }
    std::cerr << "FATAL: prepare() failed for fs=" << fs << " (case " << caseName << ")\n";
    return false;
}

// ---------------------------------------------------------------------------
// ピッチ精度: 44.1k/48k x 110/440/3520Hz x -89/-1200/+100/+1200 セント
// 期待比は常に 2^(cents/1200)。許容差は既存と同じ ±0.5%。
// ---------------------------------------------------------------------------
void testPvPitch(double fs) {
    const std::size_t warmup = static_cast<std::size_t>(fs * 0.8);
    const std::size_t frames = warmup + static_cast<std::size_t>(kFftSize);
    Scratch s;
    s.allocate(frames);

    for (int fi = 0; fi < 3; ++fi) {
        for (int ci = 0; ci < 4; ++ci) {
            const double f = kPitchFreqs[fi];
            const double cents = kPvCents[ci];
            char name[64];
            std::snprintf(name, sizeof(name), "pv-pitch@%.0f/%.0fHz/%+.0fc", fs, f, cents);
            Report r;
            r.caseName = name;
            r.metric = "ratio";
            r.expected = std::exp2(cents / 1200.0);
            r.tolerance = r.expected * kPitchRelTolerance;

            prism::PhaseVocoderShifter ps;
            if (!ps.prepare(fs, kBlockFrames) || !pvPreparedOrReport(ps, fs, r.caseName)) {
                r.passed = false;
                r.note = "prepare failed";
                g_reports.push_back(r);
                continue;
            }
            ps.setShiftCentsL(static_cast<float>(cents));
            ps.setShiftCentsR(static_cast<float>(cents));
            ps.reset();
            fillSine(s.inL, f, fs, kSignalAmplitude);
            s.inR = s.inL;
            r.allocDelta = runBlocksOn(ps, s, frames, r.caseName.c_str(), nullptr);

            const double fOut = dominantFrequency(s.outL, warmup, fs);
            r.measured = fOut / f;
            r.passed = std::fabs(r.measured - r.expected) <= r.tolerance;
            char note[96];
            std::snprintf(note, sizeof(note), "f_out=%.3fHz err=%+.2f cents", fOut,
                          1200.0 * std::log2(r.measured / r.expected));
            r.note = note;
            g_reports.push_back(r);
        }
    }
}

// ---------------------------------------------------------------------------
// 遅延: getLatencySamples() = P + (N/2)(1/r - 1) と実測の一致(許容差 hop/4)
//
// 実測はトーンバーストのエネルギー重心の移動量。インパルス応答の重心だと、
// 位相ボコーダが位相を書き換えることによる分散で大きなシフト量のとき重心が
// 数百サンプルずれるため、包絡の移動量で群遅延を測る。
// あわせて遅延予算 100ms(捕獲経路)を満たすことも判定する。
// ---------------------------------------------------------------------------
double energyCentroid(const std::vector<float>& x, std::size_t frames) {
    double energy = 0.0;
    double moment = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double e = static_cast<double>(x[i]) * static_cast<double>(x[i]);
        energy += e;
        moment += e * static_cast<double>(i);
    }
    return (energy > 0.0) ? (moment / energy) : -1.0;
}

void fillToneBurst(std::vector<float>& buf, double fs) {
    const double w = 2.0 * kPi * kGlitchFreqHz / fs;
    const std::size_t lead = static_cast<std::size_t>(fs * kPvBurstLeadSec);
    const std::size_t len = static_cast<std::size_t>(fs * kPvBurstToneSec);
    const std::size_t ramp = static_cast<std::size_t>(fs * kPvBurstRampSec);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        double gain = 0.0;
        if (i >= lead && i < lead + len) {
            const std::size_t k = i - lead;
            gain = 1.0;
            if (k < ramp) {
                gain = static_cast<double>(k) / static_cast<double>(ramp);
            } else if (k + ramp > len) {
                gain = static_cast<double>(len - k) / static_cast<double>(ramp);
            }
        }
        buf[i] = static_cast<float>(kSignalAmplitude * gain * std::sin(w * static_cast<double>(i)));
    }
}

void testPvLatency(double fs, double cents) {
    const std::size_t frames = static_cast<std::size_t>(fs * kPvBurstTotalSec);
    Scratch s;
    s.allocate(frames);

    char name[64];
    std::snprintf(name, sizeof(name), "pv-latency@%.0f/%+.0fc", fs, cents);
    Report r;
    r.caseName = name;
    r.metric = "samples";

    prism::PhaseVocoderShifter ps;
    if (!ps.prepare(fs, kBlockFrames) || !pvPreparedOrReport(ps, fs, r.caseName)) {
        r.passed = false;
        r.note = "prepare failed";
        g_reports.push_back(r);
        return;
    }
    ps.setShiftCentsL(static_cast<float>(cents));
    ps.setShiftCentsR(static_cast<float>(cents));
    ps.reset();
    fillToneBurst(s.inL, fs);
    s.inR = s.inL;
    const double design = ps.getLatencySamples();
    r.allocDelta = runBlocksOn(ps, s, frames, r.caseName.c_str(), nullptr);

    const double cIn = energyCentroid(s.inL, frames);
    const double cOut = energyCentroid(s.outL, frames);
    r.expected = design;
    r.tolerance = static_cast<double>(ps.getHopSamples()) / kPvLatencyToleranceDiv;
    if (cIn < 0.0 || cOut < 0.0) {
        r.measured = -1.0;
        r.passed = false;
        r.note = "no energy in input or output";
    } else {
        r.measured = cOut - cIn;
        const double ms = design / fs * 1000.0;
        const bool withinBudget = ms <= kPvLatencyBudgetMs;
        const bool matchesDesign = std::fabs(r.measured - design) <= r.tolerance;
        r.passed = withinBudget && matchesDesign;
        char note[128];
        std::snprintf(note, sizeof(note), "design %.1f = %.2fms (budget %.0fms), N=%d hop=%d", design,
                      ms, kPvLatencyBudgetMs, ps.getFftSize(), ps.getHopSamples());
        r.note = note;
    }
    g_reports.push_back(r);
}

// ---------------------------------------------------------------------------
// グリッチ: 既存と同じ不連続検出(出力側スロープ基準、k = 3.0、warmup 250ms 除外)
// ---------------------------------------------------------------------------
void testPvGlitch(double fs, double cents) {
    const std::size_t frames = static_cast<std::size_t>(fs * kGlitchDurationSec);
    Scratch s;
    s.allocate(frames);

    char name[64];
    std::snprintf(name, sizeof(name), "pv-glitch@%.0f/%+.0fc", fs, cents);
    Report r;
    r.caseName = name;
    r.metric = "discontinuities";
    r.expected = 0.0;
    r.tolerance = 0.0;

    prism::PhaseVocoderShifter ps;
    if (!ps.prepare(fs, kBlockFrames) || !pvPreparedOrReport(ps, fs, r.caseName)) {
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
    r.allocDelta = runBlocksOn(ps, s, frames, r.caseName.c_str(), nullptr);

    const double ratio = std::exp2(cents / 1200.0);
    const double maxSlope = 2.0 * kPi * kGlitchFreqHz * ratio * kSignalAmplitude / fs;
    const double limit = kGlitchSlopeFactor * maxSlope;
    const std::size_t skip = static_cast<std::size_t>(fs * kGlitchWarmupSec);
    long count = 0;
    double worst = 0.0;
    for (std::size_t i = skip + 1; i < frames; ++i) {
        const double d =
            std::fabs(static_cast<double>(s.outL[i]) - static_cast<double>(s.outL[i - 1]));
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
// 品質指標(report only): 期待スペクトル外エネルギー比
//
// 和音 220+277+330Hz を -89 セントすると、理想出力は 3 本の線スペクトルだけになる。
// 期待ピーク周辺 (±4 bin) 以外に現れたエネルギーの比率を artifact とみなす。
// 窓長は 2^18(約 6 秒)。ディレイライン型の跳躍・クロスフェードは跳躍間隔
// (約 760ms)の遅い振幅変調として現れるので、この分解能でないと見えない。
//
// チャープ 200->2000Hz は、期待位相 φ(i) = 2pi*r*(f0*t + slope*t^2/2)/fs
// (t = i - そのシフタの遅延)で複素復調してから 2^16 点の複素 FFT を取る。
// 理想出力は DC の線スペクトルになり、期待からのずれだけが DC 周辺に散らばる。
// 分母は搬送波近傍 ±200Hz に限る(復調で生じる和周波数成分を除くため)。
//
// どちらも両方式を同じ物差しで並べるための指標であり、合否には効かせない。
// ---------------------------------------------------------------------------
void magnitudeSpectrum(const std::vector<float>& signal, std::size_t offset, int n,
                       std::vector<double>& mags) {
    std::vector<double> re(static_cast<std::size_t>(n), 0.0);
    std::vector<double> im(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / static_cast<double>(n));
        re[static_cast<std::size_t>(i)] =
            static_cast<double>(signal[offset + static_cast<std::size_t>(i)]) * w;
    }
    fftRadix2(re, im);
    mags.assign(static_cast<std::size_t>(n) / 2u + 1u, 0.0);
    for (int k = 0; k <= n / 2; ++k) {
        const std::size_t uk = static_cast<std::size_t>(k);
        mags[uk] = std::sqrt(re[uk] * re[uk] + im[uk] * im[uk]);
    }
}

void fillChord(std::vector<float>& buf, double fs) {
    for (std::size_t i = 0; i < buf.size(); ++i) {
        double v = 0.0;
        for (int t = 0; t < 3; ++t) {
            v += kChordPartialAmp *
                 std::sin(2.0 * kPi * kChordFreqs[t] * static_cast<double>(i) / fs);
        }
        buf[i] = static_cast<float>(v);
    }
}

// 直線チャープ(瞬時周波数 f(t) = lo + slope*t、位相はその積分)
void fillChirp(std::vector<float>& buf, double fs) {
    const double slope = (kChirpHiHz - kChirpLoHz) / (kChirpSec * fs);  // Hz / sample
    for (std::size_t i = 0; i < buf.size(); ++i) {
        const double t = static_cast<double>(i);
        const double phase = 2.0 * kPi * (kChirpLoHz * t + 0.5 * slope * t * t) / fs;
        buf[i] = static_cast<float>(kSignalAmplitude * std::sin(phase));
    }
}

double chordOutOfSpec(const std::vector<float>& out, std::size_t offset, double fs, double ratio) {
    std::vector<double> mags;
    magnitudeSpectrum(out, offset, kQualityChordFft, mags);
    const int nyq = kQualityChordFft / 2;
    std::vector<char> expectedBin(static_cast<std::size_t>(nyq) + 1u, 0);
    for (int t = 0; t < 3; ++t) {
        const double fExp = kChordFreqs[t] * ratio;
        const int kc =
            static_cast<int>(std::floor(fExp * static_cast<double>(kQualityChordFft) / fs + 0.5));
        for (int k = kc - kQualityGuardBins; k <= kc + kQualityGuardBins; ++k) {
            if (k >= 1 && k <= nyq) {
                expectedBin[static_cast<std::size_t>(k)] = 1;
            }
        }
    }
    double total = 0.0;
    double inSpec = 0.0;
    for (int k = 1; k <= nyq; ++k) {
        const std::size_t uk = static_cast<std::size_t>(k);
        const double e = mags[uk] * mags[uk];
        total += e;
        if (expectedBin[uk] != 0) {
            inSpec += e;
        }
    }
    return (total > 0.0) ? (1.0 - inSpec / total) : 0.0;
}

double chirpOutOfSpec(const std::vector<float>& out, std::size_t offset, double fs, double ratio,
                      double latencySamples) {
    const int n = kQualityChirpFft;
    std::vector<double> re(static_cast<std::size_t>(n), 0.0);
    std::vector<double> im(static_cast<std::size_t>(n), 0.0);
    const double slope = (kChirpHiHz - kChirpLoHz) / (kChirpSec * fs);  // Hz / sample
    for (int i = 0; i < n; ++i) {
        // 出力サンプル i に対応する入力時刻。期待瞬時周波数は r * f_in(t)。
        const double t = static_cast<double>(offset) + static_cast<double>(i) - latencySamples;
        const double phi = 2.0 * kPi * ratio * (kChirpLoHz * t + 0.5 * slope * t * t) / fs;
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / static_cast<double>(n));
        const double y =
            static_cast<double>(out[offset + static_cast<std::size_t>(i)]) * w;
        re[static_cast<std::size_t>(i)] = y * std::cos(phi);
        im[static_cast<std::size_t>(i)] = -y * std::sin(phi);
    }
    fftRadix2(re, im);
    // bin k は周波数 k*fs/n(k > n/2 は負の周波数 (k-n)*fs/n)。
    const int nearBins =
        static_cast<int>(std::floor(kChirpNearBandHz * static_cast<double>(n) / fs));
    double total = 0.0;
    double inSpec = 0.0;
    for (int k = -nearBins; k <= nearBins; ++k) {
        const std::size_t uk = static_cast<std::size_t>((k + n) % n);
        const double e = re[uk] * re[uk] + im[uk] * im[uk];
        total += e;
        if (k >= -kQualityGuardBins && k <= kQualityGuardBins) {
            inSpec += e;
        }
    }
    return (total > 0.0) ? (1.0 - inSpec / total) : 0.0;
}

void pushQualityReport(const char* label, double measured, const char* note) {
    Report r;
    r.caseName = label;
    r.metric = "outOfSpecRatio";
    r.measured = measured;
    r.hasTolerance = false;
    r.passed = true;
    r.countsTowardExit = false;  // report only
    r.note = note;
    g_reports.push_back(r);
}

// 既存方式(捕獲経路の設定 = 走査幅 40ms)と第 2 方式を同じ信号・同じ物差しで比較する。
void testQuality(double fs) {
    const double cents = static_cast<double>(prism::PitchShifter::kShiftCentsDefault);
    const double ratio = std::exp2(cents / 1200.0);

    // --- 和音 220+277+330Hz -------------------------------------------------
    {
        const std::size_t warmup = static_cast<std::size_t>(fs * 0.8);
        const std::size_t frames = warmup + static_cast<std::size_t>(kQualityChordFft);
        Scratch s;
        s.allocate(frames);

        char name[64];
        char note[128];

        prism::PitchShifter dl;
        if (dl.prepare(fs, kBlockFrames, kCaptureSweepMs)) {
            dl.setShiftCentsL(static_cast<float>(cents));
            dl.setShiftCentsR(static_cast<float>(cents));
            dl.reset();
            fillChord(s.inL, fs);
            s.inR = s.inL;
            runBlocksOn(dl, s, frames, "quality-chord-delayline", nullptr);
            const double v = chordOutOfSpec(s.outL, warmup, fs, ratio);
            std::snprintf(name, sizeof(name), "quality-chord@%.0f/delayline", fs);
            std::snprintf(note, sizeof(note), "220+277+330Hz %+.0fc, sweep %.0fms, cf %.0fms",
                          cents, kCaptureSweepMs,
                          static_cast<double>(prism::PitchShifter::kCrossfadeMsDefault));
            pushQualityReport(name, v, note);
        }

        // 位相ボコーダは N を変えて 2 点測る。220/277/330Hz は N=2048(44.1k で
        // bin 幅 21.5Hz)だと隣接ピークが分離できず、identity phase locking が
        // 3 本を 1 つの影響圏にまとめてしまう。そのとき非ピーク partial は
        // 「同じ比」ではなく「ピークと同じ絶対 Hz」だけずれるため誤差が出る。
        // N=4096 にすると 3 本が分離してこの問題は消える。低音の和音では N を
        // 上げる必要がある、という実用上の判断材料としてそのまま報告する。
        const int pvSizes[2] = {prism::PhaseVocoderShifter::kFftSizeDefault,
                                prism::PhaseVocoderShifter::kFftSizeLarge};
        for (int i = 0; i < 2; ++i) {
            prism::PhaseVocoderShifter pv;
            if (!pv.prepare(fs, kBlockFrames, pvSizes[i])) {
                continue;
            }
            pv.setShiftCentsL(static_cast<float>(cents));
            pv.setShiftCentsR(static_cast<float>(cents));
            pv.reset();
            fillChord(s.inL, fs);
            s.inR = s.inL;
            runBlocksOn(pv, s, frames, "quality-chord-phasevocoder", nullptr);
            const double v = chordOutOfSpec(s.outL, warmup, fs, ratio);
            std::snprintf(name, sizeof(name), "quality-chord@%.0f/pv-N%d", fs, pv.getFftSize());
            std::snprintf(note, sizeof(note), "220+277+330Hz %+.0fc, N=%d hop=%d bin=%.1fHz",
                          cents, pv.getFftSize(), pv.getHopSamples(),
                          fs / static_cast<double>(pv.getFftSize()));
            pushQualityReport(name, v, note);
        }
    }

    // --- チャープ 200->2000Hz / 2s ------------------------------------------
    {
        const std::size_t frames = static_cast<std::size_t>(fs * kChirpSec);
        const std::size_t offset = static_cast<std::size_t>(fs * kChirpAnalysisStartSec);
        Scratch s;
        s.allocate(frames);

        char name[64];
        char note[128];

        prism::PitchShifter dl;
        if (dl.prepare(fs, kBlockFrames, kCaptureSweepMs)) {
            dl.setShiftCentsL(static_cast<float>(cents));
            dl.setShiftCentsR(static_cast<float>(cents));
            dl.reset();
            fillChirp(s.inL, fs);
            s.inR = s.inL;
            runBlocksOn(dl, s, frames, "quality-chirp-delayline", nullptr);
            const double v =
                chirpOutOfSpec(s.outL, offset, fs, ratio, dl.getLatencySamples());
            std::snprintf(name, sizeof(name), "quality-chirp@%.0f/delayline", fs);
            std::snprintf(note, sizeof(note), "%.0f->%.0fHz/%.0fs %+.0fc, sweep %.0fms",
                          kChirpLoHz, kChirpHiHz, kChirpSec, cents, kCaptureSweepMs);
            pushQualityReport(name, v, note);
        }

        prism::PhaseVocoderShifter pv;
        if (pv.prepare(fs, kBlockFrames)) {
            pv.setShiftCentsL(static_cast<float>(cents));
            pv.setShiftCentsR(static_cast<float>(cents));
            pv.reset();
            fillChirp(s.inL, fs);
            s.inR = s.inL;
            runBlocksOn(pv, s, frames, "quality-chirp-phasevocoder", nullptr);
            const double v =
                chirpOutOfSpec(s.outL, offset, fs, ratio, pv.getLatencySamples());
            std::snprintf(name, sizeof(name), "quality-chirp@%.0f/phasevocoder", fs);
            std::snprintf(note, sizeof(note), "%.0f->%.0fHz/%.0fs %+.0fc, N=%d", kChirpLoHz,
                          kChirpHiHz, kChirpSec, cents, pv.getFftSize());
            pushQualityReport(name, v, note);
        }
    }
}

// ---------------------------------------------------------------------------
// CPU 比(報告のみ)。既存の cpu@ 行と同じ書式・同じ除外規則に乗せる。
// ---------------------------------------------------------------------------
void testPvCpu(double fs, double cents) {
    const std::size_t frames = static_cast<std::size_t>(fs * kCpuDurationSec);
    Scratch s;
    s.allocate(frames);

    char name[64];
    std::snprintf(name, sizeof(name), "cpu@%.0f/%+.0fc/pv", fs, cents);
    Report r;
    r.caseName = name;
    r.metric = "cpuRatio";
    r.hasTolerance = false;

    prism::PhaseVocoderShifter ps;
    if (!ps.prepare(fs, kBlockFrames) || !pvPreparedOrReport(ps, fs, r.caseName)) {
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
    r.allocDelta = runBlocksOn(ps, s, frames, r.caseName.c_str(), &elapsed);

    const double realTime = static_cast<double>(frames) / fs;
    r.measured = elapsed / realTime;
    r.passed = true;
    r.countsTowardExit = false;
    char note[128];
    std::snprintf(note, sizeof(note), "report only / environment dependent (no threshold), N=%d",
                  ps.getFftSize());
    r.note = note;
    g_reports.push_back(r);
}

// ---------------------------------------------------------------------------
// 契約・境界条件(construction ガードレール「happy path + エラー/境界 2 件以上」)
// ---------------------------------------------------------------------------
void testPvContract(double fs) {
    // P1: prepare() が不正な引数を拒否すること
    {
        char name[64];
        std::snprintf(name, sizeof(name), "pv-contract@%.0f/prepare-rejects", fs);
        Report r;
        r.caseName = name;
        r.metric = "rejected/4";
        r.expected = 4.0;
        r.tolerance = 0.0;
        int rejected = 0;
        prism::PhaseVocoderShifter a;
        if (!a.prepare(prism::PhaseVocoderShifter::kSampleRateMin - 1.0, kBlockFrames)) {
            ++rejected;
        }
        prism::PhaseVocoderShifter b;
        if (!b.prepare(prism::PhaseVocoderShifter::kSampleRateMax + 1.0, kBlockFrames)) {
            ++rejected;
        }
        prism::PhaseVocoderShifter c;
        if (!c.prepare(std::numeric_limits<double>::quiet_NaN(), kBlockFrames)) {
            ++rejected;
        }
        prism::PhaseVocoderShifter d;
        if (!d.prepare(fs, 0)) {
            ++rejected;
        }
        r.measured = static_cast<double>(rejected);
        r.passed = (rejected == 4) && !a.isPrepared() && !b.isPrepared() && !c.isPrepared() &&
                   !d.isPrepared();
        r.note = "fs below/above range, NaN fs, maxBlockFrames=0";
        g_reports.push_back(r);
    }

    // P2: fftSize が 1024/2048/4096 へ丸められること(0 と巨大値は端へ)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "pv-contract@%.0f/fftsize-snap", fs);
        Report r;
        r.caseName = name;
        r.metric = "matched/5";
        r.expected = 5.0;
        r.tolerance = 0.0;
        const int requested[5] = {0, 777, 2048, 4096, 1000000};
        const int wanted[5] = {2048, 1024, 2048, 4096, 4096};
        int matched = 0;
        for (int i = 0; i < 5; ++i) {
            prism::PhaseVocoderShifter ps;
            if (ps.prepare(fs, kBlockFrames, requested[i]) && ps.getFftSize() == wanted[i] &&
                ps.getHopSamples() == wanted[i] / prism::PhaseVocoderShifter::kOverlapFactor) {
                ++matched;
            }
        }
        r.measured = static_cast<double>(matched);
        r.passed = (matched == 5);
        r.note = "0/777/2048/4096/1e6 -> 2048/1024/2048/4096/4096, hop = N/4";
        g_reports.push_back(r);
    }

    // P3: 非有限セットは無視、範囲外はクランプ、numFrames<=0 は出力を触らない
    {
        char name[64];
        std::snprintf(name, sizeof(name), "pv-contract@%.0f/setter-guards", fs);
        Report r;
        r.caseName = name;
        r.metric = "ok/3";
        r.expected = 3.0;
        r.tolerance = 0.0;
        int ok = 0;

        prism::PhaseVocoderShifter ps;
        if (ps.prepare(fs, kBlockFrames)) {
            // 非有限値は無視される -> 遅延は -89 セントのままであること
            ps.setShiftCentsL(prism::PhaseVocoderShifter::kShiftCentsDefault);
            ps.setShiftCentsR(prism::PhaseVocoderShifter::kShiftCentsDefault);
            const double before = ps.getLatencySamples();
            ps.setShiftCentsL(std::numeric_limits<float>::quiet_NaN());
            ps.setShiftCentsL(std::numeric_limits<float>::infinity());
            ps.setDryWet(std::numeric_limits<float>::quiet_NaN());
            if (ps.getLatencySamples() == before) {
                ++ok;
            }
            // 範囲外はクランプ -> 下限セント(比 0.5)の遅延式に一致すること
            ps.setShiftCentsL(-99999.0f);
            ps.setShiftCentsR(-99999.0f);
            const double clamped =
                static_cast<double>(ps.getPrePadSamples()) +
                0.5 * static_cast<double>(ps.getFftSize()) *
                    (1.0 / std::exp2(static_cast<double>(
                                         prism::PhaseVocoderShifter::kShiftCentsMin) /
                                     1200.0) -
                     1.0);
            if (std::fabs(ps.getLatencySamples() - clamped) < 1e-9) {
                ++ok;
            }
            // numFrames <= 0 は out[] を書き換えない
            std::vector<float> l(4, -7.0f);
            std::vector<float> rr(4, -7.0f);
            std::vector<float> ol(4, 42.0f);
            std::vector<float> orr(4, 42.0f);
            const float* in[2] = {l.data(), rr.data()};
            float* out[2] = {ol.data(), orr.data()};
            ps.process(in, out, 0);
            ps.process(in, out, -3);
            if (ol[0] == 42.0f && orr[3] == 42.0f) {
                ++ok;
            }
        }
        r.measured = static_cast<double>(ok);
        r.passed = (ok == 3);
        r.note = "NaN/Inf ignored, out-of-range clamped, numFrames<=0 leaves out[] untouched";
        g_reports.push_back(r);
    }

    // P4: dryWet=0 は「wet と同じ遅延に揃えた素通し」であること(遅延量の独立検証)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "pv-contract@%.0f/drywet-bypass", fs);
        Report r;
        r.caseName = name;
        r.metric = "maxAbsErr";
        r.expected = 0.0;
        r.tolerance = 0.0;

        const std::size_t frames = static_cast<std::size_t>(fs * 0.2);
        Scratch s;
        s.allocate(frames);
        prism::PhaseVocoderShifter ps;
        if (!ps.prepare(fs, kBlockFrames)) {
            r.passed = false;
            r.note = "prepare failed";
            g_reports.push_back(r);
            return;
        }
        ps.setDryWet(0.0f);
        ps.reset();
        fillSine(s.inL, kGlitchFreqHz, fs, kSignalAmplitude);
        s.inR = s.inL;
        runBlocksOn(ps, s, frames, r.caseName.c_str(), nullptr);

        const std::size_t lag = static_cast<std::size_t>(ps.getPrePadSamples());
        double worst = 0.0;
        for (std::size_t i = lag; i < frames; ++i) {
            const double d = std::fabs(static_cast<double>(s.outL[i]) -
                                       static_cast<double>(s.inL[i - lag]));
            if (d > worst) {
                worst = d;
            }
        }
        r.measured = worst;
        r.passed = (worst == 0.0);
        char note[96];
        std::snprintf(note, sizeof(note), "bit-exact passthrough delayed by P=%d samples",
                      ps.getPrePadSamples());
        r.note = note;
        g_reports.push_back(r);
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
    std::printf("methods: delay-line prism::PitchShifter (default) + phase-locked prism::PhaseVocoderShifter (pv-* rows)\n");
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
        testCaptureSweep(fs);
        testCrossfadeMax(fs);
        testLatency(fs);
        testGlitch(fs);
        testGlitchAt(fs, 100.0);
        testGlitchAt(fs, 1200.0);
        testCpu(fs, static_cast<double>(prism::PitchShifter::kShiftCentsDefault), false);
        // 跳躍頻度は |1-ratio| に比例するため、相関探索の負荷は定義域の端が最悪になる。
        testCpu(fs, static_cast<double>(prism::PitchShifter::kShiftCentsMin), true);
        testCpu(fs, static_cast<double>(prism::PitchShifter::kShiftCentsMax), true);
        testContract(fs);

        // ---- 第 2 方式: 位相ロック付き位相ボコーダ(既存検査とは独立) ----
        testPvPitch(fs);
        for (int i = 0; i < 3; ++i) {
            testPvLatency(fs, kPvLatencyCents[i]);
        }
        testPvGlitch(fs, static_cast<double>(prism::PhaseVocoderShifter::kShiftCentsDefault));
        testPvGlitch(fs, static_cast<double>(prism::PhaseVocoderShifter::kShiftCentsMin));
        testPvGlitch(fs, static_cast<double>(prism::PhaseVocoderShifter::kShiftCentsMax));
        testQuality(fs);
        testPvCpu(fs, static_cast<double>(prism::PhaseVocoderShifter::kShiftCentsDefault));
        testPvCpu(fs, static_cast<double>(prism::PhaseVocoderShifter::kShiftCentsMin));
        testPvContract(fs);
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
