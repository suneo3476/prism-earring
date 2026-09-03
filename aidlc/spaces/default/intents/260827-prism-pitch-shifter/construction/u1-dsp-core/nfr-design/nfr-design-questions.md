# NFR Design — 質問票(u1-dsp-core)

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

ライブラリ Unit のため、Web 系 NFR 設計(キャッシュ階層・水平スケール・サーキットブレーカ・分散トレース)は該当なし。
設計上の選択が残っているのは「リアルタイム経路の安全設計」と「境界の可視性」の 4 点に限られる。

## Q1. セッター→process 間のパラメータ受け渡し(atomic の型・メモリオーダー・一貫性)をどう設計するか？

対象: SR-4.1(process 経路にロックなし)、FR-1.5 / D-03(ブロック頭で 1 回 load)。

- A. `std::atomic<float>` を 4 パラメータ分それぞれ独立に持ち、セッターは `store(memory_order_relaxed)`、process はブロック頭で各 1 回 `load(memory_order_relaxed)`。`static_assert(std::atomic<float>::is_always_lock_free)` でロックフリー性をコンパイル時に保証する。4 パラメータ間にスナップショット一貫性は要求しない(相互不変条件が無く、各値は独立にクランプ済み)
- B. 4 パラメータを 1 つの構造体にまとめ `std::atomic<ShifterParams>` として丸ごと store/load し、スナップショット一貫性を得る
- C. ロックフリー SPSC FIFO にパラメータ変更イベントを積み、process が毎ブロック排出する
- X. Other (please specify)

[Answer]: A — 4 パラメータ間に相互不変条件が無い(BR1.2 で各々独立にクランプされ、組み合わせで不正になる領域が存在しない)ため、スナップショット一貫性は買う価値のない性質。B は 16 バイト構造体の atomic となり多くの ABI で `is_always_lock_free` が false になって libatomic のロックにフォールバックする(WASM では特に危険)ので、SR-4.1 に直接反する。C は FIFO 満杯時の取りこぼし処理という新たな失敗モードを持ち込み、「最新値だけが要る」というこのパラメータの性質(冪等な目標値)に対して過剰。relaxed で足る根拠は、atomic 越しに publish する被参照データが無く、値そのものが全情報であること(acquire/release が守るべき先行書き込みが存在しない)。(2026-09-03T15:30:19Z, **Mode:** chat, ユーザー委任により lead が推奨案を選択)

## Q2. 音声経路の数値安定性(デノーマル)と CPU コスト(per-sample の `2^(c/1200)` 評価)の方針は？

対象: SR-3.3(デノーマル対策)、SR-4.1(CPU 余裕)、BR1.1 / BR1.3(cents を平滑してから per-sample に rate 化)。

- A. デノーマル対策は純 C++ 手段のみ — 平滑器の再帰状態に微小 DC(1e-20)を毎サンプル加算(SR-3.3)し、併せて `|target-current|` が閾値未満になった時点で `current = target` へスナップして再帰差分がデノーマル域へ落ちる経路自体を閉じる。`2^(c/1200)` はまず `std::exp2` を素直に毎サンプル・ch ごとに呼び、Bolt B2 の CPU 計測で余裕を確認する。余裕不足が実測された場合の予備案(整定時は前回値をキャッシュして exp2 を省く)を設計文書に併記するが、先行実装はしない
- B. FTZ/DAZ を FPU 制御レジスタで設定し(`_MM_SET_DENORMALS_ZERO_MODE` 等)、`exp2` は自前の多項式近似に最初から置き換える
- C. `-ffast-math` / `-funsafe-math-optimizations` をコンパイラに委ね、コード側では何もしない
- X. Other (please specify)

[Answer]: A — B の FPU 制御レジスタ操作は x86 固有イントリンシクスであり、u1 の「依存ゼロ・純 C++」(SR-1.2、tech-stack-decisions)と WASM ターゲット(NFR-5)の両方に反する(WASM に FTZ 制御は無い)。C は結合律・NaN 伝播・非正規化の扱いをコンパイラ裁量に落とすため、ピッチ精度 0.95±0.5% を数値で検証する本件(NFR-3)で再現性を損ない、かつ SR-3.2 の「非有限値を無視」判定を最適化で消される危険がある。exp2 の実コストは 48kHz×2ch = 96k 回/秒 程度で現代 CPU の 1% 未満と見積まれ、B の近似置換は計測前の最適化にあたる。スナップ併用を A に含めたのは、1e-20 加算だけでは加算のたびに丸めで消える可能性があり、デノーマル発生源(指数的に 0 へ漸近する `target-current`)を構造的に断つ方が確実なため。(2026-09-03T15:30:19Z, **Mode:** chat, ユーザー委任により lead が推奨案を選択)

