package com.codetrans

import java.util.Locale

/** Localized words used in the status line (kept out of [Formatters] so it stays JVM-testable). */
data class StatusLabels(
    val protoRq: String = "CodeTrans",
    val protoD3: String = "Decimen",
    val protoPlain: String = "QR",
    val kindText: String = "Text",
    val kindImage: String = "Image",
    val kindFiles: String = "Files",
    /** Format with one %d argument, e.g. "%d codes". */
    val codesFormat: String = "%d codes",
)

object Formatters {
    private const val SEP = " · "

    /** Human-readable size with binary units: 999 B, 1.5 KB, 286 KB, 12 MB. */
    fun bytes(n: Long): String {
        if (n < 1024) return "${n.coerceAtLeast(0)} B"
        val units = arrayOf("KB", "MB", "GB", "TB")
        var v = n / 1024.0
        var i = 0
        while (v >= 1024 && i < units.size - 1) {
            v /= 1024
            i++
        }
        return if (v < 10) String.format(Locale.ROOT, "%.1f %s", v, units[i])
        else String.format(Locale.ROOT, "%d %s", v.toLong(), units[i])
    }

    /** protocol · kind · size · have/need · KB/s · N codes (unknown parts are omitted). */
    fun status(p: Progress, l: StatusLabels): String {
        val parts = ArrayList<String>(6)
        when (p.proto) {
            NativeCore.PROTO_RQ -> parts += l.protoRq
            NativeCore.PROTO_D3 -> parts += l.protoD3
            NativeCore.PROTO_PLAIN -> parts += l.protoPlain
        }
        when (p.kind) {
            NativeCore.KIND_TEXT -> parts += l.kindText
            NativeCore.KIND_IMAGE -> parts += l.kindImage
            NativeCore.KIND_FILES -> parts += l.kindFiles
        }
        if (p.totalBytes > 0) parts += bytes(p.totalBytes)
        if (p.need > 0) parts += "${p.have.coerceAtMost(p.need)}/${p.need}"
        parts += bytes(p.bytesPerSec) + "/s"
        parts += String.format(Locale.ROOT, l.codesFormat, p.codesInFrame)
        return parts.joinToString(SEP)
    }
}
