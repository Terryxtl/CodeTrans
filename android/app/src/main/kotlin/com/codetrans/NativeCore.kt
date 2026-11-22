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
    /** CodeTrans optical board (experimental, non-QR), found and decoded by the native scanner. */
    const val PROTO_OB = 4
    const val OB_BANDS = 0
    const val OB_TILES = 1
    const val OB_SIDE = 177

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

    /**
     * Core's file-name rules (cp_sanitize_name + cp_unique_name): safe, unique, case-insensitively
     * distinct names, in the same order. [names] are UTF-8 bytes.
     */
    @JvmStatic external fun sanitizeNames(names: Array<ByteArray>): Array<String>?

    /** [sanitizeNames] for Strings; falls back to generic names if the native call fails. */
    fun safeNames(names: List<String>): List<String> =
        sanitizeNames(Array(names.size) { names[it].toByteArray(Charsets.UTF_8) })?.toList()
            ?: List(names.size) { "file${it + 1}" }

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
    @JvmStatic external fun scan(s: Long, rx: Long, y: ByteBuffer, w: Int, h: Int, quads: IntArray?): Int

    /**
     * The camera path: like [scan], but with the confirmed-region cache of [session]
     * ([sessionNew]) in one native call: seeds taken from it, the frame scanned, the cache updated
     * with what was read. [frame] increases per camera frame, [timeMs] is its capture time
     * (SystemClock.uptimeMillis()). Safe from several decoder threads, one [s] per thread.
     */
    @JvmStatic external fun scanStep(s: Long, rx: Long, session: Long, y: ByteBuffer, w: Int, h: Int,
                                     frame: Long, timeMs: Long, quads: IntArray?): Int
    @JvmStatic external fun sessionNew(): Long
    @JvmStatic external fun sessionFree(session: Long)
    /** New frame geometry or restart: forget every grid; frames numbered below [firstFrame] no longer touch it. */
    @JvmStatic external fun sessionReset(session: Long, firstFrame: Long)

    // ---- preview outline tracker (cp_quad_tracker). Not thread-safe: callers serialise (QuadTracker).
    @JvmStatic external fun trackerNew(): Long
    @JvmStatic external fun trackerFree(tracker: Long)
    @JvmStatic external fun trackerClear(tracker: Long)
    /** [quads]: the layout filled by [scan]. False = frame older than the last one, ignored. */
    @JvmStatic external fun trackerUpdate(tracker: Long, frameNo: Long, w: Int, h: Int, rotation: Int,
                                          quads: IntArray, nowMs: Long): Boolean
    /** out (size [TRACK_SNAPSHOT_SIZE]): [sighted, frameW, frameH, rotation, count, then per box: x0, y0 .. x3, y3, read, opacity]. */
    @JvmStatic external fun trackerSnapshot(tracker: Long, nowMs: Long, out: FloatArray)
    const val TRACK_HEADER = 5
    const val TRACK_BOX = 10
    const val TRACK_MAX = 2 * 16   // cp_quad_tracker: CP_TRACK_MAX
    const val TRACK_SNAPSHOT_SIZE = TRACK_HEADER + TRACK_MAX * TRACK_BOX

    const val QUAD_INTS = 9
    const val MAX_QUADS = 16
    const val QUADS_SIZE = 1 + MAX_QUADS * QUAD_INTS
    /** out (size 8): [proto, kind, have, need, totalBytes, bytesPerSec, codesInFrame, sessionId]. */
    @JvmStatic external fun progress(rx: Long, out: LongArray)

    /** Indices into the array filled by [rxStats] (diagnostics, cp_rx_stats). */
    const val S_FRAMES = 0
    const val S_UNIQUE = 1
    const val S_DUPLICATES = 2
    const val S_REDUNDANT = 3
    const val S_RS96_K = 4
    const val S_RS96_ROWS = 5
    const val S_RS96_USEFUL = 6
    const val S_RS96_SOLVED = 7
    /** Optical board stream most recently fed: 0 none, 1 bands, 2 tiles. */
    const val S_OB_MODE = 8
    const val S_OB_K = 9
    const val S_OB_ROWS = 10
    const val S_OB_GROUPS = 11
    const val S_OB_SOLVED = 12
    const val STATS_SIZE = 13
    @JvmStatic external fun rxStats(rx: Long, out: LongArray)
    @JvmStatic external fun takePlain(rx: Long): ByteArray?
    @JvmStatic external fun takeResult(rx: Long): NativeResult?

    // ---- sender (phone -> PC), jni_tx.c

    /**
     * [kind] KIND_TEXT (datas[0] = UTF-8 text), KIND_IMAGE (datas[0] + [mime]) or KIND_FILES
     * ([names] = UTF-8 bytes of each file name). PROTO_D3 uses RS-96, [version] and [ecc].
     * PROTO_OB uses [profile] (OB_BANDS or OB_TILES). Returns 0 on error. Call off the main thread.
     */
    @JvmStatic external fun txNew(kind: Int, names: Array<ByteArray>?, datas: Array<ByteArray>, mime: String?,
                                  proto: Int, profile: Int, version: Int, ecc: Int): Long
    @JvmStatic external fun txFree(tx: Long)
    /**
     * One refresh with [count] contiguous row-major matrices (1 = dark). [fps] is the requested
     * frame rate (only picks the mask policy). Returns side, or 0 on error.
     */
    @JvmStatic external fun txNextFrame(tx: Long, count: Int, fps: Int, out: ByteArray): Int
    /** Seconds for the receiver to see enough symbols when [count] codes are shown at [fps] refreshes per second. */
    @JvmStatic external fun txEtaSeconds(tx: Long, count: Int, fps: Double): Double
    /** Display refreshes one frame lasts at [fps] on a panel of [hz] Hz (cp_pace_vblanks). */
    @JvmStatic external fun paceVblanks(fps: Int, hz: Double): Int
    /** The frame rate that really shows on a panel of [hz] Hz (cp_pace_effective_fps). */
    @JvmStatic external fun paceEffectiveFps(fps: Int, hz: Double): Double
    /** Byte-mode capacity of one QR code (cp_qr_capacity). */
    @JvmStatic external fun qrCapacity(version: Int, ecc: Int): Int
    /**
     * out (size [CONSTANTS_SIZE]): the cp.h constants mirrored below, in this order:
     * [QR_VERSION_MIN, QR_VERSION_MAX, OB_SIDE, OB_BANDS_UNITS, OB_BANDS_UNIT_BYTES, OB_TILES_UNITS,
     * OB_TILES_UNIT_BYTES, MAX_CODES, capacity of version 40-L]. NativeCoreTest asserts they agree.
     */
    @JvmStatic external fun constants(out: IntArray)
    const val CONSTANTS_SIZE = 9
    const val QR_VERSION_MIN = 5
    const val QR_VERSION_MAX = 40
    const val OB_BANDS_UNITS = 6
    const val OB_BANDS_UNIT_BYTES = 512
    const val OB_TILES_UNITS = 24
    const val OB_TILES_UNIT_BYTES = 108
    const val MAX_CODES = 16
    /** out (size [TX_INFO_SIZE]): [rawBytes, containerBytes, compressed, symbolBytes, sourceSymbols, codeBytes]. */
    @JvmStatic external fun txInfo(tx: Long, out: LongArray)
    const val TX_INFO_SIZE = 6
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
