// audio_bridge_smoke.cpp — AudioBridge のホストスモークテスト。
//
// Android 実機もエミュレータも使えない環境で、JNI と Oboe を除いた
// エンジンのロジック(状態機械 + チャンネル変換 + PitchShifter 駆動)が
// 「ビルドが通り、期待どおり動く」ことを確認する。
//
//   clang++ -std=c++17 -Wall -Wextra -Werror -O2 \
//       -I../../../../../dsp/include -I. audio_bridge_smoke.cpp -o /tmp/prism_bridge_smoke
//
// build.sh がこの通りに実行する。外部依存はゼロ。

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "AudioBridge.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (ok) {
        std::printf("  ok    %s\n", what);
    } else {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    }
}

// 全サンプルが有限か。
bool allFinite(const std::vector<float>& v) {
    for (float x : v) {
        if (!std::isfinite(x)) {
            return false;
        }
    }
    return true;
}

float peak(const std::vector<float>& v, std::size_t stride, std::size_t offset) {
    float m = 0.0f;
    for (std::size_t i = offset; i < v.size(); i += stride) {
        const float a = std::fabs(v[i]);
        if (a > m) {
            m = a;
        }
    }
    return m;
}

// numFrames フレーム分の正弦波をインタリーブで作る。amplitude は既定 0.5(呼び出し側の
// 大半はこれまでどおり半振幅の正弦波を使う。テスト [8] だけ明示的に振幅を変える)。
std::vector<float> makeSine(int frames, int channels, double freq, double fs, double phase0,
                            double amplitude = 0.5) {
    std::vector<float> buf(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
    for (int i = 0; i < frames; ++i) {
        const double t = (phase0 + static_cast<double>(i)) / fs;
        const float s = static_cast<float>(amplitude * std::sin(2.0 * M_PI * freq * t));
        for (int c = 0; c < channels; ++c) {
            buf[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels) +
                static_cast<std::size_t>(c)] = s;
        }
    }
    return buf;
}

// 状態機械を Render に到達させる。Drain 中は毎回「0 フレーム捨てた」と報告する。
void runToSteadyState(prism::AudioBridge& bridge) {
    const int limit = prism::AudioBridge::kDrainCallbacks +
                      prism::AudioBridge::kInputBurstsCushion + 4;
    for (int k = 0; k < limit; ++k) {
        const prism::AudioBridge::Step step = bridge.nextStep();
        if (step == prism::AudioBridge::Step::DrainInput) {
            bridge.reportDrain(0);
        } else if (step == prism::AudioBridge::Step::Render) {
            return;
        }
    }
}

// ---- 1. 起動同期の状態機械 -------------------------------------------------
void testStateMachine() {
    std::printf("[1] 起動同期の状態機械\n");
    prism::AudioBridge bridge;
    check(bridge.prepare(48000.0, 2, 2), "prepare(48000, 2ch in, 2ch out) が成功する");
    check(!bridge.isSynced(), "prepare 直後は同期未完了");

    // 入力に古い音がたまっている間は Drain のまま(捨てた分 > 0 は数えない)。
    bool stayedDraining = true;
    for (int i = 0; i < 50; ++i) {
        if (bridge.nextStep() != prism::AudioBridge::Step::DrainInput) {
            stayedDraining = false;
        }
        bridge.reportDrain(192);
    }
    check(stayedDraining, "捨てるフレームがある限り Drain を抜けない");

    // 空になったコールバックを kDrainCallbacks 回数えると Cushion に進む。
    for (int i = 0; i < prism::AudioBridge::kDrainCallbacks; ++i) {
        bridge.reportDrain(0);
    }
    int cushions = 0;
    while (bridge.nextStep() == prism::AudioBridge::Step::BuildCushion) {
        ++cushions;
        if (cushions > 16) {
            break;
        }
    }
    check(cushions == prism::AudioBridge::kInputBurstsCushion,
          "Cushion はちょうど kInputBurstsCushion 回");
    check(bridge.nextStep() == prism::AudioBridge::Step::Render, "その後は Render に落ち着く");
    check(bridge.isSynced(), "同期完了フラグが立つ");

    // reset で最初からやり直せる。
    bridge.reset();
    check(!bridge.isSynced(), "reset で同期状態が巻き戻る");
    check(bridge.nextStep() == prism::AudioBridge::Step::DrainInput, "reset 後は再び Drain から");
}

