# Scope Definition — 質問票

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. 価値を届けられる最小スコープ(must-have)はどれですか？(select all that apply)

- A. prism::PitchShifter DSP コア(純 C++17、ステレオ、実行時可変パラメータ)
- B. オフライン数値検証テスト(ピッチ精度・レイテンシ・グリッチ・CPU 負荷)
- C. WASM/AudioWorklet Web デモ(Pixel Chrome / Mac ブラウザ、パラメータ UI 付き)
- D. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A, B, C — 3 点セットで「Pixel でも Mac でも試せるデモ」というユーザー確認済みゴールが成立する。(2026-08-27T16:42:00Z, **Mode:** chat — intent-capture Q7/Q8 の確認済み境界より)

## Q2. nice-to-have(今回はやらない)の線引きはどこですか？

- A. JUCE アプリ・Android ネイティブ・帯域分割・デモページの見た目の作り込みはすべて範囲外。デモ UI は最小限のスライダーと遅延表示のみ
- B. デモ UI も作り込む
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — 「セットアップが楽」「コスト意識」のユーザー指示に整合。(2026-08-27T16:42:00Z, **Mode:** chat)

## Q3. 実装順序の方針は？

依存関係はコア→検証→デモの一直線で、コアの品質リスクを先に潰す順がそのまま価値順になります。

- A. 依存順 = リスク先行(コア → オフライン検証 → Web デモ)
- B. デモ先行(見た目から)
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A (2026-08-27T16:42:00Z, **Mode:** chat — 仕様書の段階的方針と検証重視の姿勢より)

## Consolidated Summary Confirmation

回答の要約:

- must-have: DSP コア + オフライン数値検証 + WASM Web デモの 3 点
- 範囲外: JUCE アプリ、Android ネイティブ、帯域分割、UI の作り込み
- 順序: コア → 検証 → デモ(依存順 = リスク先行)
- 期限: なし(個人プロジェクト)

- Looks correct
- Request changes

[Answer]: Looks correct
