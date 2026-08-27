# 要件定義 — prism (Prism Earring)

## 機能要件(FR)

### FR-1 ピッチシフト処理

- FR-1.1: 入力音声を実時間でピッチシフトする。方式はディレイライン型(リングバッファ + 2 読み出しポインタを半周ずらして走らせ等パワークロスフェード)。音声経路に FFT・位相ボコーダを使用しない。[desc]
- FR-1.2: シフト量は左右チャンネル独立の `shift_cents_L` / `shift_cents_R`(範囲 -150〜0 セント、既定 -89)。セント→速度比変換は `ratio = 2^(cents/1200)` で行い、半音=100 セントの決め打ちをしない。[desc]
- FR-1.3: `dry_wet`(0.0〜1.0、既定 1.0)で原音とのミックス比を制御する。[desc]
- FR-1.4: `crossfade_ms`(10〜100、既定 50)でディレイライン窓長を制御する。[desc]
- FR-1.5: 全パラメータは動作中に変更可能(`std::atomic` 経由)。変更値は per-sample の指数平滑(時定数 ~20ms)を経て適用し、ズィッパーノイズを防ぐ。atomic 読み取りは処理ブロック頭で 1 回。[Q3] [desc]

### FR-2 入出力形式

- FR-2.1: 初期化時にサンプルレート fs と最大ブロック長を受け取り、任意の fs(少なくとも 44.1kHz / 48kHz)で正しく動作する。内部計算はすべて fs 基準。[Q1]
- FR-2.2: 内部処理はステレオ(2ch)。モノラル入力は L=R に複製して処理する。[Q2]
- FR-2.3: 処理 API は非インタリーブ float ポインタ(`const float* const* in, float* const* out, int numFrames`)のブロック処理。[Q2]

### FR-3 オフライン検証ハーネス

- FR-3.1: 正弦波(110/440/3520 Hz)を処理し、出力の FFT ピーク周波数から入出力周波数比を算出できる(検証側 FFT はオフライン限定)。[desc]
- FR-3.2: インパルス応答から処理部遅延をサンプル数で計測できる。[desc]
- FR-3.3: 連続正弦波出力の不連続点(グリッチ)を検出できる。[desc]
- FR-3.4: 処理時間 / 実時間比(CPU 負荷余裕)を計測できる。[desc]

### FR-4 Web デモ

- FR-4.1: 静的ページ 1 枚で動作する(ビルド生成物込み)。マイク入力(getUserMedia)→ WASM 化した DSP コア(AudioWorklet 内)→ 音声出力。[Q8(intent-capture)]
- FR-4.2: ワイヤーフレームどおりの操作系: 開始/停止、shift_cents_L/R スライダー + 左右連動トグル、dry_wet・crossfade_ms スライダー、遅延表示(内訳付き)。[Q8(intent-capture)]
- FR-4.3: getUserMedia は echoCancellation / noiseSuppression / autoGainControl を無効化して取得する。[Q3(feasibility)]
- FR-4.4: マイク拒否時は許可手順の案内と再試行ボタンを表示。非対応ブラウザには対応ブラウザを案内。[Q2(rough-mockups)]

## 非機能要件(NFR)

- NFR-1 レイテンシ: DSP 処理部の追加遅延は 10ms 以下(ネイティブ最終形態の必達値。crossfade_ms 既定 50ms 時の内部遅延を含めた設計値をこの範囲に収める)。Web デモの往復遅延は測定・表示のみ(目標値なし)。[desc] [Q3(feasibility)]
- NFR-2 リアルタイム安全性: 処理コールバック内でヒープ確保/解放・ロック・I/O・システムコール・例外を行わない。バッファは初期化時確保・固定長。[desc]
- NFR-3 ピッチ精度: 既定パラメータで入出力周波数比 0.95 ± 0.5%(110/440/3520 Hz)。[desc]
- NFR-4 連続性: 連続正弦波で出力に不連続(グリッチ)なし。[desc]
- NFR-5 可搬性: DSP コアは純 C++17・依存ゼロ。macOS clang++ と Emscripten の両方でそのままコンパイル可能。[desc] [Q1(feasibility)]
- NFR-6 プライバシー: 音声データをデバイス外に送信しない。外部 CDN・解析スクリプトなし。[Q2(feasibility)]

## トレーサビリティ

すべての FR/NFR は intent-statement(課題・成功指標)、scope-document(In 境界)、team-practices(検証 4 項目のマージ条件)に遡る。ID は後続ステージ(ユーザーストーリー・設計・テスト)から参照される。

## Assumptions & Open Questions

- 帯域一定比率・左右差の未測定は既知の前提(A-01/A-02、raid-log)。パラメータ可変化で吸収する。[assumption]

## Review

**Reviewer:** aidlc-product-lead-agent
**Iteration:** 1
**Verdict:** READY

### Findings

| ID | Severity | Location | Finding | Required action | Status |
|---|---|---|---|---|---|
| R-01 | Major | FR-1.5 | Time constant "~20ms" is vague — QA cannot write a deterministic test for "approximately." | Replace with measurable specification: is this 20ms ± X%, a configurable default, or a design constraint with tolerance range? Document the acceptance threshold. | New |
| R-02 | Major | FR-1.4 | Relationship between `crossfade_ms` parameter and delay-line window length undefined. Implementation cannot proceed without knowing the exact formula (e.g., window_samples = crossfade_ms × fs / 1000?). | Specify the mapping formula and confirm it delivers the 10ms latency budget when parameters are at default (50ms crossfade). | New |
| R-03 | Minor | FR-4.2 | Requirement states "ワイヤーフレームどおり" (per wireframe) but wireframe artifact is not linked. UI acceptance testing requires the wireframe as an authoritative reference. | Link the wireframe artifact or embed its specification in this document. | New |
| R-04 | Minor | NFR-1 | Web demo latency is explicitly untargeted, but no lower bound stated. Unclear whether the demo is acceptable at, say, 150ms+ round-trip latency for usability testing purposes. | Clarify: is there a usability threshold (e.g., "tolerable up to 200ms for demo purposes"), or is latency-agnostic measurement acceptable? | New |

### Summary

Requirements are implementable and testable. Core functional and non-functional requirements trace clearly to user need and success metrics. Scope boundary is well-defined. Three clarifications needed before handoff to construction: (1) smooth parameter time constant must be measurable, (2) crossfade-to-window-length formula must be explicit for latency budgeting, (3) wireframe reference for UI acceptance must be linked. These are tractable improvements, not blockers. Offline verification harness is excellently scoped with four numeric pass/fail criteria from team-practices (pitch accuracy, latency, glitch detection, CPU load). Real-time safety constraints are explicit and align with CLAUDE.md constraints.
