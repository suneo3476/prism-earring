# PrismEarring — Android 版

マイクで拾った外界の音をリアルタイムに約 −89 セントピッチシフトし、イヤホンで聴くための
Android ネイティブアプリ。DSP は `dsp/include/prism/PitchShifter.h`(JUCE 非依存の純 C++)を
そのまま使い、オーディオ入出力は **Oboe** で直接叩く。JUCE は使わない。

Web デモ(`web/`)は WebAudio の AudioWorklet 越しに動くため、往復で 60〜150ms 程度の遅延が
避けられない。この Android 版はその層を丸ごと外すことが目的。

---

## 構成

```
android/
  settings.gradle.kts / build.gradle.kts / gradle/libs.versions.toml
  app/
    build.gradle.kts
    src/main/
      AndroidManifest.xml
      cpp/
        AudioBridge.h            純 C++。全二重の状態機械 + チャンネル変換 + PitchShifter 駆動
        PrismEngine.{h,cpp}      Oboe のストリーム生存管理
        jni_bridge.cpp           JNI(Kotlin との唯一の接点)
        audio_bridge_smoke.cpp   ホストスモークテスト(Oboe / JNI 非依存)
        CMakeLists.txt
      java/dev/saku/prismearring/
        NativeEngine.kt          libprism.so の薄いラッパ
        Params.kt                パラメータと SharedPreferences 永続化
        PrismService.kt          ForegroundService(常駐)
        CaptureController.kt     他アプリの再生音の捕獲(MediaProjection + AudioRecord)
        MainActivity.kt          画面
      res/                       Material3 DayNight(ライト既定 / values-night がダーク)。
                                 web/styles.css のトークンを踏襲
  build-smoke.sh                 ホストスモークの実行スクリプト
```

レイヤ分離は `aidlc/spaces/default/memory/team.md` の「Code Style」に従う:
**DSP コア(純 C++・依存ゼロ)/ 検証ハーネス / プラットフォーム接着層** を混ぜない。
`AudioBridge.h` が「接着層のうち、プラットフォームに依存しない部分」で、ここだけは
Mac / Windows のホストでビルドしてテストできる。

---

## 必要なもの

| 項目 | バージョン | 備考 |
|---|---|---|
| JDK | 17 以上(21 で検証) | Android Studio 同梱の JBR でもよい |
| Android Gradle Plugin | 8.6.1 | `gradle/libs.versions.toml` |
| Gradle | 8.7 | wrapper 同梱。`gradle-wrapper.jar` はコミット済み |
| Kotlin | 2.0.21 | |
| compileSdk / targetSdk | 35 | |
| minSdk | 30 | AAudio の低遅延経路を前提にする |
| NDK | 27.2.12479018 | `app/build.gradle.kts` の `ndkVersion` で固定 |
| CMake | 3.22.1 | `android.externalNativeBuild.cmake.version` |
| Oboe | 1.9.0 | Maven の prefab(`com.google.oboe:oboe`) |
| ABI | `arm64-v8a` のみ | 検証できない ABI のバイナリは配らない |

### Android Studio で開く

`android/` を **プロジェクトのルートとして** 開く(リポジトリのルートではない)。
`local.properties` は Studio が自動生成する。

### コマンドラインで揃える(Studio を使わない場合)

```bash
# macOS / Linux
SDK="$HOME/Library/Android/sdk"          # Linux は ~/Android/Sdk
yes | "$SDK/cmdline-tools/latest/bin/sdkmanager" --licenses
"$SDK/cmdline-tools/latest/bin/sdkmanager" \
    "platforms;android-35" "build-tools;35.0.0" \
    "ndk;27.2.12479018" "cmake;3.22.1"

echo "sdk.dir=$SDK" > android/local.properties
```

```powershell
# Windows (PowerShell)
$SDK = "$env:LOCALAPPDATA\Android\Sdk"
& "$SDK\cmdline-tools\latest\bin\sdkmanager.bat" --licenses
& "$SDK\cmdline-tools\latest\bin\sdkmanager.bat" `
    "platforms;android-35" "build-tools;35.0.0" `
    "ndk;27.2.12479018" "cmake;3.22.1"

"sdk.dir=$($SDK -replace '\\','\\')" | Out-File -Encoding ascii android\local.properties
```

