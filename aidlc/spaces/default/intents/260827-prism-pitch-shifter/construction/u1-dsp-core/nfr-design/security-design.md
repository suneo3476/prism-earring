# セキュリティ・安全性設計 — u1-dsp-core

`security-requirements.md` の各 SR-x.y に対する設計解。ライブラリ Unit のため、認証・認可・暗号化・秘密管理は該当なし(攻撃面そのものを持たない)。
本書が設計するのは「不正状態・未定義動作・リアルタイム違反を構造的に作れないようにする手段」である。
出典タグ: SR-x.y は nfr-requirements、BR1.x は functional-design/rules.md、契約 n は inception/contract-design、Q1〜Q4 は本ステージ質問票。

## SD-1 攻撃面ゼロの機械的担保(SR-1.1, SR-1.2)

**設計解**: include allowlist を設計上の不変条件として固定し、機械検証可能にする。

- 許可: `<atomic>`(パラメータ受け渡し)、`<cmath>`(exp2/cos/sin/isfinite)、`<cstdint>`、`<vector>`(prepare のみ)、`<cassert>`(デバッグ assert のみ)。
- 禁止: ネットワーク・ファイル・環境変数・時刻・乱数・スレッド・プロセス API の一切。`<chrono> <random> <thread> <fstream> <iostream> <cstdlib>` を含まない。
- 検証手段: u1 は単一ヘッダ(Q4-A)なので、`#include` 行を抽出して allowlist と照合する静的チェックを u2 のビルドスクリプトに 1 行で置ける。文書だけの約束にせず、ドリフトを検出できる形にするのが設計解の要点。

**代替案とトレードオフ**:

| 代替案 | 却下理由 |
|---|---|
| allowlist を文書のみで担保 | 依存追加を検出できない。レビュー漏れが即サプライチェーン面の拡大になる |
| `-nostdinc++` でフリースタンディング化 | `exp2`/`cos`/`sin` を自前実装せねばならず、ピッチ精度 0.95±0.5%(NFR-3)の数値保証コストが跳ねる。攻撃面はすでにゼロなので得るものがない |

allowlist チェックは include の綴りに依存する素朴な手段だが、対象がヘッダ 1 枚のため導入コストはほぼゼロで、費用対効果が最も高い。

## SD-2 添字安全 — 不変条件による範囲外アクセスの排除(SR-2.1)

**設計解**: 折り返し演算を単一のヘルパに集約し、添字が範囲外になる経路を不変条件で閉じる。

```cpp
// 折り返しは全経路でこの 1 箇所を通す(cap は prepare で確定・以後不変)
inline int wrap(int i, int cap) noexcept { return i >= cap ? i - cap : i; }
// readPos の前進も同じ形(1 サンプルで cap を跨がないため単一減算で足る)
// readPos += rate; if (readPos >= cap) readPos -= cap;
```

**不変条件(設計契約)**:

| ID | 不変条件 | 成立根拠 |
|---|---|---|
| INV-1 | `0 <= writeIndex < capacitySamples` | 毎フレーム 1 回の `wrap` 前進のみ(WF-2.2.8) |
| INV-2 | `0 <= readPos < capacitySamples`(各 ch・各ヘッド) | 前進量 `rate <= 1.0`(BR1.1、cents<=0)より 1 ステップで cap を跨げない |
| INV-3 | `n = floor(readPos)` と `wrap(n+1)` が共に `[0, cap-1]` | INV-2 と `wrap` の定義から |
| INV-4 | `window_samples <= window_max <= cap - maxBlockFrames - 2` | prepare で `cap` を `window_max + maxBlockFrames + 2` 以上に確定(WF-1.3)、`crossfadeMs<=100` のクランプ(BR1.2)より `window_samples<=window_max` |

