#include "PrismEngine.h"

#include <algorithm>
#include <cstdarg>

#include <android/log.h>

namespace prism {
namespace {

constexpr const char* kTag = "prism";

// 出力バッファはバースト 2 個分。1 個だと XRun が増え、3 個以上は遅延の無駄。
constexpr int kOutputBurstsInBuffer = 2;
// 入力容量は余裕をもたせる(read が遅れても Overrun しにくくする)。
constexpr int kInputBurstsCapacity = 8;

void logInfo(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logInfo(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, kTag, fmt, args);
    va_end(args);
}

void logError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_ERROR, kTag, fmt, args);
    va_end(args);
}

// 出力ビルダの共通設定。sampleRate / framesPerCallback が 0 なら未指定。
void configureOutput(oboe::AudioStreamBuilder& builder,
                     oboe::SharingMode sharing,
                     int32_t sampleRate,
                     int32_t framesPerCallback,
                     oboe::AudioStreamDataCallback* dataCallback,
                     oboe::AudioStreamErrorCallback* errorCallback) {
    builder.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(sharing)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Stereo)
        // Media/Music にする。VoiceCommunication にすると AEC と受話口ルーティングが
        // 有効になり、遅延も音質も悪化する(この用途では逆効果)。
        ->setUsage(oboe::Usage::Media)
        ->setContentType(oboe::ContentType::Music)
        ->setFormatConversionAllowed(true)
        ->setChannelConversionAllowed(true)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);
    if (sampleRate > 0) {
        builder.setSampleRate(sampleRate);
    }
    if (framesPerCallback > 0) {
        builder.setFramesPerDataCallback(framesPerCallback);
    }
    if (dataCallback != nullptr) {
        builder.setDataCallback(dataCallback);
    }
    if (errorCallback != nullptr) {
        builder.setErrorCallback(errorCallback);
    }
}

void configureInput(oboe::AudioStreamBuilder& builder,
                    oboe::SharingMode sharing,
                    oboe::InputPreset preset,
                    int32_t sampleRate,
                    int32_t capacityFrames,
                    oboe::AudioStreamErrorCallback* errorCallback) {
    builder.setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(sharing)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(oboe::ChannelCount::Stereo)
        ->setInputPreset(preset)
        ->setSampleRate(sampleRate)
        ->setFormatConversionAllowed(true)
        ->setChannelConversionAllowed(true)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);
    if (capacityFrames > 0) {
        builder.setBufferCapacityInFrames(capacityFrames);
    }
    if (errorCallback != nullptr) {
        builder.setErrorCallback(errorCallback);
    }
    // 入力はコールバックを持たない。出力コールバックから read する。
}

}  // namespace

PrismEngine::~PrismEngine() {
    stop();
}

// ---------------------------------------------------------------------------
// ライフサイクル
// ---------------------------------------------------------------------------

bool PrismEngine::start() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    const bool ok = startLocked();
    if (!ok) {
        // 片方だけ開いた状態を残さない。
        closeStreamsLocked();
    }
    return ok;
}