NDK は約 2.4 GB ある。ディスクの空きを確認してから入れること。

---

## ビルド

```bash
cd android
./gradlew assembleDebug          # macOS / Linux
gradlew.bat assembleDebug        # Windows
```

初回は Gradle 本体と AGP / Kotlin / Oboe の取得が走るため数分かかる(`--offline` は不可)。

成果物:

```
android/app/build/outputs/apk/debug/app-debug.apk
```

`.so` が入っているかの確認:

```bash
unzip -l app/build/outputs/apk/debug/app-debug.apk | grep 'lib/'
# lib/arm64-v8a/libprism.so と lib/arm64-v8a/libc++_shared.so が出れば正しい
```

### ホストスモーク(実機なしの検証)

```bash
cd android
./build-smoke.sh
```

`clang++` だけで通る。JNI と Oboe を除いた `AudioBridge` の

- 起動同期の状態機械(Drain → Cushion → Render)
- モノ入力の L/R 複製、インタリーブ ↔ 非インタリーブ変換
- 入力アンダーラン / 入力ポインタ null / 未初期化 / 不正な引数
- コールバック上限を超えるフレーム数の分割処理
- パラメータの clamp

を確認する。**実機が無い環境でリグレッションを見張れるのはここまで。**
実際のレイテンシとグリッチは実機でしか測れない(下記「未検証」)。

---

## Pixel へのサイドロード

1. `app-debug.apk` を端末へ渡す(USB、`adb install`、クラウドストレージ、いずれでもよい)。
   ```bash
   # adb がある場合
   ~/Library/Android/sdk/platform-tools/adb install -r \
       android/app/build/outputs/apk/debug/app-debug.apk
   ```
2. `adb` を使わずファイルとして渡した場合は、端末の「ファイル」アプリから APK をタップする。
3. 初回は「不明なアプリのインストール」の許可を求められる。
   **設定 → アプリ → 特別なアプリアクセス → 不明なアプリのインストール** で、
   APK を開いたアプリ(Files / Chrome など)に許可を与える。
4. インストール後、初回起動でマイクの権限を求められる。**許可**を選ぶ。
   通知の権限も聞かれる(拒否しても動作するが、常駐中の通知と「停止」ボタンが出なくなる)。

---

## 使い方

1. **有線イヤホンを挿す。** スピーカーではハウリングする。
2. アプリを開き、下部の **開始** を押す。
3. 状態ピルが「停止中」→「同期中」→「動作中」と変わる。往復遅延の推定値が
   1 秒ごとに更新される。
4. シフト量を耳で合わせる。既定は **−89 セント**。
   - 太いスライダ(常に −1200 〜 +1200 の全域。左右端に「低く」「高く」のラベル)、
     または左右の **−/+** で調整する。1 回ぶんの変化量は **刻み幅**(全音 200 /
     半音 100 / 1/4 音 50 / 1/8 音 25、既定 1/8 音)で切り替える。
   - 中央の大きな数値(L、左右独立時は R も)は**タップすると数値入力ダイアログ**が開き、
     セント数を直接キーボードで打ち込める(−1200 〜 1200、範囲外は自動で丸める)。
   - **ジャンプ行**(最小 −1200 / 0 / 既定 −89 / 最大 +1200)をタップすると、その値へ
     L/R まとめて移動する(左右独立が ON でも両方動く)。
   - **ユーザープリセット**(P1 / P2 / P3)は、タップでその値へジャンプ、**長押しで
     現在の L/R の値をそのスロットに上書き登録**する。未登録のスロットは「—」表示。
   - **左右を別々に設定** を ON にすると L/R を独立に動かせる。
5. **音量**行で出力音量を調整する(−6dB 〜 +12dB、既定 0dB)。上げすぎても
   自動でやわらかく抑える(ソフトクリップ)ので歪みにくい。端末の音量キーも効く。
6. **なめらかさプリセット**(20 / 50 / 100 / 200 ms)で反応の速さと滑らかさを切り替える。
   細かい値は「詳細設定」から。
7. **音源**カードで、マイク音量(スライダ最小で「OFF」= マイク完全ミュート)、
   他アプリの音を拾うかどうか(下記「他アプリの音を拾う」節)、出力先 / 入力元の
   デバイスを設定する。
