package com.codetrans

import android.app.Application
import android.graphics.BitmapFactory
import android.graphics.ImageFormat
import android.graphics.Matrix
import android.graphics.Rect
import android.media.Image
import androidx.camera.core.ImageInfo
import androidx.camera.core.ImageProxy
import androidx.camera.core.impl.TagBundle
import androidx.camera.core.impl.utils.ExifData
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.google.common.truth.Truth.assertThat
import org.junit.After
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.nio.ByteBuffer

/**
 * The whole receive path below the camera: frames go into ScanViewModel.analyzer exactly like
 * CameraX images (decoder threads, native rx, events, ResultStore, state flow). After one
 * transfer has completed, the next one (new content on the PC) must be received too.
 */
@RunWith(AndroidJUnit4::class)
class ScanFlowTest {
    private val instr = InstrumentationRegistry.getInstrumentation()
    private val assets = instr.context.assets
    private val app = instr.targetContext.applicationContext as Application
    private lateinit var vm: ScanViewModel

    @Before
    fun setUp() {
        instr.runOnMainSync {
            vm = ScanViewModel(app)
            vm.startScanning()
        }
    }

    @After
    fun tearDown() {
        instr.runOnMainSync { vm.stopScanning() }
    }

    private class Plane(override val buffer: ByteBuffer, override val rowStride: Int) : ImageProxy.PlaneProxy {
        override val pixelStride = 1
    }

    private class FakeImage(private val y: ByteBuffer, private val w: Int, private val h: Int) : ImageProxy {
        override val width get() = w
        override val height get() = h
        override val format get() = ImageFormat.YUV_420_888
        override val cropRect get() = Rect(0, 0, w, h)
        override fun close() {}
        override fun setCropRect(rect: Rect?) {}
        override fun getPlanes(): Array<ImageProxy.PlaneProxy> = arrayOf(Plane(y.duplicate(), w))
        override fun getImageInfo(): ImageInfo = object : ImageInfo {
            override fun getTagBundle(): TagBundle = TagBundle.emptyBundle()
            override fun getTimestamp() = 0L
            override fun getRotationDegrees() = 90
            override fun getSensorToBufferTransformMatrix() = Matrix()
            override fun populateExifData(exifBuilder: ExifData.Builder) {}
        }
        override fun getImage(): Image? = null
    }

    private fun loadY(name: String): FakeImage {
        val bmp = assets.open("frames/$name").use { BitmapFactory.decodeStream(it) }!!
        val w = bmp.width
        val h = bmp.height
        val px = IntArray(w * h)
        bmp.getPixels(px, 0, w, 0, 0, w, h)
        bmp.recycle()
        val y = ByteBuffer.allocateDirect(w * h)
        for (i in 0 until w * h) {
            val c = px[i]
            y.put(i, ((((c shr 16) and 0xFF) * 77 + ((c shr 8) and 0xFF) * 150 + (c and 0xFF) * 29) shr 8).toByte())
        }
        return FakeImage(y, w, h)
    }

    private fun frames(prefix: String) =
        assets.list("frames")!!.filter { it.startsWith(prefix) && it.endsWith(".png") }.sorted().map(::loadY)

    /** Shows the sequence (looping, ~30 fps) until a result newer than [afterId] is published. */
    private fun receive(prefix: String, afterId: Long): ReceivedResult {
        val seq = frames(prefix)
        val deadline = System.currentTimeMillis() + 30_000
        var i = 0
        var sawCodes = false
        while (System.currentTimeMillis() < deadline) {
            vm.analyzer.analyze(seq[i++ % seq.size])
            Thread.sleep(33)
            if (vm.codes.value.count > 0) sawCodes = true
            val r = vm.state.value.result
            if (r != null && r.id > afterId) return r
        }
        throw AssertionError("no new result for $prefix (codes seen: $sawCodes, state ${vm.state.value})")
    }

    @Test
    fun receivesEveryTransferInARow() {
        var last = -1L
        val r1 = receive("d3rs_", last).also { last = it.id }
        assertThat(r1.kind).isEqualTo(ResultKind.TEXT)
        // the PC still shows the finished transfer for a while: nothing new must come of it
        val seq = frames("d3rs_")
        repeat(30) { vm.analyzer.analyze(seq[it % seq.size]); Thread.sleep(33) }
        assertThat(vm.state.value.result?.id).isEqualTo(last)

        val r2 = receive("d3lt_", last).also { last = it.id }
        assertThat(r2.kind).isEqualTo(ResultKind.FILES)
        val r3 = receive("text_", last).also { last = it.id }
        assertThat(r3.kind).isEqualTo(ResultKind.TEXT)
        val r4 = receive("files_", last).also { last = it.id }
        assertThat(r4.kind).isEqualTo(ResultKind.FILES)
        assertThat(vm.state.value).isInstanceOf(ScanState.Done::class.java)
    }
}
