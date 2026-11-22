package com.codetrans

/** What to draw over the preview: boxes in frame pixels plus the frame geometry. */
class CodeSnapshot(
    val frameW: Int,
    val frameH: Int,
    val rotation: Int,
    /** 8 floats per box (x0, y0 .. x3, y3). Only codes that were decoded are listed. */
    val quads: List<FloatArray>,
    /** True while the box counts as read (a decode within the last 700 ms). */
    val read: List<Boolean>,
    /** Codes in view, read or confirmed unread (a candidate seen several times). */
    val sightedCount: Int,
    /** 0..1: boxes fade out as the tracker's hold time runs out. */
    val opacity: List<Float>,
) {
    val count get() = quads.size
    val readCount get() = read.count { it }

    companion object {
        val EMPTY = CodeSnapshot(0, 0, 0, emptyList(), emptyList(), 0, emptyList())
    }
}

/**
 * Steady boxes for per-frame detections: a thin, thread-safe wrapper over the shared native
 * cp_quad_tracker (core/scan/quad_tracker.c), which also drops frames that arrive late from
 * another decoder thread. All access is serialised here; [close] frees the native tracker.
 */
class QuadTracker : AutoCloseable {
    private var handle = if (NativeCore.isAvailable) runCatching { NativeCore.trackerNew() }.getOrDefault(0L) else 0L
    private val buf = FloatArray(NativeCore.TRACK_SNAPSHOT_SIZE)

    @Synchronized
    fun update(c: DetectedCodes, now: Long): CodeSnapshot {
        if (handle != 0L) NativeCore.trackerUpdate(handle, c.frameNo, c.frameW, c.frameH, c.rotation, c.quads, now)
        return snapshotLocked(now)
    }

    @Synchronized
    fun snapshot(now: Long): CodeSnapshot = snapshotLocked(now)

    @Synchronized
    fun clear() {
        if (handle != 0L) NativeCore.trackerClear(handle)
    }

    @Synchronized
    override fun close() {
        if (handle != 0L) NativeCore.trackerFree(handle)
        handle = 0L
    }

    private fun snapshotLocked(now: Long): CodeSnapshot {
        if (handle == 0L) return CodeSnapshot.EMPTY
        NativeCore.trackerSnapshot(handle, now, buf)
        val count = buf[4].toInt()
        val quads = ArrayList<FloatArray>(count)
        val read = ArrayList<Boolean>(count)
        val opacity = ArrayList<Float>(count)
        for (i in 0 until count) {
            val o = NativeCore.TRACK_HEADER + i * NativeCore.TRACK_BOX
            quads += buf.copyOfRange(o, o + 8)
            read += buf[o + 8] != 0f
            opacity += buf[o + 9]
        }
        return CodeSnapshot(buf[1].toInt(), buf[2].toInt(), buf[3].toInt(), quads, read, buf[0].toInt(), opacity)
    }
}