bool PrismEngine::startLocked() {
    // ---- 1. 出力デバイスの素性を調べる(サンプルレートとバースト長) ---------
    // framesPerDataCallback を「バースト長」に設定したいが、バースト長は
    // 開いてみないと分からない。そこで一度開いて閉じ、値を得てから本開きする。
    int32_t deviceSampleRate = 0;
    int32_t burst = 0;
    bool exclusive = true;
    {
        oboe::AudioStreamBuilder probe;
        configureOutput(probe, oboe::SharingMode::Exclusive, 0, 0, nullptr, nullptr);
        std::shared_ptr<oboe::AudioStream> stream;
        oboe::Result result = probe.openStream(stream);
        if (result != oboe::Result::OK) {
            // Exclusive が取れない端末は Shared で開き直す。
            exclusive = false;
            oboe::AudioStreamBuilder shared;
            configureOutput(shared, oboe::SharingMode::Shared, 0, 0, nullptr, nullptr);
            result = shared.openStream(stream);
        }
        if (result != oboe::Result::OK) {
            setError(std::string("出力ストリームを開けません: ") +
                     oboe::convertToText(result));
            return false;
        }
        deviceSampleRate = stream->getSampleRate();
        burst = stream->getFramesPerBurst();
        exclusive = (stream->getSharingMode() == oboe::SharingMode::Exclusive);
        stream->close();
    }
    if (deviceSampleRate <= 0) {
        setError("出力デバイスのサンプルレートを取得できません");
        return false;
    }
    if (burst <= 0) {
        burst = 192;  // 48kHz / 4ms 相当。取得できない端末向けの保険。
    }
    // バーストが極端に大きい端末でも作業領域に収まる範囲に制限する。
    const int32_t framesPerCallback = std::min<int32_t>(burst, AudioBridge::kMaxCallbackFrames);

    // ---- 2. 出力ストリームを本開き ------------------------------------------
    {
        oboe::AudioStreamBuilder builder;
        configureOutput(builder,
                        exclusive ? oboe::SharingMode::Exclusive : oboe::SharingMode::Shared,
                        deviceSampleRate, framesPerCallback, this, this);
        oboe::Result result = builder.openStream(outputStream_);
        if (result != oboe::Result::OK && exclusive) {
            exclusive = false;
            oboe::AudioStreamBuilder shared;
            configureOutput(shared, oboe::SharingMode::Shared, deviceSampleRate,
                            framesPerCallback, this, this);
            result = shared.openStream(outputStream_);
        }
        if (result != oboe::Result::OK) {
            setError(std::string("出力ストリームを開けません: ") +
                     oboe::convertToText(result));
            return false;
        }
        // 出力バッファはバースト 2 個分。戻り値は参考値なので捨てる。
        (void)outputStream_->setBufferSizeInFrames(outputStream_->getFramesPerBurst() *
                                                   kOutputBurstsInBuffer);
    }
    exclusiveMode_.store(outputStream_->getSharingMode() == oboe::SharingMode::Exclusive,
                         std::memory_order_relaxed);

    // ---- 3. 入力ストリーム(出力と同じサンプルレートで) ---------------------
    // Unprocessed は AGC / ノイズ抑制 / AEC をすべて切る。ピッチを正しく通し、
    // 遅延も最小になる。対応していない端末は VoiceRecognition に落とす
    // (こちらも多くの端末で AEC 無効)。
    {
        const oboe::InputPreset presets[] = {oboe::InputPreset::Unprocessed,
                                             oboe::InputPreset::VoiceRecognition,
                                             oboe::InputPreset::Generic};
        const oboe::SharingMode sharings[] = {oboe::SharingMode::Exclusive,
                                              oboe::SharingMode::Shared};
        oboe::Result result = oboe::Result::ErrorInternal;
        for (const oboe::InputPreset preset : presets) {
            for (const oboe::SharingMode sharing : sharings) {
                oboe::AudioStreamBuilder builder;
                configureInput(builder, sharing, preset, deviceSampleRate,
                               framesPerCallback * kInputBurstsCapacity, this);
                result = builder.openStream(inputStream_);
                if (result == oboe::Result::OK) {
                    unprocessedInput_.store(preset == oboe::InputPreset::Unprocessed,
                                            std::memory_order_relaxed);
                    break;
                }
            }
            if (result == oboe::Result::OK) {
                break;
            }
        }
        if (result != oboe::Result::OK) {
            setError(std::string("マイク入力を開けません(権限を確認してください): ") +
                     oboe::convertToText(result));
            return false;
        }
    }

    inputChannels_ = inputStream_->getChannelCount();
    outputChannels_ = outputStream_->getChannelCount();
    if (inputChannels_ < 1 || outputChannels_ < 1) {
        setError("入出力のチャンネル数が不正です");
        return false;
    }
    if (inputStream_->getFormat() != oboe::AudioFormat::Float ||
        outputStream_->getFormat() != oboe::AudioFormat::Float) {
        // Oboe の変換を許可してあるので通常ここには来ないが、来たら止める。
        setError("float フォーマットのストリームを確保できません");
        return false;
    }

    // ---- 4. 作業領域と DSP を用意(ここが唯一のヒープ確保) ------------------
    scratchFrames_ = AudioBridge::kMaxCallbackFrames;
    try {
        scratch_.assign(static_cast<std::size_t>(scratchFrames_) *
                            static_cast<std::size_t>(inputChannels_),
                        0.0f);
    } catch (const std::exception& e) {
        setError(std::string("作業バッファを確保できません: ") + e.what());
        return false;
    }
    if (!bridge_.prepare(static_cast<double>(outputStream_->getSampleRate()), inputChannels_,
                         outputChannels_)) {
        setError("DSP の初期化に失敗しました");
        return false;
    }

    // ---- 5. 開始。入力を先に走らせ、出力コールバックが read できる状態にする --
    inputRaw_ = inputStream_.get();
    inputErrors_.store(0, std::memory_order_relaxed);

    oboe::Result result = inputStream_->requestStart();
    if (result != oboe::Result::OK) {
        inputRaw_ = nullptr;
        setError(std::string("マイク入力を開始できません: ") + oboe::convertToText(result));
        return false;
    }
    result = outputStream_->requestStart();
    if (result != oboe::Result::OK) {
        inputStream_->requestStop();
        inputRaw_ = nullptr;
        setError(std::string("出力を開始できません: ") + oboe::convertToText(result));
        return false;
    }

    running_.store(true, std::memory_order_release);
    setError("");
    logInfo("started: fs=%d out=%dch in=%dch burst=%d cb=%d exclusive=%d unprocessed=%d",
            outputStream_->getSampleRate(), outputChannels_, inputChannels_,
            outputStream_->getFramesPerBurst(), framesPerCallback,
            exclusiveMode_.load(std::memory_order_relaxed) ? 1 : 0,
            unprocessedInput_.load(std::memory_order_relaxed) ? 1 : 0);
    return true;
}

