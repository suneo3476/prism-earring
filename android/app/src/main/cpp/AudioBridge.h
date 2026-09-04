// prism::AudioBridge — 全二重オーディオコールバックの「中身」。
//
// レイヤ分離(team.md「Code Style」):
//   DSP コア (dsp/include/prism/PitchShifter.h)  … 純 C++、依存ゼロ
//   → AudioBridge (このファイル)                … 純 C++、Oboe/JNI 非依存。ホストでビルド・テスト可能
//     → PrismEngine.{h,cpp}                     … Oboe のストリーム生存管理
//       → jni_bridge.cpp                        … JNI
//
// ここが受け持つのは 2 つだけ:
//   (1) 起動時の入出力同期の状態機械(Drain -> Cushion -> Render)
//   (2) インタリーブ <-> 非インタリーブ変換とモノ複製、そして PitchShifter の駆動
//
// render() / nextStep() / reportDrain() はオーディオコールバックから呼ばれる。
// したがってこの 3 つは リアルタイム安全: ヒープ確保/解放・ロック・I/O・ログ・
// システムコール・例外を一切行わない(CLAUDE.md「リアルタイムオーディオの鉄則」)。
// バッファは prepare() でのみ確保し、以後サイズを変えない。

#ifndef PRISM_AUDIOBRIDGE_H
#define PRISM_AUDIOBRIDGE_H

#include <atomic>
#include <cstddef>
#include <vector>

#include "prism/PitchShifter.h"

namespace prism {

class AudioBridge {
public:
    // 1 コールバックあたりの上限フレーム数。これを超える numFrames が来ても
    // render() が内部で分割処理するため、破綻はしない。
    static constexpr int kMaxCallbackFrames = 2048;

    // 起動同期のパラメータ(Oboe の FullDuplexStream と同じ考え方)。
    // Drain: 入力 FIFO にたまっている「古い音」を捨てきる。捨てるものが無くなった
    //        コールバックを kDrainCallbacks 回数えたら次へ進む。
    // Cushion: 逆に数バースト分だけ読まずに入力をためる。出力コールバックの
    //          ジッタで入力が空になり(underrun)途切れるのを防ぐ余裕。
    static constexpr int kDrainCallbacks = 8;
    static constexpr int kInputBurstsCushion = 1;

    // オーディオコールバックが今回やるべきこと。
    enum class Step {
        DrainInput,    // 入力を空になるまで read して捨てる。出力は無音。
        BuildCushion,  // 入力を read しない。出力は無音。
        Render,        // 入力を read して render() に渡す。
    };

    AudioBridge() = default;

    // ---- 初期化(音声スレッド停止中に呼ぶ。ここだけがヒープ確保を行う) --------
    // sampleRate は出力ストリームの実サンプルレート。inputChannels は入力
    // ストリームの実チャンネル数(1 でも 2 でも、それ以上でもよい)。
    bool prepare(double sampleRate, int inputChannels, int outputChannels) {
        prepared_ = false;
        if (!(sampleRate > 0.0) || inputChannels < 1 || outputChannels < 1) {
            return false;
        }
        if (!shifter_.prepare(sampleRate, kMaxCallbackFrames)) {
            return false;
        }

        sampleRate_ = sampleRate;
        inputChannels_ = inputChannels;
        outputChannels_ = outputChannels;

        try {
            // 非インタリーブの作業領域: 入力 L/R と出力 L/R の 4 面。
            planar_.assign(static_cast<std::size_t>(kMaxCallbackFrames) * 4u, 0.0f);
        } catch (...) {
            // 確保失敗は例外を漏らさず false へ変換する(呼び出し側で扱う)。
            planar_.clear();
            planar_.shrink_to_fit();
            return false;
        }

        inPlanar_[0] = planar_.data();
        inPlanar_[1] = planar_.data() + kMaxCallbackFrames;
        outPlanar_[0] = planar_.data() + kMaxCallbackFrames * 2;
        outPlanar_[1] = planar_.data() + kMaxCallbackFrames * 3;

        prepared_ = true;
        reset();
        return true;
    }

    // ---- ストリーム開始のたびに呼ぶ(確保済みバッファは保持) -----------------
    void reset() noexcept {
        if (!prepared_) {
            return;
        }
        shifter_.reset();
        for (std::size_t i = 0; i < planar_.size(); ++i) {
            planar_[i] = 0.0f;
        }
        drainRemaining_ = kDrainCallbacks;
        cushionRemaining_ = kInputBurstsCushion;
        underrunCount_.store(0, std::memory_order_relaxed);
        synced_.store(false, std::memory_order_relaxed);
    }

    // ---- 状態機械(RT 安全) ------------------------------------------------
    // 出力コールバックの先頭で 1 回だけ呼ぶ。
    Step nextStep() noexcept {
        if (drainRemaining_ > 0) {
            return Step::DrainInput;
        }
        if (cushionRemaining_ > 0) {
            --cushionRemaining_;
            return Step::BuildCushion;
        }
        // 同期完了は制御スレッド(UI)からも読むので atomic に置く。
        synced_.store(true, std::memory_order_relaxed);
        return Step::Render;
    }

    // DrainInput の結果を返す。捨てるフレームが無くなったコールバックだけを数える
    // (read が 0 を返す = 入力 FIFO が空 = 同期が取れた状態)。
    void reportDrain(int framesDrained) noexcept {
        if (drainRemaining_ <= 0) {
            return;
        }
        if (framesDrained <= 0) {
            --drainRemaining_;
        }
    }

