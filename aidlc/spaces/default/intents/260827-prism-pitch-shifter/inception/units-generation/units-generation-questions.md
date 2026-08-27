# Units Generation — 質問票

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. Unit 境界と粒度はこの構成でよいですか？

- A. 3 Unit: u1-dsp-core(PitchShifter、kind=library)/ u2-verification(VerificationHarness、kind=library)/ u3-web-demo(WasmBridge + DemoWorklet + DemoUI、kind=ui)。バックログのプロト Unit をそのまま昇格
- B. 5 コンポーネント = 5 Unit の細粒度
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — Web 側 3 コンポーネントは 1 つの成果物(静的ページ)として不可分に届くため 1 Unit が適切。(2026-08-27T17:30:00Z, **Mode:** chat)

## Q2. Unit 間の統合点は？

- A. u1→u2: C++ ヘッダ `prism/PitchShifter.h` の直接 include。u1→u3: 同ヘッダを WasmBridge が include し、JS へは extern "C" API(contract-design で確定)。共有データ・イベントなし
- B. その他の統合機構
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A (2026-08-27T17:30:00Z, **Mode:** chat — domain-design の依存グラフどおり)

## Q3. 配備モデルは？

- A. u1/u2 は配備物なし(ソース + テストバイナリ)。u3 は静的ファイル一式(埋め込み型、ローカル配信)
- B. ホスティング配備
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — practices の Deployment(なし・ローカル)どおり。(2026-08-27T17:30:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- 3 Unit: u1-dsp-core(library)→ u2-verification(library)、u1 → u3-web-demo(ui)
- 統合点: C++ ヘッダ直接 include(u1→u2, u1→u3)、JS へは extern "C" API
- 配備: u1/u2 なし、u3 は静的ファイル(ローカル)

- Looks correct
- Request changes

[Answer]: Looks correct