8. 各項目のラベル横の **ⓘ** を押すと、「どう変えるとどう聞こえるか」を平易な言葉で説明する。
9. 「詳細設定」から **テーマ**(システム / ライト / ダーク)、走査幅、
   出力用途(実験)も選べる。
10. 画面を消しても、他のアプリに移っても処理は続く。止めるときは
    アプリの **停止**、または通知の **停止**。

---

## 他アプリの音を拾う(v0.4.0)

他のアプリが再生している音(YouTube・音楽アプリなど)も、マイクの音と同じように
ピッチシフトして聴けます。ただしこの API(AudioPlaybackCapture)は元の音を
止められないため、**何もしないと原音と変換音が二重に聞こえます。** 次の手順で
原音を耳に届かないようにしてから使ってください。

1. **ダミーの Bluetooth イヤホンをつなぐ。** 実際には装着しない(あるいは電源を
   入れるだけ)。原音の出力先をこちらへ逃がすのが狙い。装着検知で自動再生・停止
   する機種は、その機能を切っておく(検知で再生が止まると原音自体が出なくなり
   確認しづらいため)。
2. 「音源」カードの **出力先** で、有線(USB)イヤホンを名指しで選ぶ。これで
   本アプリの出力は USB イヤホンへ、他アプリの原音は(システムの既定出力のままなら)
   ダミーの Bluetooth イヤホンへ、と経路が分かれる。
3. 「音源」カードの **他アプリの音を拾う** を ON にし、下部の **開始** を押す。
4. 「画面のキャプチャまたは録画」の同意ダイアログが出る(録画はしない。
   AudioPlaybackCapture の API 仕様上この文言になる)。**許可** を選ぶ。
5. YouTube や音楽アプリなどを再生する。「音源」カードの状態行が「捕獲中」に
   変われば拾えている。しばらく「他アプリが音を出していません」のままなら、
   そのアプリがキャプチャを拒否する設定になっているか、原音側の音量が絞られている
   可能性がある(**音量は下げない** — 捕獲はミュート後の音を取れないことがある)。

**制約:**

- Spotify など、一部のアプリはキャプチャを意図的に拒否しており拾えない。
- DRM 付きの動画配信サービスの音声も拾えない。
- 捕獲音の走査幅はマイク経路と別枠(既定 40ms)で、生音の漏れ込みが無いため
  10ms の遅延予算の対象外(詳しくは下の「捕獲音のミックス」節)。
- アプリを起動するたび(Service を再起動するたび)に同意ダイアログが再び出る。
  「詳細設定」の出力先 / 入力元 / 出力用途 / 走査幅を動作中に変えた場合も、
  設定の反映に Service の再起動が要るため同意ダイアログが再び出る。

---

## 聴感ヒアリングの手順

数値検証(オフライン、`verify/`)ではとらえきれない「実際に着けて聞いてどう感じるか」
を記録するための手順。値を変えるたびに、日常のいろいろな音(人の声・環境音・
テレビなど)を聞きながら耳で確かめる。

**走査幅(マイク経路):**

1. シフト量を既定の −89 セントのまま、「詳細設定」の **走査幅** を
   5 → 9.5 → 15 → 20 → 30 → 40 → 60 ms の順に上げていく。
2. 各値で、金属的な響き・ざらつきが減っていくか、生音との重なりで違和感が
   出はじめる境目はどこかを記録する。
3. 遅延表示(往復推定)も一緒に記録する。「生音と重ねても平気」と感じる主観と、
   遅延表示の数値との対応を残しておくと、あとで境界値の見当がつく。

**窓長(なめらかさ):**

1. 同じくシフト量は既定のまま、「詳細設定」の **なめらかさ** を
   10 → 50 → 100 → 150 → 190 → 200 ms の順に上げていく。
2. 190ms 付近(既定 −89 セントでの跳躍間隔)を境に、つなぎ目の聞こえ方が
   「たまに切り替わる」から「常につながっている」に変わるはずなので、
   その体感を記録する。
3. 遅延には影響しない値のはずなので、遅延表示が動かないことも確認する
   (動いていたら回帰)。

---

## 実験: 出力用途(ユーザー補助ストリーム)

