package com.codetrans

import java.util.Locale
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.roundToLong

/**
 * Estimates frames the camera produced that never reached the analyzer (CameraX
 * KEEP_ONLY_LATEST drops them silently while the analyzer is busy) from the gaps between
 * sensor timestamps. The frame interval is learned from the gaps themselves: a gap within
 * 1.5x of the current estimate refines it, a longer one counts round(gap / interval) - 1
 * missed frames. Not thread-safe: feed it from the (serial) analyzer.
 */
class FrameGapEstimator {
    private var lastNs = 0L
    private var intervalNs = 0.0
    var missed = 0L
        private set

    fun reset() {
        lastNs = 0L
        intervalNs = 0.0
    }

    /** [timestampNs]: ImageInfo.timestamp of the next analysed frame. */
    fun onFrame(timestampNs: Long) {
        val last = lastNs
        lastNs = timestampNs
        if (last == 0L || timestampNs <= last) return
        val gap = (timestampNs - last).toDouble()
        when {
            intervalNs == 0.0 -> intervalNs = gap
            gap < intervalNs * 0.6 -> intervalNs = gap        // faster than we thought (fps went up)
            gap <= intervalNs * 1.5 -> intervalNs += (gap - intervalNs) * 0.1
            else -> missed += (gap / intervalNs).roundToLong() - 1
        }
    }
}

/** Counters of [DecodePipeline], written from the analyzer and decoder threads. */
class PipelineStats {
    /** Frames delivered to the analyzer. */
    val cameraFrames = AtomicLong()
    /** Estimated frames CameraX dropped before the analyzer (see [FrameGapEstimator]). */
    val cameraMissed = AtomicLong()
    /** Frames dropped because every decoder buffer was in flight (queue full). */
    val queueDropped = AtomicLong()
    /** Frames that finished scan(). */
    val decoded = AtomicLong()
    /** Total time spent in scan(), nanoseconds. */
    val scanNanos = AtomicLong()
    /** Size of the latest analysed frame, sensor orientation; 0 while the pipeline is stopped. */
    @Volatile var frameWidth = 0
    @Volatile var frameHeight = 0
}

/** One reading of every diagnostics counter; [rate] gives per-second deltas against an older one. */
data class DiagSnapshot(
    val atMs: Long,
    val cameraFrames: Long,
    val cameraMissed: Long,
    val queueDropped: Long,
    val decoded: Long,
    val scanNanos: Long,
    val rxFrames: Long,
    val rxUnique: Long,
    val rxDuplicates: Long,
    val rxRedundant: Long,
    val rs96K: Long,
    val rs96Rows: Long,
    val rs96Useful: Long,
    val rs96Solved: Long,
    val threads: Int,
    /** NativeCore.PROTO_* of the transfer being received right now (PROTO_NONE = idle). */
    val proto: Int = NativeCore.PROTO_NONE,
    /** Codes (optical boards for PROTO_OB) in the latest camera frame. */
    val codesInFrame: Int = 0,
    /** Optical board stream most recently fed (cp_rx_stats): mode 0 none, 1 bands, 2 tiles. */
    val obMode: Long = 0,
    val obK: Long = 0,
    val obRows: Long = 0,
    val obGroups: Long = 0,
    val obSolved: Long = 0,
) {
    /**
     * Multi-line text for the developer panel. [prev]: an earlier snapshot for the
     * per-second figures (null on the first reading).
     */
    fun format(prev: DiagSnapshot?): String {
        val dt = if (prev != null && atMs > prev.atMs) (atMs - prev.atMs) / 1000.0 else 0.0
        fun rate(now: Long, then: Long?): String =
            if (dt > 0 && then != null) String.format(Locale.ROOT, "%5.1f/s", (now - then) / dt) else "    -  "
        val scanMs = if (decoded > 0) scanNanos / 1e6 / decoded else 0.0
        val dupPct = if (rxFrames > 0) 100.0 * rxDuplicates / rxFrames else 0.0
        // The counters of an earlier stream stay readable after it ends; while a transfer is
        // running only its own protocol's figures are shown.
        val showRs96 = rs96K > 0 && (proto == NativeCore.PROTO_NONE || proto == NativeCore.PROTO_D3)
        val showOb = obK > 0 && (proto == NativeCore.PROTO_NONE || proto == NativeCore.PROTO_OB)
        val unitLabel = if (showOb && !showRs96) "有效单元  " else "有效码    "
        return buildString {
            append(String.format(Locale.ROOT, "协议      %s\n", protocolLabel()))
            append(String.format(Locale.ROOT, "相机采集  %7d  %s\n", cameraFrames, rate(cameraFrames, prev?.cameraFrames)))
            append(String.format(Locale.ROOT, "相机丢帧  %7d  %s (估算)\n", cameraMissed, rate(cameraMissed, prev?.cameraMissed)))
            append(String.format(Locale.ROOT, "队列丢帧  %7d  %s\n", queueDropped, rate(queueDropped, prev?.queueDropped)))
            append(String.format(Locale.ROOT, "解码帧    %7d  %s  %.1f ms/帧 ×%d\n", decoded, rate(decoded, prev?.decoded), scanMs, threads))
            append(String.format(Locale.ROOT, "%s%7d  %s\n", unitLabel, rxUnique, rate(rxUnique, prev?.rxUnique)))
            append(String.format(Locale.ROOT, "重复扫描  %7d  %s  %.0f%%\n", rxDuplicates, rate(rxDuplicates, prev?.rxDuplicates), dupPct))
            if (rxRedundant > 0) append(String.format(Locale.ROOT, "冗余帧    %7d\n", rxRedundant))
            if (showRs96) {
                append(String.format(Locale.ROOT, "RS96 有效行 %d / %d  (收到 %d 行,已解 %d)", rs96Useful, rs96K, rs96Rows, rs96Solved))
            }
            if (showOb) {
                if (showRs96) append('\n')
                append(String.format(Locale.ROOT, "OB 有效行 %d / %d  (已解 %d / %d 组)", obRows, obK, obSolved, obGroups))
            }
            if (!showRs96 && !showOb) append("有效行 -")
        }
    }

    private fun protocolLabel(): String = when (proto) {
        NativeCore.PROTO_RQ -> "CodeTrans (RQ)"
        NativeCore.PROTO_D3 -> if (rs96K > 0) "Decimen v3 · RS96" else "Decimen v3 · LT"
        NativeCore.PROTO_PLAIN -> "普通二维码"
        NativeCore.PROTO_OB -> String.format(Locale.ROOT, "光传码面%s(实验)  画面内 %d 个", obProfile(), codesInFrame)
        else -> "-"
    }

    private fun obProfile(): String = when (obMode) {
        1L -> " · 横带"
        2L -> " · 分块"
        else -> ""
    }
}
