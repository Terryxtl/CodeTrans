package com.codetrans

import android.os.SystemClock
import android.util.Log
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.util.Locale
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.ThreadFactory
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

/**
 * Fixed pool of direct Y-plane buffers. [acquire] returns null when every buffer is in flight:
 * the caller drops the frame (back-pressure). A size change discards buffers of the old size.
 */
class FramePool(val capacity: Int) {
    private val free = ArrayDeque<ByteBuffer>()
    private var allocated = 0
    private var size = -1

    @Synchronized
    fun acquire(bytes: Int): ByteBuffer? {
        if (bytes != size) {
            free.clear()
            allocated = 0
            size = bytes
        }
        val b = free.removeFirstOrNull()
            ?: if (allocated < capacity) {
                allocated++
                ByteBuffer.allocateDirect(bytes)
            } else {
                null
            }
        b?.clear()
        return b
    }

    @Synchronized
    fun release(b: ByteBuffer) {
        if (b.capacity() == size && free.size < capacity) free.addLast(b)
    }

    @get:Synchronized
    val freeCount: Int get() = free.size + (capacity - allocated)
}

/**
 * Camera frames -> native decoder (PLAN §3/§7).
 *
 * The analyzer copies only the Y plane into a free pooled buffer (C memcpy, no Java heap) and
 * closes the ImageProxy at once; N decoder threads, each owning one native scanner, run scan().
 * Event bits are reported through [onEvents] on decoder threads.
 */
