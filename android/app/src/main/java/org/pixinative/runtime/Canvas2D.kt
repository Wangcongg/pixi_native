package org.pixinative.runtime

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.Shader
import android.text.TextPaint
import android.util.Base64
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import kotlin.math.max

/**
 * canvas2d 桥(≙ src/mac/canvas2d_jsc.h 的 CoreGraphics 实现):game_host.js 的 HTMLCanvasElement/2D context 垫层经
 * C++ __c2d* 绑定调到这里。每个画布 = 一张 ARGB_8888 Bitmap(Android 内存布局即左上行优先的预乘 RGBA,与 Apple 的
 * kCGImageAlphaPremultipliedLast 缓冲同形,getData 直接 copyPixelsToBuffer 零转换)。字体/颜色解析复用 TextRasterizer。
 * 语义对齐项:baseline(top/middle/bottom/hanging/alphabetic)按 ascent/descent 换算;渐变 stops 是扁平 [offset,r,g,b,a](0..1);
 * shadow 走 setShadowLayer;toDataURL 输出 data:image/(png|jpeg);base64,...;putData 输入直通 alpha RGBA(setPixels 内部预乘)。
 */
class Canvas2D(private val text: TextRasterizer) {
    private class Surface(var bitmap: Bitmap, var canvas: Canvas)
    private val surfaces = ArrayList<Surface?>()

    private fun at(id: Int): Surface? = if (id in surfaces.indices) surfaces[id] else null

