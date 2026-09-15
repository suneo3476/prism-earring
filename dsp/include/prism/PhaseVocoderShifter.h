// prism::PhaseVocoderShifter — 位相ロック付き位相ボコーダ(第 2 方式)
// ヘッダオンリー / 外部依存ゼロ / C++17。
//
// ============================================================================
// この方式が存在する理由(CLAUDE.md「最重要制約」の例外節 / 2026-09-16 決定)
// ============================================================================
// 既定のマイク経路は今までどおり prism::PitchShifter(ディレイライン型)。
// イヤホンから生音が漏れ込む経路では遅延 10ms 以下が絶対条件のため、FFT 系は使えない。
//
// 一方、他アプリの再生音を捕獲する経路には生音の漏れ込みが無い。この経路に限り
// 遅延 50〜100ms を許容してよく、そこでは音質を優先できる。実機(Pixel 9a)で
// 音楽を -89 セント処理したときに聞こえる「張り付いたザラザラした粒ノイズ」は
// ディレイライン型の跳躍とクロスフェードに由来するもので、STFT 系では原理的に出ない。
//
// 本クラスはその「選択式の第 2 方式」であり、既定にはならない。切替はユーザーの
// 明示操作による。prism::PitchShifter は本ファイルによって一切変更されない。
//
// ============================================================================
// アルゴリズム
// ============================================================================
// ピッチシフト = 時間伸縮(位相ボコーダ)+ 再サンプリング、という標準構成。
//
//   比 r = 2^(cents/1200)  ← 半音 = 100 セントの決め打ちはしない
//
//   1. 解析ホップ Ha = Hs / r、合成ホップ Hs = N/4(固定)で STFT を回す。
//      伸縮率は Hs/Ha = r。つまり長さは r 倍になり、音高は変わらない。
//   2. 伸縮された信号を速度 r で読む(4 点 Hermite 補間)。
//      音高が r 倍になり、長さが 1/r 倍に戻る。差し引きで長さは元どおり。
//
//   位相伝播は bin ごとの瞬時周波数推定(標準の位相ボコーダ)。そのうえで
//   **identity phase locking(Laroche & Dolson 1999)** を必ず適用する:
//      - 振幅スペクトルのピーク bin を検出する
//      - ピークだけを瞬時周波数で伝播させる
//      - ピーク周辺(隣接ピークとの中点まで)の bin は、そのピークと同じ位相回転量を
//        与える。すなわち outPhase[m] = phase[m] + (sumPhase[peak] - phase[peak])。
//      これにより 1 本の正弦波を構成する複数 bin の位相関係が壊れなくなり、
//      位相ボコーダ特有の「水っぽさ / リバーブ感」が大きく減る。
//
// 窓は解析・合成とも Hann。Hs = N/4 のとき Σ w² = 1.5(厳密な COLA)なので
// 1/1.5 で正規化する。FFT は本ヘッダ内に閉じた自前 radix-2(テーブルは prepare で確保)。
//
// ============================================================================
// 遅延
// ============================================================================
//   L(r) = P + (N/2)(1/r - 1)   [サンプル]、P = N + Hs = 1.25N
//
//   P は STFT の充填ぶんの先読み(prepare で決まる固定量)。第 2 項は時間伸縮の
//   基準点が解析窓の中心にあることから来る群遅延で、下げ方向(r<1)では正、
//   上げ方向(r>1)では負になる。getLatencySamples() はこの式そのものを返し、
//   検証ハーネスはインパルス応答のエネルギー重心で実測して突き合わせる。
//
//   N=2048 / 48kHz / -89 セント   → (2560 + 54) / 48000 = 54.5ms
//   N=2048 / 48kHz / -1200 セント → (2560 + 1024) / 48000 = 74.7ms
//
// ============================================================================
// リアルタイム安全性(CLAUDE.md の鉄則)
// ============================================================================
// process() の中でヒープ確保/解放・ロック・ファイル I/O・ログ出力・システムコール・
// 例外は一切起こさない。確保は prepare() だけ。パラメータ受け渡しは std::atomic<float>
// (ロックフリーであることを static_assert で要求)。
//
// API 形状は prism::PitchShifter と揃えてある(prepare / process / reset /
// setShiftCentsL / setShiftCentsR / setDryWet / getLatencySamples / isPrepared)。
// 呼び出し側は両者を差し替えるだけで方式を切り替えられる。

