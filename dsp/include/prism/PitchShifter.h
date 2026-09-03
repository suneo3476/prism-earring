// prism::PitchShifter — ディレイライン型ピッチシフタ(ヘッダオンリー / 依存ゼロ / C++17)
//
// 契約は inception/contract-design/contract-summary.md 契約 1、
// 振る舞いは construction/u1-dsp-core/functional-design/{functional-spec,rules,entities}.md、
// 安全設計は construction/u1-dsp-core/nfr-design/{security-design,logical-components}.md に従う。
// 設計文書からの逸脱(遅延スイープの駆動方法・跳躍量の波形同期・クロスフェード形状)は
// README.md の「設計からの逸脱」に根拠つきで記載する。
//
// 音声経路に FFT・位相ボコーダは一切使わない(レイテンシ最重要制約)。
// process() はリアルタイム安全: ヒープ確保/解放・ロック・I/O・システムコール・例外なし(BR1.5)。
//
// 論理コンポーネント(logical-components.md。物理ファイルは分割しない):
//   LC-1 ファサード / LC-2 ParameterGateway / LC-3 ParameterSmoother
//   LC-4 RingBuffer / LC-5 ReadHead x2(ch ごと)/ LC-6 Crossfader / LC-7 公開ヘッダ境界

#ifndef PRISM_PITCHSHIFTER_H
#define PRISM_PITCHSHIFTER_H

#include <atomic>   // LC-2: パラメータ受け渡し
#include <cassert>  // SR-2.2: デバッグビルドの前提条件 assert
#include <cmath>    // exp2 / floor / isfinite
#include <cstddef>
#include <vector>   // prepare() でのみ使用(process 経路では触らない)

namespace prism {

class PitchShifter {
public:
    // ---- 公開定数(SR-3.1: 閾値・定数は名前付きで単一定義) ------------------
    static constexpr float kShiftCentsMin = -150.0f;
    static constexpr float kShiftCentsMax = 0.0f;
    static constexpr float kShiftCentsDefault = -89.0f;
    static constexpr float kDryWetMin = 0.0f;
    static constexpr float kDryWetMax = 1.0f;
    static constexpr float kDryWetDefault = 1.0f;
    static constexpr float kCrossfadeMsMin = 10.0f;
    static constexpr float kCrossfadeMsMax = 100.0f;
    static constexpr float kCrossfadeMsDefault = 50.0f;

    static constexpr double kSampleRateMin = 8000.0;
    static constexpr double kSampleRateMax = 192000.0;

    // DesignConstants.baseOffset(entities.md): 基準読み出しオフセット(サンプル)。
    // 線形補間の近傍 2 サンプル参照ガード + 安全余裕。遅延式の定数項。
    static constexpr int kBaseOffsetSamples = 8;

    // 遅延スイープ幅(ms)。読み出しヘッドは baseOffset から baseOffset+sweep まで
    // 遅れを蓄積し、そこで「波形同期跳躍」で前方へ戻る。
    //   * 上限は NFR-1 の 10ms 予算 — 最大遅れ = baseOffset + sweep = 9.68ms @48kHz。
    //   * 下限は「正しくシフトできる最低周波数」— スイープ幅が入力の 1 周期未満だと
    //     跳躍で位相が入力に再同期してしまい、出力スペクトルのピークが f_in に戻る。
    //     sweep = 9.5ms は約 105Hz 以上の成分を正しくシフトできることを意味する。
    static constexpr double kSweepMs = 9.5;

    // 平滑時定数 20ms(BR1.3 / D-03)。
    static constexpr double kSmoothingTimeConstantSec = 0.020;
    // デノーマル対策(SR-3.3): 微小 DC 加算と到達スナップ閾値。
    static constexpr float kDenormalGuard = 1e-20f;
    static constexpr float kSnapCents = 1e-4f;
    static constexpr float kSnapUnit = 1e-6f;

    // 波形同期跳躍(WSOLA 相当)の探索パラメータ。
    static constexpr int kCorrelationLength = 512;   // 相関窓長(サンプル)
    static constexpr double kJumpMinFraction = 0.35; // 探索下限 = 最大跳躍量 x これ

    PitchShifter() noexcept = default;

