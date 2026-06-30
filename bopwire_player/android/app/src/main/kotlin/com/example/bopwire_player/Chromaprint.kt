package com.example.bopwire_player

/**
 * Thin Kotlin wrapper around the chromaprint JNI bridge in
 * `cpp/chromaprint_jni.cpp`. Lets [MainActivity]'s decode loop stream
 * MediaCodec PCM straight into chromaprint without bouncing through
 * Dart, which saves a 30 MB PCM copy + MethodChannel marshalling per
 * song.
 *
 * Usage:
 * ```
 * val fp = Chromaprint().use { fp ->
 *     fp.start(sampleRate, channels)
 *     fp.feed(pcmChunk, count)
 *     // ... feed more chunks ...
 *     fp.finish()
 *     fp.fingerprint()
 * }
 * ```
 *
 * Errors are thrown as IllegalStateException so the surrounding
 * MethodChannel handler can report them through `result.error(...)`.
 */
class Chromaprint : AutoCloseable {
    private var handle: Long = nativeNew()

    init {
        if (handle == 0L) {
            throw IllegalStateException("chromaprint_new returned null")
        }
    }

    /** Configure the algorithm for the given input format. */
    fun start(sampleRate: Int, channels: Int) {
        if (!nativeStart(handle, sampleRate, channels)) {
            throw IllegalStateException(
                "chromaprint_start rejected sr=$sampleRate ch=$channels")
        }
    }

    /**
     * Feed `count` interleaved 16-bit samples. Pass a reused array so
     * we don't allocate per chunk. The native side uses
     * GetPrimitiveArrayCritical so there's no copy.
     */
    fun feed(samples: ShortArray, count: Int) {
        if (count <= 0) return
        if (!nativeFeedShorts(handle, samples, count)) {
            throw IllegalStateException("chromaprint_feed failed")
        }
    }

    /** Signal end-of-stream. Must precede [fingerprint]. */
    fun finish() {
        if (!nativeFinish(handle)) {
            throw IllegalStateException("chromaprint_finish failed")
        }
    }

    /**
     * Get the compressed fingerprint string. Caller must have already
     * called [finish]. Returned text is chromaprint's URL-safe-base64
     * encoding — what the full node expects in `fingerprint.submit`.
     */
    fun fingerprint(): String {
        return nativeGetFingerprint(handle)
            ?: throw IllegalStateException("chromaprint_get_fingerprint failed")
    }

    override fun close() {
        if (handle != 0L) {
            nativeFree(handle)
            handle = 0L
        }
    }

    companion object {
        init {
            System.loadLibrary("chromaprint_jni")
        }
        @JvmStatic external fun nativeNew(): Long
        @JvmStatic external fun nativeStart(
            handle: Long, sampleRate: Int, channels: Int): Boolean
        @JvmStatic external fun nativeFeedShorts(
            handle: Long, data: ShortArray, count: Int): Boolean
        @JvmStatic external fun nativeFinish(handle: Long): Boolean
        @JvmStatic external fun nativeGetFingerprint(handle: Long): String?
        @JvmStatic external fun nativeFree(handle: Long)
    }
}
