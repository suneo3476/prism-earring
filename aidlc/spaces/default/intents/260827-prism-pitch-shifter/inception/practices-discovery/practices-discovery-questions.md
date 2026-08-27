# Practices Discovery — 質問票(インタビュー)

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. ブランチ運用はどうしますか？

- A. トランクベース(main 一本)。Construction の作業単位は短命ブランチ/ワークツリーで行い、完了時に squash して main へ 1 コミットで載せる(org 既定)
- B. main に直接コミット(ブランチなし)
- C. Git-flow 等の長命ブランチ運用
- X. Other (please specify)

[Answer]: A (2026-08-27T16:50:31Z, **Mode:** chat — org 既定を個人開発でもそのまま採用。監査証跡と作業単位が対応して残る)

## Q2. 薄い一気通貫スライスを最初に作りますか？(ウォーキングスケルトン = 全経路を最小構成で先に通し、部品が繋がることを実証してから本実装に入る手法)

- A. いいえ — Unit が 3 つの一直線(コア→検証→デモ)で全体が既に薄い。儀式は省略し U1 から通常どおり作る
- B. はい — 最小スライスを先に作る
- X. Other (please specify)

[Answer]: A (2026-08-27T16:50:31Z, **Mode:** chat — プロジェクト全体が実質ウォーキングスケルトン規模のため)

## Q3. テストの方法論と順序は？

- A. test-after(custom 床): 各 Unit を実装後、オフライン数値検証(ピッチ 0.95±0.5% @110/440/3520Hz・処理部遅延 ≤10ms・グリッチゼロ・CPU 余裕)を書いて回す。org 既定の 80% 行カバレッジ床は廃し「4 検証すべて緑がマージ条件」に置換。テストは外部依存ゼロ(clang++ のみ)
- B. TDD(テスト先行)
- C. org 既定どおり(test-after + 80% カバレッジ床 + CI)
- X. Other (please specify)

[Answer]: A — 検証仕様が数値で先に確定しており、行カバレッジより信号検証の網羅が品質を決める(品質視点の指摘どおり)。(2026-08-27T16:50:31Z, **Mode:** chat)

## Q4. デプロイはどうしますか？

- A. なし(ローカル) — Web デモは静的ファイルをローカルで開く/ローカル HTTP サーバで配信。Pixel からは同一 LAN の開発機にアクセス。ホスティング公開は今回やらない
- B. GitHub Pages に公開
- C. org 既定(マージでステージング自動デプロイ)
- X. Other (please specify)

[Answer]: A — 公開はデータ・規制面の検討(将来配布時の医療機器規制 D-03)とセットで別途判断。(2026-08-27T16:50:31Z, **Mode:** chat)

## Q5. コードスタイルは？

- A. 命名は CLAUDE.md 規約(名前空間 prism、prism::PitchShifter、表記は prism / PrismEarring)。C++17、LLVM 風 4 スペース、フォーマッタ設定ファイルは置かない。コンパイラ警告 -Wall -Wextra をエラー扱い。リアルタイム経路の禁止事項(ヒープ確保・ロック・I/O・例外・システムコール)をコード規約として明文化
- B. clang-format 設定を導入して CI 強制
- C. 特に決めない
- X. Other (please specify)

[Answer]: A (2026-08-27T16:50:31Z, **Mode:** chat — 開発視点・DevSecOps 視点の提案どおり)

## Consolidated Summary Confirmation

回答の要約:

- ブランチ: トランクベース + 短命ワークツリー、squash で main へ
- スケルトン儀式: 省略(全体が最小規模)
- テスト: test-after。カバレッジ床の代わりに「4 数値検証すべて緑」をマージ条件に。依存ゼロで clang++ のみ
- デプロイ: なし(ローカル配信のみ。公開は将来判断)
- スタイル: CLAUDE.md 命名規約 + C++17 + -Wall -Wextra エラー扱い + リアルタイム禁止事項の明文化

- Looks correct
- Request changes

[Answer]: Looks correct
