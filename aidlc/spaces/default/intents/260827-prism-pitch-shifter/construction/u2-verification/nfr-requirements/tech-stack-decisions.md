# 技術スタック決定 — u2-verification

| 項目 | 決定 | 根拠 |
|---|---|---|
| 言語 | C++17(単一実行ファイル `tests/verify.cpp`) | clang++ のみでビルド(FR-3、team-practices)|
| FFT | 自前 radix-2(サイズ 32768、ハン窓、放物線補間ピーク) | 依存ゼロ。オフライン限定(BR2.1)|
| 時間計測 | `std::chrono::steady_clock` | BR2.4 |
| ビルド | `Makefile`(`make verify` で clang++ -std=c++17 -Wall -Wextra -Werror -O2 && 実行) | team-practices |
| 出力 | ケース別 `PASS/FAIL` 行 + サマリ + 終了コード(0/1/2) | 機能設計 Q1、SR-3.2 |

## Assumptions & Open Questions

None.
