// prism::AudioBridge — 全二重オーディオコールバックの「中身」。
//
// レイヤ分離(team.md「Code Style」):
//   DSP コア (dsp/include/prism/PitchShifter.h)  … 純 C++、依存ゼロ
//   → AudioBridge (このファイル)                … 純 C++、Oboe/JNI 非依存。ホストでビルド・テスト可能
//     → PrismEngine.{h,cpp}                     … Oboe のストリーム生存管理
//       → jni_bridge.cpp                        … JNI
//
// ここが受け持つのは 5 つ:
//   (1) 起動時の入出力同期の状態機械(Drain -> Cushion -> Render)
//   (2) インタリーブ <-> 非インタリーブ変換とモノ複製、そして PitchShifter の駆動
//   (3) 捕獲音(AudioPlaybackCapture で拾った他アプリの再生音)を受け取る
//       ロックフリー SPSC リングと、その読み出し状態機械
//   (4) マイク経路と捕獲経路のミックス
//   (5) 診断用の 10 秒録音(4 本: 捕獲 in/out・マイク in/out)。固定バッファへの
//       memcpy だけを音声スレッドで行い、WAV エンコードや MediaStore 保存は
//       すべて制御スレッド側(Kotlin の DiagnosticRecorder)の責務にする。
//
// render() / nextStep() / reportDrain() はオーディオコールバックから呼ばれる。
// したがってこの 3 つは リアルタイム安全: ヒープ確保/解放・ロック・I/O・ログ・
// システムコール・例外を一切行わない(CLAUDE.md「リアルタイムオーディオの鉄則」)。
// バッファは prepare() でのみ確保し、以後サイズを変えない。
//
// スレッド:
//   pushCapture()          … Java 側の AudioRecord スレッド(捕獲入力リングの書き手)
//   捕獲ワーカースレッド    … 捕獲入力リングの読み手 兼 捕獲ステージリングの書き手
//   render() / nextStep()  … 音声スレッド(マイク入力とステージリングの読み手)
//   セッター / 診断        … 制御スレッド(すべて std::atomic 越し)
// リングは 2 本とも SPSC。書き手は write インデックスだけを、読み手は read
// インデックスだけを進める。相手側は acquire で読むので、ロックは要らない。
// 「リングを空にする」操作も必ず読み手側が行う(read を write に追いつかせる)。
//
// 捕獲経路には「生音の漏れ込み」が存在しない(耳に届くのは処理後の音だけ)ため、
// NFR-1 の 10ms 遅延予算は適用されない。走査幅を広く取り(既定 40ms)、跳躍間隔
// (= sweep / |1-比|)を伸ばして大きなシフト量でのアーティファクトを抑える。
//
// ---- 捕獲経路は専用ワーカースレッド(v0.6.1) --------------------------------
// 捕獲経路の処理(とくに位相ボコーダ)を出力コールバックの中で回すと、コールバック
// 1 回あたりの処理時間が跳ね上がり、締切を落として xrun になる(Pixel 9a 実機で
// 毎秒 20〜40 回)。捕獲経路は遅延予算の対象外なので、処理そのものを音声スレッドから
// 追い出す:
//
//   Java 録音スレッド  --push-->  捕獲入力リング(SPSC)
//   捕獲ワーカー       --pop--->  1024 フレーム単位で処理(方式切替・クロスフェード込み)
//                      --push-->  捕獲ステージリング(SPSC。生音と処理後を同じ位置へ)
//   音声スレッド       --pop--->  ミックスして出力(コールバックは FIFO を読むだけ)
//
// ワーカーはコールバックではないので、待機に条件変数を使ってよい(ロック可)。
// 音声スレッド側はこれまでどおり非ブロッキングのまま。追加遅延は
// 「ブロック長(1024)+ ステージリングのクッション」で、captureExtraLatencyMillis()
// が返す。マイク経路は従来どおりコールバック内で処理する(低遅延が本分)。
//
// ---- 処理方式の選択(v0.6.0。dsp/include/prism/PhaseVocoderShifter.h 冒頭コメント参照) --
// マイク経路・捕獲経路それぞれに、ディレイライン型(prism::PitchShifter)と
// 位相ボコーダ(prism::PhaseVocoderShifter、N=2048/4096)の 3 方式を選べるようにする。
// 「何を使うかはユーザーに託す」方針のため、DSP 側でどれかに決め打ちはしない。
//
//   * 3 方式ぶんのインスタンスは prepare() で経路ごとに 3 つとも確保しておく
//     (切替のたびにヒープ確保が走るのを避ける — RT 安全の鉄則そのもの)。
//   * 選択は std::atomic<int> 越しに制御スレッドから渡し、音声スレッド側の
//     runPathChunk() が「要求値と現在値が違う」ことを検出して切り替える。
//   * 切替の瞬間にクリックが出ないよう、旧方式の出力を kMethodSwitchCrossfadeMs
//     (既定 10ms)だけ新方式の出力へ線形クロスフェードする。定常状態では選択中の
//     1 方式だけを処理する(3 方式を常時並走させない)。CPU 比は README「処理方式」
//     節を参照 — 切替の瞬間だけ一時的に 2 方式ぶんの負荷になる。
//   * 新方式へ切り替える瞬間、その方式のインスタンスは reset() してから使う。
//     休眠中に古い音声が内部バッファへ残っていると再開時に紛れ込むため。
//     位相ボコーダへ切り替えた直後は、そのインスタンス自身の遅延(55ms/110ms)ぶん
//     出力が小さいまま滑らかに立ち上がる — これはクリックではなく本来の遅延特性。

#ifndef PRISM_AUDIOBRIDGE_H
#define PRISM_AUDIOBRIDGE_H

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/time.h>
#endif

