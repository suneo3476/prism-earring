# NFR Requirements — 質問票(u3-web-demo)

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. ページのセキュリティ境界は？

- A. 外部リソース読み込みゼロ(スクリプト・フォント・CSS すべて同梱/インライン)。音声データはページ外に出さない。localStorage 等への録音保存もしない
- B. CDN 利用
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — NFR-6 と DevSecOps 視点の指摘どおり。(2026-08-27T21:45:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- 外部リソースゼロ・音声データ非送信・録音保存なし

- Looks correct
- Request changes

[Answer]: Looks correct
