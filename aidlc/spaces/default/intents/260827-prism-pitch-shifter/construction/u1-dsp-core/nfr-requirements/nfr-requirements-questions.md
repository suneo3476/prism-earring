# NFR Requirements — 質問票(u1-dsp-core)

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. 数値異常(NaN/Inf 入力、デノーマル)の扱いは？

- A. 入力の NaN/Inf はそのまま通す(検知コスト回避。上流のマイク入力では実質発生しない)。内部状態はデノーマル対策として平滑器・フィードバック経路に微小 DC(1e-20)加算または flush-to-zero 前提を明記。セッターの NaN は無視(既存値維持)
- B. 全サンプルを検査しサニタイズ
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — per-sample 検査はリアルタイム経路のコスト。セッター側(制御レート)のみ防御。(2026-08-27T21:40:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- 音声経路は NaN 検査なし、デノーマル対策あり。セッターの NaN は無視(制御レートで防御)

- Looks correct
- Request changes

[Answer]: Looks correct
