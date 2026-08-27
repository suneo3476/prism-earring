**Collaborator:** aidlc-developer-agent

## Contribution

実装容易性視点のストーリー評価:

- ストーリーと Unit 境界の対応が明確(US2.1=dsp-core、US2.2=verification、US1.x=web-demo)。見積もり・順序付けが容易。
- 依存関係を明記すべき: US1.1〜US1.4(デモ)は US2.1(DSP コア)完了に依存。US2.2 も US2.1 に依存。デモは US2.2 の緑を前提に始めるのが安全。
- crossfade_ms → 窓長の変換は `window_samples = round(crossfade_ms * fs / 1000)` の単純式でよい。ダウンシフト時の読み出しポインタ遅れの上限は `(1 - ratio) * window` で、既定値(ratio≈0.95、窓 50ms)なら最大 2.5ms — 10ms 予算に収まる。
- パラメータ平滑化は per-sample 一次 IIR(係数 `a = exp(-1/(tau*fs))`、tau=20ms)で実装コストほぼゼロ。

## Positions

None.