    fun create(w: Int, h: Int): Int {
        val bmp = Bitmap.createBitmap(max(1, w), max(1, h), Bitmap.Config.ARGB_8888)
        surfaces.add(Surface(bmp, Canvas(bmp)))
        return surfaces.size - 1
    }
    fun resize(id: Int, w: Int, h: Int) {
        val s = at(id) ?: return
        val bmp = Bitmap.createBitmap(max(1, w), max(1, h), Bitmap.Config.ARGB_8888)
        s.bitmap.recycle(); s.bitmap = bmp; s.canvas = Canvas(bmp)
    }
    fun clearRect(id: Int, x: Float, y: Float, w: Float, h: Float) {
        val s = at(id) ?: return
        s.canvas.save(); s.canvas.clipRect(x, y, x + w, y + h); s.canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR); s.canvas.restore()
    }
    fun fillRect(id: Int, x: Float, y: Float, w: Float, h: Float, cssColor: String, alpha: Float) {
        val s = at(id) ?: return
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.color = text.parseColor(cssColor)
        paint.alpha = (Color.alpha(paint.color) * alpha.coerceIn(0f, 1f)).toInt()
        s.canvas.drawRect(x, y, x + w, y + h, paint)
    }

    private fun textPaint(font: String, align: String, shadowCss: String, blur: Float, sx: Float, sy: Float, alpha: Float): TextPaint {
        val paint = TextPaint(Paint.ANTI_ALIAS_FLAG or Paint.SUBPIXEL_TEXT_FLAG)
        text.applyFont(paint, text.parseCssFont(font))
        paint.textAlign = when (align) { "center" -> Paint.Align.CENTER; "right", "end" -> Paint.Align.RIGHT; else -> Paint.Align.LEFT }
        val shadow = if (shadowCss.isEmpty() || shadowCss == "transparent") 0 else text.parseColor(shadowCss)
        if (Color.alpha(shadow) > 0 && (blur > 0f || sx != 0f || sy != 0f)) {
            val a = (Color.alpha(shadow) * alpha.coerceIn(0f, 1f)).toInt()
            paint.setShadowLayer(max(0.01f, blur), sx, sy, Color.argb(a, Color.red(shadow), Color.green(shadow), Color.blue(shadow)))
        }
        return paint
    }
    private fun baselineY(paint: Paint, y: Float, baseline: String): Float {
        val fm = paint.fontMetrics
        val ascent = -fm.ascent; val descent = fm.descent
        return when (baseline) {
            "top" -> y + ascent
            "middle" -> y + ascent - (ascent + descent) / 2f
            "bottom" -> y - descent
            "hanging" -> y + ascent * 0.8f
            else -> y
        }
    }
    fun fillText(id: Int, str: String, x: Float, y: Float, font: String, cssColor: String, baseline: String, alpha: Float, align: String,
                 shadowCss: String, blur: Float, sx: Float, sy: Float) {
        val s = at(id) ?: return
        val paint = textPaint(font, align, shadowCss, blur, sx, sy, alpha)
        paint.color = text.parseColor(cssColor)
        paint.alpha = (Color.alpha(paint.color) * alpha.coerceIn(0f, 1f)).toInt()
        s.canvas.drawText(str, x, baselineY(paint, y, baseline), paint)
    }
    fun fillTextGrad(id: Int, str: String, x: Float, y: Float, font: String, x0: Float, y0: Float, x1: Float, y1: Float, stopsJson: String,
                     baseline: String, alpha: Float, align: String, shadowCss: String, blur: Float, sx: Float, sy: Float) {
        val s = at(id) ?: return
        val numbers = Regex("-?[0-9.]+").findAll(stopsJson).mapNotNull { it.value.toFloatOrNull() }.toList()
        val count = numbers.size / 5
        val paint = textPaint(font, align, shadowCss, blur, sx, sy, alpha)
        if (count >= 2) {
            val colors = IntArray(count); val positions = FloatArray(count)
            for (i in 0 until count) {
                positions[i] = numbers[i * 5].coerceIn(0f, 1f)
                val r = (numbers[i * 5 + 1] * 255f).toInt().coerceIn(0, 255); val g = (numbers[i * 5 + 2] * 255f).toInt().coerceIn(0, 255)
                val b = (numbers[i * 5 + 3] * 255f).toInt().coerceIn(0, 255); val a = (numbers[i * 5 + 4] * alpha.coerceIn(0f, 1f) * 255f).toInt().coerceIn(0, 255)
                colors[i] = Color.argb(a, r, g, b)
            }
            paint.shader = LinearGradient(x0, y0, x1, y1, colors, positions, Shader.TileMode.CLAMP)
        } else { paint.color = Color.WHITE; paint.alpha = (255 * alpha.coerceIn(0f, 1f)).toInt() }
        s.canvas.drawText(str, x, baselineY(paint, y, baseline), paint)
    }
    fun strokeText(id: Int, str: String, x: Float, y: Float, font: String, cssColor: String, lineWidth: Float, baseline: String, alpha: Float,
                   align: String, lineJoin: String, miterLimit: Float, shadowCss: String, blur: Float, sx: Float, sy: Float) {
        val s = at(id) ?: return
        val paint = textPaint(font, align, shadowCss, blur, sx, sy, alpha)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = max(0f, lineWidth)
        paint.strokeJoin = when (lineJoin) { "round" -> Paint.Join.ROUND; "bevel" -> Paint.Join.BEVEL; else -> Paint.Join.MITER }
        paint.strokeMiter = max(1f, miterLimit)
        paint.color = text.parseColor(cssColor)
        paint.alpha = (Color.alpha(paint.color) * alpha.coerceIn(0f, 1f)).toInt()
        s.canvas.drawText(str, x, baselineY(paint, y, baseline), paint)
    }
    fun measure(id: Int, str: String, font: String): Float {
        val paint = TextPaint(Paint.ANTI_ALIAS_FLAG or Paint.SUBPIXEL_TEXT_FLAG)
        text.applyFont(paint, text.parseCssFont(font))
        return paint.measureText(str)
    }
    /** 左上行优先、预乘 RGBA(ARGB_8888 的内存布局即如此)。 */
    fun getData(id: Int): ByteArray? {
        val s = at(id) ?: return null
        val buffer = ByteBuffer.allocate(s.bitmap.byteCount)
        s.bitmap.copyPixelsToBuffer(buffer)
        return buffer.array()
    }
    /** 直通 alpha 的 RGBA 写入(裁剪到画布内;setPixels 内部预乘)。 */
    fun putData(id: Int, rgba: ByteArray, w: Int, h: Int, dx: Int, dy: Int) {
        val s = at(id) ?: return
        if (w <= 0 || h <= 0 || rgba.size < w * h * 4) return
        val bw = s.bitmap.width; val bh = s.bitmap.height
        val left = max(0, dx); val top = max(0, dy); val right = minOf(bw, dx + w); val bottom = minOf(bh, dy + h)
        if (right <= left || bottom <= top) return
        val cw = right - left; val ch = bottom - top
        val pixels = IntArray(cw * ch)
        for (row in 0 until ch) {
            val srcRow = row + (top - dy)
            for (col in 0 until cw) {
                val srcCol = col + (left - dx)
                val o = (srcRow * w + srcCol) * 4
                val r = rgba[o].toInt() and 0xff; val g = rgba[o + 1].toInt() and 0xff; val b = rgba[o + 2].toInt() and 0xff; val a = rgba[o + 3].toInt() and 0xff
                pixels[row * cw + col] = (a shl 24) or (r shl 16) or (g shl 8) or b
            }
        }
        s.bitmap.setPixels(pixels, 0, cw, left, top, cw, ch)
    }
    fun toDataURL(id: Int, format: String, quality: Float): String {
        val s = at(id) ?: return "data:,"
        val jpeg = format.contains("jpeg") || format.contains("jpg")
        val out = ByteArrayOutputStream()
        val ok = s.bitmap.compress(if (jpeg) Bitmap.CompressFormat.JPEG else Bitmap.CompressFormat.PNG, (quality.coerceIn(0f, 1f) * 100f).toInt(), out)
        if (!ok || out.size() == 0) return "data:,"
        return "data:" + (if (jpeg) "image/jpeg" else "image/png") + ";base64," + Base64.encodeToString(out.toByteArray(), Base64.NO_WRAP)
    }
    fun save(id: Int) { at(id)?.canvas?.save() }
    fun restore(id: Int) { val c = at(id)?.canvas ?: return; if (c.saveCount > 1) c.restore() }
    fun scale(id: Int, x: Float, y: Float) { at(id)?.canvas?.scale(x, y) }
    fun translate(id: Int, x: Float, y: Float) { at(id)?.canvas?.translate(x, y) }
    fun rotate(id: Int, radians: Float) { at(id)?.canvas?.rotate((radians * 180.0 / Math.PI).toFloat()) }
    fun transform(id: Int, a: Float, b: Float, c: Float, d: Float, e: Float, f: Float) {
        val m = Matrix(); m.setValues(floatArrayOf(a, c, e, b, d, f, 0f, 0f, 1f)); at(id)?.canvas?.concat(m)
    }
    fun setTransform(id: Int, a: Float, b: Float, c: Float, d: Float, e: Float, f: Float) {
        val m = Matrix(); m.setValues(floatArrayOf(a, c, e, b, d, f, 0f, 0f, 1f)); at(id)?.canvas?.setMatrix(m)
    }
    fun resetTransform(id: Int) { at(id)?.canvas?.setMatrix(Matrix()) }
}
