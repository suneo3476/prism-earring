# Rough Mockups — 質問票

## Sources

- [desc] Initial description: "Prism Earring: real-time -89 cent pitch shifter hearing aid. Build the JUCE-independent C++ DSP core (prism::PitchShifter, delay-line pitch shifter with dual crossfading read pointers), offline verification tests (pitch accuracy via FFT at 110/440/3520 Hz, latency <=10ms, glitch detection), and a JUCE 8 standalone app (mic -> shift -> output) built on macOS. Full spec and hard constraints are in CLAUDE.md at repo root: latency dominates all decisions, no phase-vocoder/FFT processing in the audio path, no allocations/locks in the audio callback, runtime-variable parameters shift_cents_L/R (-150..0, default -89), dry_wet, crossfade_ms."
- [scope] Workflow-selected scope: `mvp`.

## Q1. デモ画面に必要な操作・表示要素はどれですか？(select all that apply)

- A. 開始/停止ボタン(マイク許可の取得と AudioContext の起動)
- B. スライダー: shift_cents_L / shift_cents_R(-150〜0、既定 -89)+ 左右連動トグル
- C. スライダー: dry_wet(0〜1、既定 1.0)、crossfade_ms(10〜100、既定 50)
- D. 表示: 実測往復遅延(ms)と動作状態
- E. 該当なし (Not applicable)
- X. Other (please specify)

[Answer]: A, B, C, D — 仕様書のパラメータ表と feasibility Q3(遅延を測定・表示)をそのまま UI に写像。左右連動トグルは左右差未確認(A-02)の探索用。(2026-08-27T16:45:00Z, **Mode:** chat)

## Q2. 利用フローの想定は？

- A. ページを開く → 開始を押す → マイク許可 → イヤホンで聴きながらスライダーで合わせ込む → 停止
- B. 事前設定ウィザード付き
- C. まだ定義されていない (Not yet defined)
- X. Other (please specify)

[Answer]: A — 「セットアップが楽」の確認済み方針どおり最短フロー。(2026-08-27T16:45:00Z, **Mode:** chat)

## Consolidated Summary Confirmation

回答の要約:

- 画面要素: 開始/停止、シフト量 L/R スライダー(連動トグル付き)、dry_wet・crossfade_ms スライダー、遅延・状態表示のみ
- フロー: 開く → 開始 → マイク許可 → 聴きながら調整 → 停止(1 画面完結)

- Looks correct
- Request changes

[Answer]: Looks correct
