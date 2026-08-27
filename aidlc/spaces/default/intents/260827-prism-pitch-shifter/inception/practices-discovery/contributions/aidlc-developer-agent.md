**Collaborator:** aidlc-developer-agent

## Contribution

実装規約視点の評価:

- 命名は CLAUDE.md の命名規約に従う: 名前空間 `prism`、コアクラス `prism::PitchShifter`、コード中の表記は `prism` / `PrismEarring`(`Prism Earring` は使わない)。
- レイヤ境界: DSP コア(純 C++17、ヘッダ中心)/ 検証ハーネス(オフライン、ファイル or メモリ入出力)/ プラットフォーム接着層(WASM ブリッジ、AudioWorklet JS)を厳密に分離。コアはフレームワーク・OS API を一切 include しない。
- リアルタイム制約はコンパイル時・レビュー時に機械的に確認できる形へ: コールバック経路では new/delete/malloc/mutex/IO/例外を禁止。パラメータ受け渡しは std::atomic のみ。バッファは初期化時確保・固定長。
- エラー処理: コールバック内は例外禁止のため、初期化 API で事前検証し bool / enum を返す。假定違反はデバッグビルドのみ assert。
- ファイル構成は単純に: `dsp/include/prism/PitchShifter.h`(ヘッダオンリー可)、`tests/`、`web/`。ビルドは Makefile か素の clang++ 呼び出しで十分、CMake は必須ではない。
- フォーマッタ: 既存設定なし。言語慣用(LLVM 風 4 スペース)で統一し、専用設定ファイルは置かない(導入コスト > 価値)。

## Positions

None.
