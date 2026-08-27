# Feasibility — 質問票

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. 開発・実行環境の制約は何ですか？(select all that apply)

- A. 開発機は macOS(Darwin 24.6 / Apple Silicon 想定)。C++17 以上、clang++ が利用可能であること
- B. デモ実行環境は Google Pixel の Chrome と MacBook Air のブラウザ(Safari/Chrome)
- C. WASM ビルドに Emscripten ツールチェーンを新規導入する(ネットワークからの取得が必要)
- D. 該当なし (Not applicable)
- X. Other (please specify)

[Answer]: A, B, C (2026-08-27T16:40:22Z, **Mode:** chat — 前ステージ確認済みの製品境界と実行環境の観測から)

## Q2. 規制・コンプライアンス要件はありますか？

- A. なし — 個人用途、外部へのデータ送信なし、個人データの保存なし。マイク入力はブラウザの許可ダイアログで本人が明示的に許可する
- B. 医療機器規制の確認が必要(配布する場合)
- C. 該当なし (Not applicable)
- X. Other (please specify)

[Answer]: A — 配布しない自家用のため医療機器規制は対象外。将来配布する場合は別途検討事項として RAID ログに記録する。(2026-08-27T16:40:22Z, **Mode:** chat)

## Q3. 最大の技術リスクをどう扱いますか？

ブラウザ(特に Android Chrome)のマイク→出力往復遅延は 20〜60ms 程度になり得ます。またブラウザ既定のエコーキャンセル/ノイズ抑制が音を加工する可能性があります。

- A. デモは「効果検証」と割り切る: DSP 処理部の遅延(数 ms)はオフラインテストで数値担保し、ブラウザ往復遅延は測定・表示するに留める。getUserMedia では echoCancellation 等を無効化する
- B. ブラウザでも 20ms 往復を必達にする(リスク大)
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A (2026-08-27T16:40:22Z, **Mode:** chat — Q8[intent-capture] のユーザー確認「デモとしては許容」に整合)

## Consolidated Summary Confirmation

回答の要約:

- 環境: macOS 開発機 + clang++、デモは Pixel Chrome / Mac ブラウザ、Emscripten を新規導入
- 規制: 対象外(自家用・データ送信なし)。将来配布時の医療機器規制は RAID ログに記録
- 技術リスク: ブラウザ往復遅延はデモでは測定・表示に留め、処理部遅延はオフラインテストで数値担保。echoCancellation 等は無効化

- Looks correct
- Request changes

[Answer]: Looks correct
