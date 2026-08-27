# コンポーネントカタログ — prism

```yaml
components:
  - name: PitchShifter
    summary: 依存ゼロの C++17 ディレイライン型ピッチシフタ(DSP コア)
    behaviour: >
      ステレオ入力をチャンネル独立のセント指定でピッチシフトする。リングバッファに書き込み、
      2 本の読み出しポインタを窓半周ずらして 2^(cents/1200) 倍速で走らせ、等パワークロスフェードで合成。
      補間は線形。全パラメータは std::atomic に格納され、処理ブロック頭で 1 回 load、
      per-sample 一次指数平滑(時定数 20ms)を経て適用。コールバック経路にヒープ確保・ロック・
      I/O・例外なし。バッファは prepare() で確保・固定。音声経路に FFT を使わない。
    responsibilities:
      - ピッチシフト処理(FR-1.1〜FR-1.5)
      - 入出力形式と fs 非依存性(FR-2.1〜FR-2.3)
      - 遅延設計値の提供 getLatencySamples()(US1.4)
    depends_on: []
    dependents:
      - component: VerificationHarness
        interaction: オフライン検証対象として直接呼び出す
      - component: WasmBridge
        interaction: WASM 化のため直接呼び出す
    external_dependencies: []
    entities:
      - name: ShifterParams
        identifier: name
        attributes: [shiftCentsL, shiftCentsR, dryWet, crossfadeMs]
  - name: VerificationHarness
    summary: clang++ のみでビルド・実行できるオフライン数値検証ランナー(C++)
    behaviour: >
      正弦波・インパルスを生成して PitchShifter に通し、自前 radix-2 FFT のピーク検出で
      周波数比を算出、インパルス到達サンプルで遅延を計測、隣接サンプル差分の相対閾値で
      グリッチを検出、処理時間/実時間比を計測する。44.1kHz と 48kHz の両方で全検証を実行し、
      緑/赤の終了コードを返す。
    responsibilities:
      - オフライン検証 4 種の実装と judge(FR-3.1〜FR-3.4)
      - 44.1k/48k 両対応の検証マトリクス(FR-2.1)
    depends_on:
      - component: PitchShifter
        interaction: 検証対象の処理呼び出し
        style: sync
    dependents: []
    external_dependencies: []
    entities:
      - name: VerificationReport
        identifier: caseName
        attributes: [fs, testName, measured, expected, tolerance, passed]
  - name: WasmBridge
    summary: PitchShifter を WASM に公開する Emscripten 接着層(C++)
    behaviour: >
      extern "C" の平坦な API(create/destroy/prepare/process/setParam/getLatencyMs)を公開する。
      process はリングバッファ渡しの float ポインタで、JS 側とは HEAPF32 上の固定領域を共有。
      例外を JS 境界に漏らさない。
    responsibilities:
      - C++ コアの WASM エクスポート(NFR-5)
      - JS との共有メモリレイアウト管理
    depends_on:
      - component: PitchShifter
        interaction: 実処理の委譲
        style: sync
    dependents:
      - component: DemoWorklet
        interaction: WASM モジュールとして読み込み呼び出す
    external_dependencies:
      - name: Emscripten (emsdk)
        kind: other
        purpose: C++ から WASM へのビルドツールチェーン(ビルド時のみ)
    entities: []
  - name: DemoWorklet
    summary: AudioWorkletProcessor(JS)。レンダ量子ごとに WASM を駆動
    behaviour: >
      128 サンプルのレンダ量子ごとに入力を HEAPF32 に書き、WasmBridge の process を呼び、
      出力をコピーする。port.onmessage でパラメータ変更を受けて setParam を呼ぶ
      (レンダ量子間で配送されるため音声処理と競合しない)。開始時と 1 秒ごとに
      {type:"latency", dspLatencyMs} を port.postMessage する(カウンタ 1 個のみの負荷)。
    responsibilities:
      - 音声スレッドでの WASM 駆動(FR-4.1)
      - パラメータメッセージの適用(FR-1.5、US1.2)
      - 遅延設計値の報告(US1.4)
    depends_on:
      - component: WasmBridge
        interaction: 処理・パラメータ設定・遅延取得
        style: sync
    dependents:
      - component: DemoUI
        interaction: AudioWorkletNode として接続・メッセージ交換
    external_dependencies: []
    entities: []
  - name: DemoUI
    summary: メインスレッド JS + 静的 HTML。取得・接続・操作・表示
    behaviour: >
      機能検出(AudioWorklet/WASM/getUserMedia)→ getUserMedia(EC/NS/AGC 無効)→
      AudioContext 構築 → Worklet ロード → グラフ接続。スライダー input で
      port.postMessage、左右連動トグル、状態表示(role=status)、エラー表示(role=alert)、
      遅延内訳表示(出力 = baseLatency+outputLatency、ブロック = 128×2/fs、DSP = Worklet 報告値)。
      外部送信なし・外部リソース読み込みなし。
    responsibilities:
      - デモの起動・停止・エラー復帰(US1.1、US1.3、FR-4.1〜FR-4.4)
      - パラメータ UI と遅延表示(US1.2、US1.4)
      - プライバシー不変条件の保持(NFR-6)
    depends_on:
      - component: DemoWorklet
        interaction: ノード接続と postMessage
        style: async
    dependents: []
    external_dependencies:
      - name: Web Audio API / getUserMedia
        kind: other
        purpose: 音声入出力(ブラウザ標準 API)
    entities:
      - name: UIState
        identifier: state
        attributes: [state, linkLR, latencyBreakdown]
```

