# Contract Design — 質問票

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. C++ コア API の形はこれでよいですか？

- A. `prism::PitchShifter` クラス: `prepare(fs, maxBlock)` / `reset()` / `process(in, out, numFrames)`(非インタリーブ 2ch)/ セッター 4 種(setShiftCentsL/R, setDryWet, setCrossfadeMs — atomic store)/ `getLatencySamples()`。ヘッダオンリー
- B. C スタイル関数群
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A (2026-08-27T21:00:00Z, **Mode:** chat — FR-2.3、D-03 どおり)

## Q2. WASM API のパラメータ ID 割当は？

- A. 0=shiftCentsL, 1=shiftCentsR, 2=dryWet, 3=crossfadeMs(D-05 の enum を確定)。範囲外 ID・非有限値は黙って無視(クランプはコア側)
- B. 文字列キー
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A (2026-08-27T21:00:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- C++ API: prepare/reset/process/セッター4種/getLatencySamples のヘッダオンリークラス
- WASM API: パラメータ ID 0〜3、不正入力は無視(クランプはコア)

- Looks correct
- Request changes

[Answer]: Looks correct
