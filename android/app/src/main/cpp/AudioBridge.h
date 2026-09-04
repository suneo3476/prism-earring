// prism::AudioBridge — 全二重オーディオコールバックの「中身」。
//
// レイヤ分離(team.md「Code Style」):
//   DSP コア (dsp/include/prism/PitchShifter.h)  … 純 C++、依存ゼロ
//   → AudioBridge (このファイル)                … 純 C++、Oboe/JNI 非依存。ホストでビルド・テスト可能
//     → PrismEngine.{h,cpp}                     … Oboe のストリーム生存管理
//       → jni_bridge.cpp                        … JNI
//
// ここが受け持つのは 4 つ:
//   (1) 起動時の入出力同期の状態機械(Drain -> Cushion -> Render)
//   (2) インタリーブ <-> 非インタリーブ変換とモノ複製、そして PitchShifter の駆動
//   (3) 捕獲音(AudioPlaybackCapture で拾った他アプリの再生音)を受け取る
//       ロックフリー SPSC リングと、その読み出し状態機械
//   (4) マイク経路と捕獲経路のミックス
//
// render() / nextStep() / reportDrain() はオーディオコールバックから呼ばれる。
// したがってこの 3 つは リアルタイム安全: ヒープ確保/解放・ロック・I/O・ログ・
// システムコール・例外を一切行わない(CLAUDE.md「リアルタイムオーディオの鉄則」)。
// バッファは prepare() でのみ確保し、以後サイズを変えない。
//
// スレッド:
//   pushCapture()          … Java 側の AudioRecord スレッド(単一の書き手)
//   render() / nextStep()  … 音声スレッド(単一の読み手)
//   セッター / 診断        … 制御スレッド(すべて std::atomic 越し)
// リングは SPSC。書き手は write インデックスだけを、読み手は read インデックスだけを
// 進める。相手側は acquire で読むので、ロックは要らない。
//
// 捕獲経路には「生音の漏れ込み」が存在しない(耳に届くのは処理後の音だけ)ため、
// NFR-1 の 10ms 遅延予算は適用されない。走査幅を広く取り(既定 40ms)、跳躍間隔
// (= sweep / |1-比|)を伸ばして大きなシフト量でのアーティファクトを抑える。

#ifndef PRISM_AUDIOBRIDGE_H
#define PRISM_AUDIOBRIDGE_H

