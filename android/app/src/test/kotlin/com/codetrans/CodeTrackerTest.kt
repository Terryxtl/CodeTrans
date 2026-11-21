package com.codetrans

import com.google.common.truth.Truth.assertThat
import org.junit.Test

class CodeTrackerTest {
    /** One frame: each code = (decoded, x, y, size) as an axis-aligned square. */
    private fun frame(n: Long, vararg codes: IntArray, t: Long = n * 33) = DetectedCodes(
        1920, 1080, 90,
        IntArray(1 + codes.size * NativeCore.QUAD_INTS).also { a ->
            a[0] = codes.size
            codes.forEachIndexed { i, (dec, x, y, s) ->
                val o = 1 + i * NativeCore.QUAD_INTS
                a[o] = dec
                intArrayOf(x, y, x + s, y, x + s, y + s, x, y + s).copyInto(a, o + 1)
            }
        },
        n, t,
    )

    private val left = intArrayOf(1, 100, 100, 400)
    private val right = intArrayOf(1, 600, 100, 400)

    @Test fun readCodeShowsAtOnce_andSurvivesAMissedFrame() {
        val tr = CodeTracker()
        assertThat(tr.update(frame(1, left, right), 33).readCount).isEqualTo(2)
        val missed = tr.update(frame(2), 66)            // nothing found in this frame
        assertThat(missed.count).isEqualTo(2)
        assertThat(tr.snapshot(66 + 500).count).isEqualTo(0)   // gone after the hold time
    }

    @Test fun unreadPhantomNeedsSeveralSightings() {
        val tr = CodeTracker()
        val phantom = intArrayOf(0, 350, 100, 400)
        val s1 = tr.update(frame(1, left, right, phantom), 33)
        assertThat(s1.count).isEqualTo(2)                // phantom not shown yet
        tr.update(frame(2, left, right), 66)
        val s3 = tr.update(frame(3, left, right), 99)
        assertThat(s3.count).isEqualTo(2)
    }

    @Test fun repeatedUnreadCandidateProvidesHintWithoutDrawingAPhantom() {
        val tr = CodeTracker()
        val candidate = intArrayOf(0, 180, 130, 350)
        tr.update(frame(1, candidate), 33)
        tr.update(frame(2, candidate), 66)
        val snap = tr.update(frame(3, candidate), 99)
        assertThat(snap.sightedCount).isEqualTo(1)
        assertThat(snap.count).isEqualTo(0)
    }

    @Test fun lateFrameIsIgnored_andBoxMovesSmoothly() {
        val tr = CodeTracker()
        tr.update(frame(5, left), 165)
        val moved = tr.update(frame(6, intArrayOf(1, 140, 100, 400)), 198)
        assertThat(moved.quads[0][0]).isWithin(0.01f).of(120f)   // halfway
        val late = tr.update(frame(4, intArrayOf(1, 900, 900, 400)), 200)
        assertThat(late.count).isEqualTo(1)
        assertThat(late.quads[0][0]).isWithin(0.01f).of(120f)
    }

    @Test fun unreadSightingKeepsConfirmedBoxAliveWithoutMovingIt() {
        val tr = CodeTracker()
        val original = tr.update(frame(1, left), 33).quads.single().toList()
        val unread = intArrayOf(0, 180, 130, 350)
        tr.update(frame(2, unread), 390)
        assertThat(tr.snapshot(460).quads.single().toList()).isEqualTo(original)
        val moved = tr.update(frame(3, unread, intArrayOf(1, 140, 100, 400)), 490)
        assertThat(moved.quads.single()[0]).isEqualTo(120f)
    }

    @Test fun cornerOrderChangeDoesNotSpinTheBox() {
        val tr = CodeTracker()
        tr.update(frame(1, left), 33)
        // same square, corners listed starting from another corner
        val q = DetectedCodes(1920, 1080, 90, intArrayOf(1, 1, 500, 100, 500, 500, 100, 500, 100, 100), 2, 66)
        val s = tr.update(q, 66)
        assertThat(s.quads[0].toList()).containsExactly(100f, 100f, 500f, 100f, 500f, 500f, 100f, 500f).inOrder()
    }
}