「詳細設定」の **出力をユーザー補助の音量系統に載せる** を ON にすると、
本アプリの出力がメディア音量ではなく「ユーザー補助」の音量系統で鳴る
(`AudioAttributes.USAGE_ACCESSIBILITY`)。メディア音量をミュートしても
本アプリの音だけを鳴らせるかを確かめるための実験的な設定。

**確認手順:**

1. ON にして開始する(設定の反映に再起動が要る)。
2. 端末のメディア音量をミュート(0)にする。
3. 本アプリの音(マイク経由・捕獲音)が鳴り続けるかを確認する。
4. 端末の音量キーがユーザー補助音量を操作するようになっているか
   (メディア音量のスライダではなく)も確認する。

結果はここに追記する(未検証)。ON のあいだ、Android のユーザー補助機能
(TalkBack 等)の音声出力と競合しないかも合わせて確認したい。

---

## 設計上の判断

### 全二重は出力コールバック駆動

入力と出力の両方にコールバックを置くと、2 本のコールバックスレッドを同期させる必要があり、
どちらかがブロックした瞬間に破綻する。Oboe が推奨する構成(`samples/LiveEffect` と同じ)に従い、
**出力ストリームだけがデータコールバックを持ち、その中で入力を `read(..., timeoutNanos=0)`
で非ブロッキングに読む**。

起動直後は入力 FIFO に古い音がたまっていて、そのまま読むと遅延がまるごと乗る。そこで
3 段階で同期を取る(`AudioBridge::Step`):

1. **Drain** — 入力が空になるまで読んで捨てる。「捨てるものが無かった」コールバックを
   8 回数えたら次へ。
2. **Cushion** — 逆に 1 バースト分だけ読まずにためる。出力コールバックのジッタで
   入力が枯れて途切れるのを防ぐ余裕。
3. **Render** — 通常運転。

入力が要求より少ししか読めなかった場合(アンダーラン)は、**読めた分を新しい側に寄せ、
足りない分は先頭を 0 で埋める**。こうすると最新サンプルと最新の出力フレームの対応が保たれ、
遅延が伸びない。回数は診断表示に出る。

### ストリームの設定

- 出力: `PerformanceMode::LowLatency` / `SharingMode::Exclusive`(不可なら `Shared`)/
  `AudioFormat::Float` / `Usage::Media` + `ContentType::Music`。
  `VoiceCommunication` にすると AEC と受話口ルーティングが有効になり、遅延も音質も悪化する。
- 入力: 同じサンプルレート、`InputPreset::Unprocessed`(不可なら `VoiceRecognition` →
  `Generic`)。Unprocessed は AGC / ノイズ抑制 / AEC をすべて切る。
- `setFramesPerDataCallback` はバースト長。バースト長は開いてみないと分からないため、
  **一度開いて `getFramesPerBurst()` と `getSampleRate()` を読み、閉じてから本開きする**。
- 出力バッファはバースト 2 個分。`prepare()` は出力ストリームの実サンプルレートで行う。

### リアルタイム安全

`onAudioReady()` と `AudioBridge::render()` / `nextStep()` は、
ヒープ確保 / 解放・ロック・ファイル I/O・ログ出力・システムコール・例外を **一切行わない**。
バッファは `start()` で確保して以後サイズを変えない。パラメータの受け渡しは
`prism::PitchShifter` 内部の `std::atomic` のみ。JNI のセッターは動作中に呼んでよい。

`-ffast-math` は **使わない**。`PitchShifter` は `std::isfinite` による非有限値の弾きと
到達スナップによるデノーマル対策を自前で持っており、fast-math はその前提を壊す。
オフライン検証(`verify/`)が素の `-O2` で緑になっている以上、Android 側も同じ数値挙動で
ビルドしておかないと「検証済み」と言えない。

### 出力ゲインとソフトクリップ

v0.1.0 にはなかった出力音量調整を `AudioBridge`(`android/app/src/main/cpp/AudioBridge.h`)
に追加した。DSP コア(`dsp/`)は触らない方針のため、ゲインとソフトクリップは
`PitchShifter::process()` の**後**、プラットフォーム接着層側で適用する。

- ゲインは倍率で `std::atomic<float>`(既定 1.0 = 0dB、範囲 0.5〜4.0 = −6dB〜+12dB)。
  範囲外・非有限値は `AudioBridge::setOutputGain()` が clamp / 既定値へ丸める。
  dB ⇔ 倍率の変換は Kotlin 側(`Params.dbToGain` / `gainToDb`)の責務。
