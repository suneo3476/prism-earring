# セキュリティ・NFR 要件 — u2-verification

検証ランナー Unit。攻撃面はローカル実行のみで、中核 NFR は再現性・依存ゼロ・実機コールバック契約との同一性。

normative な前提(本書はこれらを変更せず、その上に許容差と検出手段を規定する):

- FFT 構成(自前 radix-2 / N=32768 / ハン窓 / 放物線補間ピーク)は `tech-stack-decisions.md` の記載を normative とする。
- 終了コード契約は `functional-design/functional-spec.md` WF-6 と `functional-design/rules.md` BR2.5 を normative とする。
- 検証マトリクス(fs∈{44100,48000} × {pitch, latency, glitch, cpu})は BR2.5 を normative とする。本書はケースを追加しない。

## SR-1 隔離とオフライン性

- SR-1.1: ネットワークアクセスなし。入出力は標準出力(PASS/FAIL 行・サマリ・情報行)、標準エラー出力(診断・実行不能理由)、およびメモリ内信号のみ。**ファイル書き出しは行わない。** 要件変更時はセキュリティレビューを必須とし、本書の改訂を伴う。[desc]
  - 判定: `tests/verify.cpp` にファイル I/O(`<fstream>`, `fopen`, `open`, `mmap`)およびソケット/HTTP API(`socket`, `connect`, `curl`)の出現が 0 件であること(ソース検索で確認。PASS = 0 件、1 件以上で FAIL)。
- SR-1.2: 外部依存ゼロ(C++17 標準ライブラリのみ)。FFT は自前 radix-2 実装(検証側限定 — 音声経路には FFT を置かない、FR-3.1)。[desc]
  - 判定: `#include` が標準ヘッダと `prism/PitchShifter.h` のみで構成され、追加パッケージ 0 件で `make verify` が clang++ 単体成功すること。

## SR-2 再現性(決定論)と数値精度

- SR-2.1: 信号生成(正弦波・インパルス)は決定論。乱数を使用しない。同一入力・同一パラメータで常に同一の PASS/FAIL。[Q1]
  - 判定: ソース中の `<random>` / `rand()` / 時刻依存の初期化の出現が 0 件であること。
- SR-2.2: CPU 計測(BR2.4)のみ実行環境依存であり、合否に関与しない報告値であることを出力に明記。[Q1]
  - 判定: cpu ケースの出力行に「報告のみ・環境依存」の注記と、閾値なし(`tolerance=null` 相当)の表示が含まれること。
- SR-2.3: 周波数検出の許容差(コンパイラ/プラットフォーム間)。**bit-exact 一致は要求しない。** normative な FFT 構成における bin 幅は Δf = fs/N であり、N=32768 のとき 1.4648 Hz(48 kHz)/ 1.3458 Hz(44.1 kHz)。
  - 要件(a) 実装間差: 同一入力に対する検出ピーク f_out の値が、コンパイラ・最適化レベル・プラットフォームの違いによって **±0.01 bin = ±0.015 Hz** を超えて変動しないこと。
  - 要件(b) ハーネス自身の測定誤差(放物線補間の残差バイアスを含む)は **±0.05 bin = ±0.074 Hz** 以内であること。
  - 十分性の根拠: BR2.1 の判定余裕は |ratio − 0.95| ≤ 0.95×0.005 = 0.00475、f_out に換算して ±0.00475×f_in。最も厳しい f_in = 110 Hz でも **±0.52 Hz**(0.5% = 0.55 Hz を下回る側の実効値)であり、(a) はその 2.9%(約 1/34)、(b) は 14% にとどまる。440 Hz では ±2.09 Hz、3520 Hz では ±16.7 Hz と余裕はさらに大きい。ゆえに浮動小数の実装差が PASS/FAIL を反転させない。
  - 判定: 同一ソースを `-O0` と `-O2` の 2 ビルド(利用可能なら別コンパイラも)で実行し、全 pitch ケースの f_out の差が ±0.015 Hz 以内であること。
- SR-2.4: 同一バイナリでの決定論。同一バイナリ・同一入力を 2 回実行したとき、標準出力(cpu ケースの報告値行を除く)が bit-exact に一致すること。
  - 判定: `make verify` の出力を 2 回取得し、cpu 行を除去した上で `diff` が空(差分 0 行)であること。