// ---- 2. ハッピーパス: ステレオ入力 -> ステレオ出力 --------------------------
void testStereoHappyPath() {
    std::printf("[2] ハッピーパス(48kHz / 2ch in / 2ch out / 440Hz)\n");
    constexpr double kFs = 48000.0;
    constexpr int kFrames = 192;

    prism::AudioBridge bridge;
    check(bridge.prepare(kFs, 2, 2), "prepare が成功する");
    runToSteadyState(bridge);

    bridge.shifter().setShiftCentsL(-89.0f);
    bridge.shifter().setShiftCentsR(-89.0f);
    bridge.shifter().setDryWet(1.0f);

    // リングバッファが埋まるまで少し回してから測る。
    std::vector<float> out(static_cast<std::size_t>(kFrames) * 2u);
    double phase = 0.0;
    float last = 0.0f;
    for (int block = 0; block < 200; ++block) {
        const std::vector<float> in = makeSine(kFrames, 2, 440.0, kFs, phase);
        phase += kFrames;
        bridge.render(in.data(), kFrames, out.data(), kFrames);
        last = peak(out, 2, 0);
    }
    check(allFinite(out), "出力に NaN / Inf が無い");
    check(last > 0.2f && last < 0.9f, "出力の振幅が入力(0.5)と同程度に保たれる");
    check(peak(out, 2, 1) > 0.2f, "R チャンネルも鳴っている");
    check(bridge.underrunCount() == 0, "アンダーランが発生していない");
    check(bridge.dspLatencyMillis() > 0.0 && bridge.dspLatencyMillis() <= 10.0,
          "DSP 遅延が 10ms 以下(NFR-1)");
}

// ---- 3. 境界: モノ入力の L/R 複製 ------------------------------------------
void testMonoInputDuplication() {
    std::printf("[3] 境界: モノ入力を L/R に複製する\n");
    constexpr double kFs = 48000.0;
    constexpr int kFrames = 192;

    prism::AudioBridge bridge;
    check(bridge.prepare(kFs, 1, 2), "prepare(1ch in, 2ch out) が成功する");
    check(bridge.inputChannels() == 1, "入力チャンネル数が 1 と記録される");
    runToSteadyState(bridge);

    // L だけシフトし、R は 0 セント(素通し)にする。両 ch が鳴っていれば複製できている。
    bridge.shifter().setShiftCentsL(-89.0f);
    bridge.shifter().setShiftCentsR(0.0f);

    std::vector<float> out(static_cast<std::size_t>(kFrames) * 2u);
    double phase = 0.0;
    for (int block = 0; block < 200; ++block) {
        const std::vector<float> in = makeSine(kFrames, 1, 440.0, kFs, phase);
        phase += kFrames;
        bridge.render(in.data(), kFrames, out.data(), kFrames);
    }
    check(allFinite(out), "出力に NaN / Inf が無い");
    check(peak(out, 2, 0) > 0.2f, "L が鳴っている");
    check(peak(out, 2, 1) > 0.2f, "R にもモノ入力が複製されている");
}

// ---- 4. 異常系: 入力アンダーラン(read が要求より少ない) --------------------
void testInputUnderrun() {
    std::printf("[4] 異常系: 入力アンダーラン\n");
    constexpr double kFs = 48000.0;
    constexpr int kFrames = 192;

    prism::AudioBridge bridge;
    check(bridge.prepare(kFs, 2, 2), "prepare が成功する");
    runToSteadyState(bridge);

    std::vector<float> out(static_cast<std::size_t>(kFrames) * 2u, 7.0f);
    const std::vector<float> in = makeSine(kFrames, 2, 440.0, kFs, 0.0);

    // 半分しか読めなかった場合。落ちず、出力バッファ全体が書き換わること。
    bridge.render(in.data(), kFrames / 2, out.data(), kFrames);
    check(allFinite(out), "半分しか読めなくても NaN / Inf を出さない");
    check(peak(out, 1, 0) < 1.0f, "書き残し(7.0)が出力に残っていない");
    check(bridge.underrunCount() == 1, "アンダーランが 1 回数えられる");

    // まったく読めなかった場合(framesRead = 0)。
    bridge.render(in.data(), 0, out.data(), kFrames);
    check(allFinite(out), "0 フレームでも NaN / Inf を出さない");
    check(bridge.underrunCount() == 2, "アンダーランが 2 回目も数えられる");

    // 入力ポインタが null(ストリームが死んだ直後)でも落ちない。
    bridge.render(nullptr, kFrames, out.data(), kFrames);
    check(allFinite(out), "入力 null でも NaN / Inf を出さない");

    // 負のフレーム数 / 0 フレームは何もしない。
    bridge.render(in.data(), kFrames, out.data(), 0);
    bridge.render(in.data(), kFrames, out.data(), -1);
    check(true, "numFrames が 0 / 負でも落ちない");
}

