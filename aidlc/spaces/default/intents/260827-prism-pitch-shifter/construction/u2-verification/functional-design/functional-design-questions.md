# Functional Design — 質問票(u2-verification)

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. 検定の合否判定はどの粒度で出しますか？

- A. ケースごとに PASS/FAIL 行を標準出力し、1 つでも FAIL があれば終了コード 1。CPU 比は情報表示(閾値なし、報告のみ — FR-3.4 は計測・報告が要件)
- B. CPU 比にも合格閾値を設ける
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — CPU 閾値は実行環境依存が大きく、要件も「計測」まで。(2026-08-27T21:15:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- ケース別 PASS/FAIL + 終了コード。CPU 比は報告のみ

- Looks correct
- Request changes

[Answer]: Looks correct