- ソフトクリップは `|x| <= 0.9` はそのまま通し、それを超える分だけ `tanh` で
  ±1.0 に漸近させる(3 次多項式ではなく `tanh`: 単調で飽和が滑らかで、
  ゲインをどれだけ上げても出力が ±1.0 を超えない)。
- どちらも `render()` のチャンクループ内、`shifter_.process()` の直後で適用する。
  ヒープ確保・ロックなし(RT 安全)。`build-smoke.sh` の `audio_bridge_smoke.cpp`
  テスト [8] がゲインの反映・clamp・ソフトクリップの飽和を検証する。

### 捕獲音のミックス(AudioPlaybackCapture)

v0.4.0 で、**他アプリの再生音**をマイクと並ぶ第 2 の音源として受け取れるようにした。
捕獲は `AudioPlaybackCaptureConfiguration` + `AudioRecord`(Java 側)でしか開けない —
Oboe / AAudio にはこの API が無い。したがってネイティブ側は「Java の録音スレッドから
サンプルを受け取る口」だけを持つ。

```
Java: MediaProjection -> AudioRecord -> NativeEngine.pushCapture(FloatArray, frames, channels)
                                              |
                                    ロックフリー SPSC リング(1 秒ぶん)
                                              |
音声スレッド: onAudioReady -> AudioBridge::render()
                 マイク経路 (PitchShifter, sweep 9.5ms) x micGain
               + 捕獲経路   (PitchShifter, sweep 40ms)  x captureGain
               -> 出力ゲイン -> ソフトクリップ
```

- **走査幅が経路ごとに違う**。捕獲音には「イヤホンから漏れる生音」が存在しないため
  10ms の遅延予算が効かない。走査幅を 40ms に広げると跳躍間隔(= `sweep ÷ |1−比|`)が
  4 倍になり、シフト量が大きいときのざらつきが減る(根拠はリポジトリ直下 README の D-A2)。
- **シフト量 / dry-wet / クロスフェードは 2 本のシフタへ同じ値が流れる**
  (`PrismEngine` のセッターが両方へ書く)。違うのは走査幅だけ。
- **読み出しの状態機械**: 有効化直後とアンダーラン後は約 20ms たまるまで無音、
  たまったら毎コールバック `numFrames` ぶん pop、滞留が 200ms を超えたら古い分を
  捨てて追いつく(捕獲側と出力側のクロック差への対策)。
- **Kotlin 側の注意**:
  - `AudioRecord` は **エンジンの出力と同じサンプルレート**(`streamInfo().sampleRate`)で
    開くこと。違うと再生速度がずれる。フォーマットは `ENCODING_PCM_FLOAT`。
  - `pushCapture` は録音スレッドから直接呼ぶ(1 秒に 50〜100 回想定)。
    JNI は `GetPrimitiveArrayCritical` で配列を直接読むのでコピーは発生しない。
  - `setCaptureEnabled(false)` のあいだ `pushCapture` は 0 を返す(何も書かない)。
    再び true にすると、リングは空・シフタは初期状態から始まる。
  - `micGain = 0.0` でマイクを完全ミュートでき、「捕獲音だけを聴く」構成になる。
  - 診断は `streamInfo()` の `captureUnderruns` / `captureOverruns` / `captureFillFrames`。
- ホストスモーク `audio_bridge_smoke.cpp` のテスト [9]〜[12] がリングの順序保証・
  クッション・ミックス・under/overrun カウントを検証する。

### 出力先 / 入力元のデバイス指定と用途

`PrismEngine::setOutputDeviceId` / `setInputDeviceId`(0 = 自動)と `setOutputUsage`
(0 = Media/Music、1 = AssistanceAccessibility/Speech)を追加した。いずれも
**次の `start()` から**有効。指定した ID で開けなかった場合は自動(0)で開き直し、
`streamInfo()` の `outputDeviceFallback` / `inputDeviceFallback` が true になる
(UI が「指定先で開けませんでした」と出せる)。実際に開いたデバイスは
`outputDeviceId` / `inputDeviceId` に入る。

用途 1 の狙いは「メディア音量を絞っても本アプリの出力だけ鳴らす」実験。
Exclusive が取れなければ従来どおり Shared に落ちる。

