package com.codetrans

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.util.AttributeSet
import android.view.View

/**
 * Boxes over the camera preview for the QR codes in view (stabilised by [CodeTracker]):
 * green = read, amber = seen but unreadable (too small / blurred / moiré).
 */
class CodeOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {
    private var snap = CodeTracker.Snapshot.EMPTY
    private val path = Path()
    private val pt = FloatArray(2)
    private val density = resources.displayMetrics.density

    private val strokeOk = stroke(COLOR_OK)
    private val strokeBad = stroke(COLOR_BAD)
    private val fillOk = fill(COLOR_OK)
    private val fillBad = fill(COLOR_BAD)

    init {
        isClickable = false
        isFocusable = false
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    fun setCodes(s: CodeTracker.Snapshot) {
        if (s.count == 0 && snap.count == 0) return
        snap = s
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        val s = snap
        if (s.count == 0 || width == 0 || height == 0 || s.frameW == 0) return
        for (i in 0 until s.count) {
            val q = s.quads[i]
            path.reset()
            for (k in 0 until 4) {
                Tuning.frameToView(q[2 * k], q[2 * k + 1], s.frameW, s.frameH, s.rotation, width, height, pt)
                if (k == 0) path.moveTo(pt[0], pt[1]) else path.lineTo(pt[0], pt[1])
            }
            path.close()
            val ok = s.read[i]
            val fill = if (ok) fillOk else fillBad
            val stroke = if (ok) strokeOk else strokeBad
            fill.alpha = (0x33 * s.opacity[i]).toInt()
            stroke.alpha = (255 * s.opacity[i]).toInt()
            canvas.drawPath(path, fill)
            canvas.drawPath(path, stroke)
        }
    }

    private fun stroke(color: Int) = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 3f * density
        strokeJoin = Paint.Join.ROUND
        this.color = color
    }

    private fun fill(color: Int) = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        this.color = (color and 0x00FFFFFF) or 0x33000000
    }

    private companion object {
        const val COLOR_OK = 0xFF00E676.toInt()
        const val COLOR_BAD = 0xFFFFC400.toInt()
    }
}
