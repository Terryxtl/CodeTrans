package com.codetrans

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import androidx.annotation.OptIn
import androidx.camera.camera2.interop.Camera2CameraInfo
import androidx.camera.camera2.interop.Camera2Interop
import androidx.camera.camera2.interop.ExperimentalCamera2Interop
import androidx.camera.core.AspectRatio
import androidx.camera.core.Camera
import androidx.camera.core.CameraInfo
import androidx.camera.core.CameraSelector
import androidx.camera.core.ExtendableBuilder
import androidx.camera.core.FocusMeteringAction
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.Preview
import androidx.camera.core.resolutionselector.AspectRatioStrategy
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.lifecycle.awaitInstance
import androidx.camera.view.PreviewView
import androidx.core.content.getSystemService
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlin.math.abs

/**
 * CameraX binding for one Activity instance: Preview + ImageAnalysis (YUV_420_888,
 * KEEP_ONLY_LATEST), AE target fps via Camera2 interop, tap-to-focus, pinch zoom, exposure.
 *
 * Use cases are bound to the Activity lifecycle, so CameraX closes the camera in ON_STOP and
 * reopens it in ON_START; [start] rebinds only when [Config] changed.
 */
class CameraController(
    private val context: Context,
    private val owner: LifecycleOwner,
    private val previewView: PreviewView,
    private val onTapFocus: (x: Float, y: Float) -> Unit,
    private val onError: (Throwable) -> Unit,
) {
    /** Settings that require rebinding. [fps] 0 = auto. */
    data class Config(val analysisSize: Size, val fps: Int)

    private var provider: ProcessCameraProvider? = null
    private var camera: Camera? = null
    private var analysis: ImageAnalysis? = null
    private var boundConfig: Config? = null
    private val analysisExecutor: ExecutorService =
        Executors.newSingleThreadExecutor { r -> Thread(r, "cp-analysis") }

    init {
        setupGestures()
    }

    /**
     * Bumped by every [start] and [stop]: a start whose provider lookup finishes after a later
     * stop (or start) must not attach its analyzer.
     *
     * Not a lifecycle check: [start] is called from onStart(), where the lifecycle is still
     * CREATED (ON_START is dispatched after onStart() returns), and once the provider is cached
     * the coroutine below runs synchronously on Main.immediate. A STARTED check therefore
     * skipped every start after the first one, e.g. when coming back from the settings page.
     */
    private var generation = 0

    fun start(config: Config, exposureEv: Float, analyzer: ImageAnalysis.Analyzer) {
        val gen = ++generation
        owner.lifecycleScope.launch {
            val p = try {
                provider ?: ProcessCameraProvider.awaitInstance(context).also { provider = it }
            } catch (e: Exception) {
                onError(e)
                return@launch
            }
            if (gen != generation || owner.lifecycle.currentState == Lifecycle.State.DESTROYED) return@launch
            try {
                if (camera == null || boundConfig != config) bind(p, config)
                analysis?.setAnalyzer(analysisExecutor, analyzer)
                applyExposure(exposureEv)
            } catch (e: Exception) {
                Log.e(TAG, "camera bind failed", e)
                onError(e)
            }
        }
    }

    /** Detaches the analyzer; the camera itself is closed by the lifecycle (ON_STOP). */
    fun stop() {
        generation++
        analysis?.clearAnalyzer()
    }

    fun release() {
        stop()
        analysisExecutor.shutdown()
    }

    private fun bind(p: ProcessCameraProvider, config: Config) {
        val (selector, facing) = when {
            p.hasCamera(CameraSelector.DEFAULT_BACK_CAMERA) ->
                CameraSelector.DEFAULT_BACK_CAMERA to CameraSelector.LENS_FACING_BACK
            p.hasCamera(CameraSelector.DEFAULT_FRONT_CAMERA) ->
                CameraSelector.DEFAULT_FRONT_CAMERA to CameraSelector.LENS_FACING_FRONT
            else -> throw IllegalStateException("no camera")
        }
        val info = p.availableCameraInfos.firstOrNull { it.lensFacing == facing }
        val caps = info?.let { CameraCaps.forCameraInfo(context, it) }
        val ranges = caps?.fpsRanges ?: info?.supportedFrameRateRanges.orEmpty().map { it.lower to it.upper }
        val target = config.analysisSize
        val wantFps = Tuning.targetFps(config.fps, caps?.maxFpsFor(target.width, target.height))
        val fps = Tuning.chooseFpsRange(ranges, wantFps)

        // Prefer exactly the size picked in the settings (it comes from this camera's own list);
        // the aspect-ratio group and the closest-size rule only matter for the fallback.
        val ratio = if (abs(target.width.toFloat() / target.height - 4f / 3f) <
            abs(target.width.toFloat() / target.height - 16f / 9f)
        ) AspectRatio.RATIO_4_3 else AspectRatio.RATIO_16_9
        val ratioStrategy = AspectRatioStrategy(ratio, AspectRatioStrategy.FALLBACK_RULE_AUTO)
        val analysisSelector = ResolutionSelector.Builder()
            .setAspectRatioStrategy(ratioStrategy)
            .setResolutionStrategy(
                ResolutionStrategy(target, ResolutionStrategy.FALLBACK_RULE_CLOSEST_HIGHER_THEN_LOWER),
            )
            .setResolutionFilter { sizes, _ -> sizes.sortedBy { if (it == target) 0 else 1 } }
            .build()
        // Same aspect ratio for both streams so the ISP does not scale twice.
        val previewSelector = ResolutionSelector.Builder()
            .setAspectRatioStrategy(ratioStrategy)
            .build()

        val previewBuilder = Preview.Builder().setResolutionSelector(previewSelector)
        val analysisBuilder = ImageAnalysis.Builder()
            .setResolutionSelector(analysisSelector)
            .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
            .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888)
        applyCaptureOptions(previewBuilder, fps, caps)
        applyCaptureOptions(analysisBuilder, fps, caps)

        val preview = previewBuilder.build().also { it.setSurfaceProvider(previewView.surfaceProvider) }
        val analysis = analysisBuilder.build()
        p.unbindAll()
        camera = p.bindToLifecycle(owner, selector, preview, analysis)
        this.analysis = analysis
        boundConfig = config
        Log.i(
            TAG,
            "camera bound: want ${target.width}x${target.height}, got ${analysis.resolutionInfo?.resolution}, " +
                "fps setting ${config.fps} -> AE $fps",
        )
    }

    /**
     * Same capture-request options on both use cases (they share one repeating request):
     * AE fps range, and the "fast" variants of the ISP stages so the sensor keeps its full frame
     * rate. Video stabilisation is turned off: it crops, adds latency and can lower the frame rate,
     * and the decoder does not need a steady picture, only sharp frames.
     */
    @OptIn(ExperimentalCamera2Interop::class)
    private fun <T> applyCaptureOptions(builder: ExtendableBuilder<T>, fps: Pair<Int, Int>?, caps: CameraCaps.Caps?) {
        @Suppress("DEPRECATION") // camera2Interop {} supersedes the Extender from CameraX 1.7.
        val ext = Camera2Interop.Extender(builder)
        if (fps != null) {
            ext.setCaptureRequestOption(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, Range(fps.first, fps.second))
        }
        ext.setCaptureRequestOption(
            CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
            CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF,
        )
        if (caps == null) return
        if (CaptureRequest.NOISE_REDUCTION_MODE_FAST in caps.noiseReductionModes) {
            ext.setCaptureRequestOption(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_FAST)
        }
        if (CaptureRequest.EDGE_MODE_FAST in caps.edgeModes) {
            ext.setCaptureRequestOption(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_FAST)
        }
        if (CaptureRequest.COLOR_CORRECTION_ABERRATION_MODE_FAST in caps.aberrationModes) {
            ext.setCaptureRequestOption(
                CaptureRequest.COLOR_CORRECTION_ABERRATION_MODE,
                CaptureRequest.COLOR_CORRECTION_ABERRATION_MODE_FAST,
            )
        }
    }

    fun applyExposure(ev: Float) {
        val cam = camera ?: return
        val st = cam.cameraInfo.exposureState
        if (!st.isExposureCompensationSupported) return
        val idx = Tuning.evToIndex(
            ev, st.exposureCompensationStep.toFloat(),
            st.exposureCompensationRange.lower, st.exposureCompensationRange.upper,
        )
        if (idx != st.exposureCompensationIndex) cam.cameraControl.setExposureCompensationIndex(idx)
    }

    private fun focusAt(x: Float, y: Float) {
        val cam = camera ?: return
        val point = previewView.meteringPointFactory.createPoint(x, y)
        val action = FocusMeteringAction.Builder(point, FocusMeteringAction.FLAG_AF or FocusMeteringAction.FLAG_AE)
            .setAutoCancelDuration(3, TimeUnit.SECONDS)
            .build()
        cam.cameraControl.startFocusAndMetering(action)
        onTapFocus(x, y)
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun setupGestures() {
        val scale = ScaleGestureDetector(context, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(detector: ScaleGestureDetector): Boolean {
                val cam = camera ?: return false
                val z = cam.cameraInfo.zoomState.value ?: return false
                cam.cameraControl.setZoomRatio(
                    (z.zoomRatio * detector.scaleFactor).coerceIn(z.minZoomRatio, z.maxZoomRatio),
                )
                return true
            }
        })
        val tap = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent) = true
            override fun onSingleTapUp(e: MotionEvent): Boolean {
                focusAt(e.x, e.y)
                return true
            }
        })
        previewView.setOnTouchListener { v, ev ->
            scale.onTouchEvent(ev)
            if (!scale.isInProgress) tap.onTouchEvent(ev)
            if (ev.actionMasked == MotionEvent.ACTION_UP) v.performClick()
            true
        }
    }

    companion object {
        private const val TAG = "CodeTrans"
    }
}