#ifndef PRISM_PHASEVOCODERSHIFTER_H
#define PRISM_PHASEVOCODERSHIFTER_H

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace prism {

class PhaseVocoderShifter {
public:
    // ---- 公開定数(閾値・定数は名前付きで単一定義) --------------------------
    static constexpr float kShiftCentsMin = -1200.0f;
    static constexpr float kShiftCentsMax = 1200.0f;
    static constexpr float kShiftCentsDefault = -89.0f;
    static constexpr float kDryWetMin = 0.0f;
    static constexpr float kDryWetMax = 1.0f;
    static constexpr float kDryWetDefault = 1.0f;

    // FFT 長は 1024 / 2048 / 4096 の 3 択。prepare() に渡す。
    // 範囲外・非 2 冪の値は「最も近い許容値」へ丸める(採用値は getFftSize() で読める)。
    static constexpr int kFftSizeSmall = 1024;
    static constexpr int kFftSizeDefault = 2048;
    static constexpr int kFftSizeLarge = 4096;
    // オーバーラップ 75%。Hann の Σw² が hop = N/4 でちょうど 1.5 になる(厳密 COLA)。
    static constexpr int kOverlapFactor = 4;

    static constexpr double kSampleRateMin = 8000.0;
    static constexpr double kSampleRateMax = 192000.0;

    // 平滑時定数 20ms(PitchShifter と同じ)。cents はブロック単位、dryWet はサンプル単位。
    static constexpr double kSmoothingTimeConstantSec = 0.020;
    static constexpr float kDenormalGuard = 1e-20f;
    static constexpr float kSnapCents = 1e-4f;
    static constexpr float kSnapUnit = 1e-6f;

    // デノーマル対策: これ未満の絶対値は 0 に潰す(FFT 出力・OLA 書き込み)。
    static constexpr double kFlushToZero = 1e-30;
    // ピーク検出の下限(最大振幅に対する相対値)。無音時の数値ノイズを拾わないため。
    static constexpr double kPeakFloorRel = 1e-8;

    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kTwoPi = 6.28318530717958647692;

    PhaseVocoderShifter() noexcept = default;

    // ---- prepare -----------------------------------------------------------
    // 初期化時のみ呼ぶ(ヒープ確保はここだけ)。成功で true。
    // 音声スレッド停止中、または音声スレッド自身から呼ぶこと。
    //
    // fftSize: 1024 / 2048 / 4096。それ以外は最も近い許容値へ丸める。
    //          0 以下は既定値 2048 に丸める。
    bool prepare(double sampleRate, int maxBlockFrames, int fftSize = kFftSizeDefault) {
        prepared_ = false;
        if (!(sampleRate >= kSampleRateMin) || !(sampleRate <= kSampleRateMax)) {
            return false;  // NaN もここで弾かれる
        }
        if (maxBlockFrames < 1) {
            return false;
        }

        fftSize_ = snapFftSize(fftSize);
        hop_ = fftSize_ / kOverlapFactor;
        fs_ = sampleRate;
        maxBlockFrames_ = maxBlockFrames;

        // 先読み量 P。入力を P サンプルぶんゼロで前詰めしたのと等価に扱う。
        // P = N + Hs にすると、伸縮ストリームの「確定済み末尾」と読み出し位置の余裕が
        // 定常状態で常に Hs*r 以上(最悪 r=0.5 でも Hs/2 サンプル)確保される。
        prePad_ = fftSize_ + hop_;

        const int bins = fftSize_ / 2 + 1;
        // 入力 FIFO: 解析窓 N + 解析ホップの最大 (N/2) + 1 ブロック + 余裕。
        inCap_ = 2 * fftSize_ + maxBlockFrames_ + 16;
        // 伸縮ストリーム FIFO: 読み出しとの最大乖離 (2*Hs) + 窓長 N + 1 ブロック + 余裕。
        stretchCap_ = 4 * fftSize_ + 2 * maxBlockFrames_ + 16;

        try {
            window_.assign(static_cast<std::size_t>(fftSize_), 0.0);
            bitrev_.assign(static_cast<std::size_t>(fftSize_), 0);
            twCos_.assign(static_cast<std::size_t>(fftSize_ / 2), 0.0);
            twSin_.assign(static_cast<std::size_t>(fftSize_ / 2), 0.0);
            re_.assign(static_cast<std::size_t>(fftSize_), 0.0);
            im_.assign(static_cast<std::size_t>(fftSize_), 0.0);
            mag_.assign(static_cast<std::size_t>(bins), 0.0);
            phase_.assign(static_cast<std::size_t>(bins), 0.0);
            outPhase_.assign(static_cast<std::size_t>(bins), 0.0);
            peaks_.assign(static_cast<std::size_t>(bins), 0);
            inStorage_.assign(static_cast<std::size_t>(inCap_) * 2u, 0.0f);
            stretchStorage_.assign(static_cast<std::size_t>(stretchCap_) * 2u, 0.0f);
            for (int ch = 0; ch < 2; ++ch) {
                chan_[ch].prevPhase.assign(static_cast<std::size_t>(bins), 0.0);
                chan_[ch].sumPhase.assign(static_cast<std::size_t>(bins), 0.0);
            }
        } catch (...) {
            // 確保失敗は例外を漏らさず false へ変換する(PitchShifter と同じ約束)。
            releaseStorage();
            return false;
        }

        inRing_[0] = inStorage_.data();
        inRing_[1] = inStorage_.data() + inCap_;
        stretchRing_[0] = stretchStorage_.data();
        stretchRing_[1] = stretchStorage_.data() + stretchCap_;

        // Hann 窓(解析・合成で同じものを使う)
        for (int n = 0; n < fftSize_; ++n) {
            window_[static_cast<std::size_t>(n)] =
                0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(n) /
                                     static_cast<double>(fftSize_));
        }
        // COLA 正規化係数: Σ_k w²[n + k*hop] は n によらず一定(hop = N/4 の Hann で 1.5)。
        double colaSum = 0.0;
        for (int k = 0; k * hop_ < fftSize_; ++k) {
            const double w = window_[static_cast<std::size_t>(k * hop_)];
            colaSum += w * w;
        }
        olaScale_ = (colaSum > 0.0) ? (1.0 / colaSum) : 1.0;

        // FFT テーブル(ビット反転順 + 回転因子)
        int bits = 0;
        while ((1 << bits) < fftSize_) {
            ++bits;
        }
        for (int i = 0; i < fftSize_; ++i) {
            int rev = 0;
            for (int b = 0; b < bits; ++b) {
                if (i & (1 << b)) {
                    rev |= 1 << (bits - 1 - b);
                }
            }
            bitrev_[static_cast<std::size_t>(i)] = rev;
        }
        for (int t = 0; t < fftSize_ / 2; ++t) {
            const double ang = -kTwoPi * static_cast<double>(t) / static_cast<double>(fftSize_);
            twCos_[static_cast<std::size_t>(t)] = std::cos(ang);
            twSin_[static_cast<std::size_t>(t)] = std::sin(ang);
        }

        smoothCoeff_ =
            static_cast<float>(std::exp(-1.0 / (kSmoothingTimeConstantSec * sampleRate)));
        prepared_ = true;
        reset();
        return true;
    }

    // ---- reset -------------------------------------------------------------
    // 状態クリア(確保済みバッファは保持)。
    void reset() noexcept {
        if (!prepared_) {
            return;
        }
        for (std::size_t i = 0; i < inStorage_.size(); ++i) {
            inStorage_[i] = 0.0f;
        }
        for (std::size_t i = 0; i < stretchStorage_.size(); ++i) {
            stretchStorage_[i] = 0.0f;
        }
        // 入力を P サンプルぶんゼロで前詰めした状態から始める(先読み)。
        inWritten_ = static_cast<long long>(prePad_);
        outCount_ = 0;

        centsL_.snapTo(shiftCentsL_.load(std::memory_order_relaxed));
        centsR_.snapTo(shiftCentsR_.load(std::memory_order_relaxed));
        dryWetSm_.snapTo(dryWet_.load(std::memory_order_relaxed));

        for (int ch = 0; ch < 2; ++ch) {
            Chan& c = chan_[ch];
            for (std::size_t i = 0; i < c.prevPhase.size(); ++i) {
                c.prevPhase[i] = 0.0;
                c.sumPhase[i] = 0.0;
            }
            c.nextAnalysis = 0.0;
            c.lastStart = 0;
            c.haveLast = false;
            c.stretchWrite = 0;
            c.readAbs = 0.0;
        }
    }

    // ---- process -----------------------------------------------------------
    // 非インタリーブ 2ch。in/out は [2][numFrames]。numFrames <= maxBlockFrames。
    // モノラル入力の L=R 複製は呼び出し側の責務(PitchShifter と同じ)。
    // 確保・ロック・I/O・例外なし。
    void process(const float* const* in, float* const* out, int numFrames) noexcept {
        if (numFrames <= 0) {
            return;
        }
        assert(prepared_ &&
               "prism::PhaseVocoderShifter::process() called before a successful prepare()");
        if (!prepared_) {
            zeroFill(out, 0, numFrames);
            return;
        }
        assert(numFrames <= maxBlockFrames_ && "numFrames exceeds maxBlockFrames");
        int frames = numFrames;
        if (frames > maxBlockFrames_) {
            // リリースビルドではクランプし、超過分はゼロ埋め(未初期化メモリを出力しない)
            zeroFill(out, maxBlockFrames_, numFrames);
            frames = maxBlockFrames_;
        }

        // 1. ブロック頭で atomic を各 1 回 load
        centsL_.target = shiftCentsL_.load(std::memory_order_relaxed);
        centsR_.target = shiftCentsR_.load(std::memory_order_relaxed);
        dryWetSm_.target = dryWet_.load(std::memory_order_relaxed);

        // 2. 入力を FIFO へ書く
        for (int i = 0; i < frames; ++i) {
            const int idx = wrap(inWritten_ + static_cast<long long>(i), inCap_);
            inRing_[0][idx] = in[0][i];
            inRing_[1][idx] = in[1][i];
        }
        inWritten_ += static_cast<long long>(frames);

        // 3. cents の平滑はブロック単位(ブロック内では r 一定)。
        //    サンプル単位の指数平滑を frames 回進め、その到達値をブロックの r に使う。
        const float cL = centsL_.advance(smoothCoeff_, kSnapCents, frames);
        const float cR = centsR_.advance(smoothCoeff_, kSnapCents, frames);
        const double rate[2] = {centsToRatio(cL), centsToRatio(cR)};

        // 4. ch ごとに STFT を進め、Hermite 再サンプリングで wet を out[] に置く
        for (int ch = 0; ch < 2; ++ch) {
            Chan& c = chan_[ch];
            const double r = rate[ch];
            const double analysisHop = static_cast<double>(hop_) / r;
            // 4.1 入力が足りている限り解析フレームを進める(1 ブロックあたり高々 2〜3 回)
            while (static_cast<long long>(std::floor(c.nextAnalysis)) +
                       static_cast<long long>(fftSize_) <=
                   inWritten_) {
                renderFrame(ch, c, r);
                c.nextAnalysis += analysisHop;
            }
            // 4.2 伸縮ストリームを速度 r で読む(4 点 Hermite)
            const float* ring = stretchRing_[ch];
            // 安全網: 確定済み末尾(stretchWrite)と、リングから溢れる古さの両方を守る。
            // 定常状態では余裕が Hs*r 以上あるため、通常動作でこのクランプは効かない。
            const double hiLimit = static_cast<double>(c.stretchWrite) - 3.0;
            const double loLimit = static_cast<double>(c.stretchWrite) -
                                   static_cast<double>(stretchCap_ - fftSize_ - 8);
            for (int i = 0; i < frames; ++i) {
                double p = c.readAbs;
                if (p > hiLimit) {
                    p = hiLimit;
                }
                if (p < loLimit) {
                    p = loLimit;
                }
                const double fl = std::floor(p);
                const long long i0 = static_cast<long long>(fl);
                const float t = static_cast<float>(p - fl);
                const float xm1 = ring[wrap(i0 - 1, stretchCap_)];
                const float x0 = ring[wrap(i0, stretchCap_)];
                const float x1 = ring[wrap(i0 + 1, stretchCap_)];
                const float x2 = ring[wrap(i0 + 2, stretchCap_)];
                out[ch][i] = hermite4(xm1, x0, x1, x2, t);
                c.readAbs = p + r;
            }
        }

        // 5. dry/wet。dry は wet と同じ遅延に揃えて読む(50ms の素通しが重なると
        //    明確なエコーになるため、ディレイライン型と違いここでは遅延整合させる)。
        for (int i = 0; i < frames; ++i) {
            const float mix = dryWetSm_.tick(smoothCoeff_, kSnapUnit);
            const int idx = wrap(outCount_ + static_cast<long long>(i), inCap_);
            for (int ch = 0; ch < 2; ++ch) {
                const float dry = inRing_[ch][idx];
                out[ch][i] = (1.0f - mix) * dry + mix * out[ch][i];
            }
        }
        outCount_ += static_cast<long long>(frames);
    }

    // ---- ParameterGateway(isfinite -> clamp -> relaxed store) --------------
    void setShiftCentsL(float cents) noexcept {
        storeClamped(shiftCentsL_, cents, kShiftCentsMin, kShiftCentsMax);
    }
    void setShiftCentsR(float cents) noexcept {
        storeClamped(shiftCentsR_, cents, kShiftCentsMin, kShiftCentsMax);
    }
    void setDryWet(float mix) noexcept { storeClamped(dryWet_, mix, kDryWetMin, kDryWetMax); }

    // ---- 遅延 --------------------------------------------------------------
    // L(r) = P + (N/2)(1/r - 1)。導出は冒頭のコメントを参照。
    // L/R でシフト量が違うときは大きい方(= より下げている側)を返す。
    double getLatencySamples() const noexcept {
        if (!prepared_) {
            return 0.0;
        }
        const double lL = latencyFor(shiftCentsL_.load(std::memory_order_relaxed));
        const double lR = latencyFor(shiftCentsR_.load(std::memory_order_relaxed));
        return (lL > lR) ? lL : lR;
    }

    int getFftSize() const noexcept { return fftSize_; }
    int getHopSamples() const noexcept { return hop_; }
    // 先読み量 P(サンプル)。r = 1 のときの遅延そのもの。
    int getPrePadSamples() const noexcept { return prePad_; }
    bool isPrepared() const noexcept { return prepared_; }

private:
    // ---- ch ごとの STFT 状態 ------------------------------------------------
    struct Chan {
        std::vector<double> prevPhase;  // 前フレームの解析位相(bins)
        std::vector<double> sumPhase;   // 合成位相の累積(bins)
        double nextAnalysis = 0.0;      // 次の解析フレーム先頭(入力の絶対位置、実数)
        long long lastStart = 0;        // 前フレームの解析先頭(整数化後)
        bool haveLast = false;
        long long stretchWrite = 0;     // 次の合成フレームを書く伸縮ストリーム位置
        double readAbs = 0.0;           // 再サンプリング読み出し位置(実数)
    };

    // 一次指数平滑 + デノーマル対策(PitchShifter と同じ形)
    struct SmoothedParam {
        float target = 0.0f;
        float current = 0.0f;

        void snapTo(float v) noexcept {
            target = v;
            current = v;
        }
        float tick(float a, float snapEps) noexcept {
            const float diff = target - current;
            if (diff < snapEps && diff > -snapEps) {
                current = target;  // 到達スナップ(デノーマル源を構造的に断つ)
            } else {
                current = a * current + (1.0f - a) * target + kDenormalGuard;
            }
            return current;
        }
        // n サンプルぶん進めて到達値を返す(ブロック単位の平滑)。
        float advance(float a, float snapEps, int n) noexcept {
            for (int i = 0; i < n; ++i) {
                tick(a, snapEps);
            }
            return current;
        }
    };

    // ---- 小道具 ------------------------------------------------------------
    static int snapFftSize(int n) noexcept {
        if (n <= 0) {
            return kFftSizeDefault;
        }
        // 1024 / 2048 / 4096 のうち最も近いものへ丸める(対数距離で比較)。
        const int allowed[3] = {kFftSizeSmall, kFftSizeDefault, kFftSizeLarge};
        int best = kFftSizeDefault;
        double bestDist = -1.0;
        for (int i = 0; i < 3; ++i) {
            const double d = std::fabs(std::log2(static_cast<double>(n)) -
                                       std::log2(static_cast<double>(allowed[i])));
            if (bestDist < 0.0 || d < bestDist) {
                bestDist = d;
                best = allowed[i];
            }
        }
        return best;
    }

    static void storeClamped(std::atomic<float>& slot, float v, float lo, float hi) noexcept {
        if (!std::isfinite(v)) {
            return;  // 非有限値は無視(clamp より前に検査する)
        }
        if (v < lo) {
            v = lo;
        } else if (v > hi) {
            v = hi;
        }
        slot.store(v, std::memory_order_relaxed);
    }

    // セント -> 速度比。半音 = 100 セントの決め打ちは行わない。
    static double centsToRatio(float cents) noexcept {
        return std::exp2(static_cast<double>(cents) / 1200.0);
    }

    double latencyFor(float cents) const noexcept {
        const double r = centsToRatio(cents);
        return static_cast<double>(prePad_) +
               0.5 * static_cast<double>(fftSize_) * (1.0 / r - 1.0);
    }

    static double princarg(double x) noexcept {
        return x - kTwoPi * std::floor(x / kTwoPi + 0.5);
    }

    // 4 点 3 次 Hermite 補間(t ∈ [0,1) は x0 と x1 のあいだ)
    static float hermite4(float xm1, float x0, float x1, float x2, float t) noexcept {
        const float c = (x1 - xm1) * 0.5f;
        const float v = x0 - x1;
        const float w = c + v;
        const float a = w + v + (x2 - x0) * 0.5f;
        const float b = w + a;
        return ((a * t - b) * t + c) * t + x0;
    }

    static int wrap(long long i, int cap) noexcept {
        const long long m = i % static_cast<long long>(cap);
        return static_cast<int>(m < 0 ? m + static_cast<long long>(cap) : m);
    }

    void zeroFill(float* const* out, int from, int to) const noexcept {
        for (int ch = 0; ch < 2; ++ch) {
            for (int i = from; i < to; ++i) {
                out[ch][i] = 0.0f;
            }
        }
    }

    void releaseStorage() noexcept {
        window_.clear();
        window_.shrink_to_fit();
        bitrev_.clear();
        bitrev_.shrink_to_fit();
        twCos_.clear();
        twCos_.shrink_to_fit();
        twSin_.clear();
        twSin_.shrink_to_fit();
        re_.clear();
        re_.shrink_to_fit();
        im_.clear();
        im_.shrink_to_fit();
        mag_.clear();
        mag_.shrink_to_fit();
        phase_.clear();
        phase_.shrink_to_fit();
        outPhase_.clear();
        outPhase_.shrink_to_fit();
        peaks_.clear();
        peaks_.shrink_to_fit();
        inStorage_.clear();
        inStorage_.shrink_to_fit();
        stretchStorage_.clear();
        stretchStorage_.shrink_to_fit();
        for (int ch = 0; ch < 2; ++ch) {
            chan_[ch].prevPhase.clear();
            chan_[ch].prevPhase.shrink_to_fit();
            chan_[ch].sumPhase.clear();
            chan_[ch].sumPhase.shrink_to_fit();
        }
    }

    // ---- 自前 radix-2 FFT(テーブルは prepare 済み、確保なし) --------------
    void fft(bool inverse) noexcept {
        const int n = fftSize_;
        double* re = re_.data();
        double* im = im_.data();
        if (inverse) {
            for (int i = 0; i < n; ++i) {
                im[i] = -im[i];
            }
        }
        for (int i = 0; i < n; ++i) {
            const int j = bitrev_[static_cast<std::size_t>(i)];
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1;
            const int step = n / len;
            for (int base = 0; base < n; base += len) {
                for (int j = 0; j < half; ++j) {
                    const std::size_t t = static_cast<std::size_t>(j * step);
                    const double cr = twCos_[t];
                    const double ci = twSin_[t];
                    const int a = base + j;
                    const int b = a + half;
                    const double ur = re[a];
                    const double ui = im[a];
                    const double vr = re[b] * cr - im[b] * ci;
                    const double vi = re[b] * ci + im[b] * cr;
                    re[a] = ur + vr;
                    im[a] = ui + vi;
                    re[b] = ur - vr;
                    im[b] = ui - vi;
                }
            }
        }
        if (inverse) {
            const double s = 1.0 / static_cast<double>(n);
            for (int i = 0; i < n; ++i) {
                re[i] *= s;
                im[i] = -im[i] * s;
            }
        }
    }

    // ---- 1 フレームぶんの解析 -> 位相ロック -> 合成 -> OLA -----------------
    void renderFrame(int ch, Chan& c, double r) noexcept {
        const long long start = static_cast<long long>(std::floor(c.nextAnalysis));
        // 実際の解析ホップ(整数化の誤差を吸収するため、位相伝播にはこの実測値を使う)
        int ha = hop_;
        if (c.haveLast) {
            const long long d = start - c.lastStart;
            ha = (d < 1) ? 1 : static_cast<int>(d);
        }
        c.lastStart = start;
        c.haveLast = true;

        const int bins = fftSize_ / 2 + 1;
        const float* inR = inRing_[ch];

        // 解析: Hann 窓 + FFT
        for (int n = 0; n < fftSize_; ++n) {
            re_[static_cast<std::size_t>(n)] =
                static_cast<double>(inR[wrap(start + static_cast<long long>(n), inCap_)]) *
                window_[static_cast<std::size_t>(n)];
            im_[static_cast<std::size_t>(n)] = 0.0;
        }
        fft(false);

        double maxMag = 0.0;
        for (int m = 0; m < bins; ++m) {
            const std::size_t um = static_cast<std::size_t>(m);
            const double a = re_[um];
            const double b = im_[um];
            double mg = std::sqrt(a * a + b * b);
            if (mg < kFlushToZero) {
                mg = 0.0;
                phase_[um] = 0.0;
            } else {
                phase_[um] = std::atan2(b, a);
            }
            mag_[um] = mg;
            if (mg > maxMag) {
                maxMag = mg;
            }
        }

        // 上げ方向の再サンプリングで折り返す帯域を落とす(ナイキスト超えの除去)。
        // 再サンプリングは速度 r で読むため、fs/(2r) より上は出力で折り返す。
        int binLimit = bins - 1;
        if (r > 1.0) {
            binLimit = static_cast<int>(std::floor(static_cast<double>(fftSize_) / (2.0 * r)));
            if (binLimit < 1) {
                binLimit = 1;
            }
            if (binLimit > bins - 1) {
                binLimit = bins - 1;
            }
            for (int m = binLimit + 1; m < bins; ++m) {
                mag_[static_cast<std::size_t>(m)] = 0.0;
            }
        }

        // ピーク検出(前後 2 bin より大きい bin)。identity phase locking の核。
        int np = 0;
        const double floorMag = maxMag * kPeakFloorRel;
        for (int m = 2; m <= binLimit - 2; ++m) {
            const std::size_t um = static_cast<std::size_t>(m);
            const double v = mag_[um];
            if (v > floorMag && v > mag_[um - 1] && v > mag_[um + 1] && v > mag_[um - 2] &&
                v > mag_[um + 2]) {
                peaks_[static_cast<std::size_t>(np++)] = m;
            }
        }

        const double hs = static_cast<double>(hop_);
        const double haD = static_cast<double>(ha);
        const double binAngle = kTwoPi * haD / static_cast<double>(fftSize_);

        if (np == 0) {
            // ピークなし(無音・雑音のみ)。標準の bin 単位位相伝播にフォールバックする。
            for (int m = 0; m < bins; ++m) {
                const std::size_t um = static_cast<std::size_t>(m);
                const double expct = binAngle * static_cast<double>(m);
                const double dev = princarg(phase_[um] - c.prevPhase[um] - expct);
                const double omega = (expct + dev) / haD;
                outPhase_[um] = princarg(c.sumPhase[um] + omega * hs);
            }
        } else {
            // Laroche-Dolson identity phase locking:
            // ピークだけを瞬時周波数で伝播させ、その影響圏(隣接ピークとの中点まで)の
            // bin にはピークと同じ位相回転量を与える。bin 間の位相関係が保たれる。
            int lo = 0;
            for (int p = 0; p < np; ++p) {
                const int pk = peaks_[static_cast<std::size_t>(p)];
                const int hi = (p + 1 < np)
                                   ? ((pk + peaks_[static_cast<std::size_t>(p + 1)]) / 2)
                                   : (bins - 1);
                const std::size_t upk = static_cast<std::size_t>(pk);
                const double expct = binAngle * static_cast<double>(pk);
                const double dev = princarg(phase_[upk] - c.prevPhase[upk] - expct);
                const double omega = (expct + dev) / haD;
                const double sp = princarg(c.sumPhase[upk] + omega * hs);
                const double rot = sp - phase_[upk];
                for (int m = lo; m <= hi; ++m) {
                    const std::size_t um = static_cast<std::size_t>(m);
                    outPhase_[um] = princarg(phase_[um] + rot);
                }
                lo = hi + 1;
            }
        }

        for (int m = 0; m < bins; ++m) {
            const std::size_t um = static_cast<std::size_t>(m);
            c.prevPhase[um] = phase_[um];
            c.sumPhase[um] = outPhase_[um];
        }

        // 合成スペクトル(エルミート対称に復元)-> 逆 FFT
        for (int m = 0; m < bins; ++m) {
            const std::size_t um = static_cast<std::size_t>(m);
            re_[um] = mag_[um] * std::cos(outPhase_[um]);
            im_[um] = mag_[um] * std::sin(outPhase_[um]);
        }
        im_[0] = 0.0;
        im_[static_cast<std::size_t>(bins - 1)] = 0.0;
        for (int m = bins; m < fftSize_; ++m) {
            const std::size_t um = static_cast<std::size_t>(m);
            const std::size_t mirror = static_cast<std::size_t>(fftSize_ - m);
            re_[um] = re_[mirror];
            im_[um] = -im_[mirror];
        }
        fft(true);

        // 合成窓 + オーバーラップ加算。
        // 末尾 hop サンプルはどの旧フレームも書いていない領域なので「代入」、
        // それ以外は「加算」。これでリングを事前にゼロクリアする必要がなくなる。
        float* sr = stretchRing_[ch];
        const int assignFrom = fftSize_ - hop_;
        for (int n = 0; n < fftSize_; ++n) {
            const std::size_t un = static_cast<std::size_t>(n);
            double v = re_[un] * window_[un] * olaScale_;
            if (v > -kFlushToZero && v < kFlushToZero) {
                v = 0.0;
            }
            const int idx = wrap(c.stretchWrite + static_cast<long long>(n), stretchCap_);
            if (n < assignFrom) {
                sr[idx] += static_cast<float>(v);
            } else {
                sr[idx] = static_cast<float>(v);
            }
        }
        c.stretchWrite += static_cast<long long>(hop_);
    }

    // ロックフリー atomic を静的に要求する
    static_assert(std::atomic<float>::is_always_lock_free,
                  "prism requires lock-free float atomics");

    // 制御スレッド <-> 音声スレッドの唯一の共有(relaxed)
    std::atomic<float> shiftCentsL_{kShiftCentsDefault};
    std::atomic<float> shiftCentsR_{kShiftCentsDefault};
    std::atomic<float> dryWet_{kDryWetDefault};

    // prepare で確保、以後サイズ不変
    std::vector<double> window_;
    std::vector<int> bitrev_;
    std::vector<double> twCos_;
    std::vector<double> twSin_;
    std::vector<double> re_;
    std::vector<double> im_;
    std::vector<double> mag_;
    std::vector<double> phase_;
    std::vector<double> outPhase_;
    std::vector<int> peaks_;
    std::vector<float> inStorage_;
    std::vector<float> stretchStorage_;
    float* inRing_[2] = {nullptr, nullptr};
    float* stretchRing_[2] = {nullptr, nullptr};

    Chan chan_[2];
    SmoothedParam centsL_;
    SmoothedParam centsR_;
    SmoothedParam dryWetSm_;

    double fs_ = 0.0;
    double olaScale_ = 1.0;
    float smoothCoeff_ = 0.0f;
    int fftSize_ = kFftSizeDefault;
    int hop_ = kFftSizeDefault / kOverlapFactor;
    int prePad_ = 0;
    int maxBlockFrames_ = 0;
    int inCap_ = 0;
    int stretchCap_ = 0;
    long long inWritten_ = 0;
    long long outCount_ = 0;
    bool prepared_ = false;
};

}  // namespace prism

#endif  // PRISM_PHASEVOCODERSHIFTER_H
