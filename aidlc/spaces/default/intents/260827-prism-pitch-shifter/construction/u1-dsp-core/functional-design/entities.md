# エンティティ — u1-dsp-core

DSP コア `prism::PitchShifter`(FR-1.x/FR-2.x、D-01〜D-03)が保持する概念エンティティ。
公開設定エンティティ `ShifterParams` と、`process()` 経路が読み書きする内部状態エンティティを定義する。
技術非依存(数式のみ、コード・フレームワーク非依存)。

```yaml
entities:
  - name: ShifterParams
    kind: value-object
    summary: 実行時可変のシフタ設定(公開面)。セッター経由で atomic に反映される [FR-1.2..FR-1.5]
    identifier:
      # 設定オブジェクトはインスタンス同一性を持たない値オブジェクトのため、
      # 属性ベースの識別子を持たない。domain-design R-01 の解決(2)。
      strategy: conceptual-singleton
      note: PitchShifter インスタンスごとに 1 個の論理設定。属性キー無し。
    attributes:
      - name: shiftCentsL
        type: real            # セント
        range: [-150, 0]
        default: -89
        note: 左チャンネルのシフト量。ratio = 2^(cents/1200)(BR1.1)[FR-1.2]
      - name: shiftCentsR
        type: real            # セント
        range: [-150, 0]
        default: -89
        note: 右チャンネルのシフト量(複聴の可能性のため独立)[FR-1.2]
      - name: dryWet
        type: real
        range: [0, 1]
        default: 1
        note: 原音とのミックス比。0=原音のみ、1=処理音のみ [FR-1.3]
      - name: crossfadeMs
        type: real            # ミリ秒
        range: [10, 100]
        default: 50
        note: ディレイライン窓長。window_samples = round(crossfadeMs*fs/1000)(BR1.4)[FR-1.4]

  - name: RingBuffer
    kind: internal-state
    summary: 入力を書き込む固定長リングバッファ。prepare() で確保、以後サイズ不変 [NFR-2, D-01]
    identifier:
      strategy: owned-singleton
      note: >
        PitchShifter インスタンスあたり 1 本。レイアウトは channel-major の分離バッファ
        data[2][capacitySamples](ch ごとに連続した実数配列)。writeIndex は全 ch 共有で
        フレームごとに 1 回だけ進む。
    attributes:
      - name: capacitySamples
        type: integer
        range: [1, "∞"]
        constraint: >
          最大窓長(crossfadeMs=100)+ 最大ブロック長 + 補間余裕を包含する長さ。
          prepare(sampleRate, maxBlockFrames) 時に確定し不変。
        default: none          # prepare で算出
      - name: writeIndex
        type: integer
        range: [0, "capacitySamples-1"]
        default: 0
        note: >
          次に書き込む位置(モジュロ capacitySamples で進行)。全 ch 共有の単一インデックスで、
          1 フレーム内で全 ch 書き込み後に 1 回だけ進める。
      - name: data
        type: real-array
        constraint: >
          channel-major の分離バッファ data[2][capacitySamples]。各 ch は長さ
          capacitySamples の連続実数配列。書き込みは data[ch][writeIndex]、読み出しは
          data[ch][n],data[ch][(n+1) mod cap] の線形補間。reset() で全 ch 0 クリア。
        default: "all-zero"

  - name: ReadHead
    kind: internal-state
    summary: >
      読み出しポインタ。2 本(headA/headB)を窓半周ずらして 2^(cents/1200) 倍速で走らせ
      等パワークロスフェードで合成(BR1.6, D-01)。小数位置は線形補間で読む(D-02)。
    cardinality: 2               # ch あたり 2 head(A/B)。ステレオでは 2ch × 2 head = 4
    # ER 注記「ch ペアあたり 2 read head」= 2 本のヘッドは共有の位相機構(phaseOffset/θ の
    # 進行則)であり、各 ch にその ch 固有の ratio(ratioL/ratioR)を適用して走らせる。
    identifier:
      strategy: indexed
      note: headIndex ∈ {0,1}(A=0, B=1)。位相オフセットで区別。
    attributes:
      - name: headIndex
        type: integer
        range: [0, 1]
        default: none
        note: 0=headA(位相 0)、1=headB(位相 +window/2)。
      - name: readPos
        type: real               # 小数サンプル位置
        range: [0, "capacitySamples"]  # モジュロ環状
        default: none            # prepare/reset で writeIndex から基準オフセット分だけ手前に設定
        note: 実数位置。整数部で近傍 2 サンプルを取り、小数部で線形補間(D-02)。
      - name: rate
        type: real
        range: [0.5, 1.0]        # ratio=2^(cents/1200)、cents∈[-150,0] で約 0.917..1.0
        default: 0.9486          # ratio(-89 cents)
        note: readPos の毎サンプル進行量。平滑後シフト量から BR1.1 で算出。
      - name: phaseOffsetSamples
        type: real
        range: [0, "window_samples"]
        default: 0               # headA=0, headB=window_samples/2
        note: 2 本のヘッド間の固定位相差(窓半周 = window_samples/2, BR1.6)。

  - name: SmoothedParam
    kind: internal-state
    summary: >
      per-sample 一次指数平滑器(ズィッパーノイズ防止, FR-1.5, D-03)。
      per-sample 平滑対象は 3 個: ratioL(ch0=L のシフト)、ratioR(ch1=R のシフト)、
      dryWet(両 ch 共通)。各 ch は自分の平滑シフトを持ち、rate = 2^(current/1200) を
      独立に算出する(BR1.1)。crossfadeMs は per-sample 平滑を行わず、クロスフェード境界で
      window_samples にラッチするのみ(BR1.4, Q1-A)。dryWet と crossfadeMs は両 ch 共通で
      それぞれ 1 個。
    cardinality: 3               # ratioL(ch0), ratioR(ch1), dryWet(両ch共通)。crossfadeMs は境界ラッチ値で per-sample 平滑対象外
    identifier:
      strategy: indexed
      note: >
        paramId ∈ {0:ratioL(ch0=L のシフト平滑), 1:ratioR(ch1=R のシフト平滑),
        2:dryWet(両ch共通)}。crossfadeMs は per-sample SmoothedParam ではなく境界ラッチ値(BR1.4)。
    attributes:
      - name: paramId
        type: integer
        range: [0, 3]
        default: none
      - name: target
        type: real
        constraint: ブロック頭で atomic から 1 回 load したクランプ済み目標値(FR-1.5)。
        default: "該当パラメータの default"
      - name: current
        type: real
        constraint: 平滑後の現在値。毎サンプル current += (1-a)*(target-current)。
        default: "target と同一(prepare 直後)"
      - name: coeff_a
        type: real
        range: [0, 1)
        constraint: a = exp(-1/(0.020*fs))。時定数 20ms ±25%(63% 整定で測定)(BR1.3, FR-1.5)。
        default: none            # fs から prepare 時に算出

  - name: DesignConstants
    kind: design-constant
    summary: >
      設計時に固定する数値定数。baseOffset は基準読み出しオフセットで、遅延式 BR1.7 の
      定数項。線形補間の近傍 2 サンプル(data[n],data[n+1])参照ガード + 安全余裕として定める。
    identifier:
      strategy: conceptual-singleton
      note: PitchShifter インスタンスあたり論理 1 個。属性キー無し。
    attributes:
      - name: baseOffset
        type: integer            # サンプル
        value: 8
        default: 8
        note: >
          baseOffset = 8 サンプル(≈0.17ms @48kHz)。線形補間ガード + 安全余裕。
          readPos は writeIndex から baseOffset(+phaseOffsetSamples)手前に置く。
          遅延式 BR1.7 の定数項: latency = baseOffset + (1-ratio)*window_samples/2。
```

