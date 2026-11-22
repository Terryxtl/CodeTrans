package com.codetrans

import android.app.ActivityManager
import android.app.Application
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import android.os.SystemClock
import android.util.Log
import androidx.core.content.getSystemService
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Owns the native receiver (rx) so a transfer survives rotation and backgrounding; rx is
 * freed only in [onCleared]. Exposes [state] (StateFlow) and one-shot [events].
 */
class ScanViewModel(app: Application) : AndroidViewModel(app) {
    private val prefs = Prefs(app)
    private val store = ResultStore(app)
    private val feedback = Feedback(app)

    private val rx: Long = if (NativeCore.isAvailable) {
        runCatching {
            NativeCore.rxNew(store.rxRoot.absolutePath, maxBytes(app), prefs.decimen, prefs.plainQr)
        }.onFailure { Log.e(TAG, "rxNew failed", it) }.getOrDefault(0L)
    } else {
        0L
    }

    private val pipeline = DecodePipeline(rx, ::onNativeEvents, ::onCodes)
    val analyzer get() = pipeline.analyzer

    private val tracker = QuadTracker()
    private val _codes = MutableStateFlow(CodeSnapshot.EMPTY)
    /** Stabilised boxes of the codes in view (per frame, and pruned ~10 Hz). */
    val codes: StateFlow<CodeSnapshot> = _codes.asStateFlow()

    private val _detection = MutableStateFlow(Detection.NONE)
    /** Summary of [codes] for the status line, ~10 Hz. */
    val detection: StateFlow<Detection> = _detection.asStateFlow()
    private var undecodedSince = 0L
    private var lastLoggedDetection = Detection.NONE
    private var lastDetectionLogAt = 0L

    /** The progress bar never moves backwards within one session. */
    private var progressSession = -1L
    private var progressHave = 0L

    private val _state = MutableStateFlow<ScanState>(
        if (rx == 0L) ScanState.Error(R.string.error_native, null) else ScanState.Idle,
    )
    val state: StateFlow<ScanState> = _state.asStateFlow()

    private val _events = Channel<UiEvent>(Channel.BUFFERED)
    val events: Flow<UiEvent> = _events.receiveAsFlow()

    /** Set by EV_PROGRESS, cleared by EV_COMPLETE / EV_FAILED. */
    private val receiving = AtomicBoolean(false)
    @Volatile private var lastUnsupportedAt = 0L
    private var pollJob: Job? = null
    private var progressJob: Job? = null
    private val progressBuf = LongArray(NativeCore.PROGRESS_SIZE)
    /**
     * Decoder threads signal every accepted symbol here; the main thread refreshes the progress
     * at most every [PROGRESS_MIN_MS]. Polling alone (every 100 ms) left the bar up to 100 ms
     * behind on top of the indicator's own animation, so short transfers finished while the
     * bar still showed well under 100 %.
     */
    private val progressTick = Channel<Unit>(Channel.CONFLATED)

    private val fpsMeter = FpsMeter()
    private var fpsAt = 0L
    private val _frameInfo = MutableStateFlow<FrameInfo?>(null)
    /** Frame rate and analysis resolution for the overlay on the camera view, ~2 Hz; null = unknown. */
    val frameInfo: StateFlow<FrameInfo?> = _frameInfo.asStateFlow()

    /** Developer panel text (debug builds, while enabled), ~2 Hz; null = hidden. */
    private val _diagnostics = MutableStateFlow<String?>(null)
    val diagnostics: StateFlow<String?> = _diagnostics.asStateFlow()
    /** The toolbar button expands (true) / collapses the panel; debug builds only. */
    var diagnosticsEnabled = BuildConfig.DEBUG
        set(value) {
            field = value && BuildConfig.DEBUG
            if (field) {
                // Fresh figures: per-second rates against a reading from before the panel was
                // collapsed would be averages over the whole time it was hidden.
                diagPrev = null
                diagAt = 0L
            } else {
                _diagnostics.value = null
            }
        }
    private var diagPrev: DiagSnapshot? = null
    private var diagAt = 0L
    private val statsBuf = LongArray(NativeCore.STATS_SIZE)
    private val diagProgressBuf = LongArray(NativeCore.PROGRESS_SIZE)

