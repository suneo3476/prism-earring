# prism — Web デモ(u3-web-demo)

マイク入力をリアルタイムにピッチシフトしてイヤホンへ返す、ブラウザ上の効果確認用デモ。
外部ライブラリ・CDN・実行時ビルドツールは一切使わない静的ファイル構成。

**DSP は WASM が正**。C++ 正本 `dsp/include/prism/PitchShifter.h` を Emscripten で
ビルドした `prism.wasm` を AudioWorklet 内で直接 `WebAssembly.instantiate` して使う。
`PitchShifterJS`(同アルゴリズムの JS 移植)は、WASM を取得・初期化できない環境の
**フォールバック**として残してある。どちらで動いているかは画面の `engine:` 表示でわかる。

## ファイル

| ファイル | 役割 |
|---|---|
| `index.html` | 画面。CSP メタタグで外部オリジンへの接続を遮断(SR-2.2) |
| `styles.css` | 最小スタイル。`prefers-color-scheme` でライト/ダーク自動 |
| `main.js` | UI 層(DemoUI)。ES module。入力ソース(マイク / タブ音声 / 音声ファイル)の取得、`prism.wasm` を fetch して Worklet へ渡し、以後は契約 3 の postMessage のみで会話する |
| `prism-worklet.js` | AudioWorklet 層。`PrismWasmShifter`(WASM ローダー。**本番**)+ `PitchShifterJS`(JS 移植。フォールバック)+ `PrismProcessor`(接着層) |
| `prism.wasm` | **ビルド成果物(コミット済み)**。契約 2 の `extern "C"` API。import 0 本 |
| `wasm/prism_bridge.cpp` | 契約 2 の実装。`prism::PitchShifter` の薄いラッパ(アルゴリズムはコア側にある) |
| `wasm/build.sh` | `prism.wasm` のビルドスクリプト(再現用) |
| `wasm/inspect.mjs` `wasm/verify-shape.mjs` | 生成物の import / export を目視・自動検査する開発用ツール |
| `serve.sh` | ローカル HTTP / HTTPS 配信(`.wasm` を `application/wasm` で返す) |
| `test/wasm-test.mjs` | **WASM 経路**のオフライン数値検証(Node) |
| `test/pitch-shifter-test.mjs` | JS フォールバックのオフライン数値検証(Node) |

## WASM のビルド

`prism.wasm` はコミット済みなので、**デモを動かすだけならビルドは不要**。
`dsp/include/prism/PitchShifter.h` か `wasm/prism_bridge.cpp` を変えたときだけ実行する。

```sh
source ~/emsdk/emsdk_env.sh   # Emscripten 6.0.9 で確認
web/wasm/build.sh             # -> web/prism.wasm(+ import/export の自動検査)
node web/test/wasm-test.mjs   # 数値検証を通してからコミットする
```

生成物は **import 0 本**の単体 wasm で、Emscripten の JS グルーを一切必要としない
(`WebAssembly.instantiate(bytes, {})` だけで動く)。理由と各フラグの根拠は
`wasm/build.sh` 冒頭のコメントに書いてある。

## 起動(Mac)

```sh
./web/serve.sh          # http://localhost:8000/
```

ブラウザで `http://localhost:8000/` を開き、「▶ 開始」を押してマイクを許可する。
`localhost` は secure context の例外なので HTTP のままマイクを使える。

`prism.wasm` は HTTP 経由でのみ取得できる。`serve.sh` は `.wasm` を
`application/wasm` で返す(Python のバージョン差を吸収するため MIME を明示登録している)。
**`file://` で `index.html` を直接開くと fetch が失敗し、JS フォールバックで動く**
(音は出るが CPU 負荷が数十倍になる)。画面の `engine:` 表示で確認できる。

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

> どちらの経路でも音声は端末外へ出ない。ネットワークを使うのは同一オリジンの
> `prism.wasm` を取得する 1 本の `fetch` だけで、外部オリジンへの接続は
> CSP の `connect-src 'self'` で宣言的に遮断している(SR-1 / SR-2)。
> `XHR`/`WebSocket`/解析スクリプトは一切使わない。

## 入力ソース

「入力」から処理対象を選ぶ。**切り替えは停止中のみ**(動作中は無効化される)。

| 入力 | 用途 |
|---|---|
| マイク(既定) | 本来の用途。外界の音を拾ってイヤホンへ返す |
| ブラウザのタブ音声 | YouTube などの**タブの音**を入力にする。マイクに音を入れられない環境(リモートデスクトップ経由など)向け |
| 音声ファイル | 手元の音声ファイルをループ再生して処理する。同じ素材で繰り返し聞き比べたいとき |

