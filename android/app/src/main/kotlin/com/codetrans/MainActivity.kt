package com.codetrans

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.util.Log
import android.provider.Settings
import android.text.util.Linkify
import android.view.ViewGroup
import android.view.WindowManager
import androidx.activity.SystemBarStyle
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.annotation.RequiresApi
import androidx.annotation.StringRes
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.content.getSystemService
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isVisible
import androidx.core.view.updateLayoutParams
import androidx.core.view.updatePadding
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.codetrans.databinding.ActivityMainBinding
import com.codetrans.databinding.ItemFileBinding
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch

/**
 * The single main screen (PLAN §8): camera viewfinder with progress overlay, preview card for
 * the current result and a share FAB. Settings live behind the toolbar action.
 */
class MainActivity : AppCompatActivity() {
    private lateinit var b: ActivityMainBinding
    private lateinit var camera: CameraController
    private lateinit var prefs: Prefs
    private val vm: ScanViewModel by viewModels()

    private var permissionAsked = false
    private var permissionPermanentlyDenied = false
    private var scanning = false

    /** Id of the result currently rendered in the card (-1 = hint), to avoid re-rendering. */
    private var renderedId = Long.MIN_VALUE
    @StringRes private var renderedHint = 0
    private var shownResult: ReceivedResult? = null

    /** PowerManager.OnThermalStatusChangedListener (API 29+), kept untyped for API 23 class loading. */
    private var thermalListener: Any? = null