    /** Thermal status reached SEVERE: 1 decoder thread, 720p, 30 fps. */
    var thermalThrottled = false
        private set

    private val autoThreads by lazy {
        val n = Runtime.getRuntime().availableProcessors()
        Tuning.autoThreads(Tuning.readCpuMaxFreqs(n), n)
    }

    private var thumbCache: Pair<Long, Bitmap>? = null

    // ---- settings -> effective configuration

    fun cameraConfig() = CameraController.Config(
        analysisSize = if (thermalThrottled) Prefs.analysisSize(720) else prefs.analysisSize,
        fps = if (thermalThrottled) 30 else prefs.fps,
    )

    fun decoderThreads(): Int = when {
        thermalThrottled -> 1
        prefs.threads > 0 -> prefs.threads
        else -> autoThreads
    }

    /** Returns true if the throttle state changed. */
    fun setThermalSevere(severe: Boolean): Boolean {
        if (severe == thermalThrottled) return false
        thermalThrottled = severe
        return true
    }

    // ---- lifecycle of the decoder (driven by the Activity's onStart / onStop)

    fun startScanning() {
        if (rx == 0L) return
        NativeCore.rxSetOptions(rx, prefs.decimen, prefs.plainQr)
        val dump = if (prefs.dumpFrames) getApplication<Application>().getExternalFilesDir("frames") else null
        pipeline.start(decoderThreads(), dump)
        if (pollJob == null) {
            pollJob = viewModelScope.launch {
                while (isActive) {
                    delay(PROGRESS_INTERVAL_MS)
                    pollDetection()
                    pollProgress()
                    pollFrameInfo()
                    pollDiagnostics()
                }
            }
        }
        if (progressJob == null) {
            progressJob = viewModelScope.launch {
                for (tick in progressTick) {
                    pollProgress()
                    delay(PROGRESS_MIN_MS)
                }
            }
        }
    }

    fun stopScanning() {
        pollJob?.cancel()
        pollJob = null
        progressJob?.cancel()
        progressJob = null
        pipeline.stop()
        tracker.clear()
        _codes.value = CodeSnapshot.EMPTY
        _detection.value = Detection.NONE
        undecodedSince = 0L
        fpsMeter.reset()
        fpsAt = 0L
        _frameInfo.value = null
    }

    /** Restart decoder threads (thread count changed). */
    fun restartScanning() {
        stopScanning()
        startScanning()
    }

    override fun onCleared() {
        stopScanning()
        tracker.close()
        if (rx != 0L) {
            // Free rx only after every decoder thread has left scan().
            Thread({
                if (pipeline.awaitStopped(5_000)) {
                    pipeline.close()
                    NativeCore.rxFree(rx)
                } else Log.w(TAG, "decoder threads still running; leaking rx")
            }, "cp-rx-free").start()
        }
    }

    // ---- native events (decoder threads)

    private fun onNativeEvents(ev: Int) {
        if (ev and NativeCore.EV_PROGRESS != 0) {
            receiving.set(true)
            progressTick.trySend(Unit)
        }
        if (ev and NativeCore.EV_COMPLETE != 0) {
            receiving.set(false)
            val n = NativeCore.takeResult(rx)
            val r = n?.let { store.accept(it) }
            Log.i(TAG, "transfer complete: native result ${n != null}, accepted ${r?.id} (${r?.kind}, ${n?.paths?.size} files)")
            if (r != null) viewModelScope.launch { publish(r) }
        }
        if (ev and NativeCore.EV_PLAIN != 0) {
            NativeCore.takePlain(rx)?.let { bytes ->
                val r = store.acceptPlain(PlainText.decode(bytes))
                viewModelScope.launch { publish(r) }
            }
        }
        if (ev and NativeCore.EV_FAILED != 0) {
            Log.w(TAG, "transfer failed verification, receiving again")
            receiving.set(false)
            viewModelScope.launch {
                _state.value = store.current?.let { ScanState.Done(it) } ?: ScanState.Idle
                _events.trySend(UiEvent(R.string.msg_failed))
            }
        }
        if (ev and NativeCore.EV_UNSUPPORTED != 0) {
            val now = SystemClock.elapsedRealtime()
            if (now - lastUnsupportedAt > UNSUPPORTED_INTERVAL_MS) {
                Log.w(TAG, "unsupported codes in view (Decimen version / flags / size)")
                lastUnsupportedAt = now
                _events.trySend(UiEvent(R.string.msg_decimen_unsupported))
            }
        }
    }

