# ワイヤーフレーム — prism Web デモ

画面は 1 枚のみ。UI の作り込みはスコープ外(確認済み)。[Q1]

```
+--------------------------------------------------+
|  prism — Prism Earring demo            (h1)      |
|  ※イヤホン推奨(スピーカーはハウリングします)   |
|                                                  |
|  [ ▶ 開始 ]  状態: 停止中 / 動作中               |
|                                                  |
|  シフト量 L  [-150 ──────●── 0] -89 cent         |
|  シフト量 R  [-150 ──────●── 0] -89 cent         |
|  [x] 左右連動                                    |
|                                                  |
|  Dry/Wet     [0.0 ─────────● 1.0] 1.00           |
|  窓長        [10 ────●───── 100] 50 ms           |
|                                                  |
|  実測往復遅延: -- ms                              |
|  (出力遅延 + 処理ブロック遅延 + DSP 内部遅延)    |
+--------------------------------------------------+

エラー状態(マイク拒否時):
+--------------------------------------------------+
|  状態: マイクが許可されていません                 |
|  ブラウザのサイト設定でマイクを許可してから       |
|  [ ▶ 再試行 ] を押してください                    |
+--------------------------------------------------+

非対応ブラウザ(AudioWorklet/WASM なし)時:
+--------------------------------------------------+
|  このブラウザは対応していません。                 |
|  Chrome(Android/Mac)または Safari 最新版を      |
|  お使いください                                   |
+--------------------------------------------------+
```

アクセシビリティ注記: h1 見出し 1 つ + main ランドマークのみの単一領域。初期キーボードフォーカスは開始ボタン。全スライダーは `<input type="range">`(ラベル・値のテキスト表示付き)でキーボード操作可能。[Q1]

要素対応表:

| 要素 | 対応パラメータ / 機能 | Source |
|---|---|---|
| 開始/停止ボタン | getUserMedia 取得 + AudioContext resume / suspend。拒否時は再試行ボタンに切替 | [Q1] [Q2] |
| シフト量 L/R スライダー | shift_cents_L / shift_cents_R(-150〜0、既定 -89)、連動トグルで同時操作 | [Q1] |
| Dry/Wet スライダー | dry_wet(0.0〜1.0、既定 1.0) | [Q1] |
| 窓長スライダー | crossfade_ms(10〜100、既定 50) | [Q1] |
| 遅延表示 | outputLatency + 処理ブロック(バッファ)遅延 + DSP 内部遅延(クロスフェード窓)の合算と内訳 | [Q1] |
| エラー表示 | マイク拒否: 設定案内 + 再試行。非対応ブラウザ: 対応ブラウザ案内 | [Q2] |

## Assumptions & Open Questions

None.

## Review

**Reviewer:** aidlc-product-lead-agent
**Date:** 2026-08-27T16:49:11Z
**Iteration:** 1
**Verdict:** READY

| ID | Severity | Location | Finding | Required action | Status |
|---|---|---|---|---|---|
| R-01 | Major | wireframes.md > Accessibility note (line 38) | Missing accessibility metadata for initial load and controls | Added h1, main landmark, initial focus, range input guidance | Resolved |
| R-02 | Major | wireframes.md > Error states (lines 23-28) & user-flow.md | Mic-denied state not visualized, no recovery flow | Added error-state wireframe with retry button and retry loop in user flow | Resolved |
| R-03 | Minor | wireframes.md > Latency display (line 20) | Latency measurement scope ambiguous | Added breakdown: output latency + block delay + DSP internal latency | Resolved |
| R-04 | Minor | wireframes.md > Fallback states (lines 30-35) | Unsupported browser fallback missing | Added unsupported-browser state with Chrome/Safari guidance | Resolved |
| R-05 | Minor | wireframes.md > Accessibility note (line 38) | Error state accessibility not documented | Extend accessibility note to specify focus management and screen reader announcement (role/aria-live) for error overlays (mic-denied, unsupported-browser) | New |

**Summary**

All prior findings from the initial review are resolved: accessibility metadata now covers the main state, error states are specified with recovery flows, latency measurement is clearly broken down, and unsupported-browser fallback is present. One new minor finding: the accessibility guidance should explicitly address error-state focus management and screen reader announcements to ensure developers implement error overlays accessibly. This does not block implementation—developers can add these as part of accessible QA—but should be documented for completeness.
