package com.codetrans

import android.graphics.BitmapFactory
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.google.common.truth.Truth.assertThat
import org.junit.After
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.nio.ByteBuffer

/**
 * PLAN §14 A1 / §15: recorded frame sequences (assets/frames, made by testdata-gen/gen_frames.c)
 * go through the real JNI path (copyPlane -> scan -> takeResult) and must reproduce the
 * original bytes exactly.
 */
@RunWith(AndroidJUnit4::class)
class NativeCoreTest {
    private val instr = InstrumentationRegistry.getInstrumentation()
    private val assets = instr.context.assets
    private lateinit var outDir: File
    private var rx = 0L
    private var scanner = 0L

    @Before
    fun setUp() {
        assertThat(NativeCore.isAvailable).isTrue()
        outDir = File(instr.targetContext.cacheDir, "rx-test").apply { deleteRecursively(); mkdirs() }
        rx = NativeCore.rxNew(outDir.absolutePath, 0, true, true)
        scanner = NativeCore.scannerNew()
        assertThat(rx).isNotEqualTo(0L)
        assertThat(scanner).isNotEqualTo(0L)
    }

    @After
    fun tearDown() {
        NativeCore.scannerFree(scanner)
        NativeCore.rxFree(rx)
        outDir.deleteRecursively()
    }

    private class Frame(val y: ByteBuffer, val w: Int, val h: Int)

    /** PNG -> tightly packed luminance in a direct buffer (what DecodePipeline hands to scan). */
    private fun loadFrame(name: String, rowPadding: Int = 0): Frame {
        val bmp = assets.open("frames/$name").use { BitmapFactory.decodeStream(it) }!!
        val w = bmp.width
        val h = bmp.height
        val px = IntArray(w * h)
        bmp.getPixels(px, 0, w, 0, 0, w, h)
        bmp.recycle()
        val stride = w + rowPadding
        // A camera plane: rowStride >= width, the last row may be exactly `width` long.
        val src = ByteBuffer.allocateDirect(stride * (h - 1) + w)
        for (row in 0 until h) {
            for (x in 0 until w) {
                val c = px[row * w + x]
                val luma = (((c shr 16) and 0xFF) * 77 + ((c shr 8) and 0xFF) * 150 + (c and 0xFF) * 29) shr 8
                src.put(row * stride + x, luma.toByte())
            }
        }
        val dst = ByteBuffer.allocateDirect(w * h)
        NativeCore.copyPlane(src, stride, w, h, dst)
        return Frame(dst, w, h)
    }

    private fun frames(prefix: String) =
        assets.list("frames")!!.filter { it.startsWith(prefix) && it.endsWith(".png") }.sorted()

    private fun asset(name: String) = assets.open("frames/$name").use { it.readBytes() }

    private fun scanAll(
        prefix: String,
        rowPadding: Int = 0,
        checkProgress: Boolean = true,
        proto: Int = NativeCore.PROTO_RQ,
        kind: Int? = null,
    ): Int {
        val names = frames(prefix)
        assertThat(names).isNotEmpty()
        var events = 0
        names.forEachIndexed { i, name ->
            val f = loadFrame(name, rowPadding)
            events = events or NativeCore.scan(scanner, rx, f.y, f.w, f.h, null)
            if (i == 0 && checkProgress) {
                val p = LongArray(NativeCore.PROGRESS_SIZE)
                NativeCore.progress(rx, p)
                val prog = Progress.from(p)
                assertThat(prog.proto).isEqualTo(proto)
                if (kind != null) assertThat(prog.kind).isEqualTo(kind)
                assertThat(prog.codesInFrame).isEqualTo(2)
                assertThat(prog.have).isGreaterThan(0L)
                assertThat(prog.need).isGreaterThan(prog.have)
            }
        }
        return events
    }

    @Test
    fun textTransferReproducesOriginal() {
        val ev = scanAll("text_", rowPadding = 64)
        assertThat(ev and NativeCore.EV_COMPLETE).isNotEqualTo(0)
        assertThat(ev and NativeCore.EV_FAILED).isEqualTo(0)

        val r = NativeCore.takeResult(rx)!!
        val expected = asset("text.txt")
        assertThat(r.kind).isEqualTo(NativeCore.KIND_TEXT)
        assertThat(r.mime).isEqualTo("text/plain")
        assertThat(r.textTruncated).isFalse()
        // CJK + 4-byte emoji must survive the UTF-8 -> UTF-16 conversion in jni_bridge.c.
        assertThat(r.text).isEqualTo(String(expected, Charsets.UTF_8))
        assertThat(r.paths).hasLength(1)
        assertThat(File(r.paths[0]).readBytes()).isEqualTo(expected)
        assertThat(r.sizes[0]).isEqualTo(expected.size.toLong())
        assertThat(NativeCore.takeResult(rx)).isNull()
    }

