# 契約サマリ — prism

Unit 間・境界間の契約 3 面。ネットワーク API・共有 DB・イベントバスは存在しない。

## 契約 1: C++ コア API(u1 の公開面。消費者: u2, u3)

`dsp/include/prism/PitchShifter.h`(ヘッダオンリー、namespace `prism`、依存ゼロ)[Q1]

```cpp
namespace prism {
class PitchShifter {
public:
    // 初期化時のみ呼ぶ(ヒープ確保はここだけ)。fs: 8000〜192000、maxBlockFrames >= 1
    // 成功で true。以後 process 呼び出し可
    bool prepare(double sampleRate, int maxBlockFrames);
    void reset() noexcept;                       // 状態クリア(確保済みバッファは保持)
    // 非インタリーブ 2ch。in/out は [2][numFrames]。numFrames <= maxBlockFrames
    // リアルタイム安全(確保・ロック・I/O・例外なし)
    void process(const float* const* in, float* const* out, int numFrames) noexcept;
    // 任意スレッドから可(atomic store)。範囲外はクランプ
    void setShiftCentsL(float cents) noexcept;   // -150..0
    void setShiftCentsR(float cents) noexcept;   // -150..0
    void setDryWet(float mix) noexcept;          // 0..1
    void setCrossfadeMs(float ms) noexcept;      // 10..100
    // 設計値遅延(基準オフセット + 平均ポインタ遅れ)。現在パラメータ基準
    double getLatencySamples() const noexcept;
};
}
```

事前条件・保証:

- `process` は `prepare` 成功後のみ有効。モノラル入力の複製は呼び出し側(D-06)。
- セッターは lock-free(atomic)。適用は per-sample 平滑化(時定数 20ms ±25%)経由(D-03)。
- 窓長 `window_samples = round(crossfadeMs × fs / 1000)`。読み出し遅れ上限 `(1−ratio)×窓長`(US1.2)。

## 契約 2: extern "C" WASM API(u3 内 JS↔C++ 境界)[Q2]

| 関数 | シグネチャ | 意味 |
|---|---|---|
| `ps_create` | `() -> handle(i32)` | インスタンス生成(0 = 失敗) |
| `ps_destroy` | `(handle) -> void` | 破棄 |
| `ps_prepare` | `(handle, f64 fs, i32 maxBlock) -> i32` | 1 = 成功 |
| `ps_io_ptr` | `(handle, i32 channel) -> f32*` | 共有 I/O 領域(HEAPF32、in-place 処理。ch=0/1) |
| `ps_process` | `(handle, i32 numFrames) -> void` | 共有領域を in-place 処理 |
| `ps_set_param` | `(handle, i32 id, f32 value) -> void` | id: 0=shiftCentsL, 1=shiftCentsR, 2=dryWet, 3=crossfadeMs。範囲外 id・非有限値は無視(値クランプはコア) |
| `ps_latency_ms` | `(handle) -> f64` | 設計値遅延 ms |

- レンダ量子ごとの確保ゼロ: I/O 領域は prepare 時に確保し `ps_io_ptr` で共有。
- 例外は JS 境界に漏らさない(コアは例外を投げない)。

## 契約 3: postMessage プロトコル(u3 内 Worklet↔UI)

| 方向 | メッセージ | 意味 |
|---|---|---|
| UI → Worklet | `{type:"param", id:0..3, value:number}` | パラメータ変更(契約 2 の id と同一)|
| Worklet → UI | `{type:"latency", dspLatencyMs:number}` | 開始時 + 1 秒ごとの遅延設計値報告(D-04)|
| Worklet → UI | `{type:"ready"}` | WASM ロード・prepare 完了 |
| Worklet → UI | `{type:"error", message:string}` | 初期化失敗(WASM ロード不可等) |

バージョニング: 契約はリポジトリ内で同一コミットに共存するため互換性維持は不要(内部契約)。変更時は 3 面を同時に更新する。

出典: Q1〜Q2(本ステージ質問票)、decisions.md(D-03〜D-06)、unit-of-work-dependency.md(統合点)、requirements.md(FR-2.3)。

## Assumptions & Open Questions

None.

## Review

**Reviewer:** aidlc-architecture-reviewer-agent
**Date:** 2026-08-27T20:59:08Z
**Iteration:** 1
**Verdict:** READY

### Findings

| ID | Severity | Location | Finding | Required action | Status |
|---|---|---|---|---|---|
| R-01 | Minor | 契約 3: postMessage プロトコル | Error case "{type:"error", message:string}" is documented as "初期化失敗(WASM ロード不可等)" but does not explicitly enumerate all conditions that trigger the error message (e.g., is ps_prepare return value of 0 a trigger, or only WASM load failure?) | Clarify in Contract 3 that {type:"error"} is sent when WASM module load fails OR ps_prepare returns 0; document the exact trigger conditions | New |

### Summary

Architecture is sound: three contracts correctly partition inter-unit boundaries (C++ API), intra-unit adapter layers (WASM bridge), and async messaging within u3. Dependency DAG shows u2 and u3 consume only u1; no circular dependencies. All contract signatures are concrete and implementable. Data-flow tracing from UI slider → parameter application → audio processing confirms end-to-end clarity. References to decisions.md (D-03~D-06), requirements, and Q&A are present. Minor suggestion: clarify error-condition enumeration in Contract 3 for robustness; does not block construction.