// ---- 5. 異常系: prepare 前の render と不正な prepare 引数 -------------------
void testUnpreparedAndBadArguments() {
    std::printf("[5] 異常系: 未初期化 / 不正な引数\n");
    prism::AudioBridge bridge;
    check(!bridge.isPrepared(), "既定では未初期化");

    // prepare していない状態で render しても、無音を書くだけで落ちない。
    std::vector<float> out(256, 7.0f);
    const std::vector<float> in(256, 0.5f);
    bridge.render(in.data(), 128, out.data(), 128);
    check(out[0] == 0.0f, "未初期化の render は無音を書く");

    check(!bridge.prepare(0.0, 2, 2), "サンプルレート 0 の prepare は失敗する");
    check(!bridge.prepare(48000.0, 0, 2), "入力 0ch の prepare は失敗する");
    check(!bridge.prepare(48000.0, 2, 0), "出力 0ch の prepare は失敗する");
    // PitchShifter が受け付けない極端なサンプルレート。
    check(!bridge.prepare(1.0e9, 2, 2), "範囲外のサンプルレートの prepare は失敗する");
    check(!bridge.isPrepared(), "失敗した prepare の後も未初期化のまま");
}

// ---- 6. 境界: コールバック上限を超えるフレーム数 / モノ出力 -----------------
void testOversizedCallbackAndMonoOutput() {
    std::printf("[6] 境界: 上限超えのコールバックとモノ出力\n");
    constexpr double kFs = 48000.0;
    const int frames = prism::AudioBridge::kMaxCallbackFrames + 137;

    prism::AudioBridge bridge;
    check(bridge.prepare(kFs, 2, 1), "prepare(2ch in, 1ch out) が成功する");
    runToSteadyState(bridge);

    const std::vector<float> in = makeSine(frames, 2, 440.0, kFs, 0.0);
    std::vector<float> out(static_cast<std::size_t>(frames), 7.0f);
    bridge.render(in.data(), frames, out.data(), frames);
    check(allFinite(out), "上限超えでも NaN / Inf を出さない");
    check(out[static_cast<std::size_t>(frames) - 1] != 7.0f,
          "最終フレームまで書かれている(分割処理が全域を覆う)");
    check(bridge.underrunCount() == 0, "分割処理はアンダーラン扱いにならない");
}

// ---- 7. パラメータの clamp が DSP 側で効いていること ------------------------
void testParameterClamping() {
    std::printf("[7] パラメータの clamp\n");
    prism::AudioBridge bridge;
    check(bridge.prepare(48000.0, 2, 2), "prepare が成功する");

    // 範囲外・非有限値を投げても落ちず、後続の render が健全であること。
    bridge.shifter().setShiftCentsL(-99999.0f);
    bridge.shifter().setShiftCentsR(99999.0f);
    bridge.shifter().setDryWet(std::nanf(""));
    bridge.shifter().setCrossfadeMs(-1.0f);

    runToSteadyState(bridge);
    const std::vector<float> in = makeSine(192, 2, 440.0, 48000.0, 0.0);
    std::vector<float> out(192u * 2u);
    for (int block = 0; block < 50; ++block) {
        bridge.render(in.data(), 192, out.data(), 192);
    }
    check(allFinite(out), "範囲外パラメータの後も NaN / Inf を出さない");
    check(bridge.shifter().getWindowSamples() >= 2, "窓長が下限で clamp されている");
}

