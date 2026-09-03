<!-- INVARIANT: examples are single-line HTML comments so a fresh template parses to total=0 (MEMORY_EMPTY). Do NOT un-comment or split across lines. t100 guards this. -->
> This file is kept up to date automatically while the stage runs. Add observations at the review step, not by editing here directly.

## Interpretations
<!-- example: 2026-05-29T10:14:32Z — chose REST over GraphQL; the consuming team only needs CRUD, revisit if subscriptions land -->
- 2026-09-03T15:30:19Z — library kind の NFR 設計では service 向けパターン(キャッシュ階層・サーキットブレーカ・分散トレース)が原理的に適用できないため、ステージの意図を「不正状態・未定義動作を構造的に作れないようにする手段の設計」と読み替えた(SD-2 の INV-1〜INV-4、SD-6 の公開面最小化)。
- 2026-09-03T15:31:00Z — SR-4.1 の「Bolt B2 の CPU 計測で確認」を設計解として具体化した。per-sample の超越関数 4 回・予算 0.15〜0.3%・合格基準「コールバック実行時間/バッファ時間 < 10%」を u2 へ引き渡す形にした。

## Deviations
<!-- example: 2026-05-29T10:14:32Z — skipped the optional caching layer the stage prose suggested; the dataset is small enough that it adds risk -->
- 2026-09-03T15:31:40Z — ステージ定義は 7 成果物を列挙するが produces_kinds により library kind では security-design / logical-components / traceability の 3 点のみ生成した(performance/scalability/reliability/observability-design は対象外)。
- 2026-09-03T15:32:09Z — サマリ確認は DECISION_RECORDED までで止まり SUMMARY_CONFIRMATION_RECORDED が未記録。human-presence guard が正しく作動した(最終 HUMAN_TURN 15:00:06Z < 質問 15:31:52Z)ため、AIDLC_SKIP_HUMAN_PRESENCE_GUARD での迂回は行わなかった — 台帳に虚偽の人間確認を書くことになるため。人間の応答待ちとして残す。

## Tradeoffs
<!-- example: 2026-05-29T10:14:32Z — picked TDD over BDD this run; the team is unit-first and the domain is well-understood -->
- 2026-09-03T15:31:10Z — デノーマル対策で FTZ レジスタ操作と -ffast-math を却下し純 C++ 手段(平滑器状態への 1e-20 加算 + 到達スナップ)を選んだ。移植性(WASM に FTZ 制御なし)と数値検証の再現性(NFR-3)を CPU の微小コストより優先した。
- 2026-09-03T15:31:20Z — 単一ヘッダ構成(Q4-A)を選び、内部コンポーネントの直接単体テスト可能性を捨てて契約面の狭さを買った。u2 の 4 検証がすべて入出力ベースで足りることが受容の根拠。

## Open questions
<!-- example: 2026-05-29T10:14:32Z — confirm the retention window with compliance before the next stage hardens the schema -->
- 2026-09-03T15:31:50Z — prepare/reset を process と並行に呼べないという前提が契約 1 に未記載。contract-design への追記が必要かを次ステージで確認する(security-design A-3、logical-components A-2)。
- 2026-09-03T15:32:00Z — u3 の emcc ビルドが -fno-exceptions を使う場合、prepare の bad_alloc catch が不活性になる。確保量 ~170KB から実害なしと判断したが、u3 のビルド設定確定時に再確認する。
