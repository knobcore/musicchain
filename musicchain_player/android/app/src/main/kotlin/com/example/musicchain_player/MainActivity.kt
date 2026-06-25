package com.example.musicchain_player

import android.content.Context
import android.content.Intent
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import java.security.MessageDigest
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.ShortBuffer
import java.util.concurrent.Executors

// The player's Sprint 3 fingerprint flow runs entirely in Dart via FFI
// against libchromaprint.so, except for one step Dart can't easily do
// portably: decoding an arbitrary audio file to 16-bit PCM. Android
// provides MediaCodec / MediaExtractor for exactly this, but they only
// have a Java API. So we expose a tiny method channel that takes a file
// path and returns a ByteArray of interleaved 16-bit little-endian PCM
// plus the sample rate + channel count needed by chromaprint_start.
class MainActivity : FlutterActivity() {
    // Background pool for audio decode so MethodChannel calls don't
    // block the UI thread. Two workers lets two scans interleave
    // (rare in practice) without thrashing the CPU.
    private val decodeExecutor = Executors.newFixedThreadPool(2)
    private val mainHandler    = Handler(Looper.getMainLooper())

    private var networkChannel:   MethodChannel? = null
    private var connectivityMgr:  ConnectivityManager? = null
    private var networkCallback:  ConnectivityManager.NetworkCallback? = null

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        // Pin the swarm-link process so screen-off / app-backgrounded
        // doesn't terminate the librats client. Idempotent — the
        // service short-circuits if already started.
        MusicChainService.start(this)

        // Foreground service + wake lock aren't enough on cellular: Doze
        // throttles the Dart isolate ~30 s after screen-off and librats
        // RX stalls. Prompt the user once to whitelist us from battery
        // optimization. Some OEMs (Xiaomi, Huawei) refuse the intent
        // entirely — swallow the throw so the app still launches.
        requestBatteryOptimizationExemptionOnce()

