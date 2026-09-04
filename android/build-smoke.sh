#!/usr/bin/env bash
#
# ホストスモーク — Android 実機もエミュレータも使わずに、エンジンのロジック
# (状態機械 + チャンネル変換 + PitchShifter 駆動)がビルドでき、期待どおり
# 動くことを確認する。JNI 層と Oboe 層は含まない(それらは実機でしか動かない)。
#
# 使い方:  ./build-smoke.sh
# 依存:    clang++ のみ(Xcode Command Line Tools / 任意の LLVM)
#
# team.md「Testing Posture」: 実装のあとにテストを書き、緑を確認してから進む。
# team.md「Code Style」:     -Wall -Wextra は -Werror で扱う。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="${SCRIPT_DIR}/app/src/main/cpp"
DSP_INCLUDE="${SCRIPT_DIR}/../dsp/include"
OUT_DIR="${SCRIPT_DIR}/build/smoke"
OUT_BIN="${OUT_DIR}/audio_bridge_smoke"

if [[ ! -f "${DSP_INCLUDE}/prism/PitchShifter.h" ]]; then
    echo "error: prism::PitchShifter が見つかりません: ${DSP_INCLUDE}/prism/PitchShifter.h" >&2
    echo "       android/ はリポジトリ直下(dsp/ と兄弟)に置いてください。" >&2
    exit 1
fi

CXX="${CXX:-clang++}"
if ! command -v "${CXX}" >/dev/null 2>&1; then
    echo "error: ${CXX} が見つかりません。" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"

echo "==> compile (${CXX} -std=c++17 -Wall -Wextra -Werror -O2)"
"${CXX}" \
    -std=c++17 \
    -Wall -Wextra -Werror \
    -O2 \
    -I "${DSP_INCLUDE}" \
    -I "${CPP_DIR}" \
    "${CPP_DIR}/audio_bridge_smoke.cpp" \
    -o "${OUT_BIN}"

echo "==> run"
"${OUT_BIN}"
