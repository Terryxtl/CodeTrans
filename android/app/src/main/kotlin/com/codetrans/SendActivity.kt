package com.codetrans

import android.Manifest
import android.content.ClipboardManager
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.provider.OpenableColumns
import android.util.Log
import android.view.Choreographer
import android.view.View
import android.view.WindowManager
import android.widget.ArrayAdapter
import androidx.activity.OnBackPressedCallback
import androidx.activity.SystemBarStyle
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.TooltipCompat
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import androidx.core.content.IntentCompat
import androidx.core.content.edit
import androidx.core.content.getSystemService
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isVisible
import androidx.core.view.updatePadding
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.Lifecycle
import com.codetrans.databinding.ActivitySendBinding
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.coroutines.resume
import kotlin.math.abs
import kotlin.math.roundToInt

/** Pure helpers of the send screen (JVM-testable). */
object SendMath {
    /** QR version per density choice: low / mid (default) / high. */
    val VERSIONS = intArrayOf(15, 27, 40)
    /** Largest content the phone offers to send (the slow direction: a webcam reads it). */
    const val MAX_BYTES = 16L shl 20

    /** Key stops of the frame-rate slider (frames per second, ascending); the slider moves by index. */
    val FPS_NODES = intArrayOf(6, 10, 15, 20, 24, 30, 45, 60)
    const val DEFAULT_FPS = 10

    /** Index of the key stop closest to a stored [fps], so any old or odd value still lands on a stop. */
    fun fpsIndex(fps: Int): Int = FPS_NODES.indices.minBy { abs(FPS_NODES[it] - fps) }

    /** The frame rate of slider position [index] (clamped to the stops). */
    fun fpsAt(index: Int): Int = FPS_NODES[index.coerceIn(0, FPS_NODES.lastIndex)]

    /** A frame rate for the status line: whole numbers without a decimal point, otherwise one digit. */
    fun fpsText(fps: Double): String =
        if (abs(fps - fps.roundToInt()) < 0.05) fps.roundToInt().toString()
        else String.format(Locale.ROOT, "%.1f", fps)

    fun eta(seconds: Double): String = when {
        seconds < 60 -> String.format(Locale.ROOT, "%.0f 秒", maxOf(1.0, seconds))
        seconds < 3600 -> String.format(Locale.ROOT, "%d 分 %02d 秒", seconds.toInt() / 60, seconds.toInt() % 60)
        else -> String.format(Locale.ROOT, "%d 时 %02d 分", seconds.toInt() / 3600, seconds.toInt() / 60 % 60)
    }
}

/**
 * Phone -> PC (the reverse of [MainActivity]): text, an image or files become a stream of
 * one or two QR codes or optical boards, read by the CodeTrans PC app's camera receiver
 * (tray menu → "摄像头接收"). Also the target of "share to CodeTrans".
 */
class SendActivity : AppCompatActivity() {
    private lateinit var b: ActivitySendBinding
    private lateinit var prefs: Prefs

    private enum class Mode(val label: Int, val proto: Int, val profile: Int) {
        RS96(R.string.send_rs96, NativeCore.PROTO_D3, 0),
        BANDS(R.string.send_bands, NativeCore.PROTO_OB, NativeCore.OB_BANDS),
        TILES(R.string.send_tiles, NativeCore.PROTO_OB, NativeCore.OB_TILES);
    }

    /** The protocol picked in the dropdown. */
    private var mode = Mode.RS96

    private sealed interface Picked {
        val label: String
        data class Image(val name: String, val mime: String, val bytes: ByteArray) : Picked {
            override val label get() = "$name · ${Formatters.bytes(bytes.size.toLong())}"
        }
        data class Files(val files: List<Pair<String, ByteArray>>) : Picked {
            override val label get() = if (files.size == 1) {
                "${files[0].first} · ${Formatters.bytes(files[0].second.size.toLong())}"
            } else {
                "${files.size} 个文件 · ${Formatters.bytes(files.sumOf { it.second.size.toLong() })}"
            }
        }
    }

    private var picked: Picked? = null
    private var playJob: Job? = null
    private var preparing = false

