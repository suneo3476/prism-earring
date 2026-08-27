# Functional Design — 質問票(u3-web-demo)

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. WASM ロードの配布形態は？

- A. emcc の -sSINGLE_FILE=1 で WASM を JS に内包し、配布物を index.html + worklet.js(+ main.js)の静的ファイル数枚に抑える(file:// でも同一オリジン問題を回避しやすい)
- B. .wasm を別ファイルで fetch
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — 「セットアップが楽」を最優先(確認済み方針)。サイズ増は許容(コアは小さい)。(2026-08-27T21:15:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- WASM は SINGLE_FILE で JS に内包、静的ファイル数枚で配布

- Looks correct
- Request changes

[Answer]: Looks correct
