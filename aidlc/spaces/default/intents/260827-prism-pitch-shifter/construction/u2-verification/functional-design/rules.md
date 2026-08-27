# ビジネスルール — u2-verification

オフライン検証ランナーの判定ルール。技術非依存(数式のみ、コード・フレームワーク非依存)。
検証側 FFT はオフライン限定(音声経路ではない、FR-3.1)。出典: FR-3.x/NFR は requirements.md、Q2 は functional-design-questions.md。

```yaml
rules:
  - id: BR2.1
    statement: >
      ピッチ精度検証。正弦波 110/440/3520 Hz を既定パラメータのシフタに通し、
      出力の周波数比が 0.95 ±0.5% に収まること。
    category: judgement
    applies_to: [TestCase(pitch), VerificationReport]
    trigger: testName=pitch のケース実行時。
    logic:
      IF: 入力周波数 f_in ∈ {110,440,3520}、既定パラメータ(shiftCents=-89, dryWet=1, crossfadeMs=50)
      THEN: >
        出力を radix-2 FFT(オフライン限定, FR-3.1)し、最大ピーク近傍を放物線補間(parabolic
        interpolation)で精密化して f_out を得る;ratio = f_out/f_in;
        passed = |ratio - 0.95| <= 0.95*0.005(相対 ±0.5%)。
    violation: シフト量誤り(例 半音決め打ち BR1.1 違反)を検出 → FAIL。
    source: [FR-3.1, NFR-3, "stories US2.2 AC"]

  - id: BR2.2
    statement: >
      レイテンシ検証。振幅 1.0 の単位インパルス(warmup reset 後、sample 0 に配置)をシフタに通し、
      出力が |y| > 0.05(-26 dBFS)を初めて超えるサンプルから遅延サンプル数を得る。
      hard 要件として実測 ≤ 10ms、かつ getLatencySamples() 設計値との一致許容差内であること。
    category: judgement
    applies_to: [TestCase(latency), VerificationReport]
    trigger: testName=latency のケース実行時。
    logic:
      IF: 単位インパルス(振幅 1.0 を sample 0、warmup reset 後)、既定パラメータ、設計値 L_design=getLatencySamples()、現 ratio と window_samples
      THEN: >
        出力絶対値が |y| > 0.05(-26 dBFS)を初めて超えるサンプル index を latency_samples とする;
        latency_ms = latency_samples/fs*1000;
        passed = (latency_ms <= 10) AND (|latency_samples - L_design| <= tolerance);
        tolerance = (1-ratio)*window_samples/2 + 8 サンプル(設計値一致許容差)。
    violation: 遅延が予算超過、または設計値 API(BR1.7)と乖離 → FAIL(US1.4 突合せ不整合)。
    source: [FR-3.2, NFR-1, "stories US1.4/US2.2 AC", "decisions D-04"]

  - id: BR2.3
    statement: >
      グリッチ検証。連続正弦波を通し、隣接サンプル差分が期待最大スロープに対する相対閾値を
      超える点(不連続)をゼロ件とする。
    category: judgement
    applies_to: [TestCase(glitch), VerificationReport]
    trigger: testName=glitch のケース実行時。
    logic:
      IF: 連続正弦波(長時間)出力 y[n]、期待最大スロープ maxSlope = 2π*f*A/fs(1 サンプルあたり)
      THEN: >
        各 n で |y[n]-y[n-1]| を計算;|y[n]-y[n-1]| > k*maxSlope(k = 3.0)を不連続として計数;
        ただし先頭 warmup 250ms(5×20ms 平滑整定 + crossfadeMs=100 の窓充填をカバー)は除外;
        passed = (glitch_count == 0)。
    violation: 窓境界の位相不連続・ズィッパーノイズ(BR1.3/BR1.4 違反)を検出 → FAIL。
    source: [FR-3.3, NFR-4, "stories US2.2 AC"]

  - id: BR2.4
    statement: CPU 検証。処理時間/実時間比を計測し報告のみ(合格閾値なし)。
    category: measurement
    applies_to: [TestCase(cpu), VerificationReport]
    trigger: testName=cpu のケース実行時。
    logic:
      IF: 一定長の信号を 512 フレームブロック単位で処理
      THEN: >
        std::chrono::steady_clock で処理ループ全体の経過を計測し elapsedProcessingSeconds を得る;
        cpuRatio = elapsedProcessingSeconds / (numFrames/fs) を算出し小数第 3 位まで表示;
        tolerance=null、passed=true(常に)。終了コードに影響しない(Q2-A)。
    violation: なし(報告専用)。ただし異常値は情報として表示。
    source: [FR-3.4, Q2, "stories US2.2 AC"]

  - id: BR2.5
    statement: >
      全検証を fs=44100 と 48000 の両方で実行。per-case PASS/FAIL 行を標準出力し、
      1 つでも FAIL があれば終了コード 1。
    category: control-flow
    applies_to: [VerificationReport]
    trigger: ランナー実行時(全ケース走破後)。
    logic:
      IF: 全 TestCase(fs∈{44100,48000} × {pitch,latency,glitch,cpu})を実行
      THEN: >
        各ケースで "PASS"/"FAIL" 行 + caseName + measured/expected/tolerance を出力;
        exit_code = (∃ passed==false among non-cpu cases) ? 1 : 0(BR2.4 は除外)。
    violation: マージ条件(全検証緑, team-practices)を満たさない → CI で赤。
    source: [FR-2.1, Q2, "unit-of-work U2 制約", "stories US2.2 AC(両 fs)"]
```

## 人間可読サマリ表

| ID | 検証 | 合否基準 | 終了コード寄与 | 出典 |
|---|---|---|---|---|
| BR2.1 | ピッチ精度 | FFT+放物線補間の ratio が 0.95±0.5% | あり | FR-3.1, NFR-3 |
| BR2.2 | レイテンシ | 実測 ≤10ms かつ \|実測-設計値\| ≤ (1-ratio)*window/2 + 8 サンプル。閾値 \|y\|>0.05(-26dBFS)、インパルス振幅 1.0@sample0 | あり | FR-3.2, NFR-1 |
| BR2.3 | グリッチ | \|y[n]-y[n-1]\| > k*maxSlope(k=3.0, maxSlope=2πfA/fs)の不連続 0 件、warmup 250ms 除外 | あり | FR-3.3, NFR-4 |
| BR2.4 | CPU 比 | steady_clock で 512 フレームブロック計測、cpuRatio=elapsed/(frames/fs) 小数第3位、報告のみ | なし | FR-3.4, Q2 |
| BR2.5 | 両 fs 実行 + 判定 | 全非 CPU ケース PASS で exit 0、他は 1 | — | FR-2.1, Q2 |

## Assumptions & Open Questions

None.
