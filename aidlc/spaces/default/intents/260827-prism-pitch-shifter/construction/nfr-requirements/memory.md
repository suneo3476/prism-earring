<!-- INVARIANT: examples are single-line HTML comments so a fresh template parses to total=0 (MEMORY_EMPTY). Do NOT un-comment or split across lines. t100 guards this. -->
> This file is kept up to date automatically while the stage runs. Add observations at the review step, not by editing here directly.

## Interpretations
<!-- example: 2026-05-29T10:14:32Z — chose REST over GraphQL; the consuming team only needs CRUD, revisit if subscriptions land -->


- 2026-08-27T21:40:00Z — ライブラリ Unit のセキュリティ要件は「攻撃面を作らない」方向で定義(I/O なし・UB なし・境界安全)。認証等の Web 系 NFR は該当なし
<!-- aidlc-wave-memory:u1-dsp-core:50ba019e41633f609562376429765732f012a149ecbd0f4015441c78cf1dfaf0 -->

- 2026-08-27T21:45:00Z — デモ Unit の NFR はプライバシー(外部送信ゼロ)と可搬性(静的ファイル)が中核。emsdk バージョンは README に固定表記
<!-- aidlc-wave-memory:u3-web-demo:f63a7bf878c607b17d4fbd1c5d5167462e0c97edb5b0d3852d789f35b3afdf32 -->
## Deviations
<!-- example: 2026-05-29T10:14:32Z — skipped the optional caching layer the stage prose suggested; the dataset is small enough that it adds risk -->

## Tradeoffs
<!-- example: 2026-05-29T10:14:32Z — picked TDD over BDD this run; the team is unit-first and the domain is well-understood -->

## Open questions
<!-- example: 2026-05-29T10:14:32Z — confirm the retention window with compliance before the next stage hardens the schema -->
