package com.codetrans

import java.io.File
import kotlin.math.roundToInt

/** Small pure helpers for camera / decoder tuning (JVM-testable). */
object Tuning {
    const val MAX_AUTO_THREADS = 4

    /** "Auto" decoder threads = min(4, big cores). [maxFreqsKhz] = cpuinfo_max_freq per core. */
    fun autoThreads(maxFreqsKhz: List<Long>, nproc: Int): Int {
        val valid = maxFreqsKhz.filter { it > 0 }
        val big = if (valid.size < 2) {
            nproc
        } else {
            val slowest = valid.min()
            valid.count { it > slowest }.takeIf { it > 0 } ?: valid.size // homogeneous: all cores
        }
        return big.coerceIn(1, MAX_AUTO_THREADS)
    }

    fun readCpuMaxFreqs(nproc: Int): List<Long> = (0 until nproc).map { i ->
        runCatching {
            File("/sys/devices/system/cpu/cpu$i/cpufreq/cpuinfo_max_freq").readText().trim().toLong()
        }.getOrDefault(0L)
    }

    /** Upper bound used when the frame-rate setting is "auto". */
    const val AUTO_FPS_CAP = 60

    /**
     * AE target fps range from the camera's available ranges (lower, upper): the highest upper bound
     * not above [maxFps], and among those the highest lower bound (steady frame rate, short exposure,
     * less motion blur). Falls back to the slowest range when every range is faster than [maxFps].
     */
    fun chooseFpsRange(available: List<Pair<Int, Int>>, maxFps: Int): Pair<Int, Int>? =
        available.filter { it.second <= maxFps }.maxWithOrNull(compareBy({ it.second }, { it.first }))
            ?: available.minWithOrNull(compareBy({ it.second }, { it.first }))

    /** Frame rates worth offering in the settings: distinct upper bounds >= 10 fps, ascending. */
    fun fpsChoices(available: List<Pair<Int, Int>>): List<Int> =
        available.map { it.second }.filter { it >= 10 }.distinct().sorted()

    /** Effective fps target: [setting] (0 = auto) capped by what the stream size can deliver. */
    fun targetFps(setting: Int, sizeMaxFps: Int?): Int {
        val want = if (setting > 0) setting else AUTO_FPS_CAP
        return if (sizeMaxFps != null && sizeMaxFps > 0) minOf(want, sizeMaxFps) else want
    }

    /** Max fps from StreamConfigurationMap.getOutputMinFrameDuration (ns); 0 when unknown. */
    fun fpsFromMinFrameDuration(ns: Long): Int = if (ns <= 0) 0 else (1e9 / ns + 0.01).toInt()

    /** "1920x1080" -> (1920, 1080); null for anything else (e.g. the legacy "1080" values). */
    fun parseSize(s: String?): Pair<Int, Int>? {
        val parts = s?.split('x', 'X', '×')?.takeIf { it.size == 2 } ?: return null
        val w = parts[0].trim().toIntOrNull() ?: return null
        val h = parts[1].trim().toIntOrNull() ?: return null
        return if (w > 0 && h > 0) w to h else null
    }

    /**
     * Maps a point of an analysis frame ([w] x [h], sensor orientation, to be shown rotated by
     * [rotation] degrees clockwise) onto a [viewW] x [viewH] view that shows the camera with
     * PreviewView's default FILL_CENTER scaling (preview and analysis share the aspect ratio).
     * [out] receives (x, y) in view pixels.
     */
    fun frameToView(x: Float, y: Float, w: Int, h: Int, rotation: Int, viewW: Int, viewH: Int, out: FloatArray) {
        val (ux, uy) = when ((rotation % 360 + 360) % 360) {
            90 -> (h - y) to x
            180 -> (w - x) to (h - y)
            270 -> y to (w - x)
            else -> x to y
        }
        val uw = if (rotation % 180 != 0) h else w
        val uh = if (rotation % 180 != 0) w else h
        val scale = maxOf(viewW.toFloat() / uw, viewH.toFloat() / uh)
        out[0] = ux * scale + (viewW - uw * scale) / 2f
        out[1] = uy * scale + (viewH - uh * scale) / 2f
    }

    /** "4:3", "16:9", ... for a stream size; uncommon ratios as "2.22:1". */
    fun ratioLabel(w: Int, h: Int): String {
        if (w <= 0 || h <= 0) return ""
        val r = maxOf(w, h).toDouble() / minOf(w, h)
        val named = listOf(1.0 to "1:1", 4.0 / 3 to "4:3", 3.0 / 2 to "3:2", 16.0 / 9 to "16:9", 2.0 to "2:1")
        named.firstOrNull { kotlin.math.abs(it.first - r) < 0.02 }?.let { return it.second }
        return String.format(java.util.Locale.ROOT, "%.2f:1", r)
    }

    /** Exposure compensation index for [ev] given the device step (EV per index) and range. */
    fun evToIndex(ev: Float, stepEv: Float, lower: Int, upper: Int): Int {
        if (stepEv <= 0f || lower > upper) return 0
        return (ev / stepEv).roundToInt().coerceIn(lower, upper)
    }

    /** Largest power-of-two subsample that keeps the longer side >= [maxDim]. */
    fun inSampleSize(width: Int, height: Int, maxDim: Int): Int {
        val longer = maxOf(width, height)
        if (longer <= 0 || maxDim <= 0) return 1
        var s = 1
        while (longer / (s * 2) >= maxDim) s *= 2
        return s
    }
}
