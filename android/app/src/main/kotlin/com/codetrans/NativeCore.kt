package com.codetrans

import android.util.Log
import androidx.annotation.Keep
import java.nio.ByteBuffer

/**
 * JNI bindings for the native decoder (lib "codetrans": quirc + shared C core).
 * Constants mirror core/include/cp.h.
 */
object NativeCore {
    // ---- cp.h: event bits returned by scan()
    const val EV_PROGRESS = 1
    const val EV_COMPLETE = 2
    const val EV_PLAIN = 4
    const val EV_FAILED = 8
    const val EV_UNSUPPORTED = 16

    // ---- cp.h: protocols
    const val PROTO_NONE = 0
    const val PROTO_RQ = 1
    const val PROTO_D3 = 2
    const val PROTO_PLAIN = 3

    // ---- cp.h: result kinds
    const val KIND_NONE = 0
    const val KIND_TEXT = 1
    const val KIND_IMAGE = 2
    const val KIND_FILES = 3

    /** Indices into the array filled by [progress]. */
    const val P_PROTO = 0
    const val P_KIND = 1
    const val P_HAVE = 2
    const val P_NEED = 3
    const val P_TOTAL_BYTES = 4
    const val P_BYTES_PER_SEC = 5
    const val P_CODES_IN_FRAME = 6
    const val P_SESSION_ID = 7
    const val PROGRESS_SIZE = 8

    /** False if the native library could not be loaded (e.g. unsupported ABI). */
    val isAvailable: Boolean = try {
        System.loadLibrary("codetrans")
        true
    } catch (t: UnsatisfiedLinkError) {
        Log.e("CodeTrans", "native library failed to load", t)
        false
    }

    @JvmStatic external fun rxNew(outDir: String, maxBytes: Long, enableD3: Boolean, enablePlain: Boolean): Long
    @JvmStatic external fun rxFree(rx: Long)
    @JvmStatic external fun rxReset(rx: Long)
    /** Applies settings without losing progress. */
    @JvmStatic external fun rxSetOptions(rx: Long, enableD3: Boolean, enablePlain: Boolean)
    @JvmStatic external fun scannerNew(): Long
    @JvmStatic external fun scannerFree(s: Long)
    /** Row-by-row copy of a (possibly padded) luminance plane into a tightly packed [dst] (w*h bytes). */
    @JvmStatic external fun copyPlane(src: ByteBuffer, rowStride: Int, w: Int, h: Int, dst: ByteBuffer)
    /**
     * Detects and decodes every QR code in the frame and feeds them to [rx]. Returns EV_* bits.
     * [quads] (optional, size [QUADS_SIZE]) receives where the codes are:
     * [count, then per code: decoded (0/1), x0, y0, x1, y1, x2, y2, x3, y3] in frame pixels.
     */
    @JvmStatic external fun scan(s: Long, rx: Long, y: ByteBuffer, w: Int, h: Int, quads: IntArray?,
                                 regions: IntArray? = null): Int

    const val QUAD_INTS = 9
    const val MAX_QUADS = 16
    const val QUADS_SIZE = 1 + MAX_QUADS * QUAD_INTS
    /** out (size 8): [proto, kind, have, need, totalBytes, bytesPerSec, codesInFrame, sessionId]. */
    @JvmStatic external fun progress(rx: Long, out: LongArray)
    @JvmStatic external fun takePlain(rx: Long): ByteArray?
    @JvmStatic external fun takeResult(rx: Long): NativeResult?
}

/**
 * A completed transfer. Created from C; files already live in <cacheDir>/rx/<session>/.
 * TEXT: [text] is the full text when <= 1 MiB, otherwise a 64 KiB prefix with [textTruncated];
 * paths[0] is text.txt.
 */
@Keep
class NativeResult(
    val kind: Int,
    val mime: String,
    val text: String?,
    val textTruncated: Boolean,
    val paths: Array<String>,
    val names: Array<String>,
    val sizes: LongArray,
)
