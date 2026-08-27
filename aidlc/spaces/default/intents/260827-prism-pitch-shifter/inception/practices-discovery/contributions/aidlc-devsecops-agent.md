**Collaborator:** aidlc-devsecops-agent

## Contribution

セキュリティ・サプライチェーン視点の評価:

- 依存は Emscripten(ビルド時のみ)とブラウザ標準 API のみ。実行時のサードパーティ依存ゼロを維持すべき(デモページに外部 CDN・解析スクリプトを入れない)。
- 音声データはデバイス外に出さない(ネットワーク送信なし)。この不変条件を discovered-rules に明記することを提案。
- シークレットは存在しない構成。SAST/DAST・シークレットスキャンの導入は本件の規模では不要。
- WASM ビルドの再現性: emsdk のバージョンを README に固定表記しておく(ロックファイルに相当する軽量措置)。
- lint/format の CI 強制は行わない(個人開発・単一言語・小規模のため)。コンパイラ警告(-Wall -Wextra)をエラー扱いにする方が実効的。

## Positions

None.
