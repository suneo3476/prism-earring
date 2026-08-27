# NFR Requirements — 質問票(u2-verification)

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. 検証の再現性はどう担保しますか？

- A. 信号生成は決定論(乱数不使用)。同一入力・同一パラメータで常に同一結果。CPU 計測のみ環境依存(報告値)と明記
- B. 乱数シードを固定して使用
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A (2026-08-27T21:45:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- 決定論的検証(乱数なし)。CPU 計測のみ環境依存の報告値

- Looks correct
- Request changes

[Answer]: Looks correct
