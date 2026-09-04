// jni_bridge.cpp — Kotlin (dev.saku.prismearring.NativeEngine) との唯一の接点。
//
// ここは音声スレッドからは呼ばれない。エンジンのハンドル(jlong)を Kotlin 側が
// 持ち、Service のライフサイクルに合わせて create / destroy する。
// パラメータのセッターは PitchShifter 内部の std::atomic に直接書くだけなので、
// 動作中に呼んでもロックもグリッチも起きない。

#include <jni.h>

#include <new>
#include <string>

#include "PrismEngine.h"

namespace {

prism::PrismEngine* toEngine(jlong handle) {
    return reinterpret_cast<prism::PrismEngine*>(handle);
}

jstring toJString(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeCreate(JNIEnv* /*env*/, jclass /*clazz*/) {
    // 確保失敗は例外ではなく 0 で返す(Kotlin 側で判定する)。
    prism::PrismEngine* engine = new (std::nothrow) prism::PrismEngine();
    return reinterpret_cast<jlong>(engine);
}

JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeDestroy(JNIEnv* /*env*/, jclass /*clazz*/,
                                                      jlong handle) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine == nullptr) {
        return;
    }
    engine->stop();
    delete engine;
}

JNIEXPORT jboolean JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeStart(JNIEnv* /*env*/, jclass /*clazz*/,
                                                    jlong handle) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine == nullptr) {
        return JNI_FALSE;
    }
    return engine->start() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeStop(JNIEnv* /*env*/, jclass /*clazz*/,
                                                   jlong handle) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine != nullptr) {
        engine->stop();
    }
}

JNIEXPORT jboolean JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeIsRunning(JNIEnv* /*env*/, jclass /*clazz*/,
                                                        jlong handle) {
    prism::PrismEngine* engine = toEngine(handle);
    return (engine != nullptr && engine->isRunning()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeIsSynced(JNIEnv* /*env*/, jclass /*clazz*/,
                                                       jlong handle) {
    prism::PrismEngine* engine = toEngine(handle);
    return (engine != nullptr && engine->isSynced()) ? JNI_TRUE : JNI_FALSE;
}

// channel: 0 = L, 1 = R, それ以外 = 両方
JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeSetShiftCents(JNIEnv* /*env*/, jclass /*clazz*/,
                                                            jlong handle, jint channel,
                                                            jfloat cents) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine != nullptr) {
        engine->setShiftCents(static_cast<int>(channel), static_cast<float>(cents));
    }
}

JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeSetDryWet(JNIEnv* /*env*/, jclass /*clazz*/,
                                                        jlong handle, jfloat mix) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine != nullptr) {
        engine->setDryWet(static_cast<float>(mix));
    }
}

JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeSetCrossfadeMs(JNIEnv* /*env*/, jclass /*clazz*/,
                                                             jlong handle, jfloat ms) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine != nullptr) {
        engine->setCrossfadeMs(static_cast<float>(ms));
    }
}

// gain は倍率(0.5〜4.0)。dB <-> 倍率の変換は Kotlin 側(Params)で行う。
JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeSetOutputGain(JNIEnv* /*env*/, jclass /*clazz*/,
                                                            jlong handle, jfloat gain) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine != nullptr) {
        engine->setOutputGain(static_cast<float>(gain));
    }
}

// ---- 捕獲経路(AudioPlaybackCapture) ---------------------------------------
// data はインタリーブ float、channels ch、frames フレーム。
// Java の録音スレッドから 1 秒に 50〜100 回程度呼ばれる想定なので、
// GetPrimitiveArrayCritical でコピーを 1 回も挟まずにネイティブへ渡す
// (この区間では他の JNI 呼び出しを一切行わない)。
// 戻り値は実際に書けたフレーム数。
JNIEXPORT jint JNICALL
Java_dev_saku_prismearring_NativeEngine_nativePushCapture(JNIEnv* env, jclass /*clazz*/,
                                                          jlong handle, jfloatArray data,
                                                          jint frames, jint channels) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine == nullptr || data == nullptr || frames <= 0 || channels < 1) {
        return 0;
    }
    // 配列より長い要求は切り詰める(範囲外読み出しを防ぐ)。
    const jsize length = env->GetArrayLength(data);
    if (static_cast<jlong>(frames) * channels > static_cast<jlong>(length)) {
        frames = length / channels;
    }
    if (frames <= 0) {
        return 0;
    }
    void* raw = env->GetPrimitiveArrayCritical(data, nullptr);
    if (raw == nullptr) {
        return 0;
    }
    const int written = engine->pushCapture(static_cast<const float*>(raw),
                                            static_cast<int>(frames),
                                            static_cast<int>(channels));
    // JNI_ABORT: 配列は読むだけなので書き戻しは不要。
    env->ReleasePrimitiveArrayCritical(data, raw, JNI_ABORT);
    return static_cast<jint>(written);
}

JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeSetCaptureEnabled(JNIEnv* /*env*/, jclass /*clazz*/,
                                                                jlong handle, jboolean enabled) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine != nullptr) {
        engine->setCaptureEnabled(enabled == JNI_TRUE);
    }
}

// gain は倍率(0.0〜2.0)。0.0 でマイクを完全ミュートする。
JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeSetMicGain(JNIEnv* /*env*/, jclass /*clazz*/,
                                                         jlong handle, jfloat gain) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine != nullptr) {
        engine->setMicGain(static_cast<float>(gain));
    }
}

