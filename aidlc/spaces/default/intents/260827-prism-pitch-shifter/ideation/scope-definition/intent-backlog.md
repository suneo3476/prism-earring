# インテントバックログ — prism (Prism Earring)

MoSCoW 優先度付きのプロト Unit。依存は一直線(U1 → U2 → U3)。

| ID | プロト Unit | 優先度 | 依存 | 受け入れ条件(要約) | Source |
|---|---|---|---|---|---|
| U1 | dsp-core: `prism::PitchShifter` | Must | なし | ステレオ処理・実行時可変パラメータ・コールバック内割当ゼロ。単体でコンパイル可能 | [Q1] |
| U2 | verification: オフライン数値検証 | Must | U1 | 4 検証(ピッチ 0.95±0.5% @110/440/3520Hz・遅延 ≤10ms・グリッチなし・CPU 余裕)がすべて自動で緑 | [Q1] |
| U3 | web-demo: WASM/AudioWorklet デモ | Must | U1(U2 の緑を推奨) | Pixel Chrome / Mac ブラウザで静的ページから起動し、マイク音がシフトされて聞こえる。スライダーで実行時調整可 | [Q1] [Q2] |

Won't(今回): JUCE アプリ / Android ネイティブ / 帯域分割 / UI 作り込み [Q2]

## Assumptions & Open Questions

None.
