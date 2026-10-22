package org.pixinative.runtime

import android.app.Activity
import android.app.ActivityManager
import android.app.AlertDialog
import android.content.res.AssetManager
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.util.Base64
import android.util.Log
import android.view.Choreographer
import android.view.Gravity
import android.view.MotionEvent
import android.view.PixelCopy
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputConnectionWrapper
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.json.JSONObject
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.Headers
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.ByteArrayOutputStream
import java.util.Locale
import java.util.concurrent.Executors
import kotlin.math.roundToInt

class MainActivity : Activity(), SurfaceHolder.Callback, Choreographer.FrameCallback {
    private lateinit var gameView: SurfaceView
    private lateinit var rootView: FrameLayout
    private lateinit var textRasterizer: TextRasterizer
    private lateinit var canvas2d: Canvas2D   // canvas2d 桥(C++ __c2d* → Canvas2D.kt)
    private lateinit var audioBridge: AudioBridge
    private val mainHandler = Handler(Looper.getMainLooper())
    @Volatile private var splashReady = false
    private var running = false
    private val framePacer by lazy {
        val override = if (BuildConfig.DEBUG && intent?.hasExtra("targetFps") == true)
            intent.getIntExtra("targetFps", 0) else null
        val lowRam = getSystemService(ActivityManager::class.java)?.isLowRamDevice == true
        NativeFramePacer(NativeFramePacer.deviceReason(Build.MANUFACTURER, Build.MODEL, lowRam),
            adaptive = override == null, overrideFps = override)
    }
    // Layout, touch and IME coordinates stay in View pixels. Only the Surface buffer shrinks.
    private val gameRenderScale by lazy {
        val scale = if (BuildConfig.DEBUG) intent?.getFloatExtra("renderScale", BuildConfig.GAME_RENDER_SCALE)
            ?: BuildConfig.GAME_RENDER_SCALE else BuildConfig.GAME_RENDER_SCALE
        require(scale.isFinite() && scale in 0.5f..1.0f) { "renderScale must be between 0.5 and 1.0" }
        scale
    }
    private val fixedGameRenderSize by lazy {
        // A debug scale override still permits comparison with the original buffer sizes.
        if (BuildConfig.DEBUG && intent?.hasExtra("renderScale") == true) 0 to 0
        else BuildConfig.GAME_RENDER_WIDTH to BuildConfig.GAME_RENDER_HEIGHT
    }
    private var fpsView: TextView? = null
    private var fpsWindowStartNanos = 0L
    private var fpsWindowPresents = 0
    private var reloadOverlay: View? = null
    // 隐形输入承接器:游戏内 TextInput 聚焦时弹系统键盘,文本经 nativeKbInput 回传 JS(≙ iOS _kbField)
    private lateinit var kbField: EditText
    private var kbMaxLen = 0
    private var kbSyncing = false
    private val kbFieldRect = android.graphics.RectF()   // 游戏逻辑像素(= View 像素),不随渲染缓冲区缩放:触点命中判定 + 键盘避让
    private var kbWantHide = false      // 按下落在输入框外 → 抬起并跑完 JS 后收起(JS 重新聚焦会经 kbShow/kbKeep 取消),≙ iOS _kbWantHide
    private var kbImeHeight = 0         // 软键盘高度(窗口像素);API 30+ 由 WindowInsets ime() 精确给出,更低版本按经验估算
    private var kbImeVisible = false    // API 30+ 的 ime() 可见性(浮动键盘高度为 0 但可见)
    private var kbShift = 0f            // 当前 GL 视图上移量(≙ iOS _glView.transform 平移)
    private var kbComposing = false     // 最近一次回传 JS 的组字态(输入法未上屏的拼音等,≙ iOS markedTextRange != nil)

