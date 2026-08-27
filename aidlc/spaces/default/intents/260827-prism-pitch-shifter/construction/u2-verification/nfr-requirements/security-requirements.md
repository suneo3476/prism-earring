# セキュリティ・NFR 要件 — u2-verification

検証ランナー Unit。攻撃面はローカル実行のみで、中核 NFR は再現性と依存ゼロ。

## SR-1 隔離とオフライン性

- SR-1.1: ネットワークアクセスなし。入出力は標準出力(結果)とメモリ内信号のみ。ファイル書き出しは行わない(必要になれば将来オプション)。[desc]
- SR-1.2: 外部依存ゼロ(標準ライブラリのみ)。FFT は自前 radix-2 実装(検証側限定 — 音声経路には FFT を置かない)。[desc]

## SR-2 再現性(決定論)

- SR-2.1: 信号生成(正弦波・インパルス)は決定論。乱数を使用しない。同一入力・同一パラメータで常に同一の PASS/FAIL。[Q1]
- SR-2.2: CPU 計測(BR2.4)のみ実行環境依存であり、合否に関与しない報告値であることを出力に明記。[Q1]

## SR-3 判定の健全性

- SR-3.1: 合否閾値(0.95±0.5%、10ms、k=3.0 等)はコード中の名前付き定数とし、検証を通すための場当たり的な緩和を禁止(team-practices「品質目標を弱めない」)。
- SR-3.2: 終了コード契約: 全緑=0、いずれか FAIL=1、実行不能(prepare 失敗等)=2。

## Assumptions & Open Questions

None.

## Review

**Reviewer:** aidlc-architecture-reviewer-agent
**Iteration:** 1
**Verdict:** NOT-READY
**Date:** 2026-08-27T21:34:51Z

### Findings

| ID | Severity | Location | Finding | Required action | Status |
|---|---|---|---|---|---|
| R-01 | Critical | traceability.json, upstream_ids | NFR-2 (Real-time safety: no allocations/locks/I/O in processing callback) from inception/requirements-analysis/requirements.md is completely absent from upstream_ids list. Traceability.json lists ["NFR-1", "NFR-3", "NFR-4", "NFR-5", "NFR-6"] but omits NFR-2, violating the stage requirement to address all upstream NFR requirements. | (1) Add NFR-2 to traceability.json upstream_ids; (2) Create a corresponding security requirement in security-requirements.md (e.g., SR-4 covering real-time safety constraints for verification runner callback integrity). (3) Update coverage mapping. | New |
| R-02 | Critical | security-requirements.md > SR-3.2 vs. functional-spec.md > WF-6 | SR-3.2 specifies exit code 2 ("実行不能(prepare 失敗等)") for non-executable states, but functional-spec.md WF-6 and rules.md BR2.5 define only exit codes 0/1 with no mention of when/how code 2 is emitted. Implementers cannot determine when to use exit code 2. This breaks the exit code contract. | (1) Update functional-spec.md WF-6 to explicitly define the prepare-failure/non-executable condition and its exit code 2 mapping. (2) Ensure rules.md BR2.5 or WF-6 clarifies which layer transitions detect prepare failure and emit exit 2. (3) Cross-reference SR-3.2 ↔ WF-6 ↔ BR2.5 for consistency. | New |
| R-03 | Major | security-requirements.md > SR-2 | SR-2 asserts deterministic signal generation and reproducible verification, but does not specify numerical precision guarantees for FFT peak detection (parabolic interpolation, radix-2 implementation) across different C++17 compilers/platforms. Floating-point behavior may vary subtly; document tolerance for "bit-exact" reproducibility or specify acceptable variance range for frequency detection. | (1) Add precision specification to SR-2: either "bit-exact reproducibility across all C++17 implementations" (if required) or "frequency detection tolerance ±X Hz" (if variance is acceptable). (2) Document FFT validation approach (e.g., reference frequency at 440 Hz must detect 0.95*440 Hz ± tolerance). (3) Reference tech-stack-decisions.md FFT configuration (radix-2, 32768 size, Hann window, parabolic interpolation) as normative. | New |
| R-04 | Major | security-requirements.md > SR-1.1 | SR-1.1 claims "ファイル書き出しは行わない(必要になれば将来オプション)" (no file output, future option if needed). Hedging language "if needed" and "future option" undermines the absolute isolation requirement. Unclear conditions under which file output would be activated, compromising the offline/isolation security posture claim. | Rewrite SR-1.1 as absolute: "ファイル書き出しは行わない。今後の要件変更にはセキュリティレビューを必須とする。" (No file output. Future changes require security review.) OR explicitly define triggering conditions and security controls if file output is actually anticipated. | New |
| R-05 | Major | traceability.json > "reverse" entries | Reverse entries SR-2.2 ("measurement-environment disclosure rule") and SR-3.2 ("exit-code contract from functional design Q1") are marked N/A but semantically these reference upstream functional design. Unclear whether these are anti-requirements, trace-back mappings, or bidirectional references. The "reverse" framing creates ambiguity about what the traceability relationship actually means. | Clarify traceability.json semantics: (1) Define whether "reverse" entries mean "derived from downstream artifact" (trace-back) or "excluded from this stage's scope"; (2) either convert to normal coverage entries with target SR-* references, or document why N/A is appropriate with explicit justification. (3) Reconcile SR-3.2's exit code contract with functional-spec WF-6 (see finding R-02). | New |

### Summary

Two critical gaps prevent implementation: NFR-2 (real-time safety constraints on callback execution) is entirely absent from this artifact and traceability, and the exit code contract (SR-3.2) is inconsistent with the functional specification (0/1/2 vs 0/1), leaving developers without guidance. Three major issues affect reproducibility guarantees (FFT precision), isolation claim clarity (file output hedging), and traceability semantics (reverse N/A entries). Revisions required before handoff.