#include <atomic>
#include <cmath>
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

    // 出力ゲイン(倍率)。0.5〜4.0 = -6dB〜+12dB。既定 1.0(0dB)。
    // dB <-> 倍率の変換は UI 側(Params.kt)の責務で、ここは倍率だけを受け取る。
    static constexpr float kOutputGainMin = 0.5f;
    static constexpr float kOutputGainMax = 4.0f;
    static constexpr float kOutputGainDefault = 1.0f;

    // ソフトクリップの折れ点。|x| <= threshold はそのまま通し、それを超える分だけ
    // tanh で ±1.0 に漸近させる(3 次多項式ではなく tanh を採用: 単調で飽和が
    // 滑らかで、ゲインをどれだけ上げても発散せず ±1.0 未満に収まる)。
    static constexpr float kSoftClipThreshold = 0.9f;

    // ---- 捕獲経路(AudioPlaybackCapture) ------------------------------------
    // 走査幅の既定。10ms 予算に縛られないので広く取る(理由はファイル冒頭)。
    static constexpr double kCaptureSweepMsDefault = 40.0;
    // リング容量(秒)。Java 側の AudioRecord スレッドが数十 ms 単位でまとめて
    // push してくるため、コールバック 1 回分では足りない。1 秒あれば
    // アプリが一時停止して復帰しても取りこぼさない。
    static constexpr double kCaptureRingSeconds = 1.0;
    // 有効化直後・アンダーラン後にためるクッション(ms)。Java スレッドの
    // スケジューリングジッタを吸収する。
    static constexpr double kCaptureCushionMs = 20.0;
    // 滞留の上限(ms)。これを超えたら古い分を捨ててクッション量まで詰める
    // (捕獲側と出力側のクロックがわずかにずれても遅延が伸び続けないようにする)。
    static constexpr double kCaptureMaxFillMs = 200.0;

    // マイク経路のゲイン(倍率)。0.0 で完全ミュート(捕獲音だけを聞く用途)。
    static constexpr float kMicGainMin = 0.0f;
    static constexpr float kMicGainMax = 2.0f;
    static constexpr float kMicGainDefault = 1.0f;
    // 捕獲経路のゲイン(倍率)。0.0 で無音。
    static constexpr float kCaptureGainMin = 0.0f;
    static constexpr float kCaptureGainMax = 4.0f;
    static constexpr float kCaptureGainDefault = 1.0f;

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
    // micSweepMs / captureSweepMs は各シフタの走査幅(ms)。どちらも PitchShifter 側で
    // [kSweepMsMin, kSweepMsMax] に clamp される。
    // micSweepMs の既定 9.5ms は NFR-1 の 10ms 予算に収まる値(最大遅れ = 8 + sweep)。
    // 広げると遅延と引き換えに低域のピッチ精度と跳躍間隔が改善する — 聴き比べの
    // ために実行時(次の start())から選べるようにしてある。
    bool prepare(double sampleRate, int inputChannels, int outputChannels,
                 double micSweepMs = PitchShifter::kSweepMs,
                 double captureSweepMs = kCaptureSweepMsDefault) {
        prepared_ = false;
        if (!(sampleRate > 0.0) || inputChannels < 1 || outputChannels < 1) {
            return false;
        }
        if (!shifter_.prepare(sampleRate, kMaxCallbackFrames, micSweepMs)) {
            return false;
        }
        // 捕獲経路は遅延制約が無いので広い走査幅で開く。
        if (!captureShifter_.prepare(sampleRate, kMaxCallbackFrames, captureSweepMs)) {
            return false;
        }

        sampleRate_ = sampleRate;
        inputChannels_ = inputChannels;
        outputChannels_ = outputChannels;

        // 捕獲リングの寸法(すべて sampleRate から算出する)。
        ringFrames_ = static_cast<int>(kCaptureRingSeconds * sampleRate);
        // コールバック上限の数倍は必ず確保する(低いサンプルレートでの保険)。
        if (ringFrames_ < kMaxCallbackFrames * 4) {
            ringFrames_ = kMaxCallbackFrames * 4;
        }
        cushionFrames_ = static_cast<int>(kCaptureCushionMs * sampleRate / 1000.0);
        if (cushionFrames_ < 1) {
            cushionFrames_ = 1;
        }
        maxFillFrames_ = static_cast<int>(kCaptureMaxFillMs * sampleRate / 1000.0);
        // 上限はリング容量とクッション量の間に必ず収める。
        if (maxFillFrames_ < cushionFrames_ * 2) {
            maxFillFrames_ = cushionFrames_ * 2;
        }
        if (maxFillFrames_ > ringFrames_ - 2) {
            maxFillFrames_ = ringFrames_ - 2;
        }

        try {
            // 非インタリーブの作業領域: マイク入力 L/R・マイク出力 L/R・
            // 捕獲入力 L/R・捕獲出力 L/R の 8 面。
            planar_.assign(static_cast<std::size_t>(kMaxCallbackFrames) * 8u, 0.0f);
            // 捕獲リング(インタリーブ stereo)。
            captureRing_.assign(static_cast<std::size_t>(ringFrames_) * 2u, 0.0f);
        } catch (...) {
            // 確保失敗は例外を漏らさず false へ変換する(呼び出し側で扱う)。
            planar_.clear();
            planar_.shrink_to_fit();
            captureRing_.clear();
            captureRing_.shrink_to_fit();
            return false;
        }

        inPlanar_[0] = planar_.data();
        inPlanar_[1] = planar_.data() + kMaxCallbackFrames;
        outPlanar_[0] = planar_.data() + kMaxCallbackFrames * 2;
        outPlanar_[1] = planar_.data() + kMaxCallbackFrames * 3;
        capInPlanar_[0] = planar_.data() + kMaxCallbackFrames * 4;
        capInPlanar_[1] = planar_.data() + kMaxCallbackFrames * 5;
        capOutPlanar_[0] = planar_.data() + kMaxCallbackFrames * 6;
        capOutPlanar_[1] = planar_.data() + kMaxCallbackFrames * 7;

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
        captureShifter_.reset();
        for (std::size_t i = 0; i < planar_.size(); ++i) {
            planar_[i] = 0.0f;
        }
        drainRemaining_ = kDrainCallbacks;
        cushionRemaining_ = kInputBurstsCushion;
        underrunCount_.store(0, std::memory_order_relaxed);
        synced_.store(false, std::memory_order_relaxed);

        // 捕獲リングは「読み手を書き手に追いつかせる」形で空にする。書き手
        // (Java の録音スレッド)が同時に走っていても壊れないよう、write 側は触らない。
        captureRead_.store(captureWrite_.load(std::memory_order_acquire),
                           std::memory_order_release);
        captureCushioning_ = true;
        captureActive_ = false;  // 次の render() で captureEnabled_ を見て組み直す
        captureUnderruns_.store(0, std::memory_order_relaxed);
        captureOverruns_.store(0, std::memory_order_relaxed);
        // outputGain_ / micGain_ / captureGain_ / captureEnabled_ はユーザー設定なので
        // reset() では触らない(ストリーム再起動を挟んでも設定が既定値へ戻らないようにする)。
    }

    // ---- 出力ゲイン(制御スレッドから。動作中に呼んでよい) --------------------
    // gain は倍率(0.5〜4.0)。範囲外・非有限値は clamp/既定値に丸める。
    void setOutputGain(float gain) noexcept {
        storeGain(outputGain_, gain, kOutputGainMin, kOutputGainMax, kOutputGainDefault);
    }

    float outputGain() const noexcept { return outputGain_.load(std::memory_order_relaxed); }

    // ---- 経路ごとのゲインと有効化(制御スレッドから。動作中に呼んでよい) ------
    // micGain = 0.0 でマイクを完全ミュートできる(捕獲音だけを聞く)。
    void setMicGain(float gain) noexcept {
        storeGain(micGain_, gain, kMicGainMin, kMicGainMax, kMicGainDefault);
    }
    float micGain() const noexcept { return micGain_.load(std::memory_order_relaxed); }

    void setCaptureGain(float gain) noexcept {
        storeGain(captureGain_, gain, kCaptureGainMin, kCaptureGainMax, kCaptureGainDefault);
    }
    float captureGain() const noexcept { return captureGain_.load(std::memory_order_relaxed); }

    // false のあいだ捕獲経路は無音で、pushCapture() は何も書かずに 0 を返す。
    // 次に true にしたとき、リングは空・シフタは初期状態から始まる
    // (前回の残りが混ざらない)。
    void setCaptureEnabled(bool enabled) noexcept {
        captureEnabled_.store(enabled, std::memory_order_relaxed);
    }
    bool isCaptureEnabled() const noexcept {
        return captureEnabled_.load(std::memory_order_relaxed);
    }

    // ---- 捕獲音の投入(Java の録音スレッドから。単一の書き手であること) -------
    // interleaved: インタリーブ float、channels ch、frames フレーム。
    //              mono なら L/R に複製し、3ch 以上なら先頭 2ch だけ使う。
    // 戻り値: 実際に書けたフレーム数。リングが満杯なら余りは捨て、
    //         captureOverruns() を 1 増やす(呼び出し単位で数える)。
    // ロックも確保も行わない。
    int pushCapture(const float* interleaved, int frames, int channels) noexcept {
        if (!prepared_ || interleaved == nullptr || frames <= 0 || channels < 1) {
            return 0;
        }
        if (!captureEnabled_.load(std::memory_order_relaxed)) {
            return 0;  // 無効中は書かない(再有効化時に空から始めるため)
        }
        int w = captureWrite_.load(std::memory_order_relaxed);
        const int r = captureRead_.load(std::memory_order_acquire);
        // 満杯と空を区別するため 1 フレームは常に空けておく。
        int freeFrames = r - w - 1;
        if (freeFrames < 0) {
            freeFrames += ringFrames_;
        }
        const int n = (frames < freeFrames) ? frames : freeFrames;
        for (int i = 0; i < n; ++i) {
            const std::size_t src =
                static_cast<std::size_t>(i) * static_cast<std::size_t>(channels);
            const float l = interleaved[src];
            const float rr = (channels >= 2) ? interleaved[src + 1] : l;
            const std::size_t dst = static_cast<std::size_t>(w) * 2u;
            captureRing_[dst] = l;
            captureRing_[dst + 1] = rr;
            w = wrapRing(w + 1);
        }
        captureWrite_.store(w, std::memory_order_release);
        if (n < frames) {
            captureOverruns_.fetch_add(1, std::memory_order_relaxed);
        }
        return n;
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

        // 捕獲経路の有効/無効の切り替わりは、コールバックの先頭で 1 回だけ処理する。
        syncCaptureState();

        // numFrames が上限を超えても破綻しないよう分割して処理する。
        int done = 0;
        while (done < numFrames) {
            int chunk = numFrames - done;
            if (chunk > kMaxCallbackFrames) {
                chunk = kMaxCallbackFrames;
            }
            deinterleaveChunk(input, pad, done, chunk);
            shifter_.process(inPlanar_, outPlanar_, chunk);
            if (captureActive_) {
                fetchCaptureChunk(chunk);
                captureShifter_.process(capInPlanar_, capOutPlanar_, chunk);
            }
            mixChunk(chunk);
            interleaveChunk(output, done, chunk);
            done += chunk;
        }
    }

    // ---- 参照 ---------------------------------------------------------------
    PitchShifter& shifter() noexcept { return shifter_; }
    const PitchShifter& shifter() const noexcept { return shifter_; }
    // 捕獲経路のシフタ。シフト量 / dry-wet / クロスフェードは 2 本へ同じ値を流す
    // (PrismEngine のセッターの責務)。走査幅だけが違う。
    PitchShifter& captureShifter() noexcept { return captureShifter_; }
    const PitchShifter& captureShifter() const noexcept { return captureShifter_; }

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

    // ---- 捕獲経路の診断(制御スレッドから。すべて累計値) ---------------------
    // リングが空でコールバックを埋めきれなかった回数。
    int captureUnderruns() const noexcept {
        return captureUnderruns_.load(std::memory_order_relaxed);
    }
    // リングが満杯で pushCapture() が取りこぼした呼び出しの回数。
    int captureOverruns() const noexcept {
        return captureOverruns_.load(std::memory_order_relaxed);
    }
    // 現在リングにたまっているフレーム数(遅延の目安 = これ / sampleRate)。
    int captureFillFrames() const noexcept {
        if (!prepared_) {
            return 0;
        }
        int fill = captureWrite_.load(std::memory_order_relaxed) -
                   captureRead_.load(std::memory_order_relaxed);
        if (fill < 0) {
            fill += ringFrames_;
        }
        return fill;
    }
    // 読み出しを始めるのに必要な滞留フレーム数(kCaptureCushionMs 相当)。
    int captureCushionFrames() const noexcept { return cushionFrames_; }
    int captureRingFrames() const noexcept { return ringFrames_; }
    // 各経路の走査幅(ms)。prepare() が採用した値。
    double micSweepMs() const noexcept { return shifter_.getSweepMs(); }
    double captureSweepMs() const noexcept { return captureShifter_.getSweepMs(); }

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

    // |x| <= kSoftClipThreshold はそのまま、それを超える分は tanh で ±1.0 に
    // 漸近させる。ゲイン適用後にここを通すので、出力が ±1.0 を超えることはない。
    static float softClip(float x) noexcept {
        const float ax = x < 0.0f ? -x : x;
        if (ax <= kSoftClipThreshold) {
            return x;
        }
        const float sign = x < 0.0f ? -1.0f : 1.0f;
        const float excess = (ax - kSoftClipThreshold) / (1.0f - kSoftClipThreshold);
        const float compressed = kSoftClipThreshold + (1.0f - kSoftClipThreshold) * std::tanh(excess);
        return sign * compressed;
    }

    // 2 経路のミックス + 出力ゲイン + ソフトクリップ。結果は outPlanar_ に上書きする。
    //   out = マイク経路 x micGain + 捕獲経路 x captureGain
    // 捕獲が無効なときは capOutPlanar_ を 0 にしてあるので、係数を 0 にして
    // 分岐なしで同じ式を通す(RT 安全: 確保・ロック・I/O 一切なし)。
    void mixChunk(int chunk) noexcept {
        const float micG = micGain_.load(std::memory_order_relaxed);
        const float capG =
            captureActive_ ? captureGain_.load(std::memory_order_relaxed) : 0.0f;
        const float outG = outputGain_.load(std::memory_order_relaxed);
        for (int i = 0; i < chunk; ++i) {
            const float l = outPlanar_[0][i] * micG + capOutPlanar_[0][i] * capG;
            const float r = outPlanar_[1][i] * micG + capOutPlanar_[1][i] * capG;
            outPlanar_[0][i] = softClip(l * outG);
            outPlanar_[1][i] = softClip(r * outG);
        }
    }

    // ---- 捕獲経路(すべて音声スレッドからのみ呼ぶ) ---------------------------
    int wrapRing(int i) const noexcept {
        if (i >= ringFrames_) {
            i -= ringFrames_;
        }
        if (i < 0) {
            i += ringFrames_;
        }
        return i;
    }

    void zeroCapturePlanes() noexcept {
        for (int ch = 0; ch < 2; ++ch) {
            for (int i = 0; i < kMaxCallbackFrames; ++i) {
                capInPlanar_[ch][i] = 0.0f;
                capOutPlanar_[ch][i] = 0.0f;
            }
        }
    }

    // 有効/無効の切り替わりを検出して経路を組み直す。
    //   有効化: シフタを初期状態に戻し、クッションがたまるまで待つ状態から始める
    //           (reset() は音声スレッド自身から呼ぶ分には契約上安全)。有効化 1 回に
    //           つき 1 コールバックだけ、確保を伴わないバッファのゼロ埋め
    //           (48kHz / sweep 40ms / 窓長 200ms で約 140KB、数十マイクロ秒)が乗る。
    //           前回の残響を持ち込まないための代償として許容する。
    //   無効化: リングを読み捨てて空にする。書き手は無効中に書かないので、
    //           次の有効化時はここで空にした状態から始まる。
    // 「空にする」を無効化側に置くのが要点 — 有効化側で空にすると、制御スレッドが
    // setCaptureEnabled(true) してから最初のコールバックが走るまでに書かれた
    // 数 ms ぶんを捨ててしまう。
    void syncCaptureState() noexcept {
        const bool enabled = captureEnabled_.load(std::memory_order_relaxed);
        if (enabled == captureActive_) {
            return;
        }
        captureActive_ = enabled;
        captureCushioning_ = true;
        zeroCapturePlanes();
        if (enabled) {
            captureShifter_.reset();
        } else {
            captureRead_.store(captureWrite_.load(std::memory_order_acquire),
                               std::memory_order_release);
        }
    }

    // リングから chunk フレーム取り出して capInPlanar_ を埋める。
    // 状態機械: クッションがたまるまで無音 -> 毎回 chunk ぶん pop ->
    //           足りなければ無音で埋めてクッション待ちへ戻る。
    void fetchCaptureChunk(int chunk) noexcept {
        const int w = captureWrite_.load(std::memory_order_acquire);
        int r = captureRead_.load(std::memory_order_relaxed);
        int fill = w - r;
        if (fill < 0) {
            fill += ringFrames_;
        }

        if (captureCushioning_) {
            if (fill < cushionFrames_) {
                for (int i = 0; i < chunk; ++i) {
                    capInPlanar_[0][i] = 0.0f;
                    capInPlanar_[1][i] = 0.0f;
                }
                return;  // まだためる。read は進めない。
            }
            captureCushioning_ = false;
        }

        // ドリフト対策: 滞留が上限を超えたら古い分を捨ててクッション量まで詰める。
        if (fill > maxFillFrames_) {
            const int drop = fill - cushionFrames_;
            r = wrapRing(r + drop);
            fill -= drop;
        }

        const int avail = (fill < chunk) ? fill : chunk;
        const int pad = chunk - avail;
        // 足りない分は「古い側」を 0 で埋める(最新サンプルを最新の出力フレームに
        // 合わせる。マイク経路の pad と同じ考え方)。
        for (int i = 0; i < pad; ++i) {
            capInPlanar_[0][i] = 0.0f;
            capInPlanar_[1][i] = 0.0f;
        }
        for (int i = 0; i < avail; ++i) {
            const std::size_t base = static_cast<std::size_t>(r) * 2u;
            capInPlanar_[0][pad + i] = captureRing_[base];
            capInPlanar_[1][pad + i] = captureRing_[base + 1];
            r = wrapRing(r + 1);
        }
        captureRead_.store(r, std::memory_order_release);

        if (pad > 0) {
            captureUnderruns_.fetch_add(1, std::memory_order_relaxed);
            captureCushioning_ = true;  // たまり直すまで待つ
        }
    }

    // 出力ゲインの共通クランプ(非有限値は既定値へ丸める)。
    static void storeGain(std::atomic<float>& slot, float gain, float lo, float hi,
                          float fallback) noexcept {
        if (!std::isfinite(gain)) {
            gain = fallback;
        }
        if (gain < lo) {
            gain = lo;
        } else if (gain > hi) {
            gain = hi;
        }
        slot.store(gain, std::memory_order_relaxed);
    }

    void fillSilence(float* output, int numFrames) const noexcept {
        const std::size_t n = static_cast<std::size_t>(numFrames) *
                              static_cast<std::size_t>(outputChannels_ > 0 ? outputChannels_ : 1);
        for (std::size_t i = 0; i < n; ++i) {
            output[i] = 0.0f;
        }
    }

    // SPSC リングのインデックスをロックフリー atomic に要求する。
    static_assert(std::atomic<int>::is_always_lock_free,
                  "prism requires lock-free int atomics (SPSC capture ring)");

    PitchShifter shifter_;         // マイク経路(走査幅 9.5ms = 10ms 予算内)
    PitchShifter captureShifter_;  // 捕獲経路(走査幅は prepare の引数、既定 40ms)

    std::vector<float> planar_;
    float* inPlanar_[2] = {nullptr, nullptr};
    float* outPlanar_[2] = {nullptr, nullptr};
    float* capInPlanar_[2] = {nullptr, nullptr};
    float* capOutPlanar_[2] = {nullptr, nullptr};

    // 捕獲リング(インタリーブ stereo、prepare で確保・以後サイズ不変)。
    std::vector<float> captureRing_;
    int ringFrames_ = 0;
    int cushionFrames_ = 0;
    int maxFillFrames_ = 0;
    std::atomic<int> captureWrite_{0};  // 書き手 = Java の録音スレッドだけが進める
    std::atomic<int> captureRead_{0};   // 読み手 = 音声スレッドだけが進める
    std::atomic<int> captureUnderruns_{0};
    std::atomic<int> captureOverruns_{0};
    std::atomic<bool> captureEnabled_{false};
    std::atomic<float> micGain_{kMicGainDefault};
    std::atomic<float> captureGain_{kCaptureGainDefault};
    // 音声スレッド専用の状態(制御スレッドからは読まない)。
    bool captureActive_ = false;
    bool captureCushioning_ = true;

    double sampleRate_ = 0.0;
    int inputChannels_ = 0;
    int outputChannels_ = 0;

    int drainRemaining_ = kDrainCallbacks;
    int cushionRemaining_ = kInputBurstsCushion;

    std::atomic<int> underrunCount_{0};
    std::atomic<bool> synced_{false};
    std::atomic<float> outputGain_{kOutputGainDefault};

    bool prepared_ = false;
};

}  // namespace prism

#endif  // PRISM_AUDIOBRIDGE_H
