# User Stories — 質問票

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. ストーリーの粒度はこの構成でよいですか？

- A. デモ利用 4 本(開始/合わせ込み/エラー復帰/遅延確認)+ 技術 2 本(DSP コア/数値検証)の計 6 本。FR/NFR は受け入れ条件と traceability.json で全数カバー
- B. FR ごとに 1 ストーリー(22 本)
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — ペルソナ 1 名・Unit 3 つの構成に釣り合う最小粒度。(2026-08-27T17:00:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- ストーリー 6 本(US1.1〜US1.4 デモ利用、US2.1〜US2.2 技術)、ペルソナは P1(開発者本人)のみ
- 全 FR/NFR を受け入れ条件にマッピング(traceability.json で status OK 全数)

- Looks correct
- Request changes

[Answer]: Looks correct