    // ---- 本体(RT 安全) ----------------------------------------------------
    // input:      インタリーブ float、inputChannels_ ch、framesRead フレーム有効。
    // framesRead: 実際に read できたフレーム数(0 <= framesRead <= numFrames)。
    // output:     インタリーブ float、outputChannels_ ch、numFrames フレーム分書く。
    //
    // framesRead < numFrames のとき(入力アンダーラン)は、読めた分を「新しい側」
    // に寄せ、足りない分は先頭を 0 で埋める。こうすると最新サンプルと最新の出力
    // フレームの対応が保たれ、遅延が伸びない。
    void render(const float* input, int framesRead, float* output, int numFrames) noexcept {
        if (numFrames <= 0 || output == nullptr) {
            return;
        }
        if (!prepared_) {
            fillSilence(output, numFrames);
            return;
        }
        if (framesRead < 0) {
            framesRead = 0;
        }
        if (framesRead > numFrames) {
            framesRead = numFrames;
        }
        if (input == nullptr) {
            framesRead = 0;
        }
        if (framesRead < numFrames) {
            underrunCount_.fetch_add(1, std::memory_order_relaxed);
        }

        const int pad = numFrames - framesRead;

        // numFrames が上限を超えても破綻しないよう分割して処理する。
        int done = 0;
        while (done < numFrames) {
            int chunk = numFrames - done;
            if (chunk > kMaxCallbackFrames) {
                chunk = kMaxCallbackFrames;
            }
            deinterleaveChunk(input, pad, done, chunk);
            shifter_.process(inPlanar_, outPlanar_, chunk);
            interleaveChunk(output, done, chunk);
            done += chunk;
        }
    }

    // ---- 参照 ---------------------------------------------------------------
    PitchShifter& shifter() noexcept { return shifter_; }
    const PitchShifter& shifter() const noexcept { return shifter_; }

    bool isPrepared() const noexcept { return prepared_; }
    double sampleRate() const noexcept { return sampleRate_; }
    int inputChannels() const noexcept { return inputChannels_; }
    int outputChannels() const noexcept { return outputChannels_; }

    // DSP 側の設計値遅延(ミリ秒)。0 除算を避けるため prepared_ を見る。
    double dspLatencyMillis() const noexcept {
        if (!prepared_ || sampleRate_ <= 0.0) {
            return 0.0;
        }
        return shifter_.getLatencySamples() / sampleRate_ * 1000.0;
    }

    // 入力アンダーランの累計。診断用(UI に出す)。RT 経路からは relaxed で加算のみ。
    int underrunCount() const noexcept { return underrunCount_.load(std::memory_order_relaxed); }

    // 起動同期が完了したか。UI の「動作中」表示を実際の処理開始に合わせるために使う。
    // drainRemaining_ / cushionRemaining_ は音声スレッドが書き換えるので、
    // 制御スレッドからはこの atomic 越しにしか観測しない。
    bool isSynced() const noexcept { return synced_.load(std::memory_order_relaxed); }

private:
    // インタリーブ input の [done, done+chunk) フレームを inPlanar_ の先頭 chunk へ。
    // 先頭 pad フレームは入力が足りなかった分なので 0 とする。
    void deinterleaveChunk(const float* input, int pad, int done, int chunk) noexcept {
        const int nIn = inputChannels_;
        for (int i = 0; i < chunk; ++i) {
            const int frame = done + i;
            if (frame < pad) {
                inPlanar_[0][i] = 0.0f;
                inPlanar_[1][i] = 0.0f;
                continue;
            }
            const std::size_t base = static_cast<std::size_t>(frame - pad) *
                                     static_cast<std::size_t>(nIn);
            const float l = input[base];
            // モノ入力は L/R に複製する(BR1.8: 複製は呼び出し側の責務)。
            const float r = (nIn >= 2) ? input[base + 1] : l;
            inPlanar_[0][i] = l;
            inPlanar_[1][i] = r;
        }
    }

    // outPlanar_ の先頭 chunk を output の [done, done+chunk) フレームへ。
    void interleaveChunk(float* output, int done, int chunk) noexcept {
        const int nOut = outputChannels_;
        for (int i = 0; i < chunk; ++i) {
            const std::size_t base = static_cast<std::size_t>(done + i) *
                                     static_cast<std::size_t>(nOut);
            if (nOut == 1) {
                // モノ出力は L のみ(L/R でシフト量が違いうるため平均は取らない)。
                output[base] = outPlanar_[0][i];
                continue;
            }
            output[base] = outPlanar_[0][i];
            output[base + 1] = outPlanar_[1][i];
            // 3ch 以上のデバイスが来た場合、残りは無音にする。
            for (int c = 2; c < nOut; ++c) {
                output[base + static_cast<std::size_t>(c)] = 0.0f;
            }
        }
    }

    void fillSilence(float* output, int numFrames) const noexcept {
        const std::size_t n = static_cast<std::size_t>(numFrames) *
                              static_cast<std::size_t>(outputChannels_ > 0 ? outputChannels_ : 1);
        for (std::size_t i = 0; i < n; ++i) {
            output[i] = 0.0f;
        }
    }

    PitchShifter shifter_;

    std::vector<float> planar_;
    float* inPlanar_[2] = {nullptr, nullptr};
    float* outPlanar_[2] = {nullptr, nullptr};

    double sampleRate_ = 0.0;
    int inputChannels_ = 0;
    int outputChannels_ = 0;

    int drainRemaining_ = kDrainCallbacks;
    int cushionRemaining_ = kInputBurstsCushion;

    std::atomic<int> underrunCount_{0};
    std::atomic<bool> synced_{false};

    bool prepared_ = false;
};

}  // namespace prism

#endif  // PRISM_AUDIOBRIDGE_H
