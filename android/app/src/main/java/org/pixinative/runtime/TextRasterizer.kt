package org.pixinative.runtime

import android.content.res.AssetManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Shader
import android.graphics.Typeface
import android.text.Layout
import android.text.Spanned
import android.text.StaticLayout
import android.text.TextPaint
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min

/**
 * 原生文本栅格化(≙ mac/iOS 的 CoreText 路径):C++ NativeTextSystem 持有缓存/纹理/场景图写入,
 * 这里只做"样式串+几何 → 断行/测量/RGBA 位图"。布局公式与 ios_app.mm 的 layout() 逐行同源:
 *   fontHeight = Pixi扫描ascent+descent(缺席时才回退平台 typographic metrics)
 *   baseLineHeight = requested>0 ? requested : fontHeight+stroke
 *   lineHeight = baseLineHeight+leading
 *   glyphTopOffset = stroke*0.5 + max(0,(baseLineHeight+leading-fontHeight)*0.5)
 *   width = maxLineWidth+stroke;  height = max(base,fontHeight+stroke*2)+leading+(n-1)*lineHeight
 * 富文本 = 手写 HTML 子集(与 iOS richAttributed 同一标签/CSS 面)→ 切 token 逐个测量,排版在 C++(richtext_token_layout.h),
 * 描边用"stroke 层整层重画 + fill 层覆盖"(与 Pixi canvas/iOS 的先描后填一致)。
 */
class TextRasterizer(private val assets: AssetManager) {
    private val typefaces = HashMap<String, Typeface>()
    init {
        // Project fonts are validated and staged by kit prepare. A bad registration must
        // fail with its original error, rather than silently using another typeface.
        val manifest = JSONObject(assets.open("fonts/manifest.json").bufferedReader().use { it.readText() })
        for (family in manifest.keys()) {
            val file = manifest.getString(family)
            require(file.startsWith("fonts/") && !file.contains("..")) { "Invalid project font path: $file" }
            typefaces[family] = Typeface.createFromAsset(assets, file)
        }
    }

    private fun loadTypeface(family: String): Typeface {
        val key = family.trim().trim('"', '\'')
        typefaces[key]?.let { return it }
        val tf = Typeface.create(key, Typeface.NORMAL)
        typefaces[key] = tf
        return tf
    }

    internal data class FontSpec(val size: Float, val family: String, val bold: Boolean, val italic: Boolean)

    // "italic bold 23px Family" / "normal normal bold 23px \"Family\"" → FontSpec
    internal fun parseCssFont(font: String): FontSpec {
        var size = 12f
        var family = "sans-serif"
        var bold = false
        var italic = false
        val tokens = ArrayList<String>()
        var i = 0
        while (i < font.length) {
            if (font[i].isWhitespace()) { i++; continue }
            if (font[i] == '"' || font[i] == '\'') {
                val q = font[i]; val e = font.indexOf(q, i + 1)
                if (e > i) { tokens.add(font.substring(i + 1, e)); i = e + 1 } else { tokens.add(font.substring(i + 1)); break }
            } else {
                var e = i
                while (e < font.length && !font[e].isWhitespace()) e++
                tokens.add(font.substring(i, e)); i = e
            }
        }
        var familyStart = -1
        for ((idx, t) in tokens.withIndex()) {
            val lo = t.lowercase()
            when {
                lo == "italic" -> italic = true
                lo == "bold" -> bold = true
                lo.endsWith("px") -> {
                    size = lo.removeSuffix("px").toFloatOrNull() ?: 12f
                    familyStart = idx + 1
                }
            }
        }
        if (familyStart in tokens.indices) {
            family = tokens.subList(familyStart, tokens.size).joinToString(" ").split(",")[0].trim().trim('"', '\'')
        }
        return FontSpec(max(1f, size), family, bold, italic)
    }

    internal fun applyFont(paint: TextPaint, spec: FontSpec) {
        var tf = loadTypeface(spec.family)
        val style = (if (spec.bold) Typeface.BOLD else 0) or (if (spec.italic) Typeface.ITALIC else 0)
        if (style != 0) tf = Typeface.create(tf, style)
        paint.typeface = tf
        paint.textSize = spec.size
        paint.isAntiAlias = true
    }

    // CSS 颜色 → ARGB;transparent → 0
    internal fun parseColor(css: String): Int {
        val v = css.trim()
        if (v.isEmpty() || v.equals("transparent", true)) return 0
        try {
            if (v.startsWith("#")) {
                return when (v.length) {
                    4 -> { // #rgb
                        val r = v[1].digitToInt(16) * 17; val g = v[2].digitToInt(16) * 17; val b = v[3].digitToInt(16) * 17
                        Color.rgb(r, g, b)
                    }
                    9 -> { // #rrggbbaa → ARGB
                        val r = v.substring(1, 3).toInt(16); val g = v.substring(3, 5).toInt(16)
                        val b = v.substring(5, 7).toInt(16); val a = v.substring(7, 9).toInt(16)
                        Color.argb(a, r, g, b)
                    }
                    else -> Color.parseColor(v)
                }
            }
            if (v.startsWith("rgb")) {
                val inner = v.substringAfter('(').substringBefore(')')
                val parts = inner.split(',').map { it.trim() }
                val r = parts.getOrNull(0)?.toFloatOrNull()?.toInt() ?: 0
                val g = parts.getOrNull(1)?.toFloatOrNull()?.toInt() ?: 0
                val b = parts.getOrNull(2)?.toFloatOrNull()?.toInt() ?: 0
                val a = if (parts.size > 3) ((parts[3].toFloatOrNull() ?: 1f) * 255).toInt() else 255
                return Color.argb(a.coerceIn(0, 255), r.coerceIn(0, 255), g.coerceIn(0, 255), b.coerceIn(0, 255))
            }
            return Color.parseColor(v)
        } catch (e: Exception) { return Color.BLACK }
    }

