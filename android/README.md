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
        MainActivity.kt          画面
      res/                       ダークテーマ。web/styles.css のトークンを踏襲
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
2. アプリを開き、下部の **▶ 開始** を押す。
3. 状態ピルが「停止中」→「同期中」→「動作中」と変わる。往復遅延の推定値が
   1 秒ごとに更新される。
4. シフト量を耳で合わせる。既定は **−89 セント**。
   - 太いスライダ、または左右の **−/+** で 1 セントずつ(オクターブ範囲のときは 10 セントずつ)。
   - **スライダの範囲** でスライダの粒度を切り替える(微調整 / 半音 / 全音 / オクターブ)。
   - **左右を別々に設定** を ON にすると L/R を独立に動かせる。
5. **窓長プリセット**(20 / 50 / 100 ms)で音質と滑らかさを切り替える。
   細かい値は「詳細設定」から。
6. 画面を消しても、他のアプリに移っても処理は続く。止めるときは
   アプリの **■ 停止**、または通知の **停止**。

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

### スライダの範囲

`prism::PitchShifter` は `kShiftCentsMin = -1200` 〜 `kShiftCentsMax = +1200` を
そのまま受け付ける(clamp では頭打ちにならない)。UI の「スライダの範囲」
(微調整 ±150 / 半音 ±100 / 全音 ±200 / オクターブ ±1200)は対称範囲
`[-N, +N]` としてスライダの粒度を切り替えるだけのプリセットで、DSP 側の
上限・下限そのものとは別物。範囲を切り替えた瞬間に現在値がプリセット幅の
外にあっても値はクランプせず、範囲のほうを現在値まで広げる(web 版と同じ
挙動)。オクターブ範囲のときだけ −/+ ボタンの刻みを 10 セントにする。

### 常駐

`ForegroundService`(`microphone|mediaPlayback`)。Android 14 以降は
`foregroundServiceType` ごとに個別の権限宣言が必要なため、`FOREGROUND_SERVICE_MICROPHONE`
に加えて `FOREGROUND_SERVICE_MEDIA_PLAYBACK` も宣言している(片方だけだと
`ForegroundServiceTypeException` になりうる)。

Activity は Service に bind して状態を読むだけで、エンジンの所有者は Service。
`START_NOT_STICKY` にしてあるので、システムに殺されても勝手にマイクを開き直さない。

---

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

## 未検証

実機もエミュレータも無い環境で作ったため、以下は **一度も動かして確認していない**:

- 実機でのレイテンシ実測(目標: 往復 20ms 以下)
- 長時間動作時のグリッチとアンダーラン発生率
- Exclusive / Unprocessed が Pixel で実際に取れるか
- 画面消灯時とアプリ切り替え時にストリームが維持されるか
- ヘッドセットの抜き差し(デバイス切り替え)での自動再起動の挙動
- 通知の「停止」アクションの動作

確認できているのは「debug APK がビルドでき、`libprism.so` が `arm64-v8a` に入る」ことと、
`build-smoke.sh` が通る範囲のロジックだけ。
