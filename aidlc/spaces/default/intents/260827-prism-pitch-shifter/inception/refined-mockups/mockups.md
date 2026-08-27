# 詳細モックアップ — prism Web デモ

単一ページ。素の HTML + 最小 CSS(システムフォント、単一カラム、`prefers-color-scheme` でライト/ダーク自動)。[Q1]

## 画面構造(DOM 対応)

```
<main>                                  … 唯一のランドマーク
  <h1> prism — Prism Earring demo
  <p.note> ※イヤホン推奨(スピーカーはハウリングします)
  <p.status role="status" aria-live="polite"> 状態: 停止中
  <button#toggle> ▶ 開始                … 初期フォーカス
  <section#controls>                    … 動作中以外は disabled
    <label> シフト量 L <input#shiftL type="range" min=-150 max=0 step=1 value=-89> <output>-89 cent
    <label> シフト量 R <input#shiftR type="range" min=-150 max=0 step=1 value=-89> <output>-89 cent
    <label> <input#link type="checkbox" checked> 左右連動
    <label> Dry/Wet <input#dryWet type="range" min=0 max=1 step=0.01 value=1> <output>1.00
    <label> 窓長 <input#xfade type="range" min=10 max=100 step=1 value=50> <output>50 ms
  </section>
  <section#latency>
    実測往復遅延: <span#latTotal>-- ms</span>
    内訳: 出力 <span#latOut>--</span> / ブロック <span#latBlock>--</span> / DSP <span#latDsp>--</span>
  </section>
  <div#error hidden role="alert">       … エラー表示領域(マイク拒否 / 非対応ブラウザ)
</main>
```

## 状態とビジュアル

| 状態 | status 表示 | toggle | controls | error |
|---|---|---|---|---|
| 停止中(初期) | 状態: 停止中 | ▶ 開始 | disabled | hidden |
| 動作中 | 状態: 動作中 | ■ 停止 | enabled | hidden |
| マイク拒否 | 状態: マイクが許可されていません | ▶ 再試行 | disabled | 許可手順の案内を表示 |
| 非対応ブラウザ | 状態: 非対応ブラウザ | disabled | disabled | 対応ブラウザ(Chrome 最新安定版 / Safari 17+)案内 |

出典: ワイヤーフレーム(rough-mockups)、US1.1〜US1.4 受け入れ条件。

## Assumptions & Open Questions

None.

## Review

**Reviewer:** aidlc-product-lead-agent
**Iteration:** 1
**Verdict:** READY

### Findings

| ID | Severity | Location | Finding | Required action | Status |
|---|---|---|---|---|---|
| R-01 | Major | interaction-spec.md > スライダー反映経路(line 14) | Atomic/thread-safety semantics vague: "atomic 相当の受け渡し" does not specify std::atomic<T> vs. single-threaded passing. For real-time safety (NFR-2: no locking in audio callback), developer needs explicit guidance on memory model. | Clarify: are we using std::atomic<float> with explicit load() on audio thread, or single-threaded semantics? Document the exact synchronization mechanism for shift_cents_L/R, dry_wet, crossfade_ms. | New |
| R-02 | Major | interaction-spec.md > 遅延表示(lines 21-24) | Latency reporting mechanism underspecified. Spec says "Worklet から報告される" but does not define postMessage format, timing, or whether DSP reports block the audio thread. Developer will guess at message structure and synchronization. | Specify: (1) the exact Worklet→main message format (e.g., {dspLatencyMs: number}), (2) timing (every 1 second? on demand?), (3) whether this adds any audio-thread cost. | New |
| R-03 | Minor | accessibility-checklist.md > Items 7–8 (contrast, zoom testing) | Acceptance criteria for accessibility QA missing. Items marked "実装時確認" but no measurable pass/fail defined (e.g., WCAG AA contrast ≥4.5:1, 200% zoom without horizontal scroll). | Add concrete QA criteria: contrast ratio thresholds (light/dark modes), zoom test procedure, and tools (e.g., axe DevTools, browser zoom to 200%). | New |

### Summary

Mockups trace fully to user stories (US1.1–US1.4) and requirements (FR-4.x). All control states documented, error handling (mic-deny, unsupported browser) specified with recovery flows, and accessibility framework sound (ARIA, keyboard, focus). Two clarifications needed for real-time audio safety and latency reporting mechanism; neither represents a contradiction with upstream or missing required artifact content — both are resolvable during Construction by developer dialogue with product. Third finding is a QA procedure gap, not a design gap. Engineering can proceed.
