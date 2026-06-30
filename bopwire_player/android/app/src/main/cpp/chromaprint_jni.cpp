// Tiny JNI bridge so the Kotlin decode loop can stream PCM directly
// into chromaprint without round-tripping the full PCM buffer through
// MethodChannel → Dart → FFI. Saves a 30 MB allocation + transfer per
// 3-minute song and ~500 ms of marshalling + Dart-side copy.
//
// Lifetime model: Kotlin holds an opaque jlong context pointer (cast to
// ChromaprintContext*). Create / start / feed / finish / getFingerprint
// / free are all thin wrappers around the chromaprint C API.

#include <jni.h>
#include <chromaprint.h>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_example_bopwire_1player_Chromaprint_nativeNew(
        JNIEnv* /*env*/, jclass /*klass*/) {
    ChromaprintContext* ctx =
        chromaprint_new(CHROMAPRINT_ALGORITHM_DEFAULT);
    return reinterpret_cast<jlong>(ctx);
}

JNIEXPORT jboolean JNICALL
Java_com_example_bopwire_1player_Chromaprint_nativeStart(
        JNIEnv* /*env*/, jclass /*klass*/,
        jlong handle, jint sampleRate, jint channels) {
    auto* ctx = reinterpret_cast<ChromaprintContext*>(handle);
    if (!ctx) return JNI_FALSE;
    return chromaprint_start(ctx, sampleRate, channels) == 1
               ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_example_bopwire_1player_Chromaprint_nativeFeedShorts(
        JNIEnv* env, jclass /*klass*/,
        jlong handle, jshortArray data, jint count) {
    auto* ctx = reinterpret_cast<ChromaprintContext*>(handle);
    if (!ctx || !data || count <= 0) return JNI_FALSE;
    // (#crash) Never read past the array — `count` comes from Kotlin and a
    // stale/oversized value would over-read the buffer.
    const jsize arrLen = env->GetArrayLength(data);
    if (count > arrLen) count = arrLen;
    if (count <= 0) return JNI_FALSE;
    // (#crash) Use GetShortArrayElements, NOT GetPrimitiveArrayCritical: the
    // critical variant disables GC for the whole span, and holding it across
    // the slow chromaprint_feed can trip a CheckJNI / GC-safepoint-timeout
    // abort (SIGABRT) on Android. The elements API is safe to hold across
    // slow work (it may copy, which is an acceptable cost for not crashing).
    jshort* elems = env->GetShortArrayElements(data, nullptr);
    if (!elems) return JNI_FALSE;
    const int rc = chromaprint_feed(
        ctx, reinterpret_cast<int16_t*>(elems), count);
    env->ReleaseShortArrayElements(data, elems, JNI_ABORT);
    return rc == 1 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_example_bopwire_1player_Chromaprint_nativeFinish(
        JNIEnv* /*env*/, jclass /*klass*/, jlong handle) {
    auto* ctx = reinterpret_cast<ChromaprintContext*>(handle);
    if (!ctx) return JNI_FALSE;
    return chromaprint_finish(ctx) == 1 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_example_bopwire_1player_Chromaprint_nativeGetFingerprint(
        JNIEnv* env, jclass /*klass*/, jlong handle) {
    auto* ctx = reinterpret_cast<ChromaprintContext*>(handle);
    if (!ctx) return nullptr;
    char* out = nullptr;
    if (chromaprint_get_fingerprint(ctx, &out) != 1 || !out) {
        if (out) chromaprint_dealloc(out);
        return nullptr;
    }
    jstring result = env->NewStringUTF(out);
    chromaprint_dealloc(out);
    return result;
}

JNIEXPORT void JNICALL
Java_com_example_bopwire_1player_Chromaprint_nativeFree(
        JNIEnv* /*env*/, jclass /*klass*/, jlong handle) {
    auto* ctx = reinterpret_cast<ChromaprintContext*>(handle);
    if (ctx) chromaprint_free(ctx);
}

} // extern "C"
