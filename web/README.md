# prism — Web デモ(u3-web-demo)

マイク入力をリアルタイムにピッチシフトしてイヤホンへ返す、ブラウザ上の効果確認用デモ。
外部ライブラリ・CDN・ビルドツールは一切使わない静的ファイル 4 枚構成。

## ファイル

| ファイル | 役割 |
|---|---|
| `index.html` | 画面。CSP メタタグで外部接続を遮断(SR-2.2) |
| `styles.css` | 最小スタイル。`prefers-color-scheme` でライト/ダーク自動 |
| `main.js` | UI 層(DemoUI)。ES module。契約 3 の postMessage のみで Worklet と会話する |
| `prism-worklet.js` | AudioWorklet 層。`PitchShifterJS`(DSP コア。C++ 正本 `dsp/include/prism/PitchShifter.h` の直訳)+ `PrismProcessor`(接着層) |
| `serve.sh` | ローカル HTTP / HTTPS 配信 |
| `test/pitch-shifter-test.mjs` | `PitchShifterJS` のオフライン数値検証(Node) |

## 起動(Mac)

```sh
./web/serve.sh          # http://localhost:8000/
```

ブラウザで `http://localhost:8000/` を開き、「▶ 開始」を押してマイクを許可する。
`localhost` は secure context の例外なので HTTP のままマイクを使える。

**イヤホン必須。** スピーカーではハウリングする。

停止すると `MediaStreamTrack.stop()` でマイクを解放し、ブラウザの録音インジケータが消える。

## Pixel(同一 LAN)から開く

`localhost` 以外のオリジンでは `getUserMedia` が secure context を要求するため、
**HTTP のままではマイクを使えない**。次のどちらかで回避する。

### 方法 A: 自己署名 HTTPS で配信する(推奨)

```sh
./web/serve.sh --https   # https://<MacのLAN IP>:8443/
```

初回に `web/.certs/dev.{crt,key}` を openssl で生成する(SAN に localhost と
実行時の LAN IP を入れる。`.certs/` は gitignore 済み)。
Pixel の Chrome で `https://<MacのLAN IP>:8443/` を開くと証明書の警告が出るので、
「詳細設定」→「(安全ではないページに)アクセスする」で進む。以降マイク許可が出せる。

Mac の LAN IP が変わったら `web/.certs/` を削除して再生成する。

### 方法 B: Chrome の insecure origin 例外に登録する

Pixel の Chrome で `chrome://flags/#unsafely-treat-insecure-origin-as-secure` を開き、
テキスト欄に配信元をそのまま入力して Enabled にし、Chrome を再起動する。

```
http://192.168.11.51:8000
```

この方法なら `./web/serve.sh`(HTTP)のままで動く。**開発中だけの措置**であり、
確認が終わったらフラグを Disabled に戻すこと。

> どちらの経路でも音声は端末外へ出ない。`fetch`/`XHR`/`WebSocket` は一切使わず、
> CSP の `connect-src 'none'` で宣言的にも遮断している(SR-1 / SR-2)。

## 操作

| コントロール | 範囲 | 既定 | 意味 |
|---|---|---|---|
| シフト量 L | -150〜0 cent | -89 | 左チャンネルのシフト量 |
| シフト量 R | -150〜0 cent | -89 | 右チャンネルのシフト量 |
| 左右連動 | on/off | on | on の間、L/R のスライダーが連動する |
| Dry/Wet | 0.00〜1.00 | 1.00 | 0 で原音そのまま、1 で処理音のみ |
| 窓長 | 10〜100 ms | 50 | 跳躍時のクロスフェード時間。音の滑らかさに効く。**遅延には影響しない** |

遅延表示は 1 秒ごとに更新され、3 成分の内訳と合算を出す。

- 出力 = `AudioContext.baseLatency + outputLatency`(Safari など `outputLatency`
  未実装のブラウザでは `baseLatency` のみ。合算に「(概算)」が付く)