    // ---- WF-1 prepare ------------------------------------------------------
    // 初期化時のみ呼ぶ(ヒープ確保はここだけ、BR1.5)。成功で true。
    // 音声スレッド停止中、または音声スレッド自身から呼ぶこと(logical-components LC-7 / A-3)。
    bool prepare(double sampleRate, int maxBlockFrames) {
        prepared_ = false;
        if (!(sampleRate >= kSampleRateMin) || !(sampleRate <= kSampleRateMax)) {
            return false;  // NaN もここで弾かれる
        }
        if (maxBlockFrames < 1) {
            return false;
        }

        fs_ = sampleRate;
        maxBlockFrames_ = maxBlockFrames;
        windowMaxSamples_ = roundToInt(static_cast<double>(kCrossfadeMsMax) * sampleRate / 1000.0);
        if (windowMaxSamples_ < 2) {
            windowMaxSamples_ = 2;
        }
        sweepSamples_ = roundToInt(kSweepMs * sampleRate / 1000.0);
        if (sweepSamples_ < 4) {
            sweepSamples_ = 4;
        }
        // 容量: 最大遅れ + クロスフェード中の追い越し分 + 相関窓 + 最大ブロック長 + 補間余裕
        capacity_ = kBaseOffsetSamples + sweepSamples_ + windowMaxSamples_ + kCorrelationLength +
                    maxBlockFrames_ + 2;

        try {
            storage_.assign(static_cast<std::size_t>(capacity_) * 2u, 0.0f);
        } catch (...) {
            // SD-3: 確保失敗は例外を漏らさず false へ変換する。
            storage_.clear();
            storage_.shrink_to_fit();
            return false;
        }
        channel_[0] = storage_.data();
        channel_[1] = storage_.data() + capacity_;

        smoothCoeff_ =
            static_cast<float>(std::exp(-1.0 / (kSmoothingTimeConstantSec * sampleRate)));

        prepared_ = true;
        reset();
        return true;
    }

    // ---- WF-3 reset --------------------------------------------------------
    // 状態クリア(確保済みバッファは保持、BR1.5)。
    void reset() noexcept {
        if (!prepared_) {
            return;
        }
        for (std::size_t i = 0; i < storage_.size(); ++i) {
            storage_[i] = 0.0f;
        }
        writeIndex_ = 0;

        centsL_.snapTo(shiftCentsL_.load(std::memory_order_relaxed));
        centsR_.snapTo(shiftCentsR_.load(std::memory_order_relaxed));
        dryWetSm_.snapTo(dryWet_.load(std::memory_order_relaxed));
        window_ = latchWindowSamples();

        // スイープ中央(= 設計値遅延)から開始する。
        const double midLag = kBaseOffsetSamples + 0.5 * sweepSamples_;
        for (int ch = 0; ch < 2; ++ch) {
            voice_[ch].lag[0] = midLag;
            voice_[ch].lag[1] = midLag;
            voice_[ch].active = 0;
            voice_[ch].fadePos = -1;
            voice_[ch].fadeLen = 0;
        }
    }

    // ---- WF-2 process ------------------------------------------------------
    // 非インタリーブ 2ch。in/out は [2][numFrames]。numFrames <= maxBlockFrames。
    // モノラル入力の L=R 複製は呼び出し側の責務(BR1.8)。
    void process(const float* const* in, float* const* out, int numFrames) noexcept {
        if (numFrames <= 0) {
            return;
        }
        assert(prepared_ && "prism::PitchShifter::process() called before a successful prepare()");
        if (!prepared_) {
            zeroFill(out, 0, numFrames);  // SR-2.3
            return;
        }
        assert(numFrames <= maxBlockFrames_ && "numFrames exceeds maxBlockFrames");
        int frames = numFrames;
        if (frames > maxBlockFrames_) {
            // SR-2.2: リリースではクランプし、超過分はゼロ埋め(未初期化メモリを出力しない)
            zeroFill(out, maxBlockFrames_, numFrames);
            frames = maxBlockFrames_;
        }

        // 1. ブロック頭で atomic を各 1 回 load(FR-1.5 / D-03)
        centsL_.target = shiftCentsL_.load(std::memory_order_relaxed);
        centsR_.target = shiftCentsR_.load(std::memory_order_relaxed);
        dryWetSm_.target = dryWet_.load(std::memory_order_relaxed);
        window_ = latchWindowSamples();

        const double maxLag = kBaseOffsetSamples + static_cast<double>(sweepSamples_);

        for (int i = 0; i < frames; ++i) {
            // 2.1 平滑(BR1.3)
            const float cL = centsL_.tick(smoothCoeff_, kSnapCents);
            const float cR = centsR_.tick(smoothCoeff_, kSnapCents);
            const float mix = dryWetSm_.tick(smoothCoeff_, kSnapUnit);

            // 2.2 rate 更新(ch 独立、BR1.1)
            const double rate[2] = {centsToRatio(cL), centsToRatio(cR)};

            // 2.3 書き込み
            channel_[0][writeIndex_] = in[0][i];
            channel_[1][writeIndex_] = in[1][i];

            for (int ch = 0; ch < 2; ++ch) {
                Voice& v = voice_[ch];
                const double drift = 1.0 - rate[ch];  // rate<=1 なので遅れは単調増加

                // 2.4 読み出し(線形補間、D-02)+ 2.5 クロスフェード合成(LC-6)
                float wet;
                if (v.fadePos < 0) {
                    wet = readAt(ch, v.lag[v.active]);
                } else {
                    const float u = static_cast<float>(v.fadePos) / static_cast<float>(v.fadeLen);
                    const float yOld = readAt(ch, v.lag[1 - v.active]);
                    const float yNew = readAt(ch, v.lag[v.active]);
                    // 波形同期跳躍により 2 本のヘッドは相関しているため、定振幅(線形)
                    // クロスフェードを用いる(等パワーだと +3dB の振幅こぶが出る)。
                    wet = (1.0f - u) * yOld + u * yNew;
                    ++v.fadePos;
                    if (v.fadePos >= v.fadeLen) {
                        v.fadePos = -1;  // 旧ヘッドを解放
                    }
                }

                // 2.7 dry/wet ミックス(FR-1.3)
                out[ch][i] = (1.0f - mix) * in[ch][i] + mix * wet;

                // 遅れの前進(両ヘッド)
                v.lag[0] += drift;
                v.lag[1] += drift;

                // 2.6 スイープ端に達したら波形同期跳躍 + クロスフェード開始(WF-4 / BR1.4)
                if (v.fadePos < 0 && v.lag[v.active] >= maxLag) {
                    startJump(ch, v);
                }
            }

            // 2.8 writeIndex 前進(全 ch 共有、フレームごとに 1 回)
            writeIndex_ = wrapIndex(writeIndex_ + 1);
        }
    }

