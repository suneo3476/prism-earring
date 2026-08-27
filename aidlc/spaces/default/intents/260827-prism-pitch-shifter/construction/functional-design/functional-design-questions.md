# Functional Design — 質問票(ステージ横断、Unit 別ラベル付き)

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. [u1-dsp-core] グリッチ判定と平滑化の境界条件: crossfade_ms 変更時の窓長再構成はどう扱いますか？

窓長(リングバッファ読み出し窓)が動作中に変わると、ポインタ位相の不連続が出得ます。

- A. crossfade_ms も他パラメータ同様 20ms 平滑を通し、窓長は「次のクロスフェード境界」でのみ更新する(フェーズ跨ぎの不連続ゼロ)
- B. 即時再構成(短いグリッチ許容)
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — グリッチゼロ要件(NFR-4)を守る唯一の安全な更新点。(2026-08-27T21:15:00Z, **Mode:** chat)

## Q2. [u2-verification] 検定の合否判定はどの粒度で出しますか？

- A. ケースごとに PASS/FAIL 行を標準出力し、1 つでも FAIL があれば終了コード 1。CPU 比は情報表示(閾値なし、報告のみ — FR-3.4 は計測・報告が要件)
- B. CPU 比にも合格閾値を設ける
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — CPU 閾値は実行環境依存が大きく、要件も「計測」まで。(2026-08-27T21:15:00Z, **Mode:** chat)

## Q3. [u3-web-demo] WASM ロードの配布形態は？

- A. emcc の -sSINGLE_FILE=1 で WASM を JS に内包し、配布物を index.html + worklet.js(+ main.js)の静的ファイル数枚に抑える(file:// でも同一オリジン問題を回避しやすい)
- B. .wasm を別ファイルで fetch
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — 「セットアップが楽」を最優先(確認済み方針)。サイズ増は許容(コアは小さい)。(2026-08-27T21:15:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- [u1] 窓長変更はクロスフェード境界でのみ適用(グリッチゼロ維持)。他パラメータは 20ms 平滑
- [u2] ケース別 PASS/FAIL + 終了コード。CPU 比は報告のみ
- [u3] WASM は SINGLE_FILE で JS に内包、静的ファイル数枚で配布

- Looks correct
- Request changes

[Answer]: Looks correct