### マイク経路の走査幅(聴感ヒアリング用)

`NativeEngine.setMicSweepMs(Double)`(既定 9.5、次の `start()` から有効)。
広げるほど低域のピッチ精度と跳躍間隔が改善する代わりに遅延が増える
(設計値遅延 ≈ 8 サンプル + 走査幅の半分)。採用値は `latency().micSweepMs`、
そのときの DSP 遅延は従来どおり `latency().dspMs`。範囲はネイティブ側で
2.0〜100.0ms に clamp される。

### スライダの範囲と刻み幅

`prism::PitchShifter` は `kShiftCentsMin = -1200` 〜 `kShiftCentsMax = +1200` を
そのまま受け付ける(clamp では頭打ちにならない)。UI のスライダは**常にこの全域**
(−1200 〜 +1200)を表示し、v0.1.0 にあった「スライダの範囲」切替(微調整 / 半音 /
全音 / オクターブで表示幅そのものを変える方式)は廃止した。代わりに −/+ ボタン
1 回ぶんの変化量を切り替える **刻み幅**(全音 200 / 半音 100 / 1/4 音 50 / 1/8 音 25、
既定 1/8 音)を用意している。表示幅を固定したことで、値がプリセット幅の外に出て
スライダごと動く、という v0.1.0 の挙動(web 版由来)は無くなった。

### Edge-to-edge と inset

Pixel 9a 実機で v0.1.0 を確認したところ、画面上端がステータスバー(時計・電池)と
重なる不具合が見つかった。`fitsSystemWindows="true"` だけでは、ヘッダ内の要素の
位置までは調整してくれない。v0.2.0 では `WindowCompat.setDecorFitsSystemWindows(window,
false)` で明示的に edge-to-edge にした上で、`ViewCompat.setOnApplyWindowInsetsListener`
が `systemBars() | displayCutout()` の上下 inset を**ヘッダ行の paddingTop**と
**固定フッタの paddingBottom** にだけ加算する(左右は root の padding)。
ステータスバーのアイコン色は `WindowInsetsControllerCompat.isAppearanceLightStatusBars`
で、実行時に解決されたテーマ(ライト/ダーク)に合わせて切り替える。

### ライト/ダークテーマ

`Theme.Material3.DayNight.NoActionBar` を親にし、色トークンは `values/colors.xml`
(ライト、既定)と `values-night/colors.xml`(ダーク、v0.1.0 までの固定配色をそのまま
移した)に分けた。詳細設定の「テーマ」(システム / ライト / ダーク)は
`AppCompatDelegate.setDefaultNightMode()` を呼ぶ。実際に解決される夜間モードが変わると
Activity は自動的に再生成される。選択は `Params.themeMode` として永続化するが、
エンジンには渡さない(UI だけの設定のため `Params.applyTo()` は素通りする)。

### 常駐

`ForegroundService`(`microphone|mediaPlayback|mediaProjection`)。Android 14 以降は
`foregroundServiceType` ごとに個別の権限宣言が必要なため、`FOREGROUND_SERVICE_MICROPHONE` /
`FOREGROUND_SERVICE_MEDIA_PLAYBACK` / `FOREGROUND_SERVICE_MEDIA_PROJECTION` を宣言している
(宣言に無い型だと `ForegroundServiceTypeException` になりうる)。マニフェストは 3 つとも
宣言するが、実際に `startForeground()` へ渡す型は `Params.captureEnabled` に応じて
microphone + mediaPlayback の 2 つか、そこに mediaProjection を足した 3 つかを動的に選ぶ
(`PrismService.startForegroundCompat()`)。捕獲を動作中に ON にした場合は、
`MediaProjectionManager.getMediaProjection()` を呼ぶ **前** に mediaProjection 型を含めて
前景化し直す(順序を逆にすると Android 14 以降で `SecurityException` になる)。

Activity は Service に bind して状態を読むだけで、エンジンの所有者は Service。
`START_NOT_STICKY` にしてあるので、システムに殺されても勝手にマイクを開き直さない。

---

## アイコン

