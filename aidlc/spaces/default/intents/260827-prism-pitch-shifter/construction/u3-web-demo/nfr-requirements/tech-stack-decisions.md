# 技術スタック決定 — u3-web-demo

| 項目 | 決定 | 根拠 |
|---|---|---|
| フロントエンド | 素の HTML + vanilla JS(フレームワーク・ビルドツールなし) | design-system-mapping(確認済み)|
| 音声 | Web Audio API: AudioWorklet + getUserMedia(EC/NS/AGC 無効) | FR-4.1/4.3、契約 3 |
| WASM ビルド | Emscripten(emcc)。`-O2 -sSINGLE_FILE=1 -sMODULARIZE=1 -sEXPORTED_FUNCTIONS=[ps_*]` 相当。バージョンは README に固定表記 | 機能設計 Q1、SR-4.1 |
| 接着層 | extern "C" 平坦 API + HEAPF32 共有(契約 2) | D-05 |
| 配信 | 静的ファイル(index.html / main.js / worklet.js)。ローカル HTTP サーバ(`python3 -m http.server` 等)で配信。Pixel からは同一 LAN の Mac へアクセス(getUserMedia は secure context 要件のため localhost 以外は HTTPS が必要 — README に手順記載) | practices Deployment |
| CSP | メタタグで外部接続遮断 | SR-2.2 |

## Assumptions & Open Questions

- Pixel(LAN 経由)でのマイク許可には HTTPS(自己署名または `chrome://flags` の insecure origin 許可)が必要。README に両手順を記載する。[assumption]