`%`(剰余)ではなく条件減算を選ぶ理由は 2 つ — 整数除算命令を音声経路から外すこと、および浮動小数の `readPos` に `fmod` を持ち込まないこと。正当性の前提は INV-2 なので、`rate <= 1.0` が破れる変更(正方向のピッチシフト対応)を将来入れる場合は `wrap` を `while` 版に戻す必要がある、と設計上の前提として明示しておく。

**代替案**: 各アクセス点で個別に境界チェックする案は、チェック漏れの検出が目視レビューに依存するため却下。折り返しを 1 箇所に集約すれば、正しさの証明対象がヘルパ 1 個と不変条件 4 本に縮む。

## SD-3 前提条件違反の扱い(SR-2.2, SR-2.3, Q3-A)

**設計解**: `process` は `noexcept`・戻り値 `void`(契約 1)なので、その場で通知する面を持たない。したがって「安全に縮退し、通知は別の面に集約する」。

| 状況 | デバッグビルド | リリースビルド | 出力 |
|---|---|---|---|
| `prepared_ == false`(prepare 前 / 失敗後) | `assert` 発火 | 縮退 | `out[0..1][0..numFrames)` をゼロ埋めして即 return(SR-2.3) |
| `numFrames > maxBlockFrames` | `assert` 発火 | `maxBlockFrames` にクランプして処理 | 先頭 `maxBlockFrames` は正常処理、残り `[maxBlockFrames, numFrames)` はゼロ埋め |
| `numFrames <= 0` | — | no-op | 書き込みなし(範囲が空) |

超過分をゼロ埋めするのは、書かずに返すと呼び出し側バッファの未初期化メモリがそのまま再生され得るため。ゼロ埋めは無音というグリッチにはなるが、未定義の内容を出力しない点で安全側であり、SR-2.3 の方針と一貫する。

**エラーの可視化(construction ガードレール「サイレント失敗は許容しない」との整合)**:
唯一のエラー通知面は `prepare` の `bool` 戻り値である。その先の可視化は境界層に委譲する — 契約 2 の `ps_prepare` が 0 を返し、契約 3 が `{type:"error"}` を UI に送る。`process` 自体は観測点を持てないので、ここで沈黙する代わりに**上流で必ず落ちる**構造にした。u2 の検証もゼロ埋め出力を FAIL として検出できる(無音は 4 つの数値検証すべてを落とす)。

**確保失敗の扱い**: `prepare` は `<vector>`(tech-stack-decisions)で確保するため、失敗時は `std::bad_alloc` が候補になる。例外を境界外に漏らさない方針(契約 2)に合わせ、`prepare` 内で捕捉して `false` に変換し `prepared_ = false` を確定させる。代替案の `new (std::nothrow)` + `unique_ptr<float[]>` は例外機構に一切依存しない点で優れるが、承認済み tech-stack-decisions からの逸脱になるため採らない。確保量は最大でも約 170 KB(192kHz・100ms 窓・2ch)なので、どちらの経路でも実運用上到達しない失敗モードである(下記 A-2 参照)。

## SD-4 数値安全(SR-3.1, SR-3.2, SR-3.3)

### SD-4.1 検査を制御レートに一元化(SR-3.1, SR-3.2)

音声経路は per-sample の NaN/Inf 検査を持たない(SR-3.1)。防御はすべてセッター側(制御レート)に置く。

```cpp
void setShiftCentsL(float c) noexcept {
    if (!std::isfinite(c)) return;              // 先に非有限を弾く(SR-3.2)
    shiftCentsL_.store(std::fmin(std::fmax(c, -150.0f), 0.0f),
                       std::memory_order_relaxed);   // 次にクランプ(BR1.2)
}
```

**順序が設計上の要点**: `isfinite` 検査を clamp より**前**に置く。NaN はあらゆる比較が false になるため、clamp を先に通すと実装依存の値が store され得る。この順序を不変条件として全 4 セッターに適用する。

