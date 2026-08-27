# Functional Design — 質問票(u1-dsp-core)

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. crossfade_ms 変更時の窓長再構成はどう扱いますか？

窓長(リングバッファ読み出し窓)が動作中に変わると、ポインタ位相の不連続が出得ます。

- A. crossfade_ms も他パラメータ同様 20ms 平滑を通し、窓長は「次のクロスフェード境界」でのみ更新する(フェーズ跨ぎの不連続ゼロ)
- B. 即時再構成(短いグリッチ許容)
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — グリッチゼロ要件(NFR-4)を守る唯一の安全な更新点。(2026-08-27T21:15:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- 窓長変更はクロスフェード境界でのみ適用(グリッチゼロ維持)。他パラメータは 20ms 平滑

- Looks correct
- Request changes

[Answer]: Looks correct