#include "prism/PhaseVocoderShifter.h"
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
    // アプリが一時停止して復帰しても取りこぼさない。入力リングとステージリングの
    // どちらもこの容量で確保する。
    static constexpr double kCaptureRingSeconds = 1.0;
    // 捕獲ワーカーの処理単位(フレーム)。位相ボコーダのホップより十分大きく取り、
    // 起床 1 回あたりの処理をまとめる。48kHz で 21.3ms 相当。
    static constexpr int kCaptureWorkerBlockFrames = 1024;
    // ステージリングのクッションのうち、ブロック長に上乗せする余裕(ms)。
    // ステージリングへはブロック単位(= 21.3ms ごと)にしかデータが積まれないため、
    // クッションはブロック 1 個ぶん + この余裕にする。これを下回らせると、
    // ブロックの到着直前に必ずリングが枯れてアンダーランになる。
    static constexpr double kCaptureStageCushionMs = 10.0;
    // 滞留の上限(ms)。これを超えたら古い分を捨てて詰める(捕獲側と出力側の
    // クロックがわずかにずれても遅延が伸び続けないようにする)。入力リングは
    // ワーカーが、ステージリングは音声スレッドが、それぞれ自分の read 側で行う。
    static constexpr double kCaptureMaxFillMs = 200.0;

    // マイク経路のゲイン(倍率)。0.0 で完全ミュート(捕獲音だけを聞く用途)。
    static constexpr float kMicGainMin = 0.0f;
    static constexpr float kMicGainMax = 2.0f;
    static constexpr float kMicGainDefault = 1.0f;
    // 捕獲経路のゲイン(倍率)。0.0 で無音。
    static constexpr float kCaptureGainMin = 0.0f;
    static constexpr float kCaptureGainMax = 4.0f;
    static constexpr float kCaptureGainDefault = 1.0f;

    // ---- 処理方式(経路ごとに独立選択) ---------------------------------------
    static constexpr int kMethodDelayLine = 0;         // prism::PitchShifter(低遅延)
    static constexpr int kMethodPhaseVocoder2048 = 1;  // prism::PhaseVocoderShifter N=2048
    static constexpr int kMethodPhaseVocoder4096 = 2;  // prism::PhaseVocoderShifter N=4096
    static constexpr int kMethodCount = 3;
    // マイク経路の既定は低遅延(生音の漏れ込みがあるため)。捕獲経路の既定は
    // 音楽向けの位相ボコーダ N=4096(生音の漏れ込みが無く、遅延を許容できるため)。
    static constexpr int kMicMethodDefault = kMethodDelayLine;
    static constexpr int kCaptureMethodDefault = kMethodPhaseVocoder4096;
    // 方式切替時のクロスフェード長(ms)。固定値 — dry-wet 用のクロスフェード
    // (PitchShifter::kCrossfadeMsDefault、跳躍のつなぎ目用)とは別物。
    static constexpr float kMethodSwitchCrossfadeMs = 10.0f;

    // ---- 診断用の 10 秒録音 --------------------------------------------------
    // 4 本(捕獲 in / 捕獲 out / マイク in / マイク out)を同時刻に開始して
    // この秒数ぶん記録する。開始(バッファ確保)は制御スレッドから
    // (startDiagnosticRecording())、書き込みは音声スレッドから(render() 内)。
    static constexpr double kDiagRecordSeconds = 10.0;

    // オーディオコールバックが今回やるべきこと。
    enum class Step {
        DrainInput,    // 入力を空になるまで read して捨てる。出力は無音。
        BuildCushion,  // 入力を read しない。出力は無音。
        Render,        // 入力を read して render() に渡す。
    };

    AudioBridge() = default;
    // ワーカースレッドを持つのでコピー・ムーブはしない。
    AudioBridge(const AudioBridge&) = delete;
    AudioBridge& operator=(const AudioBridge&) = delete;
    ~AudioBridge() { stopCaptureWorker(); }

    // ---- 初期化(音声スレッド停止中に呼ぶ。ここだけがヒープ確保を行う) --------
    // 捕獲ワーカーも停止中であること(prepare() はリングの読み書き位置と
    // シフタの状態を作り直すため)。PrismEngine は stopCaptureWorker() 済みの
    // 状態からしか prepare() を呼ばない。
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
        // 3 方式のうち残り 2 つ(位相ボコーダ N=2048/4096)も経路ごとに確保しておく。
        // 「切替のたびに確保しない」ため、選択されていなくても常に prepare() する。
        if (!micPv2048_.prepare(sampleRate, kMaxCallbackFrames,
                                PhaseVocoderShifter::kFftSizeDefault) ||
            !micPv4096_.prepare(sampleRate, kMaxCallbackFrames,
                                PhaseVocoderShifter::kFftSizeLarge) ||
            !capturePv2048_.prepare(sampleRate, kMaxCallbackFrames,
                                    PhaseVocoderShifter::kFftSizeDefault) ||
            !capturePv4096_.prepare(sampleRate, kMaxCallbackFrames,
                                    PhaseVocoderShifter::kFftSizeLarge)) {
            return false;
        }
        // 捕獲経路の dry-wet は常に 1.0(全 wet)固定。捕獲経路には「イヤホンから
        // 漏れる生音」に相当するものが無く、dry 成分は原音そのもの(= 二重再生)に
        // なってしまうため、UI の原音まぜ(Dry/Wet)設定はマイク経路にしか流さない
        // (PrismEngine::setDryWet 参照)。ここで明示しておくことで prepare() の
        // 呼び直しでも既定に戻ることを保証する。捕獲経路の 3 方式すべてに適用する。
        captureShifter_.setDryWet(PitchShifter::kDryWetDefault);
        capturePv2048_.setDryWet(PhaseVocoderShifter::kDryWetDefault);
        capturePv4096_.setDryWet(PhaseVocoderShifter::kDryWetDefault);

        sampleRate_ = sampleRate;
        inputChannels_ = inputChannels;
        outputChannels_ = outputChannels;

        // 方式切替のクロスフェード長(サンプル)。0 除算を避けるため最低 1。
        methodFadeTotalSamples_ =
            static_cast<int>(kMethodSwitchCrossfadeMs * sampleRate / 1000.0 + 0.5);
        if (methodFadeTotalSamples_ < 1) {
            methodFadeTotalSamples_ = 1;
        }

        // 捕獲リングの寸法(すべて sampleRate から算出する)。
        ringFrames_ = static_cast<int>(kCaptureRingSeconds * sampleRate);
        // コールバック上限とワーカーのブロック長の数倍は必ず確保する
        // (低いサンプルレートでの保険)。
        const int minRing = (kMaxCallbackFrames > kCaptureWorkerBlockFrames
                                 ? kMaxCallbackFrames
                                 : kCaptureWorkerBlockFrames) * 4;
        if (ringFrames_ < minRing) {
            ringFrames_ = minRing;
        }
        // ステージリングのクッション = ワーカーのブロック長 + 余裕。ステージリングへは
        // ブロック単位でしか積まれないので、ブロック 1 個ぶんを下回らせてはいけない。
        cushionFrames_ = kCaptureWorkerBlockFrames +
                         static_cast<int>(kCaptureStageCushionMs * sampleRate / 1000.0);
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
            // 捕獲入力 L/R・捕獲出力 L/R(ここまで音声スレッド)・マイク旧方式出力 L/R
            // (切替クロスフェード用のスクラッチ)・捕獲旧方式出力 L/R・
            // ワーカー入力 L/R・ワーカー出力 L/R(ここから捕獲ワーカー)の 16 面。
            planar_.assign(static_cast<std::size_t>(kMaxCallbackFrames) * 16u, 0.0f);
            // 捕獲入力リング(インタリーブ stereo)。Java 録音スレッド -> ワーカー。
            captureRing_.assign(static_cast<std::size_t>(ringFrames_) * 2u, 0.0f);
            // 捕獲ステージリング。ワーカー -> 音声スレッド。1 フレームあたり 4 float
            // (生 L / 生 R / 処理後 L / 処理後 R)。生音も同じ位置へ並べておくことで、
            // 診断録音の capture-in と capture-out が必ず同じ時刻で揃う。
            captureStageRing_.assign(static_cast<std::size_t>(ringFrames_) * 4u, 0.0f);
        } catch (...) {
            // 確保失敗は例外を漏らさず false へ変換する(呼び出し側で扱う)。
            planar_.clear();
            planar_.shrink_to_fit();
            captureRing_.clear();
            captureRing_.shrink_to_fit();
            captureStageRing_.clear();
            captureStageRing_.shrink_to_fit();
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
        micOldPlanar_[0] = planar_.data() + kMaxCallbackFrames * 8;
        micOldPlanar_[1] = planar_.data() + kMaxCallbackFrames * 9;
        captureOldPlanar_[0] = planar_.data() + kMaxCallbackFrames * 10;
        captureOldPlanar_[1] = planar_.data() + kMaxCallbackFrames * 11;
        workInPlanar_[0] = planar_.data() + kMaxCallbackFrames * 12;
        workInPlanar_[1] = planar_.data() + kMaxCallbackFrames * 13;
        workOutPlanar_[0] = planar_.data() + kMaxCallbackFrames * 14;
        workOutPlanar_[1] = planar_.data() + kMaxCallbackFrames * 15;

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
        micPv2048_.reset();
        micPv4096_.reset();
        captureShifter_.reset();
        capturePv2048_.reset();
        capturePv4096_.reset();
        for (std::size_t i = 0; i < planar_.size(); ++i) {
            planar_[i] = 0.0f;
        }
        drainRemaining_ = kDrainCallbacks;
        cushionRemaining_ = kInputBurstsCushion;
        underrunCount_.store(0, std::memory_order_relaxed);
        micShortfallFrames_.store(0, std::memory_order_relaxed);
        synced_.store(false, std::memory_order_relaxed);

        // 方式切替の状態機械を、いま要求されている方式から始める(既定値へ戻すのでは
        // なく、ユーザーが選んでいた方式を尊重する)。フェード中ではないので直ちに
        // その方式だけが有効になる — start() 直後にいきなりクロスフェードは起きない。
        micActiveMethod_ = clampMethod(micMethod_.load(std::memory_order_relaxed));
        micFadeFromMethod_ = -1;
        micFadeRemaining_ = 0;
        captureActiveMethod_ = clampMethod(captureMethod_.load(std::memory_order_relaxed));
        captureFadeFromMethod_ = -1;
        captureFadeRemaining_ = 0;

        // 診断録音は毎回の start() で必ず未開始状態へ戻す。ベクタの解放も含めて
        // ここで行ってよい(prepare() 経由でのみ呼ばれ、音声スレッドはまだ走っていない)。
        diagActive_.store(false, std::memory_order_relaxed);
        diagDone_.store(false, std::memory_order_relaxed);
        diagWritten_.store(0, std::memory_order_relaxed);
        diagTotalFrames_ = 0;
        diagCaptureIn_.clear();
        diagCaptureIn_.shrink_to_fit();
        diagCaptureOut_.clear();
        diagCaptureOut_.shrink_to_fit();
        diagMicIn_.clear();
        diagMicIn_.shrink_to_fit();
        diagMicOut_.clear();
        diagMicOut_.shrink_to_fit();

        // 捕獲入力リングは「読み手を書き手に追いつかせる」形で空にする。書き手
        // (Java の録音スレッド)が同時に走っていても壊れないよう、write 側は触らない。
        captureRead_.store(captureWrite_.load(std::memory_order_acquire),
                           std::memory_order_release);
        // ステージリングはワーカーが停止している前提で作り直してよい(prepare() の
        // 契約。呼び出し側は stopCaptureWorker() 済みであること)。
        captureStageWrite_.store(0, std::memory_order_relaxed);
        captureStageRead_.store(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < captureStageRing_.size(); ++i) {
            captureStageRing_[i] = 0.0f;
        }
        // 世代番号は 3 者とも同じ値から始める(prepare() は音声スレッドもワーカーも
        // 止まっている前提なので、ここで揃えてよい)。
        captureEpoch_.store(0, std::memory_order_relaxed);
        workerEpoch_ = 0;
        audioEpoch_ = 0;
        workerActive_ = captureEnabled_.load(std::memory_order_relaxed);
        captureCushioning_ = true;
        // 捕獲が有効なままの再 prepare() では、最初の render() で世代が一致するため
        // ここで実際の値に合わせておく(無効なら次の有効化で世代が進む)。
        captureActive_ = workerActive_;
        captureUnderruns_.store(0, std::memory_order_relaxed);
        captureOverruns_.store(0, std::memory_order_relaxed);
        captureShortfallFramesTotal_.store(0, std::memory_order_relaxed);
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

    // ---- 処理方式(制御スレッドから。動作中に呼んでよい。再起動不要) -----------
    // kMethodDelayLine / kMethodPhaseVocoder2048 / kMethodPhaseVocoder4096。
    // 範囲外の値は clampMethod() で低遅延(既定)に丸める。実際の切替は音声スレッド側
    // (runPathChunk())が次のチャンクで検出し、kMethodSwitchCrossfadeMs だけかけて
    // クロスフェードする。
    void setMicMethod(int method) noexcept {
        micMethod_.store(clampMethod(method), std::memory_order_relaxed);
    }
    int micMethod() const noexcept { return micMethod_.load(std::memory_order_relaxed); }

    void setCaptureMethod(int method) noexcept {
        captureMethod_.store(clampMethod(method), std::memory_order_relaxed);
    }
    int captureMethod() const noexcept { return captureMethod_.load(std::memory_order_relaxed); }

    // ---- シフト量 / dry-wet を経路の 3 方式すべてへ流す ------------------------
    // どの方式がいま選ばれていても正しい値で鳴るように、常に 3 インスタンスとも
    // 更新する(切替やクロスフェード中に値がずれないようにするため)。
    void setMicShiftCentsL(float cents) noexcept {
        shifter_.setShiftCentsL(cents);
        micPv2048_.setShiftCentsL(cents);
        micPv4096_.setShiftCentsL(cents);
    }
    void setMicShiftCentsR(float cents) noexcept {
        shifter_.setShiftCentsR(cents);
        micPv2048_.setShiftCentsR(cents);
        micPv4096_.setShiftCentsR(cents);
    }
    void setCaptureShiftCentsL(float cents) noexcept {
        captureShifter_.setShiftCentsL(cents);
        capturePv2048_.setShiftCentsL(cents);
        capturePv4096_.setShiftCentsL(cents);
    }
    void setCaptureShiftCentsR(float cents) noexcept {
        captureShifter_.setShiftCentsR(cents);
        capturePv2048_.setShiftCentsR(cents);
        capturePv4096_.setShiftCentsR(cents);
    }
    // マイク経路の dry-wet のみ UI から可変(捕獲経路は prepare() で常に 1.0 固定。
    // 上の prepare() 内コメント参照)。
    void setMicDryWet(float mix) noexcept {
        shifter_.setDryWet(mix);
        micPv2048_.setDryWet(mix);
        micPv4096_.setDryWet(mix);
    }

    // ---- 経路ごとの DSP 遅延(制御スレッドから。1 秒に 1 回程度を想定) ----------
    // 「いま要求されている方式」(= micMethod()/captureMethod())の遅延を返す。
    // クロスフェード中の一時的な過渡(最大 kMethodSwitchCrossfadeMs)は無視する —
    // 音声スレッド専用のクロスフェード状態を制御スレッドから読むと競合するため、
    // 要求値ベースで十分な近似としている。
    double micDspLatencyMillis() const noexcept {
        return latencyMillisForMethod(micMethod(), shifter_, micPv2048_, micPv4096_);
    }
    double captureDspLatencyMillis() const noexcept {
        return latencyMillisForMethod(captureMethod(), captureShifter_, capturePv2048_,
                                      capturePv4096_);
    }

    // false のあいだ捕獲経路は無音で、pushCapture() は何も書かずに 0 を返す。
    // 次に true にしたとき、リングは空・シフタは初期状態から始まる
    // (前回の残りが混ざらない)。
    // 切り替えのたびに世代番号を進める。音声スレッドとワーカーは「値が違うか」では
    // なく「世代が変わったか」で切り替わりを検出する — 値だけを見ていると、相手が
    // 観測する前に OFF -> ON と往復した場合に取りこぼし、前回の残りが混ざる。
    void setCaptureEnabled(bool enabled) noexcept {
        const bool previous = captureEnabled_.exchange(enabled, std::memory_order_acq_rel);
        if (previous != enabled) {
            captureEpoch_.fetch_add(1, std::memory_order_release);
        }
        // 有効/無効の切り替わりをワーカーにすぐ処理させる(制御スレッドなのでロック可)。
        notifyCaptureWorker();
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
        if (n > 0) {
            // 捕獲ワーカーを起こす。ここは Java の録音スレッド(ブロッキング read で
            // 回っている通常スレッド)であり、オーディオコールバックではないので
            // ロックを取ってよい。
            notifyCaptureWorker();
        }
        return n;
    }

    // ---- 捕獲ワーカー(制御スレッドから) --------------------------------------
    // 捕獲経路の処理をオーディオコールバックの外へ出すためのスレッド。
    // prepare() の後・ストリーム開始の前に start、ストリームを閉じた後に stop する。
    // 二重 start は無害(既に動いていれば true を返すだけ)。
    // 確保に失敗したら false を返す(捕獲経路が無音になるだけで、マイク経路は動く)。
    bool startCaptureWorker() {
        if (workerThread_.joinable()) {
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(workerMutex_);
            workerQuit_ = false;
            workerWake_ = false;
        }
        try {
            workerThread_ = std::thread([this] { captureWorkerLoop(); });
        } catch (...) {
            return false;
        }
        return true;
    }

    void stopCaptureWorker() noexcept {
        if (!workerThread_.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(workerMutex_);
            workerQuit_ = true;
            workerWake_ = true;
        }
        workerCv_.notify_all();
        workerThread_.join();
    }

    bool isCaptureWorkerRunning() const noexcept { return workerThread_.joinable(); }

    // テスト専用: ワーカー本体を呼び出しスレッドで 1 回だけ回す。
    // ホストスモークはスレッドを起こさずにこれを呼ぶことで、タイミングに依存しない
    // 決定的な検査ができる(実機では captureWorkerLoop() が同じ関数を回す)。
    void pumpCaptureWorkerForTesting() noexcept { processCaptureBlocks(); }

    // ---- 診断用の 10 秒録音(制御スレッドから。ここだけがヒープ確保/解放を行う) ---
    // 4 本(捕獲 in / 捕獲 out / マイク in / マイク out)を同時刻に開始して記録する。
    // 書き込みは音声スレッド(render() 内)が行う。二重起動はしない。
    // 成功したら true。prepared_ でない、またはヒープ確保に失敗したら false。
    bool startDiagnosticRecording() {
        if (!prepared_ || diagActive_.load(std::memory_order_relaxed)) {
            return false;
        }
        const int totalFrames = static_cast<int>(kDiagRecordSeconds * sampleRate_ + 0.5);
        if (totalFrames <= 0) {
            return false;
        }
        const int micCh = inputChannels_ > 0 ? inputChannels_ : 1;
        try {
            diagCaptureIn_.assign(static_cast<std::size_t>(totalFrames) * 2u, 0.0f);
            diagCaptureOut_.assign(static_cast<std::size_t>(totalFrames) * 2u, 0.0f);
            diagMicIn_.assign(static_cast<std::size_t>(totalFrames) * static_cast<std::size_t>(micCh),
                              0.0f);
            diagMicOut_.assign(static_cast<std::size_t>(totalFrames) * 2u, 0.0f);
        } catch (...) {
            diagCaptureIn_.clear();
            diagCaptureIn_.shrink_to_fit();
            diagCaptureOut_.clear();
            diagCaptureOut_.shrink_to_fit();
            diagMicIn_.clear();
            diagMicIn_.shrink_to_fit();
            diagMicOut_.clear();
            diagMicOut_.shrink_to_fit();
            return false;
        }
        diagTotalFrames_ = totalFrames;
        diagWritten_.store(0, std::memory_order_relaxed);
        diagDone_.store(false, std::memory_order_relaxed);
        // release: 上のベクタへの書き込みが、これを acquire で見る音声スレッドから
        // 必ず見えるようにする(音声スレッドはこの flag 越しにしかポインタへ触れない)。
        diagActive_.store(true, std::memory_order_release);
        return true;
    }

    // 録音を打ち切る(atomic 1 個への書き込みだけなので、音声スレッドが走っていても
    // 安全に呼べる)。バッファそのものは解放しない — 解放していいのは音声スレッドが
    // 確実に止まっている状況に限られるため、実際の解放は releaseDiagnosticRecording()
    // (完了検知後)または次の startDiagnosticRecording()/prepare() が行う。
    void cancelDiagnosticRecording() noexcept {
        diagActive_.store(false, std::memory_order_relaxed);
    }

    // 完了(isDiagnosticRecordingDone() == true)を確認した後にだけ呼ぶこと。
    // その時点で音声スレッドはもうこのバッファに触れないと保証されているので、
    // 制御スレッドから解放してよい。
    void releaseDiagnosticRecording() noexcept {
        diagCaptureIn_.clear();
        diagCaptureIn_.shrink_to_fit();
        diagCaptureOut_.clear();
        diagCaptureOut_.shrink_to_fit();
        diagMicIn_.clear();
        diagMicIn_.shrink_to_fit();
        diagMicOut_.clear();
        diagMicOut_.shrink_to_fit();
        diagWritten_.store(0, std::memory_order_relaxed);
        diagDone_.store(false, std::memory_order_relaxed);
        diagTotalFrames_ = 0;
    }

    bool isDiagnosticRecordingActive() const noexcept {
        return diagActive_.load(std::memory_order_acquire);
    }
    // 10 秒ぶん書き終わり、音声スレッドがもう触れない状態になったら true。
    bool isDiagnosticRecordingDone() const noexcept {
        return diagDone_.load(std::memory_order_acquire);
    }
    int diagnosticTotalFrames() const noexcept { return diagTotalFrames_; }
    // 以下はすべて isDiagnosticRecordingDone() == true の間にだけ読むこと。
    // 空なら nullptr(startDiagnosticRecording() が一度も成功していない等)。
    const float* diagnosticCaptureIn() const noexcept {
        return diagCaptureIn_.empty() ? nullptr : diagCaptureIn_.data();
    }
    const float* diagnosticCaptureOut() const noexcept {
        return diagCaptureOut_.empty() ? nullptr : diagCaptureOut_.data();
    }
    const float* diagnosticMicIn() const noexcept {
        return diagMicIn_.empty() ? nullptr : diagMicIn_.data();
    }
    const float* diagnosticMicOut() const noexcept {
        return diagMicOut_.empty() ? nullptr : diagMicOut_.data();
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

        const int pad = numFrames - framesRead;
        if (pad > 0) {
            underrunCount_.fetch_add(1, std::memory_order_relaxed);
            micShortfallFrames_.fetch_add(pad, std::memory_order_relaxed);
        }

        // 捕獲経路の有効/無効の切り替わりは、コールバックの先頭で 1 回だけ処理する。
        syncCaptureState();

        // このコールバックの間だけ有効/無効を固定する(チャンクの途中で
        // startDiagnosticRecording() が飛び込んできても、書き込み先の一貫性を保つ)。
        const bool recording = diagActive_.load(std::memory_order_relaxed);

        // numFrames が上限を超えても破綻しないよう分割して処理する。
        int done = 0;
        while (done < numFrames) {
            int chunk = numFrames - done;
            if (chunk > kMaxCallbackFrames) {
                chunk = kMaxCallbackFrames;
            }
            deinterleaveChunk(input, pad, done, chunk);
            runMicPath(chunk);
            if (captureActive_) {
                // 捕獲経路の処理はワーカースレッドが済ませてある。ここは
                // ステージリングから生音と処理後の音を取り出すだけ。
                fetchCaptureStageChunk(chunk);
            }
            if (recording) {
                recordDiagnosticChunk(input, pad, done, chunk);
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

    // DSP 側の設計値遅延(ミリ秒)。マイク経路の値(= micDspLatencyMillis() の別名。
    // 既存呼び出し元(PrismEngine::latency()・ホストスモーク)との互換のため残す)。
    double dspLatencyMillis() const noexcept { return micDspLatencyMillis(); }

    // 入力アンダーランの累計。診断用(UI に出す)。RT 経路からは relaxed で加算のみ。
    int underrunCount() const noexcept { return underrunCount_.load(std::memory_order_relaxed); }
    // マイク入力側で「要求フレーム数に足りなかった」不足フレームの累計
    // (underrunCount() は回数、こちらはフレーム数)。start() のたびにリセットされる。
    int micShortfallFrames() const noexcept {
        return micShortfallFrames_.load(std::memory_order_relaxed);
    }

    // ---- 捕獲経路の診断(制御スレッドから。すべて累計値) ---------------------
    // リングが空でコールバックを埋めきれなかった回数。
    int captureUnderruns() const noexcept {
        return captureUnderruns_.load(std::memory_order_relaxed);
    }
    // リングが満杯で pushCapture() が取りこぼした呼び出しの回数。
    int captureOverruns() const noexcept {
        return captureOverruns_.load(std::memory_order_relaxed);
    }
    // 捕獲リング側で「要求フレーム数に足りなかった」不足フレームの累計
    // (captureUnderruns() は回数、こちらはフレーム数)。start() のたびにリセットされる。
    int captureShortfallFrames() const noexcept {
        return captureShortfallFramesTotal_.load(std::memory_order_relaxed);
    }
    // 捕獲経路に滞留しているフレーム数の合計(入力リング + ステージリング)。
    // 遅延の目安 = これ / sampleRate。UI の「捕獲中かどうか」の判定にも使う。
    int captureFillFrames() const noexcept {
        return captureInputFillFrames() + captureStageFillFrames();
    }
    // 入力リング(Java 録音スレッド -> ワーカー)の滞留。
    int captureInputFillFrames() const noexcept {
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
    // ステージリング(ワーカー -> 音声スレッド)の滞留。
    int captureStageFillFrames() const noexcept {
        if (!prepared_) {
            return 0;
        }
        int fill = captureStageWrite_.load(std::memory_order_relaxed) -
                   captureStageRead_.load(std::memory_order_relaxed);
        if (fill < 0) {
            fill += ringFrames_;
        }
        return fill;
    }
    // ステージリングの読み出しを始めるのに必要な滞留フレーム数
    // (= ワーカーのブロック長 + kCaptureStageCushionMs 相当)。
    int captureCushionFrames() const noexcept { return cushionFrames_; }
    int captureRingFrames() const noexcept { return ringFrames_; }
    // ワーカー経由にしたことで増える遅延(ms)。
    // = ブロック 1 個ぶんの待ち(kCaptureWorkerBlockFrames)+ ステージリングの
    //   クッション。方式そのものの遅延(captureDspLatencyMillis())とは別勘定。
    double captureExtraLatencyMillis() const noexcept {
        if (!prepared_ || sampleRate_ <= 0.0) {
            return 0.0;
        }
        return static_cast<double>(kCaptureWorkerBlockFrames + cushionFrames_) / sampleRate_ *
               1000.0;
    }
    // 各経路の走査幅(ms)。prepare() が採用した値。
    double micSweepMs() const noexcept { return shifter_.getSweepMs(); }
    double captureSweepMs() const noexcept { return captureShifter_.getSweepMs(); }

    // 起動同期が完了したか。UI の「動作中」表示を実際の処理開始に合わせるために使う。
    // drainRemaining_ / cushionRemaining_ は音声スレッドが書き換えるので、
    // 制御スレッドからはこの atomic 越しにしか観測しない。
    bool isSynced() const noexcept { return synced_.load(std::memory_order_relaxed); }

private:
    // ---- 処理方式(RT 安全: 確保・ロック・I/O・例外なし) -----------------------
    // 範囲外の値は低遅延(既定)へ丸める。
    static int clampMethod(int method) noexcept {
        if (method < kMethodDelayLine || method > kMethodPhaseVocoder4096) {
            return kMethodDelayLine;
        }
        return method;
    }

    static void processMethod(int method, PitchShifter& dl, PhaseVocoderShifter& pv2k,
                              PhaseVocoderShifter& pv4k, const float* const* in,
                              float* const* out, int chunk) noexcept {
        switch (method) {
            case kMethodPhaseVocoder2048:
                pv2k.process(in, out, chunk);
                break;
            case kMethodPhaseVocoder4096:
                pv4k.process(in, out, chunk);
                break;
            default:
                dl.process(in, out, chunk);
                break;
        }
    }

    static void resetMethod(int method, PitchShifter& dl, PhaseVocoderShifter& pv2k,
                            PhaseVocoderShifter& pv4k) noexcept {
        switch (method) {
            case kMethodPhaseVocoder2048:
                pv2k.reset();
                break;
            case kMethodPhaseVocoder4096:
                pv4k.reset();
                break;
            default:
                dl.reset();
                break;
        }
    }

    double latencyMillisForMethod(int method, const PitchShifter& dl,
                                  const PhaseVocoderShifter& pv2k,
                                  const PhaseVocoderShifter& pv4k) const noexcept {
        if (!prepared_ || sampleRate_ <= 0.0) {
            return 0.0;
        }
        double samples;
        switch (clampMethod(method)) {
            case kMethodPhaseVocoder2048:
                samples = pv2k.getLatencySamples();
                break;
            case kMethodPhaseVocoder4096:
                samples = pv4k.getLatencySamples();
                break;
            default:
                samples = dl.getLatencySamples();
                break;
        }
        return samples / sampleRate_ * 1000.0;
    }

    // 経路 1 つぶんを「要求方式」まで進める。要求と現在値が違えば、現在値を
    // fadeFrom に退避してから要求値へ切り替え(切替先は reset() してから使う)、
    // kMethodSwitchCrossfadeMs ぶんクロスフェードを開始する。フェード中でなければ
    // 選択中の 1 方式だけを処理する(3 方式を常時並走させない — README「処理方式」参照)。
    // out には最終的な(必要ならブレンド済みの)結果を書く。oldOut はフェード中だけ
    // 使うスクラッチで、この経路専用に prepare() で確保済みのものを渡すこと。
    void runPathChunk(std::atomic<int>& methodAtomic, int& activeMethod, int& fadeFrom,
                      int& fadeRemaining, PitchShifter& dl, PhaseVocoderShifter& pv2k,
                      PhaseVocoderShifter& pv4k, const float* const* in, float* const* out,
                      float* const* oldOut, int chunk) noexcept {
        const int requested = clampMethod(methodAtomic.load(std::memory_order_relaxed));
        if (requested != activeMethod) {
            fadeFrom = activeMethod;
            activeMethod = requested;
            resetMethod(activeMethod, dl, pv2k, pv4k);
            fadeRemaining = methodFadeTotalSamples_;
        }
        processMethod(activeMethod, dl, pv2k, pv4k, in, out, chunk);
        if (fadeRemaining > 0 && fadeFrom >= 0) {
            processMethod(fadeFrom, dl, pv2k, pv4k, in, oldOut, chunk);
            const int n = (fadeRemaining < chunk) ? fadeRemaining : chunk;
            const int elapsedBase = methodFadeTotalSamples_ - fadeRemaining;
            for (int i = 0; i < n; ++i) {
                float t = static_cast<float>(elapsedBase + i + 1) /
                          static_cast<float>(methodFadeTotalSamples_);
                if (t > 1.0f) {
                    t = 1.0f;
                }
                out[0][i] = out[0][i] * t + oldOut[0][i] * (1.0f - t);
                out[1][i] = out[1][i] * t + oldOut[1][i] * (1.0f - t);
            }
            fadeRemaining -= n;
            if (fadeRemaining <= 0) {
                fadeFrom = -1;
            }
        }
    }

    void runMicPath(int chunk) noexcept {
        runPathChunk(micMethod_, micActiveMethod_, micFadeFromMethod_, micFadeRemaining_,
                    shifter_, micPv2048_, micPv4096_, inPlanar_, outPlanar_, micOldPlanar_,
                    chunk);
    }

    // 捕獲経路 1 ブロックぶん(ワーカースレッドから)。マイク経路と同じ
    // runPathChunk() を、ワーカー専用の作業面に対して回す。
    void runCapturePathBlock(int frames) noexcept {
        runPathChunk(captureMethod_, captureActiveMethod_, captureFadeFromMethod_,
                    captureFadeRemaining_, captureShifter_, capturePv2048_, capturePv4096_,
                    workInPlanar_, workOutPlanar_, captureOldPlanar_, frames);
    }

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

    // ---- 診断録音(音声スレッドから。render() の recording==true のときだけ) -----
    // mixChunk() が outPlanar_ / capOutPlanar_ を最終ミックスへ上書きする「前」に
    // 呼ぶこと — ここで読むのは各経路単独(シフト後・ミックス前)の値。
    //   mic-in      : シフト前のマイク入力(入力 ch 数のまま、pad 分は 0)
    //   capture-in  : シフト前の捕獲入力(常に 2ch)
    //   mic-out     : softClip(mic 単独 x micGain x outputGain)
    //   capture-out : softClip(capture 単独 x captureGain x outputGain)
    // mic-out / capture-out は「そのパス単体しか無かったら聞こえたはずの音」であり、
    // 実際のミックス出力(2 パスの和にゲイン/ソフトクリップをかけたもの)そのものでは
    // ない点に注意(2 パスは加算後に非線形(softClip)を通るため、各パスをここで
    // 個別に softClip したものを単純に足しても実際のミックスとは一致しない)。
    void recordDiagnosticChunk(const float* input, int pad, int done, int chunk) noexcept {
        int w = diagWritten_.load(std::memory_order_relaxed);
        int remaining = diagTotalFrames_ - w;
        if (remaining <= 0) {
            diagActive_.store(false, std::memory_order_release);
            diagDone_.store(true, std::memory_order_release);
            return;
        }
        const int n = (chunk < remaining) ? chunk : remaining;
        const float outG = outputGain_.load(std::memory_order_relaxed);
        const float micG = micGain_.load(std::memory_order_relaxed);
        const float capG = captureActive_ ? captureGain_.load(std::memory_order_relaxed) : 0.0f;
        const int nIn = inputChannels_;
        for (int i = 0; i < n; ++i) {
            const int frame = done + i;
            const std::size_t micDst =
                static_cast<std::size_t>(w + i) * static_cast<std::size_t>(nIn);
            if (frame < pad) {
                for (int c = 0; c < nIn; ++c) {
                    diagMicIn_[micDst + c] = 0.0f;
                }
            } else {
                const std::size_t src = static_cast<std::size_t>(frame - pad) *
                                        static_cast<std::size_t>(nIn);
                for (int c = 0; c < nIn; ++c) {
                    diagMicIn_[micDst + c] = input[src + c];
                }
            }
            const std::size_t stereoDst = static_cast<std::size_t>(w + i) * 2u;
            diagCaptureIn_[stereoDst] = capInPlanar_[0][i];
            diagCaptureIn_[stereoDst + 1] = capInPlanar_[1][i];
            diagMicOut_[stereoDst] = softClip(outPlanar_[0][i] * micG * outG);
            diagMicOut_[stereoDst + 1] = softClip(outPlanar_[1][i] * micG * outG);
            diagCaptureOut_[stereoDst] = softClip(capOutPlanar_[0][i] * capG * outG);
            diagCaptureOut_[stereoDst + 1] = softClip(capOutPlanar_[1][i] * capG * outG);
        }
        w += n;
        diagWritten_.store(w, std::memory_order_relaxed);
        if (w >= diagTotalFrames_) {
            diagActive_.store(false, std::memory_order_release);
            diagDone_.store(true, std::memory_order_release);
        }
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

    // 有効/無効の切り替わりを検出して、音声スレッド側の状態を組み直す。
    //   有効化: 作業面をゼロにし、ステージリングのクッションがたまるまで待つ状態から
    //           始める。有効化 1 回につき 1 コールバックだけ、確保を伴わない
    //           バッファのゼロ埋め(数十マイクロ秒)が乗る。
    //   無効化: ステージリングを読み捨てて空にする(音声スレッドはこのリングの
    //           読み手なので、read を write に追いつかせるのは自分の側の操作)。
    // シフタの reset は音声スレッドでは行わない — 捕獲経路の 3 方式はワーカーが
    // 所有しており、有効化時の初期化は processCaptureBlocks() が行う。
    void syncCaptureState() noexcept {
        const int epoch = captureEpoch_.load(std::memory_order_acquire);
        if (epoch == audioEpoch_) {
            return;
        }
        audioEpoch_ = epoch;
        captureActive_ = captureEnabled_.load(std::memory_order_acquire);
        captureCushioning_ = true;
        zeroCapturePlanes();
        // 空にするのは無効化のときだけ。read を write に追いつかせるのは読み手で
        // ある自分の操作なので安全。有効化のときに空にしてはいけない —
        // ワーカーが有効化後に積んだ正規のデータまで捨ててしまう。
        if (!captureActive_) {
            captureStageRead_.store(captureStageWrite_.load(std::memory_order_acquire),
                                    std::memory_order_release);
        }
    }

    // ステージリングから chunk フレーム取り出して capInPlanar_(生音)と
    // capOutPlanar_(処理後)を埋める。ワーカーが同じ位置へ並べて書いているので、
    // 2 つは必ず同じ時刻で揃う。
    // 状態機械: クッションがたまるまで無音 -> 毎回 chunk ぶん pop ->
    //           足りなければ無音で埋めてクッション待ちへ戻る。
    void fetchCaptureStageChunk(int chunk) noexcept {
        const int w = captureStageWrite_.load(std::memory_order_acquire);
        int r = captureStageRead_.load(std::memory_order_relaxed);
        int fill = w - r;
        if (fill < 0) {
            fill += ringFrames_;
        }

        if (captureCushioning_) {
            if (fill < cushionFrames_) {
                for (int i = 0; i < chunk; ++i) {
                    capInPlanar_[0][i] = 0.0f;
                    capInPlanar_[1][i] = 0.0f;
                    capOutPlanar_[0][i] = 0.0f;
                    capOutPlanar_[1][i] = 0.0f;
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
            capOutPlanar_[0][i] = 0.0f;
            capOutPlanar_[1][i] = 0.0f;
        }
        for (int i = 0; i < avail; ++i) {
            const std::size_t base = static_cast<std::size_t>(r) * 4u;
            capInPlanar_[0][pad + i] = captureStageRing_[base];
            capInPlanar_[1][pad + i] = captureStageRing_[base + 1];
            capOutPlanar_[0][pad + i] = captureStageRing_[base + 2];
            capOutPlanar_[1][pad + i] = captureStageRing_[base + 3];
            r = wrapRing(r + 1);
        }
        captureStageRead_.store(r, std::memory_order_release);

        if (pad > 0) {
            captureUnderruns_.fetch_add(1, std::memory_order_relaxed);
            captureShortfallFramesTotal_.fetch_add(pad, std::memory_order_relaxed);
            captureCushioning_ = true;  // たまり直すまで待つ
        }
    }

    // ---- 捕獲ワーカー本体(ワーカースレッド、またはテストの pump から) ----------

    void notifyCaptureWorker() noexcept {
        if (!workerThread_.joinable()) {
            return;  // ワーカーを起こしていない(ホストスモークの pump 運用)
        }
        {
            std::lock_guard<std::mutex> lock(workerMutex_);
            workerWake_ = true;
        }
        workerCv_.notify_one();
    }

    // スレッド優先度を上げる。SCHED_FIFO が取れなければ nice 値を
    // ANDROID_PRIORITY_AUDIO(-16)相当まで下げる。どちらも失敗しても致命的では
    // ないので、戻り値は見ない(ワーカーは締切を落としても xrun にはならない —
    // ステージリングのクッションが吸収する)。
    static void raiseCaptureWorkerPriority() noexcept {
#if defined(__ANDROID__) || defined(__linux__)
        sched_param param{};
        param.sched_priority = 2;  // SCHED_FIFO の下限付近(音声スレッドより低く)
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
            // PRIO_PROCESS + who=0 は Linux/Android では「呼び出しスレッド」を指す。
            (void)setpriority(PRIO_PROCESS, 0, -16);
        }
#endif
    }

    void captureWorkerLoop() noexcept {
        raiseCaptureWorkerPriority();
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(workerMutex_);
                // タイムアウトを置くのは保険。push が来なくなっても、有効/無効の
                // 切り替わりとドリフト処理は定期的に回したい。
                workerCv_.wait_for(lock, std::chrono::milliseconds(10),
                                   [this] { return workerWake_ || workerQuit_; });
                if (workerQuit_) {
                    return;
                }
                workerWake_ = false;
            }
            processCaptureBlocks();
        }
    }

    // 入力リングにたまっているぶんを kCaptureWorkerBlockFrames 単位で処理し、
    // 生音と処理後の音をステージリングへ並べて書く。
    // ここはオーディオコールバックではないので締切は緩いが、確保・I/O はしない
    // (すべて prepare() 済みのバッファの上で完結する)。
    void processCaptureBlocks() noexcept {
        if (!prepared_) {
            return;
        }
        const int epoch = captureEpoch_.load(std::memory_order_acquire);
        const bool enabled = captureEnabled_.load(std::memory_order_acquire);
        if (epoch != workerEpoch_) {
            workerEpoch_ = epoch;
            workerActive_ = enabled;
            // 入力リングの読み手は自分なので、read を write に追いつかせて空にする。
            captureRead_.store(captureWrite_.load(std::memory_order_acquire),
                               std::memory_order_release);
            if (enabled) {
                // 3 方式とも前回の残響を持ち込まないよう、いま要求されている方式から
                // クリーンに始める(runPathChunk() は活性化直後は fadeFrom=-1 なので、
                // 有効化直後にクロスフェードが走ることはない)。
                captureShifter_.reset();
                capturePv2048_.reset();
                capturePv4096_.reset();
                captureActiveMethod_ =
                    clampMethod(captureMethod_.load(std::memory_order_relaxed));
                captureFadeFromMethod_ = -1;
                captureFadeRemaining_ = 0;
            }
        }
        if (!workerActive_) {
            return;
        }

        for (;;) {
            const int w = captureWrite_.load(std::memory_order_acquire);
            int r = captureRead_.load(std::memory_order_relaxed);
            int fill = w - r;
            if (fill < 0) {
                fill += ringFrames_;
            }
            // ドリフト対策: 入力の滞留が上限を超えたら古い分を捨てる
            // (捨てる瞬間は不連続になるが、遅延が伸び続けるよりはよい)。
            if (fill > maxFillFrames_) {
                r = wrapRing(r + (fill - kCaptureWorkerBlockFrames));
                fill = kCaptureWorkerBlockFrames;
                captureRead_.store(r, std::memory_order_release);
            }
            if (fill < kCaptureWorkerBlockFrames) {
                return;  // 1 ブロックたまるまで待つ
            }

            const int sw = captureStageWrite_.load(std::memory_order_relaxed);
            const int sr = captureStageRead_.load(std::memory_order_acquire);
            int freeFrames = sr - sw - 1;
            if (freeFrames < 0) {
                freeFrames += ringFrames_;
            }
            if (freeFrames < kCaptureWorkerBlockFrames) {
                return;  // 音声スレッドが追いつくまで待つ(入力側は滞留上限で守る)
            }

            // 入力リング -> 作業面
            for (int i = 0; i < kCaptureWorkerBlockFrames; ++i) {
                const std::size_t base = static_cast<std::size_t>(r) * 2u;
                workInPlanar_[0][i] = captureRing_[base];
                workInPlanar_[1][i] = captureRing_[base + 1];
                r = wrapRing(r + 1);
            }
            captureRead_.store(r, std::memory_order_release);

            runCapturePathBlock(kCaptureWorkerBlockFrames);

            // 作業面 -> ステージリング(生音と処理後を同じ位置へ)
            int sweep = sw;
            for (int i = 0; i < kCaptureWorkerBlockFrames; ++i) {
                const std::size_t base = static_cast<std::size_t>(sweep) * 4u;
                captureStageRing_[base] = workInPlanar_[0][i];
                captureStageRing_[base + 1] = workInPlanar_[1][i];
                captureStageRing_[base + 2] = workOutPlanar_[0][i];
                captureStageRing_[base + 3] = workOutPlanar_[1][i];
                sweep = wrapRing(sweep + 1);
            }
            captureStageWrite_.store(sweep, std::memory_order_release);
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

    PitchShifter shifter_;         // マイク経路・方式 0(走査幅 9.5ms = 10ms 予算内)
    PitchShifter captureShifter_;  // 捕獲経路・方式 0(走査幅は prepare の引数、既定 40ms)
    // 方式 1/2(位相ボコーダ N=2048/4096)。経路ごとに 2 つずつ、常に prepare 済み。
    PhaseVocoderShifter micPv2048_;
    PhaseVocoderShifter micPv4096_;
    PhaseVocoderShifter capturePv2048_;
    PhaseVocoderShifter capturePv4096_;

    // 経路ごとの選択方式(制御スレッドが書き、音声スレッドが読む)。
    std::atomic<int> micMethod_{kMicMethodDefault};
    std::atomic<int> captureMethod_{kCaptureMethodDefault};
    // 方式切替のクロスフェード状態(音声スレッド専用。制御スレッドからは読まない —
    // micDspLatencyMillis()/captureDspLatencyMillis() は micMethod_/captureMethod_
    // 越しに「要求値」を読むので、ここには触れない)。
    int micActiveMethod_ = kMicMethodDefault;
    int micFadeFromMethod_ = -1;  // -1 = フェード中ではない
    int micFadeRemaining_ = 0;    // 残りサンプル数
    int captureActiveMethod_ = kCaptureMethodDefault;
    int captureFadeFromMethod_ = -1;
    int captureFadeRemaining_ = 0;
    int methodFadeTotalSamples_ = 0;  // prepare() で算出(kMethodSwitchCrossfadeMs 相当)

    std::vector<float> planar_;
    float* inPlanar_[2] = {nullptr, nullptr};
    float* outPlanar_[2] = {nullptr, nullptr};
    float* capInPlanar_[2] = {nullptr, nullptr};
    float* capOutPlanar_[2] = {nullptr, nullptr};
    // 方式切替クロスフェード中だけ使うスクラッチ(旧方式の出力の受け皿)。
    float* micOldPlanar_[2] = {nullptr, nullptr};
    float* captureOldPlanar_[2] = {nullptr, nullptr};  // 捕獲ワーカー専用
    // 捕獲ワーカー専用の作業面(ブロック 1 個ぶん)。
    float* workInPlanar_[2] = {nullptr, nullptr};
    float* workOutPlanar_[2] = {nullptr, nullptr};

    // 捕獲入力リング(インタリーブ stereo、prepare で確保・以後サイズ不変)。
    // Java の録音スレッド -> 捕獲ワーカー。
    std::vector<float> captureRing_;
    int ringFrames_ = 0;
    int cushionFrames_ = 0;  // ステージリングのクッション(ブロック長 + 余裕)
    int maxFillFrames_ = 0;
    std::atomic<int> captureWrite_{0};  // 書き手 = Java の録音スレッドだけが進める
    std::atomic<int> captureRead_{0};   // 読み手 = 捕獲ワーカーだけが進める

    // 捕獲ステージリング(1 フレーム 4 float: 生 L / 生 R / 処理後 L / 処理後 R)。
    // 捕獲ワーカー -> 音声スレッド。
    std::vector<float> captureStageRing_;
    std::atomic<int> captureStageWrite_{0};  // 書き手 = 捕獲ワーカー
    std::atomic<int> captureStageRead_{0};   // 読み手 = 音声スレッド

    // 捕獲ワーカースレッド。制御スレッドが start/stop し、pushCapture() /
    // setCaptureEnabled() が起こす。
    std::thread workerThread_;
    std::mutex workerMutex_;
    std::condition_variable workerCv_;
    bool workerWake_ = false;
    bool workerQuit_ = false;
    bool workerActive_ = false;  // ワーカー専用。captureEnabled_ の追従状態
    int workerEpoch_ = 0;        // ワーカー専用。captureEpoch_ の追従状態
    std::atomic<int> captureUnderruns_{0};
    std::atomic<int> captureOverruns_{0};
    std::atomic<bool> captureEnabled_{false};
    // 有効/無効の切り替え世代。setCaptureEnabled() が進め、音声スレッド
    // (audioEpoch_)とワーカー(workerEpoch_)がそれぞれ追従する。
    std::atomic<int> captureEpoch_{0};
    std::atomic<float> micGain_{kMicGainDefault};
    std::atomic<float> captureGain_{kCaptureGainDefault};
    // 音声スレッド専用の状態(制御スレッドからは読まない)。
    bool captureActive_ = false;
    bool captureCushioning_ = true;
    int audioEpoch_ = 0;  // 音声スレッド専用。captureEpoch_ の追従状態

    double sampleRate_ = 0.0;
    int inputChannels_ = 0;
    int outputChannels_ = 0;

    int drainRemaining_ = kDrainCallbacks;
    int cushionRemaining_ = kInputBurstsCushion;

    std::atomic<int> underrunCount_{0};
    std::atomic<int> micShortfallFrames_{0};
    std::atomic<bool> synced_{false};
    std::atomic<float> outputGain_{kOutputGainDefault};
    std::atomic<int> captureShortfallFramesTotal_{0};

    // ---- 診断用の 10 秒録音 --------------------------------------------------
    // 確保/解放は制御スレッドのみ(startDiagnosticRecording() /
    // releaseDiagnosticRecording() / reset())。音声スレッドは diagActive_ 越しに
    // しかこれらへ触らない(true を観測した時点でベクタは既に確保済みであることを
    // diagActive_ の release ストアで保証する)。
    std::vector<float> diagCaptureIn_;   // インタリーブ 2ch
    std::vector<float> diagCaptureOut_;  // インタリーブ 2ch
    std::vector<float> diagMicIn_;       // インタリーブ inputChannels_ ch
    std::vector<float> diagMicOut_;      // インタリーブ 2ch
    int diagTotalFrames_ = 0;
    std::atomic<int> diagWritten_{0};
    std::atomic<bool> diagActive_{false};
    std::atomic<bool> diagDone_{false};

    bool prepared_ = false;
};

}  // namespace prism

#endif  // PRISM_AUDIOBRIDGE_H
