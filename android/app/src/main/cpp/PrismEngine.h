// prism::PrismEngine — Oboe の全二重ストリーム生存管理。
//
// 構成は Oboe の samples/LiveEffect と同じ「出力コールバック駆動の全二重」:
//   出力ストリームだけがデータコールバックを持ち、そのコールバックの中で
//   入力ストリームを timeoutNanos = 0 で read する。入力側にコールバックを
//   置いて 2 本のコールバックスレッドを同期させるより、はるかに単純で堅い。
//
// 実際の音声処理(状態機械・チャンネル変換・PitchShifter)はすべて
// AudioBridge にある。このクラスはストリームを開く/閉じる/測るだけを持つ。
//
// スレッド:
//   - onAudioReady()      … 音声スレッド。ロック・確保・ログを一切しない。
//   - start/stop/latency  … 制御スレッド(JNI)。controlMutex_ で直列化する。
//   - onErrorAfterClose() … Oboe の内部スレッド。controlMutex_ を取ってよい。
//
// 音声スレッドが触るのは inputRaw_ / scratch_ / bridge_ のみ。inputRaw_ は
// 「出力ストリームが停止していて、コールバックが走っていない」間にしか
// 書き換えない(requestStop() はコールバック完了まで戻らない)。

#ifndef PRISM_PRISMENGINE_H
#define PRISM_PRISMENGINE_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <oboe/Oboe.h>

#include "AudioBridge.h"

namespace prism {

class PrismEngine : public oboe::AudioStreamDataCallback,
                    public oboe::AudioStreamErrorCallback {
public:
    struct LatencyReport {
        double inputMillis = 0.0;
        double outputMillis = 0.0;
        double dspMillis = 0.0;
        double totalMillis = 0.0;
        bool valid = false;
    };

    PrismEngine() = default;
    ~PrismEngine() override;

    PrismEngine(const PrismEngine&) = delete;
    PrismEngine& operator=(const PrismEngine&) = delete;

    // ---- ライフサイクル(制御スレッドから) ---------------------------------
    // 失敗したら false。理由は lastError() に入る。
    bool start();
    void stop();

    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    // 起動同期(Drain/Cushion)が終わり、実際に音を通しているか。
    bool isSynced() const noexcept { return running_.load(std::memory_order_acquire) &&
                                            bridge_.isSynced(); }

    // ---- 計測(制御スレッドから。1 秒に 1 回程度を想定) ---------------------
    LatencyReport latency() const;

    int sampleRate() const;
    int inputChannelCount() const;
    int outputChannelCount() const;
    int framesPerBurst() const;
    int underrunCount() const noexcept { return bridge_.underrunCount(); }
    // 入力 read が失敗した累計。診断用。
    int inputErrorCount() const noexcept {
        return inputErrors_.load(std::memory_order_relaxed);
    }
    bool usingExclusiveMode() const noexcept {
        return exclusiveMode_.load(std::memory_order_relaxed);
    }
    bool usingUnprocessedInput() const noexcept {
        return unprocessedInput_.load(std::memory_order_relaxed);
    }

    std::string lastError() const;

    // ---- パラメータ(制御スレッドから。内部は std::atomic なのでロック不要) --
    // channel: 0 = L, 1 = R, それ以外 = 両方
    void setShiftCents(int channel, float cents) noexcept;
    void setDryWet(float mix) noexcept { bridge_.shifter().setDryWet(mix); }
    void setCrossfadeMs(float ms) noexcept { bridge_.shifter().setCrossfadeMs(ms); }
    // gain は倍率(0.5〜4.0)。AudioBridge::setOutputGain が clamp する。
    void setOutputGain(float gain) noexcept { bridge_.setOutputGain(gain); }

private:
    // oboe::AudioStreamDataCallback
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* outputStream,
                                          void* audioData,
                                          int32_t numFrames) override;
    // oboe::AudioStreamErrorCallback
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

    // controlMutex_ を保持した状態で呼ぶこと。
    bool startLocked();
    void stopLocked();
    void closeStreamsLocked();
    void setError(const std::string& message);

    static void writeSilence(float* output, int32_t numFrames, int32_t channels) noexcept;

    mutable std::mutex controlMutex_;

    // 所有権は制御スレッドのみ。音声スレッドは inputRaw_ 経由でしか触らない。
    std::shared_ptr<oboe::AudioStream> inputStream_;
    std::shared_ptr<oboe::AudioStream> outputStream_;
    oboe::AudioStream* inputRaw_ = nullptr;

    AudioBridge bridge_;

    // 入力 read の受け皿。start() で確保し、コールバック中は伸縮させない。
    std::vector<float> scratch_;
    int scratchFrames_ = 0;
    int inputChannels_ = 0;
    int outputChannels_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> restartInFlight_{false};
    std::atomic<int> inputErrors_{0};
    std::atomic<bool> exclusiveMode_{false};
    std::atomic<bool> unprocessedInput_{false};

    mutable std::mutex errorMutex_;
    std::string lastError_;
};

}  // namespace prism

#endif  // PRISM_PRISMENGINE_H
