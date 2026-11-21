package com.codetrans

import androidx.annotation.StringRes
import java.io.File

enum class ResultKind { TEXT, IMAGE, FILES }

data class ReceivedFile(
    val file: File,
    /** Sanitized display name. */
    val name: String,
    val size: Long,
    val mime: String,
)

/** The single "current" result (no history is kept). */
data class ReceivedResult(
    /** Monotonic id, used to detect "a new result arrived". */
    val id: Long,
    val kind: ResultKind,
    /** True for an ordinary (non-CodeTrans) QR code. */
    val plain: Boolean,
    /** TEXT only: the text, or just a prefix when [textTruncated]. */
    val text: String?,
    val textTruncated: Boolean,
    /** TEXT only: UTF-8 size of the complete text. */
    val textBytes: Long,
    val mime: String,
    /** TEXT: text.txt (absent for plain QR); IMAGE: one file; FILES: all files. */
    val files: List<ReceivedFile>,
    /** Session directory under cache/rx/, deleted when this result is replaced. */
    val dir: File?,
)

/** Snapshot of NativeCore.progress(). */
data class Progress(
    val proto: Int,
    val kind: Int,
    val have: Long,
    val need: Long,
    val totalBytes: Long,
    val bytesPerSec: Long,
    val codesInFrame: Int,
    val sessionId: Long,
) {
    /** 0..1000 for a determinate progress indicator. */
    val permille: Int
        get() = if (need <= 0) 0 else ((have.coerceAtMost(need) * 1000) / need).toInt()

    companion object {
        fun from(a: LongArray) = Progress(
            proto = a[NativeCore.P_PROTO].toInt(),
            kind = a[NativeCore.P_KIND].toInt(),
            have = a[NativeCore.P_HAVE],
            need = a[NativeCore.P_NEED],
            totalBytes = a[NativeCore.P_TOTAL_BYTES],
            bytesPerSec = a[NativeCore.P_BYTES_PER_SEC],
            codesInFrame = a[NativeCore.P_CODES_IN_FRAME].toInt(),
            sessionId = a[NativeCore.P_SESSION_ID] and 0xFFFFFFFFL,
        )
    }
}

/**
 * QR codes located in one analysed frame (decoded or not), for the boxes over the preview.
 * [quads] = NativeCore.scan() layout, trimmed: [count, then per code: decoded, x0, y0 .. x3, y3]
 * in frame pixels; the frame is [frameW] x [frameH] in sensor orientation, shown rotated by
 * [rotation] degrees clockwise.
 */
class DetectedCodes(
    val frameW: Int,
    val frameH: Int,
    val rotation: Int,
    val quads: IntArray,
    /** Analysis frame number: frames finish out of order on several decoder threads. */
    val frameNo: Long,
    /** SystemClock.uptimeMillis() when the frame was decoded. */
    val atMs: Long,
) {
    val count: Int get() = quads[0]
    val decodedCount: Int get() = (0 until count).count { decoded(it) }
    fun decoded(i: Int) = quads[1 + i * NativeCore.QUAD_INTS] != 0
    fun x(i: Int, corner: Int) = quads[2 + i * NativeCore.QUAD_INTS + corner * 2]
    fun y(i: Int, corner: Int) = quads[3 + i * NativeCore.QUAD_INTS + corner * 2]
}

/** What the camera currently sees, summarised for the status line (updated ~10 Hz). */
data class Detection(
    /** Codes located in the latest frame (0 = none in view). */
    val count: Int,
    /** ... of which decoded. */
    val decoded: Int,
    /** Codes have been in view for a while without a single one decoding. */
    val stuck: Boolean,
) {
    companion object {
        val NONE = Detection(0, 0, false)
    }
}

sealed interface ScanState {
    /** Result to show in the preview card / to share, if any. */
    val result: ReceivedResult?

    data object Idle : ScanState {
        override val result: ReceivedResult? get() = null
    }

    /** A transfer is in progress; the card keeps showing the previous result. */
    data class Receiving(val progress: Progress, val last: ReceivedResult?) : ScanState {
        override val result: ReceivedResult? get() = last
    }

    data class Done(override val result: ReceivedResult) : ScanState

    data class Error(@StringRes val message: Int, val last: ReceivedResult?) : ScanState {
        override val result: ReceivedResult? get() = last
    }
}

/** One-shot UI messages (Snackbars). */
data class UiEvent(@StringRes val text: Int, val arg: Any? = null, val long: Boolean = false)