    /** Decoder threads, every frame (late frames are ignored by the tracker). */
    private fun onCodes(c: DetectedCodes) {
        synchronized(tracker) { _codes.value = tracker.update(c, c.atMs) }
    }

    /** Main thread, ~10 Hz. */
    private fun pollDetection() {
        val now = SystemClock.uptimeMillis()
        val snap = synchronized(tracker) {
            tracker.snapshot(now).also { _codes.value = it }
        }
        val d = if (snap.sightedCount == 0) {
            undecodedSince = 0L
            Detection.NONE
        } else {
            if (snap.readCount > 0) undecodedSince = 0L
            else if (undecodedSince == 0L) undecodedSince = now
            Detection(snap.sightedCount, snap.readCount, undecodedSince != 0L && now - undecodedSince >= STUCK_MS)
        }
        _detection.value = d
        if (d != lastLoggedDetection && now - lastDetectionLogAt >= 1_000) {
            Log.i(TAG, "in view: ${d.count} codes, ${d.decoded} read${if (d.stuck) ", unreadable" else ""}; state ${stateName()}")
            lastLoggedDetection = d
            lastDetectionLogAt = now
        }
    }

    private fun stateName() = when (val s = _state.value) {
        is ScanState.Receiving -> "receiving ${s.progress.have}/${s.progress.need}"
        is ScanState.Done -> "done #${s.result.id}"
        is ScanState.Error -> "error"
        ScanState.Idle -> "idle"
    }

    /** Main thread, every [FPS_INTERVAL_MS]: processed frames per second + analysis resolution. */
    private fun pollFrameInfo() {
        val now = SystemClock.uptimeMillis()
        if (now - fpsAt < FPS_INTERVAL_MS) return
        fpsAt = now
        val p = pipeline.stats
        val fps = fpsMeter.update(now, p.decoded.get())
        val w = p.frameWidth
        val h = p.frameHeight
        if (fps != null && w > 0 && h > 0) _frameInfo.value = FrameInfo(fps, w, h)
    }

    /** Main thread, every [DIAG_INTERVAL_MS] while the developer panel is on. */
    private fun pollDiagnostics() {
        if (!diagnosticsEnabled) return
        val now = SystemClock.uptimeMillis()
        if (now - diagAt < DIAG_INTERVAL_MS) return
        diagAt = now
        if (rx != 0L) {
            NativeCore.rxStats(rx, statsBuf)
            NativeCore.progress(rx, diagProgressBuf)
        }
        val p = pipeline.stats
        val snap = DiagSnapshot(
            atMs = now,
            cameraFrames = p.cameraFrames.get(),
            cameraMissed = p.cameraMissed.get(),
            queueDropped = p.queueDropped.get(),
            decoded = p.decoded.get(),
            scanNanos = p.scanNanos.get(),
            rxFrames = statsBuf[NativeCore.S_FRAMES],
            rxUnique = statsBuf[NativeCore.S_UNIQUE],
            rxDuplicates = statsBuf[NativeCore.S_DUPLICATES],
            rxRedundant = statsBuf[NativeCore.S_REDUNDANT],
            rs96K = statsBuf[NativeCore.S_RS96_K],
            rs96Rows = statsBuf[NativeCore.S_RS96_ROWS],
            rs96Useful = statsBuf[NativeCore.S_RS96_USEFUL],
            rs96Solved = statsBuf[NativeCore.S_RS96_SOLVED],
            threads = pipeline.threads,
            proto = diagProgressBuf[NativeCore.P_PROTO].toInt(),
            codesInFrame = diagProgressBuf[NativeCore.P_CODES_IN_FRAME].toInt(),
            obMode = statsBuf[NativeCore.S_OB_MODE],
            obK = statsBuf[NativeCore.S_OB_K],
            obRows = statsBuf[NativeCore.S_OB_ROWS],
            obGroups = statsBuf[NativeCore.S_OB_GROUPS],
            obSolved = statsBuf[NativeCore.S_OB_SOLVED],
        )
        _diagnostics.value = snap.format(diagPrev)
        diagPrev = snap
    }

