package com.codetrans

import com.google.common.truth.Truth.assertThat
import org.junit.Test

class TuningTest {
    private val ranges = listOf(15 to 30, 30 to 30, 24 to 24, 7 to 60, 60 to 60, 15 to 15)

    @Test fun chooseFpsRange_highestUpperNotAboveTarget_thenHighestLower() {
        assertThat(Tuning.chooseFpsRange(ranges, 60)).isEqualTo(60 to 60)
        assertThat(Tuning.chooseFpsRange(ranges, 30)).isEqualTo(30 to 30)
        assertThat(Tuning.chooseFpsRange(ranges, 29)).isEqualTo(24 to 24)
        assertThat(Tuning.chooseFpsRange(ranges, 120)).isEqualTo(60 to 60)
    }

    @Test fun chooseFpsRange_fallsBackToSlowest() {
        assertThat(Tuning.chooseFpsRange(listOf(30 to 30, 15 to 30), 10)).isEqualTo(15 to 30)
        assertThat(Tuning.chooseFpsRange(emptyList(), 30)).isNull()
    }

    @Test fun fpsChoices() {
        assertThat(Tuning.fpsChoices(ranges + (5 to 8))).containsExactly(15, 24, 30, 60).inOrder()
    }

    @Test fun targetFps() {
        assertThat(Tuning.targetFps(0, null)).isEqualTo(Tuning.AUTO_FPS_CAP)
        assertThat(Tuning.targetFps(0, 30)).isEqualTo(30)
        assertThat(Tuning.targetFps(60, 30)).isEqualTo(30)
        assertThat(Tuning.targetFps(24, 60)).isEqualTo(24)
        assertThat(Tuning.targetFps(30, 0)).isEqualTo(30)
    }

    @Test fun fpsFromMinFrameDuration() {
        assertThat(Tuning.fpsFromMinFrameDuration(33_333_333)).isEqualTo(30)
        assertThat(Tuning.fpsFromMinFrameDuration(16_666_666)).isEqualTo(60)
        assertThat(Tuning.fpsFromMinFrameDuration(50_000_000)).isEqualTo(20)
        assertThat(Tuning.fpsFromMinFrameDuration(0)).isEqualTo(0)
    }

    @Test fun parseSize() {
        assertThat(Tuning.parseSize("1920x1080")).isEqualTo(1920 to 1080)
        assertThat(Tuning.parseSize("4000×3000")).isEqualTo(4000 to 3000)
        assertThat(Tuning.parseSize("1080")).isNull()
        assertThat(Tuning.parseSize("0x10")).isNull()
        assertThat(Tuning.parseSize(null)).isNull()
    }

    @Test fun frameToView() {
        val o = FloatArray(2)
        // 1920x1080 sensor frame, phone upright (rotation 90) -> 1080x1920 upright, view 540x960 (scale 0.5)
        Tuning.frameToView(0f, 0f, 1920, 1080, 90, 540, 960, o)
        assertThat(o.toList()).containsExactly(540f, 0f).inOrder()      // sensor top-left -> view top-right
        Tuning.frameToView(1920f, 1080f, 1920, 1080, 90, 540, 960, o)
        assertThat(o.toList()).containsExactly(0f, 960f).inOrder()
        // no rotation, FILL_CENTER crop: 1920x1080 into 1000x1000 -> scale 1000/1080, centred
        Tuning.frameToView(960f, 540f, 1920, 1080, 0, 1000, 1000, o)
        assertThat(o[0]).isWithin(0.01f).of(500f)
        assertThat(o[1]).isWithin(0.01f).of(500f)
        Tuning.frameToView(0f, 0f, 1920, 1080, 180, 1920, 1080, o)
        assertThat(o.toList()).containsExactly(1920f, 1080f).inOrder()
        Tuning.frameToView(0f, 0f, 1920, 1080, 270, 1080, 1920, o)
        assertThat(o.toList()).containsExactly(0f, 1920f).inOrder()
    }

    @Test fun ratioLabel() {
        assertThat(Tuning.ratioLabel(4000, 3000)).isEqualTo("4:3")
        assertThat(Tuning.ratioLabel(1920, 1080)).isEqualTo("16:9")
        assertThat(Tuning.ratioLabel(1088, 1088)).isEqualTo("1:1")
        assertThat(Tuning.ratioLabel(1920, 864)).isEqualTo("2.22:1")
    }
}