    // "#c1|#c2|..." + "0,0.5,1" → 颜色/位置对(≥5 对才生效,对齐 c2d::textGradientStops 的 >=10 floats 阈值)
    private fun gradientStops(colors: String, stops: String): Pair<IntArray, FloatArray>? {
        if (colors.isEmpty()) return null
        val cs = colors.split('|').map { parseColor(it) }
        val ps = stops.split(',').mapNotNull { it.trim().toFloatOrNull() }
        if (cs.size < 2) return null
        val positions = FloatArray(cs.size) { idx -> ps.getOrElse(idx) { idx.toFloat() / (cs.size - 1) }.coerceIn(0f, 1f) }
        return Pair(cs.toIntArray(), positions)
    }

    // ---- 纯文本 ----
    private class PlainSpec(o: JSONObject) {
        val text: String = o.optString("text")
        val font: String = o.optString("font")
        val fill: String = o.optString("fill")
        val stroke: String = o.optString("stroke")
        val strokeWidth = o.optDouble("strokeWidth", 0.0).toFloat()
        val shadow: String = o.optString("shadow")
        val shadowBlur = o.optDouble("shadowBlur", 0.0).toFloat()
        val shadowX = o.optDouble("shadowX", 0.0).toFloat()
        val shadowY = o.optDouble("shadowY", 0.0).toFloat()
        val align: String = o.optString("align", "left")
        val padding = o.optDouble("padding", 0.0).toFloat()
        val wrapWidth = o.optDouble("wrapWidth", 0.0).toFloat()
        val requestedLineHeight = o.optDouble("requestedLineHeight", 0.0).toFloat()
        val verticalAlign: String = o.optString("verticalAlign", "top")
        val letterSpacing = o.optDouble("letterSpacing", 0.0).toFloat()
        val leading = o.optDouble("leading", 0.0).toFloat()
        val pixiAscent = o.optDouble("pixiAscent", 0.0).toFloat()
        val pixiDescent = o.optDouble("pixiDescent", 0.0).toFloat()
        val gradientColors: String = o.optString("gradientColors")
        val gradientStops: String = o.optString("gradientStops")
        val gradientType = o.optInt("gradientType", 0)
    }

    private class PlainLayout {
        val lines = ArrayList<String>()
        val widths = ArrayList<Float>()
        var width = 0f; var height = 0f; var lineHeight = 0f; var glyphTopOffset = 0f
        var fontHeight = 0f; var fontAscent = 0f
    }

    private fun layoutPlain(s: PlainSpec): PlainLayout {
        val out = PlainLayout()
        val paint = TextPaint()
        applyFont(paint, parseCssFont(s.font))
        if (abs(s.letterSpacing) > 1e-4f) paint.letterSpacing = s.letterSpacing / paint.textSize
        val fm = paint.fontMetrics
        val hasPixiMetrics = s.pixiAscent > 0f && s.pixiDescent >= 0f &&
            s.pixiAscent.isFinite() && s.pixiDescent.isFinite()
        val fontAscent = if (hasPixiMetrics) s.pixiAscent else -fm.ascent
        val fontHeight = if (hasPixiMetrics) fontAscent + s.pixiDescent
            else ceil((-fm.ascent + fm.descent).toDouble()).toFloat()
        val stroke = max(0f, s.strokeWidth)
        val base = if (s.requestedLineHeight > 0f) s.requestedLineHeight
            else if (hasPixiMetrics) fontHeight + stroke
            else ceil((fontHeight + stroke).toDouble()).toFloat()
        out.fontHeight = fontHeight; out.fontAscent = fontAscent
        out.lineHeight = base + s.leading
        // PIXI's measured lineHeight already includes leading. Centre the font metrics in that
        // complete advance so leading is split above/below instead of pushing every glyph upward.
        out.glyphTopOffset = stroke * 0.5f + max(0f, (base + s.leading - fontHeight) * 0.5f)
        var maxWidth = 0f
        for (para in s.text.split("\n")) {
            if (para.isEmpty()) { out.lines.add(""); out.widths.add(0f); continue }
            if (s.wrapWidth > 0f) {
                // StaticLayout 词感知断行(≙ CTTypesetterSuggestLineBreak)
                val layout = StaticLayout.Builder.obtain(para, 0, para.length, paint, max(1, s.wrapWidth.toInt()))
                    .setIncludePad(false).build()
                for (li in 0 until layout.lineCount) {
                    val line = para.substring(layout.getLineStart(li), layout.getLineEnd(li)).trimEnd('\n')
                    val w = paint.measureText(line)
                    out.lines.add(line); out.widths.add(w); maxWidth = max(maxWidth, w)
                }
            } else {
                val w = paint.measureText(para)
                out.lines.add(para); out.widths.add(w); maxWidth = max(maxWidth, w)
            }
        }
        if (out.lines.isEmpty()) { out.lines.add(""); out.widths.add(0f) }
        out.width = maxWidth + stroke
        out.height = max(base, fontHeight + stroke * 2) + s.leading + (out.lines.size - 1) * out.lineHeight
        return out
    }