// gain は倍率(0.0〜4.0)。
JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeSetCaptureGain(JNIEnv* /*env*/, jclass /*clazz*/,
                                                             jlong handle, jfloat gain) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine != nullptr) {
        engine->setCaptureGain(static_cast<float>(gain));
    }
}

// ---- 次の start() から効く設定 ----------------------------------------------
// 0 = 自動。指定 ID で開けなければ自動で開き直す(結果は nativeGetStreamInfo)。
JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeSetOutputDeviceId(JNIEnv* /*env*/, jclass /*clazz*/,
                                                                jlong handle, jint id) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine != nullptr) {
        engine->setOutputDeviceId(static_cast<int32_t>(id));
    }
}

JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeSetInputDeviceId(JNIEnv* /*env*/, jclass /*clazz*/,
                                                               jlong handle, jint id) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine != nullptr) {
        engine->setInputDeviceId(static_cast<int32_t>(id));
    }
}

// usage: 0 = Media/Music(既定), 1 = AssistanceAccessibility/Speech
JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeSetOutputUsage(JNIEnv* /*env*/, jclass /*clazz*/,
                                                             jlong handle, jint usage) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine != nullptr) {
        engine->setOutputUsage(static_cast<int>(usage));
    }
}

// マイク経路の走査幅(ms)。範囲外は PitchShifter が clamp する。次の start() から有効。
JNIEXPORT void JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeSetMicSweepMs(JNIEnv* /*env*/, jclass /*clazz*/,
                                                            jlong handle, jdouble ms) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine != nullptr) {
        engine->setMicSweepMs(static_cast<double>(ms));
    }
}

// [0]=入力 ms, [1]=出力 ms, [2]=DSP ms(マイク経路), [3]=合計 ms, [4]=有効なら 1,
// [5]=マイク経路の走査幅 ms, [6]=捕獲経路の走査幅 ms
JNIEXPORT jdoubleArray JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeGetLatency(JNIEnv* env, jclass /*clazz*/,
                                                         jlong handle) {
    constexpr jsize kCount = 7;
    jdoubleArray out = env->NewDoubleArray(kCount);
    if (out == nullptr) {
        return nullptr;  // OutOfMemoryError は JNI が投げている
    }
    prism::PrismEngine* engine = toEngine(handle);
    jdouble values[kCount] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    if (engine != nullptr) {
        const prism::PrismEngine::LatencyReport r = engine->latency();
        values[0] = r.inputMillis;
        values[1] = r.outputMillis;
        values[2] = r.dspMillis;
        values[3] = r.totalMillis;
        values[4] = r.valid ? 1.0 : 0.0;
        values[5] = r.micSweepMillis;
        values[6] = r.captureSweepMillis;
    }
    env->SetDoubleArrayRegion(out, 0, kCount, values);
    return out;
}

// nativeGetStreamInfo の配列レイアウト(追加は末尾のみ。既存の並びは変えない):
//   [0]  サンプルレート
//   [1]  入力ch
//   [2]  出力ch
//   [3]  バースト長
//   [4]  マイク経路のアンダーラン数
//   [5]  入力エラー数
//   [6]  Exclusive なら 1
//   [7]  Unprocessed なら 1
//   [8]  捕獲経路のアンダーラン数
//   [9]  捕獲経路のオーバーラン数(取りこぼした push の回数)
//   [10] 捕獲リングの現在の滞留フレーム数
//   [11] 実際に開いた出力デバイス ID(0 = 未取得)
//   [12] 実際に開いた入力デバイス ID(0 = 未取得)
//   [13] 出力が指定デバイスで開けず自動に落ちたなら 1
//   [14] 入力が指定デバイスで開けず自動に落ちたなら 1
//   [15] 出力の用途(0 = Media, 1 = AssistanceAccessibility)
JNIEXPORT jintArray JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeGetStreamInfo(JNIEnv* env, jclass /*clazz*/,
                                                            jlong handle) {
    constexpr jsize kCount = 16;
    jintArray out = env->NewIntArray(kCount);
    if (out == nullptr) {
        return nullptr;
    }
    prism::PrismEngine* engine = toEngine(handle);
    jint values[kCount] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (engine != nullptr) {
        values[0] = engine->sampleRate();
        values[1] = engine->inputChannelCount();
        values[2] = engine->outputChannelCount();
        values[3] = engine->framesPerBurst();
        values[4] = engine->underrunCount();
        values[5] = engine->inputErrorCount();
        values[6] = engine->usingExclusiveMode() ? 1 : 0;
        values[7] = engine->usingUnprocessedInput() ? 1 : 0;
        values[8] = engine->captureUnderruns();
        values[9] = engine->captureOverruns();
        values[10] = engine->captureFillFrames();
        values[11] = engine->actualOutputDeviceId();
        values[12] = engine->actualInputDeviceId();
        values[13] = engine->outputDeviceFallback() ? 1 : 0;
        values[14] = engine->inputDeviceFallback() ? 1 : 0;
        values[15] = engine->outputUsage();
    }
    env->SetIntArrayRegion(out, 0, kCount, values);
    return out;
}

JNIEXPORT jstring JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeGetLastError(JNIEnv* env, jclass /*clazz*/,
                                                           jlong handle) {
    prism::PrismEngine* engine = toEngine(handle);
    if (engine == nullptr) {
        return toJString(env, "エンジンが初期化されていません");
    }
    return toJString(env, engine->lastError());
}

}  // extern "C"
