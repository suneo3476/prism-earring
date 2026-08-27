# 実現可能性評価 — prism (Prism Earring)

## 技術的実現性

**評価: 実現可能(高確度)** [Q1] [Q3]

- コアアルゴリズム(ディレイライン型ピッチシフタ、2 ポインタ + クロスフェード)は確立された古典手法であり、シフト量が -89 セント(読み出し速度 0.95)と 1.0 に近いためアーティファクトは原理的に小さい。[desc]
- DSP コアは依存ゼロの純 C++17 で書けるため、macOS の clang++ だけでオフライン検証まで完結する。[Q1]
- 同一の C++ ソースを Emscripten で WASM 化し AudioWorklet で駆動する構成は、静的ページ 1 枚で Pixel Chrome / Mac ブラウザの両方に配れる。[Q1]
- 処理部の理論遅延はクロスフェード窓長に支配され数 ms に収まる見込み。オフラインのインパルス試験で数値検証する。[Q3]

## リスク分析(要約 — 詳細は raid-log.md)

| リスク | 影響 | 緩和策 | Source |
|---|---|---|---|
| ブラウザ往復遅延 20〜60ms | デモでの聴感が最終形態より劣化(唸り・エコー感) | デモは効果検証と割り切り、遅延を測定・表示。低遅延はネイティブで実現 | [Q3] |
| ブラウザの音声前処理(エコーキャンセル等)が音を加工 | ピッチ補正の評価が困難になる | getUserMedia で echoCancellation / noiseSuppression / autoGainControl を無効化 | [Q3] |
| Emscripten 導入失敗・ビルド不調 | Web デモが作れない | フォールバックとして AudioWorklet 内 JS 実装(同一アルゴリズム)を用意可能 | [Q1] |
| iOS Safari の AudioWorklet 制約 | 対象外デバイスでの動作不安定 | 対象は Pixel Chrome と Mac ブラウザのみ(ユーザー確認済み) | [Q1] |

## 判定

段階的アプローチ(DSP コア → オフライン検証 → Web デモ)により、致命的なブロッカーなしに完走可能と判断する。[Q1] [Q3]

## Assumptions & Open Questions

- Emscripten(emsdk)のダウンロード・インストールがこの開発機のネットワーク環境で完了できること。[assumption]
- Pixel Chrome の AudioWorklet + WASM 実行が実用速度で動くこと(近年の Pixel であればまず問題ないが実測未了)。[assumption]
