<!-- INVARIANT: examples are single-line HTML comments so a fresh template parses to total=0 (MEMORY_EMPTY). Do NOT un-comment or split across lines. t100 guards this. -->
> This file is kept up to date automatically while the stage runs. Add observations at the review step, not by editing here directly.

## Interpretations
<!-- example: 2026-05-29T10:14:32Z — chose REST over GraphQL; the consuming team only needs CRUD, revisit if subscriptions land -->


- 2026-08-27T21:40:00Z — ライブラリ Unit のセキュリティ要件は「攻撃面を作らない」方向で定義(I/O なし・UB なし・境界安全)。認証等の Web 系 NFR は該当なし
<!-- aidlc-wave-memory:u1-dsp-core:50ba019e41633f609562376429765732f012a149ecbd0f4015441c78cf1dfaf0 -->

- 2026-08-27T21:45:00Z — デモ Unit の NFR はプライバシー(外部送信ゼロ)と可搬性(静的ファイル)が中核。emsdk バージョンは README に固定表記
<!-- aidlc-wave-memory:u3-web-demo:f63a7bf878c607b17d4fbd1c5d5167462e0c97edb5b0d3852d789f35b3afdf32 -->

- 2026-08-27T21:45:00Z — 検証 Unit の NFR は再現性(決定論)と依存ゼロが中核。NFR-3/4 の検証実装がこの Unit の存在理由なので traceability に含めた
<!-- aidlc-wave-memory:u2-verification:50f3174352bb5a2ccec5b640f2709a65a15b807f6e3ab5af82dcfc8aab2e2d61 -->

- 2026-09-04T00:00:00Z — レビュー R-01 対応。NFR-2 を「検証ハーネス観点」で再解釈した: ランナーが process() を呼ぶ計測区間を実機コールバックと同一契約とみなし、SR-4 として要件化。検出手段はグローバル operator new 差し替えの確保カウンタ(計測区間の境界で差分 0 を assert)
<!-- aidlc-wave-memory:u2-verification:4f7b7008a3e8c018e2c79ff4a29620fbc906b9b6bfa729ee69dbd0cb556dfb07 -->
## Deviations
<!-- example: 2026-05-29T10:14:32Z — skipped the optional caching layer the stage prose suggested; the dataset is small enough that it adds risk -->


- 2026-09-04T00:00:00Z — R-02 対応で SR-3.2 の終了コードを 0/1/2 から 0/1 に後退させた。上流(WF-6 / BR2.5)が承認済みで編集不可のため、下流である本 SR を上流に合わせる方向で不整合を解消した(逆方向の上流改訂は行わない)
<!-- aidlc-wave-memory:u2-verification:522dde1eb57265debde6d5f5eded5596c0ad221069659ba6d2ec1e46eea5393b -->
## Tradeoffs
<!-- example: 2026-05-29T10:14:32Z — picked TDD over BDD this run; the team is unit-first and the domain is well-understood -->


- 2026-09-04T00:00:00Z — R-03 対応で bit-exact 再現性は要求せず、周波数検出の許容差(±0.01 bin = ±0.015 Hz)で規定した。bit-exact はコンパイラ/最適化レベル間で現実的に担保できず、一方で BR2.1 の判定余裕は 110 Hz でも ±0.52 Hz あり許容差の約 34 倍。判定を反転させない精度を担保しつつ移植性(NFR-5)を犠牲にしない選択
<!-- aidlc-wave-memory:u2-verification:62e8120787b9b20b69de4817b0f548ade0037b2414a281019988936f3ce4a429 -->

- 2026-09-04T00:00:00Z — R-04 対応でファイル書き出しの「将来オプション」ヘッジを削除し断定形にした。将来の柔軟性より隔離要件の明確さを取り、変更にはセキュリティレビューを必須とする条件を付けた
<!-- aidlc-wave-memory:u2-verification:a563df099f4f444e017eabafe3edb28b4b330ac5ca31fe1b3f892111c102576f -->

- 2026-09-04T00:00:00Z — R-05 対応で traceability.json の "reverse" エントリを廃止し通常の coverage に変換した。"reverse" の意味(trace-back か scope 外か)が曖昧だったため、上流 ID を主キーとする u1 と同形式に揃えて曖昧性を排除。代償として FR-2.1 / FR-3.4 を upstream_ids に追加し、NFR 中心だった宣言集合が少し広がった
<!-- aidlc-wave-memory:u2-verification:fcf99ced5ca8fded437075e61fcc6cbd982908022131aa0c52bb3f48429a59eb -->
## Open questions
<!-- example: 2026-05-29T10:14:32Z — confirm the retention window with compliance before the next stage hardens the schema -->