    @Test
    fun filesTransferReproducesOriginals() {
        val ev = scanAll("files_")
        assertThat(ev and NativeCore.EV_COMPLETE).isNotEqualTo(0)

        val r = NativeCore.takeResult(rx)!!
        val names = String(asset("files.txt"), Charsets.UTF_8).trim().lines()
        assertThat(r.kind).isEqualTo(NativeCore.KIND_FILES)
        assertThat(r.names.toList()).isEqualTo(names)
        assertThat(File(r.paths[0]).readBytes()).isEqualTo(asset("file0.bin"))
        assertThat(File(r.paths[1]).readBytes()).isEqualTo(asset("file1.bin"))
        r.paths.forEach { assertThat(File(it).canonicalPath).startsWith(outDir.canonicalPath + File.separator) }

        // The finished session is ignored from now on (completed-session LRU).
        val again = scanAll("files_", checkProgress = false)
        assertThat(again and (NativeCore.EV_COMPLETE or NativeCore.EV_PROGRESS)).isEqualTo(0)
    }

    /** Decimen v3 with the 0.5.3-fixed Cauchy RS-96 outer code (flag 0x02): a text snippet. */
    @Test
    fun decimenRs96TextReproducesOriginal() {
        val ev = scanAll("d3rs_", rowPadding = 16, proto = NativeCore.PROTO_D3, kind = NativeCore.KIND_TEXT)
        assertThat(ev and NativeCore.EV_COMPLETE).isNotEqualTo(0)
        assertThat(ev and (NativeCore.EV_FAILED or NativeCore.EV_UNSUPPORTED)).isEqualTo(0)

        val r = NativeCore.takeResult(rx)!!
        val expected = asset("text.txt")
        assertThat(r.kind).isEqualTo(NativeCore.KIND_TEXT)
        assertThat(r.mime).isEqualTo("text/plain")
        assertThat(r.text).isEqualTo(String(expected, Charsets.UTF_8))
        assertThat(File(r.paths[0]).readBytes()).isEqualTo(expected)
        assertThat(File(r.paths[0]).canonicalPath).startsWith(outDir.canonicalPath + File.separator)
    }

    /** Decimen v3 with the stock v0.5.3 LT carousel: one binary file, repair frames needed. */
    @Test
    fun decimenLtFileReproducesOriginal() {
        val ev = scanAll("d3lt_", proto = NativeCore.PROTO_D3, kind = NativeCore.KIND_FILES)
        assertThat(ev and NativeCore.EV_COMPLETE).isNotEqualTo(0)
        assertThat(ev and (NativeCore.EV_FAILED or NativeCore.EV_UNSUPPORTED)).isEqualTo(0)

        val r = NativeCore.takeResult(rx)!!
        assertThat(r.kind).isEqualTo(NativeCore.KIND_FILES)
        assertThat(r.names.toList()).containsExactly("data.bin")
        assertThat(File(r.paths[0]).readBytes()).isEqualTo(asset("file1.bin"))
    }

    @Test
    fun decimenIgnoredWhenDisabled() {
        NativeCore.rxSetOptions(rx, false, true)
        val ev = scanAll("d3rs_", checkProgress = false)
        assertThat(ev).isEqualTo(0)
        assertThat(NativeCore.takeResult(rx)).isNull()
        assertThat(NativeCore.takePlain(rx)).isNull()   // never shown as plain QR text either
    }

    /** A blurred frame quirc cannot read on its own; the Otsu + high-boost retry can. */
    @Test
    fun contrastRetryRescuesBlurredFrame() {
        assumeTrue(assets.list("frames")!!.contains("d3hard.png"))
        val f = loadFrame("d3hard.png")
        val ev = NativeCore.scan(scanner, rx, f.y, f.w, f.h, null)
        assertThat(ev and NativeCore.EV_COMPLETE).isNotEqualTo(0)
        val r = NativeCore.takeResult(rx)!!
        assertThat(r.kind).isEqualTo(NativeCore.KIND_TEXT)
        assertThat(r.text).isEqualTo(String(asset("d3hard.txt"), Charsets.UTF_8))
    }

