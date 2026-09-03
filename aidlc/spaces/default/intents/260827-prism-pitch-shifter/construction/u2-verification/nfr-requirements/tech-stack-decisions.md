# 技術スタック決定 — u2-verification

| 項目 | 決定 | 根拠 |
|---|---|---|
| 言語 | C++17(単一実行ファイル `tests/verify.cpp`) | clang++ のみでビルド(FR-3、team-practices)|
| FFT | 自前 radix-2、サイズ **N=32768**、**ハン窓**、**放物線補間**ピーク。bin 幅 Δf=fs/N = 1.4648 Hz(48 kHz)/ 1.3458 Hz(44.1 kHz) | 依存ゼロ。オフライン限定(BR2.1、FR-3.1)。**SR-2.3 の許容差算出の normative 定義**|
| 時間計測 | `std::chrono::steady_clock` | BR2.4 |
| リアルタイム安全検証 | グローバル `operator new` / `delete` 差し替えによる確保カウンタ(非 atomic `std::size_t`、逐次ランナー前提)。計測区間の境界で差分 0 を `assert` | NFR-2、SR-4.1〜SR-4.3 |
| ビルド | `Makefile`(`make verify` で clang++ -std=c++17 -Wall -Wextra -Werror -O2 && 実行)。**`NDEBUG` は定義しない**(`assert` を有効に保つ) | team-practices、SR-4.2 |
| 再現性検証ビルド | 同一ソースを `-O0` / `-O2` の 2 構成でビルドし f_out を比較 | SR-2.3 判定手段 |
| 出力 | ケース別 `PASS/FAIL` 行 + 情報行(`alloc=0`、cpu は `cpuRatio`)+ サマリ(標準出力)、診断・実行不能理由(標準エラー出力)+ 終了コード **0/1** | 終了コードは WF-6 / BR2.5 が normative、SR-3.2。出力面は Q1、SR-1.1、SR-4.3 |

## Assumptions & Open Questions

None.