    private val pickImage = registerForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri != null) load(listOf(uri), image = true)
    }
    private val pickFiles = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) load(uris, image = false)
    }

    /** The file the system camera app is writing its photo to (see [launchCamera]). */
    private var captureFile: File? = null
    private val requestCamera = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) launchCamera() else snackbar(getString(R.string.permission_denied_permanently))
    }
    private val takePicture = registerForActivityResult(ActivityResultContracts.TakePicture()) { taken ->
        val file = captureFile
        captureFile = null
        if (taken && file != null && file.length() > 0) {
            load(listOf(FileProvider.getUriForFile(this, ShareHelper.authority(this), file)), image = true, own = true)
        } else {
            clearCaptures()
        }
    }

    private val backWhilePlaying = object : OnBackPressedCallback(false) {
        override fun handleOnBackPressed() = stopPlaying()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT))
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= 33) CodeTransApp.forceAppLocale()
        b = ActivitySendBinding.inflate(layoutInflater)
        setContentView(b.root)
        prefs = Prefs(this)
        onBackPressedDispatcher.addCallback(this, backWhilePlaying)

        b.toolbar.setNavigationOnClickListener { finish() }
        ViewCompat.setOnApplyWindowInsetsListener(b.root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            b.toolbar.updatePadding(left = bars.left, top = bars.top, right = bars.right)
            b.composeScroll.updatePadding(left = bars.left, right = bars.right, bottom = bars.bottom)
            b.player.updatePadding(left = bars.left, top = bars.top, right = bars.right)
            val pad = (12 * resources.displayMetrics.density).toInt()
            b.playBar.updatePadding(left = bars.left + pad, right = bars.right + pad, bottom = bars.bottom + pad / 3)
            insets
        }

        savedInstanceState?.getString(STATE_CAPTURE)?.let { captureFile = File(captureDir(), it) }

        b.pickCamera.setOnClickListener { takePhoto() }
        b.pickImage.setOnClickListener { pickImage.launch("image/*") }
        b.pickFiles.setOnClickListener { pickFiles.launch(arrayOf("*/*")) }
        b.pasteText.setOnClickListener { paste() }
        b.clearPicked.setOnClickListener { setPicked(null) }
        b.start.setOnClickListener { startPlaying() }
        b.stop.setOnClickListener { stopPlaying() }
        // The icon-only buttons say what they do on long-press (and to screen readers, see the layout).
        tooltip(b.pickCamera, R.string.send_take_photo)
        tooltip(b.pickImage, R.string.send_pick_image)
        tooltip(b.pickFiles, R.string.send_pick_files)
        tooltip(b.pasteText, R.string.send_paste)

        val sp = prefs.sp
        // The dropdown never filters (inputType none, items set once); its text is set with setText(.., false).
        b.protocolDropdown.setAdapter(ArrayAdapter(this, R.layout.item_dropdown, Mode.entries.map { getString(it.label) }))
        b.protocolDropdown.setOnItemClickListener { _, _, position, _ -> selectMode(Mode.entries[position]) }
        val savedMode = sp.getString(KEY_MODE, null)
        selectMode(Mode.entries.firstOrNull { it.name == savedMode } ?: Mode.RS96, persist = false)
        b.countGroup.check(if (sp.getInt(KEY_COUNT, 1) == 2) b.count2.id else b.count1.id)
        b.densityGroup.check(
            when (sp.getInt(KEY_DENSITY, 1)) { 0 -> b.densityLow.id; 2 -> b.densityHigh.id; else -> b.densityMid.id },
        )
        // The slider moves over the indices of SendMath.FPS_NODES, so it snaps to the key stops. The
        // stored value is still the plain fps (as with the old buttons) and any value maps to a stop.
        b.fpsSlider.valueTo = SendMath.FPS_NODES.lastIndex.toFloat()
        b.fpsSlider.setLabelFormatter { SendMath.fpsAt(it.roundToInt()).toString() }
        b.fpsSlider.addOnChangeListener { _, _, _ -> showFps() }
        b.fpsSlider.value = SendMath.fpsIndex(runCatching { sp.getInt(KEY_FPS, SendMath.DEFAULT_FPS) }
            .getOrDefault(SendMath.DEFAULT_FPS)).toFloat()
        showFps()

        if (savedInstanceState == null) handleShareIntent(intent)
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        // The system may drop this activity while the camera app is in front.
        captureFile?.let { outState.putString(STATE_CAPTURE, it.name) }
    }

    private fun tooltip(view: View, text: Int) = TooltipCompat.setTooltipText(view, getString(text))

    private fun selectMode(selected: Mode, persist: Boolean = true) {
        mode = selected
        b.protocolDropdown.setText(getString(selected.label), false)
        // Only RS96 has a code density; the optical boards have a fixed size.
        b.densityLabel.isVisible = selected == Mode.RS96
        b.densityGroup.isVisible = selected == Mode.RS96
        if (persist) prefs.sp.edit { putString(KEY_MODE, selected.name) }
    }

    private fun showFps() {
        b.fpsValue.text = getString(R.string.send_fps_value, SendMath.fpsAt(b.fpsSlider.value.roundToInt()))
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        if (playJob == null) handleShareIntent(intent)
    }

    override fun onStop() {
        // A stream nobody looks at only drains the battery; the transfer restarts cheaply.
        stopPlaying()
        super.onStop()
    }

    override fun onDestroy() {
        stopPlaying()
        super.onDestroy()
    }

    // ---- input

    private fun handleShareIntent(intent: Intent?) {
        intent ?: return
        when (intent.action) {
            Intent.ACTION_SEND -> {
                val uri = IntentCompat.getParcelableExtra(intent, Intent.EXTRA_STREAM, Uri::class.java)
                if (uri != null) {
                    load(listOf(uri), image = intent.type?.startsWith("image/") == true)
                } else {
                    intent.getCharSequenceExtra(Intent.EXTRA_TEXT)?.let { b.textInput.setText(it) }
                }
            }
            Intent.ACTION_SEND_MULTIPLE -> {
                val uris = IntentCompat.getParcelableArrayListExtra(intent, Intent.EXTRA_STREAM, Uri::class.java)
                if (!uris.isNullOrEmpty()) load(uris, image = false)
            }
        }
    }

    private fun paste() {
        val clip = getSystemService<ClipboardManager>()?.primaryClip
        val text = clip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(this)
        if (text.isNullOrEmpty()) snackbar(getString(R.string.send_clipboard_empty))
        else b.textInput.setText(text)
    }

    /**
     * Only documents of other apps: this activity is exported (share target), so a URI of our own
     * FileProvider (received files) or a file:// path must not make us put that data on screen.
     */
    private fun foreign(uri: Uri) =
        uri.scheme == "content" && uri.authority != null && uri.authority != "$packageName.fileprovider"

    // ---- system camera

    private fun captureDir() = File(cacheDir, "tx")

    /** Deletes the photo(s) of the system camera; they only live until they are read. */
    private fun clearCaptures() {
        captureDir().listFiles()?.forEach { it.delete() }
    }

    /**
     * Takes a photo with the system camera app (no camera code of our own). Launching it without
     * the CAMERA permission we declare throws a SecurityException, so ask for it first.
     */
    private fun takePhoto() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            launchCamera()
        } else {
            requestCamera.launch(Manifest.permission.CAMERA)
        }
    }

    private fun launchCamera() {
        clearCaptures()
        val name = "IMG_${SimpleDateFormat("yyyyMMdd_HHmmss", Locale.ROOT).format(Date())}.jpg"
        val file = File(captureDir(), name)
        try {
            // The file must exist and be ours before the camera app gets a write grant for it.
            file.parentFile?.mkdirs()
            file.createNewFile()
            captureFile = file
            takePicture.launch(FileProvider.getUriForFile(this, ShareHelper.authority(this), file))
        } catch (e: Exception) {
            // IOException (no cache space), ActivityNotFoundException (no camera app), SecurityException.
            Log.w(TAG, "cannot start the camera app", e)
            captureFile = null
            file.delete()
            snackbar(getString(if (e is IOException) R.string.send_read_failed else R.string.error_camera))
        }
    }

    /**
     * Reads the picked documents into memory (IO thread), capped at [SendMath.MAX_BYTES] in total.
     * [own]: the document is our own camera capture (our FileProvider URIs are otherwise refused, see [foreign]).
     */
    private fun load(all: List<Uri>, image: Boolean, own: Boolean = false) {
        val uris = if (own) all else all.filter(::foreign)
        if (uris.isEmpty()) {
            snackbar(getString(R.string.send_read_failed))
            return
        }
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    var total = 0L
                    val items = uris.map { uri ->
                        val name = displayName(uri)
                        val bytes = contentResolver.openInputStream(uri)?.use { input ->
                            val out = java.io.ByteArrayOutputStream()
                            val buf = ByteArray(64 * 1024)
                            while (true) {
                                val n = input.read(buf)
                                if (n < 0) break
                                total += n
                                if (total > SendMath.MAX_BYTES) throw TooLarge()
                                out.write(buf, 0, n)
                            }
                            out.toByteArray()
                        } ?: throw IOException("cannot open $uri")
                        Triple(name, contentResolver.getType(uri), bytes)
                    }
                    val names = NativeCore.safeNames(items.map { it.first })
                    if (image && items.size == 1 && items[0].second?.startsWith("image/") == true) {
                        Picked.Image(names[0], items[0].second!!, items[0].third)
                    } else {
                        Picked.Files(names.zip(items.map { it.third }))
                    }
                }.also { if (own) clearCaptures() } // the bytes are in memory now, drop the photo file
            }
            result.onSuccess { setPicked(it) }.onFailure {
                Log.w(TAG, "cannot read picked content", it)
                snackbar(
                    if (it is TooLarge) getString(R.string.send_too_large, Formatters.bytes(SendMath.MAX_BYTES))
                    else getString(R.string.send_read_failed),
                )
            }
        }
    }

    private class TooLarge : IOException()

    private fun displayName(uri: Uri): String {
        runCatching {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                if (c.moveToFirst() && !c.isNull(0)) return c.getString(0)
            }
        }
        return uri.lastPathSegment?.substringAfterLast('/') ?: "file"
    }

    private fun setPicked(p: Picked?) {
        picked = p
        b.pickedRow.isVisible = p != null
        b.pickedText.text = p?.label
        b.textLayout.isEnabled = p == null
        // Pasting into a disabled text box would do nothing visible.
        b.pasteText.isEnabled = p == null
        b.pasteText.alpha = if (p == null) 1f else 0.38f
    }

    // ---- playback

    private fun options(): Triple<Int, Int, Int> {
        val count = if (b.countGroup.checkedButtonId == b.count2.id) 2 else 1
        val density = when (b.densityGroup.checkedButtonId) { b.densityLow.id -> 0; b.densityHigh.id -> 2; else -> 1 }
        val fps = SendMath.fpsAt(b.fpsSlider.value.roundToInt())
        prefs.sp.edit {
            putInt(KEY_COUNT, count)
            putInt(KEY_DENSITY, density)
            putInt(KEY_FPS, fps)
        }
        return Triple(count, SendMath.VERSIONS[density], fps)
    }

    private fun startPlaying() {
        if (preparing || playJob != null) return
        if (!NativeCore.isAvailable) {
            snackbar(getString(R.string.error_native))
            return
        }
        val p = picked
        val text = b.textInput.text?.toString().orEmpty()
        if (p == null && text.isEmpty()) {
            snackbar(getString(R.string.send_nothing))
            return
        }
        val (count, version, fps) = options()
        val selected = mode
        preparing = true
        b.start.isEnabled = false
        b.start.setText(R.string.send_preparing)
        lifecycleScope.launch {
            var handle = 0L
            try {
                withContext(Dispatchers.Default) {
                    handle = when (p) {
                        is Picked.Image -> NativeCore.txNew(NativeCore.KIND_IMAGE, null, arrayOf(p.bytes), p.mime,
                            selected.proto, selected.profile, version, ECC_L)
                        is Picked.Files -> NativeCore.txNew(NativeCore.KIND_FILES,
                            p.files.map { it.first.toByteArray(Charsets.UTF_8) }.toTypedArray(),
                            p.files.map { it.second }.toTypedArray(), null, selected.proto, selected.profile, version, ECC_L)
                        null -> NativeCore.txNew(NativeCore.KIND_TEXT, null, arrayOf(text.toByteArray(Charsets.UTF_8)),
                            null, selected.proto, selected.profile, version, ECC_L)
                    }
                }
                if (!lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED) || isFinishing) return@launch
                if (handle == 0L) {
                    snackbar(getString(R.string.send_failed))
                    return@launch
                }
                play(handle, count, fps, selected, version, kindLabel(p))
                handle = 0L // the playback job now owns it
            } finally {
                if (handle != 0L) NativeCore.txFree(handle)
                preparing = false
                b.start.isEnabled = true
                b.start.setText(R.string.send_start)
            }
        }
    }

    private fun kindLabel(p: Picked?) = when (p) {
        is Picked.Image -> getString(R.string.kind_image)
        is Picked.Files -> getString(R.string.kind_files_short)
        null -> getString(R.string.kind_text)
    }

    /** Refresh rate of the panel the player is on (the current mode: it can change at run time). */
    private fun refreshHz(): Double =
        (b.player.display?.refreshRate?.toDouble() ?: 0.0).takeIf { it >= 20.0 } ?: 60.0

    private fun play(handle: Long, count: Int, fps: Int, mode: Mode, version: Int, kind: String) {
        val info = LongArray(NativeCore.TX_INFO_SIZE).also { NativeCore.txInfo(handle, it) }
        val size = if (info[2] != 0L) "${Formatters.bytes(info[0])}→${Formatters.bytes(info[1])}"
        else Formatters.bytes(info[0])
        // A frame lasts a whole number of display refreshes, so e.g. 45 fps on a 60 Hz panel really
        // shows 60: the status line and the ETA use the rate that is actually on the screen.
        fun headText(hz: Double): String {
            val effective = NativeCore.paceEffectiveFps(fps, hz)
            val eta = SendMath.eta(NativeCore.txEtaSeconds(handle, count, effective))
            return "${getString(mode.label)} · $kind $size · ${count}码×${SendMath.fpsText(effective)}fps · ≈$eta"
        }
        var hz = refreshHz()
        var head = headText(hz)

        b.composePanel.isVisible = false
        b.playPanel.isVisible = true
        backWhilePlaying.isEnabled = true
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.attributes = window.attributes.apply { screenBrightness = 1f }
        b.playStatus.text = head

        val job = lifecycleScope.launch {
            val moduleSide = if (mode == Mode.RS96) 4 * version + 17 else NativeCore.OB_SIDE
            val buf = ByteArray(count * moduleSide * moduleSide)
            var lastShowNs = 0L
            var shown = 0L
            while (isActive) {
                // tx is only touched here, one frame after the other. The next frame is encoded
                // right after the previous one was shown and then waits for its slot.
                val side = withContext(Dispatchers.Default) {
                    NativeCore.txNextFrame(handle, count, fps, buf)
                }
                if (side == 0) {
                    snackbar(getString(R.string.send_failed))
                    stopPlaying()
                    break
                }
                // Flip on a display refresh, `vblanks` of them after the previous frame (see cp_pace_vblanks):
                // two frames can never share one refresh, however a ms timer drifts against the panel.
                val periodNs = 1e9 / hz
                val frameNs = periodNs * NativeCore.paceVblanks(fps, hz)
                if (lastShowNs != 0L) {
                    // Sleep through the bulk of a long frame instead of waking on every refresh.
                    val waitNs = lastShowNs + frameNs - System.nanoTime() - 1.5 * periodNs
                    if (waitNs > periodNs) delay((waitNs / 1e6).toLong())
                }
                var t = awaitRefresh()
                while (lastShowNs != 0L && t - lastShowNs < frameNs - periodNs * 0.5) t = awaitRefresh()
                b.player.show(side, count, buf)
                shown++
                // Stay phase-locked unless far behind (a stall, or encoding slower than the frame):
                // then restart the schedule from this refresh instead of racing to catch up.
                lastShowNs = if (lastShowNs != 0L && t - lastShowNs < frameNs * 2) lastShowNs + frameNs.toLong() else t
                if (shown % maxOf(1, fps) == 0L) {
                    val now = refreshHz()
                    if (now != hz) {
                        hz = now
                        head = headText(hz)
                    }
                    b.playStatus.text = "$head · 已播 ${shown} 帧"
                }
            }
        }
        // The native stream is freed after any in-flight frame finishes.
        job.invokeOnCompletion { NativeCore.txFree(handle) }
        playJob = job
    }

    private fun stopPlaying() {
        playJob?.cancel()
        playJob = null
        if (!b.playPanel.isVisible) return
        b.player.clear()
        b.playPanel.isVisible = false
        b.composePanel.isVisible = true
        backWhilePlaying.isEnabled = false
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.attributes = window.attributes.apply {
            screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
    }

    private fun snackbar(text: String) {
        Snackbar.make(b.root, text, Snackbar.LENGTH_LONG).show()
    }

    companion object {
        private const val TAG = "CodeTrans"
        private const val KEY_COUNT = "send_count"
        private const val KEY_MODE = "send_mode"
        private const val KEY_DENSITY = "send_density"
        private const val KEY_FPS = "send_fps"
        private const val STATE_CAPTURE = "capture_file"
        private const val ECC_L = 0
    }
}

/**
 * Suspends until the next display refresh begins and returns its time (System.nanoTime base).
 * Main thread only (a Choreographer is per looper).
 */
private suspend fun awaitRefresh() = suspendCancellableCoroutine<Long> { cont ->
    val choreographer = Choreographer.getInstance()
    val callback = Choreographer.FrameCallback { frameTimeNanos -> cont.resume(frameTimeNanos) }
    choreographer.postFrameCallback(callback)
    cont.invokeOnCancellation { choreographer.removeFrameCallback(callback) }
}
