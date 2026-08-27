# セキュリティ・NFR 要件 — u1-dsp-core

ライブラリ Unit のため、方針は「攻撃面・不確定動作を作らない」。Web 系 NFR(認証・認可・暗号化)は該当なし。

## SR-1 攻撃面ゼロ

- SR-1.1: コアはネットワーク・ファイル・環境変数・システムコールに一切触れない(NFR-2、NFR-6 の基盤)。[desc]
- SR-1.2: 外部依存ゼロ(標準ヘッダ `<atomic> <cmath> <cstdint> <vector>`(初期化時のみ)程度に限定)。サプライチェーンリスクなし。[desc]

## SR-2 メモリ・境界安全

- SR-2.1: すべてのバッファアクセスは prepare 時に確定した容量に対する剰余/クランプ済みインデックスで行う。範囲外アクセスなし。[desc]
- SR-2.2: process の numFrames > maxBlockFrames はデバッグビルドで assert、リリースでは maxBlockFrames にクランプして処理(未定義動作にしない)。
- SR-2.3: 未初期化(prepare 前/失敗後)の process 呼び出しは出力ゼロ埋めで安全に返す。

## SR-3 数値安全

- SR-3.1: 音声経路では NaN/Inf 検査を行わない(リアルタイムコスト回避)。[Q1]
- SR-3.2: セッターに渡された NaN は無視(既存値維持)。有限値は範囲クランプ(BR1.2)。[Q1]
- SR-3.3: デノーマル対策: 平滑器状態に微小オフセット(1e-20)を加算し、持続的デノーマル演算による CPU スパイクを防ぐ。[Q1]

## SR-4 リアルタイム安全(NFR-2 の再掲・検証観点)

- SR-4.1: process 経路にヒープ確保・ロック・I/O・例外・システムコールなし。コードレビューと Bolt B2 の CPU 計測で確認。[desc]

## Assumptions & Open Questions

None.

## Review

**Reviewer:** aidlc-architecture-reviewer-agent
**Iteration:** 1
**Verdict:** READY

### Findings

| ID | Severity | Location | Finding | Required action | Status |
|---|---|---|---|---|---|
| R-01 | Major | nfr-requirements/traceability.json upstream_ids field | NFR-3 (Pitch accuracy: 95% ±0.5% at 110/440/3520 Hz) and NFR-4 (Continuity: no glitches in continuous sine wave output) are defined in inception/requirements-analysis/requirements.md but omitted from upstream_ids in nfr-requirements/traceability.json. These are critical algorithm design constraints for a pitch shifter — not verification-only concerns. While addressed in functional-design rules (BR1.1 for pitch accuracy via 2^(cents/1200) formula; BR1.3/BR1.6 for continuity via per-sample smoothing and equal-power crossfading), a developer reading only the nfr-requirements stage would not see these requirements. This creates a traceability gap. | Add NFR-3 and NFR-4 to upstream_ids in traceability.json; map each in coverage section to the functional-design rules that implement them (e.g., NFR-3 → BR1.1; NFR-4 → BR1.3, BR1.6). Alternatively, explicitly mark them N/A with documented justification if they are verification-only concerns of u2 rather than u1 design concerns. | New |
| R-02 | Minor | security-requirements.md SR-3.1 | SR-3.1 ("音声経路では NaN/Inf 検査を行わない") is implemented based on an assumption that microphone input will not produce NaN/Inf, documented in nfr-requirements-questions.md Q1-A ("上流のマイク入力では実質発生しない"). This assumption is not referenced in the security-requirements.md artifact itself, making it less visible to readers. | Add a [Q1] reference note to SR-3.1 clarifying that this tradeoff assumes real microphone input will not produce NaN/Inf, or explicitly document the assumption in an "Assumptions" section. | New |

### Summary

The library's security and safety requirements are well-founded: zero attack surface (no network/file/syscalls/dependencies), memory and boundary safety with defensive overflow handling, numeric safety with documented NaN passthrough and denormal protection, and real-time safety through lock-free parameter updates and fixed-size buffers. All security-related NFRs (NFR-2, NFR-5, NFR-6) are properly traced. However, two critical algorithm-design NFRs (NFR-3 pitch accuracy, NFR-4 continuity) are absent from the traceability, creating a documentation gap. These constraints ARE addressed in the functional design but should be explicitly mapped in the nfr-requirements stage for completeness. With this single clarification, the artifact is complete and implementable.
