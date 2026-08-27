# Delivery Planning — 質問票

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. 何から作りますか？(ビルド 1 回分の作業単位 = Bolt。動くものが 1 つ出来て終わる)

- A. リスク先行 = 依存順: DSP コア → 数値検証 → Web デモ。コアの品質リスク(ピッチ精度・遅延)を最初の 2 Bolt で潰す
- B. デモ先行(見た目から)
- C. 薄い一気通貫スライス(ウォーキングスケルトン)を最初に
- X. Other (please specify)

[Answer]: A — scope-definition Q3 で確認済みの順序。スケルトン儀式は practices で省略と確認済み(全体が最小規模)。(2026-08-27T21:05:00Z, **Mode:** chat)

## Q2. Bolt の粒度と並行度は？

- A. Bolt = Unit の 1:1 で 3 本、直列実行。開発者 1 名(ソロ)のため並行の利益なし。WSJF 等の採点モデルは不要(3 本の順序は依存で一意)
- B. 複数 Bolt 並行
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A (2026-08-27T21:05:00Z, **Mode:** chat)

## Q3. 外部依存・最大の懸念は？

- A. 外部チーム依存なし。外部依存は Emscripten の導入のみ(Bolt 3 をブロック。失速時は AudioWorklet 内 JS 実装に切替 — raid-log R-03)。最大の懸念はピッチ精度検定の合格(Bolt 2 で最初に判明。不合格なら補間を Hermite に切替 — D-02)
- B. その他の外部依存がある
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A (2026-08-27T21:05:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- Bolt 3 本を直列: B1=u1-dsp-core → B2=u2-verification → B3=u3-web-demo(リスク先行 = 依存順)
- 採点モデル不要・並行なし(ソロ開発)
- 外部依存は Emscripten のみ(切替策あり)。最大懸念はピッチ精度(B2 で早期判明、D-02 の切替策)

- Looks correct
- Request changes

[Answer]: Looks correct
