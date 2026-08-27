<!-- INVARIANT: examples are single-line HTML comments so a fresh template parses to total=0 (MEMORY_EMPTY). Do NOT un-comment or split across lines. t100 guards this. -->
> This file is kept up to date automatically while the stage runs. Add observations at the review step, not by editing here directly.

## Interpretations
<!-- example: 2026-05-29T10:14:32Z — chose REST over GraphQL; the consuming team only needs CRUD, revisit if subscriptions land -->
- 2026-08-27T16:50:31Z — CLAUDE.md「リアルタイムオーディオの鉄則」をチーム慣行の Forbidden として昇格するのが本ステージの実質的な狙い。カバレッジ床(org 既定 80%)は DSP 検証テストの性質に合わないため custom 方針で置換

## Deviations
<!-- example: 2026-05-29T10:14:32Z — skipped the optional caching layer the stage prose suggested; the dataset is small enough that it adds risk -->
- 2026-08-27T16:50:31Z — コスト配慮(ユーザー指示)のため、リード草稿と支援 3 視点の起草をオーケストレータが各視点に成り代わって直接行った(ブラインド性は視点の独立記述で維持)。識別マーカーと成果物構成はプロトコルどおり

## Tradeoffs
<!-- example: 2026-05-29T10:14:32Z — picked TDD over BDD this run; the team is unit-first and the domain is well-understood -->
- 2026-08-27T16:50:31Z — test-after を採用(TDD ではなく)。検証仕様が先に数値で確定しており、実装→数値検証の順が自然なため

## Open questions
<!-- example: 2026-05-29T10:14:32Z — confirm the retention window with compliance before the next stage hardens the schema -->