前景(`ic_launcher_foreground`)は `web/assets/icon-foreground.svg` をヘッドレス Chrome で
密度ごとに PNG 化したもの:`mipmap-{mdpi,hdpi,xhdpi,xxhdpi,xxxhdpi}/ic_launcher_foreground.png`
= 108 / 162 / 216 / 324 / 432 px、`--default-background-color=00000000` で背景を透過して書き出す。

monochrome 層(`ic_launcher_monochrome.xml`、Android 13 以降のテーマアイコン用)と
通知アイコン(`ic_notification.xml`)は、`logo.svg` の魚体パスから起こした白シルエットの
VectorDrawable。どちらも PNG ではなくベクタなので密度別ファイルは不要。

## よくある質問

**Q. シフト量を大きくすると音が歪む / こもる。故障ですか?**

仕様です。ディレイライン方式で処理遅延を 10ms 以内に収めるため、読み出しヘッドの
走査幅は 9.5ms 固定にしてあります。ヘッドが端に着いてもう一方へ切り替わる「跳躍」の
間隔は `9.5ms ÷ |1 − 速度比|` で決まり、既定の −89 セントでは速度比が 1 に近いため
跳躍間隔は約 190ms とまばらで、つなぎ目はほぼ聞こえません。しかし ±1200 セントまで
シフト量を大きくすると速度比が 1 から離れ、跳躍間隔は 9.5〜19ms まで縮みます。
上げ方向はこの跳躍が周期的なざらつき(歪み)として、下げ方向は 2 本のヘッドが常時
クロスフェードし続ける状態のくし形フィルタと区間の飛ばしとして(こもり・聴き取り
にくさ)聞こえ方に現れます。改善する唯一の手段は走査幅を伸ばすことですが、それは
そのまま遅延を増やすことと同義でトレードオフの関係にあります。本アプリの目的は
−89 セント前後の使用なので、大きなシフト量は実験用途と割り切っています。

**Q. システム音声や他アプリの音を拾って変換できますか?**

v0.4.0 から、Android 10 以降の AudioPlaybackCapture(MediaProjection)を使って
「他アプリの音を拾う」機能を追加しました。「音源」カードのスイッチを ON にすると、
他のアプリが再生している音(YouTube や音楽アプリなど)もマイクの音と同じように
ピッチシフトして混ぜられます。

v0.3.0 で見送っていた理由(元の音がそのまま二重に聞こえる)は解消していません
——この API では元の音を止められないためです。そこで運用でカバーします。
**原音を「ダミーの Bluetooth イヤホン」に流し、本アプリの出力先には有線(USB)
イヤホンを名指しで選ぶ**ことで、耳には変換した音だけが届くようにします。
詳しくは下の「他アプリの音を拾う」節を参照してください。

そのほかの制約は変わりません。DRM 付きの音声や、キャプチャを拒否する設定のアプリの
音は拾えず、アプリの起動のたびに画面キャプチャの同意ダイアログへの応答が必要です。
捕獲経路には生音の漏れ込みが無いため 10ms の遅延予算の対象外とし、走査幅を
別枠で広め(既定 40ms)に取ることで遅延要件と両立させています(設計の詳細は
下の「捕獲音のミックス」節)。

## 既知の制限

- **Bluetooth イヤホンは実用にならない。** A2DP / LE Audio のコーデック遅延だけで
  100〜200ms あり、この用途の要件(往復 20ms 以下)を満たしようがない。**有線を使うこと。**
- **AEC(エコーキャンセラ)は意図的に無効。** 音程を正しく通すために `Unprocessed` を
  要求しているため、スピーカー出力ではハウリングする。イヤホン必須。
- **端末依存。** `SharingMode::Exclusive` と `InputPreset::Unprocessed` が取れるかは端末次第。
  取れなかった場合は自動で `Shared` / `VoiceRecognition` に落ちるが、その分遅延が増える。
  実際に何が取れたかは「詳細設定」の診断行に出る。
- **遅延表示は推定値。** Oboe の `calculateLatencyMillis()`(入力 + 出力)に DSP の
  設計値遅延を足したもの。デバイス側の物理的な遅延(D/A、イヤホン)は含まない。
  ストリーム開始直後はタイムスタンプが揃わず「測定中」と出る。
- **`arm64-v8a` のみ。** 32bit 端末とエミュレータ(x86_64)では動かない。
- **初回の権限。** マイクを許可しないと開始できない。通知を拒否すると常駐中の
  「停止」ボタンが通知に出ない(アプリからは止められる)。
