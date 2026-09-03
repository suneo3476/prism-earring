#!/usr/bin/env bash
#
# build.sh — prism_bridge.cpp(契約 2 の extern "C" API)を WASM にビルドする。
#
#   source ~/emsdk/emsdk_env.sh   # emcc を PATH に載せる
#   web/wasm/build.sh             # -> web/prism.wasm
#
# 成果物 web/prism.wasm はリポジトリにコミットする(emsdk 無しでもデモとテストが
# 動くようにするため)。再現性のためにこのスクリプトも一緒にコミットしてある。
#
# ---- ビルド構成の根拠 ----------------------------------------------------
# AudioWorklet のグローバルスコープには fetch も importScripts も無い。そこで
# Emscripten の JS グルーは一切使わず、**Worklet 側で WebAssembly.instantiate を
# 直接呼ぶ**。そのために「import ゼロ・エクスポート名が素のまま」の .wasm が要る。
#
#   * -o *.wasm(= STANDALONE_WASM)を選んだ理由:
#     非スタンドアロン(-o *.js)構成では -O3 のとき emcc が wasm の import/export
#     名を 1 文字に潰す(MINIFY_WASM_IMPORTS_AND_EXPORTS。内部設定なのでコマンド
#     ラインから無効化できず、無効化には -g3 = DWARF 同梱 330KB が必要)。
#     さらに env.__cxa_throw / env.emscripten_resize_heap / env._abort_js の
#     3 つの import が残り、ローダー側でスタブを用意する必要が出る。
#     スタンドアロン構成では **import ゼロ・エクスポート名そのまま**になり、
#     `WebAssembly.instantiate(bytes, {})` だけで動く。計測値は README を参照。
#   * WASI ランタイムには依存しない(import が 0 本なので依存しようがない)。
#     リアクタ規約に従い、インスタンス化後に `_initialize()` を 1 回呼んで
#     静的コンストラクタを走らせる(ローダーが実施)。
#   * -sALLOW_MEMORY_GROWTH=0 + 固定 INITIAL_MEMORY: オーディオスレッドで
#     メモリ拡張(= 確保とコピー)を起こさない。HEAPF32 のビューも張り替え不要。
#   * -sMALLOC=emmalloc -sFILESYSTEM=0: malloc 由来・FS 由来のコードを最小化。
#   * -fno-rtti: 未使用。-fno-exceptions は使えない(DSP コアが prepare の
#     確保失敗を try/catch で false に畳んでいるため。既定の
#     「catch 無効・throw は abort」構成のままにする)。
#   * -DNDEBUG: コアの assert を無効化する(リリース相当。SR-2.2 の
#     クランプ経路は assert とは独立に効く)。

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEB_DIR="$(dirname "$HERE")"
REPO_ROOT="$(dirname "$WEB_DIR")"

OUT="${WEB_DIR}/prism.wasm"
SRC="${HERE}/prism_bridge.cpp"
INCLUDE_DIR="${REPO_ROOT}/dsp/include"

# emcc を探す。PATH に無ければ emsdk の既定位置を順に試す。
if ! command -v em++ >/dev/null 2>&1; then
    for candidate in "${EMSDK:-}/upstream/emscripten" "${HOME}/emsdk/upstream/emscripten"; do
        if [ -x "${candidate}/em++" ]; then
            PATH="${candidate}:${PATH}"
            export PATH
            break
        fi
    done
fi

if ! command -v em++ >/dev/null 2>&1; then
    echo "em++ が見つかりません。先に 'source ~/emsdk/emsdk_env.sh' を実行してください。" >&2
    exit 1
fi

echo "emcc: $(em++ --version | head -1)"

# 契約 2 の 7 関数 + 検証ハーネス向けの 4 関数。
EXPORTS="_ps_create,_ps_destroy,_ps_prepare,_ps_reset,_ps_io_ptr,_ps_process"
EXPORTS="${EXPORTS},_ps_set_param,_ps_latency_ms,_ps_latency_samples"
EXPORTS="${EXPORTS},_ps_window_samples,_ps_sweep_samples"

em++ \
    -std=c++17 \
    -O3 \
    -Wall -Wextra -Werror \
    -DNDEBUG \
    -fno-rtti \
    -I "${INCLUDE_DIR}" \
    "${SRC}" \
    -o "${OUT}" \
    --no-entry \
    -sALLOW_MEMORY_GROWTH=0 \
    -sINITIAL_MEMORY=2097152 \
    -sSTACK_SIZE=65536 \
    -sMALLOC=emmalloc \
    -sFILESYSTEM=0 \
    -sASSERTIONS=0 \
    -sEXPORTED_FUNCTIONS="${EXPORTS}"

# emcc は出力に実行ビットを立てるが、これはデータファイルなので落とす
# (再ビルドのたびにモード変更だけの差分が出るのを防ぐ)。
chmod 644 "${OUT}"

echo "生成: ${OUT} ($(wc -c < "${OUT}" | tr -d ' ') bytes)"

# import ゼロ・必要な export が揃っていることを毎回検査する(退行検出)。
if command -v node >/dev/null 2>&1; then
    node "${HERE}/inspect.mjs" "${OUT}"
    node "${HERE}/verify-shape.mjs" "${OUT}"
else
    echo "node が見つからないため、import/export の検査はスキップしました。" >&2
fi
