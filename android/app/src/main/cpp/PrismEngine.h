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
        double micSweepMillis = 0.0;      // マイク経路の走査幅(採用値)
        double captureSweepMillis = 0.0;  // 捕獲経路の走査幅(採用値)
        double captureDspMillis = 0.0;    // 捕獲経路の DSP 遅延(選択中の方式の値)
    };

    // 処理方式。setMethod() の method 引数(AudioBridge::kMethod* と同じ値)。
    static constexpr int kMethodDelayLine = AudioBridge::kMethodDelayLine;
    static constexpr int kMethodPhaseVocoder2048 = AudioBridge::kMethodPhaseVocoder2048;
    static constexpr int kMethodPhaseVocoder4096 = AudioBridge::kMethodPhaseVocoder4096;
    // setMethod() の path 引数。
    static constexpr int kPathMic = 0;
    static constexpr int kPathCapture = 1;

    // 出力の用途。setOutputUsage() の引数。
    //   Media  … 既定。メディア音量に乗る(ContentType::Music)。
    //   Accessibility … アクセシビリティ用途(ContentType::Speech)。メディア音量を
    //                   絞っても本アプリの出力だけ鳴らせるかの実験用。
    static constexpr int kUsageMedia = 0;
    static constexpr int kUsageAccessibility = 1;

    // マイクの前処理。setInputPreset() の引数(次の start() から有効)。
    //   Raw               … oboe::InputPreset::Unprocessed。AGC / ノイズ抑制 / AEC を
    //                        すべて切る。ピッチを最も正しく通すが、マイクのノイズ床が
    //                        そのまま乗る(無音時に「サー」というノイズが出やすい)。
    //   NoiseSuppression  … oboe::InputPreset::VoiceRecognition。端末のノイズ抑制を通す
    //                        (AEC は多くの端末で無効のまま)。既定。
    //   VoiceCommunication… oboe::InputPreset::VoiceCommunication。AEC / AGC も有効になる。
    //                        通話向け。声は聞きやすいが音楽には不向き。
    static constexpr int kInputPresetRaw = 0;
    static constexpr int kInputPresetNoiseSuppression = 1;
    static constexpr int kInputPresetVoiceCommunication = 2;

    // 0 = 自動(OS 既定のデバイス)。setOutput/InputDeviceId() の「指定なし」。
    static constexpr int32_t kDeviceIdAuto = 0;

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
    // マイク入力側で「要求フレーム数に足りなかった」不足フレームの累計(診断用)。
    int micShortfallFrames() const noexcept { return bridge_.micShortfallFrames(); }
    // 捕獲リング側で「要求フレーム数に足りなかった」不足フレームの累計(診断用)。
    int captureShortfallFrames() const noexcept { return bridge_.captureShortfallFrames(); }
    // Oboe の xRun カウント(制御スレッドから。ストリームが無ければ 0)。
    // AudioPlaybackCapture(捕獲経路)は AudioRecord(Java)なので Oboe の
    // AudioStream ではなく、対応する xRun カウントは存在しない
    // (代わりに captureUnderruns()/captureOverruns() を使う)。
    int outputXRunCount() const;
    int inputXRunCount() const;
    bool usingExclusiveMode() const noexcept {
        return exclusiveMode_.load(std::memory_order_relaxed);
    }
    bool usingUnprocessedInput() const noexcept {
        return unprocessedInput_.load(std::memory_order_relaxed);
    }

    // ---- 捕獲経路の診断 -----------------------------------------------------
    int captureUnderruns() const noexcept { return bridge_.captureUnderruns(); }
    int captureOverruns() const noexcept { return bridge_.captureOverruns(); }
    int captureFillFrames() const noexcept { return bridge_.captureFillFrames(); }

    // ---- 実際に開いたデバイスと用途 -----------------------------------------
    // 0 = 未取得。指定した ID で開けなかった場合は *DeviceFallback() が true になる。
    int32_t actualOutputDeviceId() const;
    int32_t actualInputDeviceId() const;
    bool outputDeviceFallback() const noexcept {
        return outputDeviceFallback_.load(std::memory_order_relaxed);
    }
    bool inputDeviceFallback() const noexcept {
        return inputDeviceFallback_.load(std::memory_order_relaxed);
    }
    int outputUsage() const noexcept { return outputUsage_.load(std::memory_order_relaxed); }

    std::string lastError() const;

    // ---- パラメータ(制御スレッドから。内部は std::atomic なのでロック不要) --
    // シフト量 / クロスフェードはマイク経路と捕獲経路の両方へ流す
    // (2 本のシフタで違うのは走査幅だけ)。dry-wet だけは例外:
    // 捕獲経路には「イヤホンから漏れる生音」に相当するものが無く、
    // 捕獲経路の dry 成分は原音そのもの(=二重再生)になってしまうため、
    // 捕獲経路は常に wet=1.0 固定にする(PitchShifter::kDryWetDefault が
    // 既に 1.0 のため、そもそも書き換えない = ここでは触らない)。
    // マイク経路の dry-wet は従来どおり UI の値を反映する。
    // シフト量 / dry-wet はどちらも、経路ごとに選択されている 3 方式(ディレイライン /
    // 位相ボコーダ N=2048 / N=4096)すべてへ流す(AudioBridge::setMic*/setCapture* 参照)。
    // channel: 0 = L, 1 = R, それ以外 = 両方
    void setShiftCents(int channel, float cents) noexcept;
    void setDryWet(float mix) noexcept {
        bridge_.setMicDryWet(mix);
    }
    void setCrossfadeMs(float ms) noexcept {
        bridge_.shifter().setCrossfadeMs(ms);
        bridge_.captureShifter().setCrossfadeMs(ms);
    }
    // gain は倍率(0.5〜4.0)。AudioBridge::setOutputGain が clamp する。
    void setOutputGain(float gain) noexcept { bridge_.setOutputGain(gain); }

    // ---- 処理方式(制御スレッドから。動作中に呼んでよい。再起動不要) -----------
    // path: kPathMic / kPathCapture。method: kMethodDelayLine /
    // kMethodPhaseVocoder2048 / kMethodPhaseVocoder4096。範囲外の値は低遅延に丸める。
    void setMethod(int path, int method) noexcept {
        if (path == kPathCapture) {
            bridge_.setCaptureMethod(method);
        } else {
            bridge_.setMicMethod(method);
        }
    }
    int micMethod() const noexcept { return bridge_.micMethod(); }
    int captureMethod() const noexcept { return bridge_.captureMethod(); }

    // ---- 捕獲経路(制御スレッドから。動作中に呼んでよい) ---------------------
    void setCaptureEnabled(bool enabled) noexcept { bridge_.setCaptureEnabled(enabled); }
    bool isCaptureEnabled() const noexcept { return bridge_.isCaptureEnabled(); }
    // 0.0 でマイクを完全ミュート(捕獲音だけを聞く)。
    void setMicGain(float gain) noexcept { bridge_.setMicGain(gain); }
    void setCaptureGain(float gain) noexcept { bridge_.setCaptureGain(gain); }
    // Java の AudioRecord スレッドから。戻り値は実際に書けたフレーム数。
    int pushCapture(const float* interleaved, int frames, int channels) noexcept {
        return bridge_.pushCapture(interleaved, frames, channels);
    }

    // ---- 診断用の 10 秒録音(制御スレッドから。動作中にだけ開始できる) ----------
    // 捕獲 in/out・マイク in/out の 4 本を同時刻に開始して 10 秒間記録する。
    // WAV エンコードや MediaStore 保存はここではなく Kotlin 側の責務
    // (jni_bridge が生の float 配列を返し、そこから先は Java/Kotlin で行う)。
    bool startDiagnosticRecording() noexcept {
        return isRunning() && bridge_.startDiagnosticRecording();
    }
    void cancelDiagnosticRecording() noexcept { bridge_.cancelDiagnosticRecording(); }
    // isDiagnosticRecordingDone() == true を確認した後にだけ呼ぶこと。
    void releaseDiagnosticRecording() noexcept { bridge_.releaseDiagnosticRecording(); }
    bool isDiagnosticRecordingActive() const noexcept {
        return bridge_.isDiagnosticRecordingActive();
    }
    bool isDiagnosticRecordingDone() const noexcept { return bridge_.isDiagnosticRecordingDone(); }
    int diagnosticTotalFrames() const noexcept { return bridge_.diagnosticTotalFrames(); }
    int diagnosticInputChannels() const noexcept { return bridge_.inputChannels(); }
    const float* diagnosticCaptureIn() const noexcept { return bridge_.diagnosticCaptureIn(); }
    const float* diagnosticCaptureOut() const noexcept { return bridge_.diagnosticCaptureOut(); }
    const float* diagnosticMicIn() const noexcept { return bridge_.diagnosticMicIn(); }
    const float* diagnosticMicOut() const noexcept { return bridge_.diagnosticMicOut(); }

    // ---- 次の start() から効く設定(制御スレッドから) ------------------------
    // 0 = 自動。指定 ID で開けなければ自動で開き直し、*DeviceFallback() が立つ。
    void setOutputDeviceId(int32_t id) noexcept {
        outputDeviceId_.store(id, std::memory_order_relaxed);
    }
    void setInputDeviceId(int32_t id) noexcept {
        inputDeviceId_.store(id, std::memory_order_relaxed);
    }
    // kUsageMedia / kUsageAccessibility。未知の値は kUsageMedia として扱う。
    void setOutputUsage(int usage) noexcept {
        outputUsage_.store(usage == kUsageAccessibility ? kUsageAccessibility : kUsageMedia,
                           std::memory_order_relaxed);
    }
    // kInputPresetRaw / kInputPresetNoiseSuppression / kInputPresetVoiceCommunication。
    // 未知の値は kInputPresetNoiseSuppression(既定)として扱う。
    void setInputPreset(int preset) noexcept {
        inputPreset_.store(
            (preset == kInputPresetRaw || preset == kInputPresetVoiceCommunication)
                ? preset
                : kInputPresetNoiseSuppression,
            std::memory_order_relaxed);
    }
    // マイク経路の走査幅(ms)。範囲外は PitchShifter が clamp する。
    void setMicSweepMs(double ms) noexcept {
        micSweepMs_.store(ms, std::memory_order_relaxed);
    }
    // 実際に採用されている走査幅(ms)。start 前は要求値をそのまま返す。
    double micSweepMs() const noexcept {
        return bridge_.isPrepared() ? bridge_.micSweepMs()
                                    : micSweepMs_.load(std::memory_order_relaxed);
    }
    double captureSweepMs() const noexcept { return bridge_.captureSweepMs(); }

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

    // 次の start() で使う設定と、その結果。
    std::atomic<int32_t> outputDeviceId_{kDeviceIdAuto};
    std::atomic<int32_t> inputDeviceId_{kDeviceIdAuto};
    std::atomic<bool> outputDeviceFallback_{false};
    std::atomic<bool> inputDeviceFallback_{false};
    std::atomic<int> outputUsage_{kUsageMedia};
    std::atomic<int> inputPreset_{kInputPresetNoiseSuppression};
    std::atomic<double> micSweepMs_{PitchShifter::kSweepMs};

    mutable std::mutex errorMutex_;
    std::string lastError_;
};

}  // namespace prism

#endif  // PRISM_PRISMENGINE_H