    /** The scanner reports where each code is (for the boxes drawn over the preview). */
    @Test
    fun scanReportsCodeCorners() {
        val f = loadFrame("plain.png", rowPadding = 32)
        val quads = IntArray(NativeCore.QUADS_SIZE)
        NativeCore.scan(scanner, rx, f.y, f.w, f.h, quads)
        assertThat(quads[0]).isEqualTo(1)
        assertThat(quads[1]).isEqualTo(1)   // decoded
        for (k in 0 until 4) {
            assertThat(quads[2 + 2 * k]).isIn(0 until f.w)
            assertThat(quads[3 + 2 * k]).isIn(0 until f.h)
        }
        val blank = ByteBuffer.allocateDirect(f.w * f.h).apply { put(ByteArray(f.w * f.h) { 0x7f }); clear() }
        NativeCore.scan(scanner, rx, blank, f.w, f.h, quads)
        assertThat(quads[0]).isEqualTo(0)
    }

    @Test
    fun trackedGridReadsWhenOneFinderIsDamaged() {
        val clean = loadFrame("plain.png")
        val damaged = ByteBuffer.allocateDirect(clean.w * clean.h)
        damaged.put(clean.y.duplicate())
        // Fill the white ring of one finder. The other two finders and the payload remain intact.
        for (row in 162 until 183)
            for (col in 412 until 433) damaged.put(row * clean.w + col, 24)
        val quads = IntArray(NativeCore.QUADS_SIZE)
        fun reads(y: ByteBuffer): Int {
            NativeCore.scan(scanner, rx, y, clean.w, clean.h, quads)
            return DetectedCodes(clean.w, clean.h, 0, quads, 0, 0).decodedCount
        }
        assertThat(reads(damaged)).isEqualTo(0)
        assertThat(reads(clean.y)).isEqualTo(1)
        // Includes a periodic full scan while tracking the damaged finder.
        repeat(16) { assertThat(reads(damaged)).isEqualTo(1) }

        val secondWorker = NativeCore.scannerNew()
        try {
            val cache = ConfirmedRegions()
            val regions = IntArray(ConfirmedRegions.BUFFER_SIZE)
            fun scan(worker: Long, y: ByteBuffer, frame: Long, ms: Long): Int {
                cache.seeds(frame, ms, regions)
                NativeCore.scan(worker, rx, y, clean.w, clean.h, quads, regions)
                cache.observed(frame, ms, regions)
                return DetectedCodes(clean.w, clean.h, 0, quads, frame, ms).decodedCount
            }
            val blank = ByteBuffer.allocateDirect(clean.w * clean.h)
            assertThat(scan(scanner, clean.y, 1, 100)).isEqualTo(1)
            assertThat(scan(secondWorker, blank, 2, 150)).isEqualTo(0)
            assertThat(scan(secondWorker, damaged, 3, 950)).isEqualTo(1)
            assertThat(scan(secondWorker, damaged, 4, 2600)).isEqualTo(0)
        } finally {
            NativeCore.scannerFree(secondWorker)
        }
    }

    @Test
    fun denseQrTransferSurvivesRadialDistortionOnColdAcquisition() {
        // Warp each 512px crop separately, as in the optical comparison. Both sides
        // carry different LT symbols. The recovered file verifies those bytes end to end.
        val size = 512
        val pixels = ByteArray(size * size)
        val distorted = ByteBuffer.allocateDirect(pixels.size)
        val regions = IntArray(ConfirmedRegions.BUFFER_SIZE)
        val quads = IntArray(NativeCore.QUADS_SIZE)
        var events = 0
        for (name in frames("d3lt_")) {
            val f = loadFrame(name)
            for (offset in intArrayOf(0, 620)) {
                for (y in 0 until size) {
                    f.y.position((y + 70) * f.w + offset)
                    f.y.get(pixels, y * size, size)
                }
                distorted.clear()
                for (y in 0 until size) for (x in 0 until size) {
                    val dx = x - 256.0
                    val dy = y - 256.0
                    val scale = 1 + 0.04 * (dx * dx + dy * dy) / (256 * 256)
                    val px = (256 + dx * scale).coerceIn(0.0, 510.999)
                    val py = (256 + dy * scale).coerceIn(0.0, 510.999)
                    val i = py.toInt() * size + px.toInt()
                    val fx = px - px.toInt()
                    val fy = py - py.toInt()
                    val top = (pixels[i].toInt() and 255) * (1-fx) + (pixels[i+1].toInt() and 255) * fx
                    val bottom = (pixels[i+size].toInt() and 255) * (1-fx) + (pixels[i+size+1].toInt() and 255) * fx
                    distorted.put((top * (1-fy) + bottom * fy).toInt().toByte())
                }
                val cold = NativeCore.scannerNew()
                try {
                    regions[0] = 0
                    events = events or NativeCore.scan(cold, rx, distorted, size, size, quads, regions)
                    assertThat(regions[0]).isEqualTo(1)
                } finally {
                    NativeCore.scannerFree(cold)
                }
            }
        }
        assertThat(events and NativeCore.EV_COMPLETE).isNotEqualTo(0)
        assertThat(File(NativeCore.takeResult(rx)!!.paths.single()).readBytes()).isEqualTo(asset("file1.bin"))
    }

