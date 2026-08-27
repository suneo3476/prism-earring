# ユーザーストーリー — prism

ペルソナは P1(saku)のみ。US1.x = デモ利用、US2.x = 技術(DSP・検証)。

依存関係: US2.2 は US2.1 に依存。US1.1〜US1.4(デモ)は US2.1 の完了に依存し、US2.2 の全検証緑を開始条件とする(delivery-planning に反映)。

## US1.1 デモを開始して補正音声を聞く

P1 として、静的ページを開いて開始ボタンを押すだけで、マイクの音が -89 セント補正されてイヤホンから聞こえるようにしたい。セットアップの手間なく効果を試したいからだ。

依存: US2.1(DSP コア)。

受け入れ条件:
- ページを開いて開始 → マイク許可 → 音が出るまでに追加のインストール・設定が不要(FR-4.1)
- 取得ストリームは echoCancellation / noiseSuppression / autoGainControl 無効(FR-4.3)
- 対応環境で動作: Pixel は Chrome 最新安定版、MacBook Air は Chrome 最新安定版または Safari 17+(FR-4.1、NFR-5)

## US1.2 聴きながらパラメータを合わせ込む

P1 として、音を聞きながらシフト量(左右独立または連動)・Dry/Wet・窓長をスライダーで動かし、変化が即座に滑らかに反映されてほしい。実環境音で自分の偏差に合う値を探したいからだ。

依存: US2.1(DSP コア)。

受け入れ条件:
- shift_cents_L/R(-150〜0、既定 -89)、dry_wet(0〜1、既定 1)、crossfade_ms(10〜100、既定 50)を動作中に変更できる(FR-1.2〜FR-1.5、FR-4.2)
- パラメータ変更は per-sample 一次指数平滑(時定数 20ms、許容 ±25%、63% 整定時間で測定)を経て適用され、ステップ変化直後の出力に不連続(グリッチ判定基準超え)がない(FR-1.5)
- crossfade_ms → 窓長の変換式は `window_samples = round(crossfade_ms × fs / 1000)`。ダウンシフト時の読み出し遅れ上限は `(1 − ratio) × 窓長` で、既定値(ratio ≈ 0.95、窓 50ms)なら最大 2.5ms となり NFR-1 の 10ms 予算に収まる(FR-1.4)
- 左右連動トグルで両チャンネルを同時操作できる(FR-4.2)

## US1.3 マイク拒否から復帰する

P1 として、誤ってマイクを拒否しても、許可手順の案内と再試行ボタンで復帰したい。ページ再読み込みや原因調査をしたくないからだ。

依存: US2.1(DSP コア)。

受け入れ条件:
- 拒否時に許可手順の案内と再試行ボタンが表示される(ワイヤーフレームのエラー画面と一致)(FR-4.4)
- 非対応ブラウザでは対応ブラウザ(Chrome 最新安定版 / Safari 17+)が案内される(FR-4.4)

## US1.4 遅延を確認する

P1 として、いま聞いている音の往復遅延の実測値と内訳を見たい。デモの聴感(エコー感)がどこから来るか判断したいからだ。

依存: US2.1(DSP コア)。

受け入れ条件:
- 遅延内訳が次の定義で表示される(FR-4.2、NFR-1):
  - 出力遅延 = `AudioContext.baseLatency + outputLatency`(WebAudio API 報告値、実行時取得)
  - ブロック遅延 = AudioWorklet レンダ量子(128 サンプル)× 2 ÷ fs(入出力バッファ分、実行時算出)
  - DSP 内部遅延 = 基準読み出しオフセット + 平均ポインタ遅れ(初期化時にパラメータから算出する設計値。オフライン検証 FR-3.2 の実測と一致することをテストで確認)
- 3 成分の合算値が表示される

## US2.1 DSP コアを組み込む(技術)

開発者として、`prism::PitchShifter` を依存ゼロの C++17 単一ソースとして実装し、clang++ と Emscripten の両方でそのままコンパイルしたい。Android 移植までコアを書き直したくないからだ。

依存: なし(最初に着手)。

受け入れ条件:
- ディレイライン型実装、音声経路に FFT なし(FR-1.1)
- 任意 fs 対応・ステレオ・非インタリーブ float ブロック API(FR-2.1〜FR-2.3)
- コールバック内にヒープ確保・ロック・I/O・例外なし。バッファは初期化時固定(NFR-2)
- clang++ -std=c++17 -Wall -Wextra -Werror と emcc の両方で警告ゼロでコンパイル(NFR-5)

## US2.2 数値検証を自動で回す(技術)

開発者として、オフライン検証 4 種を 1 コマンドで実行し緑/赤を判定したい。耳ではなく数値で品質を担保したいからだ。

依存: US2.1(DSP コア)。

受け入れ条件:
- ピッチ精度: 110/440/3520 Hz で入出力周波数比 0.95±0.5%(FR-3.1、NFR-3)
- レイテンシ: インパルスで処理部遅延を計測し 10ms 以下(FR-3.2、NFR-1)
- グリッチ: 連続正弦波で不連続ゼロ(FR-3.3、NFR-4)
- CPU: 処理時間/実時間比を計測・報告(FR-3.4)
- 44.1kHz と 48kHz の両方で全検証が緑(FR-2.1)
- 音声データの外部送信なし(NFR-6 — デモ実装のレビュー観点として検証)

## Assumptions & Open Questions

None.

## Review

**Reviewer:** aidlc-product-lead-agent
**Iteration:** 1
**Verdict:** READY

### Findings

| ID | Severity | Location | Finding | Required action | Status |
|---|---|---|---|---|---|
| R-01 | Critical | US1.2 | Zipper-noise AC was untestable (time constant "~20ms" vague) | Implemented: per-sample exponential smoothing with measurable time constant (20ms ±25%, settling at 63%) in AC | Resolved |
| R-02 | Major | US1.2 | Crossfade→window formula undefined for latency budgeting | Implemented: explicit formula `window_samples = round(crossfade_ms × fs / 1000)` with lag bound `(1−ratio)×window ≤ 2.5ms` in AC | Resolved |
| R-03 | Major | US1.1–US1.4, US2.x | Story dependencies on US2.1 not declared | Implemented: dependency section at artifact top; all demo stories explicitly depend on US2.1 completion; US2.2 depends on US2.1 | Resolved |
| R-04 | Major | US1.4 | Latency breakdown undefined (output, block, DSP internal components) | Implemented: three-component latency definition in AC (baseLatency+outputLatency, 128×2/fs block, DSP design value verified offline) | Resolved |
| R-05 | Minor | US1.1 | Browser spec vague ("latest stable") | Implemented: explicit browser pinning (Chrome latest stable on Pixel; Chrome latest stable or Safari 17+ on Mac) in AC | Resolved |

### Summary

All five prior findings fully resolved. Stories now specify testable acceptance criteria for smoothing time constant with measurable settling, explicit window-length formula with latency bounds, three-part latency breakdown with runtime and design-time components, and pinned browser support. Dependencies declared at top. Traceability complete (22 FR/NFR → 6 US, all OK). Mob contributions (design, developer, quality) present with no objections. Engineering can proceed without clarification.