    /** Main thread: on accepted symbols (at most every [PROGRESS_MIN_MS]) and every 100 ms. */
    private fun pollProgress() {
        if (!receiving.get() || rx == 0L) return
        NativeCore.progress(rx, progressBuf)
        var p = Progress.from(progressBuf)
        if (p.proto == NativeCore.PROTO_NONE) return
        if (p.sessionId != progressSession) {
            Log.i(TAG, "receiving session ${p.sessionId.toString(16)} (proto ${p.proto}, need ${p.need})")
            progressSession = p.sessionId
            progressHave = 0L
        }
        if (p.have < progressHave) p = p.copy(have = progressHave) else progressHave = p.have
        _state.value = ScanState.Receiving(p, store.current)
    }

    /** Main thread. */
    private fun publish(r: ReceivedResult) {
        _state.value = when (val s = _state.value) {
            is ScanState.Receiving -> if (receiving.get()) s.copy(last = r) else ScanState.Done(r)
            else -> ScanState.Done(r)
        }
        if (prefs.vibrate) feedback.vibrate()
        if (prefs.sound) feedback.beep()
        if (r.kind == ResultKind.TEXT && prefs.autoCopy) {
            if (ShareRules.fitsInline(r)) {
                if (feedback.copyText(r.text!!) && Build.VERSION.SDK_INT < 33) {
                    _events.trySend(UiEvent(R.string.msg_copied))
                }
            } else {
                _events.trySend(UiEvent(R.string.msg_too_large_to_copy))
            }
        }
        if (r.kind != ResultKind.TEXT && prefs.autoSave && Build.VERSION.SDK_INT >= 29) {
            viewModelScope.launch(Dispatchers.IO) {
                val n = store.saveToDownloads(r)
                if (n > 0) _events.trySend(UiEvent(R.string.msg_saved, n))
                else _events.trySend(UiEvent(R.string.msg_save_failed))
            }
        }
    }

    // ---- save (Storage Access Framework) and reset

    /**
     * What the Save button asked the system file picker for; kept here, not in the Activity,
     * so the answer still finds it after a rotation.
     */
    var pendingSave: SavePlan? = null

    /** The picker returned [uri]: write the pending result there; the outcome arrives as an event. */
    fun save(uri: Uri) {
        val plan = pendingSave ?: return
        pendingSave = null
        val app = getApplication<Application>()
        viewModelScope.launch(Dispatchers.IO) {
            val event = when (plan) {
                is SavePlan.One ->
                    UiEvent(if (SaveHelper.write(app, uri, plan)) R.string.msg_save_done else R.string.msg_save_error)
                is SavePlan.Many -> {
                    val n = SaveHelper.writeAll(app, uri, plan.files)
                    when {
                        n == plan.files.size -> UiEvent(R.string.msg_save_done_many, n)
                        n > 0 -> UiEvent(R.string.msg_save_partial, n)
                        else -> UiEvent(R.string.msg_save_error)
                    }
                }
                SavePlan.None -> return@launch
            }
            _events.trySend(event)
        }
    }

    /**
     * The reset button of the preview card: back to the initial state. Forgets the result with
     * [id] (and deletes its files), drops any transfer in progress together with its progress
     * bar, and clears the native receiver's memory of finished transfers, so scanning the same
     * file again is received anew from 0 %.
     */
    fun clearResult(id: Long) {
        if (_state.value.result?.id != id) return
        thumbCache = null
        if (rx != 0L) NativeCore.rxReset(rx)
        receiving.set(false)
        // The same file comes back with the same session id; without this the never-backwards
        // guard of pollProgress would show the old progress again.
        progressSession = -1L
        progressHave = 0L
        _state.value = when (val s = _state.value) {
            is ScanState.Error -> s.copy(last = null)
            else -> ScanState.Idle
        }
        viewModelScope.launch(Dispatchers.IO) { store.clear(id) }
    }

    // ---- preview helpers