### ブラウザのタブ音声

1. 「ブラウザのタブ音声」を選び、「▶ 開始」を押す。
2. 共有ダイアログで **「Chrome のタブ」**(Edge / Vivaldi も同様)のタブを開く。
3. 音を鳴らしたいタブ(YouTube など)を選ぶ。
4. 左下の **「タブの音声を共有」にチェックを入れる**。これを忘れると音声トラックが
   付いてこないので、その場合は画面に手順を出して停止する。

`getDisplayMedia` は Chromium で `video: true` を指定しないと共有ダイアログが出ないため
映像も要求するが、映像トラックは取得直後に `stop()` して MediaStream から外している。

共有中は **共有元タブの音をブラウザが自動で止める**(`suppressLocalAudioPlayback: true`、
Chromium 109 以降)。イヤホンには処理音だけが届くので、YouTube などを正しい高さで聴く
用途にそのまま使える。この指定を無視する古いブラウザでは、共有元タブを(タブのミュート
ボタンで)手動でミュートすれば同じ状態になる。Chromium のタブ音声キャプチャは出力前の音を
取るため、ミュートしても取り込みは続く。

同じ理屈で「Web デモを開いたタブ」は共有候補から外している(`selfBrowserSurface: 'exclude'`)。
自分の出力を自分で拾うとフィードバックループになるため。

共有元タブ側で「共有を停止」を押すと、`ended` を検知して自動的に停止する。

**制限**

- **同じブラウザ内のタブのみ。** 別ブラウザ・別アプリの音は取れない。
- **macOS ではタブ音声のみ。** 画面全体やウィンドウを選んだ場合、音声は取得できない
  (OS がシステム音声のキャプチャを許さない)。
- **Chromium 系のみ。** Chrome / Edge / Vivaldi で動作する。Safari と Firefox は
  `getDisplayMedia` の音声取得に対応していない。
- タブ音声は既にブラウザのオーディオパイプラインを通っているため、遅延はマイク入力より
  やや大きくなる。効果の確認用と割り切る。

### 音声ファイル

「音声ファイル」を選ぶとファイル選択欄が出る。ブラウザがデコードできる形式
(wav / mp3 / m4a / ogg など)を選び、「▶ 開始」を押すとループ再生される。
ファイルは `decodeAudioData` でメモリ上に展開するだけで、端末外へは出ない。

## 操作

| コントロール | 範囲 | 既定 | 意味 |
|---|---|---|---|
| シフト量 L | -150〜0 cent | -89 | 左チャンネルのシフト量 |
| シフト量 R | -150〜0 cent | -89 | 右チャンネルのシフト量 |
| 左右連動 | on/off | on | on の間、L/R のスライダーが連動する |
| Dry/Wet | 0.00〜1.00 | 1.00 | 0 で原音そのまま、1 で処理音のみ |
| 窓長 | 10〜100 ms | 50 | 跳躍時のクロスフェード時間。音の滑らかさに効く。**遅延には影響しない** |

遅延表示は 1 秒ごとに更新され、3 成分の内訳と合算、それに使用中のエンジンを出す。

- 出力 = `AudioContext.baseLatency + outputLatency`(Safari など `outputLatency`
  未実装のブラウザでは `baseLatency` のみ。合算に「(概算)」が付く)
- ブロック = `128 × 2 ÷ サンプリングレート`
- DSP = Worklet が報告する設計値遅延
- `engine: wasm(prism.wasm)` … 本番経路。`engine: js(フォールバック: 理由)` … JS 移植で動作中

## テスト

```sh
node web/test/wasm-test.mjs           # WASM 経路(本番)
node web/test/pitch-shifter-test.mjs  # JS フォールバック
```

Node 18+ のみで動く(外部パッケージなし)。どちらも `prism-worklet.js` を `node:vm` で
classic script として評価し、**本番と同じローダー / 同じコア**を取り出して検証する。
周波数推定には自前の離散フーリエ変換を使う(**検証側のみ**。音声経路には
FFT・位相ボコーダを一切使わない)。

