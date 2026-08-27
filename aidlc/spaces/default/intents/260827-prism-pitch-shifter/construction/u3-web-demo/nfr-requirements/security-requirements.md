# セキュリティ・NFR 要件 — u3-web-demo

UI Unit。中核 NFR はプライバシー(音声データの非流出)と可搬性(静的ファイル)。

## SR-1 プライバシー(NFR-6)

- SR-1.1: 音声データをデバイス外に送信しない。fetch/XHR/WebSocket/Beacon を一切使用しない。[Q1] [desc]
- SR-1.2: 録音・保存をしない(localStorage / IndexedDB / File API への音声書き出しなし)。[Q1]
- SR-1.3: 解析スクリプト・トラッキングなし。[Q1]

## SR-2 外部リソースゼロ

- SR-2.1: スクリプト・CSS・フォントはすべて同梱またはインライン。外部オリジンへのリクエストが 1 本も発生しない(WASM は SINGLE_FILE で JS 内包 — 機能設計 Q1)。[Q1]
- SR-2.2: 生成物に `<meta http-equiv="Content-Security-Policy" content="default-src 'self' 'unsafe-inline' blob:; connect-src 'none'">` 相当の CSP を付し、外部接続を宣言的にも遮断(blob: は Worklet モジュール用)。

## SR-3 権限の最小化

- SR-3.1: 要求する権限はマイクのみ。取得は開始ボタン押下時(ユーザー操作起点)に限る。停止時にトラックを stop() し、インジケータを確実に消す。[desc]

## SR-4 ビルド再現性

- SR-4.1: emsdk のバージョンを README に固定表記(DevSecOps 視点の軽量ロック)。ビルドスクリプトは 1 コマンド。

## Assumptions & Open Questions

None.

## Review

**Reviewer:** aidlc-architecture-reviewer-agent
**Iteration:** 1
**Verdict:** READY
**Date:** 2026-08-27T21:36:28Z

### Findings

| ID | Severity | Location | Finding | Required action | Status |
|---|---|---|---|---|---|
| R-01 | Minor | SR-2.2 CSP Implementation | CSP policy value specified as meta tag example but no explicit clarification whether HTTP header (preferred) or meta tag should be the deployment mechanism | Optionally document in tech-stack-decisions or README which CSP delivery mechanism is chosen (header vs meta tag); requirement allows both but header is more standards-compliant | New |
| R-02 | Minor | Overall cross-unit coherence | Traceability.json maps NFR-1 (latency measurement in u3 context) to SR-3.1 (microphone permissions) without clear rationale; latency is not primarily a security concern | Clarify or correct traceability mapping: NFR-1 should map to performance-requirements deliverables, not security requirements; if connection exists (permissions → latency), document the reasoning | New |

### Validation

✓ All upstream requirements (NFR-1, NFR-5, NFR-6 from requirements.md) are addressed or justified.
✓ Cross-references ([Q1], [desc], DevSecOps note) resolve to existing upstream artifacts (nfr-requirements-questions.md, CLAUDE.md initial description, team practices).
✓ Contracts 2 and 3 (postMessage, WASM API) impose no additional security burden; CSP and permission minimization support their correct implementation.
✓ Functional-spec workflows (WF-1 to WF-6) require no external resource fetches, no data transmission, no tracking — all enabled by SR-1 and SR-2.
✓ Frontend-components UI layer has no external dependencies and aligns with SR-2.1 and SR-3.1.
✓ Tech-stack-decisions (SINGLE_FILE, static delivery, HTTPS on LAN) are fully supported by security requirements.

### Summary

Security architecture is sound and implementable. Privacy (no external data transmission), isolation (no external resources, CSP), and permissions (microphone only, on-demand) are correctly partitioned across four SR categories with concrete, verifiable constraints. SINGLE_FILE WASM embedding and CSP blob: allowance are correctly justified for the AudioWorklet context. Build reproducibility is appropriate for personal use. All implementability checks pass: developer has sufficient detail to implement without architectural guidance beyond this document. Minor notes on traceability mapping (R-02) and CSP delivery mechanism (R-01) do not block construction.
