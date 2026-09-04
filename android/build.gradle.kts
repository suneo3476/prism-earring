// ルート。プラグインは apply false で版だけ固定する。
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.android) apply false
}
