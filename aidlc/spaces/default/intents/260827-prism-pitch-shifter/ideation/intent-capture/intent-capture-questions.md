# Intent Capture — 質問票

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. このプロジェクトが解決する課題は何ですか？

開発の出発点となる課題を確認します。以後のすべての要件がここに遡ります。

- A. 開発者本人の音高知覚の偏差(すべての音が約89セント高く聞こえる、15年来一貫)を補正し、外界を正しい音高で聞けるようにする
- B. 音楽制作向けの汎用ピッチシフタを提供する
- C. 聴覚保護・騒音低減
- D. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — 実測値: 440Hz 純音が A♯ に聞こえ、418Hz(比 0.95、約 -88.8 セント)で正しく A に聞こえる。日による変動なし。(2026-08-27T16:31:11Z, **Mode:** chat — ユーザー提供の仕様書 CLAUDE.md「背景」より抽出)

## Q2. 対象顧客(受益者)は誰ですか？

- A. 開発者本人のみ(単一ユーザーの自家用補装具)
- B. 同様の症状を持つ一般ユーザー(製品として配布)
- C. まず開発者本人、効果が実証されたら将来的に一般配布を検討
- D. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — 本ワークフローの範囲では開発者本人が唯一の顧客・利用者。最終形態は本人所有の Android 端末向けアプリ。(2026-08-27T16:31:11Z, **Mode:** chat — 仕様書の背景・目的および会話文脈より抽出)

## Q3. 成功をどう測りますか？(select all that apply)

- A. 数値検証: 入出力周波数比が 0.95 ± 0.5%(110/440/3520 Hz 正弦波、FFT 検定)
- B. 数値検証: 処理部レイテンシ 10ms 以下(インパルス応答で計測)
- C. 数値検証: 連続正弦波で出力に不連続(グリッチ)がない
- D. 本人の聴感: 実環境音で正しい音高に聞こえ、会話が破綻しない
- E. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A, B, C, D — 仕様書「検証方法」が数値検証 4 項目(ピッチ精度・レイテンシ・グリッチ・CPU 負荷)を明記し、「耳で確認するだけでは不十分」とする。聴感確認は実機で本人が行う。(2026-08-27T16:31:11Z, **Mode:** chat)

## Q4. この取り組みを今始めるきっかけは何ですか？

- A. 15年来の症状に対し、リアルタイム DSP による自作補装具が技術的・コスト的に現実的になったため
- B. 市場圧力・競合
- C. 規制対応
- D. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — 併せて AI-DLC v2 開発手法の試験導入も動機のひとつ(ユーザー発言「せっかくなので AI-DLC v2 を試してみたい」)。(2026-08-27T16:31:11Z, **Mode:** chat)

## Q5. 主要なステークホルダーと意思決定者は誰ですか？

- A. 開発者本人のみ — 顧客・開発者・意思決定者を兼ねる個人プロジェクト
- B. 複数の関係者がいる(別途列挙)
- C. 特定されていない (Not identified)
- X. Other (please specify)

[Answer]: A — スコープ・優先度の決定権はすべて開発者本人。本セッションでは本人の包括承認に基づきオーケストレータが承認を代行。(2026-08-27T16:31:11Z, **Mode:** chat)

## Q6. 進捗報告やコミュニケーションの要件はありますか？

- A. なし — 個人プロジェクトであり、リポジトリのコミット履歴と AI-DLC の監査証跡が記録を兼ねる
- B. 定期的な進捗報告が必要
- C. 該当なし (Not applicable)
- X. Other (please specify)

[Answer]: A (2026-08-27T16:31:11Z, **Mode:** chat — ユーザーは完走後の報告のみを求めている)

## Q7. ワークフロー選択スコープ `mvp` は意図する製品境界と一致していますか？

今回のスコープ(検証寄り MVP): JUCE 非依存 DSP コア + オフライン数値検証 + JUCE スタンドアロンアプリ(macOS ビルド)。Android 版(段階3)と帯域分割(段階4)は今回の範囲外。

- A. 一致している — mvp スコープを確認する(段階1〜2 まで。Android・帯域分割は将来の別ワークフロー)
- B. 異なる境界を定義する(別途記述)
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — 仕様書「設計方針/段階」が段階的推進を明記し、帯域分割は「最初から作らない」とする。今回は段階1〜2 が対象。(2026-08-27T16:31:11Z, **Mode:** chat)。→ Q8 でデモ形態を更新(JUCE アプリは Web デモに置換)

## Q8. デモの提供形態はどうしますか？(ステージ実行中のユーザー発言による追加質問)

ユーザー発言(原文): 「とりあえず私のgoogle pixel でもこのMacBook Airでも試せるデモができるのがゴールですね　セットアップが楽だと嬉しいね　スタンドアロンにする場合は WebAssemblyとか必須だろうか？しらんけど」

- A. Web デモ — C++ DSP コアを WebAssembly にコンパイルし AudioWorklet で実行。静的ページを開くだけで Pixel(Chrome)と MacBook Air の両方で動く。JUCE スタンドアロンは今回スコープから外す
- B. JUCE macOS アプリ + 別途 Android アプリの 2 本立て
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — 「Pixel でも MacBook Air でも試せる」「セットアップが楽」を最短で満たすのは Web デモ。WASM は必須ではないが、DSP を C++ 単一ソースに保ち Android(Oboe)移植に直結させるため WASM 化を採用。ブラウザ経由の遅延(20〜60ms 程度)は最終要件未達だが、効果検証デモとしては許容(低遅延の最終形態はネイティブ Android のまま)。(2026-08-27T16:37:00Z, **Mode:** chat — ユーザー発言より抽出)

## Q9. AI 実行コストへの配慮はどうしますか？(ステージ実行中のユーザー発言による追加質問)

ユーザー発言(原文): 「当たり前だけどモデルのコストは意識しておくれよ　書くのはopus5が得意だろう？」

- A. レビュアー等のサブエージェントは安価なモデルで実行し、成果物・質問数は必要最小限に絞る
- B. コストは考慮不要
- C. 該当なし (Not applicable)
- X. Other (please specify)

[Answer]: A (2026-08-27T16:37:00Z, **Mode:** chat — 補足: "Opus 5" は存在しないモデル名。サブエージェントには Sonnet/Haiku 系を用いる)

## Consolidated Summary Confirmation

回答の要約:

- 課題: 開発者本人の約 +89 セントの音高知覚偏差を実時間ピッチシフト(-89 セント)で補正する
- 顧客: 開発者本人のみ(単一ユーザーの自家用補装具)
- 成功指標: 周波数比 0.95±0.5%、遅延 ≤10ms(処理部・ネイティブ最終形態)、グリッチなし、実環境での聴感確認
- きっかけ: 症状への自作補装具が現実的になったこと + AI-DLC v2 の試験導入
- ステークホルダー: 開発者本人が顧客・開発者・意思決定者を兼ねる
- コミュニケーション: 追加要件なし(コミット履歴と監査証跡で足りる)
- スコープ: `mvp` を確認 — JUCE 非依存 C++ DSP コア + オフライン数値検証 + **WASM/AudioWorklet Web デモ(Pixel と Mac の両ブラウザで動作、セットアップ最小)**。JUCE アプリ・Android ネイティブ・帯域分割は対象外
- 実行コスト: サブエージェントは安価なモデルで実行し、成果物は必要最小限に絞る

- Looks correct
- Request changes

[Answer]: Looks correct
