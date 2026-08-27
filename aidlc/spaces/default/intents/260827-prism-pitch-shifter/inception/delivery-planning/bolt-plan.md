# Bolt 計画 — prism

Bolt = 1 回のビルドパスで動くものが 1 つ出来る作業単位。本計画は 3 本を直列に実行する。[Q1] [Q2]

ブランチ運用(practices 準拠): 各 Bolt は main 起点の短命ワークツリーで作業し、完了時に squash して main へ 1 コミット。

| Bolt | Unit | Definition of Done | 確信仮説(出荷で検証されること) | オーナー |
|---|---|---|---|---|
| B1 | u1-dsp-core | `prism/PitchShifter.h` が契約 1 どおりに実装され、clang++ -std=c++17 -Wall -Wextra -Werror で警告ゼロコンパイル。スモーク(正弦波 1 本を通して出力が有限値)通過 | ディレイライン型コアが契約 API のまま実装可能である | aidlc-developer-agent |
| B2 | u2-verification | 検証ランナーが 44.1k/48k × 4 検証(ピッチ 0.95±0.5% @110/440/3520Hz・遅延 ≤10ms・グリッチゼロ・CPU 比報告)をすべて緑で通過。1 コマンド実行 | コアが数値要件(特にピッチ精度と遅延)を実際に満たす | aidlc-developer-agent |
| B3 | u3-web-demo | Emscripten ビルドが通り、静的ページが Mac ブラウザで起動(マイク→シフト→出力)。Pixel Chrome での確認手順を README に記載 | 同一 C++ ソースがブラウザで実時間動作し、デモとして成立する | aidlc-developer-agent |

- B1 が最初(依存ゼロ・最大リスク源)。B2 で数値リスクが判明(不合格時は D-02 の補間切替)。B3 は B2 の全緑を開始条件とする。[Q1] [Q3]
- ウォーキングスケルトン(最小の一気通貫スライスを先に作る手法)は用いない — practices で省略を確認済み(プロジェクト全体が最小規模)。[Q1]
- 採点モデル(WSJF 等)は不要 — 3 本の順序は依存と品質判断で一意。[Q2]

## Assumptions & Open Questions

None.
