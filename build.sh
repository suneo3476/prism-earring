#!/bin/sh
# prism — ビルド + オフライン数値検証(外部ツール不要、clang++ のみ)
#
#   ./build.sh          ビルドして検証を実行(全緑で exit 0、1 件でも FAIL なら exit 1)
#   ./build.sh --quick  数値検証だけ実行(ソース検査・再現性検査を省略)
#
# 検証の内容と読み方は README.md を参照。

set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
OUT="$ROOT/build"
CXX=${CXX:-clang++}
# NDEBUG は定義しない — SR-4.2 の確保カウンタ assert を有効に保つ。
CXXFLAGS="-std=c++17 -Wall -Wextra -Werror -I$ROOT/dsp/include"
SRC="$ROOT/verify/verify.cpp"

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# SR-1.1 / SR-1.2 / SR-2.1: 検証ランナーの隔離性・依存ゼロ・決定論をソースで検査
# ---------------------------------------------------------------------------
if [ "$QUICK" -eq 0 ]; then
    printf 'source checks (SR-1.1 / SR-1.2 / SR-2.1)\n'
    fail=0
    for pat in '<fstream>' 'fopen' 'mmap' 'socket(' 'curl' '<random>' 'rand()' '<thread>' '<mutex>'; do
        if grep -n -F "$pat" "$SRC" "$ROOT/dsp/include/prism/PitchShifter.h" >/dev/null 2>&1; then
            printf '  FAIL forbidden token found: %s\n' "$pat"
            fail=1
        fi
    done
    # 音声経路に FFT を置かないこと(検証側 verify.cpp のみ可)。コメント行は対象外。
    if sed -e 's,//.*,,' "$ROOT/dsp/include/prism/PitchShifter.h" | grep -n -i 'fft' >/dev/null 2>&1; then
        printf '  FAIL FFT appears in the DSP core\n'
        fail=1
    fi
    [ "$fail" -eq 0 ] && printf '  PASS no forbidden dependency / no FFT in the audio path\n'
    [ "$fail" -eq 0 ] || exit 1
fi

# ---------------------------------------------------------------------------
# ビルド(-O2 が本番構成、-O0 は SR-2.3 の再現性比較用)
# ---------------------------------------------------------------------------
printf '\nbuilding %s -O2 ... ' "$CXX"
$CXX $CXXFLAGS -O2 -o "$OUT/verify_O2" "$SRC"
printf 'ok\n'

# ---------------------------------------------------------------------------
# 数値検証(これが合否のゲート)
# ---------------------------------------------------------------------------
set +e
"$OUT/verify_O2" | tee "$OUT/run1.txt"
STATUS=${PIPESTATUS:-$?}
set -e
# PIPESTATUS は sh では使えない環境があるため RESULT 行で判定し直す
if grep -q '^RESULT: ALL GREEN' "$OUT/run1.txt"; then
    STATUS=0
else
    STATUS=1
fi

if [ "$QUICK" -eq 1 ]; then
    exit "$STATUS"
fi

# ---------------------------------------------------------------------------
# SR-2.4: 同一バイナリの決定論(cpu 行を除いて bit-exact)
# ---------------------------------------------------------------------------
printf '\ndeterminism check (SR-2.4) ... '
"$OUT/verify_O2" > "$OUT/run2.txt" 2>/dev/null || true
grep -v 'cpu@' "$OUT/run1.txt" > "$OUT/run1.nocpu"
grep -v 'cpu@' "$OUT/run2.txt" > "$OUT/run2.nocpu"
if diff -q "$OUT/run1.nocpu" "$OUT/run2.nocpu" >/dev/null; then
    printf 'PASS (identical output across two runs)\n'
else
    printf 'FAIL (output differs across runs)\n'
    diff "$OUT/run1.nocpu" "$OUT/run2.nocpu" || true
    STATUS=1
fi

# ---------------------------------------------------------------------------
# SR-2.3: -O0 / -O2 間で検出ピーク f_out が ±0.015 Hz 以内であること
# ---------------------------------------------------------------------------
# -O0 の実行は SR-4.2(確保カウンタ)の正となる構成でもある — -O2 では
# 対になった new/delete がコンパイラに省略されうるため、確保検出は -O0 を信頼する。
printf 'reproducibility check -O0 vs -O2 (SR-2.3) + allocation counter (SR-4.2) ... '
$CXX $CXXFLAGS -O0 -o "$OUT/verify_O0" "$SRC"
if ! "$OUT/verify_O0" > "$OUT/run_O0.txt" 2>&1; then
    printf 'FAIL (-O0 run did not finish green)\n'
    tail -5 "$OUT/run_O0.txt"
    STATUS=1
fi
if ! grep -q '^RESULT: ALL GREEN' "$OUT/run_O0.txt"; then
    printf 'FAIL (-O0 run reported failures)\n'
    STATUS=1
fi
extract_fout() {
    grep 'f_out=' "$1" | sed -e 's/.*f_out=//' -e 's/Hz.*//'
}
extract_fout "$OUT/run1.txt" > "$OUT/fout_O2.txt"
extract_fout "$OUT/run_O0.txt" > "$OUT/fout_O0.txt"
if paste "$OUT/fout_O2.txt" "$OUT/fout_O0.txt" | awk '
    { d = $1 - $2; if (d < 0) d = -d; if (d > 0.015) { printf "  peak drift %.4f Hz (%s vs %s)\n", d, $1, $2; bad = 1 } }
    END { exit bad ? 1 : 0 }'; then
    printf 'PASS (|delta f_out| <= 0.015 Hz)\n'
else
    printf 'FAIL\n'
    STATUS=1
fi

printf '\n'
if [ "$STATUS" -eq 0 ]; then
    printf 'BUILD + VERIFY: ALL GREEN\n'
else
    printf 'BUILD + VERIFY: FAILED\n'
fi
exit "$STATUS"