    /** measure:out=[width,height,lineHeight,glyphTopOffset](plain)或[width,height,lineHeight,0](rich)。*/
    fun measure(specJson: String, out: FloatArray): Boolean {
        return try {
            val o = JSONObject(specJson)
            if (o.optString("kind") == "rich") {
                false   // 富文本改走 richTokens(排版在 C++),此入口只服务普通文本
            } else {
                val s = PlainSpec(o)
                if (s.text.isEmpty()) { out[0] = 0f; out[1] = 0f; out[2] = 0f; out[3] = 0f; return true }
                val l = layoutPlain(s)
                out[0] = l.width; out[1] = l.height; out[2] = l.lineHeight; out[3] = l.glyphTopOffset
                true
            }
        } catch (e: Exception) { false }
    }

    /** rasterize:pw×ph 的 ARGB_8888 位图(top-down,premultiplied——Android Bitmap 默认即是)。
     *  imgOut 非空时(rich 且 spec 带 images)按 <img> 序回填占位落点 [x,y,w,h](逻辑坐标,缺席槽保持 -1)。*/
    fun rasterize(specJson: String, pw: Int, ph: Int, resolution: Float,
                  surfaceW: Float, surfaceH: Float, naturalH: Float, imgOut: FloatArray?): Bitmap? {
        return try {
            val o = JSONObject(specJson)
            val bmp = Bitmap.createBitmap(max(1, pw), max(1, ph), Bitmap.Config.ARGB_8888)
            val canvas = Canvas(bmp)
            canvas.scale(resolution, resolution)
            if (o.optString("kind") == "rich") return null   // 富文本改走 richTokensRasterize
            drawPlain(PlainSpec(o), canvas, surfaceW, surfaceH)
            bmp
        } catch (e: Exception) { null }
    }

    private fun drawPlain(s: PlainSpec, canvas: Canvas, surfaceW: Float, surfaceH: Float) {
        val l = layoutPlain(s)
        val paint = TextPaint()
        applyFont(paint, parseCssFont(s.font))
        if (abs(s.letterSpacing) > 1e-4f) paint.letterSpacing = s.letterSpacing / paint.textSize
        val stroke = max(0f, s.strokeWidth)
        val strokeInset = stroke * 0.5f
        val naturalHeight = l.height
        val top0 = when (s.verticalAlign) {
            "middle" -> max(s.padding, (surfaceH - naturalHeight) * 0.5f)
            "bottom" -> max(s.padding, surfaceH - s.padding - naturalHeight)
            else -> s.padding
        }
        val gradient = gradientStops(s.gradientColors, s.gradientStops)
        val shadowColor = parseColor(s.shadow)
        for ((li, line) in l.lines.withIndex()) {
            val yTop = top0 + l.glyphTopOffset + li * l.lineHeight
            // Canvas/Pixi positions the alphabetic baseline with its scanned ascent, not
            // Android Paint.FontMetrics.ascent. Using the latter is the same upward drift as the
            // CoreText path and is most visible on vertically centred button labels.
            val baseline = yTop + l.fontAscent
            val x: Float; val alignPaint: Paint.Align
            when (s.align) {
                "center" -> { x = surfaceW * 0.5f; alignPaint = Paint.Align.CENTER }
                "right", "end" -> { x = surfaceW - s.padding - strokeInset; alignPaint = Paint.Align.RIGHT }
                else -> { x = s.padding + strokeInset; alignPaint = Paint.Align.LEFT }
            }
            paint.textAlign = alignPaint
            if (stroke > 0f && parseColor(s.stroke) != 0) {
                paint.style = Paint.Style.STROKE
                paint.strokeWidth = stroke
                paint.strokeJoin = Paint.Join.ROUND
                paint.strokeMiter = 10f
                paint.color = parseColor(s.stroke)
                paint.shader = null
                paint.clearShadowLayer()
                canvas.drawText(line, x, baseline, paint)
            }
            paint.style = Paint.Style.FILL
            paint.strokeWidth = 0f
            if (gradient != null) {
                val gh = max(1f, l.lineHeight - s.leading)
                paint.shader = if (s.gradientType == 1)
                    LinearGradient(s.padding, yTop + gh * 0.5f, max(s.padding, surfaceW - s.padding), yTop + gh * 0.5f, gradient.first, gradient.second, Shader.TileMode.CLAMP)
                else
                    LinearGradient(surfaceW * 0.5f, yTop, surfaceW * 0.5f, yTop + gh, gradient.first, gradient.second, Shader.TileMode.CLAMP)
                paint.color = Color.WHITE
                if (shadowColor != 0) paint.setShadowLayer(max(0.01f, s.shadowBlur), s.shadowX, s.shadowY, shadowColor) else paint.clearShadowLayer()
                canvas.drawText(line, x, baseline, paint)
                paint.shader = null
            } else {
                paint.shader = null
                paint.color = parseColor(s.fill)
                if (shadowColor != 0) paint.setShadowLayer(max(0.01f, s.shadowBlur), s.shadowX, s.shadowY, shadowColor) else paint.clearShadowLayer()
                canvas.drawText(line, x, baseline, paint)
            }
            paint.clearShadowLayer()
        }
    }