## 人間可読ビュー

| コンポーネント | 言語/場所 | 役割 | 依存 |
|---|---|---|---|
| PitchShifter | C++17 / `dsp/` | DSP コア(唯一のビジネスロジック) | なし |
| VerificationHarness | C++17 / `tests/` | オフライン数値検証 | PitchShifter |
| WasmBridge | C++ / `web/src/` | WASM エクスポート | PitchShifter |
| DemoWorklet | JS / `web/` | 音声スレッド駆動 | WasmBridge |
| DemoUI | JS+HTML / `web/` | 取得・操作・表示 | DemoWorklet |

データフロー: マイク → DemoUI(グラフ構築)→ DemoWorklet → WasmBridge → PitchShifter → 出力。
パラメータ: DemoUI → postMessage → DemoWorklet → setParam → PitchShifter(atomic → 平滑化)。

出典: Q1〜Q3(本ステージ質問票)、requirements.md、stories.md、team-practices.md(レイヤ分離)。

## Assumptions & Open Questions

None.

## Review

**Reviewer:** aidlc-architecture-reviewer-agent
**Date:** 2026-08-27T17:11:55Z
**Iteration:** 1
**Verdict:** READY

### Findings

| ID | Severity | Location | Finding | Required action | Status |
|---|---|---|---|---|---|
| R-01 | Minor | aidlc/spaces/default/intents/260827-prism-pitch-shifter/inception/domain-design/components.md > entities ShifterParams and VerificationReport | Entity identifiers in YAML (identifier="name" for ShifterParams, identifier="caseName" for VerificationReport) do not correspond to declared attributes; domain-design schema requires identifier to be one of the listed attribute names | Clarify in Functional Design: either (1) rename attributes to include these identifiers, or (2) document why these entities use conceptual identifiers rather than attribute-based ones for configuration/report objects | New |

### Summary

Domain design is architecturally sound. Five components with clear single responsibilities, acyclic dependency chain, unambiguous entity ownership, and comprehensive traceability to all 28 upstream requirements (FR/NFR/US). Design decisions well-grounded in hard constraints from CLAUDE.md (latency ≤10ms, no FFT, no allocations in audio path) and resolved from prior stage reviews. One minor entity modeling consistency issue does not block implementation — developers have sufficient clarity to build. Ready to proceed to construction planning and units generation.