- ブロック = `128 × 2 ÷ サンプリングレート`
- DSP = Worklet が報告する設計値遅延

## テスト

```sh
node web/test/pitch-shifter-test.mjs
```

Node 18+ のみで動く(外部パッケージなし)。`prism-worklet.js` を `node:vm` で
classic script として評価し、`PitchShifterJS` を取り出して検証する。
周波数推定には自前の離散フーリエ変換を使う(**検証側のみ**。音声経路には
FFT・位相ボコーダを一切使わない)。

検証内容: ピッチ精度(110/440/3520 Hz × 44.1k/48k、比 0.95 ±0.5%)、
セント→比の一般性、L/R 独立、設計値遅延の式、インパルス実測遅延(≤10ms)、
グリッチ 0 件、`prepare`/`setParam` の異常系、dryWet=0 バイパス、無音・可変
ブロック長、窓長のライブ変更、平滑化、処理コスト、`process` 経路にヒープ確保が
ないこと、そして C++ 正本の実測値との一致。判定式と閾値は `verify/verify.cpp`
(C++ 正本の検証ハーネス)と揃えてある。

## 既知の制限

### 1. WASM ではなく JS 実装

本来の設計(`tech-stack-decisions.md`)は C++ コアを Emscripten で WASM 化して
使う。本環境に `emcc` が無いため、`external-dependency-map.md` の失速時代替
「AudioWorklet 内 JS 実装に切替(アルゴリズム同一)」を採用した。

`PitchShifterJS` のメソッド名は契約 2 の `extern "C"` API に 1:1 対応させてある
(`create`/`destroy`/`prepare`/`ioPtr`/`process`/`setParam`/`latencyMs`)。
WASM ビルドができたら `PrismProcessor` 側の 7 箇所の呼び出しを差し替えるだけで移行できる。
契約 3(postMessage)は変更不要。

### 2. 約 105 Hz 未満のピッチ精度

DSP コアは C++ 正本 `dsp/include/prism/PitchShifter.h` の直訳で、遅延スイープ幅を
設計定数 9.5 ms に固定している(ルート `README.md` の逸脱 D-A)。この幅は
「正しくシフトできる最低周波数 ≈ 1/9.5ms ≈ 105 Hz」を意味し、それ未満の帯域では
誤差が増える(C++ 実測で 90 Hz のとき −15 cents)。人の話し声の基本周波数は
おおむね 85〜255 Hz なので、男性低音域の基音は精度が落ちる。

改善するにはスイープを広げる = 遅延を増やすしかない。現行値は NFR-1 の 10 ms 予算
(最大遅れ = 8 サンプル + 9.5 ms = 9.68 ms)いっぱいに取ってある。

### 3. 瞬時最大遅れは設計値より大きい

`dspLatencyMs` が報告するのは設計値(平均遅れ)`baseOffset + sweep/2` = 約 4.92 ms。
主ヘッドの遅れはスイープ区間を走査するため瞬時には最大 9.68 ms まで伸び、
クロスフェード中の旧ヘッドはさらに `(1-ratio)×窓長`(既定 50 ms 窓で約 2.6 ms)
遅れる。インパルスの立ち上がり実測は 5.17 ms(44.1k / 48k とも。C++ 正本と一致)。

### 4. その他

- `crossfadeMs` はブロック頭でラッチされ、次の跳躍のクロスフェード長から反映される
  (BR1.4)。設計値遅延は窓長に依存しないため、窓長を動かしても遅延表示は変わらない。
- シフト量が 0 のときは遅れが蓄積せず跳躍も起きない(素通しと同じ経路になる)。
- 遅延報告(1 秒ごとの `postMessage`)はオーディオスレッドから送るため、
  構造化クローズのぶんだけごく小さな確保が発生する。契約 3 の要求であり、
  レンダ量子ごとの処理経路には確保は一切ない。
