# フロントエンド構成 — u3-web-demo

DemoUI(メインスレッド JS + 静的 HTML)のコンポーネント階層・状態・検証・統合点。
functional-spec.md の WF・状態機械に対応。refined-mockups の DOM 仕様に準拠。

## コンポーネント階層

```
MainPage (index.html + main.js)
├── StatusLine        … role=status。現在状態(停止中/動作中/マイク拒否/非対応)を表示
├── ToggleButton      … 開始/停止トグル(WF-1/WF-2)
├── ControlsSection
│   ├── SliderShiftL      … shiftCentsL(id=0)
│   ├── SliderShiftR      … shiftCentsR(id=1)
│   ├── LinkLRCheckbox    … 左右連動トグル(UIState.linkLR)
│   ├── SliderDryWet      … dryWet(id=2)
│   └── SliderCrossfade   … crossfadeMs(id=3)
├── LatencySection    … 出力/ブロック/DSP 内訳 + 合算(WF-6)
└── ErrorRegion       … role=alert。エラー表示 + 許可手順案内 + 再試行ボタン(WF-3)
```

## Props / State 設計

`UIState`(単一ソース、DemoUI が保持):

```yaml
UIState:
  state: enum[停止中, 動作中, マイク拒否, 非対応]   # 画面状態機械の現在値
  linkLR: boolean                                    # 左右連動トグル(既定 false)
  latencyBreakdown:
    outputMs: number      # baseLatency + outputLatency(WebAudio 報告値)
    blockMs: number       # 128 * 2 / fs * 1000
    dspMs: number         # Worklet 報告の dspLatencyMs(D-04, BR1.7)
    totalMs: number       # 3 成分合算
```

- 各コンポーネントは UIState を props 的に受け取り表示。ToggleButton/SliderX/LinkLRCheckbox/再試行ボタンが UIState と副作用(postMessage・グラフ操作)を駆動。
- StatusLine/ErrorRegion/LatencySection は UIState を読むだけの表示コンポーネント。

## インタラクションフロー(WF 参照)

| コンポーネント | イベント | 動作 | 参照 |
|---|---|---|---|
| ToggleButton | click(停止中) | WF-1 起動シーケンス | US1.1 |
| ToggleButton | click(動作中) | WF-2 停止シーケンス | US1.1 |
| SliderShiftL/R | input | `{type:"param",id,value}` 送信。linkLR 時は id=0/1 両方 | WF-5, US1.2 |
| SliderDryWet | input | `{type:"param",id:2,value}` 送信 | WF-5 |
| SliderCrossfade | input | `{type:"param",id:3,value}` 送信 | WF-5 |
| LinkLRCheckbox | change | UIState.linkLR 更新 | WF-5 |
| 再試行ボタン | click | WF-3 復帰(getUserMedia 再要求) | US1.3 |
| (port.onmessage) | latency | UIState.latencyBreakdown 更新・再描画 | WF-6, US1.4 |
| (port.onmessage) | ready/error | 状態遷移・エラー表示 | WF-1 |

## 検証(スライダー min/max/step)

各コアパラメータ範囲(contract-summary 契約 1、FR-1.2〜FR-1.5)に一致させる:

| スライダー | id | min | max | step | 既定 |
|---|---|---|---|---|---|
| shiftCentsL | 0 | -150 | 0 | 1 | -89 |
| shiftCentsR | 1 | -150 | 0 | 1 | -89 |
| dryWet | 2 | 0 | 1 | 0.01 | 1 |
| crossfadeMs | 3 | 10 | 100 | 1 | 50 |

- UI 側で min/max にクランプ。コア側も BR1.2 で再クランプ(二重防御)、非有限 id/value は無視(契約 2)。

## 統合点

- **AudioWorkletNode.port**: UI→Worklet は `{type:"param",...}`、Worklet→UI は `{type:"ready"}` / `{type:"latency",dspLatencyMs}` / `{type:"error",message}`(契約 3)。error トリガは WASM ロード失敗 OR ps_prepare が 0(レビュー R-01)。
- **getUserMedia 制約**: `audio: {echoCancellation:false, noiseSuppression:false, autoGainControl:false}`(FR-4.3)。
- **WASM**: SINGLE_FILE 内包モジュールを AudioContext.audioWorklet.addModule でロード(Q3-A)。extern "C" API(ps_create/prepare/io_ptr/process/set_param/latency_ms、契約 2)は Worklet 内から呼ばれ UI は直接触れない。
- **プライバシー**: 外部 CDN・解析・音声外部送信なし(NFR-6)。

## Assumptions & Open Questions

None.