## SR-3 判定の健全性

- SR-3.1: 合否閾値(0.95±0.5%、10ms、k=3.0、warmup 250ms、N=32768 等)はコード中の名前付き定数とし、検証を通すための場当たり的な緩和を禁止(team-practices「品質目標を弱めない」)。
  - 判定: 各閾値が単一の名前付き定数として定義され(リテラルの散在なし)、判定式がその定数のみを参照していること。
- SR-3.2: 終了コード契約。**normative は WF-6 / BR2.5 であり、本 SR は追加の区別を導入しない。** 二値のみ: 非 CPU ケースが全 PASS で `0`、1 件以上 FAIL で `1`。
  - 「実行不能」(prepare 失敗、fs / maxBlockFrames 不正など)は当該ケースを `passed=false` として扱い、理由を標準エラー出力に 1 行出力する。`prepare` の成否は fs と maxBlockFrames にのみ依存するため、prepare 失敗時は同一 fs の非 CPU ケース(pitch / latency / glitch)も必ず FAIL となり、終了コード `1` が保証される。実行不能を別コードで区別する必要はない。
  - 終了コード `2` は使用しない。将来 `2` を導入する場合は functional-design(WF-6 / BR2.5)の改訂を前提とし、本書の単独変更では導入できない。
  - 判定: 全緑実行で `$? == 0`、意図的に閾値を破った実行で `$? == 1`、prepare を失敗させた実行で `$? == 1` かつ stderr に理由行が 1 行以上あること。ソース上に `2` を返す経路が 0 件であること。

## SR-4 リアルタイム安全性(NFR-2 — 検証ハーネス観点)

- SR-4.1: 契約の同一性。検証ランナーが u1 の `process()` を呼ぶ計測区間は、実機オーディオコールバックと**同一の契約**を満たす: ヒープ確保/解放・ロック・ファイル I/O・ログ出力・システムコール・例外を一切発生させない(NFR-2、契約 1)。信号生成・出力蓄積・PASS/FAIL 出力はすべて計測区間の**外**で行い、入力信号バッファと出力蓄積バッファは `prepare` 呼び出し前に最終サイズまで確保(`resize` / `reserve`)しておく。
  - 判定: 計測区間内で SR-4.2 の確保カウンタ差分が 0、かつ区間内のコードに `printf` / ストリーム出力 / mutex / `throw` の出現が 0 件であること。
- SR-4.2: 検出手段(確保カウンタ)。グローバル `operator new` / `operator new[]` / `operator delete` / `operator delete[]` を `tests/verify.cpp` 内で差し替え、呼び出し回数を単一の非 atomic な `std::size_t` カウンタで数える(WF-1 が逐次ランナー・並行なしを定めるため同期は不要で、カウンタ自体がロックを持たない)。各ケースの process ループの入口と出口でカウンタ値を読み、**差分 == 0** を検証ビルドで `assert` する。
  - 差分 > 0 の場合は `assert` により即時 abort し、ケース名と確保回数を標準エラー出力に出す。abort は WF-6 の正常終了経路ではないため、0/1 の終了コード契約に干渉しない(u1 の SR-2.2 と同じ「デバッグ時 assert」方針)。
  - カウンタの読み取り・比較・`assert` はいずれも計測区間の**境界**で行い、区間内では行わない。したがって SR-4.1 に違反しない。
  - 検証ビルドは `NDEBUG` を定義しない(`assert` を有効に保つ)。
  - 判定: 全ケースで確保デルタ == 0 であること(PASS)。1 回でも > 0 なら本要件は未達(abort で顕在化)。
- SR-4.3: 報告。各ケースの確保回数(期待値 0)を標準出力の情報行に含め、cpu ケースでは BR2.4 の `cpuRatio` と並記する。これは既存の検証マトリクス(BR2.5)に**新しいケースを追加せず**、各ケースの `passed` 判定式(BR2.1〜BR2.3)も**変更しない**横断的不変条件である。
  - 判定: 全ケースの情報行に `alloc=0` 相当の表示があること。