// ---- 8. 出力ゲインとソフトクリップ ------------------------------------------
void testOutputGainAndSoftClip() {
    std::printf("[8] 出力ゲインとソフトクリップ\n");
    constexpr double kFs = 48000.0;
    constexpr int kFrames = 192;

    prism::AudioBridge bridge;
    check(bridge.prepare(kFs, 2, 2), "prepare が成功する");
    check(bridge.outputGain() == 1.0f, "既定ゲインは 1.0(0dB)");
    runToSteadyState(bridge);
    bridge.shifter().setShiftCentsL(0.0f);
    bridge.shifter().setShiftCentsR(0.0f);
    bridge.shifter().setDryWet(1.0f);

    // ゲイン 1.0: 小振幅(0.1)入力はソフトクリップの折れ点(0.9)より十分下なので
    // ほぼそのまま通る。
    std::vector<float> out(static_cast<std::size_t>(kFrames) * 2u);
    double phase = 0.0;
    float peakUnity = 0.0f;
    for (int block = 0; block < 100; ++block) {
        const std::vector<float> in = makeSine(kFrames, 2, 440.0, kFs, phase, 0.1);
        phase += kFrames;
        bridge.render(in.data(), kFrames, out.data(), kFrames);
        peakUnity = peak(out, 2, 0);
    }
    check(allFinite(out), "ゲイン 1.0 で NaN / Inf が出ない");
    check(peakUnity > 0.05f && peakUnity < 0.15f, "ゲイン 1.0 では振幅がほぼそのまま");

    // ゲイン 2.0: 折れ点より十分下の振幅は線形にほぼ 2 倍になる。
    bridge.setOutputGain(2.0f);
    check(bridge.outputGain() == 2.0f, "ゲイン 2.0 が反映される");
    float peakDoubled = 0.0f;
    phase = 0.0;
    for (int block = 0; block < 100; ++block) {
        const std::vector<float> in = makeSine(kFrames, 2, 440.0, kFs, phase, 0.1);
        phase += kFrames;
        bridge.render(in.data(), kFrames, out.data(), kFrames);
        peakDoubled = peak(out, 2, 0);
    }
    check(allFinite(out), "ゲイン 2.0 で NaN / Inf が出ない");
    check(peakDoubled > peakUnity * 1.6f, "ゲイン 2.0 で振幅がおおむね倍になる");

    // 範囲外のゲインは clamp される(上限 4.0)。
    bridge.setOutputGain(999.0f);
    check(bridge.outputGain() == prism::AudioBridge::kOutputGainMax,
          "範囲外の大きすぎるゲインは上限 4.0 に clamp される");
    bridge.setOutputGain(-1.0f);
    check(bridge.outputGain() == prism::AudioBridge::kOutputGainMin,
          "範囲外の小さすぎるゲインは下限 0.5 に clamp される");
    bridge.setOutputGain(std::nanf(""));
    check(bridge.outputGain() == prism::AudioBridge::kOutputGainDefault,
          "非有限なゲインは既定値 1.0 に丸められる");

    // ゲイン最大 + フルスケール入力(振幅 0.5)でもソフトクリップにより
    // 出力が ±1.0 を超えない。
    bridge.setOutputGain(prism::AudioBridge::kOutputGainMax);
    float peakClipped = 0.0f;
    phase = 0.0;
    for (int block = 0; block < 100; ++block) {
        const std::vector<float> in = makeSine(kFrames, 2, 440.0, kFs, phase, 0.5);
        phase += kFrames;
        bridge.render(in.data(), kFrames, out.data(), kFrames);
        peakClipped = peak(out, 2, 0);
    }
    check(allFinite(out), "最大ゲイン + フルスケール入力でも NaN / Inf が出ない");
    // tanh は数学的に ±1.0 未満だが、float32 では極端な引数で丸めにより厳密に
    // 1.0 になりうる(1.0 を超えることはない)。「超えない」ことだけを確認する。
    check(peakClipped <= 1.0f, "ソフトクリップにより出力が ±1.0 を超えない(NFR: 出力保護)");
    check(peakClipped > 0.9f, "折れ点(0.9)を超える入力はきちんと持ち上がる");
}

// ---- 捕獲経路の共通ヘルパ ---------------------------------------------------
// 捕獲経路を「素通し(dryWet=0)+ マイク無音」に構成する。こうするとリングに
// 書いた値がそのまま出力に現れるため、順序・クッション・欠落を厳密に検査できる。
//   out = マイク経路 x 0.0 + 捕獲経路 x 1.0、出力ゲイン 1.0、|x| <= 0.9 は素通し。
void configurePassthroughCapture(prism::AudioBridge& bridge) {
    bridge.captureShifter().setDryWet(0.0f);  // 有効化時の reset() で即座に整定する
    bridge.setMicGain(0.0f);
    bridge.setCaptureGain(1.0f);
    bridge.setCaptureEnabled(true);
}