    @Test
    fun scannerFindsNewNeighbourAndReacquiresMovingCodes() {
        val source = loadFrame("text_000.png")
        val row = ByteArray(source.w)
        val y = ByteBuffer.allocateDirect(source.w * source.h)
        val quads = IntArray(NativeCore.QUADS_SIZE)
        fun scan(leftOnly: Boolean, dx: Int): List<Int> {
            source.y.rewind()
            y.clear()
            repeat(source.h) {
                source.y.get(row)
                if (leftOnly) row.fill(200.toByte(), source.w / 2)
                repeat(dx) { y.put(200.toByte()) }
                y.put(row, 0, source.w - dx)
            }
            NativeCore.scan(scanner, rx, y, source.w, source.h, quads)
            val codes = DetectedCodes(source.w, source.h, 0, quads, 0, 0)
            return (0 until codes.count).filter { codes.decoded(it) }
                .map { i -> (0..3).minOf { k -> codes.x(i, k) } }.sorted()
        }
        val initial = scan(leftOnly = true, dx = 0).single()
        var positions = emptyList<Int>()
        for (i in 0 until 14) {
            positions = scan(leftOnly = false, dx = 0)
            if (positions.size == 2) break
        }
        assertThat(positions).hasSize(2)
        for (dx in listOf(2, 26, 140, 0)) {
            val moved = scan(leftOnly = false, dx = dx)
            assertThat(moved).hasSize(2)
            assertThat(moved.first()).isWithin(3).of(initial + dx)
        }
    }

    @Test
    fun plainQrIsReportedOnce() {
        val f = loadFrame("plain.png", rowPadding = 32)
        val ev = NativeCore.scan(scanner, rx, f.y, f.w, f.h, null)
        assertThat(ev and NativeCore.EV_PLAIN).isNotEqualTo(0)
        val bytes = NativeCore.takePlain(rx)!!
        assertThat(bytes).isEqualTo(asset("plain.txt"))
        assertThat(PlainText.decode(bytes)).isEqualTo(String(asset("plain.txt"), Charsets.UTF_8))

        assertThat(NativeCore.scan(scanner, rx, f.y, f.w, f.h, null) and NativeCore.EV_PLAIN).isEqualTo(0)
        assertThat(NativeCore.takePlain(rx)).isNull()
    }

    @Test
    fun plainQrIgnoredWhenDisabled() {
        NativeCore.rxSetOptions(rx, true, false)
        val f = loadFrame("plain.png")
        assertThat(NativeCore.scan(scanner, rx, f.y, f.w, f.h, null) and NativeCore.EV_PLAIN).isEqualTo(0)
        assertThat(NativeCore.takePlain(rx)).isNull()
    }

    @Test
    fun resetDropsProgress() {
        val f = loadFrame(frames("text_").first())
        assertThat(NativeCore.scan(scanner, rx, f.y, f.w, f.h, null) and NativeCore.EV_PROGRESS).isNotEqualTo(0)
        NativeCore.rxReset(rx)
        val p = LongArray(NativeCore.PROGRESS_SIZE)
        NativeCore.progress(rx, p)
        assertThat(p[NativeCore.P_PROTO]).isEqualTo(NativeCore.PROTO_NONE.toLong())
    }

    @Test
    fun invalidArgumentsAreHarmless() {
        val small = ByteBuffer.allocateDirect(16)
        assertThat(NativeCore.scan(scanner, rx, small, 1280, 720, null)).isEqualTo(0)  // buffer too small
        assertThat(NativeCore.scan(0, rx, small, 4, 4, null)).isEqualTo(0)
        assertThat(NativeCore.scan(scanner, 0, small, 4, 4, null)).isEqualTo(0)
        NativeCore.copyPlane(small, 2, 4, 4, small)   // rowStride < width: ignored
        assertThat(NativeCore.takeResult(0)).isNull()
        assertThat(NativeCore.takePlain(0)).isNull()
        NativeCore.progress(0, LongArray(NativeCore.PROGRESS_SIZE))
    }
}
