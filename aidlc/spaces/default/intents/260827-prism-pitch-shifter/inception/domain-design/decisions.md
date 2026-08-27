# 設計判断(ADR)— prism

## D-01 ディレイライン型ピッチシフタ(確定事項の再掲)

- 決定: リングバッファ + 2 読み出しポインタ(窓半周ずらし)+ 等パワークロスフェード。音声経路に FFT・位相ボコーダ不使用。
- 理由: レイテンシ最重要制約(仕様書)。シフト量 -89 セントは速度比 0.95 で 1.0 に近く、本方式の弱点(金属的響き)がほぼ出ない。
- 帰結: 内部遅延は窓長依存で数 ms。`window_samples = round(crossfade_ms × fs / 1000)`、読み出し遅れ上限 `(1−ratio)×窓長`(既定で最大 2.5ms)。

## D-02 補間は線形(必要になったら 4 点 Hermite に差し替え)

- 決定: 読み出しの小数位置補間は線形補間とする。
- 理由: 速度比が 1 に極めて近く、線形補間の高域減衰・歪みは実用上無視できる。演算・分岐最小。
- 帰結: ピッチ精度検証(0.95±0.5%)が万一不合格なら Hermite に切替(補間関数を差し替え可能な内部構造にする)。

## D-03 パラメータ同期: postMessage → std::atomic → ブロック頭 load → per-sample 平滑化

- 決定: UI からは port.postMessage(レンダ量子間配送)。コアのセッターは std::atomic<float> に store。process() はブロック頭で 1 回 load し、per-sample 一次 IIR(係数 a = exp(−1/(0.020×fs)))で平滑化して適用。
- 理由: コア API をプラットフォーム非依存に保つ(AudioParam は Web 専用)。ネイティブ(Android/Oboe)移植時に別スレッドから同じセッターを呼んでも安全。ロックフリーで NFR-2 を満たす。refined-mockups レビュー R-01 の解決。
- 帰結: WASM 単一スレッドでは atomic は実質無コスト。時定数 20ms(±25% 許容、63% 整定で測定)。

## D-04 遅延報告: {type:"latency", dspLatencyMs} を 1 秒ごとに postMessage

- 決定: DemoWorklet が開始時と以後 1 秒ごと(レンダ量子カウンタで判定)にコアの getLatencySamples()/fs×1000 を送出。
- 理由: 音声スレッド負荷はカウンタ 1 個。UI 側ポーリング不要。refined-mockups レビュー R-02 の解決。
- 帰結: DSP 内部遅延の設計値 = 基準読み出しオフセット + 平均ポインタ遅れ((1−ratio)×窓長/2)。オフライン検証 FR-3.2 の実測値と一致することをテストで確認(US1.4)。

## D-05 WASM ブリッジは extern "C" 平坦 API + HEAPF32 共有

- 決定: embind を使わず extern "C"(create/destroy/prepare/process/setParam/getLatencyMs)。入出力は初期化時に確保した HEAPF32 上の固定領域を共有。
- 理由: embind は生成コード・オーバーヘッドが大きい。平坦 API は Android JNI 移植時の写像も素直。レンダ量子ごとのメモリ確保ゼロ。
- 帰結: setParam はパラメータ ID enum(0=shiftL,1=shiftR,2=dryWet,3=crossfadeMs)で受ける。

## D-06 モノラル入力の複製は DemoWorklet で実施

- 決定: マイク 1ch 入力の L=R 複製は Worklet 側で行い、コアは常に 2ch を処理する。
- 理由: コアの API を単純に保つ(FR-2.2)。

出典: Q1〜Q3、D-01〜D-02 は [desc] と requirements、D-03〜D-04 は refined-mockups レビュー指摘の解決。

## Assumptions & Open Questions

None.
