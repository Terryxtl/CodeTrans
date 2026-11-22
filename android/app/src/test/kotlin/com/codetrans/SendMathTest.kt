package com.codetrans

import com.google.common.truth.Truth.assertThat
import org.junit.Test

class SendMathTest {
    @Test
    fun etaFormatting() {
        assertThat(SendMath.eta(0.2)).isEqualTo("1 秒")
        assertThat(SendMath.eta(75.0)).isEqualTo("1 分 15 秒")
        assertThat(SendMath.eta(3700.0)).isEqualTo("1 时 01 分")
    }

    @Test
    fun fpsNodesAreAscendingAndEndAtSixty() {
        assertThat(SendMath.FPS_NODES.toList()).isInOrder()
        assertThat(SendMath.FPS_NODES.last()).isEqualTo(60)
        assertThat(SendMath.FPS_NODES.toList()).contains(SendMath.DEFAULT_FPS)
    }

    @Test
    fun oldStoredFpsValuesKeepTheirNode() {
        // The send screen used to offer exactly these five.
        for (fps in intArrayOf(6, 10, 15, 20, 24)) {
            assertThat(SendMath.fpsAt(SendMath.fpsIndex(fps))).isEqualTo(fps)
        }
    }

    @Test
    fun oddStoredFpsSnapsToTheClosestNode() {
        assertThat(SendMath.fpsAt(SendMath.fpsIndex(0))).isEqualTo(6)
        assertThat(SendMath.fpsAt(SendMath.fpsIndex(26))).isEqualTo(24)
        assertThat(SendMath.fpsAt(SendMath.fpsIndex(40))).isEqualTo(45)
        assertThat(SendMath.fpsAt(SendMath.fpsIndex(120))).isEqualTo(60)
        assertThat(SendMath.fpsAt(-3)).isEqualTo(6)
        assertThat(SendMath.fpsAt(99)).isEqualTo(60)
    }

    @Test
    fun densitiesAreV15V27V40() {
        // Low / mid (default, index 1) / high; v40 matches the optical boards' 177-module side.
        assertThat(SendMath.VERSIONS.toList()).containsExactly(15, 27, 40).inOrder()
        assertThat(SendMath.VERSIONS.max()).isAtMost(NativeCore.QR_VERSION_MAX)
        assertThat(SendMath.VERSIONS.min()).isAtLeast(NativeCore.QR_VERSION_MIN)
    }
}
