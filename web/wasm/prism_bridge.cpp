// prism_bridge.cpp — 契約 2(extern "C" WASM API)の実装
//
// 位置づけ(team.md「レイヤ分離」): これは **プラットフォーム接着層** であり、
// DSP コア dsp/include/prism/PitchShifter.h(正本、依存ゼロ)を薄く包むだけ。
// アルゴリズムはここに一切書かない。コアには手を入れない。
//
// 契約 2(inception/contract-design/contract-summary.md):
//   ps_create      () -> handle(i32)                      0 = 失敗
//   ps_destroy     (handle) -> void
//   ps_prepare     (handle, f64 fs, i32 maxBlock) -> i32   1 = 成功
//   ps_io_ptr      (handle, i32 channel) -> f32*           共有 I/O 領域(in-place)
//   ps_process     (handle, i32 numFrames) -> void
//   ps_set_param   (handle, i32 id, f32 value) -> void
//   ps_latency_ms  (handle) -> f64
// 追加(内部契約の拡張。JS フォールバック PitchShifterJS と面を揃えるため):
//   ps_reset / ps_latency_samples / ps_window_samples / ps_sweep_samples
//
// リアルタイム安全性: ps_process はコアの process() を呼ぶだけで、確保・ロック・
// I/O・例外を行わない。インスタンス実体と共有 I/O 領域は静的記憶域に置き、
// ps_create / ps_io_ptr でも malloc を経由しない(確保はコアの prepare のみ)。

#include <prism/PitchShifter.h>

#include <cmath>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define PRISM_EXPORT extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define PRISM_EXPORT extern "C"
#endif

namespace {

// 同時に生きるインスタンス数。デモは 1 個しか使わないが、
// ページ内で作り直したときの取り違えを防げる程度の余裕を持たせる。
constexpr int kMaxInstances = 4;

// 共有 I/O 領域のフレーム数。Web Audio のレンダ量子は 128 だが、
// 将来量子長が可変になっても耐えられる幅を静的に確保しておく。
constexpr int kMaxIoFrames = 1024;

struct Slot {
    prism::PitchShifter shifter;
    float io[2][kMaxIoFrames];
    double sampleRate;
    int maxBlockFrames;
    bool used;
    bool prepared;
};

// 静的記憶域。ps_create で malloc しないための配置。
Slot g_slots[kMaxInstances];

void clearIo(Slot& slot) noexcept {
    for (int ch = 0; ch < 2; ++ch) {
        for (int n = 0; n < kMaxIoFrames; ++n) {
            slot.io[ch][n] = 0.0f;
        }
    }
}

// handle は 1 始まり(0 = 失敗/無効を表す契約 2 の規約)。
Slot* resolve(int handle) noexcept {
    if (handle < 1 || handle > kMaxInstances) {
        return nullptr;
    }
    Slot& slot = g_slots[handle - 1];
    return slot.used ? &slot : nullptr;
}

}  // namespace

// ---------------------------------------------------------------- ps_create
PRISM_EXPORT int ps_create() {
    for (int i = 0; i < kMaxInstances; ++i) {
        Slot& slot = g_slots[i];
        if (!slot.used) {
            slot.used = true;
            slot.prepared = false;
            slot.sampleRate = 0.0;
            slot.maxBlockFrames = 0;
            clearIo(slot);
            return i + 1;
        }
    }
    return 0;  // 空きなし: 契約 2 のとおり 0 を返す
}

// --------------------------------------------------------------- ps_destroy
PRISM_EXPORT void ps_destroy(int handle) {
    Slot* slot = resolve(handle);
    if (slot == nullptr) {
        return;  // 無効ハンドルは黙って無視(JS 境界へ例外を出さない)
    }
    slot->used = false;
    slot->prepared = false;
    slot->sampleRate = 0.0;
    slot->maxBlockFrames = 0;
}