    private val requestCamera =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) {
                permissionPermanentlyDenied = false
                if (lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) startScanning()
            } else {
                permissionPermanentlyDenied = !shouldShowRequestPermissionRationale(Manifest.permission.CAMERA)
            }
            updatePermissionView()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT))
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= 33) CodeTransApp.forceAppLocale()
        b = ActivityMainBinding.inflate(layoutInflater)
        setContentView(b.root)
        prefs = Prefs(this)
        savedInstanceState?.let {
            permissionAsked = it.getBoolean(STATE_ASKED)
            permissionPermanentlyDenied = it.getBoolean(STATE_DENIED)
        }

        applyInsets()
        b.toolbar.setOnMenuItemClickListener { item ->
            if (item.itemId == R.id.action_settings) {
                startActivity(Intent(this, SettingsActivity::class.java))
                true
            } else {
                false
            }
        }
        b.share.setOnClickListener {
            if (!ShareHelper.share(this, shownResult)) snackbar(UiEvent(R.string.msg_share_failed))
        }
        b.camera.permissionButton.setOnClickListener { onPermissionButton() }

        camera = CameraController(
            context = this,
            owner = this,
            previewView = b.camera.previewView,
            onTapFocus = ::showFocusRing,
            onError = { snackbar(UiEvent(R.string.error_camera)) },
        )

        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch { vm.state.collect(::render) }
                launch { combine(vm.state, vm.detection, ::Pair).collect { (s, d) -> renderStatus(s, d) } }
                launch { vm.codes.collect { b.camera.codeOverlay.setCodes(it) } }
                launch { vm.events.collect(::snackbar) }
            }
        }

        if (savedInstanceState == null) handleDemoIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleDemoIntent(intent)
    }

    override fun onStart() {
        super.onStart()
        if (hasCameraPermission()) {
            startScanning()
        } else if (!permissionAsked) {
            permissionAsked = true
            requestCamera.launch(Manifest.permission.CAMERA)
        }
        updatePermissionView()
        if (Build.VERSION.SDK_INT >= 29) addThermalListener()
    }

    override fun onStop() {
        if (Build.VERSION.SDK_INT >= 29) removeThermalListener()
        stopScanning()
        super.onStop()
    }

    override fun onDestroy() {
        camera.release()
        super.onDestroy()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putBoolean(STATE_ASKED, permissionAsked)
        outState.putBoolean(STATE_DENIED, permissionPermanentlyDenied)
    }

    // ---- camera + decoder

    private fun hasCameraPermission() =
        ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    /** Idempotent: (re)applies settings changed while the settings page was open. */
    private fun startScanning() {
        Log.i("CodeTrans", "start scanning")
        camera.start(vm.cameraConfig(), prefs.exposureEv, vm.analyzer)
        vm.startScanning()
        scanning = true
        if (prefs.keepScreenOn) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    private fun stopScanning() {
        if (!scanning) return
        Log.i("CodeTrans", "stop scanning")
        scanning = false
        camera.stop()
        vm.stopScanning()
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    // ---- thermal throttling (PLAN §13)

    @RequiresApi(29)
    private fun addThermalListener() {
        val pm = getSystemService<PowerManager>() ?: return
        val l = PowerManager.OnThermalStatusChangedListener { status -> onThermalStatus(status) }
        // Called once right away with the current status.
        pm.addThermalStatusListener(ContextCompat.getMainExecutor(this), l)
        thermalListener = l
    }

    @RequiresApi(29)
    private fun removeThermalListener() {
        val l = thermalListener as? PowerManager.OnThermalStatusChangedListener ?: return
        getSystemService<PowerManager>()?.removeThermalStatusListener(l)
        thermalListener = null
    }

    @RequiresApi(29)
    private fun onThermalStatus(status: Int) {
        val severe = status >= PowerManager.THERMAL_STATUS_SEVERE
        if (!vm.setThermalSevere(severe)) return
        Log.w("CodeTrans", "thermal status $status: ${if (severe) "throttling to 720p / 1 thread" else "back to normal"}")
        if (severe) snackbar(UiEvent(R.string.msg_thermal, long = true))
        if (scanning) {
            camera.start(vm.cameraConfig(), prefs.exposureEv, vm.analyzer)
            vm.restartScanning()
        }
    }

    // ---- permission UI

    private fun onPermissionButton() {
        if (permissionPermanentlyDenied) {
            runCatching {
                startActivity(
                    Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", packageName, null))
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                )
            }
        } else {
            permissionAsked = true
            requestCamera.launch(Manifest.permission.CAMERA)
        }
    }

    private fun updatePermissionView() {
        val granted = hasCameraPermission()
        val c = b.camera
        c.permissionView.isVisible = !granted
        c.previewView.isVisible = granted
        if (!granted) {
            c.permissionText.setText(
                if (permissionPermanentlyDenied) R.string.permission_denied_permanently else R.string.permission_rationale,
            )
            c.permissionButton.setText(
                if (permissionPermanentlyDenied) R.string.permission_open_settings else R.string.permission_grant,
            )
        }
    }

    // ---- rendering

    /**
     * Bottom of the camera area: the transfer progress while receiving; before that, as soon as
     * codes are in view, what the scanner sees (and how to fix it when nothing can be read).
     */
    private fun renderStatus(state: ScanState, d: Detection) {
        val o = b.camera
        when {
            state is ScanState.Receiving -> {
                o.statusOverlay.isVisible = true
                o.progress.setProgressCompat(state.progress.permille, true)
                val status = Formatters.status(state.progress, statusLabels)
                o.statusText.text = if (d.stuck) status + "\n" + getString(R.string.detect_stuck_short) else status
            }
            d.count > 0 -> {
                o.statusOverlay.isVisible = true
                o.progress.setProgressCompat(0, false)
                o.statusText.text = when {
                    d.decoded > 0 -> getString(R.string.detect_reading, d.decoded, d.count)
                    d.stuck -> getString(R.string.detect_stuck, d.count)
                    else -> getString(R.string.detect_found, d.count)
                }
            }
            else -> o.statusOverlay.isVisible = false
        }
    }

    private fun render(state: ScanState) {
        val hint = if (state is ScanState.Error) state.message else R.string.hint_idle
        renderPreview(state.result, hint)
        shownResult = state.result
        b.share.isEnabled = state.result != null
    }

    private val statusLabels by lazy {
        StatusLabels(
            protoPlain = getString(R.string.proto_plain),
            kindText = getString(R.string.kind_text),
            kindImage = getString(R.string.kind_image),
            kindFiles = getString(R.string.kind_files_short),
            codesFormat = getString(R.string.codes_format),
        )
    }

    private fun renderPreview(r: ReceivedResult?, @StringRes hint: Int) {
        val id = r?.id ?: -1L
        if (id == renderedId && (r != null || hint == renderedHint)) return
        renderedId = id
        renderedHint = hint
        val p = b.preview
        p.hintView.isVisible = r == null
        p.textScroll.isVisible = r?.kind == ResultKind.TEXT
        p.imageContainer.isVisible = r?.kind == ResultKind.IMAGE
        p.filesScroll.isVisible = r?.kind == ResultKind.FILES
        p.image.setImageDrawable(null)
        p.filesList.removeAllViews()

        if (r == null) {
            p.cardTitle.setText(R.string.preview_title)
            p.hintText.setText(hint)
            return
        }
        p.cardTitle.text = cardTitle(r)
        when (r.kind) {
            ResultKind.TEXT -> renderText(r)
            ResultKind.IMAGE -> renderImage(r)
            ResultKind.FILES -> renderFiles(r)
        }
    }

    private fun cardTitle(r: ReceivedResult): String = when (r.kind) {
        ResultKind.TEXT ->
            if (r.plain) getString(R.string.kind_plain)
            else getString(R.string.kind_text) + SEP + Formatters.bytes(r.textBytes)
        ResultKind.IMAGE -> getString(R.string.kind_image) + SEP + Formatters.bytes(r.files.sumOf { it.size })
        ResultKind.FILES ->
            getString(R.string.kind_files, r.files.size) + SEP + Formatters.bytes(r.files.sumOf { it.size })
    }

    private fun renderText(r: ReceivedResult) {
        val full = r.text.orEmpty()
        var shown = full
        if (shown.length > MAX_PREVIEW_CHARS) {
            shown = shown.substring(0, MAX_PREVIEW_CHARS)
            if (shown.last().isHighSurrogate()) shown = shown.dropLast(1)
        }
        val truncated = r.textTruncated || shown.length < full.length
        val tv = b.preview.textView
        // Selectable TextView still opens links on tap when autoLinkMask is set (PLAN §8: plain QR URLs).
        tv.autoLinkMask = if (r.plain) Linkify.WEB_URLS else 0
        tv.text = if (truncated) shown + "\n\n" + getString(R.string.preview_truncated) else shown
        b.preview.textScroll.scrollTo(0, 0)
    }

    private fun renderImage(r: ReceivedResult) {
        val p = b.preview
        val f = r.files.firstOrNull()
        p.imageCaption.text = f?.let { it.name + SEP + Formatters.bytes(it.size) }.orEmpty()
        lifecycleScope.launch {
            val bmp = vm.thumbnail(r)
            if (renderedId != r.id) return@launch
            if (bmp != null) p.image.setImageBitmap(bmp)
            else p.imageCaption.text = getString(R.string.image_unavailable)
        }
    }

    private fun renderFiles(r: ReceivedResult) {
        val list = b.preview.filesList
        r.files.take(MAX_FILES_LISTED).forEach { f ->
            val row = ItemFileBinding.inflate(layoutInflater, list, true)
            row.fileName.text = f.name
            row.fileSize.text = Formatters.bytes(f.size)
        }
        val more = r.files.size - MAX_FILES_LISTED
        if (more > 0) {
            val row = ItemFileBinding.inflate(layoutInflater, list, true)
            row.fileName.text = getString(R.string.files_more, more)
            row.fileSize.isVisible = false
        }
        b.preview.filesScroll.scrollTo(0, 0)
    }

    private fun showFocusRing(x: Float, y: Float) {
        val ring = b.camera.focusRing
        ring.animate().cancel()
        ring.x = x - ring.width / 2f
        ring.y = y - ring.height / 2f
        ring.scaleX = 1.3f
        ring.scaleY = 1.3f
        ring.alpha = 1f
        ring.animate().scaleX(1f).scaleY(1f).setDuration(200).withEndAction {
            ring.animate().alpha(0f).setStartDelay(600).setDuration(300).start()
        }.start()
    }

    private fun snackbar(e: UiEvent) {
        val text = if (e.arg != null) getString(e.text, e.arg) else getString(e.text)
        Snackbar.make(b.root, text, if (e.long) Snackbar.LENGTH_LONG else Snackbar.LENGTH_SHORT)
            .setAnchorView(b.share)
            .show()
    }

    // ---- edge-to-edge (targetSdk 35+ enforces it)

    private fun applyInsets() {
        val fabMargin = resources.getDimensionPixelSize(R.dimen.fab_margin)
        ViewCompat.setOnApplyWindowInsetsListener(b.root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            b.toolbar.updatePadding(left = bars.left, top = bars.top, right = bars.right)
            b.content.updatePadding(left = bars.left, right = bars.right, bottom = bars.bottom)
            b.share.updateLayoutParams<ViewGroup.MarginLayoutParams> {
                bottomMargin = fabMargin + bars.bottom
                rightMargin = fabMargin + bars.right
                marginEnd = fabMargin + bars.right
            }
            insets
        }
    }

    // ---- debug: `adb shell am start -n com.codetrans/.MainActivity -e demo text|image|files|plain|progress`

    private fun handleDemoIntent(intent: Intent?) {
        if (!BuildConfig.DEBUG) return
        intent?.getStringExtra(EXTRA_DEMO)?.let { vm.injectDemo(it) }
    }

    companion object {
        private const val STATE_ASKED = "permission_asked"
        private const val STATE_DENIED = "permission_denied"
        private const val EXTRA_DEMO = "demo"
        private const val SEP = " · "
        /** PLAN §8: only the first 20 000 characters are rendered; Share sends everything. */
        private const val MAX_PREVIEW_CHARS = 20_000
        private const val MAX_FILES_LISTED = 200
    }
}