## Assumptions & Open Questions

None.

## Review

**Reviewer:** aidlc-architecture-reviewer-agent
**Iteration:** 2
**Verdict:** READY
**Request Challenge:** review:0586766254d36609c5e720cc0af0d8a5
**Date:** 2026-09-03T15:18:09Z

### Findings

| ID | Severity | Location | Finding | Required action | Status |
|---|---|---|---|---|---|
| R-01 | Critical | traceability.json, upstream_ids | NFR-2(リアルタイム安全性)が upstream_ids から欠落していた。 | 修正済み: `upstream_ids` に `NFR-2` を追加し、`coverage` で `target: "SR-4.1, SR-4.2, SR-4.3"` にマッピング。security-requirements.md に SR-4(リアルタイム安全性)セクションが新設され、確保カウンタによる検出手段まで具体化されている。requirements.md の NFR-1〜NFR-6 は全て upstream_ids に含まれることを確認した。 | Resolved |
| R-02 | Critical | security-requirements.md > SR-3.2 vs. functional-spec.md > WF-6 | SR-3.2 の終了コード 2 が WF-6/BR2.5(0/1 のみ)と整合しなかった。 | 修正済み: 承認済みの上流(WF-6 / BR2.5)を編集せず、SR-3.2 側を「二値のみ(0/1)。実行不能は該当ケースを FAIL として扱い exit 1 に帰着させる。終了コード 2 は使用しない」に統一。WF-6・BR2.5 本文(「1つでも FAIL があれば終了コード1、なければ0」)と実際に一致することを確認した。 | Resolved |
| R-03 | Major | security-requirements.md > SR-2 | FFT ピーク検出の数値精度・許容差が未規定だった。 | 修正済み: SR-2.3 に実装間差(±0.015 Hz)とハーネス測定誤差(±0.074 Hz)の許容差、bin 幅 Δf=fs/N の算出根拠、BR2.1 判定余裕との比較による十分性の定量的根拠、および `-O0`/`-O2` 比較による判定手段が追加された。 | Resolved |
| R-04 | Major | security-requirements.md > SR-1.1 | 「ファイル書き出しは行わない(必要になれば将来オプション)」というヘッジ表現が隔離要件を曖昧にしていた。 | 修正済み: 「ファイル書き出しは行わない。要件変更時はセキュリティレビューを必須とし、本書の改訂を伴う」と断定表現に変更され、変更条件も明記された。 | Resolved |
| R-05 | Major | traceability.json > "reverse" entries | reverse エントリ(SR-2.2, SR-3.2)の意味論が曖昧だった。 | 修正済み: `reverse` は空配列 `[]` になり、該当する下流→上流の対応は通常の `coverage` エントリ(`FR-2.1` → SR-3.2、`FR-3.4` → SR-2.2)として明示的な `target` 文字列付きで再構成された。曖昧な二重構造が解消されている。 | Resolved |

### Validation Tool Results

| Tool | Result | Interpretation |
|---|---|---|
| `python3 -m json.tool traceability.json` | 成功、整形出力あり | JSON は妥当。 |
| requirements.md NFR-x 全数チェック | NFR-1〜NFR-6 が upstream_ids に全件含まれる | R-01 解消を裏付け。N/A 根拠を要する欠落なし。 |
| 各 SR の判定可能性チェック | SR-1.1〜SR-4.3 の全項目に定量的な「判定:」記述あり(件数閾値、bit-exact diff、許容差 Hz、確保デルタ==0 など) | pass/fail が機械的に判定可能。 |
| u1 側 SR-2.2 参照のスポットチェック | `construction/u1-dsp-core/nfr-requirements/security-requirements.md` に SR-2.2(デバッグ assert / リリースクランプ)が実在し、SR-4.2 の記述と整合 | u2→u1 のクロスユニット参照は有効。 |

### Summary

Iteration 1 の Critical 2 件・Major 3 件はすべて解消を確認した。特に R-02 は上流(WF-6/BR2.5)を変更せず下流側を整合させる方針で、承認済み成果物の再編集を避けつつ矛盾を解消しており妥当。新規の Critical/Major 欠陥は検出されず、READY と判定する。

