package com.codetrans

import kotlin.math.round

/** What the live overlay in the corner of the camera view shows. */
data class FrameInfo(
    /** Frames per second that went through the decoder, smoothed. */
    val fps: Double,
    /** Analysis frame size (sensor orientation). */
    val width: Int,
    val height: Int,
)

/**
 * Frames per second from a counter that only grows. Every [update] adds one reading; the rate is
 * measured over the last [windowMs] and then low-pass filtered, so the number on screen does not
 * jump by a whole frame every time a frame lands on the other side of the window edge.
 * Not thread-safe: use it from one thread.
 */
class FpsMeter(
    private val windowMs: Long = 1_000L,
    /** Weight of the newest window measurement, 0 < x <= 1 (1 = no filtering). */
    private val smoothing: Double = 0.5,
) {
    private val times = ArrayDeque<Long>()
    private val counts = ArrayDeque<Long>()
    private var smoothed = -1.0

    /** Forget everything, e.g. when the counter's source was stopped for a while. */
    fun reset() {
        times.clear()
        counts.clear()
        smoothed = -1.0
    }

    /**
     * [count]: total frames processed so far, read at [nowMs] (any monotonic clock, milliseconds).
     * Returns frames per second rounded to 0.1, or null until there is a long enough span to measure.
     */
    fun update(nowMs: Long, count: Long): Double? {
        times.addLast(nowMs)
        counts.addLast(count)
        // Keep the newest reading that is at least one window old (or the very first one).
        while (times.size > 2 && nowMs - times[1] >= windowMs) {
            times.removeFirst()
            counts.removeFirst()
        }
        val span = nowMs - times.first()
        if (span < MIN_SPAN_MS) return null
        val raw = (count - counts.first()).coerceAtLeast(0L) * 1000.0 / span
        smoothed = if (smoothed < 0 || raw == 0.0) raw else smoothed + (raw - smoothed) * smoothing
        return round(smoothed * 10) / 10
    }

    private companion object {
        /** Shorter spans are too coarse to give a rate (one frame more or less is a huge swing). */
        const val MIN_SPAN_MS = 300L
    }
}
