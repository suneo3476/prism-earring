# ビジネスルール — u1-dsp-core

`prism::PitchShifter` が守る計算・安全性ルール。技術非依存(数式のみ)。
出典タグ: FR/NFR は requirements.md、D-nn は decisions.md、Q1 は functional-design-questions.md。

```yaml
rules:
  - id: BR1.1
    statement: セント→速度比の変換は ratio = 2^(cents/1200)。半音=100 セントの決め打ちを禁止する。
    category: computation
    applies_to: [ShifterParams.shiftCentsL, ShifterParams.shiftCentsR, ReadHead.rate]
    trigger: 平滑後シフト量から ReadHead.rate を更新するとき(per-sample)。
    logic:
      IF: 平滑後の cents 値 c(∈[-150,0])が与えられる
      THEN: rate = pow(2, c/1200) とする(例: c=-89 → 0.94860…、c=0 → 1.0)
    violation: 誤ったピッチ(例 -89 を半音扱い)→ ピッチ精度検証 BR2.1 が FAIL。
    source: [FR-1.2, "CLAUDE.md 半音決め打ち禁止", D-01]

  - id: BR1.2
    statement: 全パラメータはセッター受領時に定義域へクランプする。
    category: validation
    applies_to: [ShifterParams]
    trigger: setShiftCentsL/R・setDryWet・setCrossfadeMs 呼び出し時(atomic store 前)。
    logic:
      IF: 入力値 v
      THEN: >
        shiftCentsL/R → clamp(v, -150, 0);
        dryWet → clamp(v, 0, 1);
        crossfadeMs → clamp(v, 10, 100)。非有限値(NaN/Inf)は無視(store しない)。
    violation: 範囲外速度比・負の窓長 → バッファ外アクセスやグリッチ。
    source: [FR-1.2, FR-1.3, FR-1.4, "contract-summary 契約1/契約2"]

  - id: BR1.3
    statement: パラメータ適用は per-sample 一次指数平滑を経る。時定数 20ms ±25%(63% 整定で測定)。
    category: computation
    applies_to: [SmoothedParam]
    trigger: process() の per-sample ループ内、各パラメータ反映時。
    logic:
      IF: 目標 target(ブロック頭で atomic から load)、現在 current、係数 a=exp(-1/(0.020*fs))
      THEN: current = a*current + (1-a)*target(毎サンプル)。current を実効値として使用。
    violation: ステップ変化がズィッパーノイズ(不連続)→ BR2.3 グリッチ検証 FAIL。
    source: [FR-1.5, D-03, "stories US1.2 AC"]

  - id: BR1.4
    statement: >
      窓長 window_samples = round(crossfadeMs*fs/1000)。crossfadeMs の平滑は行うが、
      window_samples への反映(ラッチ)は「次のクロスフェード境界」でのみ行う。
    category: state-transition
    applies_to: [ReadHead.phaseOffsetSamples, RingBuffer]
    trigger: per-sample。クロスフェードのフェード完了(境界)を跨いだ瞬間。
    logic:
      IF: 現ブロックのクロスフェード位相が境界(headA↔headB の入れ替わり点)に到達
      THEN: >
        pending_window = round(current_crossfadeMs*fs/1000) を有効窓長として latch し、
        phaseOffsetSamples = pending_window/2 を更新する。境界外では旧窓長を維持。
    violation: 窓長を即時変更すると位相不連続 → グリッチ(NFR-4 違反)。
    source: [FR-1.4, D-01, Q1, "stories US1.2 AC(window_samples 式)"]

  - id: BR1.5
    statement: process() はリアルタイム安全。ヒープ確保/解放・ロック・I/O・システムコール・例外を行わない。
    category: safety
    applies_to: [PitchShifter.process]
    trigger: process() 実行中(常時)。
    logic:
      IF: process() 経路
      THEN: >
        メモリは prepare() で確保済みの固定バッファのみ使用;可変長コンテナの伸長禁止;
        mutex/lock 禁止;printf/ファイル I/O 禁止;throw 禁止(noexcept)。
        パラメータ授受は atomic load/store のみ。
    violation: 実機ドロップアウト・優先度逆転(NFR-2 違反)。
    source: [NFR-2, "CLAUDE.md リアルタイムの鉄則", D-03]

  - id: BR1.6
    statement: 2 本の読み出しヘッドを窓半周(window_samples/2)ずらして等パワークロスフェード合成する。
    category: computation
    applies_to: [ReadHead, RingBuffer]
    trigger: per-sample の出力合成時。
    logic:
      IF: headA/headB の補間読み値 yA/yB、クロスフェード位相 θ∈[0, π/2]
      THEN: >
        gA = cos(θ), gB = sin(θ)(gA^2+gB^2=1 の等パワー);out_wet = gA*yA + gB*yB。
        θ は 1 窓ぶんで 0→π/2 を線形に進み、境界で headA/headB の役割を入れ替える。
    violation: 単純線形フェードだと合成パワーが谷になり振幅うねり → BR2.3 FAIL リスク。
    source: [FR-1.1, D-01, "CLAUDE.md 2 ポインタ半周ずらし"]

  - id: BR1.7
    statement: >
      設計値遅延 latencySamples = 基準読み出しオフセット baseOffset + 平均ポインタ遅れ、
      平均ポインタ遅れ = (1-ratio)*window_samples/2。baseOffset = 8 サンプル(固定, DesignConstants)。
      getLatencySamples() は「現在ラッチ済みの window_samples」で算出した設計値を返し、
      未ラッチの pending window(WF-4 の境界待ち)は反映しない。
    category: computation
    applies_to: [PitchShifter.getLatencySamples, ReadHead, DesignConstants]
    trigger: getLatencySamples() 呼び出し時、および遅延報告(D-04)時。
    logic:
      IF: baseOffset = 8 サンプル(≈0.17ms @48kHz;線形補間ガード + 安全余裕)、現 ratio、現在ラッチ済み window_samples
      THEN: >
        latencySamples = baseOffset + (1-ratio)*window_samples/2。
        window_samples は現在ラッチ済みの値を用いる(pending window 変更はラッチ完了まで
        getLatencySamples() の戻り値に影響しない)ため任意時刻で安定・予測可能。
      WORST_CASE_BUDGET: >
        最悪ケースは shift=-150 cents(ratio = 2^(-150/1200) ≈ 0.9170)かつ crossfadeMs=100
        (window = 100ms)。最大ポインタ遅れ(窓全走査)= (1-ratio)*window ≈ 0.083*100ms ≈ 8.3ms。
        baseOffset(8 サンプル ≈ 0.17ms @48kHz) + 最大遅れ ≈ 8.5ms ≤ 10ms 予算(NFR-1)を満たす。
    violation: 表示遅延と実測(BR2.2/FR-3.2)が乖離 → US1.4 テスト不一致。
    source: [NFR-1, D-04, "stories US1.4 AC", "contract-summary getLatencySamples"]

  - id: BR1.8
    statement: モノラル入力の L=R 複製は呼び出し側の責務。コアは常に 2ch を処理する。
    category: contract
    applies_to: [PitchShifter.process]
    trigger: process(in, out, numFrames) 呼び出し時。
    logic:
      IF: 論理入力が 1ch
      THEN: 呼び出し側(Worklet/検証ハーネス)が in[0]=in[1] を用意する。コアは in/out を [2][numFrames] とみなす。
    violation: コアが ch 数分岐を持つと API 複雑化(FR-2.2 の設計意図に反する)。
    source: [FR-2.2, FR-2.3, D-06]
```

