<!-- INVARIANT: examples are single-line HTML comments so a fresh template parses to total=0 (MEMORY_EMPTY). Do NOT un-comment or split across lines. t100 guards this. -->
> This file is kept up to date automatically while the stage runs. Add observations at the review step, not by editing here directly.

## Interpretations
<!-- example -->
- 2026-08-27T21:45:00Z — 検証 Unit の NFR は再現性(決定論)と依存ゼロが中核。NFR-3/4 の検証実装がこの Unit の存在理由なので traceability に含めた
- 2026-09-04T00:00:00Z — レビュー R-01 対応。NFR-2 を「検証ハーネス観点」で再解釈した: ランナーが process() を呼ぶ計測区間を実機コールバックと同一契約とみなし、SR-4 として要件化。検出手段はグローバル operator new 差し替えの確保カウンタ(計測区間の境界で差分 0 を assert)

## Deviations
<!-- example -->
- 2026-09-04T00:00:00Z — R-02 対応で SR-3.2 の終了コードを 0/1/2 から 0/1 に後退させた。上流(WF-6 / BR2.5)が承認済みで編集不可のため、下流である本 SR を上流に合わせる方向で不整合を解消した(逆方向の上流改訂は行わない)

## Tradeoffs
<!-- example -->
- 2026-09-04T00:00:00Z — R-03 対応で bit-exact 再現性は要求せず、周波数検出の許容差(±0.01 bin = ±0.015 Hz)で規定した。bit-exact はコンパイラ/最適化レベル間で現実的に担保できず、一方で BR2.1 の判定余裕は 110 Hz でも ±0.52 Hz あり許容差の約 34 倍。判定を反転させない精度を担保しつつ移植性(NFR-5)を犠牲にしない選択
- 2026-09-04T00:00:00Z — R-04 対応でファイル書き出しの「将来オプション」ヘッジを削除し断定形にした。将来の柔軟性より隔離要件の明確さを取り、変更にはセキュリティレビューを必須とする条件を付けた
- 2026-09-04T00:00:00Z — R-05 対応で traceability.json の "reverse" エントリを廃止し通常の coverage に変換した。"reverse" の意味(trace-back か scope 外か)が曖昧だったため、上流 ID を主キーとする u1 と同形式に揃えて曖昧性を排除。代償として FR-2.1 / FR-3.4 を upstream_ids に追加し、NFR 中心だった宣言集合が少し広がった

## Open questions
<!-- example -->
