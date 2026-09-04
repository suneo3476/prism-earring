plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "dev.saku.prismearring"
    compileSdk = 35
    // AGP 8.6.1 の既定は 34.0.0。compileSdk 35 に合わせて明示的に固定する
    // (放っておくと build-tools 34 が余分にダウンロードされる)。
    buildToolsVersion = "35.0.0"
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "dev.saku.prismearring"
        minSdk = 30
        targetSdk = 35
        versionCode = 3
        versionName = "0.3.0"

        // Pixel(64bit ARM)のみを対象にする。APK を小さく保ち、
        // 検証できない ABI のバイナリを配らない。
        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                // C++17 / 例外・RTTI は使わない(リアルタイム経路の鉄則)。
                arguments += listOf("-DANDROID_STL=c++_shared")
                cppFlags += listOf("-std=c++17")
            }
        }
    }

    buildFeatures {
        // Oboe を Maven の prefab で取り込む。
        prefab = true
        viewBinding = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        jniLibs {
            // libc++_shared.so は 1 つだけ。
            useLegacyPackaging = false
        }
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.constraintlayout)
    implementation(libs.oboe)
}