## 人間可読サマリ表

| ID | ルール | カテゴリ | 適用対象 | 出典 |
|---|---|---|---|---|
| BR1.1 | ratio = 2^(cents/1200)、半音決め打ち禁止 | computation | shiftCents→rate | FR-1.2 |
| BR1.2 | セッターで定義域クランプ、非有限値は無視 | validation | ShifterParams | FR-1.2/1.3/1.4 |
| BR1.3 | per-sample 指数平滑、a=exp(-1/(0.020*fs))、20ms±25% | computation | SmoothedParam | FR-1.5, D-03 |
| BR1.4 | window=round(crossfadeMs*fs/1000)、境界でのみラッチ | state-transition | 窓長/位相 | FR-1.4, Q1 |
| BR1.5 | process はリアルタイム安全(確保/ロック/IO/例外なし) | safety | process() | NFR-2 |
| BR1.6 | 2 ヘッド半周ずらし等パワー(cos/sin)合成 | computation | ReadHead | FR-1.1, D-01 |
| BR1.7 | latency = baseOffset(=8) + (1-ratio)*window/2、ラッチ済み窓長で算出、最悪 ≈8.5ms ≤ 10ms | computation | getLatencySamples | NFR-1, D-04 |
| BR1.8 | モノラル複製は呼び出し側、コアは常に 2ch | contract | process() | FR-2.2, D-06 |

## Assumptions & Open Questions

None.