    // ---- 富文本(HTML 子集,与 iOS richAttributed 同一标签/CSS 面) ----
    private data class RichStyle(
        var bold: Boolean = false, var italic: Boolean = false, var underline: Boolean = false,
        var size: Float = 12f, var kern: Float = 0f, var strokeWidth: Float = 0f, var gradientType: Int = 0,
        var lineHeight: Float = 0f, var leading: Float = 0f,
        var color: String = "#000000", var strokeColor: String = "transparent",
        var gradientColors: String = "", var gradientStops: String = "", var valign: String = "baseline",
    )
    // <img> 内联图占位(与 mac appendRichImage 同源):图集像素在 GL 纹理里,Kotlin 只留白;
    // index 对应 spec images[] 槽位,落点经 imgOut 回传 C++ 组 native quad。
    private data class ImgAttachment(val index: Int, val width: Float, val height: Float,
                                     val ascent: Float, val descent: Float, val placementAscent: Float,
                                     val block: Boolean = false)   // imgDisplay=block → 布局器强制换行
    private data class RichRun(val text: String, val style: RichStyle, val attachment: ImgAttachment? = null)
    private fun entityDecode(sIn: String): String {
        val out = StringBuilder(sIn.length)
        var i = 0
        while (i < sIn.length) {
            val c = sIn[i]
            if (c != '&') { out.append(c); i++; continue }
            val e = sIn.indexOf(';', i + 1)
            if (e < 0 || e - i > 12) { out.append('&'); i++; continue }
            when (val k = sIn.substring(i + 1, e)) {
                "lt" -> out.append('<'); "gt" -> out.append('>'); "amp" -> out.append('&')
                "quot" -> out.append('"'); "apos", "#39" -> out.append('\''); "nbsp" -> out.append(' ')
                else -> {
                    if (k.startsWith("#")) {
                        val hex = k.length > 1 && (k[1] == 'x' || k[1] == 'X')
                        val cp = k.substring(if (hex) 2 else 1).toIntOrNull(if (hex) 16 else 10)
                        if (cp != null) out.appendCodePoint(cp) else out.append(sIn, i, e + 1)
                    } else out.append(sIn, i, e + 1)
                }
            }
            i = e + 1
        }
        return out.toString()
    }
    private fun tagAttr(source: String, wanted: String): String {
        val lo = source.lowercase(); val key = wanted.lowercase()
        var p = 0
        while (true) {
            p = lo.indexOf(key, p)
            if (p < 0) return ""
            if (p > 0 && (lo[p - 1].isLetterOrDigit() || lo[p - 1] == '-' || lo[p - 1] == '_')) { p += key.length; continue }
            var q = p + key.length
            while (q < lo.length && lo[q].isWhitespace()) q++
            if (q >= lo.length || lo[q] != '=') { p = q; continue }
            q++
            while (q < source.length && source[q].isWhitespace()) q++
            if (q >= source.length) return ""
            val quote = source[q]
            if (quote == '\'' || quote == '"') {
                val e = source.indexOf(quote, q + 1)
                return if (e < 0) source.substring(q + 1) else source.substring(q + 1, e)
            }
            var e = q
            while (e < source.length && !source[e].isWhitespace() && source[e] != '>') e++
            return source.substring(q, e)
        }
    }
    private fun cssStyle(css: String, style: RichStyle) {
        for (one in css.split(';')) {
            val c = one.indexOf(':')
            if (c < 0) continue
            val k = one.substring(0, c).trim().lowercase()
            val v = one.substring(c + 1).trim()
            val lv = v.lowercase()
            when (k) {
                "color" -> { style.color = v; style.gradientColors = ""; style.gradientStops = "" }
                "font-size" -> style.size = max(1f, v.removeSuffix("px").trim().toFloatOrNull() ?: style.size)
                "font-weight" -> if (lv == "bold" || (v.toIntOrNull() ?: 0) >= 600) style.bold = true
                "font-style" -> if (lv.contains("italic")) style.italic = true
                "text-decoration" -> if (lv.contains("underline")) style.underline = true
                "letter-spacing" -> style.kern = v.removeSuffix("px").trim().toFloatOrNull() ?: 0f
                "-native-valign" -> if (lv == "top" || lv == "middle" || lv == "bottom" || lv == "baseline") style.valign = lv
                "-native-stroke-color" -> style.strokeColor = v
                "-native-stroke-width" -> style.strokeWidth = max(0f, v.removeSuffix("px").trim().toFloatOrNull() ?: 0f)
                "-native-line-height" -> style.lineHeight = max(0f, leadFloat(v))
                "-native-leading" -> style.leading = leadFloat(v)
                "-native-fill-gradient-colors" -> { style.gradientColors = if (lv == "none") "" else v; if (style.gradientColors.isEmpty()) style.gradientStops = "" }
                "-native-fill-gradient-stops" -> style.gradientStops = v
                "-native-fill-gradient-type" -> style.gradientType = if ((v.toIntOrNull() ?: 0) == 1) 1 else 0
            }
        }
    }
    // ≙ mac/iOS 的 strtod 前缀解析:"20px"→20("20" 合法前缀);严格 toFloatOrNull 会把带单位的
    // authored 尺寸整体判 null 而被忽略,与另两平台分叉。
    private fun leadFloat(s: String): Float {
        var e = 0
        while (e < s.length && (s[e].isDigit() || s[e] == '.' || s[e] == '-' || s[e] == '+')) e++
        return if (e == 0) 0f else (s.substring(0, e).toFloatOrNull() ?: 0f)
    }
    // 复刻 mac appendRichImage 的呈现尺寸/对齐契约(authored w/h 覆盖 → imgDisplay=icon 缩放 →
    // valign 度量 ≙ richtext::inlineAttachmentMetrics(richtext_layout_contract.h))。
    private fun buildImgAttachment(index: Int, desc: JSONObject, body: String, style: RichStyle, family: String): ImgAttachment? {
        var width = desc.optDouble("width", 0.0).toFloat()
        var height = desc.optDouble("height", 0.0).toFloat()
        if (width <= 0f || height <= 0f) return null
        val aw = leadFloat(tagAttr(body, "width"))
        val ah = leadFloat(tagAttr(body, "height"))
        if (aw > 0f && ah > 0f) { width = aw; height = ah }
        else if (aw > 0f) { height *= aw / max(1f, width); width = aw }
        else if (ah > 0f) { width *= ah / max(1f, height); height = ah }
        val paint = TextPaint(); applyFont(paint, FontSpec(style.size, family, style.bold, style.italic))
        val fm = paint.fontMetrics
        val fontAscent = -fm.ascent; val fontDescent = fm.descent
        if (tagAttr(body, "imgDisplay").lowercase() == "icon") {
            var scale = 0.8f * fontAscent / max(1f, height)
            val iconScale = leadFloat(tagAttr(body, "iconScale"))
            if (iconScale > 0f) scale *= iconScale
            width *= scale; height *= scale
            val sx = leadFloat(tagAttr(body, "fontScaleWidth"))
            val sy = leadFloat(tagAttr(body, "fontScaleHeight"))
            if (sx > 0f) width *= sx
            if (sy > 0f) height *= sy
            if (abs(style.kern) > 1e-4f) width += style.kern
        }
        var placementAscent = height
        when (style.valign) {
            "middle" -> placementAscent = (height + fontAscent - fontDescent) * 0.5f
            "top" -> placementAscent = fontAscent
            "bottom" -> placementAscent = height - fontDescent
        }
        return ImgAttachment(index, max(0f, width), height, max(0f, placementAscent), max(0f, height - placementAscent), placementAscent,
            tagAttr(body, "imgDisplay").lowercase() == "block")
    }
    private fun parseRich(markup: String, font: String, fill: String, stroke: String, strokeWidth: Float,
                          images: JSONArray? = null, verticalAlign: String = "baseline"): Pair<List<RichRun>, String> {
        val base = parseCssFont(font)
        val runs = ArrayList<RichRun>()
        var style = RichStyle(size = base.size, color = fill, strokeColor = stroke, strokeWidth = max(0f, strokeWidth))
        if (verticalAlign == "top" || verticalAlign == "middle" || verticalAlign == "bottom" || verticalAlign == "baseline") style.valign = verticalAlign
        var imgIndex = 0
        data class Frame(val name: String, val before: RichStyle)
        val stack = ArrayList<Frame>()
        fun emit(textRaw: String) {
            val text = entityDecode(textRaw)
            if (text.isNotEmpty()) runs.add(RichRun(text, style.copy()))
        }
        fun lastIsNewline(): Boolean = runs.isNotEmpty() && runs.last().text.endsWith("\n")
        var pos = 0
        while (pos < markup.length) {
            val lt = markup.indexOf('<', pos)
            if (lt < 0) { emit(markup.substring(pos)); break }
            if (lt > pos) emit(markup.substring(pos, lt))
            val gt = markup.indexOf('>', lt + 1)
            if (gt < 0) { emit(markup.substring(lt)); break }
            val body = markup.substring(lt + 1, gt).trim()
            var lo = body.lowercase()
            val closing = lo.startsWith("/")
            if (closing) lo = lo.substring(1).trim()
            val sp = lo.indexOfFirst { it == ' ' || it == '\t' || it == '/' }
            val name = if (sp < 0) lo else lo.substring(0, sp)
            if (closing) {
                if (name == "p" && runs.isNotEmpty() && !lastIsNewline()) emit("\n")
                for (i in stack.indices.reversed()) if (stack[i].name == name) { style = stack[i].before; while (stack.size > i) stack.removeAt(stack.size - 1); break }
            } else when (name) {
                "br" -> emit("\n")
                "img" -> {   // FairyGUI 图集图走留白占位 + native quad(≙ mac appendRichImage);非包内 URL 保留文本降级
                    val idx = imgIndex++
                    val att = images?.optJSONObject(idx)?.let { buildImgAttachment(idx, it, body, style, base.family) }
                    if (att != null) runs.add(RichRun("￼", style.copy(), att))
                    else {
                        var alt = tagAttr(body, "alt")
                        if (alt.isEmpty()) alt = tagAttr(body, "src")
                        if (alt.isNotEmpty()) {
                            val slash = alt.lastIndexOfAny(charArrayOf('/', '\\'))
                            if (slash >= 0) alt = alt.substring(slash + 1)
                            emit(alt)
                        }
                    }
                }
                "sup", "sub" -> { stack.add(Frame(name, style.copy())); style.size = max(1f, style.size * 0.75f) }
                else -> {
                    val before = style.copy()
                    when (name) {
                        "b", "strong" -> style.bold = true
                        "i", "em" -> style.italic = true
                        "u" -> style.underline = true
                        "a" -> { style.underline = true; val color = tagAttr(body, "color"); if (color.isNotEmpty()) { style.color = color; style.gradientColors = ""; style.gradientStops = "" } }
                        "font" -> {
                            val styleAttr = tagAttr(body, "style"); if (styleAttr.isNotEmpty()) cssStyle(styleAttr, style)
                            val color = tagAttr(body, "color"); if (color.isNotEmpty()) { style.color = color; style.gradientColors = ""; style.gradientStops = "" }
                            val size = tagAttr(body, "size"); if (size.isNotEmpty()) style.size = max(1f, size.toFloatOrNull() ?: style.size)
                        }
                        "span" -> cssStyle(tagAttr(body, "style"), style)
                        "p", "div" -> if (runs.isNotEmpty() && !lastIsNewline()) emit("\n")
                    }
                    stack.add(Frame(name, before))
                }
            }
            pos = gt + 1
        }
        return Pair(runs, base.family)
    }