/** What a camera really supports, read from Camera2 (settings page + [CameraController.bind]). */
object CameraCaps {
    /** One YUV_420_888 output size (sensor orientation) and the highest fps it can stream at. */
    data class StreamSize(val width: Int, val height: Int, val maxFps: Int)

    class Caps(
        val cameraId: String,
        /** AE target fps ranges (lower, upper). */
        val fpsRanges: List<Pair<Int, Int>>,
        /** Largest first. */
        val yuvSizes: List<StreamSize>,
        val noiseReductionModes: Set<Int>,
        val edgeModes: Set<Int>,
        val aberrationModes: Set<Int>,
    ) {
        /** Max fps of exactly this size, null if the camera does not list it. */
        fun maxFpsFor(w: Int, h: Int): Int? =
            yuvSizes.firstOrNull { it.width == w && it.height == h }?.maxFps?.takeIf { it > 0 }
    }

    /**
     * The camera CameraX picks for DEFAULT_BACK_CAMERA: the first back-facing camera that is
     * BACKWARD_COMPATIBLE (CameraX ignores depth-only and similar cameras).
     */
    fun backCamera(context: Context): Caps? = try {
        val cm = context.getSystemService<CameraManager>()
        cm?.cameraIdList?.firstNotNullOfOrNull { id ->
            val ch = cm.getCameraCharacteristics(id)
            val caps = ch.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: IntArray(0)
            if (ch.get(CameraCharacteristics.LENS_FACING) != CameraCharacteristics.LENS_FACING_BACK ||
                CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE !in caps
            ) null else of(id, ch)
        }
    } catch (e: Exception) {
        Log.w("CodeTrans", "camera caps", e)
        null
    }