**既知の性質(意図的に受容)**: 入力サンプルに NaN が混入した場合、リングバッファに書かれた NaN は最大 `window_max + baseOffset` サンプル(約 100ms)出力に残留する。`reset()` で消去できる。これは u3 の UI に reset 操作を露出させる設計根拠になる。per-sample 検査(SR-3.1 の却下案)なら残留しないが、192k 回/秒の分岐を音声経路に追加する対価に見合わない。

### SD-4.2 デノーマル対策(SR-3.3, Q2-A)

**発生源の網羅**: 再帰(IIR)状態を持つのは `SmoothedParam` のみ。`RingBuffer` は入力の遅延コピー(無音は厳密な 0)、線形補間・cos/sin ゲイン・dry/wet ミックスはいずれも非再帰であり、デノーマルを自己生成しない。よって**平滑器だけを守れば十分**である。これが設計解の要点で、対策範囲を 1 コンポーネントに閉じ込める。

**二重の手段**:

1. 微小 DC 加算(SR-3.3): `current = a*current + (1-a)*target + 1e-20`。定常バイアスは `1e-20/(1-a)` で、`fs=48kHz` のとき `1-a ≈ 1/960` より約 `9.6e-18` — float の相対精度 `1.2e-7` に対して 10 桁以上小さく、ピッチ精度(±0.5% ≒ ±8.7 cents)に一切影響しない。
2. 到達スナップ: `|target - current|` が閾値未満になったら `current = target` を代入する。閾値は cents 系で `1e-4` cents(ピッチ精度予算 8.7 cents に対し 5 桁の余裕)、dryWet で `1e-6`。

スナップを併用する理由は、(1) の加算が float の丸めで消えてデノーマル抑止が効かない可能性があるため。指数的に 0 へ漸近する `target-current` というデノーマル発生源そのものを構造的に断つ方が確実である。

**代替案とトレードオフ**:

| 代替案 | 却下理由 |
|---|---|
| FTZ/DAZ を FPU 制御レジスタで設定 | x86 固有イントリンシクスで「依存ゼロ・純 C++」(SR-1.2)に反し、WASM(NFR-5)には FTZ 制御が存在しない |
| `-ffast-math` に委ねる | 結合律・NaN 伝播・非正規化の扱いがコンパイラ裁量になり、数値検証(NFR-3)の再現性を損なう。さらに SR-3.2 の `isfinite` 判定が最適化で消される危険がある |

## SD-5 リアルタイム安全(SR-4.1, Q1-A, Q2-A)

### SD-5.1 スレッド間受け渡し(Q1-A)

`std::atomic<float>` を 4 パラメータ分**独立に**持ち、セッターは `store(relaxed)`、`process` はブロック頭で各 1 回 `load(relaxed)`(FR-1.5, D-03)。

```cpp
static_assert(std::atomic<float>::is_always_lock_free,
              "prism requires lock-free float atomics (SR-4.1)");
```

- **スナップショット一貫性を要求しない根拠**: 4 パラメータ間に相互不変条件がない。各値は BR1.2 で独立にクランプされ、任意の組み合わせが有効な状態である。
- **relaxed で足る根拠**: atomic 越しに publish する被参照データが存在せず、値そのものが全情報。acquire/release が守るべき先行書き込みがない。
- **却下案**: `std::atomic<ShifterParams>`(16 バイト)は多くの ABI で `is_always_lock_free` が false になり libatomic のロックにフォールバックする — WASM では特に危険で、SR-4.1 に直接反する。ロックフリー FIFO 案は満杯時の取りこぼし処理という新たな失敗モードを持ち込み、「最新の目標値だけが要る」という冪等なパラメータの性質に対して過剰。

### SD-5.2 禁止事項の構造的担保