    // ---- token 级富文本(与 host_core.h updateRich 同构):排版在 C++ richtext_token_layout.h,这里只做
    //      markup 解析 → 切 token(≙ CoreText longestEffectiveRange + 空白逐字符/词整体)→ 逐 token 测量
    //      (≙ CTLineGetTypographicBounds)→ 按 C++ 回传的落点绘制(≙ CTLineDraw)。StaticLayout 不再参与断行。 ----
    private data class RichToken(val kind: Int, val text: String, val style: RichStyle, val att: ImgAttachment?, val graphemes: Int)
    // ≙ NSCharacterSet.whitespaceAndNewlineCharacterSet(Zs + tab + 换行族)
    private fun isRichWhitespace(c: Char): Boolean =
        c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\u000B' || c == '\u000C' || c == '\u0085' || c == '\u2028' || c == '\u2029' ||
        Character.getType(c) == Character.SPACE_SEPARATOR.toInt()
    // composed grapheme 边界(≙ rangeOfComposedCharacterSequenceAtIndex),末元素 = length
    private fun graphemeBoundaries(text: String): IntArray {
        val it = java.text.BreakIterator.getCharacterInstance(); it.setText(text)
        val b = ArrayList<Int>(); var p = it.first()
        while (p != java.text.BreakIterator.DONE) { b.add(p); p = it.next() }
        if (b.isEmpty() || b.last() != text.length) b.add(text.length)
        return b.toIntArray()
    }
    private fun tokenizeRich(o: JSONObject): Pair<List<RichToken>, String>? {
        val markup = o.optString("markup")
        if (markup.isEmpty()) return null
        val (runsRaw, family) = parseRich(markup, o.optString("font"), o.optString("fill"), o.optString("stroke"),
            o.optDouble("strokeWidth", 0.0).toFloat(), o.optJSONArray("images"), o.optString("verticalAlign", "baseline"))
        if (runsRaw.isEmpty()) return null
        // ≙ longestEffectiveRange:同属性相邻 append 段合并,词不被样式相同的段边界拆开(拆开=各 +1×letterSpacing)
        val runs = ArrayList<RichRun>()
        for (r in runsRaw) {
            val last = runs.lastOrNull()
            if (last != null && last.attachment == null && r.attachment == null && last.style == r.style) runs[runs.size - 1] = RichRun(last.text + r.text, last.style)
            else runs.add(r)
        }
        val tokens = ArrayList<RichToken>()
        for (r in runs) {
            if (r.attachment != null) { tokens.add(RichToken(3, r.text, r.style, r.attachment, 1)); continue }
            val bounds = graphemeBoundaries(r.text)
            var i = 0
            while (i < bounds.size - 1) {
                val c0 = r.text[bounds[i]]
                if (isRichWhitespace(c0)) {   // 空白逐字符成 token(≙ TaggedText splitAroundWhitespace 的 /\s/ 单字符切分)
                    tokens.add(RichToken(if (c0 == '\n') 2 else 1, r.text.substring(bounds[i], bounds[i + 1]), r.style, null, 1)); i++
                } else {
                    val start = bounds[i]; var n = 0
                    while (i < bounds.size - 1 && !isRichWhitespace(r.text[bounds[i]])) { n++; i++ }
                    tokens.add(RichToken(0, r.text.substring(start, bounds[i]), r.style, null, max(1, n)))
                }
            }
        }
        return Pair(tokens, family)
    }
    private fun tokenPaint(t: RichToken, family: String): TextPaint {
        val p = TextPaint(); applyFont(p, FontSpec(t.style.size, family, t.style.bold, t.style.italic)); p.letterSpacing = 0f; return p
    }
    private fun strokeVisible(st: RichStyle): Boolean = st.strokeWidth > 0f && parseColor(st.strokeColor) != 0
    private val richFontMetrics = HashMap<FontSpec, FloatArray>()

