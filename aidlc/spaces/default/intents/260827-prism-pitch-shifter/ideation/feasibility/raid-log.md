# RAID ログ — prism (Prism Earring)

## Risks(リスク)

| ID | リスク | 確度 | 影響 | 緩和策 | Source |
|---|---|---|---|---|---|
| R-01 | ブラウザ往復遅延がデモの聴感を損なう(20〜60ms) | 高 | 中(デモ品質) | デモは効果検証と位置づけ、遅延を測定・表示。低遅延はネイティブ段階で実現 | [Q3] |
| R-02 | ブラウザの音声前処理がピッチ評価を歪める | 中 | 中 | getUserMedia の echoCancellation / noiseSuppression / autoGainControl を無効化 | [Q3] |
| R-03 | Emscripten 導入・ビルドの不調 | 低 | 中 | AudioWorklet 内 JS 実装への切替余地を残す(同一アルゴリズム) | [Q1] |
| R-04 | クロスフェード起因の変調感(コムフィルタ的な色付け) | 中 | 低 | シフト量が小さく影響軽微。crossfade_ms を実行時可変にして耳で追い込む | [desc] |

## Assumptions(前提)

| ID | 前提 | 検証方法 | Source |
|---|---|---|---|
| A-01 | 音高偏差は全帯域で一定比率 0.95(110Hz/3520Hz は未測定) | shift_cents を可変にし実使用で確認 | [assumption] |
| A-02 | 左右の偏差は同一(左右差未確認) | shift_cents_L/R を独立パラメータ化して確認 | [assumption] |
| A-03 | Pixel Chrome の AudioWorklet + WASM が実用速度で動く | デモ実機起動で確認 | [assumption] |

## Issues(顕在化した問題)

None.

## Dependencies(依存)

| ID | 依存先 | 内容 | Source |
|---|---|---|---|
| D-01 | Emscripten (emsdk) | C++ → WASM ビルドツールチェーン | [Q1] |
| D-02 | ブラウザ API | getUserMedia / AudioWorklet / WebAssembly(Pixel Chrome・Mac ブラウザの現行版で利用可能) | [Q1] |
| D-03 | (将来・範囲外) 医療機器規制の確認 | 一般配布する場合のみ。今回は自家用のため対象外 | [Q2] |

## Assumptions & Open Questions

- 上記 Assumptions 表(A-01〜A-03)を参照。[assumption]