void PrismEngine::stop() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    stopLocked();
}

void PrismEngine::stopLocked() {
    // running_ を先に落とす。onErrorAfterClose がこれを見て再起動を諦める。
    running_.store(false, std::memory_order_release);
    closeStreamsLocked();
}

void PrismEngine::closeStreamsLocked() {
    // 順序が重要: 出力を止めるとコールバックが完全に止まる(requestStop() は
    // 実行中のコールバックの完了を待つ)。その後でなければ inputRaw_ を
    // 触ってはいけない。
    if (outputStream_) {
        outputStream_->requestStop();
        outputStream_->close();
        outputStream_.reset();
    }
    inputRaw_ = nullptr;
    if (inputStream_) {
        inputStream_->requestStop();
        inputStream_->close();
        inputStream_.reset();
    }
}

// ---------------------------------------------------------------------------
// 音声スレッド — ここではロック・確保・ログ・I/O を一切しない
// ---------------------------------------------------------------------------

void PrismEngine::writeSilence(float* output, int32_t numFrames, int32_t channels) noexcept {
    const std::size_t n = static_cast<std::size_t>(numFrames) *
                          static_cast<std::size_t>(channels > 0 ? channels : 1);
    for (std::size_t i = 0; i < n; ++i) {
        output[i] = 0.0f;
    }
}

