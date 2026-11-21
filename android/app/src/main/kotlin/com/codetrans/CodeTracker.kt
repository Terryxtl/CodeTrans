package com.codetrans

import kotlin.math.hypot

/**
 * Turns per-frame detections (which flicker: a code is missed in one frame, quirc invents a
 * phantom code across two neighbours in another) into stable boxes:
 *  - each located code is matched to a track by its centre; matched tracks move smoothly,
 *  - a track survives [holdMs] without being seen, so one missed frame does not blink,
 *  - an unread candidate contributes to the status hint after [confirmHits] sightings,
 *    while only decode-confirmed geometry is drawn,
 *  - a track counts as read for [readHoldMs] after its last successful decode.
 * Thread-safe; frames that arrive late (several decoder threads) are ignored.
 */
class CodeTracker(
    private val holdMs: Long = 400,
    private val readHoldMs: Long = 700,
    private val confirmHits: Int = 3,
) {
    class Track internal constructor(val quad: FloatArray, var lastSeen: Long, var readAt: Long, var hits: Int)

    /** What to draw: boxes in frame pixels plus the frame geometry. */
    class Snapshot(
        val frameW: Int,
        val frameH: Int,
        val rotation: Int,
        /** 8 floats per box (x0, y0 .. x3, y3). */
        val quads: List<FloatArray>,
        val read: List<Boolean>,
        val sightedCount: Int,
        val opacity: List<Float>,
    ) {
        val count get() = quads.size
        val readCount get() = read.count { it }

        companion object {
            val EMPTY = Snapshot(0, 0, 0, emptyList(), emptyList(), 0, emptyList())
        }
    }

    private val tracks = ArrayList<Track>()
    private var frameW = 0
    private var frameH = 0
    private var rotation = 0
    private var lastFrameNo = -1L

    @Synchronized
    fun update(c: DetectedCodes, now: Long): Snapshot {
        if (c.frameNo < lastFrameNo) return snapshotLocked(now)
        lastFrameNo = c.frameNo
        if (c.frameW != frameW || c.frameH != frameH || c.rotation != rotation) {
            tracks.clear()
            frameW = c.frameW
            frameH = c.frameH
            rotation = c.rotation
        }
        // match only against the tracks that existed before this frame
        tracks.removeAll { now - it.lastSeen > holdMs }
        val existing = tracks.size
        val used = BooleanArray(existing)
        for (i in (0 until c.count).sortedByDescending { c.decoded(it) }) {
            val q = FloatArray(8) { k -> (if (k % 2 == 0) c.x(i, k / 2) else c.y(i, k / 2)).toFloat() }
            val (cx, cy) = centre(q)
            val size = side(q)
            var best = -1
            var bestD = Float.MAX_VALUE
            for (t in 0 until existing) {
                if (used[t]) continue
                val (tx, ty) = centre(tracks[t].quad)
                val d = hypot(cx - tx, cy - ty)
                if (d < bestD && d < 0.5f * maxOf(size, side(tracks[t].quad))) {
                    best = t
                    bestD = d
                }
            }
            if (best >= 0) {
                used[best] = true
                val t = tracks[best]
                // corners may come back in another order (quirc picks the orientation per frame):
                // align to the stored order before smoothing
                if (c.decoded(i) || t.readAt == 0L) {
                    val aligned = alignCorners(t.quad, q)
                    if (c.decoded(i) && t.readAt == 0L) aligned.copyInto(t.quad)
                    else for (k in 0 until 8) t.quad[k] += SMOOTH * (aligned[k] - t.quad[k])
                }
                t.lastSeen = now
                t.hits++
                if (c.decoded(i)) t.readAt = now
            } else {
                tracks += Track(q, now, if (c.decoded(i)) now else 0L, 1)
            }
        }
        return snapshotLocked(now)
    }

    @Synchronized
    fun snapshot(now: Long): Snapshot = snapshotLocked(now)

    @Synchronized
    fun clear() {
        tracks.clear()
        lastFrameNo = -1L
    }

    private fun snapshotLocked(now: Long): Snapshot {
        tracks.removeAll { now - it.lastSeen > holdMs }
        val shown = tracks.filter { it.readAt != 0L }
        val sighted = tracks.count { it.readAt != 0L || it.hits >= confirmHits }
        val opacity = shown.map {
            val age = (now - it.lastSeen).coerceAtLeast(0L)
            (2f * (1f - age.toFloat() / holdMs.coerceAtLeast(1))).coerceIn(0f, 1f)
        }
        return Snapshot(frameW, frameH, rotation, shown.map { it.quad.copyOf() },
            shown.map { isRead(it, now) }, sighted, opacity)
    }

    private fun isRead(t: Track, now: Long) = t.readAt != 0L && now - t.readAt <= readHoldMs

    private companion object {
        const val SMOOTH = 0.5f

        fun centre(q: FloatArray) = ((q[0] + q[2] + q[4] + q[6]) / 4f) to ((q[1] + q[3] + q[5] + q[7]) / 4f)

        fun side(q: FloatArray) = maxOf(hypot(q[2] - q[0], q[3] - q[1]), hypot(q[6] - q[0], q[7] - q[1]))

        /** [q] rotated (cyclically, either winding) to best match [ref] corner by corner. */
        fun alignCorners(ref: FloatArray, q: FloatArray): FloatArray {
            var best = q
            var bestD = Float.MAX_VALUE
            for (dir in intArrayOf(1, -1)) {
                for (shift in 0 until 4) {
                    val cand = FloatArray(8)
                    for (k in 0 until 4) {
                        val src = ((shift + dir * k) % 4 + 4) % 4
                        cand[2 * k] = q[2 * src]
                        cand[2 * k + 1] = q[2 * src + 1]
                    }
                    var d = 0f
                    for (k in 0 until 4) d += hypot(cand[2 * k] - ref[2 * k], cand[2 * k + 1] - ref[2 * k + 1])
                    if (d < bestD) {
                        bestD = d
                        best = cand
                    }
                }
            }
            return best
        }
    }
}
