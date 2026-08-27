# 外部依存マップ — prism

外部チーム・外部 API・承認プロセスへの依存はなし。[Q3]

| 依存 | 種別 | オーナー | ブロックする Bolt | リードタイム | 失速時の代替 |
|---|---|---|---|---|---|
| Emscripten (emsdk) | ビルドツールチェーン | 開発者自身(ローカル導入) | B3 | 数分〜(ダウンロード) | AudioWorklet 内 JS 実装に切替(アルゴリズム同一、C++ 単一ソースは一時放棄) |
| ブラウザ標準 API(getUserMedia / AudioWorklet / WASM) | 実行環境 | ブラウザベンダ | B3 | なし(既存) | 対応ブラウザ案内(FR-4.4) |

## Assumptions & Open Questions

None.