    // Match PIXI.TextMetrics.measureFont: its scanned ink bounds differ from Paint.fontMetrics.
    // TaggedText lays out full PIXI.Text bounds (including stroke), not typographic line height.
    private fun pixiFontMetrics(font: FontSpec, paint: TextPaint): FloatArray = richFontMetrics.getOrPut(font) {
        val measurePaint = TextPaint(paint)
        measurePaint.style = Paint.Style.FILL
        measurePaint.color = Color.BLACK
        measurePaint.shader = null
        measurePaint.isUnderlineText = false
        measurePaint.clearShadowLayer()
        val sample = "|ÉqÅM"
        val width = max(1, ceil(paint.measureText(sample).toDouble()).toInt())
        val symbolWidth = max(1, ceil(paint.measureText("M").toDouble()).toInt())
        val height = symbolWidth * 2
        val baseline = (symbolWidth * 1.4).toInt()
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        try {
            bitmap.eraseColor(Color.RED)
            Canvas(bitmap).drawText(sample, 0f, baseline.toFloat(), measurePaint)
            val pixels = IntArray(width * height)
            bitmap.getPixels(pixels, 0, width, 0, 0, width, height)
            fun hasInk(row: Int): Boolean = (0 until width).any { Color.red(pixels[row * width + it]) != 255 }
            var top = 0
            while (top < baseline && !hasInk(top)) top++
            var bottom = height
            while (bottom > baseline && !hasInk(bottom - 1)) bottom--
            floatArrayOf((baseline - top).toFloat(), (bottom - baseline).toFloat())
        } finally {
            bitmap.recycle()
        }
    }

