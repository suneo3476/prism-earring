# エンティティ — u2-verification

オフライン数値検証ランナー(VerificationHarness, FR-3.x)のエンティティ。
技術非依存(データ構造の論理定義のみ)。u1 の公開面(contract-summary 契約 1)を検証対象として直接呼ぶ。

```yaml
entities:
  - name: TestCase
    kind: value-object
    summary: 1 件の検証入力仕様。fs × testName × 信号仕様の組(FR-3.1〜FR-3.4, US2.2)。
    identifier:
      strategy: composite
      note: (fs, testName) の組で一意。属性ベース識別子(domain-design R-01 の解決(2))。
    attributes:
      - name: fs
        type: integer            # Hz
        range: [44100, 48000]    # 検証マトリクスは 44.1k と 48k(BR2.5)
        default: none
      - name: testName
        type: enum
        values: [pitch, latency, glitch, cpu]
        default: none
        note: 検証 4 種(BR2.1..BR2.4)。
      - name: signalSpec
        type: struct
        constraint: >
          testName に応じた入力信号定義。
          pitch: {kind: sine, freqs: [110,440,3520] Hz, durationSec, amplitude};
          latency: {kind: impulse, amplitude: 1.0, position: sample 0(warmup reset 後)};
          glitch: {kind: sine, freq, durationSec(長時間), amplitude};
          cpu: {kind: sine or noise, durationSec, blockFrames: 512}。
        default: none

  - name: VerificationReport
    kind: value-object
    summary: 1 ケースの判定結果。per-case PASS/FAIL 行と合算終了コードの元データ(Q2-A)。
    identifier:
      strategy: attribute
      note: caseName を一意キーとする(例 "pitch@44100/440Hz")。domain-design R-01 の解決(1)。
    attributes:
      - name: caseName
        type: string
        constraint: 一意な人間可読ケース名(testName + fs + サブケースを含む)。
        default: none
      - name: fs
        type: integer
        range: [44100, 48000]
        default: none
      - name: testName
        type: enum
        values: [pitch, latency, glitch, cpu]
        default: none
      - name: measured
        type: real
        constraint: >
          実測量。pitch=出力/入力周波数比;latency=遅延サンプル数(または ms);
          glitch=検出不連続点数;cpu=処理時間/実時間比。
        default: none
      - name: expected
        type: real
        constraint: 期待値。pitch=0.95;latency=getLatencySamples() 設計値;glitch=0;cpu=なし(報告のみ)。
        default: none
      - name: tolerance
        type: real
        constraint: >
          許容差。pitch=±0.5%(相対);latency=(1-ratio)*window_samples/2 + 8 サンプル(設計値一致許容)+ 上限 10ms;
          glitch=0(不連続ゼロ、k=3.0*maxSlope 超えを計数、warmup 250ms 除外);cpu=null(閾値なし, BR2.4)。
        default: none
      - name: passed
        type: boolean
        constraint: >
          measured が expected±tolerance を満たすか。cpu は常に true(報告のみ, Q2-A)。
          いずれかの passed=false があれば runner は終了コード 1(BR2.5)。
        default: none
```

## サマリ(日本語)

`TestCase` は「どの fs で・どの検証を・どんな信号で」を表す入力仕様で、検証マトリクスは fs∈{44100,48000} × testName∈{pitch,latency,glitch,cpu}(BR2.5)。
`VerificationReport` はケース単位の結果(caseName・measured・expected・tolerance・passed)を保持し、
per-case で PASS/FAIL 行を出力、1 つでも passed=false があれば終了コード 1 を返す(Q2-A)。
CPU 比(BR2.4)は tolerance=null・常に passed=true の報告専用ケース(FR-3.4 は計測・報告まで)。
`VerificationReport` は caseName を属性ベースの一意識別子とし、domain-design レビュー R-01 を解決する。

## Assumptions & Open Questions

None.
