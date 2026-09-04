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

// [0]=入力 ms, [1]=出力 ms, [2]=DSP ms, [3]=合計 ms, [4]=有効なら 1
JNIEXPORT jdoubleArray JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeGetLatency(JNIEnv* env, jclass /*clazz*/,
                                                         jlong handle) {
    jdoubleArray out = env->NewDoubleArray(5);
    if (out == nullptr) {
        return nullptr;  // OutOfMemoryError は JNI が投げている
    }
    prism::PrismEngine* engine = toEngine(handle);
    jdouble values[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
    if (engine != nullptr) {
        const prism::PrismEngine::LatencyReport r = engine->latency();
        values[0] = r.inputMillis;
        values[1] = r.outputMillis;
        values[2] = r.dspMillis;
        values[3] = r.totalMillis;
        values[4] = r.valid ? 1.0 : 0.0;
    }
    env->SetDoubleArrayRegion(out, 0, 5, values);
    return out;
}

// [0]=サンプルレート, [1]=入力ch, [2]=出力ch, [3]=バースト長,
// [4]=アンダーラン数, [5]=入力エラー数, [6]=Exclusive なら 1, [7]=Unprocessed なら 1
JNIEXPORT jintArray JNICALL
Java_dev_saku_prismearring_NativeEngine_nativeGetStreamInfo(JNIEnv* env, jclass /*clazz*/,
                                                            jlong handle) {
    jintArray out = env->NewIntArray(8);
    if (out == nullptr) {
        return nullptr;
    }
    prism::PrismEngine* engine = toEngine(handle);
    jint values[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (engine != nullptr) {
        values[0] = engine->sampleRate();
        values[1] = engine->inputChannelCount();
        values[2] = engine->outputChannelCount();
        values[3] = engine->framesPerBurst();
        values[4] = engine->underrunCount();
        values[5] = engine->inputErrorCount();
        values[6] = engine->usingExclusiveMode() ? 1 : 0;
        values[7] = engine->usingUnprocessedInput() ? 1 : 0;
    }
    env->SetIntArrayRegion(out, 0, 8, values);
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