class DecodePipeline(
    private val rx: Long,
    private val onEvents: (Int) -> Unit,
    /** Every decoded frame (decoder threads): where the codes are, possibly none. */
    private val onCodes: (DetectedCodes) -> Unit,
) {
    private val lock = Any()
    private val scanner = ThreadLocal<Long>()
    // Not ThreadLocal.withInitial: that is API 26, minSdk is 24.
    private val quadsBuf = object : ThreadLocal<IntArray>() {
        override fun initialValue() = IntArray(NativeCore.QUADS_SIZE)
    }

    /**
     * Native scan session: the confirmed-region cache shared by the decoder threads (locked
     * inside the JNI bridge). Freed by [close] once every decoder thread has stopped.
     */
    private var session = if (rx != 0L && NativeCore.isAvailable) NativeCore.sessionNew() else 0L
    /** Geometry the session's cache belongs to; touched on the serial analyzer (and reset by [start]). */
    @Volatile private var geometryW = 0
    @Volatile private var geometryH = 0
    @Volatile private var geometryRotation = -1

    @Volatile private var executor: ThreadPoolExecutor? = null
    @Volatile private var pool: FramePool? = null
    private var scannersGone: CountDownLatch? = null
    private val stopping = CopyOnWriteArrayList<CountDownLatch>()

    @Volatile private var dumpDir: File? = null
    private val frameNo = AtomicLong()
    /** Diagnostics counters (developer panel). */
    val stats = PipelineStats()
    /** Analyzer thread only. */
    private val gaps = FrameGapEstimator()
    @Volatile var threads = 0
        private set

    val isRunning: Boolean get() = executor != null

    val analyzer = ImageAnalysis.Analyzer { image ->
        try {
            stats.cameraFrames.incrementAndGet()
            gaps.onFrame(image.imageInfo.timestamp)
            stats.cameraMissed.set(gaps.missed)
            submit(image)
        } catch (t: Throwable) {
            Log.e(TAG, "analyzer failed", t)
        } finally {
            image.close()
        }
    }

    /** Starts [threads] decoder threads (no-op if running). [dumpFramesTo]: debug Y-frame export. */
    fun start(threads: Int, dumpFramesTo: File? = null) {
        synchronized(lock) {
            if (executor != null || rx == 0L) return
            val n = threads.coerceIn(1, 8)
            geometryW = 0 // a restart begins with an empty cache
            val latch = CountDownLatch(n)
            val idx = AtomicInteger()
            val factory = ThreadFactory { r ->
                Thread({
                    val s = NativeCore.scannerNew()
                    scanner.set(s)
                    try {
                        r.run()
                    } finally {
                        scanner.remove()
                        if (s != 0L) NativeCore.scannerFree(s)
                        latch.countDown()
                    }
                }, "cp-decode-${idx.incrementAndGet()}")
            }
            dumpDir = dumpFramesTo?.apply { mkdirs() }
            this.threads = n
            pool = FramePool(n + 2)
            scannersGone = latch
            executor = ThreadPoolExecutor(n, n, 0L, TimeUnit.MILLISECONDS, LinkedBlockingQueue(), factory)
                .also { it.prestartAllCoreThreads() }
            Log.i(TAG, "decode pipeline started with $n threads")
        }
    }

    /**
     * Stops the decoder threads without blocking (queued frames are dropped; a scan in progress
     * finishes and frees its scanner). rx is untouched. Use [awaitStopped] before freeing rx.
     */
    fun stop() {
        val ex: ThreadPoolExecutor
        synchronized(lock) {
            ex = executor ?: return
            scannersGone?.let { stopping += it }
            executor = null
            pool = null
            scannersGone = null
        }
        // The first frame after a restart is not a "gap". A frame being analysed right now
        // can at worst miscount one gap; this only feeds the developer panel.
        gaps.reset()
        stats.frameWidth = 0
        stats.frameHeight = 0
        ex.shutdownNow()
    }

    /** Waits until every decoder thread of every stopped pool has exited. */
    fun awaitStopped(timeoutMs: Long): Boolean {
        val deadline = System.nanoTime() + timeoutMs * 1_000_000
        for (latch in stopping) {
            val left = deadline - System.nanoTime()
            if (left <= 0 || !latch.await(left, TimeUnit.NANOSECONDS)) return false
            stopping.remove(latch)
        }
        return true
    }

    /** Frees the native session. Only after [awaitStopped] returned true; the pipeline cannot be restarted. */
    fun close() {
        synchronized(lock) {
            if (session != 0L) NativeCore.sessionFree(session)
            session = 0L
        }
    }

    private fun submit(image: ImageProxy) {
        val (ex, pool) = synchronized(lock) { (executor ?: return) to (pool ?: return) }
        val w = image.width
        val h = image.height
        stats.frameWidth = w
        stats.frameHeight = h
        val plane = image.planes[0]
        val buf = pool.acquire(w * h)
        if (buf == null) {
            stats.queueDropped.incrementAndGet()
            return
        }
        NativeCore.copyPlane(plane.buffer, plane.rowStride, w, h, buf)
        val rotation = image.imageInfo.rotationDegrees
        val n = frameNo.incrementAndGet()
        val capturedAt = SystemClock.uptimeMillis()
        // A new frame size / rotation makes every cached grid meaningless. Frames still in
        // flight with an older number no longer touch the cache.
        val sess = synchronized(lock) {
            if (session != 0L && (w != geometryW || h != geometryH || rotation != geometryRotation)) {
                NativeCore.sessionReset(session, n)
            }
            geometryW = w
            geometryH = h
            geometryRotation = rotation
            session
        }
        val dump = dumpDir?.takeIf { n % DUMP_EVERY == 0L }
        try {
            ex.execute { decode(buf, w, h, rotation, pool, dump, n, capturedAt, sess) }
        } catch (_: RejectedExecutionException) {
            pool.release(buf)
        }
    }

    private fun decode(buf: ByteBuffer, w: Int, h: Int, rotation: Int, pool: FramePool, dump: File?,
                       n: Long, capturedAt: Long, sess: Long) {
        try {
            if (dump != null) writeFrame(dump, n, buf, w, h)
            val s = scanner.get() ?: return
            val q = quadsBuf.get()!!
            q[0] = 0
            val t0 = System.nanoTime()
            val ev = if (sess != 0L) NativeCore.scanStep(s, rx, sess, buf, w, h, n, capturedAt, q)
            else NativeCore.scan(s, rx, buf, w, h, q)
            stats.scanNanos.addAndGet(System.nanoTime() - t0)
            stats.decoded.incrementAndGet()
            if (ev != 0) onEvents(ev)
            val used = 1 + q[0].coerceIn(0, NativeCore.MAX_QUADS) * NativeCore.QUAD_INTS
            synchronized(lock) {
                if (this.pool === pool) onCodes(DetectedCodes(w, h, rotation, q.copyOf(used), n, capturedAt))
            }
        } catch (t: Throwable) {
            Log.e(TAG, "decode failed", t)
        } finally {
            pool.release(buf)
        }
    }

    /** Debug only: raw 8-bit luminance, w*h bytes, plus an index line "name w h". */
    private fun writeFrame(dir: File, n: Long, buf: ByteBuffer, w: Int, h: Int) {
        runCatching {
            val name = String.format(Locale.ROOT, "frame_%07d.y", n)
            FileOutputStream(File(dir, name)).use { it.channel.write(buf.duplicate().apply { clear() }) }
            synchronized(this) { File(dir, "frames.txt").appendText("$name $w $h\n") }
        }.onFailure { Log.w(TAG, "frame dump failed", it) }
    }

    companion object {
        private const val TAG = "CodeTrans"
        /** Debug export: every Nth analysed frame. */
        const val DUMP_EVERY = 15L
    }
}