    external fun nativeConfigure(query: String, prelude: String)
    external fun nativeCreate(assets: AssetManager, assetServerUrl: String, filesPath: String)
    // Web Vibration API: a duration, alternating on/off pattern, or cancellation.
    fun nativeVibrate(patternJson: String): Boolean {
        val vibrator = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            getSystemService(android.os.VibratorManager::class.java)?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            getSystemService(android.content.Context.VIBRATOR_SERVICE) as? android.os.Vibrator
        }
        if (vibrator?.hasVibrator() != true) return false
        return try {
            val input = org.json.JSONArray(patternJson)
            require(input.length() <= 128) { "Vibration pattern exceeds 128 entries" }
            val durations = LongArray(input.length()) { index ->
                input.getLong(index).also { require(it in 0L..10000L) { "Invalid vibration duration" } }
            }
            if (durations.isEmpty() || durations.all { it == 0L }) vibrator.cancel()
            else {
                val timings = longArrayOf(0L) + durations
                vibrator.vibrate(android.os.VibrationEffect.createWaveform(timings, -1))
            }
            Log.i("PixiNative", "[vibration] accepted pattern=$patternJson")
            true
        } catch (error: Exception) {
            Log.e("PixiNative", "[vibration] request failed", error)
            false
        }
    }
    external fun nativeSurfaceCreated(surface: Surface, width: Int, height: Int, bufferWidth: Int, bufferHeight: Int, safeAreaLeftPx: Float)
    external fun nativeSurfaceChanged(width: Int, height: Int, bufferWidth: Int, bufferHeight: Int)
    external fun nativeDrawFrame(timeNanos: Long): Boolean   // 返回本帧是否上屏(首帧后放行系统 SplashScreen)
    external fun nativeSurfaceDestroyed()
    external fun nativeDestroy()
    external fun nativeTouch(action: Int, pointerId: Int, x: Float, y: Float, timeMillis: Long)
    external fun nativeSetVisibility(visible: Boolean)
    external fun nativeHttpComplete(requestId: Int, status: Int, statusText: String, headers: String, body: ByteArray?)
    external fun nativeAudioEnded(aid: Int)
    external fun nativeKbInput(text: String, composing: Boolean)   // composing=输入法组字中(JS 派发 compositionstart/end)
    external fun nativeKbDone(isReturn: Boolean)   // true=回车提交(JS 派发 keyCode 13),false=结束编辑(仅 blur)
    external fun nativePromptDone(token: Int, text: String?)
    external fun nativeReloadRequested()

    private val io = Executors.newCachedThreadPool()

    /** 异步 XHR(C++ __xhrSend 经此发起,结果回 nativeHttpComplete → 帧头泵)。 */
    fun startHttp(requestId: Int, method: String, url: String, headersJson: String, body: ByteArray?) {
        io.execute {
            try {
                val headers = JSONObject(headersJson.ifEmpty { "{}" })
                val requestHeaders = Headers.Builder()
                for (key in headers.keys()) requestHeaders.set(key, headers.optString(key))
                val request = NativeHttpTransport.xhrRequest(method, url, requestHeaders.build(), body)
                NativeHttpTransport.resources.newCall(request).execute().use { response ->
                    val responseHeaders = JSONObject()
                    response.headers.toMultimap().forEach { (key, values) -> responseHeaders.put(key, values.joinToString(", ")) }
                    nativeHttpComplete(requestId, response.code, response.message, responseHeaders.toString(), response.body.bytes())
                }
            } catch (error: Exception) { nativeHttpComplete(requestId, 0, error.message ?: "Network error", "{}", null) }
        }
    }

    /** 同步资源 GET(C++ 后台线程/JS 线程直调,阻塞语义与 iOS fetchRemoteAsset 一致)。 */
    fun httpFetchSync(url: String): ByteArray? {
        val diagnosticPath = url.substringBefore('?').substringBefore('#')
        return NativeHttpTransport.fetchResource(url) { status, size, error ->
            val success = status == 200 && size > 0 && error == null
            val errorType = when { error != null -> error.javaClass.name; status != 200 -> "HTTP"; size == 0L -> "EmptyBody"; else -> "" }
            val message = if (error != null) error.stackTraceToString()
                else if (!success) "HTTP $status, bytes=$size" else ""
            if (!success) Log.e("PixiNative", "[fetch-http] $errorType $diagnosticPath")

        }
    }

    /** 本地持久化(C++ __persistentStorage*,≙ iOS/mac NSUserDefaults "pixi_native.localStorage.<key>")。 */
    private val localStorage by lazy { getSharedPreferences("pixi_native.localStorage", MODE_PRIVATE) }
    fun storageGet(key: String): String? = localStorage.getString(key, null)
    fun storageSet(key: String, value: String) { localStorage.edit().putString(key, value).apply() }
    fun storageRemove(key: String) { localStorage.edit().remove(key).apply() }
    fun storageClear() { localStorage.edit().clear().apply() }
    /** 剪贴板(C++ __clipboardWrite,≙ UIPasteboard.generalPasteboard.string)。 */
    fun clipboardWrite(text: String): Boolean {
        runOnUiThread {
            try { (getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager).setPrimaryClip(android.content.ClipData.newPlainText("text", text)) }
            catch (e: Exception) { android.util.Log.w("pixi_native", "clipboard write failed: ${e.message}") }
        }
        return true
    }
    /** 外链(C++ __nativeOpenURL,≙ UIApplication.openURL;http/https 已在 C++ 侧校验)。 */
    fun openUrl(url: String): Boolean {
        val intent = android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(url)).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
        if (intent.resolveActivity(packageManager) == null) return false
        runOnUiThread { try { startActivity(intent) } catch (e: Exception) { android.util.Log.w("pixi_native", "open url failed: ${e.message}") } }
        return true
    }
    // ---- canvas2d 桥(JS 线程同步 JNI;见 Canvas2D.kt) ----
    fun c2dCreate(w: Int, h: Int): Int = canvas2d.create(w, h)
    fun c2dResize(id: Int, w: Int, h: Int) = canvas2d.resize(id, w, h)
    fun c2dClearRect(id: Int, x: Float, y: Float, w: Float, h: Float) = canvas2d.clearRect(id, x, y, w, h)
    fun c2dFillRect(id: Int, x: Float, y: Float, w: Float, h: Float, color: String, alpha: Float) = canvas2d.fillRect(id, x, y, w, h, color, alpha)
    fun c2dFillText(id: Int, s: String, x: Float, y: Float, font: String, color: String, baseline: String, alpha: Float, align: String, shadow: String, blur: Float, sx: Float, sy: Float) =
        canvas2d.fillText(id, s, x, y, font, color, baseline, alpha, align, shadow, blur, sx, sy)
    fun c2dFillTextGrad(id: Int, s: String, x: Float, y: Float, font: String, x0: Float, y0: Float, x1: Float, y1: Float, stops: String, baseline: String, alpha: Float, align: String, shadow: String, blur: Float, sx: Float, sy: Float) =
        canvas2d.fillTextGrad(id, s, x, y, font, x0, y0, x1, y1, stops, baseline, alpha, align, shadow, blur, sx, sy)
    fun c2dStrokeText(id: Int, s: String, x: Float, y: Float, font: String, color: String, lineWidth: Float, baseline: String, alpha: Float, align: String, lineJoin: String, miter: Float, shadow: String, blur: Float, sx: Float, sy: Float) =
        canvas2d.strokeText(id, s, x, y, font, color, lineWidth, baseline, alpha, align, lineJoin, miter, shadow, blur, sx, sy)
    fun c2dMeasure(id: Int, s: String, font: String): Float = canvas2d.measure(id, s, font)
    fun c2dGetData(id: Int): ByteArray? = canvas2d.getData(id)
    fun c2dPutData(id: Int, rgba: ByteArray, w: Int, h: Int, dx: Int, dy: Int) = canvas2d.putData(id, rgba, w, h, dx, dy)
    fun c2dToDataURL(id: Int, format: String, quality: Float): String = canvas2d.toDataURL(id, format, quality)
    fun c2dSave(id: Int) = canvas2d.save(id)
    fun c2dRestore(id: Int) = canvas2d.restore(id)
    fun c2dScale(id: Int, x: Float, y: Float) = canvas2d.scale(id, x, y)
    fun c2dTranslate(id: Int, x: Float, y: Float) = canvas2d.translate(id, x, y)
    fun c2dRotate(id: Int, r: Float) = canvas2d.rotate(id, r)
    fun c2dTransform(id: Int, a: Float, b: Float, c: Float, d: Float, e: Float, f: Float) = canvas2d.transform(id, a, b, c, d, e, f)
    fun c2dSetTransform(id: Int, a: Float, b: Float, c: Float, d: Float, e: Float, f: Float) = canvas2d.setTransform(id, a, b, c, d, e, f)
    fun c2dResetTransform(id: Int) = canvas2d.resetTransform(id)
    /** stb_image 不认的格式(WebP 等;资产管线里有以 .png 命名的 WebP 图集页)→ BitmapFactory 解码,返回 [w, h, ARGB(直通 alpha)...];任意线程可调。 */
    fun decodeImageArgb(bytes: ByteArray, path: String): IntArray? {
        return try {
            val opts = android.graphics.BitmapFactory.Options()
            opts.inPreferredConfig = android.graphics.Bitmap.Config.ARGB_8888
            opts.inPremultiplied = false
            val bmp = android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.size, opts)
            if (bmp == null) {
                Log.e("PixiNative", "Image decode failed: $path ${opts.outMimeType}")
                return null
            }
            val w = bmp.width; val h = bmp.height
            val out = IntArray(2 + w * h); out[0] = w; out[1] = h
            bmp.getPixels(out, 2, w, 0, 0, w, h); bmp.recycle()
            out
        } catch (e: Throwable) {
            Log.e("PixiNative", "Image decode failed: $path", e)
            null
        }
    }
    // ---- 音频(UI 线程 = JS 线程,直调) ----
    fun auLoad(aid: Int, bytes: ByteArray): Double = audioBridge.load(aid, bytes)
    fun auClone(nid: Int, sid: Int): Double = audioBridge.clone(nid, sid)
    fun auCtl(aid: Int, op: Int, v1: Double, v2: Double): Double = audioBridge.ctl(aid, op, v1, v2)
    fun auShutdown() = audioBridge.shutdown()   // C++ setupGame 开头调:新 JS 世界之前释放旧世界全部实例

    // ---- 文本栅格化(UI 线程直调;C++ NativeTextSystem 持缓存,这里只做测量与位图) ----
    fun textMeasure(specJson: String, out: FloatArray): Boolean = textRasterizer.measure(specJson, out)
    /** token 级富文本(排版在 C++):度量 / 按落点绘制,签名与 native_runtime.cpp 的 gMidRichTokens/gMidRichRaster 对应。 */
    fun richTokens(specJson: String): FloatArray? = textRasterizer.richTokens(specJson)
    fun richRasterize(specJson: String, pw: Int, ph: Int, resolution: Float, placements: FloatArray): Bitmap? =
        textRasterizer.richTokensRasterize(specJson, pw, ph, resolution, placements)
    fun textRasterize(specJson: String, pw: Int, ph: Int, resolution: Float,
                      surfaceW: Float, surfaceH: Float, naturalH: Float, imgOut: FloatArray?): Bitmap? =
        textRasterizer.rasterize(specJson, pw, ph, resolution, surfaceW, surfaceH, naturalH, imgOut)

    // ---- 键盘(隐形 EditText 承接;文本变化回传 __kbInput,回车/收起回传 __kbDone;语义逐条对齐 ios_app.mm 的 kb* 方法) ----
    fun kbShow(text: String, maxLen: Int, secure: Boolean, x: Float, y: Float, w: Float, h: Float) {
        kbWantHide = false   // 点击落在输入框上重新聚焦:取消延迟收起
        kbFieldRect.set(x, y, x + w, y + h)
        kbMaxLen = maxLen
        kbSyncing = true
        kbField.inputType = if (secure)
            InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        else
            InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        kbField.setText(text)
        kbField.setSelection(kbField.text.length)
        kbSyncing = false
        kbComposing = false   // setText 重启输入会话,旧组字区随之作废
        kbField.visibility = View.VISIBLE
        kbField.requestFocus()
        (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager)
            .showSoftInput(kbField, InputMethodManager.SHOW_IMPLICIT)
        if (Build.VERSION.SDK_INT < 30 && kbImeHeight == 0) kbImeHeight = (gameView.height * 0.45f).toInt()   // 无 ime() insets:横屏软键盘约占 40-50% 高
        kbUpdateShift()   // 键盘已在显示时切换输入框:insets 不会再变,这里重算避让
    }
    fun kbHide() = kbHideInternal(reportDone = true)
    /** reportDone:收起后补 __kbDone(0)(≙ iOS resignFirstResponder → textFieldDidEndEditing;JS 侧幂等);回车路径已发过 __kbDone(1) 则不再补。 */
    private fun kbHideInternal(reportDone: Boolean) {
        kbWantHide = false
        kbComposing = false   // JS 的 __kbDone 自行收尾 compositionend
        if (kbField.hasFocus()) {
            (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager)
                .hideSoftInputFromWindow(kbField.windowToken, 0)
            kbField.clearFocus()
            if (reportDone) nativeKbDone(false)
        }
        kbField.visibility = View.GONE
        if (Build.VERSION.SDK_INT < 30) kbImeHeight = 0
        kbUpdateShift()
    }
    fun kbKeep() { kbWantHide = false }   // JS 重复聚焦当前输入框(会话未变):仅取消延迟收起,不动文本
    /** 软键盘被系统收起(BACK / 手势 / 切走):JS 仍处于编辑态,不回传就会残留 __kbActive,再点同一输入框只走 __kbKeep 而键盘不再弹出。 */
    private fun kbImeDismissed(reason: String) {
        if (!kbField.hasFocus()) return
        Log.i("PixiNative", "[kb] ime dismissed by $reason")
        kbWantHide = false
        kbComposing = false
        kbField.clearFocus()
        kbField.visibility = View.GONE
        nativeKbDone(false)
        if (Build.VERSION.SDK_INT < 30) kbImeHeight = 0
        kbUpdateShift()
    }
    /** 键盘避让(≙ iOS kbUpdateShift):输入框底边被键盘盖住多少,GL 视图就上移多少(+12dp 边距);SurfaceView 的 translationY 会同步移动 surface。 */
    private fun kbUpdateShift() {
        var shift = 0f
        val fieldBottom = kbFieldRect.bottom
        if (kbImeHeight > 0 && fieldBottom > 0 && kbField.hasFocus()) {
            val keyboardTop = gameView.height - kbImeHeight
            val wanted = fieldBottom + dp(12)
            if (wanted > keyboardTop) shift = wanted - keyboardTop
        }
        if (shift == kbShift) return
        Log.i("PixiNative", "[kb-shift] fieldBottom=${fieldBottom.toInt()} imeHeight=$kbImeHeight viewH=${gameView.height} → shift=${shift.toInt()}")
        kbShift = shift
        gameView.animate().cancel()
        gameView.animate().translationY(-shift).setDuration(200).start()
    }
    fun kbSetText(text: String) {
        if (kbField.hasFocus() && kbField.text.toString() != text) {
            kbSyncing = true
            kbField.setText(text)
            kbField.setSelection(kbField.text.length)
            kbSyncing = false
        }
    }
    /** 输入法组字区(拼音等未上屏的候选,带 SPAN_COMPOSING)是否存在:≙ iOS UITextField.markedTextRange != nil。 */
    private fun kbHasComposing(text: CharSequence?): Boolean {
        val spannable = text as? android.text.Spannable ?: return false
        val start = BaseInputConnection.getComposingSpanStart(spannable)
        val end = BaseInputConnection.getComposingSpanEnd(spannable)
        return start >= 0 && end > start
    }
    /** 文本 + 组字态回传 JS(≙ iOS kbChanged → __kbInput(text, composing)):JS 按组字态派发 compositionstart/compositionend,
     *  业务在组字期间不校验、不回写——回写会走 kbSetText → EditText.setText,重启输入会话并丢掉正在拼的字。 */
    private fun kbReportText() {
        val composing = kbHasComposing(kbField.text)
        kbComposing = composing
        nativeKbInput(kbField.text.toString(), composing)
    }
    /** 输入法结束组字但文本不变(finishComposingText 只摘组字 span,不触发 TextWatcher):补一次非组字回传,
     *  否则 JS 停在组字态,业务的输入校验要等到收键盘才执行。 */
    private fun kbComposingFinished() {
        if (!kbComposing || kbSyncing || !kbField.hasFocus() || kbHasComposing(kbField.text)) return
        kbReportText()
    }

    // ---- prompt(非阻塞编辑框,完成回 nativePromptDone) ----
    fun promptShow(token: Int, title: String, value: String, placeholder: String) {
        mainHandler.post {
            val input = EditText(this)
            input.setText(value)
            input.hint = placeholder
            var finished = false
            val finish = { text: String? ->
                if (!finished) { finished = true; nativePromptDone(token, text) }
            }
            AlertDialog.Builder(this)
                .setTitle(title.ifEmpty { "Edit value" })
                .setView(input)
                .setNegativeButton("Cancel") { _, _ -> finish(null) }
                .setPositiveButton("OK") { _, _ -> finish(input.text.toString()) }
                .setOnCancelListener { finish(null) }
                .show()
        }
    }

    // ---- reload 弹窗:1:1 复刻 iOS showReloadTitle:content:(ios_app.mm),dp ≙ iOS point ----
    // 遮罩 rgba(0,0,0,.5) 拦截触摸;弹窗以宽 50% 排版,高度不足时等比缩小,上下各留至少 24dp / 8%;
    // 按下缩放 .95(80ms)后 reload。字号用 dp 不用 sp:iOS 的 UIFont 固定磅值不随系统字体缩放。
    private fun nativeTypeface(bold: Boolean): Typeface = if (bold) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
    fun showReload(title: String, content: String) {
        mainHandler.post {
            if (reloadOverlay != null) return@post
            val density = resources.displayMetrics.density
            val overlay = object : FrameLayout(this) {
                override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
                    val width = View.MeasureSpec.getSize(widthMeasureSpec)
                    val height = View.MeasureSpec.getSize(heightMeasureSpec)
                    setMeasuredDimension(width, height)
                    // Measure the complete card before fitting it. An AT_MOST height
                    // would clip the text/button before the scale can be calculated.
                    val card = getChildAt(0)
                    card.measure(View.MeasureSpec.makeMeasureSpec(width / 2, View.MeasureSpec.EXACTLY),
                        View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED))
                    val margin = maxOf(dp(24).toFloat(), height * 0.08f)
                    val scale = if (card.measuredHeight > 0)
                        ((height - 2f * margin).coerceAtLeast(0f) / card.measuredHeight).coerceAtMost(1f)
                    else 1f
                    card.scaleX = scale
                    card.scaleY = scale
                }
            }
            overlay.setBackgroundColor(Color.argb(128, 0, 0, 0))
            overlay.isClickable = true
            val popup = LinearLayout(this)
            popup.orientation = LinearLayout.VERTICAL
            popup.gravity = Gravity.CENTER_HORIZONTAL
            popup.background = android.graphics.drawable.GradientDrawable().apply { setColor(Color.WHITE); cornerRadius = dp(20).toFloat() }
            popup.setPadding(dp(30), dp(30), dp(30), dp(40))
            val titleView = TextView(this)
            titleView.text = title.ifEmpty { "Application error" }
            titleView.typeface = nativeTypeface(true)
            titleView.setTextSize(android.util.TypedValue.COMPLEX_UNIT_DIP, 24f)
            titleView.setTextColor(Color.BLACK)
            titleView.gravity = Gravity.CENTER
            popup.addView(titleView, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(15) })
            val contentView = TextView(this)
            contentView.text = content.ifEmpty { "The application reported an error. See the runtime log." }
            contentView.typeface = nativeTypeface(false)
            contentView.setTextSize(android.util.TypedValue.COMPLEX_UNIT_DIP, 16f)
            contentView.setTextColor(Color.BLACK)
            contentView.gravity = Gravity.CENTER
            popup.addView(contentView, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(10) })
            val button = TextView(this)   // 不用 Button:避开 Material 的最小高度/内边距/大写/水波纹,按 iOS 自绘
            button.text = "REFRESH"
            button.typeface = nativeTypeface(false)
            button.setTextSize(android.util.TypedValue.COMPLEX_UNIT_DIP, 16f)
            button.setTextColor(Color.WHITE)
            button.gravity = Gravity.CENTER
            val red = Color.rgb(237, 103, 85)
            button.background = android.graphics.drawable.GradientDrawable().apply { setColor(red); cornerRadius = dp(5).toFloat() }
            button.elevation = dp(6).toFloat()   // ≙ iOS shadow offset(0,6) radius 13 opacity .6:同色阴影
            if (Build.VERSION.SDK_INT >= 28) { button.outlineSpotShadowColor = Color.argb(153, 237, 103, 85); button.outlineAmbientShadowColor = Color.argb(153, 237, 103, 85) }
            button.isClickable = true
            button.setOnClickListener {
                button.animate().scaleX(0.95f).scaleY(0.95f).setDuration(80).withEndAction {
                    button.scaleX = 1f; button.scaleY = 1f
                    hideReload(); nativeReloadRequested()
                }.start()
            }
            popup.addView(button, LinearLayout.LayoutParams(dp(200), dp(48)).apply { topMargin = dp(60) })
            overlay.addView(popup, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
            rootView.addView(overlay, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
            reloadOverlay = overlay
            Log.i("PixiNative", "[reload] 弹窗已显示: $title / $content")
        }
    }
    fun hideReload() {
        mainHandler.post {
            reloadOverlay?.let { rootView.removeView(it) }
            reloadOverlay = null
        }
    }
    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    /** PixelCopy 的完成回调线程:调用方(JS 线程 = UI 线程)要同步等结果,回调不能投回 UI 线程——否则等待期间回调永远排不上,
     *  恒定 2 秒超时返回 null(2026-09-17 审计 P0-6)。*/
    private val pixelCopyThread by lazy { android.os.HandlerThread("pixel-copy").also { it.start() } }
    /** 整屏截图 → "data:image/jpeg;base64,..."(SurfaceView 内容用 PixelCopy 同步取;≙ iOS 同步 glReadPixels)。*/
    fun snapshotJpegBase64(quality: Float): String? {
        return try {
            val w = gameView.width; val h = gameView.height
            if (w <= 0 || h <= 0) return null
            val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
            var ok = false
            if (Build.VERSION.SDK_INT >= 24) {
                val done = java.util.concurrent.CountDownLatch(1)
                PixelCopy.request(gameView, bmp, { result ->
                    ok = result == PixelCopy.SUCCESS
                    if (!ok) Log.e("PixiNative", "[snapshot] PixelCopy failed result=$result")
                    done.countDown()
                }, Handler(pixelCopyThread.looper))
                if (!done.await(2, java.util.concurrent.TimeUnit.SECONDS)) Log.e("PixiNative", "[snapshot] PixelCopy timed out")
            }
            if (!ok) return null
            val out = ByteArrayOutputStream()
            bmp.compress(Bitmap.CompressFormat.JPEG, (quality * 100).toInt().coerceIn(1, 100), out)
            "data:image/jpeg;base64," + Base64.encodeToString(out.toByteArray(), Base64.NO_WRAP)
        } catch (e: Exception) { null }
    }

    // 诊断 extras(hostQuery/jsPrelude,仅 debug 包)只消费一次:它们跟着任务的根 intent 留在最近任务里,进程被杀后从最近任务
    // 重开会被系统原样重放(2026-09-18 用户遇到"一打开就自动进机台"=上一次 adb 测试的 __NATIVE_TEST_MACHINE)。同一份参数
    // 重放时忽略;要让同样的参数再次生效,adb 加 --es launchNonce <新值>(测试脚本都带)。
    private fun debugLaunchExtras(): Pair<String, String> {
        if (!BuildConfig.DEBUG) return "" to ""
        val query = intent?.getStringExtra("hostQuery") ?: ""
        val prelude = intent?.getStringExtra("jsPrelude") ?: ""
        if (query.isEmpty() && prelude.isEmpty()) return "" to ""
        val key = (intent?.getStringExtra("launchNonce") ?: "") + "\u0000" + query + "\u0000" + prelude
        val prefs = getSharedPreferences("native_debug_launch", MODE_PRIVATE)
        if (prefs.getString("consumed", null) == key) {
            android.util.Log.i("PixiNative", "[game] 忽略重放的诊断参数(hostQuery/jsPrelude 与上次相同;要再次生效加 --es launchNonce)")
            return "" to ""
        }
        prefs.edit().putString("consumed", key).apply()
        return query to prelude
    }

    override fun onCreate(state: Bundle?) {
        val splashScreen = installSplashScreen()
        splashScreen.setKeepOnScreenCondition { !splashReady }
        splashScreen.setOnExitAnimationListener { it.remove() }
        super.onCreate(state)
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)   // ≙ iOS idleTimerDisabled=YES
        // Keep the Surface full size even while transient bars are visible. This
        // also avoids the navigation-bar strip on pre-Android-15 devices (Fire).
        WindowCompat.enableEdgeToEdge(window)
        hideGameSystemBars()
        textRasterizer = TextRasterizer(assets)
        canvas2d = Canvas2D(textRasterizer)
        audioBridge = AudioBridge(this, mainHandler) { aid -> nativeAudioEnded(aid) }
        val debugExtras = debugLaunchExtras()
        nativeConfigure(debugExtras.first,debugExtras.second)
        val assetServer = ""
        nativeCreate(assets, assetServer, filesDir.absolutePath)
        rootView = FrameLayout(this)
        gameView = object : SurfaceView(this) {
            override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
                super.onSizeChanged(w, h, oldw, oldh)
                if (w <= 0 || h <= 0) return
                val (bufferWidth, bufferHeight) = fixedGameRenderSize
                if (bufferWidth > 0 && bufferHeight > 0) {
                    holder.setFixedSize(bufferWidth, bufferHeight)
                } else if (gameRenderScale < 1f) {
                    holder.setFixedSize((w * gameRenderScale).roundToInt().coerceAtLeast(1),
                        (h * gameRenderScale).roundToInt().coerceAtLeast(1))
                }
                // Fixed-size buffers may round two nearby View sizes to the same dimensions,
                // so layout changes must be forwarded even without a surfaceChanged callback.
                if (running) {
                    val frame = holder.surfaceFrame
                    nativeSurfaceChanged(w, h, frame.width(), frame.height())
                }
            }
        }
        // 永不熄屏要挂在 View 上而不只是窗口 flag:UnityPlayer 起来后按它自己的 Screen.sleepTimeout(默认跟随系统)
        // 直接 clearFlags 宿主窗口的 FLAG_KEEP_SCREEN_ON(2026-09-18 真机:进 96180 后 flag 消失,30s 熄屏);
        // ViewRootImpl.applyKeepScreenOnFlag 取"窗口 flag ∪ 任一可见 View 的 keepScreenOn",View 级的 Unity 清不掉。
        gameView.keepScreenOn = true

        gameView.setOnTouchListener { _, event ->
            // 点画面收键盘(≙ iOS touchesBegan/touchesEnded):1) 触点命中当前输入框矩形(外扩 20px)→ 不收(库经 setTimeout 才 re-focus,
            // 同步链里等不到);2) 其余延迟到 pointerup 跑完 JS 再执行——JS 重新聚焦会经 kbShow/kbKeep 取消。立即收会走
            // "收起→重弹+文本重置",覆盖键盘会话里已输入的字符。触点与 kbShow 都是 View 逻辑像素,不乘渲染倍率。
            if (event.actionMasked == MotionEvent.ACTION_DOWN && kbField.hasFocus()) {
                kbWantHide = !android.graphics.RectF(kbFieldRect.left - 20f, kbFieldRect.top - 20f, kbFieldRect.right + 20f, kbFieldRect.bottom + 20f)
                    .contains(event.x, event.y)
            }
            if (event.actionMasked == MotionEvent.ACTION_MOVE || event.actionMasked == MotionEvent.ACTION_CANCEL) {
                for (index in 0 until event.pointerCount)
                    nativeTouch(event.actionMasked, event.getPointerId(index), event.getX(index), event.getY(index), event.eventTime)
            } else {
                val index = event.actionIndex
                nativeTouch(event.actionMasked, event.getPointerId(index), event.getX(index), event.getY(index), event.eventTime)
            }
            if (event.actionMasked == MotionEvent.ACTION_UP && kbWantHide) kbHide()   // nativeTouch 同步跑完 JS:未重新聚焦 → 点了输入框以外
            if (event.actionMasked == MotionEvent.ACTION_CANCEL) kbWantHide = false
            true
        }
        gameView.holder.addCallback(this)
        rootView.addView(gameView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT,
        ))
        // 隐形输入承接器(移出可视区;内容变化回传 JS,游戏内 surrogate 文本实时显示)
        kbField = object : EditText(this) {
            // 软键盘显示期间按 BACK:事件先到这里再到 IME。IME 收起后系统不会通知任何人,这里补 __kbDone(0)。
            override fun onKeyPreIme(keyCode: Int, event: android.view.KeyEvent): Boolean {
                if (keyCode == android.view.KeyEvent.KEYCODE_BACK && event.action == android.view.KeyEvent.ACTION_UP) kbImeDismissed("back")
                return super.onKeyPreIme(keyCode, event)
            }
            // 组字区被输入法直接摘掉(文本不变)时 TextWatcher 不会触发,只能在输入连接上接住。
            override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
                val target = super.onCreateInputConnection(outAttrs) ?: return null
                return object : InputConnectionWrapper(target, true) {
                    override fun finishComposingText(): Boolean {
                        val handled = super.finishComposingText()
                        kbComposingFinished()
                        return handled
                    }
                }
            }
        }
        kbField.visibility = View.GONE
        // 横屏下 IME 默认进入全屏"提取编辑"模式,会把整个游戏盖住;NO_EXTRACT_UI / NO_FULLSCREEN 让键盘只占下半屏。
        kbField.imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_DONE or
            android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI or android.view.inputmethod.EditorInfo.IME_FLAG_NO_FULLSCREEN
        kbField.isSingleLine = true
        kbField.alpha = 0.01f
        kbField.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: android.text.Editable?) {
                if (kbSyncing) return
                var text = s?.toString() ?: ""
                if (kbMaxLen > 0 && text.length > kbMaxLen) {
                    text = text.substring(0, kbMaxLen)
                    kbSyncing = true
                    kbField.setText(text)
                    kbField.setSelection(text.length)
                    kbSyncing = false
                }
                kbReportText()
            }
        })
        kbField.setOnEditorActionListener { _, _, _ ->
            nativeKbDone(true)   // 回车:JS 派发 keydown/keyup(13)再 blur(≙ iOS textFieldShouldReturn → __kbDone(1))
            kbHideInternal(reportDone = false); true
        }
        // API 30+:软键盘高度与可见性由 ime() insets 精确给出(windowSoftInputMode=adjustNothing 时窗口不变,只靠它做避让);
        // 键盘从可见变为不可见而输入框仍持焦 = 被系统收起(BACK 已由 onKeyPreIme 处理,这里兜住手势/浮窗键盘的收起按钮/切走)。
        // 浮动键盘(OEM 输入法横屏浮窗)高度为 0 但 isVisible 为真,所以可见性与高度分开看。
        ViewCompat.setOnApplyWindowInsetsListener(rootView) { view, insets ->
            // Desktop/freeform caption bars cannot be hidden. Reserve only that
            // space; cutouts remain handled by the game's existing safe-area API.
            // Transient status/navigation bars overlay the scene without resizing it.
            val caption = insets.getInsets(WindowInsetsCompat.Type.captionBar())
            if (view.paddingLeft != caption.left || view.paddingTop != caption.top ||
                view.paddingRight != caption.right || view.paddingBottom != caption.bottom) {
                view.setPadding(caption.left, caption.top, caption.right, caption.bottom)
            }
            if (Build.VERSION.SDK_INT >= 30) {
                val ime = insets.getInsets(WindowInsetsCompat.Type.ime()).bottom
                val visible = insets.isVisible(WindowInsetsCompat.Type.ime())
                if (ime != kbImeHeight || visible != kbImeVisible) {
                    Log.i("PixiNative", "[kb] ime insets bottom=$ime visible=$visible focused=${kbField.hasFocus()}")
                    val wasVisible = kbImeVisible
                    kbImeHeight = ime; kbImeVisible = visible
                    if (wasVisible && !visible) kbImeDismissed("insets") else kbUpdateShift()
                }
            }
            insets
        }
        rootView.addView(kbField, FrameLayout.LayoutParams(dp(120), dp(36), Gravity.TOP or Gravity.START).also {
            it.topMargin = -dp(200)
        })
        if (BuildConfig.DEBUG) {
            fpsView = TextView(this).apply {
                text = "FPS --"
                setTextColor(Color.WHITE)
                textSize = 14f
                setTypeface(typeface, android.graphics.Typeface.BOLD)
                setPadding(dp(6), dp(3), dp(6), dp(3))
                setBackgroundColor(Color.argb(180, 0, 0, 0))
                isClickable = false
                isFocusable = false
            }
            rootView.addView(fpsView, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.END,
            ).also { it.rightMargin = dp(8); it.topMargin = dp(8) })
        }
        setContentView(rootView)
        ViewCompat.requestApplyInsets(rootView)
    }

    fun dismissSplash() {
        splashReady = true
    }

    private fun safeAreaLeftPx(): Float {
        if (Build.VERSION.SDK_INT >= 28) {
            val cutout = window.decorView.rootWindowInsets?.displayCutout ?: return 0f
            return maxOf(cutout.safeInsetLeft, cutout.safeInsetRight).toFloat()
        }
        return 0f
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val frame = holder.surfaceFrame
        nativeSurfaceCreated(holder.surface, gameView.width, gameView.height, frame.width(), frame.height(), safeAreaLeftPx())
        fpsWindowStartNanos = 0L
        fpsWindowPresents = 0
        fpsView?.text = "FPS --"
        framePacer.resetForSurface()
        Log.i("PixiNative", "[frame-pacing] target=${framePacer.targetFps} reason=${framePacer.reason} model=${Build.MODEL}")
        running = true
        Choreographer.getInstance().postFrameCallback(this)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        nativeSurfaceChanged(gameView.width, gameView.height, width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        running = false
        Choreographer.getInstance().removeFrameCallback(this)
        nativeSurfaceDestroyed()
    }

    override fun doFrame(frameTimeNanos: Long) {
        if (!running) return
        var presented = false
        if (framePacer.shouldRender(frameTimeNanos)) {
            val startedNanos = System.nanoTime()
            presented = nativeDrawFrame(frameTimeNanos)
            if (framePacer.onFrameCompleted(frameTimeNanos, System.nanoTime() - startedNanos, presented))
                Log.i("PixiNative", "[frame-pacing] target=${framePacer.targetFps} reason=${framePacer.reason}")
        }
        if (fpsView != null) {
            if (fpsWindowStartNanos == 0L) fpsWindowStartNanos = frameTimeNanos
            if (presented) fpsWindowPresents++
            val elapsedNanos = frameTimeNanos - fpsWindowStartNanos
            if (elapsedNanos >= 500_000_000L) {
                val fps = fpsWindowPresents * 1_000_000_000.0 / elapsedNanos
                fpsView?.text = String.format(Locale.US, "FPS %.0f", fps)
                fpsWindowStartNanos = frameTimeNanos
                fpsWindowPresents = 0
            }
        }
        // 系统 SplashScreen(AppTheme.Launch)被 setKeepOnScreenCondition{!splashReady} 挂住:首帧真正上屏后放行,
        // 对齐 iOS LaunchScreen"首帧绘制即消失"的语义。此前 dismissSplash 没有调用者,logo 会永远盖在 GL 面上。
        if (presented && !splashReady) dismissSplash()
        Choreographer.getInstance().postFrameCallback(this)
    }

    // Android 独有的返回键:iOS 无此语义,H5 也不处理;默认 finish 会直接销毁整局游戏(JS 状态全丢)。
    // 改为退到后台(≙ iOS Home 键),游戏状态保留,再次进入即恢复。
    @Deprecated("Activity 基类回调;应用未启用 predictive back")
    override fun onBackPressed() {
        moveTaskToBack(true)
    }
    // 音频随可见性起停(≙ iOS didEnterBackground/willEnterForeground 的 auSetBackgrounded):onStop 后 JS 冻结,
    // 循环 BGM 没人能停;onStart 把 JS 认为在播的实例补播回来。
    override fun onStart() { super.onStart();nativeSetVisibility(true);if(::audioBridge.isInitialized)audioBridge.setBackgrounded(false) }
    override fun onStop() { nativeSetVisibility(false);if(::audioBridge.isInitialized)audioBridge.setBackgrounded(true);super.onStop() }

    private fun hideGameSystemBars() {
        WindowCompat.getInsetsController(window, window.decorView).apply {
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(WindowInsetsCompat.Type.statusBars() or WindowInsetsCompat.Type.navigationBars())
        }
    }
    override fun onDestroy() { io.shutdownNow();audioBridge.destroy();nativeDestroy();super.onDestroy() }

    companion object { init { System.loadLibrary("pixi_native") } }
}
