package com.codetrans

import com.google.common.truth.Truth.assertThat
import org.junit.Test

class DiagnosticsTest {
    private val ms = 1_000_000L

    @Test
    fun steadyStreamHasNoGaps() {
        val e = FrameGapEstimator()
        var t = 1_000 * ms
        repeat(100) { e.onFrame(t); t += 33 * ms + (it % 3) * ms / 2 }
        assertThat(e.missed).isEqualTo(0)
    }

    @Test
    fun countsDroppedFrames() {
        val e = FrameGapEstimator()
        var t = 1_000 * ms
        repeat(20) { e.onFrame(t); t += 33 * ms }
        t += 33 * ms            // one frame never delivered
        e.onFrame(t); t += 33 * ms
        t += 3 * 33 * ms        // three more
        e.onFrame(t)
        assertThat(e.missed).isEqualTo(4)
    }

    @Test
    fun followsFrameRateIncrease() {
        val e = FrameGapEstimator()
        var t = 1_000 * ms
        repeat(10) { e.onFrame(t); t += 66 * ms }
        repeat(30) { e.onFrame(t); t += 33 * ms }   // 15 -> 30 fps: not counted as drops
        assertThat(e.missed).isEqualTo(0)
    }

    @Test
    fun resetForgetsTheLastTimestamp() {
        val e = FrameGapEstimator()
        e.onFrame(1_000 * ms)
        e.onFrame(1_033 * ms)
        e.reset()
        e.onFrame(9_000 * ms)    // camera restarted: no giant gap
        e.onFrame(9_033 * ms)
        assertThat(e.missed).isEqualTo(0)
    }

    @Test
    fun formatShowsRs96Rows() {
        val a = DiagSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2)
        val b = a.copy(atMs = 1_000, cameraFrames = 30, decoded = 28, scanNanos = 28 * 12 * ms, rxFrames = 50,
            rxUnique = 40, rxDuplicates = 10, rs96K = 120, rs96Rows = 45, rs96Useful = 40, rs96Solved = 0)
        val text = b.format(a)
        assertThat(text).contains("30.0/s")
        assertThat(text).contains("RS96 有效行 40 / 120")
        assertThat(text).contains("20%")
    }

    @Test
    fun formatShowsOpticalBoardRows() {
        val a = DiagSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2)
        val b = a.copy(atMs = 1_000, rxFrames = 50, rxUnique = 40, rxDuplicates = 10,
            proto = NativeCore.PROTO_OB, codesInFrame = 2,
            obMode = 2, obK = 120, obRows = 45, obGroups = 4, obSolved = 1)
        val text = b.format(a)
        assertThat(text).contains("光传码面 · 分块(实验)")
        assertThat(text).contains("画面内 2 个")
        assertThat(text).contains("有效单元")
        assertThat(text).contains("OB 有效行 45 / 120")
        assertThat(text).contains("已解 1 / 4 组")
        assertThat(text).doesNotContain("RS96")
    }

    @Test
    fun formatShowsOnlyTheRunningProtocol() {
        val a = DiagSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2)
        // Counters of an earlier RS-96 stream are still there while a board stream runs, and vice versa.
        val both = a.copy(rs96K = 120, rs96Useful = 40, obMode = 1, obK = 50, obRows = 5, obGroups = 2)
        assertThat(both.copy(proto = NativeCore.PROTO_OB).format(null)).doesNotContain("RS96 有效行")
        val rs = both.copy(proto = NativeCore.PROTO_D3).format(null)
        assertThat(rs).contains("RS96 有效行 40 / 120")
        assertThat(rs).doesNotContain("OB 有效行")
    }
}