    /** Downsampled image thumbnail (cached for the current result, survives rotation). */
    suspend fun thumbnail(r: ReceivedResult, maxDim: Int = 1280): Bitmap? {
        thumbCache?.let { (id, bmp) -> if (id == r.id) return bmp }
        val f = r.files.firstOrNull()?.file ?: return null
        val bmp = withContext(Dispatchers.IO) {
            runCatching {
                val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeFile(f.path, bounds)
                if (bounds.outWidth <= 0) return@runCatching null
                BitmapFactory.decodeFile(f.path, BitmapFactory.Options().apply {
                    inSampleSize = Tuning.inSampleSize(bounds.outWidth, bounds.outHeight, maxDim)
                })
            }.getOrNull()
        }
        if (bmp != null) thumbCache = r.id to bmp
        return bmp
    }

    // ---- debug-only demo results (UI verification without a sender)

    fun injectDemo(kind: String) {
        if (!BuildConfig.DEBUG) return
        viewModelScope.launch {
            when (kind) {
                "progress" -> _state.value = ScanState.Receiving(
                    Progress(NativeCore.PROTO_RQ, NativeCore.KIND_IMAGE, 620, 1000, 292_864, 46_080, 2, 0x1234),
                    store.current,
                )
                "plain" -> publish(store.acceptPlain("https://example.com/codetrans 你好,CodeTrans!"))
                else -> withContext(Dispatchers.IO) { Demo.make(getApplication(), store, kind) }
                    ?.let { publish(it) }
            }
        }
    }

    companion object {
        private const val TAG = "CodeTrans"
        private const val PROGRESS_INTERVAL_MS = 100L
        /** Fastest progress refresh, ~30 Hz: about one display frame of a 30 fps sender. */
        private const val PROGRESS_MIN_MS = 33L
        private const val DIAG_INTERVAL_MS = 500L
        /** The fps overlay is refreshed this often (each figure covers the last second). */
        private const val FPS_INTERVAL_MS = 500L
        private const val UNSUPPORTED_INTERVAL_MS = 5_000L
        /** Codes in view this long without one decoding: tell the user how to fix it. */
        private const val STUCK_MS = 1_500L

        /** First version caps the in-memory transfer by device RAM (≤ 256 MB). */
        fun maxBytes(app: Application): Long {
            val cap = 256L shl 20
            val mi = ActivityManager.MemoryInfo()
            app.getSystemService<ActivityManager>()?.getMemoryInfo(mi)
            return if (mi.totalMem <= 0) cap else (mi.totalMem / 8).coerceIn(32L shl 20, cap)
        }
    }
}

/** Debug-only sample results written under cache/rx/demo-<n>/. */
private object Demo {
    private var n = 0

    fun make(app: Application, store: ResultStore, kind: String): ReceivedResult? {
        val dir = File(store.rxRoot, "demo-${++n}").apply { mkdirs() }
        return when (kind) {
            "text" -> {
                val text = buildString {
                    append("CodeTrans 演示文本 / demo text\n\n")
                    repeat(40) { append("第 ${it + 1} 行:The quick brown fox jumps over the lazy dog.\n") }
                }
                val f = File(dir, "text.txt").apply { writeText(text) }
                store.acceptDemo(ResultKind.TEXT, listOf(ReceivedFile(f, "text.txt", f.length(), "text/plain")), text, dir)
            }
            "image" -> {
                val bmp = Bitmap.createBitmap(1600, 900, Bitmap.Config.ARGB_8888)
                val c = android.graphics.Canvas(bmp)
                c.drawColor(0xFF00695C.toInt())
                val paint = android.graphics.Paint().apply { color = 0xFFFFCA28.toInt(); textSize = 160f }
                c.drawText("CodeTrans", 360f, 500f, paint)
                val f = File(dir, "demo.png")
                f.outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
                store.acceptDemo(ResultKind.IMAGE, listOf(ReceivedFile(f, "demo.png", f.length(), "image/png")), null, dir)
            }
            else -> {
                val files = listOf("报告.pdf" to 183_000, "notes.txt" to 2_400, "photo.jpg" to 1_250_000).map { (name, size) ->
                    val f = File(dir, name)
                    f.outputStream().use { it.write(ByteArray(size)) }
                    ReceivedFile(f, name, f.length(), ResultStore.mimeFor(name))
                }
                store.acceptDemo(ResultKind.FILES, files, null, dir)
            }
        }
    }
}