// 値が単調増加するインタリーブ stereo の列(L=R)。
std::vector<float> makeRamp(int frames) {
    std::vector<float> buf(static_cast<std::size_t>(frames) * 2u);
    for (int i = 0; i < frames; ++i) {
        const float v = 0.0001f * static_cast<float>(i + 1);
        buf[static_cast<std::size_t>(i) * 2u] = v;
        buf[static_cast<std::size_t>(i) * 2u + 1u] = v;
    }
    return buf;
}

// ---- 9. 捕獲リング: push した順に pop されること ----------------------------
void testCaptureRingOrdering() {
    std::printf("[9] 捕獲リング: push/pop の順序保証\n");
    constexpr double kFs = 48000.0;
    constexpr int kFrames = 192;

    prism::AudioBridge bridge;
    check(bridge.prepare(kFs, 2, 2), "prepare が成功する");
    check(!bridge.isCaptureEnabled(), "捕獲は既定で無効");
    check(bridge.captureSweepMs() == prism::AudioBridge::kCaptureSweepMsDefault,
          "捕獲シフタの走査幅が既定 40ms で開かれている");
    check(bridge.shifter().getSweepMs() == prism::PitchShifter::kSweepMs,
          "マイクシフタの走査幅は従来どおり 9.5ms");

    check(bridge.pushCapture(nullptr, 128, 2) == 0, "無効中の pushCapture は 0 を返す");

    configurePassthroughCapture(bridge);
    runToSteadyState(bridge);

    // クッション(20ms = 960 フレーム @48k)を超える量をまとめて書く。
    const int pushed = 2000;
    const std::vector<float> ramp = makeRamp(pushed);
    check(bridge.pushCapture(ramp.data(), pushed, 2) == pushed, "2000 フレーム全部書ける");
    check(bridge.captureFillFrames() == pushed, "滞留が push した分だけ増える");

    const std::vector<float> silence(static_cast<std::size_t>(kFrames) * 2u, 0.0f);
    std::vector<float> out(static_cast<std::size_t>(kFrames) * 2u, 7.0f);

    bridge.render(silence.data(), kFrames, out.data(), kFrames);
    bool ordered = true;
    for (int i = 0; i < kFrames; ++i) {
        if (out[static_cast<std::size_t>(i) * 2u] != ramp[static_cast<std::size_t>(i) * 2u]) {
            ordered = false;
        }
    }
    check(ordered, "1 回目のコールバックに先頭 192 フレームが順番どおり現れる");

    bridge.render(silence.data(), kFrames, out.data(), kFrames);
    bool continued = true;
    for (int i = 0; i < kFrames; ++i) {
        const std::size_t src = static_cast<std::size_t>(kFrames + i) * 2u;
        if (out[static_cast<std::size_t>(i) * 2u] != ramp[src]) {
            continued = false;
        }
    }
    check(continued, "2 回目は続きから途切れずに出る(取りこぼし・重複なし)");
    check(bridge.captureFillFrames() == pushed - 2 * kFrames, "滞留が pop した分だけ減る");
    check(bridge.captureUnderruns() == 0 && bridge.captureOverruns() == 0,
          "順調な経路では under/overrun が発生しない");

    // モノ捕獲は L/R に複製される。
    const std::vector<float> mono(64, 0.25f);
    check(bridge.pushCapture(mono.data(), 64, 1) == 64, "モノ 64 フレームを書ける");
}