`wasm-test.mjs` の検証内容: モジュールの形(import 0 本・契約 2 の export 一式)、
ピッチ精度(110/440/3520 Hz × 44.1k/48k、比 0.95 ±0.5%)、セント→比の一般性、
L/R 独立、設計値遅延の式と実測遅延(≤10ms、C++ 正本の 228/248 サンプルと一致)、
グリッチ 0 件、WASM↔JS の一致、`ps_prepare`/`ps_set_param` の異常系、dryWet=0 バイパス、
無音・可変ブロック長・`numFrames<=0`、ローダーの失敗経路、C API のハンドル管理
(枯渇・無効ハンドル)、処理コスト、`process` 経路の確保ゼロ、そして接着層の統合
(契約 3 の ready/latency/error とエンジン選択・フォールバック)。
判定式と閾値は `verify/verify.cpp`(C++ 正本の検証ハーネス)と揃えてある。

実測(Apple M2 / Node 22 / Emscripten 6.0.9):

| 項目 | WASM | JS フォールバック |
|---|---|---|
| 128 フレームの処理時間 @48k | 0.0103 ms(レンダ量子の 0.38%) | 0.686 ms(25.7%) |
| インパルス実測遅延 | 228 サンプル @44.1k / 248 @48k(いずれも 5.17ms) | 同左 |
| ピッチ精度 @-89 cents | -0.18〜-0.28 cents | 同左 |

## 既知の制限

### 1. 設計からの逸脱: WASM を JS に内包していない

`functional-spec.md` Q3-A / `tech-stack-decisions.md` は `-sSINGLE_FILE=1` で WASM を
JS に内包する構成を指定していたが、**`.wasm` を別ファイルにして fetch する構成**を採った。

- 理由: AudioWorklet のグローバルスコープには `fetch` も `importScripts` も無く、
  Emscripten の JS グルーは Worklet 内で動かない。バイト列をメインスレッドで取得し
  `processorOptions` で渡して `WebAssembly.instantiate` する方が、グルー無し・
  import 0 本で確実に動く(実測: import 0 本 / 13,705 バイト)。
- 影響: `file://` では fetch できず JS フォールバックになる(HTTP で配信すれば解決)。
  外部オリジンへの接続が発生しない点(SR-2.1 の趣旨)は変わらない。
- CSP は `connect-src 'self'`(同一オリジンの `prism.wasm` 取得のため。外部は不許可)と
  `script-src 'self' 'wasm-unsafe-eval'`(WASM コンパイルに必要)に更新した。

### 2. JS フォールバックの CPU 負荷

`PitchShifterJS` は WASM の約 66 倍の処理時間を要する(上表)。レンダ量子の 25% 程度で
動きはするが、非力な端末やバックグラウンドタブではドロップアウトの余地がある。
フォールバックはあくまで保険であり、常用する構成ではない。

### 3. 約 105 Hz 未満のピッチ精度

WASM 経路は C++ 正本 `dsp/include/prism/PitchShifter.h` そのもの、JS フォールバックは
その直訳で、いずれも遅延スイープ幅を設計定数 9.5 ms に固定している
(ルート `README.md` の逸脱 D-A)。この幅は
「正しくシフトできる最低周波数 ≈ 1/9.5ms ≈ 105 Hz」を意味し、それ未満の帯域では
誤差が増える(C++ 実測で 90 Hz のとき −15 cents)。人の話し声の基本周波数は
おおむね 85〜255 Hz なので、男性低音域の基音は精度が落ちる。

改善するにはスイープを広げる = 遅延を増やすしかない。現行値は NFR-1 の 10 ms 予算
(最大遅れ = 8 サンプル + 9.5 ms = 9.68 ms)いっぱいに取ってある。

### 4. 瞬時最大遅れは設計値より大きい

`dspLatencyMs` が報告するのは設計値(平均遅れ)`baseOffset + sweep/2` = 約 4.92 ms。
主ヘッドの遅れはスイープ区間を走査するため瞬時には最大 9.68 ms まで伸び、
クロスフェード中の旧ヘッドはさらに `(1-ratio)×窓長`(既定 50 ms 窓で約 2.6 ms)
遅れる。インパルスの立ち上がり実測は 5.17 ms(44.1k / 48k とも。C++ 正本と一致)。

### 5. その他

- `crossfadeMs` はブロック頭でラッチされ、次の跳躍のクロスフェード長から反映される
  (BR1.4)。設計値遅延は窓長に依存しないため、窓長を動かしても遅延表示は変わらない。
- シフト量が 0 のときは遅れが蓄積せず跳躍も起きない(素通しと同じ経路になる)。
- 遅延報告(1 秒ごとの `postMessage`)はオーディオスレッドから送るため、
  構造化クローズのぶんだけごく小さな確保が発生する。契約 3 の要求であり、
  レンダ量子ごとの処理経路には確保は一切ない。
