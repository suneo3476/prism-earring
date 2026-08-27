# Refined Mockups — 質問票

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. ビジュアルの方針は？

- A. 素の HTML + 最小 CSS(システムフォント、単一カラム、ライト/ダーク自動)。デザインシステム・CSS フレームワークは使わない
- B. CSS フレームワーク(Tailwind 等)導入
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — 「UI 作り込みはスコープ外」「実行時依存ゼロ」の確認済み方針どおり。(2026-08-27T17:10:00Z, **Mode:** chat)

## Q2. スライダー操作の反映タイミングは？

- A. input イベントで即時反映(ドラッグ中も連続反映)。値表示も同時更新
- B. change イベント(離した時)のみ反映
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — 「聴きながら合わせ込む」(US1.2)には連続反映が必須。DSP 側の平滑化がノイズを防ぐ。(2026-08-27T17:10:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- ビジュアル: 素の HTML + 最小 CSS、フレームワークなし、ライト/ダーク自動
- 操作反映: input イベントで即時連続反映(平滑化は DSP 側)

- Looks correct
- Request changes

[Answer]: Looks correct