// ---- 10. クッション: たまるまで読まないこと --------------------------------
void testCaptureCushion() {
    std::printf("[10] 捕獲リング: クッション動作\n");
    constexpr double kFs = 48000.0;
    constexpr int kFrames = 192;

    prism::AudioBridge bridge;
    check(bridge.prepare(kFs, 2, 2), "prepare が成功する");
    configurePassthroughCapture(bridge);
    runToSteadyState(bridge);

    const int cushion = bridge.captureCushionFrames();
    check(cushion == static_cast<int>(prism::AudioBridge::kCaptureCushionMs * kFs / 1000.0),
          "クッションは 20ms 相当(960 フレーム @48k)");

    // クッション未満: 読まずに無音を出す(滞留も減らない)。
    const int half = cushion / 2;
    const std::vector<float> quiet(static_cast<std::size_t>(half) * 2u, 0.5f);
    check(bridge.pushCapture(quiet.data(), half, 2) == half, "クッション未満だけ書く");

    const std::vector<float> silence(static_cast<std::size_t>(kFrames) * 2u, 0.0f);
    std::vector<float> out(static_cast<std::size_t>(kFrames) * 2u, 7.0f);
    bridge.render(silence.data(), kFrames, out.data(), kFrames);
    check(peak(out, 1, 0) == 0.0f, "クッションがたまるまで捕獲経路は無音");
    check(bridge.captureFillFrames() == half, "読まないので滞留は減らない");
    check(bridge.captureUnderruns() == 0, "クッション待ちはアンダーランに数えない");

    // クッションを超えたら読み始める。
    check(bridge.pushCapture(quiet.data(), half, 2) == half, "追加で書いてクッションを超える");
    bridge.render(silence.data(), kFrames, out.data(), kFrames);
    check(out[0] == 0.5f, "クッションが満ちたら捕獲音がそのまま出る");
    check(bridge.captureFillFrames() == 2 * half - kFrames, "読んだ分だけ滞留が減る");
}

// ---- 11. mic 0 + capture 1 のとき捕獲音だけが出ること -----------------------
void testMicMuteCaptureOnly() {
    std::printf("[11] ミックス: micGain=0 なら捕獲音だけが出る\n");
    constexpr double kFs = 48000.0;
    constexpr int kFrames = 192;

    prism::AudioBridge bridge;
    check(bridge.prepare(kFs, 2, 2), "prepare が成功する");
    check(bridge.micGain() == 1.0f && bridge.captureGain() == 1.0f, "ゲインの既定は 1.0");
    runToSteadyState(bridge);

    // 捕獲無効 + micGain=0: マイクに信号があっても出力は完全な無音。
    bridge.setMicGain(0.0f);
    std::vector<float> out(static_cast<std::size_t>(kFrames) * 2u, 7.0f);
    double phase = 0.0;
    for (int block = 0; block < 50; ++block) {
        const std::vector<float> in = makeSine(kFrames, 2, 440.0, kFs, phase);
        phase += kFrames;
        bridge.render(in.data(), kFrames, out.data(), kFrames);
    }
    check(peak(out, 1, 0) == 0.0f, "micGain=0 かつ捕獲無効なら出力は無音");

    // 捕獲を有効にして一定値を流すと、その値だけが出る(マイクは混ざらない)。
    bridge.captureShifter().setDryWet(0.0f);
    bridge.setCaptureGain(1.0f);
    bridge.setCaptureEnabled(true);
    const std::vector<float> constant(4000u * 2u, 0.25f);
    check(bridge.pushCapture(constant.data(), 4000, 2) == 4000, "捕獲音を 4000 フレーム書く");

    const std::vector<float> mic = makeSine(kFrames, 2, 440.0, kFs, 0.0);
    bridge.render(mic.data(), kFrames, out.data(), kFrames);
    bool captureOnly = true;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] != 0.25f) {
            captureOnly = false;
        }
    }
    check(captureOnly, "マイク信号があっても捕獲音の値だけが出る");

    // 捕獲ゲイン 0 では無音に戻る。
    bridge.setCaptureGain(0.0f);
    bridge.render(mic.data(), kFrames, out.data(), kFrames);
    check(peak(out, 1, 0) == 0.0f, "captureGain=0 で捕獲経路も無音になる");

    // ゲインの clamp。
    bridge.setMicGain(-1.0f);
    check(bridge.micGain() == prism::AudioBridge::kMicGainMin, "micGain の下限は 0.0");
    bridge.setMicGain(99.0f);
    check(bridge.micGain() == prism::AudioBridge::kMicGainMax, "micGain の上限は 2.0");
    bridge.setMicGain(std::nanf(""));
    check(bridge.micGain() == prism::AudioBridge::kMicGainDefault,
          "非有限な micGain は既定値 1.0 に丸められる");
    bridge.setCaptureGain(99.0f);
    check(bridge.captureGain() == prism::AudioBridge::kCaptureGainMax,
          "captureGain の上限は 4.0");
    bridge.setCaptureGain(std::nanf(""));
    check(bridge.captureGain() == prism::AudioBridge::kCaptureGainDefault,
          "非有限な captureGain は既定値 1.0 に丸められる");
}

