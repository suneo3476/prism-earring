# Unit 依存グラフ — prism

```yaml
units:
  - name: u1-dsp-core
    kind: library
    depends_on: []
  - name: u2-verification
    kind: library
    depends_on: [u1-dsp-core]
  - name: u3-web-demo
    kind: ui
    depends_on: [u1-dsp-core]
```

## DAG(テキスト)

- u2-verification → u1-dsp-core に依存(ヘッダ include、処理呼び出し)
- u3-web-demo → u1-dsp-core に依存(WasmBridge がヘッダ include)
- u2 と u3 は相互に依存なし → **並行開発可能**(ただし品質上、u3 着手前に u2 の緑を推奨 — economic 判断は delivery-planning)

## 統合点

| 統合点 | 形式 | 消費側 |
|---|---|---|
| `dsp/include/prism/PitchShifter.h` | C++ ヘッダ(クラス API) | u2, u3 |
| extern "C" WASM API(contract-design で確定) | フラット C API + HEAPF32 共有 | u3 内部(JS ↔ C++) |
| `{type:"latency", dspLatencyMs}` メッセージ | postMessage JSON | u3 内部(Worklet ↔ UI) |

共有データベース・イベントバス・ネットワーク API はなし。

## Assumptions & Open Questions

None.
