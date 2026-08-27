# Requirements Analysis — 質問票

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. 対応するサンプルレートは？

AudioWorklet は環境により 44.1kHz または 48kHz で動きます(Pixel は通常 48kHz)。

- A. 任意のサンプルレートに対応(初期化時に fs を受け取り、内部計算をすべて fs 基準で行う。44.1k/48k を検証対象にする)
- B. 44.1kHz 固定
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — Pixel(48kHz)と Mac(44.1k/48k 混在)の両対応が確認済みゴールのため必須。(2026-08-27T16:55:00Z, **Mode:** chat)

## Q2. チャンネル構成は？

- A. ステレオ(2ch)処理を基本とし、モノラル入力(マイクは通常 1ch)は L=R に複製して両耳へ出す。シフト量は L/R 独立
- B. モノラルのみ
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — マイクは 1ch だが出力は両耳。左右独立シフト(複聴可能性)の仕様を活かすため内部はステレオ。(2026-08-27T16:55:00Z, **Mode:** chat)

## Q3. パラメータ変更時の音の途切れ・ノイズ対策は？

スライダー操作でシフト量等を動かすと、生の値の飛びは「ズィッパーノイズ」(プチプチ音)になります。

- A. パラメータは per-sample で指数平滑(数十 ms 時定数)してから適用する。atomic 読み取りはブロック頭で 1 回
- B. 対策なし(生の値を直接使用)
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — 「聴きながら合わせ込む」ユースケースの中核操作のため必須。(2026-08-27T16:55:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- サンプルレート: 任意対応(fs を初期化時受領)。44.1k/48k を検証
- チャンネル: 内部ステレオ、モノラル入力は複製、L/R 独立シフト
- パラメータ平滑化: per-sample 指数平滑でズィッパーノイズ防止

- Looks correct
- Request changes

[Answer]: Looks correct
