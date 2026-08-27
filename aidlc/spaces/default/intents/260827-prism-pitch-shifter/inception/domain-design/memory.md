<!-- INVARIANT: examples are single-line HTML comments so a fresh template parses to total=0 (MEMORY_EMPTY). Do NOT un-comment or split across lines. t100 guards this. -->
> This file is kept up to date automatically while the stage runs. Add observations at the review step, not by editing here directly.

## Interpretations
<!-- example: 2026-05-29T10:14:32Z — chose REST over GraphQL; the consuming team only needs CRUD, revisit if subscriptions land -->
- 2026-08-27T17:20:00Z — refined-mockups レビューの R-01(同期機構)と R-02(遅延報告形式)をここで設計確定した。AudioWorklet 内は単一スレッドだが、コアはネイティブ移植を見据えて std::atomic を採用(Emscripten でもコストほぼゼロ)

## Deviations
<!-- example: 2026-05-29T10:14:32Z — skipped the optional caching layer the stage prose suggested; the dataset is small enough that it adds risk -->

## Tradeoffs
<!-- example: 2026-05-29T10:14:32Z — picked TDD over BDD this run; the team is unit-first and the domain is well-understood -->
- 2026-08-27T17:20:00Z — 補間は 3 次(4 点 Hermite)ではなく線形を採用。シフト量が小さく読み出し速度が 1 に近いため線形で十分、かつ分岐・演算が少ない。検証でピッチ精度が出なければ Hermite に切替可能な構造にする

## Open questions
<!-- example: 2026-05-29T10:14:32Z — confirm the retention window with compliance before the next stage hardens the schema -->