// ---- 12. overrun / underrun のカウント -------------------------------------
void testCaptureOverrunUnderrun() {
    std::printf("[12] 捕獲リング: overrun / underrun のカウント\n");
    constexpr double kFs = 48000.0;
    constexpr int kFrames = 192;

    prism::AudioBridge bridge;
    check(bridge.prepare(kFs, 2, 2), "prepare が成功する");
    configurePassthroughCapture(bridge);
    runToSteadyState(bridge);

    const int ring = bridge.captureRingFrames();
    check(ring == static_cast<int>(prism::AudioBridge::kCaptureRingSeconds * kFs),
          "リング容量は 1 秒ぶん(48000 フレーム @48k)");

    // 容量を超える push は余りを捨て、overrun を 1 数える。
    const int over = ring + 1000;
    const std::vector<float> big(static_cast<std::size_t>(over) * 2u, 0.1f);
    check(bridge.pushCapture(big.data(), over, 2) == ring - 1,
          "満杯の 1 フレーム手前まで書ける(空と満杯を区別するため)");
    check(bridge.captureOverruns() == 1, "取りこぼした push が 1 回数えられる");
    check(bridge.pushCapture(big.data(), 128, 2) == 0, "満杯のあいだは 1 フレームも書けない");
    check(bridge.captureOverruns() == 2, "満杯への push も取りこぼしとして数える");

    // 滞留が上限(200ms)を超えているので、最初の読み出しで古い分が捨てられる。
    const std::vector<float> silence(static_cast<std::size_t>(kFrames) * 2u, 0.0f);
    std::vector<float> out(static_cast<std::size_t>(kFrames) * 2u, 7.0f);
    bridge.render(silence.data(), kFrames, out.data(), kFrames);
    check(bridge.captureFillFrames() <= bridge.captureCushionFrames(),
          "滞留が上限を超えたら古い分を捨ててクッション量まで詰める(ドリフト対策)");
    check(bridge.captureUnderruns() == 0, "捨てただけではアンダーランに数えない");

    // 読み続けるとリングが空になり、アンダーランが数えられる。
    for (int k = 0; k < 16; ++k) {
        bridge.render(silence.data(), kFrames, out.data(), kFrames);
    }
    check(bridge.captureUnderruns() >= 1, "リングが空になるとアンダーランを数える");

    // アンダーラン後はクッション待ちに戻るので、たまるまでは無音。
    const int before = bridge.captureUnderruns();
    bridge.render(silence.data(), kFrames, out.data(), kFrames);
    check(peak(out, 1, 0) == 0.0f, "アンダーラン後はクッション待ちに戻って無音になる");
    check(bridge.captureUnderruns() == before,
          "クッション待ちのあいだはアンダーランを重ねて数えない");

    // 無効化すると経路が無音になり、再有効化ではリングが空から始まる。
    check(bridge.pushCapture(big.data(), 2000, 2) == 2000, "無音化の前に捕獲音を書いておく");
    bridge.setCaptureEnabled(false);
    bridge.render(silence.data(), kFrames, out.data(), kFrames);
    check(peak(out, 1, 0) == 0.0f, "無効化すると捕獲経路は無音");
    bridge.setCaptureEnabled(true);
    bridge.render(silence.data(), kFrames, out.data(), kFrames);
    check(bridge.captureFillFrames() == 0, "再有効化ではリングが空から始まる");
    check(peak(out, 1, 0) == 0.0f, "再有効化直後はクッションがたまるまで無音");
}

}  // namespace

int main() {
    std::printf("prism AudioBridge host smoke\n\n");
    testStateMachine();
    testStereoHappyPath();
    testMonoInputDuplication();
    testInputUnderrun();
    testUnpreparedAndBadArguments();
    testOversizedCallbackAndMonoOutput();
    testParameterClamping();
    testOutputGainAndSoftClip();
    testCaptureRingOrdering();
    testCaptureCushion();
    testMicMuteCaptureOnly();
    testCaptureOverrunUnderrun();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("PASS — 失敗 0 件\n");
        return EXIT_SUCCESS;
    }
    std::printf("FAIL — 失敗 %d 件\n", g_failures);
    return EXIT_FAILURE;
}
