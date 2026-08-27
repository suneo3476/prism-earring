# Domain Design — 質問票

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. コンポーネント分割はこの 5 つでよいですか？

- A. PitchShifter(C++ DSP コア)/ VerificationHarness(C++ オフライン検証)/ WasmBridge(Emscripten 接着)/ DemoWorklet(AudioWorkletProcessor JS)/ DemoUI(メインスレッド JS + HTML)の 5 コンポーネント
- B. DSP と検証を 1 コンポーネントに統合
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — レイヤ分離方針(コア/検証/接着層、practices)と Unit 境界(U1/U2/U3)に対応する自然な分割。(2026-08-27T17:20:00Z, **Mode:** chat)

## Q2. パラメータの受け渡し機構は？

- A. UI → Worklet: `port.postMessage`(レンダ量子間で配送)。Worklet → コア: `PitchShifter::setShiftCents 等のセッター(std::atomic<float> に格納)`。コア内: 処理ブロック頭で 1 回 load し per-sample 平滑化(時定数 20ms)
- B. AudioParam(k-rate)を 4 本定義
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — コアの API をプラットフォーム非依存に保てる(AudioParam は Web 専用概念)。std::atomic はネイティブ移植時にそのまま正しく、Emscripten 単一スレッドでもコストなし。refined-mockups レビュー R-01 の確定。(2026-08-27T17:20:00Z, **Mode:** chat)

## Q3. Worklet → UI への遅延報告形式は？

- A. `{type:"latency", dspLatencyMs:number}` を開始時と以後 1 秒ごとに port.postMessage。dspLatencyMs はコアの `getLatencySamples()/fs*1000`(基準オフセット + 平均ポインタ遅れの設計値)。音声スレッド負荷はカウンタ 1 個
- B. UI からポーリング要求
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — refined-mockups レビュー R-02 の確定。(2026-08-27T17:20:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- コンポーネント: PitchShifter / VerificationHarness / WasmBridge / DemoWorklet / DemoUI の 5 つ
- パラメータ: postMessage → セッター(std::atomic)→ ブロック頭 1 回 load → per-sample 平滑化
- 遅延報告: {type:"latency", dspLatencyMs} を 1 秒ごとに postMessage、値はコアの設計値 API から取得

- Looks correct
- Request changes

[Answer]: Looks correct