oboe::DataCallbackResult PrismEngine::onAudioReady(oboe::AudioStream* outputStream,
                                                   void* audioData,
                                                   int32_t numFrames) {
    float* output = static_cast<float*>(audioData);
    const int32_t outCh = outputStream->getChannelCount();

    oboe::AudioStream* input = inputRaw_;
    if (input == nullptr || output == nullptr || numFrames <= 0) {
        if (output != nullptr && numFrames > 0) {
            writeSilence(output, numFrames, outCh);
        }
        return oboe::DataCallbackResult::Continue;
    }

    // scratch_ は start() で確保済み。ここでは伸縮させない。
    const int32_t maxFrames = std::min<int32_t>(numFrames, scratchFrames_);
    float* scratch = scratch_.data();

    switch (bridge_.nextStep()) {
        case AudioBridge::Step::DrainInput: {
            // 入力 FIFO にたまっている古い音を空になるまで捨てる。
            int32_t drained = 0;
            for (int guard = 0; guard < 64; ++guard) {
                oboe::ResultWithValue<int32_t> r = input->read(scratch, maxFrames, 0);
                if (!r) {
                    inputErrors_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (r.value() <= 0) {
                    break;
                }
                drained += r.value();
            }
            bridge_.reportDrain(drained);
            writeSilence(output, numFrames, outCh);
            break;
        }

        case AudioBridge::Step::BuildCushion:
            // わざと読まずに入力をためる。出力は無音。
            writeSilence(output, numFrames, outCh);
            break;

        case AudioBridge::Step::Render: {
            oboe::ResultWithValue<int32_t> r = input->read(scratch, maxFrames, 0);
            int32_t framesRead = 0;
            if (r) {
                framesRead = r.value();
            } else {
                // Overrun / Disconnected など。無音ではなく「足りない分は 0」として
                // 処理を続ける。致命的な切断は onErrorAfterClose が拾って再起動する。
                inputErrors_.fetch_add(1, std::memory_order_relaxed);
            }
            bridge_.render(scratch, framesRead, output, numFrames);
            break;
        }
    }

    return oboe::DataCallbackResult::Continue;
}

// ---------------------------------------------------------------------------
// エラー(Oboe の内部スレッド。コールバックスレッドではないのでロック可)
// ---------------------------------------------------------------------------

void PrismEngine::onErrorAfterClose(oboe::AudioStream* /*stream*/, oboe::Result error) {
    if (!running_.load(std::memory_order_acquire)) {
        return;  // 明示的な stop() の途中。何もしない。
    }
    // 入出力の両方から同時に呼ばれうるので、再起動は 1 回だけに絞る。
    bool expected = false;
    if (!restartInFlight_.compare_exchange_strong(expected, true)) {
        return;
    }

    logError("stream error: %s — 再起動します", oboe::convertToText(error));

    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (running_.load(std::memory_order_acquire)) {
            closeStreamsLocked();
            running_.store(false, std::memory_order_release);
            if (startLocked()) {
                logInfo("restart succeeded");
            } else {
                closeStreamsLocked();
                logError("restart failed: %s", lastError().c_str());
            }
        }
    }

    restartInFlight_.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// 計測とパラメータ
// ---------------------------------------------------------------------------

PrismEngine::LatencyReport PrismEngine::latency() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    LatencyReport report;
    report.dspMillis = bridge_.dspLatencyMillis();
    if (!running_.load(std::memory_order_acquire) || !inputStream_ || !outputStream_) {
        report.totalMillis = report.dspMillis;
        return report;
    }

    // calculateLatencyMillis() はタイムスタンプが揃うまで失敗しうる。
    // その場合は 0 のままにして valid を落とす(嘘の数字を出さない)。
    bool ok = true;
    oboe::ResultWithValue<double> inLat = inputStream_->calculateLatencyMillis();
    if (inLat) {
        report.inputMillis = inLat.value();
    } else {
        ok = false;
    }
    oboe::ResultWithValue<double> outLat = outputStream_->calculateLatencyMillis();
    if (outLat) {
        report.outputMillis = outLat.value();
    } else {
        ok = false;
    }
    report.valid = ok;
    report.totalMillis = report.inputMillis + report.outputMillis + report.dspMillis;
    return report;
}

int PrismEngine::sampleRate() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return outputStream_ ? outputStream_->getSampleRate() : 0;
}

int PrismEngine::inputChannelCount() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return inputStream_ ? inputStream_->getChannelCount() : 0;
}

int PrismEngine::outputChannelCount() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return outputStream_ ? outputStream_->getChannelCount() : 0;
}

int PrismEngine::framesPerBurst() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return outputStream_ ? outputStream_->getFramesPerBurst() : 0;
}

std::string PrismEngine::lastError() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

void PrismEngine::setError(const std::string& message) {
    if (!message.empty()) {
        logError("%s", message.c_str());
    }
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = message;
}

void PrismEngine::setShiftCents(int channel, float cents) noexcept {
    if (channel != 1) {
        bridge_.shifter().setShiftCentsL(cents);
    }
    if (channel != 0) {
        bridge_.shifter().setShiftCentsR(cents);
    }
}

}  // namespace prism
