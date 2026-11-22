package com.codetrans

import com.google.common.truth.Truth.assertThat
import org.junit.Test

class FrameRateTest {
    @Test
    fun steadyStreamGivesItsRate() {
        val m = FpsMeter()
        var last: Double? = null
        repeat(12) { last = m.update(it * 500L, it * 15L) }     // 30 fps, read every 0.5 s
        assertThat(last).isEqualTo(30.0)
    }

    @Test
    fun noRateUntilThereIsATimeSpan() {
        val m = FpsMeter()
        assertThat(m.update(0, 0)).isNull()
        assertThat(m.update(100, 3)).isNull()
        assertThat(m.update(500, 15)).isEqualTo(30.0)
    }

    @Test
    fun stalledStreamDropsToZero() {
        val m = FpsMeter()
        repeat(6) { m.update(it * 500L, it * 15L) }
        var last: Double? = null
        for (i in 6..9) last = m.update(i * 500L, 5 * 15L)      // no frames any more
        assertThat(last).isEqualTo(0.0)
    }

    @Test
    fun followsARateChange() {
        val m = FpsMeter()
        var n = 0L
        repeat(8) { m.update(it * 500L, n); n += 15 }           // 30 fps
        var last: Double? = null
        for (i in 8..20) { last = m.update(i * 500L, n); n += 5 }   // 10 fps
        assertThat(last).isEqualTo(10.0)
    }

    @Test
    fun resetForgetsEarlierReadings() {
        val m = FpsMeter()
        repeat(6) { m.update(it * 500L, it * 15L) }
        m.reset()
        assertThat(m.update(60_000, 1_000_000)).isNull()
        assertThat(m.update(60_500, 1_000_015)).isEqualTo(30.0)
    }

    @Test
    fun overlayText() {
        assertThat(Formatters.frameInfo(FrameInfo(29.97, 1920, 1080))).isEqualTo("30.0 fps · 1920×1080")
        assertThat(Formatters.frameInfo(FrameInfo(0.0, 1280, 720))).isEqualTo("0.0 fps · 1280×720")
    }
}