| 禁止事項 | 担保手段 |
|---|---|
| 例外 | `process` を `noexcept` 宣言(throw すれば `terminate` なので、例外経路が残っていれば設計ではなくバグとして顕在化する) |
| ヒープ確保/解放 | `process` が触る状態を固定長配列・スカラのみに限定。`std::vector` の参照は `prepare` に閉じる(BR1.5) |
| ロック | 共有は `std::atomic<float>` のみ(SD-5.1)。mutex 型をヘッダに include しない(SD-1 の allowlist が機械的に保証) |
| I/O・システムコール | allowlist に I/O ヘッダを含めない(SD-1) |

**意図的なトレードオフ**: `assert` はデバッグビルドで `abort`/stderr 出力を伴うため、**デバッグビルドの `process` はリアルタイム安全でない**。これを受容するのは、デバッグビルドの用途が u2 のオフライン検証(実時間制約なし)に限られ、実時間で動く経路(u3 の AudioWorklet)は `NDEBUG` 付きでビルドするためである。この対応関係を u2/u3 のビルド設定の要件として引き渡す。

### SD-5.3 CPU 予算(SR-4.1, Q2-A)

per-sample の超越関数呼び出しは 4 回 — `exp2` が ch ごとに 1 回(計 2、BR1.1)、`cos`/`sin` が共有位相 θ に対し 1 組(計 2、BR1.6)。

- 見積り: `48kHz × 4 = 192k 回/秒`。1 回 20〜40 サイクルとして 4〜8 Mcycle/秒 → 3GHz で **0.15〜0.3%**。WASM が 2〜3 倍遅くても 1% 未満。
- **合格基準(u2 / Bolt B2 に引き渡す)**: コールバック実行時間 / バッファ時間 < 10%。
- **最適化は実測後に判断する**(Q2-A を一貫適用)。予備案を 2 つ文書化しておく:
  1. `exp2` の省略 — 平滑器がスナップ済み(SD-4.2)なら前回の `rate` を再利用する。整定中のみ計算する。
  2. `cos`/`sin` の回転漸化式化 — θ が線形進行するので 2 mul + 2 add に置換できる。誤差蓄積と振幅ドリフトを持ち込むが、θ は窓境界ごとに 0 へリセットされる(WF-4)ため誤差の蓄積区間は最大 1 窓(100ms = 4800 サンプル)に限定される。
- どちらも計測で必要性が示されるまで実装しない。計測前の最適化は、検証済みの数値挙動(NFR-3)を理由なくリスクに晒す。

## SD-6 公開面の最小化による境界安全(SR-2.1 の補強, Q4-A)

公開面は `prism::PitchShifter` 1 型・7 メソッドのみ(契約 1)。`RingBuffer`/`ReadHead`/`SmoothedParam` は private な入れ子として実装し外部に見せない(詳細は `logical-components.md`)。
これは可読性の話ではなく境界安全の設計解である — 内部型を公開しなければ、外部から INV-1〜INV-4 を破る状態を**構築できない**。不正状態の到達経路が 7 メソッドの引数検証(BR1.2, SD-3)に限定され、検証すべき入力面が有限かつ小さくなる。

## Assumptions & Open Questions

- A-1: emcc(WASM)の `std::exp2`/`std::cos`/`std::sin` のコストが x86 ネイティブの 2〜3 倍以内に収まると仮定する。SD-5.3 の予算は 1% 未満なので 10 倍でも予算内だが、実測は Bolt B2 の CPU 計測で確認する。
- A-2: `prepare` の確保失敗は実運用上到達しないと仮定する(最大約 170 KB = 192kHz・100ms 窓・2ch・float)。u3 の emcc ビルドが `-fno-exceptions` を使う場合 SD-3 の `catch` は不活性になり確保失敗は `abort` になるが、この確保量では現実的な失敗モードでない。
- A-3: `prepare`/`reset` は音声スレッドが停止している間、または音声スレッド自身から呼ばれると仮定する(契約 1 は同時呼び出しを禁止していないが、`prepared_` とバッファはスレッド間で保護されない)。この前提を契約 1 の事前条件として明文化することを推奨する — 詳細と根拠は `logical-components.md` の LC-7 に記載。
