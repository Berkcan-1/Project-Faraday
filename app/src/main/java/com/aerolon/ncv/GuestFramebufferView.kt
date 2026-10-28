package com.aerolon.ncv

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.View

/** Displays pixels written by the guest into NCV's simple virtual framebuffer. */
class GuestFramebufferView(context: Context) : View(context) {
    companion object {
        const val WIDTH = 640
        const val HEIGHT = 480
    }

    private val bitmap = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888)
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.DITHER_FLAG)
    private val target = RectF()

    init {
        setBackgroundColor(0xFF050A10.toInt())
        contentDescription = "Guest framebuffer output"
    }

    fun setArgbPixels(pixels: IntArray) {
        if (pixels.size != WIDTH * HEIGHT) return
        bitmap.setPixels(pixels, 0, WIDTH, 0, 0, WIDTH, HEIGHT)
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        canvas.drawColor(0xFF050A10.toInt())
        val scale = minOf(width.toFloat() / WIDTH, height.toFloat() / HEIGHT)
        if (scale <= 0f) return
        val w = WIDTH * scale
        val h = HEIGHT * scale
        target.set((width - w) / 2f, (height - h) / 2f, (width + w) / 2f, (height + h) / 2f)
        canvas.drawBitmap(bitmap, null, target, paint)
    }
}