    // [TaggedText ascent, descent, full token height, raster baseline adjustment]. The fourth
    // value keeps Canvas's baseline inside the stroke-padded token box used by the layout.
    private fun tokenVerticalMetrics(t: RichToken, family: String, paint: TextPaint): FloatArray {
        val fm = pixiFontMetrics(FontSpec(t.style.size, family, t.style.bold, t.style.italic), paint)
        val stroke = t.style.strokeWidth
        val fontHeight = fm[0] + fm[1]
        val lineHeight = if (t.style.lineHeight > 0f) t.style.lineHeight else fontHeight + stroke
        val height = max(lineHeight, fontHeight + stroke * 2) + t.style.leading
        val baselineAdjustment = max(0f, (lineHeight + t.style.leading - fontHeight) * 0.5f)
        return floatArrayOf(fm[0] + stroke * 0.5f, fm[1] + stroke * 0.5f,
            ceil(height.toDouble()).toFloat(), baselineAdjustment)
    }
    /** token 度量:[naturalLineHeight, baseAscent, n] + n×12
     *  [kind(0 词/1 空白/2 换行/3 图), nakedWidth, graphemes, ascent, descent, naturalLineHeight, kern, strokeWidth(可见描边时), block, imageIndex, imageHeight, placementAscent]。
     *  letterSpacing/描边补偿不进测量(≙ mac 私有属性交给布局器线性累计),nakedWidth 是无 kern 的整 token 宽(token 内保留字体 pair kerning)。 */
    fun richTokens(specJson: String): FloatArray? {
        return try {
            val o = JSONObject(specJson)
            val basePaint = TextPaint(); applyFont(basePaint, parseCssFont(o.optString("font")))
            val bfm = basePaint.fontMetrics
            val naturalLineHeight = ceil((-bfm.ascent + bfm.descent + bfm.leading).toDouble()).toFloat()
            val tk = tokenizeRich(o) ?: return floatArrayOf(naturalLineHeight, -bfm.ascent, 0f)
            val (tokens, family) = tk
            val out = FloatArray(3 + tokens.size * 12)
            out[0] = naturalLineHeight; out[1] = -bfm.ascent; out[2] = tokens.size.toFloat()
            for ((k, t) in tokens.withIndex()) {
                val b = 3 + k * 12
                out[b] = t.kind.toFloat()
                val att = t.att
                if (att != null) {
                    out[b + 1] = att.width; out[b + 2] = 1f; out[b + 3] = att.ascent; out[b + 4] = att.descent; out[b + 5] = att.ascent + att.descent
                    out[b + 8] = if (att.block) 1f else 0f; out[b + 9] = att.index.toFloat(); out[b + 10] = att.height; out[b + 11] = att.placementAscent
                } else {
                    val p = tokenPaint(t, family)
                    out[b + 1] = p.measureText(t.text); out[b + 2] = t.graphemes.toFloat()
                    val v = tokenVerticalMetrics(t, family, p)
                    out[b + 3] = v[0]; out[b + 4] = v[1]
                    out[b + 5] = if (t.kind == 2) 0f else v[2]
                    out[b + 6] = t.style.kern; out[b + 7] = if (strokeVisible(t.style)) t.style.strokeWidth else 0f
                }
            }
            out
        } catch (e: Exception) { null }
    }
    /** 按 C++ 排版落点绘制:placements 每 token [x, baselineY](逻辑坐标);先描后填(≙ Pixi canvas/CoreText 路径)。
     *  letterSpacing≠0 的词逐 grapheme 绘制,advance_i = w(suffix_i) − w(suffix_{i+1}) + ls(≙ mac drawTokenLetterSpaced,后缀差分保留 pair kerning);
     *  渐变词保持整词绘制,渐变按该 token 墨迹盒建(≙ c2d::drawLineTextGradients 的 path bounds)。 */
    fun richTokensRasterize(specJson: String, pw: Int, ph: Int, resolution: Float, placements: FloatArray): Bitmap? {
        return try {
            val o = JSONObject(specJson)
            val bmp = Bitmap.createBitmap(max(1, pw), max(1, ph), Bitmap.Config.ARGB_8888)
            val tk = tokenizeRich(o) ?: return bmp
            val (tokens, family) = tk
            val canvas = Canvas(bmp)
            canvas.scale(resolution, resolution)
            fun drawTok(k: Int, t: RichToken, p: TextPaint) {
                if (k * 2 + 1 >= placements.size) return
                val x = placements[k * 2]
                val y = placements[k * 2 + 1] + tokenVerticalMetrics(t, family, p)[3]
                val perChar = abs(t.style.kern) > 1e-4f && t.kind == 0 && gradientStops(t.style.gradientColors, t.style.gradientStops) == null
                if (!perChar) { canvas.drawText(t.text, x, y, p); return }
                val bounds = graphemeBoundaries(t.text)
                var pos = x; var prev = p.measureText(t.text)
                for (i in 0 until bounds.size - 1) {
                    val s = bounds[i]; val e = bounds[i + 1]
                    canvas.drawText(t.text, s, e, pos, y, p)
                    val cur = if (e < t.text.length) p.measureText(t.text, e, t.text.length) else 0f
                    pos += prev - cur + t.style.kern; prev = cur
                }
            }
            val hasStroke = tokens.any { it.att == null && it.kind != 2 && strokeVisible(it.style) }
            if (hasStroke) for ((k, t) in tokens.withIndex()) {
                if (t.att != null || t.kind == 2 || !strokeVisible(t.style)) continue
                val p = tokenPaint(t, family)
                p.style = Paint.Style.STROKE; p.strokeWidth = t.style.strokeWidth; p.strokeJoin = Paint.Join.ROUND
                p.color = parseColor(t.style.strokeColor)
                drawTok(k, t, p)
            }
            for ((k, t) in tokens.withIndex()) {
                if (t.att != null || t.kind == 2) continue
                val p = tokenPaint(t, family)
                p.style = Paint.Style.FILL; p.isUnderlineText = t.style.underline
                val g = gradientStops(t.style.gradientColors, t.style.gradientStops)
                if (g != null && k * 2 + 1 < placements.size) {
                    val x = placements[k * 2]; val y = placements[k * 2 + 1]
                    val r = android.graphics.Rect(); p.getTextBounds(t.text, 0, t.text.length, r)
                    val left = x + r.left; val right = x + max(r.right, r.left + 1); val top = y + r.top; val bottom = y + max(r.bottom, r.top + 1)
                    p.shader = if (t.style.gradientType == 1)
                        LinearGradient(left, (top + bottom) * 0.5f, right, (top + bottom) * 0.5f, g.first, g.second, Shader.TileMode.CLAMP)
                    else
                        LinearGradient((left + right) * 0.5f, top, (left + right) * 0.5f, bottom, g.first, g.second, Shader.TileMode.CLAMP)
                    p.color = Color.WHITE
                } else {
                    p.shader = null; p.color = parseColor(t.style.color)
                }
                drawTok(k, t, p)
                p.shader = null
            }
            bmp
        } catch (e: Exception) { null }
    }
}