    // ---- LC-2 ParameterGateway(セッター: isfinite -> clamp -> relaxed store) ----
    void setShiftCentsL(float cents) noexcept {
        storeClamped(shiftCentsL_, cents, kShiftCentsMin, kShiftCentsMax);
    }
    void setShiftCentsR(float cents) noexcept {
        storeClamped(shiftCentsR_, cents, kShiftCentsMin, kShiftCentsMax);
    }
    void setDryWet(float mix) noexcept {
        storeClamped(dryWet_, mix, kDryWetMin, kDryWetMax);
    }
    void setCrossfadeMs(float ms) noexcept {
        storeClamped(crossfadeMs_, ms, kCrossfadeMsMin, kCrossfadeMsMax);
    }

    // ---- 設計値遅延(BR1.7 相当) ------------------------------------------
    // 遅れはスイープ区間 [baseOffset, baseOffset+sweep] を一様に走査するため、
    // 設計値(平均遅れ)= baseOffset + sweep/2。クロスフェード窓長には依存しない。
    double getLatencySamples() const noexcept {
        if (!prepared_) {
            return 0.0;
        }
        return kBaseOffsetSamples + 0.5 * static_cast<double>(sweepSamples_);
    }

    // 現在ラッチ済みのクロスフェード窓長(サンプル)。検証ハーネスが許容差算出に用いる(BR2.2)。
    int getWindowSamples() const noexcept { return window_; }
    // 遅延スイープ幅(サンプル)。最大遅れ = baseOffset + これ。
    int getSweepSamples() const noexcept { return sweepSamples_; }
    bool isPrepared() const noexcept { return prepared_; }

private:
    // LC-5 ReadHead x2 + LC-6 Crossfader を ch ごとに保持する。
    // L/R はシフト量が独立(FR-1.2)なので、跳躍スケジュールも ch 独立になる。
    struct Voice {
        double lag[2] = {0.0, 0.0};  // 書き込み位置からの遅れ(サンプル、実数)
        int active = 0;              // 主ヘッド
        int fadePos = -1;            // -1 = クロスフェードなし
        int fadeLen = 0;
    };

    // LC-3 ParameterSmoother: per-sample 一次指数平滑 + デノーマル対策(BR1.3 / SD-4.2)
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
    };

    static int roundToInt(double v) noexcept { return static_cast<int>(std::floor(v + 0.5)); }

    static void storeClamped(std::atomic<float>& slot, float v, float lo, float hi) noexcept {
        if (!std::isfinite(v)) {
            return;  // SR-3.2: 非有限値は無視(clamp より前に検査する)
        }
        if (v < lo) {
            v = lo;
        } else if (v > hi) {
            v = hi;
        }
        slot.store(v, std::memory_order_relaxed);
    }

    // BR1.1: セント -> 速度比。半音=100 セントの決め打ちは行わない。
    static double centsToRatio(float cents) noexcept {
        return std::exp2(static_cast<double>(cents) / 1200.0);
    }

    int latchWindowSamples() const noexcept {
        const double ms = static_cast<double>(crossfadeMs_.load(std::memory_order_relaxed));
        int w = roundToInt(ms * fs_ / 1000.0);
        if (w < 2) {
            w = 2;
        }
        if (w > windowMaxSamples_) {
            w = windowMaxSamples_;
        }
        return w;
    }

