# デザインシステム対応 — prism Web デモ

デザインシステムは使用しない(確認済み方針: 素の HTML + 最小 CSS、実行時依存ゼロ)。[Q1]

| UI 要素 | 実装 | 備考 |
|---|---|---|
| タイポグラフィ | `font-family: system-ui` | Web フォント読み込みなし |
| 配色 | `prefers-color-scheme` メディアクエリでライト/ダーク自動 | CSS カスタムプロパティ 4 個(背景/文字/アクセント/枠)のみ |
| レイアウト | 単一カラム、`max-width: 28rem; margin: auto` | Pixel 縦持ちと Mac ウィンドウの両方で自然に収まる |
| コントロール | ネイティブ `<input type="range">` / `<button>` / `<input type="checkbox">` | カスタム描画なし。OS 標準の操作性・アクセシビリティを継承 |
| 状態表示 | `role="status"` + `aria-live="polite"`、エラーは `role="alert"` | |

## Assumptions & Open Questions

None.