    @OptIn(ExperimentalCamera2Interop::class)
    fun forCameraInfo(context: Context, info: CameraInfo): Caps? = try {
        val id = Camera2CameraInfo.from(info).cameraId
        context.getSystemService<CameraManager>()?.let { of(id, it.getCameraCharacteristics(id)) }
    } catch (e: Exception) {
        Log.w("CodeTrans", "camera caps", e)
        null
    }

    private fun of(id: String, ch: CameraCharacteristics): Caps {
        val map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val sizes = map?.getOutputSizes(ImageFormat.YUV_420_888).orEmpty()
            .map { s ->
                val ns = runCatching { map!!.getOutputMinFrameDuration(ImageFormat.YUV_420_888, s) }.getOrDefault(0L)
                StreamSize(s.width, s.height, Tuning.fpsFromMinFrameDuration(ns))
            }
            .distinct()
            .sortedByDescending { it.width.toLong() * it.height }
        fun modes(k: CameraCharacteristics.Key<IntArray>) = ch.get(k)?.toSet().orEmpty()
        return Caps(
            cameraId = id,
            fpsRanges = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
                ?.map { it.lower to it.upper }.orEmpty(),
            yuvSizes = sizes,
            noiseReductionModes = modes(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES),
            edgeModes = modes(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES),
            aberrationModes = modes(CameraCharacteristics.COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES),
        )
    }
}
