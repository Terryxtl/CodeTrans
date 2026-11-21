package com.codetrans

import com.google.common.truth.Truth.assertThat
import org.junit.Test

class ConfirmedRegionsTest {
    private fun found(x: Int) = intArrayOf(1, 29, 0, x, 20, x+100, 20, x+100, 120, x, 120)

    @Test fun lateResultsCannotMoveNewerGeometryAndMissesDoNotMoveIt() {
        val regions = ConfirmedRegions()
        val out = IntArray(ConfirmedRegions.BUFFER_SIZE)
        regions.observed(1, 10, found(20))
        regions.observed(3, 90, found(40))
        regions.observed(2, 50, found(25))
        regions.seeds(4, 130, out)
        assertThat(out[3]).isEqualTo(40)
        assertThat(out[2]).isEqualTo(10) // half the measured displacement
        val unread = intArrayOf(1, 0, 43, 20, 143, 20, 143, 120, 43, 120)
        regions.observed(4, 1000, intArrayOf(0), unread)
        regions.seeds(5, 1800, out)
        assertThat(out[0]).isEqualTo(1)
        assertThat(out[3]).isEqualTo(40)
        regions.seeds(6, 2600, out)
        assertThat(out[0]).isEqualTo(0)
    }

    @Test fun oneGoodCodeDoesNotEvictMissingNeighbourOrLeakAcrossGeometryChanges() {
        val pool = FramePool(2)
        val original = pool.regionsFor(1920, 1080, 90)
        val both = intArrayOf(2) + found(20).drop(1) + found(200).drop(1)
        original.observed(1, 0, both)
        original.observed(2, 40, found(205))
        val out = IntArray(ConfirmedRegions.BUFFER_SIZE)
        original.seeds(3, 80, out)
        assertThat(out[0]).isEqualTo(2)
        val rotated = pool.regionsFor(1920, 1080, 270)
        original.observed(4, 120, found(18))
        rotated.seeds(5, 150, out)
        assertThat(out[0]).isEqualTo(0)
    }
}