- **他アプリの音を拾う設定は起動のたびに同意が要る。** MediaProjection のトークンは
  プロセス単位で、永続的な許可にはできない仕様(OS 側の制約)。
- **一部のアプリ・DRM 付き音声は拾えない。** 詳しくは上の「他アプリの音を拾う」節。

## 設定項目一覧(v0.4.0 時点)

| 設定 | 範囲 | 既定値 | 反映タイミング |
|---|---|---|---|
| シフト量(L / R) | −1200 〜 +1200 セント | −89 | 即時 |
| 原音まぜ(Dry/Wet) | 0.0 〜 1.0 | 1.0 | 即時 |
| なめらかさ(窓長) | 10 〜 200 ms | 50 | 即時 |
| 刻み幅(−/+ 1 回ぶん) | 全音 / 半音 / 1/4 音 / 1/8 音 | 1/8 音 | 即時 |
| 出力音量 | −6 〜 +12 dB | 0 dB | 即時 |
| マイク音量 | −30(OFF)〜 +12 dB | 0 dB | 即時 |
| 他アプリの音を拾う | ON / OFF | OFF | 即時(実際の捕獲開始は同意ダイアログ後) |
| 他アプリの音の音量 | −12 〜 +12 dB | 0 dB | 即時 |
| 出力先デバイス | 自動 / 一覧から選択 | 自動 | 次の開始から(動作中の変更は自動で再起動) |
| 入力元デバイス | 自動 / 一覧から選択 | 自動 | 同上 |
| 出力用途(実験) | メディア / ユーザー補助 | メディア | 同上 |
| マイク経路の走査幅 | 5 / 9.5 / 15 / 20 / 30 / 40 / 60 ms | 9.5 ms | 同上 |
| テーマ | システム / ライト / ダーク | システム | 即時(Activity 再生成) |

## 未検証

v0.1.0 は Pixel 9a 実機(有線イヤホン)で動作確認済み(ほぼ遅延なし)。

v0.4.0 で追加した以下の項目は、実機でまだ一度も確認していない:

- 「他アプリの音を拾う」の実際の動作(同意ダイアログ、捕獲音のミックス、
  ダミー Bluetooth イヤホン運用での二重聞こえの解消)
- マイク音量スライダの「OFF」位置(完全ミュート)の実際の聴感
- 出力先 / 入力元デバイススピナーの一覧内容・選択・抜き差し追随
- デバイス指定 / 出力用途 / 走査幅を動作中に変更した際の自動再起動(stop → start)
  と、捕獲 ON 時に同意ダイアログが再び出る挙動
- 出力用途(ユーザー補助ストリーム)の実験結果(上の「実験」節)
- 走査幅プリセット(5〜60ms)を切り替えたときの聴感の違い
- なめらかさ(窓長)を 200ms まで伸ばしたときの聴感
- Android 14 以降での `startForeground` 型の動的な増減(mediaProjection の
  有無での再宣言)が実際にクラッシュなく動くか

v0.2.0 で追加・変更した以下の項目は、実機でまだ一度も確認していない:

- 出力音量調整とソフトクリップの実際の聴感(歪み方・上げすぎたときの挙動)
- 刻み幅切替(全音 / 半音 / 1/4 音 / 1/8 音)の操作性
- ライト/ダークテーマの実際の見え方とコントラスト、テーマ切替時の Activity 再生成
- edge-to-edge 化と inset 適用後、Pixel 9a のステータスバー重なりが実際に解消したか
- ⓘ 情報ボタン(BottomSheetDialog)の表示・操作性
- Material Slider への置き換えによる操作感の変化(つまみの大きさ・ドラッグ精度)

v0.1.0 の時点で以下は未検証のままである:

- 長時間動作時のグリッチとアンダーラン発生率
- Exclusive / Unprocessed が Pixel で実際に取れるか
- 画面消灯時とアプリ切り替え時にストリームが維持されるか
- ヘッドセットの抜き差し(デバイス切り替え)での自動再起動の挙動
- 通知の「停止」アクションの動作

確認できているのは「debug APK がビルドでき、`libprism.so` が `arm64-v8a` に入る」ことと、
`build-smoke.sh` が通る範囲のロジックだけ。