        // Notify Dart side whenever the default network changes (wifi ↔
        // cellular flip, hotspot tether, etc.) so it can force-redial the
        // VPS rats handshake immediately instead of waiting up to 30 s for
        // TCP keepalive probes to declare the old socket dead. The 30 s
        // detection is already a huge improvement over the OS default
        // (~2 h) thanks to apply_tcp_keepalive in libmc_rats, but a
        // proactive nudge here cuts the choppy gap down to <1 s.
        networkChannel = MethodChannel(flutterEngine.dartExecutor.binaryMessenger,
                                       "musicchain/network")
        registerNetworkCallback()

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger,
                      "musicchain/fingerprint_decode")
            .setMethodCallHandler { call, result ->
                val path = call.argument<String>("path")
                if (path == null) {
                    result.error("path_missing", "path argument required", null)
                    return@setMethodCallHandler
                }
                if (!File(path).exists()) {
                    result.error("file_missing", "no file at $path", null)
                    return@setMethodCallHandler
                }
                // Both old "decodeToPcm" (kept for compat / Windows
                // fallback would be Dart-side) and new "fingerprint"
                // (stream-decode + feed chromaprint inline) live here.
                // The new path is much faster: zero PCM round-trip
                // through Dart, one allocation = one ShortArray reused
                // chunk-by-chunk.
                // (#crash) execute() throws RejectedExecutionException if the
                // pool was shut down by a prior onDestroy — and that throw is
                // on the Flutter platform thread, OUTSIDE the runnable's own
                // try below, so it would crash uncaught. Guard the submit.
                try {
                decodeExecutor.execute {
                    try {
                        when (call.method) {
                            "decodeToPcm" -> {
                                val decoded = decodeToPcm(path)
                                mainHandler.post {
                                    result.success(mapOf(
                                        "pcm"           to decoded.pcm,
                                        "sample_rate"   to decoded.sampleRate,
                                        "channel_count" to decoded.channelCount,
                                    ))
                                }
                            }
                            "fingerprint" -> {
                                val fp = decodeAndFingerprint(path)
                                mainHandler.post {
                                    result.success(mapOf(
                                        "compressed"    to fp.compressed,
                                        "sample_rate"   to fp.sampleRate,
                                        "channel_count" to fp.channelCount,
                                        "pcm_samples"   to fp.pcmSamples,
                                    ))
                                }
                            }
                            else -> {
                                mainHandler.post { result.notImplemented() }
                            }
                        }
                    } catch (e: Throwable) {
                        mainHandler.post {
                            result.error("decode_failed",
                                         e.message ?: e.toString(),
                                         null)
                        }
                    }
                }
                } catch (e: java.util.concurrent.RejectedExecutionException) {
                    result.error("decode_unavailable",
                                 "decode executor shut down", null)
                }
            }

        // #5 structural device attestation: hardware-derived fingerprint
        // the NDK can't read on Android (ANDROID_ID + Build.* identifiers).
        // SHA-256'd here so the raw identifiers never leave the device and
        // the wire shape matches the desktop FFI (mc_device_fingerprint):
        // lowercase hex of a digest. Stable across app reinstalls (ANDROID_ID
        // is per-app-signing-key + per-user, persisted by the OS), so the
        // full node's per-device limiter buckets real hardware.
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger,
                      "musicchain/device")
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "fingerprint" -> {
                        try {
                            result.success(deviceFingerprintHex())
                        } catch (e: Throwable) {
                            result.error("fp_failed", e.message ?: e.toString(), null)
                        }
                    }
                    else -> result.notImplemented()
                }
            }
    }

    @Suppress("HardwareIds")
    private fun deviceFingerprintHex(): String {
        val androidId = Settings.Secure.getString(
            contentResolver, Settings.Secure.ANDROID_ID) ?: ""
        // Concatenate stable hardware/build identifiers. ANDROID_ID carries
        // most of the entropy; the Build.* fields disambiguate two devices
        // that (rarely) collide on ANDROID_ID and bind the fingerprint to
        // the physical model. Deliberately NOT the wallet — device_id must
        // be per-device across wallets (the wallet binds one layer up, in
        // the signed bundle / session.start).
        val material = buildString {
            append("android_id=").append(androidId).append('\n')
            append("manufacturer=").append(Build.MANUFACTURER).append('\n')
            append("brand=").append(Build.BRAND).append('\n')
            append("model=").append(Build.MODEL).append('\n')
            append("device=").append(Build.DEVICE).append('\n')
            append("board=").append(Build.BOARD).append('\n')
            append("hardware=").append(Build.HARDWARE).append('\n')
            append("fingerprint=").append(Build.FINGERPRINT).append('\n')
        }
        val digest = MessageDigest.getInstance("SHA-256")
            .digest(material.toByteArray(Charsets.UTF_8))
        val sb = StringBuilder(digest.size * 2)
        for (b in digest) {
            val v = b.toInt() and 0xFF
            sb.append("0123456789abcdef"[v ushr 4])
            sb.append("0123456789abcdef"[v and 0x0F])
        }
        return sb.toString()
    }

    override fun onDestroy() {
        decodeExecutor.shutdownNow()
        unregisterNetworkCallback()
        super.onDestroy()
    }

    // One-shot Doze exemption prompt. Skips silently if the user already
    // granted (or saw and dismissed) the dialog, since re-prompting on
    // every launch is hostile UX and some OEM ROMs flag it as spammy.
    private fun requestBatteryOptimizationExemptionOnce() {
        val prefs = getSharedPreferences("musicchain_prefs", Context.MODE_PRIVATE)
        if (prefs.getBoolean(PREF_BATTERY_OPT_REQUESTED, false)) return
        val pm = getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return
        if (pm.isIgnoringBatteryOptimizations(packageName)) {
            prefs.edit().putBoolean(PREF_BATTERY_OPT_REQUESTED, true).apply()
            return
        }
        try {
            val intent = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
                data = Uri.parse("package:$packageName")
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            }
            startActivity(intent)
        } catch (_: Throwable) {
            // Some OEMs (Xiaomi MIUI, Huawei EMUI) disallow this intent.
            // Nothing we can do from here; the user has to dig through
            // Settings manually.
        }
        // Mark requested either way — we don't want to spam on every
        // launch when the OEM blocks the dialog.
        prefs.edit().putBoolean(PREF_BATTERY_OPT_REQUESTED, true).apply()
    }

    private fun registerNetworkCallback() {
        val cm = applicationContext
            .getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        connectivityMgr = cm

        val req = NetworkRequest.Builder()
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()

        val cb = object : ConnectivityManager.NetworkCallback() {
            private var lastNetwork: Network? = null

            override fun onAvailable(network: Network) {
                handleChange(network, reason = "available")
            }

            override fun onLost(network: Network) {
                // onLost can fire either side of the switch; treat as a
                // hint, not authoritative. The forthcoming onAvailable
                // for the replacement network will deliver the actual
                // redial trigger.
                if (network == lastNetwork) {
                    handleChange(null, reason = "lost")
                }
            }

            override fun onCapabilitiesChanged(network: Network,
                                               caps: NetworkCapabilities) {
                if (network != lastNetwork) {
                    handleChange(network, reason = "capabilities")
                }
            }

            private fun handleChange(network: Network?, reason: String) {
                lastNetwork = network
                mainHandler.post {
                    networkChannel?.invokeMethod("networkChanged",
                                                 mapOf("reason" to reason))
                }
            }
        }
        networkCallback = cb
        try {
            cm.registerNetworkCallback(req, cb)
        } catch (e: Throwable) {
            // Permission missing or device too old — Dart side just falls
            // back to TCP keepalive timing.
        }
    }

    private fun unregisterNetworkCallback() {
        val cm = connectivityMgr ?: return
        val cb = networkCallback ?: return
        try { cm.unregisterNetworkCallback(cb) } catch (_: Throwable) {}
        networkCallback = null
    }

    private data class Pcm(
        val pcm: ByteArray,
        val sampleRate: Int,
        val channelCount: Int,
    )

    // Decode any container/codec MediaExtractor + MediaCodec can chew on
    // (mp3, ogg, flac, m4a, aac, wav, opus) into raw little-endian 16-bit
    // PCM, channels interleaved. Stops at MediaCodec's BUFFER_FLAG_END_OF_STREAM
    // or after `maxDurationSec` of audio.
    //
    // The cap used to be a hard `60 * 11025 * 2` samples (60 s at 11025
    // Hz), which produced fingerprints ~10× smaller than the Windows
    // MediaFoundation decoder's full-file output — so the same song
    // submitted from Android vs Windows hashed to different
    // `fingerprint_hash` values and never deduped via the exact-hash
    // path. We now decode up to 10 minutes of audio scaled to whatever
    // sample rate the file actually uses, matching Windows behavior for
    // any normal-length track and capping memory growth on long files.
    private data class FingerprintResult(
        val compressed:   String,
        val sampleRate:   Int,
        val channelCount: Int,
        val pcmSamples:   Long,
    )

    /**
     * Streaming decode + chromaprint feed in one pass. Each MediaCodec
     * output buffer is read into a reused ShortArray and fed straight
     * into chromaprint via JNI, then dropped. Peak memory ≈ size of one
     * MediaCodec output buffer (~16 KB) instead of the full PCM buffer
     * (~30 MB for a 3-minute song). Wall-clock is dominated by decode
     * itself (a few seconds with the 90 s sample cap).
     */
    private fun decodeAndFingerprint(path: String,
                                     maxDurationSec: Int = 90): FingerprintResult {
        val extractor = MediaExtractor()
        extractor.setDataSource(path)
        var audioTrack = -1
        var format: MediaFormat? = null
        for (i in 0 until extractor.trackCount) {
            val f = extractor.getTrackFormat(i)
            val mime = f.getString(MediaFormat.KEY_MIME) ?: continue
            if (mime.startsWith("audio/")) { audioTrack = i; format = f; break }
        }
        if (audioTrack < 0 || format == null) {
            extractor.release()
            throw IllegalStateException("no audio track in $path")
        }
        extractor.selectTrack(audioTrack)

        val sampleRate   = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
        val channelCount = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
        val mime         = format.getString(MediaFormat.KEY_MIME)!!
        val maxSamples   = maxDurationSec.toLong() * sampleRate.toLong() *
                           channelCount.toLong()

        val codec = MediaCodec.createDecoderByType(mime)
        codec.configure(format, null, null, 0)
        codec.start()

        val bufInfo  = MediaCodec.BufferInfo()
        var inputEos = false
        var totalSamples = 0L
        val timeoutUs = 5_000L
        val startMs   = System.currentTimeMillis()
        val maxWallMs = 30_000L

        val cp = Chromaprint()
        var scratch = ShortArray(0)

        try {
            cp.start(sampleRate, channelCount)
            while (true) {
                if (System.currentTimeMillis() - startMs > maxWallMs) {
                    throw IllegalStateException(
                        "decodeAndFingerprint timeout after ${maxWallMs} ms")
                }
                if (!inputEos) {
                    val inIdx = codec.dequeueInputBuffer(timeoutUs)
                    if (inIdx >= 0) {
                        val buf = codec.getInputBuffer(inIdx)!!
                        val n = extractor.readSampleData(buf, 0)
                        if (n < 0) {
                            codec.queueInputBuffer(inIdx, 0, 0, 0,
                                MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputEos = true
                        } else {
                            codec.queueInputBuffer(inIdx, 0, n,
                                extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }
                val outIdx = codec.dequeueOutputBuffer(bufInfo, timeoutUs)
                if (outIdx >= 0) {
                    val buf = codec.getOutputBuffer(outIdx)
                    if (buf != null && bufInfo.size > 0) {
                        val sampleCount = bufInfo.size / 2
                        if (scratch.size < sampleCount) {
                            // Grow with headroom so most chunks land in
                            // the same allocation (16-bit buffers run
                            // 4 KB–16 KB typically).
                            scratch = ShortArray(sampleCount * 2)
                        }
                        buf.position(bufInfo.offset)
                        buf.limit(bufInfo.offset + bufInfo.size)
                        val asShorts =
                            buf.order(ByteOrder.nativeOrder()).asShortBuffer()
                        asShorts.get(scratch, 0, sampleCount)
                        cp.feed(scratch, sampleCount)
                        totalSamples += sampleCount
                    }
                    codec.releaseOutputBuffer(outIdx, false)
                    val eos = (bufInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0
                    if (eos || totalSamples >= maxSamples) break
                }
            }
            cp.finish()
            return FingerprintResult(
                compressed   = cp.fingerprint(),
                sampleRate   = sampleRate,
                channelCount = channelCount,
                pcmSamples   = totalSamples,
            )
        } finally {
            cp.close()
            try { codec.stop() } catch (_: Throwable) {}
            codec.release()
            extractor.release()
        }
    }

    private fun decodeToPcm(path: String, maxDurationSec: Int = 90): Pcm {
        val extractor = MediaExtractor()
        extractor.setDataSource(path)
        var audioTrack = -1
        var format: MediaFormat? = null
        for (i in 0 until extractor.trackCount) {
            val f = extractor.getTrackFormat(i)
            val mime = f.getString(MediaFormat.KEY_MIME) ?: continue
            if (mime.startsWith("audio/")) { audioTrack = i; format = f; break }
        }
        if (audioTrack < 0 || format == null) {
            extractor.release()
            throw IllegalStateException("no audio track in $path")
        }
        extractor.selectTrack(audioTrack)

        val sampleRate   = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
        val channelCount = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
        val mime         = format.getString(MediaFormat.KEY_MIME)!!
        val maxSamples   = maxDurationSec.toLong() * sampleRate.toLong() *
                           channelCount.toLong()

        val codec = MediaCodec.createDecoderByType(mime)
        codec.configure(format, null, null, 0)
        codec.start()

        val bufInfo  = MediaCodec.BufferInfo()
        val out      = ByteArrayOutputStream()
        var inputEos = false
        var totalSamples = 0L
        val timeoutUs = 5_000L
        // Wall-clock cap: a wedged codec would otherwise spin forever
        // inside the dequeue loop. 30 s is generous — a 3-minute decode
        // on a phone normally takes 5-15 s, so this only trips on a
        // genuinely stuck codec.
        val startMs   = System.currentTimeMillis()
        val maxWallMs = 30_000L

        try {
            while (true) {
                if (System.currentTimeMillis() - startMs > maxWallMs) {
                    throw IllegalStateException(
                        "MediaCodec decode timeout after ${maxWallMs} ms")
                }
                if (!inputEos) {
                    val inIdx = codec.dequeueInputBuffer(timeoutUs)
                    if (inIdx >= 0) {
                        val buf = codec.getInputBuffer(inIdx)!!
                        val n = extractor.readSampleData(buf, 0)
                        if (n < 0) {
                            codec.queueInputBuffer(inIdx, 0, 0, 0,
                                MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputEos = true
                        } else {
                            codec.queueInputBuffer(inIdx, 0, n,
                                extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }
                val outIdx = codec.dequeueOutputBuffer(bufInfo, timeoutUs)
                if (outIdx >= 0) {
                    val buf = codec.getOutputBuffer(outIdx)
                    if (buf != null && bufInfo.size > 0) {
                        buf.position(bufInfo.offset)
                        buf.limit(bufInfo.offset + bufInfo.size)
                        val chunk = ByteArray(bufInfo.size)
                        buf.get(chunk)
                        out.write(chunk)
                        totalSamples += bufInfo.size / 2
                    }
                    codec.releaseOutputBuffer(outIdx, false)
                    val eos = (bufInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0
                    if (eos || totalSamples >= maxSamples) break
                }
                // INFO_TRY_AGAIN_LATER (-1), INFO_OUTPUT_FORMAT_CHANGED
                // (-2), INFO_OUTPUT_BUFFERS_CHANGED (-3) all fall
                // through here — we just loop and try again on the
                // next dequeue.
            }
        } finally {
            try { codec.stop() } catch (_: Throwable) {}
            codec.release()
            extractor.release()
        }

        // MediaCodec emits native-endian PCM_16 by default on Android.
        // Normalise to little-endian (chromaprint expects host-native, but
        // arm64 is little-endian so this is usually a no-op; we coerce to
        // be safe across exotic Android targets).
        val raw = out.toByteArray()
        if (ByteOrder.nativeOrder() == ByteOrder.LITTLE_ENDIAN) return Pcm(raw, sampleRate, channelCount)
        val le = ByteArray(raw.size)
        val src = ByteBuffer.wrap(raw).order(ByteOrder.BIG_ENDIAN).asShortBuffer()
        val dstShorts = ShortBuffer.allocate(raw.size / 2)
        while (src.hasRemaining()) dstShorts.put(src.get())
        dstShorts.flip()
        ByteBuffer.wrap(le).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().put(dstShorts)
        return Pcm(le, sampleRate, channelCount)
    }

    companion object {
        private const val PREF_BATTERY_OPT_REQUESTED = "mc_battery_opt_requested"
    }
}
