# ストーリーマップ — prism

| Story ID | ストーリー | Unit ID | Directory |
|---|---|---|---|
| US1.1 | デモを開始して補正音声を聞く | U3 | u3-web-demo |
| US1.2 | 聴きながらパラメータを合わせ込む | U3 | u3-web-demo |
| US1.3 | マイク拒否から復帰する | U3 | u3-web-demo |
| US1.4 | 遅延を確認する | U3 | u3-web-demo |
| US2.1 | DSP コアを組み込む(技術) | U1 | u1-dsp-core |
| US2.2 | 数値検証を自動で回す(技術) | U2 | u2-verification |

- 横断ストーリー: なし(US1.2 と US1.4 は U1 の API(セッター・getLatencySamples)を前提とするが、実装責務は U3 の範囲。API 自体は US2.1 で U1 に実装される)。
- Unit 内実装順: U3 は US1.1(起動経路)→ US1.2(パラメータ)→ US1.3(エラー)→ US1.4(遅延表示)。
- カバレッジ: 全 6 ストーリー割当済み、全 3 Unit にストーリーあり。

## Assumptions & Open Questions

None.
