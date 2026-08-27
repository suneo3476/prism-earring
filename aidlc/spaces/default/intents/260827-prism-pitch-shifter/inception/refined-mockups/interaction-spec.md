# インタラクション仕様 — prism Web デモ

## 開始/停止(US1.1、US1.3)

1. `#toggle` クリック(停止中)→ 機能検出(AudioWorklet / WebAssembly / getUserMedia)。欠落時は非対応ブラウザ状態へ。
2. `getUserMedia({audio: {echoCancellation:false, noiseSuppression:false, autoGainControl:false}})` を要求。
   - 許可 → AudioContext 生成(`latencyHint: "interactive"`)、Worklet モジュールロード、グラフ接続(mic → worklet → destination)、動作中状態へ。
   - 拒否(NotAllowedError)→ マイク拒否状態へ。`#toggle` は「▶ 再試行」となり、クリックで再度 getUserMedia。
3. `#toggle` クリック(動作中)→ ストリームのトラック停止 + AudioContext close、停止中状態へ。

## スライダー(US1.2)

- `input` イベントで即時反映(ドラッグ中も連続)。`<output>` の値表示を同時更新。[Q2]
- 反映経路: メインスレッド → `AudioParam` もしくは `port.postMessage` → Worklet 内で atomic 相当の受け渡し → DSP の per-sample 平滑化(時定数 20ms)。
- 左右連動 `#link` が checked の間、`#shiftL` の操作が `#shiftR` に複製される(逆も同様)。unchecked で独立操作。
- キーボード: range 標準操作(矢印 = step、PageUp/Down = 10×step)。

## 遅延表示(US1.4)

- 動作開始時と以後 1 秒ごとに更新:
  - 出力 = `(ctx.baseLatency + (ctx.outputLatency ?? 0)) * 1000` ms
  - ブロック = `128 * 2 / fs * 1000` ms
  - DSP = Worklet から報告される設計値(基準オフセット + 平均ポインタ遅れ)ms
  - 合算を `#latTotal` に表示
- `outputLatency` 未実装ブラウザ(Safari)では出力 = baseLatency のみとし、表示に「(概算)」を付す。

## エラー表示(US1.3)

- `#error` は `role="alert"`(挿入時に読み上げ)。表示時に `#toggle` へフォーカスを戻す。
- マイク拒否文言: 「ブラウザのサイト設定でマイクを許可してから、再試行を押してください」

## Assumptions & Open Questions

None.