// --------------------------------------------------------------- ps_prepare
// 1 = 成功、0 = 失敗。失敗理由(範囲外 fs / ブロック長超過 / コア確保失敗)は
// すべて 0 に畳み込む。呼び出し側(Worklet)は 0 を契約 3 の error トリガにする。
PRISM_EXPORT int ps_prepare(int handle, double sampleRate, int maxBlockFrames) {
    Slot* slot = resolve(handle);
    if (slot == nullptr) {
        return 0;
    }
    slot->prepared = false;
    if (!std::isfinite(sampleRate)) {
        return 0;
    }
    if (maxBlockFrames < 1 || maxBlockFrames > kMaxIoFrames) {
        return 0;  // 共有 I/O 領域を超えるブロック長は受け付けない
    }
    if (!slot->shifter.prepare(sampleRate, maxBlockFrames)) {
        return 0;  // fs 範囲外・確保失敗(コアが判定)
    }
    slot->sampleRate = sampleRate;
    slot->maxBlockFrames = maxBlockFrames;
    slot->prepared = true;
    clearIo(*slot);
    return 1;
}

// ----------------------------------------------------------------- ps_reset
PRISM_EXPORT void ps_reset(int handle) {
    Slot* slot = resolve(handle);
    if (slot == nullptr || !slot->prepared) {
        return;
    }
    slot->shifter.reset();
}

// ---------------------------------------------------------------- ps_io_ptr
// HEAPF32 上の共有 I/O 領域。prepare 後に一度取得すれば以後不変(領域は静的)。
// 無効ハンドル・未 prepare・範囲外 channel では 0(= null)を返す。
PRISM_EXPORT float* ps_io_ptr(int handle, int channel) {
    Slot* slot = resolve(handle);
    if (slot == nullptr || !slot->prepared) {
        return nullptr;
    }
    if (channel != 0 && channel != 1) {
        return nullptr;
    }
    return slot->io[channel];
}

// --------------------------------------------------------------- ps_process
// 共有 I/O 領域を in-place 処理する(コアの process() は in[ch][i] を読んでから
// out[ch][i] を書くため、in == out で呼んで安全)。
PRISM_EXPORT void ps_process(int handle, int numFrames) {
    Slot* slot = resolve(handle);
    if (slot == nullptr || !slot->prepared) {
        return;
    }
    if (numFrames <= 0) {
        return;
    }
    int frames = numFrames;
    if (frames > slot->maxBlockFrames) {
        frames = slot->maxBlockFrames;  // 超過分は処理しない(コアと同じ方針)
    }
    const float* in[2] = {slot->io[0], slot->io[1]};
    float* out[2] = {slot->io[0], slot->io[1]};
    slot->shifter.process(in, out, frames);
}

// ------------------------------------------------------------- ps_set_param
// id: 0=shiftCentsL, 1=shiftCentsR, 2=dryWet, 3=crossfadeMs。
// 未知 id は無視。非有限値はコア側(storeClamped)が無視し、値はコアがクランプする。
PRISM_EXPORT void ps_set_param(int handle, int id, float value) {
    Slot* slot = resolve(handle);
    if (slot == nullptr) {
        return;
    }
    switch (id) {
        case 0:
            slot->shifter.setShiftCentsL(value);
            return;
        case 1:
            slot->shifter.setShiftCentsR(value);
            return;
        case 2:
            slot->shifter.setDryWet(value);
            return;
        case 3:
            slot->shifter.setCrossfadeMs(value);
            return;
        default:
            return;  // 未知 id は無視(契約 2)
    }
}

// ------------------------------------------------------------ ps_latency_ms
PRISM_EXPORT double ps_latency_ms(int handle) {
    Slot* slot = resolve(handle);
    if (slot == nullptr || !slot->prepared) {
        return 0.0;
    }
    return (slot->shifter.getLatencySamples() / slot->sampleRate) * 1000.0;
}

// ------------------------------------------------- 検証ハーネス向けの追加面
// 設計値遅延(サンプル)。C++ 検証 verify.cpp / JS テストとの突き合わせに使う。
PRISM_EXPORT double ps_latency_samples(int handle) {
    Slot* slot = resolve(handle);
    if (slot == nullptr || !slot->prepared) {
        return 0.0;
    }
    return slot->shifter.getLatencySamples();
}

// 現在ラッチ済みのクロスフェード窓長(サンプル)。
PRISM_EXPORT int ps_window_samples(int handle) {
    Slot* slot = resolve(handle);
    if (slot == nullptr || !slot->prepared) {
        return 0;
    }
    return slot->shifter.getWindowSamples();
}

// 遅延スイープ幅(サンプル)。最大遅れ = baseOffset + これ。
PRISM_EXPORT int ps_sweep_samples(int handle) {
    Slot* slot = resolve(handle);
    if (slot == nullptr || !slot->prepared) {
        return 0;
    }
    return slot->shifter.getSweepSamples();
}
