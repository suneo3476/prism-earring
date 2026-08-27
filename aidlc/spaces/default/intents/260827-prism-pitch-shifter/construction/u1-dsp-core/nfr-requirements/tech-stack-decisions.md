# 技術スタック決定 — u1-dsp-core

| 項目 | 決定 | 根拠 |
|---|---|---|
| 言語 | C++17 | 仕様書指定。NFR-5(clang++/emcc 両対応)[desc] |
| 形態 | ヘッダオンリー(`dsp/include/prism/PitchShifter.h`) | 依存ゼロ・組み込み容易(D-05、unit-of-work)|
| 標準ライブラリ使用 | `<atomic> <cmath> <cstdint>` + 初期化時のみ `<vector>` | SR-1.2。process 経路では vector の伸長なし(NFR-2)|
| コンパイラフラグ | `-std=c++17 -Wall -Wextra -Werror -O2` | team-practices(警告ゼロ)|
| 例外・RTTI | コアは例外を投げない(noexcept 徹底)。RTTI 不使用 | NFR-2、契約 1 |
| 乱数・時刻 | 不使用 | 決定論的処理(検証容易性)|
| ビルド | 単体ではビルド物なし(ヘッダ提供)。検証は u2、WASM 化は u3 のビルドで消費 | unit-of-work |

## Assumptions & Open Questions

None.
