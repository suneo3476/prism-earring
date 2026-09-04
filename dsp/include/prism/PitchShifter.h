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
    static constexpr float kShiftCentsMin = -1200.0f;
    static constexpr float kShiftCentsMax = 1200.0f;
    static constexpr float kShiftCentsDefault = -89.0f;
    static constexpr float kDryWetMin = 0.0f;
    static constexpr float kDryWetMax = 1.0f;
    static constexpr float kDryWetDefault = 1.0f;
    static constexpr float kCrossfadeMsMin = 10.0f;
    // 上限 200ms は「跳躍間隔いっぱいまでフェードし続ける」極端を聴けるようにする値。
    // 既定 -89 セント・sweep 9.5ms では跳躍間隔 = sweep / |1-比| ≈ 190ms なので、
    // 200ms は実質「常時クロスフェード」に相当する(実効長は下の byExcursion 上限で
    // 跳躍間隔に丸められる)。容量は windowMaxSamples_ 経由で自動的に追従する。
    static constexpr float kCrossfadeMsMax = 200.0f;
    static constexpr float kCrossfadeMsDefault = 50.0f;

    static constexpr double kSampleRateMin = 8000.0;
    static constexpr double kSampleRateMax = 192000.0;

    // DesignConstants.baseOffset(entities.md): 基準読み出しオフセット(サンプル)。
    // 線形補間の近傍 2 サンプル参照ガード + 安全余裕。遅延式の定数項。
    static constexpr int kBaseOffsetSamples = 8;

    // 遅延スイープ幅(ms)。読み出しヘッドはこの幅ぶん遅れを走査し、端で「波形同期
    // 跳躍」して反対端へ戻る。走査の向きはシフト方向で決まる:
    //   * 下げ(rate<1): 遅れは単調増加。lag ∈ [baseOffset, baseOffset+sweep]。
    //   * 上げ(rate>1): 遅れは単調減少。lag ∈ [baseOffset+guard, baseOffset+guard+sweep]。
    //   * 上限は NFR-1 の 10ms 予算 — 最大遅れ = baseOffset + sweep = 9.68ms @48kHz
    //     (上げ方向は +guard で 7.3ms。いずれも 10ms 予算内)。
    //   * 下限は「正しくシフトできる最低周波数」— スイープ幅が入力の 1 周期未満だと
    //     跳躍で位相が入力に再同期してしまい、出力スペクトルのピークが f_in に戻る。
    //     sweep = 9.5ms は約 105Hz 以上の成分を正しくシフトできることを意味する。
    //
    // この値は prepare() の第 3 引数で個体ごとに変えられる(既定はここの 9.5ms)。
    // 生音の漏れ込みが無い経路(例: Android の再生音キャプチャ)では 10ms 予算に
    // 縛られないため、走査幅を広げて跳躍間隔(= sweep / |1-比|)を伸ばし、
    // 大きなシフト量でのアーティファクトを減らせる。
    static constexpr double kSweepMs = 9.5;
    static constexpr double kSweepMsMin = 2.0;
    static constexpr double kSweepMsMax = 100.0;

    // 上げ方向専用のガード帯(サンプル)= sweep / kGuardDivisor。
    // 上げ方向ではクロスフェード中の旧ヘッドが「遅れの小さい側」へ走り抜けるため、
    // baseOffset の下に走行余地が要る。この帯を確保する代わりに走査域全体を guard ぶん
    // 持ち上げ、スイープ幅(= 跳躍探索の範囲 = 低域の精度)を削らない。
    // 下げ方向は旧ヘッドが「遅れの大きい側」へ抜けるだけなので帯は不要 — したがって
    // 既定 -89 セントの遅延は従来どおり baseOffset + sweep/2 のまま変わらない。
    static constexpr int kGuardDivisor = 4;

    // 読み出しヘッドが取りうる最小の遅れ(サンプル)。線形補間の近傍 2 サンプル参照を
    // 常に「書き込み済み」領域に収めるための下限。通常動作では到達しない安全網で、
    // パラメータ急変(フェード中に rate が跳ね上がる)への保険として毎サンプル効かせる。
    static constexpr double kMinLagSamples = 2.0;

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
    //
    // sweepMs: 遅延スイープ幅(ms)。省略すると kSweepMs = 9.5ms(従来と 1 サンプルも
    //          変わらない)。[kSweepMsMin, kSweepMsMax] に clamp し、非有限値は既定値に
    //          丸める。容量・遅延・跳躍間隔はすべてこの値から導かれる。
    bool prepare(double sampleRate, int maxBlockFrames, double sweepMs = kSweepMs) {
        prepared_ = false;
        if (!(sampleRate >= kSampleRateMin) || !(sampleRate <= kSampleRateMax)) {
            return false;  // NaN もここで弾かれる
        }
        if (maxBlockFrames < 1) {
            return false;
        }
        if (!std::isfinite(sweepMs)) {
            sweepMs = kSweepMs;  // SR-3.2: 非有限値は既定値に丸める(clamp より前に検査)
        }
        if (sweepMs < kSweepMsMin) {
            sweepMs = kSweepMsMin;
        } else if (sweepMs > kSweepMsMax) {
            sweepMs = kSweepMsMax;
        }

        fs_ = sampleRate;
        maxBlockFrames_ = maxBlockFrames;
        windowMaxSamples_ = roundToInt(static_cast<double>(kCrossfadeMsMax) * sampleRate / 1000.0);
        if (windowMaxSamples_ < 2) {
            windowMaxSamples_ = 2;
        }
        sweepSamples_ = roundToInt(sweepMs * sampleRate / 1000.0);
        if (sweepSamples_ < 4) {
            sweepSamples_ = 4;
        }
        guardSamples_ = sweepSamples_ / kGuardDivisor;
        if (guardSamples_ < 1) {
            guardSamples_ = 1;
        }
        // 走査域の端(サンプル)。下げは [baseOffset, downTrigger]、
        // 上げは [upTrigger, upCeiling](= upTrigger + sweep)。
        downTriggerLag_ = static_cast<double>(kBaseOffsetSamples + sweepSamples_);
        upTriggerLag_ = static_cast<double>(kBaseOffsetSamples + guardSamples_);
        upCeilingLag_ = upTriggerLag_ + static_cast<double>(sweepSamples_);

        // 容量は最悪ケース基準(最大比 2.0 = +1200 セント / 最小比 0.5 = -1200 セント):
        //   走査域の最大到達 = baseOffset + guard + sweep(上げ)
        //   + クロスフェード中の旧ヘッドの追い越し(下げ。予算は跳躍量 <= 1 sweep)
        //   + 方向反転直後の過渡にもう 1 sweep ぶんの余裕
        //   + パラメータ急変時の保険としての windowMax(旧実装と同じ項)
        //   + 相関窓 + 最大ブロック長 + 補間余裕
        capacity_ = kBaseOffsetSamples + 3 * sweepSamples_ + windowMaxSamples_ +
                    kCorrelationLength + maxBlockFrames_ + 2;
        // 遅れの上限(安全網)。相関窓は旧ヘッドより更に kCorrelationLength 古い側を
        // 読むため、その分を差し引いた位置を超えさせない。
        lagMax_ = static_cast<double>(capacity_ - kCorrelationLength - maxBlockFrames_ - 2);

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

        sweepMs_ = sweepMs;  // 採用値(clamp 後)。確保に成功した経路でだけ記録する。
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

        // スイープ中央(= その ch の設計値遅延)から開始する。走査域が方向で違うため、
        // ch ごとに現在のシフト量の符号を見て中央を決める(BR1.2 / FR-1.2)。
        const float centsAtReset[2] = {shiftCentsL_.load(std::memory_order_relaxed),
                                       shiftCentsR_.load(std::memory_order_relaxed)};
        for (int ch = 0; ch < 2; ++ch) {
            const double midLag = midLagFor(centsAtReset[ch]);
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
                // drift>0(下げ)なら遅れは単調増加、drift<0(上げ)なら単調減少。
                const double drift = 1.0 - rate[ch];

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

                // 遅れの前進(両ヘッド)。安全網のクランプも毎サンプル通す。
                v.lag[0] = clampLag(v.lag[0] + drift);
                v.lag[1] = clampLag(v.lag[1] + drift);

                // 2.6 スイープ端に達したら波形同期跳躍 + クロスフェード開始(WF-4 / BR1.4)
                // 走査の向きが逆になるため、跳躍の向き・端・探索範囲も方向で入れ替える。
                if (v.fadePos < 0) {
                    if (drift > 0.0) {
                        if (v.lag[v.active] >= downTriggerLag_) {
                            startJump(ch, v, 1, v.lag[v.active] - kBaseOffsetSamples, drift);
                        }
                    } else if (drift < 0.0) {
                        if (v.lag[v.active] <= upTriggerLag_) {
                            startJump(ch, v, -1, upCeilingLag_ - v.lag[v.active], -drift);
                        }
                    }
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
    // 遅れはスイープ区間を一様に走査するため、設計値(平均遅れ)= 区間の中央。
    // 下げ(既定 -89 を含む)は baseOffset + sweep/2 で従来どおり。上げは走査域が
    // ガード帯ぶん持ち上がるので + guard。クロスフェード窓長には依存しない。
    // L/R でシフト方向が違うときは大きい方(= 上げ側)を返す。
    double getLatencySamples() const noexcept {
        if (!prepared_) {
            return 0.0;
        }
        const float cl = shiftCentsL_.load(std::memory_order_relaxed);
        const float cr = shiftCentsR_.load(std::memory_order_relaxed);
        const double base = (cl > 0.0f || cr > 0.0f) ? upTriggerLag_
                                                     : static_cast<double>(kBaseOffsetSamples);
        return base + 0.5 * static_cast<double>(sweepSamples_);
    }

    // 現在ラッチ済みのクロスフェード窓長(サンプル)。検証ハーネスが許容差算出に用いる(BR2.2)。
    int getWindowSamples() const noexcept { return window_; }
    // 遅延スイープ幅(サンプル)。下げ方向の最大遅れ = baseOffset + これ。
    int getSweepSamples() const noexcept { return sweepSamples_; }
    // prepare() が実際に採用した遅延スイープ幅(ms)。clamp 後の値。
    // prepare 前・prepare 失敗後は既定値 kSweepMs を返す。
    double getSweepMs() const noexcept { return sweepMs_; }
    // 上げ方向のガード帯(サンプル)。上げ方向の走査域は baseOffset + これ から始まる。
    int getGuardSamples() const noexcept { return guardSamples_; }
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

    // その ch の走査域の中央(= 設計値遅延)。上げ方向はガード帯ぶん持ち上がる。
    double midLagFor(float cents) const noexcept {
        const double base =
            (cents > 0.0f) ? upTriggerLag_ : static_cast<double>(kBaseOffsetSamples);
        return base + 0.5 * static_cast<double>(sweepSamples_);
    }

    // 安全網: 遅れを読み出し可能な範囲へ丸める。通常動作では効かない(SR-2.3)。
    double clampLag(double lag) const noexcept {
        if (lag < kMinLagSamples) {
            return kMinLagSamples;
        }
        if (lag > lagMax_) {
            return lagMax_;
        }
        return lag;
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
    // dirSign: +1 = 下げ(遅れを減らす方向へ跳ぶ)/ -1 = 上げ(遅れを増やす方向へ跳ぶ)。
    // jumpRoom: その向きに残っている走行余地(サンプル)。driftAbs: |1-rate|。
    void startJump(int ch, Voice& v, int dirSign, double jumpRoom, double driftAbs) noexcept {
        if (jumpRoom <= 1.0) {
            return;
        }
        int jMax = static_cast<int>(jumpRoom);
        int jMin = static_cast<int>(jumpRoom * kJumpMinFraction);
        if (jMin < 1) {
            jMin = 1;
        }
        if (jMax <= jMin) {
            jMax = jMin;
        }

        const double oldLag = v.lag[v.active];
        // [0, capacity) に正規化してから引く(wrapIndex は +/-1 周ぶんしか直さないため)。
        const int oldBase = wrapIndex(static_cast<int>(std::floor(
            static_cast<double>(writeIndex_) - oldLag + static_cast<double>(capacity_))));
        double bestScore = -1.0e30;
        int bestJump = jMax;
        for (int j = jMax; j >= jMin; --j) {
            const int off = dirSign * j;  // 上げ方向は「より古い側」と相関を取る
            double dot = 0.0;
            double energy = 0.0;
            for (int k = 0; k < kCorrelationLength; ++k) {
                const float a = channel_[ch][wrapIndex(oldBase - k)];
                const float b = channel_[ch][wrapIndex(oldBase - k + off)];
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
        v.lag[next] = clampLag(oldLag - static_cast<double>(dirSign * bestJump));
        v.active = next;
        int len = window_;
        // フェード中に旧ヘッドが走り抜ける遅れの幅を予算内に収める。これが実効の
        // 上限であり、下げ方向では「次の跳躍までの走行長」そのものになる。
        //   下げ: 予算 = 跳躍量(= 次の跳躍までの走行長)。したがって
        //         fadeLen <= 跳躍量 / |1-比| = 跳躍間隔。
        //   上げ: 予算 = ガード帯(旧ヘッドを baseOffset より下へ出さない)。
        // 以前はここに runLimit = 跳躍量 x 8 という別のキャップがあったが、
        // 下げ方向では byExcursion より必ず小さくなる(|1-比| <= 0.125 の域)ため、
        // 窓長を実効 76ms 程度に丸めてしまっていた。上の予算だけで安全は保たれる
        // ので撤廃した(既定の窓長 50ms では runLimit は効いていなかったため、
        // 既定の挙動・数値は従来と完全に一致する)。
        if (driftAbs > 0.0) {
            const double budget =
                (dirSign > 0) ? static_cast<double>(bestJump) : static_cast<double>(guardSamples_);
            const int byExcursion = static_cast<int>(budget / driftAbs);
            if (len > byExcursion) {
                len = byExcursion;
            }
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
    double sweepMs_ = kSweepMs;
    float smoothCoeff_ = 0.0f;
    int maxBlockFrames_ = 0;
    int windowMaxSamples_ = 0;
    int sweepSamples_ = 0;
    int guardSamples_ = 0;
    double downTriggerLag_ = 0.0;
    double upTriggerLag_ = 0.0;
    double upCeilingLag_ = 0.0;
    double lagMax_ = 0.0;
    int window_ = 2;
    bool prepared_ = false;
};

}  // namespace prism

#endif  // PRISM_PITCHSHIFTER_H
