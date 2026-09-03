#!/usr/bin/env bash
#
# serve.sh — PrismEarring Web デモをローカル配信する
#
#   ./serve.sh              … HTTP で配信(既定ポート 8000)
#   ./serve.sh 9000         … ポート指定
#   ./serve.sh --https      … 自己署名 TLS で配信(Pixel など LAN 端末用)
#   ./serve.sh --https 8443 … TLS + ポート指定
#
# getUserMedia は secure context を要求する。localhost は例外扱いなので
# Mac 上のブラウザからは HTTP のままで動く。LAN の IP でアクセスする
# Pixel などからは HTTPS が必要 —— --https を使うか、Chrome の
# chrome://flags/#unsafely-treat-insecure-origin-as-secure で例外登録する。
# 詳細は README.md「Pixel から開く」を参照。
#
# 依存: python3(標準)。--https のときのみ openssl も使う。

set -euo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
HERE="$(dirname "$SCRIPT")"
cd "$HERE"

USE_TLS=0
PORT=""

for arg in "$@"; do
    case "$arg" in
        --https) USE_TLS=1 ;;
        --help|-h)
            sed -n '3,16p' "$SCRIPT" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        ''|*[!0-9]*)
            echo "不明な引数: $arg" >&2
            exit 1
            ;;
        *) PORT="$arg" ;;
    esac
done

if [ -z "$PORT" ]; then
    if [ "$USE_TLS" -eq 1 ]; then PORT=8443; else PORT=8000; fi
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 が見つかりません。配信には python3 が必要です。" >&2
    exit 1
fi

# LAN IP(macOS / Linux 双方でだいたい取れる方法を順に試す)
lan_ip() {
    if command -v ipconfig >/dev/null 2>&1; then
        for iface in en0 en1; do
            ip="$(ipconfig getifaddr "$iface" 2>/dev/null || true)"
            [ -n "$ip" ] && { echo "$ip"; return; }
        done
    fi
    if command -v hostname >/dev/null 2>&1; then
        ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
        [ -n "$ip" ] && { echo "$ip"; return; }
    fi
    echo ""
}

IP="$(lan_ip)"

if [ "$USE_TLS" -eq 0 ]; then
    echo "HTTP で配信します(Ctrl+C で停止)"
    echo "  この Mac から      : http://localhost:${PORT}/"
    if [ -n "$IP" ]; then
        echo "  同一 LAN の端末から: http://${IP}:${PORT}/  ← マイクは HTTPS でないと使えません"
        echo "  → Pixel から試すときは ./serve.sh --https を使うか、README の chrome://flags 手順へ"
    fi
    exec python3 -m http.server "$PORT" --bind 0.0.0.0
fi

# ---- 自己署名 TLS ----
if ! command -v openssl >/dev/null 2>&1; then
    echo "openssl が見つかりません。--https には openssl が必要です。" >&2
    exit 1
fi

CERT_DIR=".certs"
CERT="${CERT_DIR}/dev.crt"
KEY="${CERT_DIR}/dev.key"

if [ ! -f "$CERT" ] || [ ! -f "$KEY" ]; then
    mkdir -p "$CERT_DIR"
    SAN="DNS:localhost,IP:127.0.0.1"
    [ -n "$IP" ] && SAN="${SAN},IP:${IP}"
    echo "自己署名証明書を作成します: ${CERT}(SAN: ${SAN})"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$KEY" -out "$CERT" -days 365 \
        -subj "/CN=prism-earring-dev" \
        -addext "subjectAltName=${SAN}" >/dev/null 2>&1
fi

echo "HTTPS(自己署名)で配信します(Ctrl+C で停止)"
echo "  この Mac から      : https://localhost:${PORT}/"
[ -n "$IP" ] && echo "  同一 LAN の端末から: https://${IP}:${PORT}/"
echo "  ※ 初回は証明書の警告が出ます。「詳細設定」→「アクセスする」で進んでください。"

exec python3 - "$PORT" "$CERT" "$KEY" <<'PY'
import http.server, ssl, sys

port, cert, key = int(sys.argv[1]), sys.argv[2], sys.argv[3]
httpd = http.server.ThreadingHTTPServer(('0.0.0.0', port), http.server.SimpleHTTPRequestHandler)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(certfile=cert, keyfile=key)
httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
httpd.serve_forever()
PY
