# Unit of Work — prism

| Unit ID | Directory | 名称 | kind | 複雑度 | 配備モデル |
|---|---|---|---|---|---|
| U1 | u1-dsp-core | DSP コア | library | M | なし(ソース提供、u2/u3 に埋め込み) |
| U2 | u2-verification | オフライン数値検証 | library | M | なし(ローカル実行バイナリ) |
| U3 | u3-web-demo | Web デモ | ui | M | 静的ファイル一式(ローカル配信、埋め込み型) |

## U1: u1-dsp-core(kind: library)

- 境界: `dsp/include/prism/PitchShifter.h`(ヘッダオンリー C++17)。コンポーネント PitchShifter を所有。
- 責務: ピッチシフト処理(FR-1.x)、入出力形式(FR-2.x)、遅延設計値 API。リアルタイム安全(NFR-2)。
- 制約: 依存ゼロ。clang++ と emcc の両方で -Wall -Wextra -Werror 警告ゼロ(NFR-5)。
- 実装ノート: D-01〜D-03(decisions.md)に従う。補間は線形、差し替え可能な構造。

## U2: u2-verification(kind: library)

- 境界: `tests/`(検証ランナー + 自前 FFT)。コンポーネント VerificationHarness を所有。
- 責務: オフライン検証 4 種(FR-3.x)を 44.1k/48k 両方で実行し終了コードで緑/赤を返す(US2.2)。
- 制約: 外部依存ゼロ、clang++ のみでビルド。マージ条件 = 全検証緑(team-practices)。
- 実装ノート: グリッチ判定は期待振幅に対する相対閾値。遅延実測は設計値 API と突き合わせ。

## U3: u3-web-demo(kind: ui)

- 境界: `web/`(WasmBridge C++、DemoWorklet JS、DemoUI HTML+JS、ビルドスクリプト)。
- 責務: FR-4.x、US1.1〜US1.4。refined-mockups の DOM/状態/インタラクション仕様どおり。
- 制約: 実行時外部依存ゼロ・外部送信なし(NFR-6)。Emscripten はビルド時のみ。
- 実装ノート: D-05(extern "C" + HEAPF32 共有)、D-04(遅延報告)、D-06(モノラル複製は Worklet)。

出典: Q1〜Q3(本ステージ質問票)、intent-backlog(プロト Unit)、components.md。

## Assumptions & Open Questions

None.

## Review

**Reviewer:** aidlc-architecture-reviewer-agent
**Date:** 2026-08-27T20:55:41Z
**Iteration:** 1
**Verdict:** READY

### Summary

Dependency DAG is acyclic and topologically sound. All three units (u1-dsp-core, u2-verification, u3-web-demo) have unambiguous single responsibilities aligned with domain-design component ownership. All six upstream user stories (US1.1–US1.4, US2.1–US2.2) are fully mapped to implementing units with complete traceability. Cross-references resolve: both u2 and u3 correctly depend only on u1; integration points (C++ header include, extern "C" WASM API, postMessage protocol) are explicit and match component contracts. No circular dependencies. A developer could build this without architectural clarification. Proceed to construction phase.
