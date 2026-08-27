# 発見されたルール — prism (Prism Earring)

project.md へ昇格する硬い制約。出典は仕様書(CLAUDE.md)とインタビュー確認。

## Mandated(必須)

- ALWAYS ピッチシフトはディレイライン型(リングバッファ + 2 読み出しポインタ + クロスフェード)で実装する。音声経路に FFT・位相ボコーダを使わない(検証側 FFT はオフライン限定で可)。[Q3] [desc]
- ALWAYS シフト量・dry_wet・crossfade_ms は実行時可変パラメータとして実装し、`std::atomic` で受け渡す。シフト量は左右独立(-150〜0 セント、既定 -89)。半音=100 セントの決め打ちをしない。[desc]
- ALWAYS オーディオバッファは初期化時に確保し、以後サイズを変えない。[desc]
- ALWAYS マージ条件として 4 つの数値検証(ピッチ 0.95±0.5% @ 110/440/3520 Hz、処理部遅延 ≤10ms、グリッチゼロ、CPU 余裕)の緑を要求する。[Q3]
- ALWAYS 音声データはデバイス外に送信しない(ネットワーク送信なし、外部 CDN・解析スクリプトなし)。[Q4]

## Forbidden(禁止)

- NEVER オーディオコールバック内でヒープ確保/解放(new, delete, malloc, free, std::vector 伸長, std::string)を行わない。[desc] [Q5]
- NEVER オーディオコールバック内でロック(mutex, lock_guard)を取らない。[desc] [Q5]
- NEVER オーディオコールバック内でファイル I/O・ログ出力・printf・システムコール・例外送出を行わない。[desc] [Q5]
- NEVER DSP コア(`prism::PitchShifter`)にフレームワーク(JUCE 等)・OS API への依存を持ち込まない。[desc] [Q5]

## Assumptions & Open Questions

None.