## サマリ(日本語)

`ShifterParams` は実行時に変更できる 4 つの公開設定(shiftCentsL/R・dryWet・crossfadeMs)を持つ値オブジェクトで、
セッターは atomic に store し、`process()` はブロック頭で 1 回だけ load する(FR-1.5, D-03)。
内部状態は 4 種:`RingBuffer`(prepare で確保する固定長入力バッファ。channel-major の分離バッファ data[2][capacitySamples]、writeIndex は全 ch 共有でフレームごとに 1 回進行、NFR-2/D-01)、
`ReadHead`(2^(cents/1200) 倍速で走る読み出しポインタ。ch あたり 2 本を窓半周ずらして等パワー合成、線形補間、D-01/D-02。2 本は共有位相機構で各 ch にその ch の ratio を適用)、
`SmoothedParam`(per-sample 平滑は ratioL(ch0)・ratioR(ch1)・dryWet の 3 個。crossfadeMs は境界ラッチ値で per-sample 平滑対象外。係数 a=exp(-1/(0.020*fs))、FR-1.5/D-03)、
`DesignConstants`(baseOffset = 8 サンプル。遅延式 BR1.7 の基準読み出しオフセット、線形補間ガード + 安全余裕)。
`ShifterParams` と `RingBuffer` は属性ベースの一意キーを持たない設定/所有シングルトンのため、
domain-design レビュー R-01 の指摘は「概念識別子(値オブジェクト/所有シングルトン)」として本ステージで解決する。

## Assumptions & Open Questions

None.
