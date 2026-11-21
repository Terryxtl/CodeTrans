package com.codetrans

import kotlin.math.hypot
import kotlin.math.roundToInt

/** Session-local, un-smoothed sampling geometry. Worker threads keep their own pixel buffers. */
internal class ConfirmedRegions {
    companion object {
        private const val STRIDE = 10 // module count, drift, four (x,y) pairs
        const val BUFFER_SIZE = 1 + NativeCore.MAX_QUADS * STRIDE
        private const val HOLD_MS = 1500L
    }

    private class Region(val grid: IntArray, val frame: Long, val time: Long) {
        private val xs = intArrayOf(grid[2], grid[4], grid[6], grid[8])
        private val ys = intArrayOf(grid[3], grid[5], grid[7], grid[9])
        val x = xs.average()
        val y = ys.average()
        val side = maxOf(xs.max() - xs.min(), ys.max() - ys.min())
        var seenAt = time
    }

    private val confirmed = ArrayList<Region>()
    private var latestFrame = -1L

    @Synchronized
    fun seeds(frame: Long, time: Long, buffer: IntArray) {
        var count = 0
        for (r in confirmed) {
            if (r.frame >= frame || time - r.seenAt !in 0..HOLD_MS) continue
            r.grid.copyInto(buffer, 1 + count * STRIDE)
            count++
        }
        buffer[0] = count
    }

    @Synchronized
    fun observed(frame: Long, time: Long, buffer: IntArray, detections: IntArray? = null) {
        if (frame <= latestFrame) return
        confirmed.removeAll { time - it.seenAt > HOLD_MS }
        // Only a nearby, similarly sized sighting can extend a region. It cannot
        // change the sampling geometry or create a new region.
        if (detections != null && confirmed.isNotEmpty()) {
            for (i in 0 until detections[0]) {
                val base = 1 + i * NativeCore.QUAD_INTS
                if (detections[base] != 0) continue
                val xs = (0..3).map { detections[base + 1 + it * 2] }
                val ys = (0..3).map { detections[base + 2 + it * 2] }
                val size = maxOf(xs.max() - xs.min(), ys.max() - ys.min())
                val x = xs.average()
                val y = ys.average()
                val match = confirmed.minByOrNull { hypot(x - it.x, y - it.y) } ?: continue
                if (size.toDouble() in match.side * 0.7..match.side * 1.4 &&
                    hypot(x - match.x, y - match.y) < match.side * 0.25)
                    match.seenAt = maxOf(match.seenAt, time)
            }
        }
        // A faster empty frame must not discard a still-in-flight successful observation.
        if (buffer[0] == 0) return
        latestFrame = frame
        if (buffer[0] < 0) {
            confirmed.clear() // more symbols than the fixed cache can represent
            return
        }
        val previous = confirmed.toMutableList()
        confirmed.clear()
        for (i in 0 until buffer[0]) {
            val start = 1 + STRIDE * i
            val current = Region(buffer.copyOfRange(start, start + STRIDE), frame, time)
            var best: Region? = null
            var distance = Double.POSITIVE_INFINITY
            for (prior in previous) {
                val d = hypot(current.x - prior.x, current.y - prior.y)
                if (d < distance && d < minOf(current.side, prior.side) * 0.5) {
                    best = prior
                    distance = d
                }
            }
            if (best != null) {
                current.grid[1] = ((best.grid[1] + distance) / 2).roundToInt()
                previous.remove(best)
            }
            confirmed.add(current)
        }
        confirmed.addAll(previous.take(NativeCore.MAX_QUADS - confirmed.size))
    }
}
