package com.codetrans

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.util.AttributeSet
import android.view.View
import androidx.core.graphics.createBitmap

/**
 * Shows one or two QR codes or optical boards as large as fit, on white,
 * with integer pixels per module and no filtering so every module edge stays sharp for the PC's
 * webcam. Two codes sit side by side in landscape and one above the other in portrait.
 */
class QrPlayerView @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) : View(context, attrs) {
    private val paint = Paint().apply {
        isFilterBitmap = false
        isAntiAlias = false
        isDither = false
    }
    private var bitmaps = arrayOfNulls<Bitmap>(0)
    private var side = 0
    private val dst = Rect()
    private var pixels = IntArray(0)

    /** [modules] holds [count] contiguous matrices. Main thread. */
    fun show(side: Int, count: Int, modules: ByteArray) {
        if (side != this.side || bitmaps.size != count) {
            bitmaps.forEach { it?.recycle() }
            bitmaps = Array(count) { createBitmap(side, side) }
            pixels = IntArray(side * side)
            this.side = side
        }
        bitmaps.forEachIndexed { i, bitmap ->
            val offset = i * pixels.size
            for (k in pixels.indices) pixels[k] = if (modules[offset + k].toInt() != 0) Color.BLACK else Color.WHITE
            bitmap!!.setPixels(pixels, 0, side, 0, 0, side, side)
        }
        invalidate()
    }

    fun clear() {
        bitmaps.forEach { it?.recycle() }
        bitmaps = arrayOfNulls(0)
        side = 0
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(Color.WHITE)
        val n = bitmaps.size
        if (n == 0 || side == 0) return
        val w = width - paddingLeft - paddingRight
        val h = height - paddingTop - paddingBottom
        val sideBySide = w >= h
        val cols = if (sideBySide) n else 1
        val rows = if (sideBySide) 1 else n
        val cell = side + 2 * QUIET
        val module = minOf(w / (cols * cell), h / (rows * cell)).coerceAtLeast(1)
        val totalW = cols * cell * module
        val totalH = rows * cell * module
        val x0 = paddingLeft + (w - totalW) / 2
        val y0 = paddingTop + (h - totalH) / 2
        for (i in 0 until n) {
            val c = if (sideBySide) i else 0
            val r = if (sideBySide) 0 else i
            val left = x0 + (c * cell + QUIET) * module
            val top = y0 + (r * cell + QUIET) * module
            dst.set(left, top, left + side * module, top + side * module)
            canvas.drawBitmap(bitmaps[i]!!, null, dst, paint)
        }
    }

    private companion object {
        /** Quiet zone in modules around each code (the ISO minimum is 4). */
        const val QUIET = 4
    }
}