## Q3. 失敗モード(prepare 前/失敗後の process 呼び出し、numFrames 超過)をどう扱い、誰に見せるか？

対象: SR-2.2、SR-2.3、契約 1(`process` は noexcept・戻り値 void)、construction ガードレール「サイレント失敗は許容しない」。

- A. `prepared_` フラグを持ち、(1) 未初期化 process は out を両 ch ゼロ埋めして即 return、(2) `numFrames > maxBlockFrames` はデバッグビルドで assert・リリースでは maxBlockFrames にクランプして処理、(3) `numFrames <= 0` は no-op、(4) prepare 失敗時は `prepared_ = false` を確定させ以後もゼロ埋め。エラー通知面は `prepare` の bool 戻り値のみとし、可視化は境界層の責務(契約 2 の `ps_prepare` → 0、契約 3 の `{type:"error"}`)として設計文書で明示する
- B. 前提条件違反で例外を投げる
- C. 前提条件違反は呼び出し側の責務とし未定義動作を許容する(契約プログラミング)
- X. Other (please specify)

[Answer]: A — B は契約 1 の `noexcept` と BR1.5 に直接反し、リアルタイムスレッドでの巻き戻しコストも許容できない。C は SR-2.1〜2.3 が明示的に否定している(未定義動作にしない)。「サイレント失敗は許容しない」ガードレールとの整合は、失敗の *通知* を `prepare` の bool 一点に集約し、その先の可視化を u3 の境界層(`{type:"error"}` メッセージ)と u2 の検証(ゼロ埋め出力を FAIL として検出可能)に委譲することで満たす — process 自体は観測点を持てないので、ここで沈黙する代わりに上流で必ず落ちる構造にする。デバッグビルドの assert が開発時の即時検出を担う。(2026-09-03T15:30:19Z, **Mode:** chat, ユーザー委任により lead が推奨案を選択)

## Q4. 論理コンポーネント(RingBuffer / ReadHead / Crossfader / ParameterSmoother)を物理的にどう配置し、内部型を u2/u3 にどこまで見せるか？

対象: 契約 1(ヘッダオンリー、公開面は `prism::PitchShifter` のみ)、D-05、team-practices のレイヤ分離。

- A. 単一ヘッダ `dsp/include/prism/PitchShifter.h` に収め、内部コンポーネントは `PitchShifter` クラス内の private な入れ子構造体(または `prism::detail` 名前空間)として実装する。u2/u3 から見える境界は「1 ヘッダを include して `prism::PitchShifter` を使う」だけ。論理コンポーネントは文書上の責務区分として `logical-components.md` に記述し、物理ファイルは分割しない
- B. コンポーネントごとにヘッダを分割(`RingBuffer.h`, `ReadHead.h`, …)し、すべて public include にする
- C. pimpl イディオムで宣言と実装を分け、`.cpp` を持つ静的ライブラリにする
- X. Other (please specify)

[Answer]: A — B は内部型を契約面に昇格させてしまい、u2/u3 が内部構造に依存できるようになる(契約 1 の「公開面は PitchShifter のみ」が形骸化し、u1 のリファクタが u2/u3 の変更を強制する)。C はビルド成果物とビルドシステムを u1 に持ち込み、tech-stack-decisions の「単体ではビルド物なし(ヘッダ提供)」および u3 の emcc 一発ビルドと衝突する。A のトレードオフは、単一ファイルが長くなる(400 行規模)ことと、内部コンポーネントを直接単体テストできないことの 2 点。後者は u2 が公開面のみの黒箱検証で 4 つの数値検証(ピッチ精度・遅延・グリッチ・CPU)を賄える(入出力ベースの検証なので内部への到達は不要)ため受容する。(2026-09-03T15:30:19Z, **Mode:** chat, ユーザー委任により lead が推奨案を選択)

## Consolidated Summary Confirmation

回答の要約:

- パラメータ受け渡しは `std::atomic<float>` 4 本を独立に relaxed store/load(`is_always_lock_free` を static_assert)。スナップショット一貫性は要求しない [Q1]
- デノーマルは純 C++ 手段(平滑器状態への 1e-20 加算 + 到達スナップ)で対処。FTZ レジスタ操作と `-ffast-math` は使わない。`2^(c/1200)` は `std::exp2` を素直に毎サンプル呼び、最適化は B2 の CPU 実測後に判断する [Q2]
- 失敗モードは「ゼロ埋め + デバッグ assert + リリースはクランプ」。エラー通知面は `prepare` の bool 戻り値のみ、可視化は u3 の境界層と u2 の検証に委譲する [Q3]
- 論理コンポーネントは単一ヘッダ内の private 入れ子として実装し、u2/u3 から見える境界は `prism::PitchShifter` 1 型のみ。物理ファイルは分割しない [Q4]

- Looks correct
- Request changes

[Answer]: Looks correct