    // SD-2: 折り返しは全経路でこのヘルパを通す(INV-1..INV-4)
    int wrapIndex(int i) const noexcept {
        if (i >= capacity_) {
            i -= capacity_;
        }
        if (i < 0) {
            i += capacity_;
        }
        return i;
    }

    // LC-5: 遅れ lag(サンプル、実数)の位置を線形補間で読む(D-02)
    float readAt(int ch, double lag) const noexcept {
        double pos = static_cast<double>(writeIndex_) - lag;
        while (pos < 0.0) {
            pos += static_cast<double>(capacity_);
        }
        const double fl = std::floor(pos);
        int n = static_cast<int>(fl);
        if (n >= capacity_) {
            n -= capacity_;
        }
        const float f = static_cast<float>(pos - fl);
        const float a = channel_[ch][n];
        const float b = channel_[ch][wrapIndex(n + 1)];
        return a + f * (b - a);
    }

    // 波形同期跳躍(WSOLA 相当): 旧ヘッド直近 kCorrelationLength サンプルと最も相関する
    // 位置へ新ヘッドを置く。跳躍量が入力周期の整数倍に近くなるため、跳躍時の位相再同期に
    // よるスペクトル汚染(ピークが f_in へ戻る現象)が起きない。
    void startJump(int ch, Voice& v) noexcept {
        const double oldLag = v.lag[v.active];
        const double jumpMax = oldLag - static_cast<double>(kBaseOffsetSamples);
        if (jumpMax <= 1.0) {
            return;
        }
        int jMax = static_cast<int>(jumpMax);
        int jMin = static_cast<int>(jumpMax * kJumpMinFraction);
        if (jMin < 1) {
            jMin = 1;
        }
        if (jMax <= jMin) {
            jMax = jMin;
        }

        const int oldBase = static_cast<int>(std::floor(
            static_cast<double>(writeIndex_) - oldLag + static_cast<double>(capacity_)));
        double bestScore = -1.0e30;
        int bestJump = jMax;
        for (int j = jMax; j >= jMin; --j) {
            double dot = 0.0;
            double energy = 0.0;
            for (int k = 0; k < kCorrelationLength; ++k) {
                const float a = channel_[ch][wrapIndex(oldBase - k)];
                const float b = channel_[ch][wrapIndex(oldBase - k + j)];
                dot += static_cast<double>(a) * static_cast<double>(b);
                energy += static_cast<double>(b) * static_cast<double>(b);
            }
            const double score = dot / std::sqrt(energy + 1.0e-12);
            if (score > bestScore) {
                bestScore = score;
                bestJump = j;  // 同点なら大きい j(= 長い走行)を採る
            }
        }

        const int next = 1 - v.active;
        v.lag[next] = oldLag - static_cast<double>(bestJump);
        v.active = next;
        int len = window_;
        // クロスフェードは走行長を超えてはならない(跳躍間隔の 1/2 を上限にする)
        const int runLimit = bestJump * 8;
        if (len > runLimit) {
            len = runLimit;
        }
        if (len < 2) {
            len = 2;
        }
        v.fadeLen = len;
        v.fadePos = 0;
    }

    void zeroFill(float* const* out, int from, int to) const noexcept {
        for (int ch = 0; ch < 2; ++ch) {
            for (int i = from; i < to; ++i) {
                out[ch][i] = 0.0f;
            }
        }
    }

    // SD-5.1: ロックフリー atomic を静的に要求する
    static_assert(std::atomic<float>::is_always_lock_free,
                  "prism requires lock-free float atomics (SR-4.1)");

    // LC-2: 制御スレッド <-> 音声スレッドの唯一の共有(relaxed)
    std::atomic<float> shiftCentsL_{kShiftCentsDefault};
    std::atomic<float> shiftCentsR_{kShiftCentsDefault};
    std::atomic<float> dryWet_{kDryWetDefault};
    std::atomic<float> crossfadeMs_{kCrossfadeMsDefault};

    // LC-4 RingBuffer(channel-major、prepare で確保・以後サイズ不変)
    std::vector<float> storage_;
    float* channel_[2] = {nullptr, nullptr};
    int capacity_ = 0;
    int writeIndex_ = 0;

    Voice voice_[2];

    // LC-3
    SmoothedParam centsL_;
    SmoothedParam centsR_;
    SmoothedParam dryWetSm_;

    double fs_ = 0.0;
    float smoothCoeff_ = 0.0f;
    int maxBlockFrames_ = 0;
    int windowMaxSamples_ = 0;
    int sweepSamples_ = 0;
    int window_ = 2;
    bool prepared_ = false;
};

}  // namespace prism

#endif  // PRISM_PITCHSHIFTER_H
