# 根拠 — Practices Discovery

グリーンフィールドのため、根拠は以下の 3 系統。

| 根拠 | 内容 | 反映先 |
|---|---|---|
| `aidlc/spaces/default/memory/org.md` | トランクベース + squash、test-after 既定、スコープ別カバレッジ床 | Way of Working(採用)、Testing Posture(床は custom に置換) |
| `CLAUDE.md`(ユーザー作成の仕様書) | リアルタイムの鉄則、命名規約、検証方法、パラメータ仕様 | Code Style、Mandated/Forbidden、Testing Posture の検証 4 項目 |
| インタビュー(Q1〜Q5、本ステージ質問票) | 5 領域すべての確認回答 | 全セクション |

支援視点の独立評価(contributions/):

- aidlc-quality-agent: カバレッジ床の不適合指摘 → custom 床に反映
- aidlc-developer-agent: レイヤ分離・エラー処理方針 → Code Style に反映
- aidlc-devsecops-agent: 音声データ不送信の不変条件・-Wall -Wextra 強制 → Mandated / Code Style に反映

## Assumptions & Open Questions

None.
