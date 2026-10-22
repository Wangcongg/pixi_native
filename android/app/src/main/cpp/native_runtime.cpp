// 与 src/ios/ios_app.mm 逐段同构 —— 绑定回调直接沿用 JSC 风格(src/android/jsc_shim.h 弥合),
// 平台服务(资源 IO/图片解码/文本栅格化/音频/键盘)换 Android 实现:
//   · JS 引擎:JavaScriptCore → V8(预编译,jsc_v8_shim.h)或 QuickJS(jsc_quickjs_shim.h),CMake PIXINATIVE_JS_ENGINE 选;
//     引擎相关的少数操作(微任务泵/GC/未处理 rejection/原生字符串)只经 jsengine:: 接口,采样 profiler 按引擎各一份
//   · 图形:EAGL/CAEAGLLayer → EGL/ANativeWindow + GLES3,present 用 eglSwapBuffers
//   · 帧驱动:CADisplayLink → Choreographer(Kotlin 侧 doFrame → nativeDrawFrame)
//   · 主线程语义:UI 线程 = Choreographer 线程 = JS/GL 线程(与 iOS 相同的单线程模型);
//     dispatch_main_queue → aplat::MainQueue(帧头 drain),gLoadQ → aplat::WorkPool
//   · 资源:bundle game/ → APK assets;LAN 远程兜底走 JNI→Kotlin 同步 HTTP
//   · 文本:CoreText → JNI→Kotlin TextRasterizer(StaticLayout/Paint);纹理仍由 C++ 持有上传
//   · 音频:AVAudioPlayer → JNI→Kotlin AudioBridge(MediaPlayer 实例池,同一套池化语义)
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <zlib.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "android/jsc_shim.h"
#if PIXI_JS_ENGINE_V8
#include <v8-profiler.h>   // [jsprof] 的 V8 版走 CpuProfiler
#endif
#include "common/native_script_utf8.h"            // __decodeScriptUTF8 的严格 UTF-8 校验(Apple 版 decode 在同一头文件的 __APPLE__ 段)
#include "common/native_diag_log.h"           // 排查态 C++ 日志编译期开关(默认关)
#include "common/sha256.h"
#include "common/native_environment.h"   // __nativeGameConfig 默认值(ENV/MODE/APP_ID/SERVER_URL,与 Apple 同源)
#include <functional>
#include <random>
#include <thread>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <initializer_list>
#include "host/host_core_common.h"   // 三端共用宿主核心(类型/状态/绑定;Android 只保留平台专有部分)
#include "android/android_platform.h"

#include "cmdbuf/decoder.h"
#include "cmdbuf/scenegraph.h"
#include "cmdbuf/batcher.h"
#include "cmdbuf/native_fgui.h"
#include "cmdbuf/spineskin.h"
#include "cmdbuf/spineskel.h"
#include "cmdbuf/spinex.h"
#include "cmdbuf/richtext_layout_contract.h"
#include "cmdbuf/richtext_token_spacing.h"
#include "cmdbuf/richtext_token_layout.h"   // token 级排版(与 mac/iOS 同一份 oracle 语义)
#include "cmdbuf/native_animation_frame.h"

#include "android/native_resource_manager_android.h"
#include "common/sha256.h"

#define LOG_TAG "PixiNative"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

static macres::ResourceManager gNativeResources(&gDec);   // 字节/图片异步属主:解码在后台队列,像素不进 JS,JS 只拿 cid
static GLuint gNativeFguiWhiteTexture = 0;                // fgui 纯色图元的 1x1 白纹理
static const uint32_t kNativeFguiWhiteCid = 7999999;      // 该白纹理的 decoder 外部纹理 id(与 mac/iOS 同值)
static sg::Batcher gBatcher;        // conch P1:native sprite 合批渲染器
static sg::Batcher gShadowBatcher;  // RenderTexture 离屏合批(+coverage 计数);Android 不带 shadow 对拍旁路
static spsk::SpineSkin gSpSkin;     // spine 蒙皮桥(C++ computeWorldVertices)
static sk::SkelSet gSkel;           // P1a: spine 骨骼 world 变换桥(native updateWorldTransform)
static float* gSkelBatchBuf = nullptr;   // spine 批量驱动参数缓冲(布局=sk::BATCH_*,与 mac/iOS 对齐)
static float* gSpBuf = nullptr;
static uint32_t* gBuf = nullptr;
static const size_t kBufWords = 1u << 24; // 64MB,需容纳 4096 图集单条上传命令
static double gScreenW = 1590, gScreenH = 960, gScreenDPR = 1;  // 真机屏幕像素,传给 game_host.js
static int gRenderW = 1590, gRenderH = 960;  // Surface buffer pixels; layout/input retain gScreenW/H.
int W = 1590, H = 960;   // host_core_common 平台接口:逻辑视口,与 gScreenW/H 同步更新
static double gSafeAreaLeft = 0;    // 刘海/挖孔横向安全区(native 像素,=max(左,右)),传给 game_host.js
static std::string gRemote;   // 资源镜像根(CDN 或 Debug 局域网),由 Kotlin nativeCreate 按环境传入;空 = 不改写,按原 URL 直连。
                              // 不给默认值:局域网字面量不能编进 Release 的 .so(MainActivity 只在 Debug 且环境未命名时才传 LAN)。

// Android 特有:EGL/JNI/调度
static ANativeWindow* gWindow = nullptr;
static EGLDisplay gEglDisplay = EGL_NO_DISPLAY;
static EGLContext gEglContext = EGL_NO_CONTEXT;
static EGLSurface gEglSurface = EGL_NO_SURFACE;
static EGLConfig  gEglConfig  = nullptr;
static AAssetManager* gAssets = nullptr;
static JavaVM* gVM = nullptr;
static jobject gActivity = nullptr;    // MainActivity 全局引用
static jmethodID gMidStartHttp = nullptr;       // (ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)V
static jmethodID gMidNativeVibrate = nullptr;
static jmethodID gMidHttpFetchSync = nullptr;   // (Ljava/lang/String;)[B  任意线程,同步 GET
static jmethodID gMidAuLoad = nullptr;          // (I[B)D
static jmethodID gMidAuClone = nullptr;         // (II)D
static jmethodID gMidAuCtl = nullptr;           // (IIDD)D
static jmethodID gMidAuShutdown = nullptr;      // ()V  新 JS 世界之前释放旧世界全部 MediaPlayer(≙ iOS shutdownAudio)
static jmethodID gMidKbShow = nullptr;          // (Ljava/lang/String;IZFFFF)V
static jmethodID gMidKbHide = nullptr;          // ()V
static jmethodID gMidKbKeep = nullptr;          // ()V
static jmethodID gMidKbSetText = nullptr;       // (Ljava/lang/String;)V
static jmethodID gMidPrompt = nullptr;          // (ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V
static jmethodID gMidShowReload = nullptr;      // (Ljava/lang/String;Ljava/lang/String;)V
static jmethodID gMidHideReload = nullptr;      // ()V
static jmethodID gMidSnapshot = nullptr;        // (F)Ljava/lang/String;
static jmethodID gMidTextMeasure = nullptr;     // (Ljava/lang/String;[F)Z
static jmethodID gMidTextRaster = nullptr;      // (Ljava/lang/String;IIFFFF)Landroid/graphics/Bitmap;
static jmethodID gMidRichTokens = nullptr;      // (Ljava/lang/String;)[F  token 级富文本度量(≙ CoreText 单 token CTLineGetTypographicBounds)
static jmethodID gMidRichRaster = nullptr;      // (Ljava/lang/String;IIF[F)Landroid/graphics/Bitmap;  按 C++ 排版落点绘制 token
static int gRichTextLineTrace = 12;
static void reportRuntimeError(const char* domain,const char* code,const std::string& message,bool fatal) {
 ALOGE("[runtime] %s/%s fatal=%d: %s",domain,code,(int)fatal,message.c_str());
};   // reload/surface 重建世代:后台任务回投时比对(≙ requestContext 比对)

static aplat::MainQueue gMainQ;
static aplat::WorkPool gLoadPool;
static aplat::CpuGate gCpuGate(2);
static std::atomic<uint64_t> gHostEpoch{1};
// JNI:任意线程拿 JNIEnv(后台线程惰性 attach,线程退出自动 detach)
static JNIEnv* jniEnv() {
    if (!gVM) return nullptr;
    JNIEnv* env = nullptr;
    if (gVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    static thread_local struct Attach {
        JNIEnv* env = nullptr;
        Attach() { if (gVM && gVM->AttachCurrentThread(&env, nullptr) != JNI_OK) env = nullptr; }
        ~Attach() { if (env && gVM) gVM->DetachCurrentThread(); }
    } attach;
    return attach.env;
}
static jstring jstr(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }
static std::string jstringToStd(JNIEnv* env, jstring value);

static void resourceDiagnostic(const char* stage,const std::string& path,const char* source,
 bool success,size_t bytes,const char* errorType,const std::string& message) {
 if(!success)ALOGE("[resource] %s %s %s bytes=%zu %s: %s",stage,path.c_str(),source,bytes,errorType,message.c_str());
}

// ======================= 资源加载(对齐 ios L92-185) =======================
// URL 归一成相对路径:去协议+host、去 dev1/ 与 ./ 前缀、去 query/hash。
static std::string relOf(std::string s) {
 const auto suffix=s.find_first_of("?#");if(suffix!=std::string::npos)s.resize(suffix);
 while(s.rfind("./",0)==0)s.erase(0,2);
 while(!s.empty()&&s.front()=='/')s.erase(s.begin());
 return s;
}
// APK 把 game/ 平铺进 assets 根。业务逻辑统一请求 assets/...，而 Android
// 保留源目录名 assets1/（Apple 打包阶段会改名为 assets/），因此两种逻辑根都要兼容。
// 线程安全(AAssetManager 可多线程)。

static macres::Bytes bundledAssetData(const std::string& rel) {
    if (rel.empty() || !gAssets) return nullptr;
    std::vector<std::string> candidates;
    candidates.push_back(rel);
    candidates.push_back(rel.rfind("game/", 0) == 0 ? rel.substr(5) : "game/" + rel);
    for (const std::string& path : candidates) {
        AAsset* asset = AAssetManager_open(gAssets, path.c_str(), AASSET_MODE_BUFFER);
        if (!asset) continue;
        const auto size = static_cast<size_t>(AAsset_getLength64(asset));
        auto out = std::make_shared<std::vector<uint8_t>>(size);
        size_t read = 0;
        int result = 0;
        while (read < size) {
            result = AAsset_read(asset, out->data() + read, std::min(size - read, (size_t)INT_MAX));
            if (result <= 0) break;
            read += (size_t)result;
        }
        AAsset_close(asset);
        resourceDiagnostic("bundle_read", rel, "apk", read == size, read,
                           read == size ? "" : "AssetShortRead",
                           "path=" + path + " expected=" + std::to_string(size) + " read=" + std::to_string(read) + " result=" + std::to_string(result));
        if (read != size) return nullptr;
        return out;
    }
    resourceDiagnostic("bundle_read", rel, "apk", false, 0, "AssetOpenFailed", "AAssetManager_open failed for all bundle paths");
    return nullptr;
}
static macres::Bytes localAssetData(const std::string& rel) { return bundledAssetData(rel); }
// iOS 的 NativeH5AssetVersionMap.activeVersionCode 同语义：已选版本优先，离线回落包内版本。


// ============================================================================
// JS 线程、TweetNaCl 验签(Apple 用 CryptoKit)、基线清单建表(Apple 用 NSJSONSerialization)。
// ============================================================================
static std::string jsToStd(JSContextRef ctx, JSValueRef v);   // 定义在下文
static void callJsGlobal(const char* fname, int argc, const JSValueRef* args);   // 定义在下文(JS 线程)
static macres::Bytes fetchRemoteAsset(const std::string& url,bool immutableVersioned=false) {
 JNIEnv* env=jniEnv();if(!env||!gActivity||!gMidHttpFetchSync)return nullptr;
 jstring request=jstr(env,url);auto result=(jbyteArray)env->CallObjectMethod(gActivity,gMidHttpFetchSync,request);
 env->DeleteLocalRef(request);
 if(env->ExceptionCheck()){env->ExceptionDescribe();env->ExceptionClear();return nullptr;}
 if(!result)return nullptr;
 const auto size=env->GetArrayLength(result);auto bytes=std::make_shared<std::vector<uint8_t>>((size_t)size);
 env->GetByteArrayRegion(result,0,size,reinterpret_cast<jbyte*>(bytes->data()));env->DeleteLocalRef(result);return bytes;
}   // 定义在下文(任意线程,Kotlin 同步 GET)
static pid_t gJsThreadTid = 0;                 // JS/GL/UI 线程 tid(nativeCreate 在其上调用):[fetch-slow] 按实际线程措辞、统计阻塞 JS 线程的同步取资源
static std::atomic<uint64_t> gJsThreadSyncFetchUs{0};   // 本 perf 窗口内阻塞 JS 线程的同步远端取资源累计耗时
static std::atomic<uint32_t> gJsThreadSyncFetchCount{0};
static int gNativeAppBuild = 0;
static std::string gHostQuery, gJsPrelude;
static bool gBootSealEnforce = false;   // ≙ host_core.mm injectNativeBootConfiguration:只在开启时写 __NATIVE_BOOT_SEAL_ENFORCE=1,不写 0(保留 prelude 置位能力)   // gJsPrelude:调试用,在 game_host.js 之前求值(≙ mac GAME_JS_PRELUDE)
static bool gAstcSupported = true;   // GL 扩展查得(nativeSurfaceCreated);false ⇒ 注入 __NATIVE_DISABLE_ASTC,JS 请求 png/webp,ktx2 候选跳过(模拟器 GLES 转译层无 ASTC)   // gHostQuery:调试用 host query(≙ mac GAME_HOST_QUERY,如 no_popup=1)   // nativeConfigure 的 gameConfigJson 覆盖(空=native_environment.h 默认)
static jmethodID gMidStorageGet = nullptr, gMidStorageSet = nullptr, gMidStorageRemove = nullptr, gMidStorageClear = nullptr;   // SharedPreferences
static jmethodID gMidClipboardWrite = nullptr, gMidOpenUrl = nullptr;
static jmethodID gMidDecodeImageArgb = nullptr;   // bytes + diagnostic path → BitmapFactory ARGB pixels
// canvas2d 桥(≙ src/mac/canvas2d_jsc.h;Android 实现在 Canvas2D.kt,经 MainActivity.c2d* 转发)
static jmethodID gMidC2dCreate = nullptr, gMidC2dResize = nullptr, gMidC2dClearRect = nullptr, gMidC2dFillRect = nullptr, gMidC2dFillText = nullptr,
    gMidC2dFillTextGrad = nullptr, gMidC2dStrokeText = nullptr, gMidC2dMeasure = nullptr, gMidC2dGetData = nullptr, gMidC2dPutData = nullptr,
    gMidC2dToDataURL = nullptr, gMidC2dSave = nullptr, gMidC2dRestore = nullptr, gMidC2dScale = nullptr, gMidC2dTranslate = nullptr,
    gMidC2dRotate = nullptr, gMidC2dTransform = nullptr, gMidC2dSetTransform = nullptr, gMidC2dResetTransform = nullptr;
static std::string jstringToStd(JNIEnv* env, jstring value) {
    if (!env || !value) return std::string();
    const char* chars = env->GetStringUTFChars(value, nullptr); std::string out(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(value, chars); return out;
}

// 工作线程完成后经平台层既有的 gMainQ(帧头 drain)回投 JS 线程(Apple 用 dispatch_get_main_queue)
static std::string lowerAscii(std::string value) { for (char& c : value) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return value; }
static bool httpsUrl(const std::string& url) { return url.size() > 8 && lowerAscii(url.substr(0, 8)) == "https://"; }
static bool hex64(const std::string& value) { if (value.size() != 64) return false; for (char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false; return true; }
static bool positiveInteger(double value, double maxValue) { return std::isfinite(value) && value > 0 && value <= maxValue && std::floor(value) == value; }


static void setReadOnlyString(JSContextRef ctx, JSObjectRef global, const char* name, const std::string& value) {
    JSStringRef key = JSStringCreateWithUTF8CString(name); JSStringRef text = JSStringCreateWithUTF8CString(value.c_str());
    JSObjectSetProperty(ctx, global, key, JSValueMakeString(ctx, text), kJSPropertyAttributeReadOnly, nullptr);
    JSStringRelease(text); JSStringRelease(key);
}


// __nativeGameConfig / __nativeFguiConfig(≙ mac injectNativeGameConfiguration / host_core injectNativeFguiConfiguration):
// 游戏配置:默认值来自共用的 native_environment.h,gradle 的 GAME_* 字段可覆盖;fgui 配置:Android 无 plist/远程 kill,合帧默认开、审计 sampled。


// ---- 状态操作(语义逐项对齐 host_core.mm 的 Apple 版本) ----


// ---- 网络(工作线程 + Kotlin 同步 HTTP;Apple 为 NSURLSession 15s 空闲/240s 总预算,这里受 Kotlin 30s 读超时约束) ----


// 返回 false,JS 回退直连目录 URL。JS 侧 validatePointer 的全部校验(环境、不可变控制 URL、sha、kill switch)原样保留为第二道。

// Kotlin Remote Config 解析完成(主线程回调)→ 帧头 gMainQ 回 JS;reload 后旧上下文的请求作废(≙ iOS requestContext 守卫)。


// ---- persistentStorage(≙ Apple NSUserDefaults "pixi_native.localStorage.<key>";Android 用 SharedPreferences 同名文件,JS 线程同步 JNI) ----
static JSValueRef cbPersistentStorageGet(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef*) {
    if (argc < 1) return JSValueMakeNull(ctx);
    JNIEnv* env = jniEnv(); if (!env || !gActivity || !gMidStorageGet) return JSValueMakeNull(ctx);
    jstring jk = jstr(env, jsToStd(ctx, args[0]));
    auto jv = (jstring)env->CallObjectMethod(gActivity, gMidStorageGet, jk); env->DeleteLocalRef(jk);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return JSValueMakeNull(ctx); }
    if (!jv) return JSValueMakeNull(ctx);
    const std::string value = jstringToStd(env, jv); env->DeleteLocalRef(jv);
    JSStringRef text = JSStringCreateWithUTF8CString(value.c_str()); JSValueRef out = JSValueMakeString(ctx, text); JSStringRelease(text); return out;
}
static JSValueRef cbPersistentStorageSet(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef*) {
    if (argc < 2) return JSValueMakeBoolean(ctx, false);
    JNIEnv* env = jniEnv(); if (!env || !gActivity || !gMidStorageSet) return JSValueMakeBoolean(ctx, false);
    jstring jk = jstr(env, jsToStd(ctx, args[0])), jv = jstr(env, jsToStd(ctx, args[1]));
    env->CallVoidMethod(gActivity, gMidStorageSet, jk, jv); env->DeleteLocalRef(jk); env->DeleteLocalRef(jv);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return JSValueMakeBoolean(ctx, false); }
    return JSValueMakeBoolean(ctx, true);
}
static JSValueRef cbPersistentStorageRemove(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef*) {
    if (argc < 1) return JSValueMakeBoolean(ctx, false);
    JNIEnv* env = jniEnv(); if (!env || !gActivity || !gMidStorageRemove) return JSValueMakeBoolean(ctx, false);
    jstring jk = jstr(env, jsToStd(ctx, args[0])); env->CallVoidMethod(gActivity, gMidStorageRemove, jk); env->DeleteLocalRef(jk);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return JSValueMakeBoolean(ctx, false); }
    return JSValueMakeBoolean(ctx, true);
}
static JSValueRef cbPersistentStorageClear(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    JNIEnv* env = jniEnv(); if (!env || !gActivity || !gMidStorageClear) return JSValueMakeBoolean(ctx, false);
    env->CallVoidMethod(gActivity, gMidStorageClear);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return JSValueMakeBoolean(ctx, false); }
    return JSValueMakeBoolean(ctx, true);
}
// ---- 剪贴板 / openURL(≙ iOS UIPasteboard / UIApplication openURL;Kotlin 在 UI 线程执行) ----
static JSValueRef cbClipboardWrite(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef*) {
    if (argc < 1) return JSValueMakeBoolean(ctx, false);
    JNIEnv* env = jniEnv(); if (!env || !gActivity || !gMidClipboardWrite) return JSValueMakeBoolean(ctx, false);
    jstring jt = jstr(env, jsToStd(ctx, args[0])); const jboolean ok = env->CallBooleanMethod(gActivity, gMidClipboardWrite, jt); env->DeleteLocalRef(jt);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return JSValueMakeBoolean(ctx, false); }
    return JSValueMakeBoolean(ctx, ok == JNI_TRUE);
}
static JSValueRef cbNativeOpenURL(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef*) {
    if (argc < 1) return JSValueMakeBoolean(ctx, false);
    const std::string url = jsToStd(ctx, args[0]);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return JSValueMakeBoolean(ctx, false);
    JNIEnv* env = jniEnv(); if (!env || !gActivity || !gMidOpenUrl) return JSValueMakeBoolean(ctx, false);
    jstring ju = jstr(env, url); const jboolean ok = env->CallBooleanMethod(gActivity, gMidOpenUrl, ju); env->DeleteLocalRef(ju);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return JSValueMakeBoolean(ctx, false); }
    ALOGI("[open-url] %s -> %s", url.c_str(), ok ? "ok" : "fail");
    return JSValueMakeBoolean(ctx, ok == JNI_TRUE);
}
// ---- spinex 附件(≙ host_core.mm 的 Apple 版,仅 NSString→std::string) ----
static JSValueRef cbSpinexSetSlotAttachment(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    if (argc < 4) return JSValueMakeBoolean(ctx, false);
    const bool hasName = !(JSValueIsNull(ctx, args[3]) || JSValueIsUndefined(ctx, args[3]));
    const std::string name = hasName ? jsToStd(ctx, args[3]) : std::string();
    return JSValueMakeBoolean(ctx, gSpinexRt.setSlotAttachment((int)JSValueToNumber(ctx, args[0], exc), (int)JSValueToNumber(ctx, args[1], exc),
        (int)JSValueToNumber(ctx, args[2], exc), hasName ? name.c_str() : nullptr));
}
static JSValueRef cbSpinexUpdateAttachmentRegion(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    if (argc < 15) return JSValueMakeBoolean(ctx, false);
    const std::string name = jsToStd(ctx, args[2]);
    return JSValueMakeBoolean(ctx, gSpinexRt.updateAttachmentRegion(
        (int)JSValueToNumber(ctx, args[0], exc), (int)JSValueToNumber(ctx, args[1], exc), name.c_str(), (uint32_t)JSValueToNumber(ctx, args[3], exc),
        (float)JSValueToNumber(ctx, args[4], exc), (float)JSValueToNumber(ctx, args[5], exc), (float)JSValueToNumber(ctx, args[6], exc), (float)JSValueToNumber(ctx, args[7], exc),
        (int)JSValueToNumber(ctx, args[8], exc), (int)JSValueToNumber(ctx, args[9], exc), (int)JSValueToNumber(ctx, args[10], exc), (int)JSValueToNumber(ctx, args[11], exc),
        (int)JSValueToNumber(ctx, args[12], exc), (float)JSValueToNumber(ctx, args[13], exc), (float)JSValueToNumber(ctx, args[14], exc),
        argc > 15 && JSValueToBoolean(ctx, args[15])));
}
// ---- canvas2d(__c2d*):JS 垫层(game_host.js HTMLCanvasElement 2D context)→ Kotlin Canvas2D。参数面与 canvas2d_jsc.h 注释一致。 ----
namespace c2dbridge {
struct Call {
    JSContextRef ctx; const JSValueRef* a; size_t argc; JSValueRef* exc; JNIEnv* env;
    double num(size_t i, double def = 0) const { return i < argc ? JSValueToNumber(ctx, a[i], exc) : def; }
    float f(size_t i, double def = 0) const { const double v = num(i, def); return std::isfinite(v) ? (float)v : (float)def; }
    int i32(size_t i, int def = 0) const { const double v = num(i, def); return std::isfinite(v) ? (int)v : def; }
    jstring str(size_t i, const char* def = "") const { return jstr(env, i < argc && !JSValueIsUndefined(ctx, a[i]) && !JSValueIsNull(ctx, a[i]) ? jsToStd(ctx, a[i]) : std::string(def)); }
};
static bool ready(jmethodID mid) { return gActivity && mid; }
static bool clearException(JNIEnv* env) { if (env->ExceptionCheck()) { env->ExceptionClear(); return true; } return false; }
}
#define C2D_PROLOGUE(mid, fallback) JNIEnv* env = jniEnv(); if (!env || !c2dbridge::ready(mid)) return fallback; c2dbridge::Call call{ctx, args, argc, exc, env};
static JSValueRef cbC2dCreate(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    C2D_PROLOGUE(gMidC2dCreate, JSValueMakeNumber(ctx, -1));
    const jint id = env->CallIntMethod(gActivity, gMidC2dCreate, (jint)call.i32(0, 1), (jint)call.i32(1, 1));
    return JSValueMakeNumber(ctx, c2dbridge::clearException(env) ? -1 : id);
}
static JSValueRef cbC2dResize(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    C2D_PROLOGUE(gMidC2dResize, JSValueMakeUndefined(ctx));
    env->CallVoidMethod(gActivity, gMidC2dResize, (jint)call.i32(0), (jint)call.i32(1, 1), (jint)call.i32(2, 1)); c2dbridge::clearException(env); return JSValueMakeUndefined(ctx);
}
static JSValueRef cbC2dClearRect(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    C2D_PROLOGUE(gMidC2dClearRect, JSValueMakeUndefined(ctx));
    env->CallVoidMethod(gActivity, gMidC2dClearRect, (jint)call.i32(0), call.f(1), call.f(2), call.f(3), call.f(4)); c2dbridge::clearException(env); return JSValueMakeUndefined(ctx);
}
static JSValueRef cbC2dFillRect(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    C2D_PROLOGUE(gMidC2dFillRect, JSValueMakeUndefined(ctx));
    jstring color = call.str(5, "#000");
    env->CallVoidMethod(gActivity, gMidC2dFillRect, (jint)call.i32(0), call.f(1), call.f(2), call.f(3), call.f(4), color, call.f(6, 1));
    env->DeleteLocalRef(color); c2dbridge::clearException(env); return JSValueMakeUndefined(ctx);
}
static JSValueRef cbC2dFillText(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    C2D_PROLOGUE(gMidC2dFillText, JSValueMakeUndefined(ctx));
    jstring text = call.str(1), font = call.str(4, "10px sans-serif"), color = call.str(5, "#000"), baseline = call.str(6, "alphabetic"), align = call.str(8, "left"), shadow = call.str(9, "transparent");
    env->CallVoidMethod(gActivity, gMidC2dFillText, (jint)call.i32(0), text, call.f(2), call.f(3), font, color, baseline, call.f(7, 1), align, shadow, call.f(10), call.f(11), call.f(12));
    for (jstring j : { text, font, color, baseline, align, shadow }) env->DeleteLocalRef(j);
    c2dbridge::clearException(env); return JSValueMakeUndefined(ctx);
}
static JSValueRef cbC2dFillTextGrad(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    C2D_PROLOGUE(gMidC2dFillTextGrad, JSValueMakeUndefined(ctx));
    jstring text = call.str(1), font = call.str(4, "10px sans-serif"), stops = call.str(9, "[]"), baseline = call.str(10, "alphabetic"), align = call.str(12, "left"), shadow = call.str(13, "transparent");
    env->CallVoidMethod(gActivity, gMidC2dFillTextGrad, (jint)call.i32(0), text, call.f(2), call.f(3), font, call.f(5), call.f(6), call.f(7), call.f(8), stops, baseline, call.f(11, 1), align, shadow, call.f(14), call.f(15), call.f(16));
    for (jstring j : { text, font, stops, baseline, align, shadow }) env->DeleteLocalRef(j);
    c2dbridge::clearException(env); return JSValueMakeUndefined(ctx);
}
static JSValueRef cbC2dStrokeText(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    C2D_PROLOGUE(gMidC2dStrokeText, JSValueMakeUndefined(ctx));
    jstring text = call.str(1), font = call.str(4, "10px sans-serif"), color = call.str(5, "#000"), baseline = call.str(7, "alphabetic"), align = call.str(9, "left"), join = call.str(10, "miter"), shadow = call.str(12, "transparent");
    env->CallVoidMethod(gActivity, gMidC2dStrokeText, (jint)call.i32(0), text, call.f(2), call.f(3), font, color, call.f(6, 1), baseline, call.f(8, 1), align, join, call.f(11, 10), shadow, call.f(13), call.f(14), call.f(15));
    for (jstring j : { text, font, color, baseline, align, join, shadow }) env->DeleteLocalRef(j);
    c2dbridge::clearException(env); return JSValueMakeUndefined(ctx);
}
static JSValueRef cbC2dMeasure(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    C2D_PROLOGUE(gMidC2dMeasure, JSValueMakeNumber(ctx, 0));
    jstring text = call.str(1), font = call.str(2, "10px sans-serif");
    const jfloat w = env->CallFloatMethod(gActivity, gMidC2dMeasure, (jint)call.i32(0), text, font);
    env->DeleteLocalRef(text); env->DeleteLocalRef(font);
    return JSValueMakeNumber(ctx, c2dbridge::clearException(env) ? 0 : w);
}
static JSValueRef cbC2dGetData(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    C2D_PROLOGUE(gMidC2dGetData, JSValueMakeNull(ctx));
    auto arr = (jbyteArray)env->CallObjectMethod(gActivity, gMidC2dGetData, (jint)call.i32(0));
    if (c2dbridge::clearException(env) || !arr) return JSValueMakeNull(ctx);
    const jsize n = env->GetArrayLength(arr);
    void* buf = malloc((size_t)std::max<jsize>(n, 1));
    if (n) env->GetByteArrayRegion(arr, 0, n, reinterpret_cast<jbyte*>(buf));
    env->DeleteLocalRef(arr);
    return JSObjectMakeArrayBufferWithBytesNoCopy(ctx, buf, (size_t)n, [](void* p, void*) { free(p); }, nullptr, exc);
}
static JSValueRef cbC2dPutData(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    C2D_PROLOGUE(gMidC2dPutData, JSValueMakeUndefined(ctx));
    if (argc < 4 || !JSValueIsObject(ctx, args[1])) return JSValueMakeUndefined(ctx);
    JSObjectRef bufObj = JSValueToObject(ctx, args[1], exc); if (!bufObj) return JSValueMakeUndefined(ctx);
    const unsigned char* src = (const unsigned char*)JSObjectGetArrayBufferBytesPtr(ctx, bufObj, exc);
    const size_t srcLen = JSObjectGetArrayBufferByteLength(ctx, bufObj, exc);
    const int w = call.i32(2), h = call.i32(3);
    if (!src || w <= 0 || h <= 0 || srcLen < (size_t)w * h * 4) return JSValueMakeUndefined(ctx);
    jbyteArray arr = env->NewByteArray((jsize)((size_t)w * h * 4));
    env->SetByteArrayRegion(arr, 0, (jsize)((size_t)w * h * 4), reinterpret_cast<const jbyte*>(src));
    env->CallVoidMethod(gActivity, gMidC2dPutData, (jint)call.i32(0), arr, (jint)w, (jint)h, (jint)call.i32(4), (jint)call.i32(5));
    env->DeleteLocalRef(arr); c2dbridge::clearException(env); return JSValueMakeUndefined(ctx);
}
static JSValueRef cbC2dToDataURL(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    auto empty = [&]() { JSStringRef s = JSStringCreateWithUTF8CString("data:,"); JSValueRef v = JSValueMakeString(ctx, s); JSStringRelease(s); return v; };
    C2D_PROLOGUE(gMidC2dToDataURL, empty());
    jstring fmt = call.str(1, "image/png");
    auto js = (jstring)env->CallObjectMethod(gActivity, gMidC2dToDataURL, (jint)call.i32(0), fmt, call.f(2, 0.92));
    env->DeleteLocalRef(fmt);
    if (c2dbridge::clearException(env) || !js) return empty();
    const std::string url = jstringToStd(env, js); env->DeleteLocalRef(js);
    JSStringRef s = JSStringCreateWithUTF8CString(url.c_str()); JSValueRef v = JSValueMakeString(ctx, s); JSStringRelease(s); return v;
}
#define C2D_VOID1(NAME, MID) static JSValueRef NAME(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) { \
    C2D_PROLOGUE(MID, JSValueMakeUndefined(ctx)); env->CallVoidMethod(gActivity, MID, (jint)call.i32(0)); c2dbridge::clearException(env); return JSValueMakeUndefined(ctx); }
#define C2D_VOID3(NAME, MID) static JSValueRef NAME(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) { \
    C2D_PROLOGUE(MID, JSValueMakeUndefined(ctx)); env->CallVoidMethod(gActivity, MID, (jint)call.i32(0), call.f(1), call.f(2)); c2dbridge::clearException(env); return JSValueMakeUndefined(ctx); }
#define C2D_VOID7(NAME, MID) static JSValueRef NAME(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) { \
    C2D_PROLOGUE(MID, JSValueMakeUndefined(ctx)); env->CallVoidMethod(gActivity, MID, (jint)call.i32(0), call.f(1, 1), call.f(2), call.f(3), call.f(4, 1), call.f(5), call.f(6)); c2dbridge::clearException(env); return JSValueMakeUndefined(ctx); }
C2D_VOID1(cbC2dSave, gMidC2dSave)
C2D_VOID1(cbC2dRestore, gMidC2dRestore)
C2D_VOID1(cbC2dResetTransform, gMidC2dResetTransform)
C2D_VOID3(cbC2dScale, gMidC2dScale)
C2D_VOID3(cbC2dTranslate, gMidC2dTranslate)
C2D_VOID7(cbC2dTransform, gMidC2dTransform)
C2D_VOID7(cbC2dSetTransform, gMidC2dSetTransform)
static JSValueRef cbC2dRotate(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc) {
    C2D_PROLOGUE(gMidC2dRotate, JSValueMakeUndefined(ctx)); env->CallVoidMethod(gActivity, gMidC2dRotate, (jint)call.i32(0), call.f(1)); c2dbridge::clearException(env); return JSValueMakeUndefined(ctx);
}

struct H5AssetLookup {
    std::string url;
    bool preferRemote = false;
    bool currentBundledKtx2 = false;
};

// 远程兜底:JNI→Kotlin 共享 OkHttp 同步 GET(与 iOS fetchRemoteAsset 的同步语义一致,任意线程可调)。
// immutableVersioned:版本表映射出的不可变 URL(路径含内容版本目录)——先查 800 MiB 磁盘缓存,命中不看过期;网络拿到完整 200 才落盘。
// 版本表判定"远端更新"的文件每次冷启动都重下,真机单个 2s 上下、且经常阻塞 JS 线程。

static int b64Val(char c){ if(c>='A'&&c<='Z')return c-'A'; if(c>='a'&&c<='z')return c-'a'+26; if(c>='0'&&c<='9')return c-'0'+52; if(c=='+')return 62; if(c=='/')return 63; return -1; }
static macres::Bytes base64Decode(const std::string& in) {
    auto out = std::make_shared<std::vector<uint8_t>>();
    out->reserve(in.size() * 3 / 4);
    int acc = 0, bits = 0;
    for (char c : in) {
        int v = b64Val(c); if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out->push_back((uint8_t)((acc >> bits) & 0xff)); }
    }
    return out;
}
static macres::Bytes loadAsset(const char* path) {
 std::string requested=path ? path : "";
 if(requested.rfind("https://",0)==0||requested.rfind("http://",0)==0)return fetchRemoteAsset(requested);
 std::string rel=relOf(path ? path : "");
 if(rel.empty()||rel.find("../")!=std::string::npos)return nullptr;
 return bundledAssetData(rel);
}

// ---- stb 失败时的回退:Kotlin BitmapFactory(WebP 等)。输出与 decodeImageRaw 同约定:premultiplied RGBA8 top-down。
// 资产管线里存在以 .png 命名的 WebP 图集页(zspine 内 quan/zp_shou/lobby_jackpot 等);Apple 的 ImageIO 不看扩展名,Android 这里补齐。
static unsigned char* decodeImageViaKotlin(const uint8_t* bytes, size_t len, int* ow, int* oh,
                                          const std::string& url = {}, bool sourcePremultiplied = false) {
    JNIEnv* env = jniEnv(); if (!env || !gActivity || !gMidDecodeImageArgb || !bytes || !len) return nullptr;
    jbyteArray arr = env->NewByteArray((jsize)len); if (!arr) return nullptr;
    env->SetByteArrayRegion(arr, 0, (jsize)len, reinterpret_cast<const jbyte*>(bytes));
    jstring path = jstr(env, url);
    auto out = (jintArray)env->CallObjectMethod(gActivity, gMidDecodeImageArgb, arr, path);
    env->DeleteLocalRef(arr); env->DeleteLocalRef(path);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!out) return nullptr;
    const jsize n = env->GetArrayLength(out);
    if (n < 2) { env->DeleteLocalRef(out); return nullptr; }
    jint wh[2]; env->GetIntArrayRegion(out, 0, 2, wh);
    const int w = wh[0], h = wh[1];
    if (w <= 0 || h <= 0 || (jsize)((size_t)w * h + 2) > n || (uint64_t)w * (uint64_t)h > (16384ull * 16384ull)) { env->DeleteLocalRef(out); return nullptr; }
    std::vector<jint> argb((size_t)w * h); env->GetIntArrayRegion(out, 2, (jsize)((size_t)w * h), argb.data()); env->DeleteLocalRef(out);
    const size_t rowBytes = (size_t)w * 4; unsigned char* buf = (unsigned char*)malloc(rowBytes * (size_t)h); if (!buf) return nullptr;
    for (int y = 0; y < h; ++y) {
        unsigned char* drow = buf + (size_t)y * rowBytes;
        for (int x = 0; x < w; ++x) {
            const uint32_t p = (uint32_t)argb[(size_t)y * w + x]; const unsigned a = (p >> 24) & 0xff, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
            const uint8_t rgba[4]={(uint8_t)r,(uint8_t)g,(uint8_t)b,(uint8_t)a};
            nativeimage::copyPremultipliedRGBA(drow+x*4,rgba,1,sourcePremultiplied);
        }
    }
    *ow = w; *oh = h; return buf;
}
// ---- 图片解码:png/jpg/webp 字节 → premultiplied RGBA8 top-down(行 0 = 图顶,与 mac/iOS 的 CGBitmapContext 内存同向;WebGL 无 UNPACK_FLIP_Y 时同此) ----
static unsigned char* decodeImageRaw(const uint8_t* bytes, size_t len, int* ow, int* oh, bool sourcePremultiplied = false) {
    if (!bytes || !len) return nullptr;
    int w = 0, h = 0, comp = 0;
    stbi_uc* src = stbi_load_from_memory(bytes, (int)len, &w, &h, &comp, 4);
    if (!src) return decodeImageViaKotlin(bytes, len, ow, oh, {}, sourcePremultiplied);   // WebP 等
    if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > (16384ull * 16384ull)) { stbi_image_free(src); return nullptr; }
    const size_t rowBytes = (size_t)w * 4;
    unsigned char* buf = (unsigned char*)malloc(rowBytes * (size_t)h);
    if (!buf) { stbi_image_free(src); return nullptr; }
    nativeimage::copyPremultipliedRGBA(buf, src, (size_t)w*h, sourcePremultiplied);
    stbi_image_free(src);
    if (ow) *ow = w; if (oh) *oh = h;
    return buf;
}
static JSValueRef decodeImageData(JSContextRef ctx, const macres::Bytes& d, JSValueRef* exc, bool sourcePremultiplied = false) {
    int w = 0, h = 0;
    unsigned char* buf = d ? decodeImageRaw(d->data(), d->size(), &w, &h, sourcePremultiplied) : nullptr;
    if (!buf) return JSValueMakeNull(ctx);
    JSObjectRef abuf = JSObjectMakeArrayBufferWithBytesNoCopy(ctx, buf, (size_t)w*h*4, [](void* p, void*){ free(p); }, nullptr, exc);
    JSObjectRef obj = JSObjectMake(ctx, nullptr, nullptr);
    auto setp = [&](const char* k, JSValueRef v){ JSStringRef s = JSStringCreateWithUTF8CString(k); JSObjectSetProperty(ctx, obj, s, v, kJSPropertyAttributeNone, nullptr); JSStringRelease(s); };
    setp("width", JSValueMakeNumber(ctx, w)); setp("height", JSValueMakeNumber(ctx, h)); setp("data", abuf);
    return obj;
}

// ======================= JSC 桥接辅助(对齐 ios L986-1039;NSString/NSData → std) =======================
static double gFrameDecodeUs = 0;   // 本帧 native 解码/GL 提交耗时(累计各次 flush,宿主每帧清零)
static double gFrameJSUs = 0, gFrameComputeUs = 0, gFrameBatchUs = 0, gFramePresentUs = 0;
static double gFramePreJSUs = 0;
static bool gFrameHadCmds = false;   // 本帧是否有 GL 命令(静止帧跳过时不 present,保持屏幕)
static bool gFramePresented = false;
static bool gFrameHadContent = false;

static std::string jsToStd(JSContextRef ctx, JSValueRef v) {
    if (!v) return {};
    JSStringRef s = JSValueToStringCopy(ctx, v, nullptr);
    std::string out = s ? s->s : std::string();
    JSStringRelease(s);
    return out;
}
static std::string jsErrorDetail(JSContextRef ctx, JSValueRef value) {
    const std::string message = jsToStd(ctx, value);
    if (!value || !JSValueIsObject(ctx, value)) return message;
    JSStringRef key = JSStringCreateWithUTF8CString("stack");
    JSValueRef propertyError = nullptr;
    JSValueRef stackValue = JSObjectGetProperty(ctx, JSValueToObject(ctx, value, nullptr), key, &propertyError);
    JSStringRelease(key);
    if (propertyError || !stackValue || !JSValueIsString(ctx, stackValue)) return message;
    const std::string stack = jsToStd(ctx, stackValue);
    return stack.empty() ? message : (stack.find(message) == 0 ? stack : message + "\n" + stack);
}
// string / ArrayBuffer / TypedArray → 字节副本(≙ jsValueToData)
static macres::Bytes jsToBytes(JSContextRef ctx, JSValueRef v, JSValueRef* exc) {
    if (!v) return nullptr;
    if (JSValueIsString(ctx, v)) {
        std::string s = jsToStd(ctx, v);
        return std::make_shared<std::vector<uint8_t>>(s.begin(), s.end());
    }
    if (!JSValueIsObject(ctx, v)) return nullptr;
    JSObjectRef obj = JSValueToObject(ctx, v, exc);
    JSTypedArrayType kind = JSValueGetTypedArrayType(ctx, v, exc);
    if (kind == kJSTypedArrayTypeArrayBuffer) {
        void* p = JSObjectGetArrayBufferBytesPtr(ctx, obj, exc);
        size_t n = JSObjectGetArrayBufferByteLength(ctx, obj, exc);
        if (!p) return nullptr;
        return std::make_shared<std::vector<uint8_t>>((uint8_t*)p, (uint8_t*)p + n);
    }
    if (kind != kJSTypedArrayTypeNone) {
        void* p = JSObjectGetTypedArrayBytesPtr(ctx, obj, exc);
        size_t n = JSObjectGetTypedArrayByteLength(ctx, obj, exc);
        if (!p) return nullptr;
        return std::make_shared<std::vector<uint8_t>>((uint8_t*)p, (uint8_t*)p + n);
    }
    return nullptr;
}
static void callJsGlobal(const char* fname, int argc, const JSValueRef* args) {   // JS 线程调用 JS 全局函数
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef nm = JSStringCreateWithUTF8CString(fname);
    JSValueRef fv = JSObjectGetProperty(gCtx, global, nm, nullptr); JSStringRelease(nm);
    if (!JSValueIsObject(gCtx, fv)) return;
    JSObjectRef fn = JSValueToObject(gCtx, fv, nullptr);
    if (!fn || !JSObjectIsFunction(gCtx, fn)) return;
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(gCtx, fn, nullptr, argc, args, &exc);
    if (exc) { std::string es = jsErrorDetail(gCtx, exc); ALOGE("[async-load] %s 回调异常: %s", fname, es.c_str()); reportRuntimeError("javascript_runtime", "callback_exception", es, false); }
}
// 送达真值版:返回"目标函数存在且完成了调用"。携带所有权引用的回调(publish 的 cid)必须用它。
static bool callJsGlobalChecked(const char* fname, int argc, const JSValueRef* args) {
    if (!gCtx) return false;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef nm = JSStringCreateWithUTF8CString(fname);
    JSValueRef fv = JSObjectGetProperty(gCtx, global, nm, nullptr); JSStringRelease(nm);
    if (!JSValueIsObject(gCtx, fv)) return false;
    JSObjectRef fn = JSValueToObject(gCtx, fv, nullptr);
    if (!fn || !JSObjectIsFunction(gCtx, fn)) return false;
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(gCtx, fn, nullptr, argc, args, &exc);
    if (exc) { static int excN = 0; if (++excN <= 8) ALOGE("[async-load] %s 回调抛异常(所有权按已送达处理,第 %d 次)", fname, excN); }
    return true;
}

// ======================= 原生文本系统(对齐 ios L1043-1313;CoreText → Kotlin StaticLayout/Paint) =======================
// JS 只提供样式串与几何;断行/测量/字形绘制在 Kotlin(android.graphics),位图经 AndroidBitmap_lockPixels
// 零拷贝直上 GL 纹理(top-down,与 CoreText 面同向,UV {0,0,1,0,1,1,0,1} 不变)。缓存 key/sg 槽位写入与 iOS 逐字同源。
class NativeTextSystem {
    struct Entry {
        GLuint gl=0; uint32_t cid=0; std::string key;
        std::string richRequestKey; double richWidth=0,richHeight=0,richLineHeight=0;
        double richContentX=0,richContentY=0,richContentWidth=0,richContentHeight=0;             // 语义内容盒(含内联图,owner 坐标)
        double richTextContentX=0,richTextContentY=0,richTextContentWidth=0,richTextContentHeight=0; // 纯文字内容盒(metrics ABI 3)
        double richSurfaceOriginX=0,richSurfaceOriginY=0; // compact/overflow raster placement
        double richAnchorWidth=0; // independent from raster width for dynamic MultiStyleText
        std::vector<sg::NativeQuad> richQuads;
    };
    std::unordered_map<int, Entry> nodes_;
    uint32_t nextCid_ = 8000000; // exactly representable in scenegraph Float32 and clear of WebGL ids
    // ---- 富文本 <img> 内联图(与 mac/iOS 同源):纹理/uv 权威在 C++(Kotlin 拿不到图集像素),
    // Kotlin 只负责占位留白与位置回传,图作为 native quad 叠加在文本纹理之上。 ----
    static std::string lowerStr(std::string s){for(char& c:s)c=(char)std::tolower((unsigned char)c);return s;}
    static std::string tagAttr(const std::string& source,const std::string& wanted){
        std::string lo=lowerStr(source),key=lowerStr(wanted);size_t p=0;
        while((p=lo.find(key,p))!=std::string::npos){if(p>0&&(std::isalnum((unsigned char)lo[p-1])||lo[p-1]=='-'||lo[p-1]=='_')){p+=key.size();continue;}size_t q=p+key.size();while(q<lo.size()&&std::isspace((unsigned char)lo[q]))q++;if(q>=lo.size()||lo[q]!='='){p=q;continue;}q++;while(q<source.size()&&std::isspace((unsigned char)source[q]))q++;if(q>=source.size())return {};char quote=source[q];if(quote=='\''||quote=='"'){size_t e=source.find(quote,q+1);return e==std::string::npos?source.substr(q+1):source.substr(q+1,e-q-1);}size_t e=q;while(e<source.size()&&!std::isspace((unsigned char)source[e])&&source[e]!='>')e++;return source.substr(q,e-q);}
        return {};
    }
    static std::string richImageSignature(int node,const std::string& markup){
        // Include the live atlas descriptor in the raster cache. Rebinding/reinstalling a package
        // can keep the same ui:// URL while changing its texture id or frame.
        std::string signature,lo=lowerStr(markup);size_t pos=0;
        while((pos=lo.find("<img",pos))!=std::string::npos){size_t gt=markup.find('>',pos+4);if(gt==std::string::npos)break;std::string body=markup.substr(pos+1,gt-pos-1),src=tagAttr(body,"src");if(src.empty())src=tagAttr(body,"imgSrc");nfgui::InlineImageDescriptor image;
            signature.push_back('|');signature+=src;if(richImageDescriptor(node,body,image)){signature+=':';signature+=std::to_string(image.textureId)+':'+std::to_string(image.width)+'x'+std::to_string(image.height)+':'+std::to_string(image.premul);for(float uv:image.uv){signature.push_back(',');signature+=std::to_string(uv);}}else signature+="!missing";pos=gt+1;}
        return signature;
    }
    static bool richImageDescriptor(int node,const std::string& body,nfgui::InlineImageDescriptor& image){
        std::string src=tagAttr(body,"src");if(src.empty())src=tagAttr(body,"imgSrc");if(!src.empty()&&gNativeFgui.inlineImageForNode(node,src,image))return true;
        int texture=std::atoi(tagAttr(body,"nativeTexture").c_str());float width=std::strtof(tagAttr(body,"nativeWidth").c_str(),nullptr),height=std::strtof(tagAttr(body,"nativeHeight").c_str(),nullptr);
        std::string encoded=tagAttr(body,"nativeUv");if(texture<=0||width<=0||height<=0||encoded.empty())return false;
        const char* cursor=encoded.c_str();char* end=nullptr;float uv[8];for(int i=0;i<8;i++){uv[i]=std::strtof(cursor,&end);if(end==cursor)return false;cursor=end;if(i<7){if(*cursor!=',')return false;cursor++;}}
        image={};image.textureId=texture;image.width=width;image.height=height;image.premul=std::strtof(tagAttr(body,"nativePremul").c_str(),nullptr);for(int i=0;i<8;i++)image.uv[i]=uv[i];return true;
    }
    static void publishRich(int node,const Entry& en,float* s,double width,double height,double anchorX,double anchorY){
        const float ox=(float)(en.richSurfaceOriginX-anchorX*en.richAnchorWidth),oy=(float)(en.richSurfaceOriginY-anchorY*height);
        if(!en.richQuads.empty()){std::vector<sg::NativeQuad> quads=en.richQuads;for(auto& q:quads)for(int p=0;p<4;p++){q[1+p*2]+=ox;q[2+p*2]+=oy;}gSG.setNativeQuads(node,std::move(quads));s[sg::TEXID]=0;}
        else{gSG.clearNativeQuads(node);s[sg::TYPE]=sg::T_SPRITE;s[sg::TEXID]=(float)en.cid;s[sg::RX]=ox;s[sg::RY]=oy;const float uv[8]={0,0,1,0,1,1,0,1};for(int i=0;i<8;i++)s[sg::U0+i]=uv[i];s[sg::PREMUL]=0;}
        s[sg::RW]=(float)width;s[sg::RH]=(float)height;s[sg::TINT]=(float)0xFFFFFF;s[sg::RENDERABLE]=1;gSG.markBoundsDirty(node);
    }
    // JSON 字符串转义(spec 传 Kotlin)
    static void jsonEsc(std::string& out, const std::string& s) {
        out.push_back('"');
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                    else out.push_back(c);
            }
        }
        out.push_back('"');
    }
    static void jsonKV(std::string& out, const char* k, const std::string& v){ out += '"'; out += k; out += "\":"; jsonEsc(out, v); out += ','; }
    static void jsonKV(std::string& out, const char* k, double v){ char b[64]; snprintf(b, sizeof b, "\"%s\":%.6f,", k, v); out += b; }
    // Kotlin 测量:outMetrics=[width,height,lineHeight,glyphTopOffset];失败返回 false
    static bool jniMeasure(const std::string& spec, double out[4]) {
        JNIEnv* env = jniEnv();
        if (!env || !gActivity || !gMidTextMeasure) return false;
        jstring js = jstr(env, spec);
        jfloatArray arr = env->NewFloatArray(4);
        if (!arr) { if (env->ExceptionCheck()) env->ExceptionClear(); env->DeleteLocalRef(js); return false; }   // OOM:带 pending 异常再调 JNI 是非法(CheckJNI abort)
        jboolean ok = env->CallBooleanMethod(gActivity, gMidTextMeasure, js, arr);
        env->DeleteLocalRef(js);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(arr); return false; }
        jfloat vals[4] = {0,0,0,0};
        env->GetFloatArrayRegion(arr, 0, 4, vals);
        env->DeleteLocalRef(arr);
        if (!ok) return false;
        for (int i = 0; i < 4; i++) out[i] = vals[i];
        return true;
    }
    // Bitmap(ARGB_8888,top-down,premultiplied)→ 该节点的外部 GL 纹理(首次分配 cid 并登记到解码器)。
    bool uploadBitmap(Entry& en, JNIEnv* env, jobject bmp, int pw, int ph) {
        AndroidBitmapInfo info{};
        void* pixels = nullptr;
        bool ok = AndroidBitmap_getInfo(env, bmp, &info) == ANDROID_BITMAP_RESULT_SUCCESS
               && info.format == ANDROID_BITMAP_FORMAT_RGBA_8888
               && (int)info.width == pw && (int)info.height == ph
               && AndroidBitmap_lockPixels(env, bmp, &pixels) == ANDROID_BITMAP_RESULT_SUCCESS;
        if (!ok) return false;
        GLint old = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
        if (!en.gl) { glGenTextures(1, &en.gl); en.cid = nextCid_++; gDec.registerExternalTexture(en.cid, en.gl); }
        glBindTexture(GL_TEXTURE_2D, en.gl); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (info.stride == info.width * 4) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        } else {   // stride 不紧凑:逐行压紧(Bitmap 一般紧凑,兜底)
            std::vector<uint8_t> tight((size_t)pw * ph * 4);
            for (int y = 0; y < ph; ++y)
                memcpy(tight.data() + (size_t)y * pw * 4, (uint8_t*)pixels + (size_t)y * info.stride, (size_t)pw * 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, tight.data());
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, (GLuint)old);
        AndroidBitmap_unlockPixels(env, bmp);
        return true;
    }
    // token 级富文本度量(≙ host_core.h updateRich 的 CTLine 逐 token 测量):Kotlin 解析 markup、切 token、
    // 逐 token 测量;返回 [naturalLineHeight, baseAscent, n] + n×12
    // [kind(0 词/1 空白/2 换行/3 图),nakedWidth,graphemes,ascent,descent,naturalLineHeight,kern,strokeWidth(可见描边时),block,imageIndex,imageHeight,placementAscent]。
    static bool jniRichTokens(const std::string& spec, std::vector<float>& out) {
        JNIEnv* env = jniEnv();
        if (!env || !gActivity || !gMidRichTokens) return false;
        jstring js = jstr(env, spec);
        auto arr = (jfloatArray)env->CallObjectMethod(gActivity, gMidRichTokens, js);
        env->DeleteLocalRef(js);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!arr) return false;
        const jsize n = env->GetArrayLength(arr);
        out.resize((size_t)std::max<jsize>(0, n));
        if (n > 0) env->GetFloatArrayRegion(arr, 0, n, out.data());
        env->DeleteLocalRef(arr);
        return n >= 3;
    }
    // 按 C++ 排版落点(每 token [x, baselineY] 逻辑坐标)让 Kotlin 绘制 token 并上传。
    bool jniRichRasterUpload(Entry& en, const std::string& spec, int pw, int ph, double resolution, const std::vector<float>& placements) {
        JNIEnv* env = jniEnv();
        if (!env || !gActivity || !gMidRichRaster) return false;
        jstring js = jstr(env, spec);
        jfloatArray pl = env->NewFloatArray((jsize)placements.size());
        if (!pl) { if (env->ExceptionCheck()) env->ExceptionClear(); env->DeleteLocalRef(js); return false; }
        if (!placements.empty()) env->SetFloatArrayRegion(pl, 0, (jsize)placements.size(), placements.data());
        jobject bmp = env->CallObjectMethod(gActivity, gMidRichRaster, js, pw, ph, (jfloat)resolution, pl);
        env->DeleteLocalRef(js); env->DeleteLocalRef(pl);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!bmp) return false;
        const bool ok = uploadBitmap(en, env, bmp, pw, ph);
        env->DeleteLocalRef(bmp);
        return ok;
    }
    // Kotlin 栅格化 → Bitmap → glTexImage2D。surfaceW/H 是逻辑尺寸,pw/ph 是像素尺寸。
    // imgCount>0 时附带 imgCount*4 的位置数组([x,y,w,h] 逻辑坐标,缺席槽 -1),Kotlin 填每个
    // <img> 占位在布局里的落点,C++ 据此组 native quad(与 mac richImageQuads 同一坐标语义)。
    bool jniRasterUpload(Entry& en, const std::string& spec, int pw, int ph, double resolution,
                         double surfaceW, double surfaceH, double naturalH,
                         int imgCount = 0, std::vector<float>* imgPlacements = nullptr) {
        JNIEnv* env = jniEnv();
        if (!env || !gActivity || !gMidTextRaster) return false;
        jstring js = jstr(env, spec);
        jfloatArray imgArr = nullptr;
        if (imgCount > 0) {
            imgArr = env->NewFloatArray(imgCount * 4);
            if (!imgArr) { if (env->ExceptionCheck()) env->ExceptionClear(); }   // OOM 降级:不带位置数组继续(图占位不回传,文本照常;pending 异常必须先清)
            else { std::vector<jfloat> init((size_t)imgCount * 4, -1.0f); env->SetFloatArrayRegion(imgArr, 0, imgCount * 4, init.data()); }
        }
        jobject bmp = env->CallObjectMethod(gActivity, gMidTextRaster, js, pw, ph,
                                            (jfloat)resolution, (jfloat)surfaceW, (jfloat)surfaceH, (jfloat)naturalH, imgArr);
        env->DeleteLocalRef(js);
        if (env->ExceptionCheck()) { env->ExceptionClear(); if (imgArr) env->DeleteLocalRef(imgArr); return false; }
        if (!bmp) { if (imgArr) env->DeleteLocalRef(imgArr); return false; }
        const bool ok = uploadBitmap(en, env, bmp, pw, ph);
        if (ok && imgArr && imgPlacements) {
            imgPlacements->resize((size_t)imgCount * 4);
            env->GetFloatArrayRegion(imgArr, 0, imgCount * 4, imgPlacements->data());
        }
        if (imgArr) env->DeleteLocalRef(imgArr);
        env->DeleteLocalRef(bmp);
        return ok;
    }
public:
    ~NativeTextSystem(){ clear(); }
    void clear(){ for(auto& kv:nodes_) release(kv.second); nodes_.clear(); }
    bool hasEntry(int node) const { return nodes_.find(node)!=nodes_.end(); }
    double richAnchorWidth(int node) const { auto it=nodes_.find(node);return it==nodes_.end()?0:it->second.richAnchorWidth; }
    void detach(int node){ auto it=nodes_.find(node);if(it==nodes_.end())return;release(it->second);nodes_.erase(it); }
    void clearFresh(int node){
        detach(node);if(node<=0||node>=sg::NODE_CAP||!gSGBuf)return;
        gSG.clearNativeQuads(node);float* s=gSGBuf+(size_t)node*sg::STRIDE;
        s[sg::TYPE]=sg::T_CONTAINER;s[sg::TEXID]=0;s[sg::RX]=s[sg::RY]=s[sg::RW]=s[sg::RH]=0;
        s[sg::QUADCNT]=0;s[sg::PREMUL]=0;s[sg::TINT]=(float)0xFFFFFF;gSG.markBoundsDirty(node);
    }
    bool update(int node,const std::string& text,const std::string& font,const std::string& fill,
                const std::string& stroke,double strokeWidth,const std::string& shadow,double shadowBlur,
                double shadowX,double shadowY,const std::string& align,double padding,double resolution,
                double anchorX,double anchorY,double wrapWidth,double requestedLineHeight,
                double boxWidth,double boxHeight,const std::string& verticalAlign,double letterSpacing,double leading,
                const std::string& fillGradientColors,const std::string& fillGradientStops,int fillGradientType,
                double pixiAscent,double pixiDescent,
                double& width,double& height,double& lineHeight){
        if(node<=0||node>=sg::NODE_CAP||!gSGBuf)return false;
        float* s=gSGBuf+(size_t)node*sg::STRIDE;
        // spec 一次构造,measure 与 raster 共用(Kotlin 侧两次断行结果确定一致)
        std::string spec = "{\"kind\":\"plain\",";
        jsonKV(spec,"text",text);jsonKV(spec,"font",font);jsonKV(spec,"fill",fill);jsonKV(spec,"stroke",stroke);
        jsonKV(spec,"strokeWidth",strokeWidth);jsonKV(spec,"shadow",shadow);jsonKV(spec,"shadowBlur",shadowBlur);
        jsonKV(spec,"shadowX",shadowX);jsonKV(spec,"shadowY",shadowY);jsonKV(spec,"align",align);
        jsonKV(spec,"padding",std::max(0.0,padding));jsonKV(spec,"wrapWidth",wrapWidth);
        jsonKV(spec,"requestedLineHeight",requestedLineHeight);jsonKV(spec,"verticalAlign",verticalAlign);
        jsonKV(spec,"letterSpacing",letterSpacing);jsonKV(spec,"leading",leading);
        jsonKV(spec,"pixiAscent",pixiAscent);jsonKV(spec,"pixiDescent",pixiDescent);
        jsonKV(spec,"gradientColors",fillGradientColors);jsonKV(spec,"gradientStops",fillGradientStops);
        jsonKV(spec,"gradientType",(double)fillGradientType);
        spec += "\"end\":0}";
        double m[4] = {0,0,0,0};
        if(!jniMeasure(spec,m))return false;
        width=m[0];height=m[1];lineHeight=m[2];
        double naturalHeight=height;padding=std::max(0.0,padding);
        // ≙ host_core.h update()/H5 TextMetrics.measureText:dropShadow 开启时 +distance,canvas=ceil(width+2pad);
        // 扩边全落在右/下,行对齐基准=surface=max(natural,box)(box 双态天然覆盖 owner-box/natural 两种 frame)。
        const double shadowDist=(shadowX!=0||shadowY!=0)?std::hypot(shadowX,shadowY):0.0;
        width=std::max(std::ceil(width+shadowDist+padding*2),boxWidth);height=std::max(std::ceil(height+shadowDist+padding*2),boxHeight);
        if(text.empty()||width<=0||height<=0){detach(node);s[sg::TYPE]=sg::T_CONTAINER;s[sg::TEXID]=0;return true;}
        resolution=std::max(1.0,std::min(3.0,resolution));
        if(!std::isfinite(width)||!std::isfinite(height)||width>16384||height>16384)return false;
        int pw=std::max(1,(int)ceil(width*resolution)),ph=std::max(1,(int)ceil(height*resolution));
        GLint maxTexture=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maxTexture);
        if(pw>maxTexture||ph>maxTexture||(uint64_t)pw*(uint64_t)ph>64u*1024u*1024u)return false;
        // cache key:所有影响像素或 sprite 描述的输入(与 iOS 逐字同源)
        std::string key=text;
        auto keyPart=[&](const std::string& value){key.push_back('\x1f');key+=value;};
        keyPart(font);keyPart(fill);keyPart(stroke);keyPart(shadow);keyPart(align);keyPart(verticalAlign);
        keyPart(fillGradientColors);keyPart(fillGradientStops);keyPart(std::to_string(fillGradientType));
        keyPart(std::to_string(pw));keyPart(std::to_string(ph));keyPart(std::to_string(strokeWidth));
        keyPart(std::to_string(shadowBlur));keyPart(std::to_string(shadowX));keyPart(std::to_string(shadowY));
        keyPart(std::to_string(padding));keyPart(std::to_string(resolution));keyPart(std::to_string(wrapWidth));
        keyPart(std::to_string(requestedLineHeight));
        keyPart(std::to_string(letterSpacing));keyPart(std::to_string(leading));
        keyPart(std::to_string(pixiAscent));keyPart(std::to_string(pixiDescent));
        keyPart(std::to_string(boxWidth));keyPart(std::to_string(boxHeight));keyPart(std::to_string(width));
        keyPart(std::to_string(height));keyPart(std::to_string(lineHeight));
        Entry& en=nodes_[node];
        // A node may switch between the ordinary and attributed text surfaces. Do not let an
        // earlier rich request masquerade as a hit after the ordinary texture replaced it.
        en.richRequestKey.clear();en.richQuads.clear();gSG.clearNativeQuads(node);
        if(en.key!=key){
            if(!jniRasterUpload(en,spec,pw,ph,resolution,width,height,naturalHeight))return false;
            en.key=key;
        }
        s[sg::TYPE]=sg::T_SPRITE;s[sg::TEXID]=(float)en.cid;s[sg::RX]=(float)(-anchorX*width);s[sg::RY]=(float)(-anchorY*height);
        s[sg::RW]=(float)width;s[sg::RH]=(float)height;
        const float uv[8]={0,0,1,0,1,1,0,1};for(int i=0;i<8;i++)s[sg::U0+i]=uv[i];
        s[sg::TINT]=(float)0xFFFFFF;s[sg::PREMUL]=0;s[sg::RENDERABLE]=1;gSG.markBoundsDirty(node);return true;
    }
    // horizontalFrame(三宽度布局协议,docs/text-layout-contract-v2.md 第一部分;与 host_core.h 同语义):
    //   0=legacy(V1:对齐基准=surface=max(natural,box)) 1=owner-box 2=paragraph-box 3=natural 4=tagged-content。
    // 排版权威在 C++(richtext_token_layout.h,与 mac/iOS 同一份 oracle 语义);Kotlin 只做 markup 解析、
    // 逐 token 测量(≙ CTLineGetTypographicBounds)与按落点绘制(≙ CTLineDraw),不再用 StaticLayout 二次断行。
    bool updateRich(int node,const std::string& markup,const std::string& font,const std::string& fill,
                    const std::string& stroke,double strokeWidth,const std::string& align,double resolution,
                    double anchorX,double anchorY,double wrapWidth,double requestedLineHeight,
                    double boxWidth,double boxHeight,const std::string& verticalAlign,
                    double& width,double& height,double& lineHeight,int horizontalFrame=0,bool breakWords=false,
                    double* contentWidthOut=nullptr,double* contentXOut=nullptr,
                    double* contentHeightOut=nullptr,double* contentYOut=nullptr,
                    double* textContentWidthOut=nullptr,double* textContentXOut=nullptr,
                    double* textContentHeightOut=nullptr,double* textContentYOut=nullptr,
                    double lineVerticalOffset=0,double verticalCenterHeight=0){
        if(node<=0||node>=sg::NODE_CAP||!gSGBuf)return false;
        float* s=gSGBuf+(size_t)node*sg::STRIDE;
        resolution=std::max(1.0,std::min(3.0,resolution));
        std::string requestKey=markup+'\x1f'+font+'\x1f'+fill+'\x1f'+stroke+'\x1f'+align+"|rich-request|"
            +std::to_string(strokeWidth)+'|'+std::to_string(resolution)+'|'+std::to_string(wrapWidth)+'|'
            +std::to_string(requestedLineHeight)+'|'+std::to_string(boxWidth)+'x'+std::to_string(boxHeight)+'|'+verticalAlign
            +"|hf"+std::to_string(horizontalFrame)+(breakWords?"|bw":"")
            +"|lineY"+std::to_string(lineVerticalOffset)+"|centerY"+std::to_string(verticalCenterHeight)
            +'|'+richImageSignature(node,markup);
        Entry& en=nodes_[node];
        auto emitOuts=[&](){
            if(contentWidthOut)*contentWidthOut=en.richContentWidth;if(contentXOut)*contentXOut=en.richContentX;
            if(contentHeightOut)*contentHeightOut=en.richContentHeight;if(contentYOut)*contentYOut=en.richContentY;
            if(textContentWidthOut)*textContentWidthOut=en.richTextContentWidth;if(textContentXOut)*textContentXOut=en.richTextContentX;
            if(textContentHeightOut)*textContentHeightOut=en.richTextContentHeight;if(textContentYOut)*textContentYOut=en.richTextContentY;
        };
        if(en.gl&&en.richRequestKey==requestKey){
            width=en.richWidth;height=en.richHeight;lineHeight=en.richLineHeight;
            emitOuts();publishRich(node,en,s,width,height,anchorX,anchorY);return true;
        }
        if(markup.empty()){detach(node);gSG.clearNativeQuads(node);s[sg::TYPE]=sg::T_CONTAINER;s[sg::TEXID]=0;width=height=lineHeight=0;
            Entry zero{};en.richContentX=en.richContentY=en.richContentWidth=en.richContentHeight=0;en.richTextContentX=en.richTextContentY=en.richTextContentWidth=en.richTextContentHeight=0;(void)zero;
            emitOuts();return true;}
        // <img> 的图集描述符只有 C++ 能解析(inlineImageForNode),按出现顺序注入 spec:Kotlin 据此算占位度量。
        std::vector<nfgui::InlineImageDescriptor> imgDescs;std::vector<char> imgOk;
        {std::string lo=lowerStr(markup);size_t pos=0;
         while((pos=lo.find("<img",pos))!=std::string::npos){
             size_t after=pos+4;
             if(after<lo.size()&&(std::isalnum((unsigned char)lo[after])||lo[after]=='-'||lo[after]=='_')){pos=after;continue;}
             size_t gt=markup.find('>',pos+4);if(gt==std::string::npos)break;std::string body=markup.substr(pos+1,gt-pos-1);
             nfgui::InlineImageDescriptor image;bool ok=richImageDescriptor(node,body,image);imgDescs.push_back(image);imgOk.push_back(ok?1:0);pos=gt+1;}}
        std::string spec = "{\"kind\":\"rich\",";
        jsonKV(spec,"markup",markup);jsonKV(spec,"font",font);jsonKV(spec,"fill",fill);jsonKV(spec,"stroke",stroke);
        jsonKV(spec,"strokeWidth",strokeWidth);jsonKV(spec,"align",align);jsonKV(spec,"wrapWidth",wrapWidth);
        jsonKV(spec,"requestedLineHeight",requestedLineHeight);jsonKV(spec,"verticalAlign",verticalAlign);
        if(!imgDescs.empty()){spec+="\"images\":[";
            for(size_t i=0;i<imgDescs.size();++i){if(i)spec+=',';if(!imgOk[i]){spec+="null";continue;}
                char b[128];snprintf(b,sizeof b,"{\"width\":%.3f,\"height\":%.3f}",imgDescs[i].width,imgDescs[i].height);spec+=b;}
            spec+="],";}
        spec += "\"end\":0}";
        std::vector<float> tk;
        if(!jniRichTokens(spec,tk))return false;
        const double naturalLineHeight=tk[0],baseAscent=tk[1];const size_t n=(size_t)std::max(0.0f,tk[2]);
        if(tk.size()<3+n*12)return false;
        lineHeight=naturalLineHeight;
        if(requestedLineHeight>0)lineHeight=std::max(naturalLineHeight,requestedLineHeight);
        struct TokenImage{int index=-1;float height=0,placementAscent=0;};
        std::vector<richtext::TokenInput> inputs(n);std::vector<TokenImage> images(n);
        for(size_t k=0;k<n;++k){
            const float* f=&tk[3+k*12];richtext::TokenInput& in=inputs[k];const int kind=(int)f[0];
            in.kind=kind==3?richtext::TokenKind::Image:kind==2?richtext::TokenKind::Newline:kind==1?richtext::TokenKind::Whitespace:richtext::TokenKind::Word;
            in.nakedWidth=f[1];in.graphemeCount=std::max(1,(int)f[2]);in.ascent=f[3];in.descent=f[4];in.naturalLineHeight=f[5];
            if(in.kind==richtext::TokenKind::Image){in.blockImage=f[8]>0.5f;images[k].index=(int)f[9];images[k].height=f[10];images[k].placementAscent=f[11];}
            else if(in.kind==richtext::TokenKind::Newline){in.naturalLineHeight=0;}
            else{in.letterSpacing=richtext::tokenInternalGapKern(f[6]);
                 in.strokeAdvance=richtext::tokenStrokeAdvanceCorrection(f[7],in.kind==richtext::TokenKind::Whitespace,f[7]>0);
                 // default style 级 breakWords(业务唯一用法,zh-TW):相连 segment 词逐个成词、可在 segment 边界断行。
                 in.breakWords=breakWords&&in.kind==richtext::TokenKind::Word;}
        }
        // 断行仅由 wordWrapWidth 约束(FGUI box 只定 surface/对齐,不参与断行——历史契约保留)。
        richtext::TokenLayoutResult laid=richtext::layoutTokens(inputs,wrapWidth,std::numeric_limits<double>::infinity(),richtext::TokenAlign::Left,requestedLineHeight,naturalLineHeight,baseAscent);
        const double surfaceOriginY=richtext::applyLineVerticalCorrection(laid,lineVerticalOffset,verticalCenterHeight,&inputs);
        const double naturalW=std::ceil(laid.naturalWidth);
        width=naturalW;height=std::ceil(laid.naturalHeight);double naturalHeight=height;width=std::max(width,boxWidth);height=std::max(height,boxHeight);
        if(!std::isfinite(width)||!std::isfinite(height)||width<0||height<0||width>16384||height>16384)return false;
        // frame policy 决议对齐基准(三宽度协议):owner-box 严格用 owner 宽,负 x 由 surface-origin 重基表示。
        double alignWidth=width;
        if(horizontalFrame==1)alignWidth=boxWidth>0?boxWidth:naturalW;
        else if(horizontalFrame==2||horizontalFrame==4)alignWidth=wrapWidth>0?wrapWidth:naturalW;
        else if(horizontalFrame==3)alignWidth=naturalW;
        richtext::applyLineAlignOffsets(laid,alignWidth,align=="center"?richtext::TokenAlign::Center:((align=="right"||align=="end")?richtext::TokenAlign::Right:richtext::TokenAlign::Left));
        double surfaceOriginX=0;
        if(horizontalFrame==1)surfaceOriginX=richtext::rebaseNegativeLineOrigins(laid);
        else if(horizontalFrame==4)surfaceOriginX=richtext::compactLineOrigins(laid);
        // paragraph-box 的超宽行(首词不可断)会产生负行偏移;单 surface 无法向左溢出绘制,显式钳 0 并记录。
        for(size_t li=0;horizontalFrame==2&&li<laid.lines.size();++li){
            if(laid.lines[li].x>=0)continue;
            static int overflowLogLeft=8;
            if(overflowLogLeft-->0)ALOGE("[richtext-frame] 行 %zu 偏移 %.1f<0(alignWidth=%.1f<行宽),钳 0 —— paragraph-box 超宽行",li,laid.lines[li].x,alignWidth);
            const double shift=-laid.lines[li].x;laid.lines[li].x=0;
            for(auto& tp:laid.tokens)if((size_t)tp.line==li)tp.x+=shift;
        }
        const richtext::HorizontalContentBounds contentBounds=richtext::horizontalContentBounds(laid,surfaceOriginX);
        double top=0;if(verticalAlign=="middle")top=std::max(0.0,(height-naturalHeight)*.5);else if(verticalAlign=="bottom")top=std::max(0.0,height-naturalHeight);
        const richtext::ContentBounds fullTokenBounds=richtext::tokenContentBounds(laid,inputs,surfaceOriginX,surfaceOriginY,top,false);
        const richtext::ContentBounds textTokenBounds=richtext::tokenContentBounds(laid,inputs,surfaceOriginX,surfaceOriginY,top,true);
        if(gRichTextLineTrace>0){
            gRichTextLineTrace--;
            ALOGI("[richtext-frame] node=%d hFrame=%d wrap=%.1f alignWidth=%.1f surface=%.1fx%.1f natural=%.1fx%.1f content=%.1f,%.1f %.1fx%.1f text=%.1f,%.1f %.1fx%.1f surfaceOrigin=%.1f,%.1f align=%s lines=%zu tokens=%zu markup=%.60s",
                  node,horizontalFrame,wrapWidth,alignWidth,width,height,naturalW,naturalHeight,contentBounds.x,fullTokenBounds.y,contentBounds.width,fullTokenBounds.height,
                  textTokenBounds.x,textTokenBounds.y,textTokenBounds.width,textTokenBounds.height,surfaceOriginX,surfaceOriginY,align.c_str(),laid.lines.size(),n,markup.c_str());
        }
        int pw=std::max(1,(int)std::ceil(width*resolution)),ph=std::max(1,(int)std::ceil(height*resolution));
        GLint maxTexture=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maxTexture);if(pw>maxTexture||ph>maxTexture||(uint64_t)pw*(uint64_t)ph>64u*1024u*1024u)return false;
        std::string key=requestKey+"|pixels|"+std::to_string(pw)+"x"+std::to_string(ph);
        if(en.key!=key){
            // token 落点(top-down 逻辑坐标):baseline = top + line.y + line.ascent(≙ mac baselineUp 的翻转前值)
            std::vector<float> placements(n*2,0.0f);
            for(size_t k=0;k<n&&k<laid.tokens.size();++k){const auto& tp=laid.tokens[k];if(tp.line<0||(size_t)tp.line>=laid.lines.size())continue;const auto& ln=laid.lines[(size_t)tp.line];
                placements[k*2]=(float)tp.x;placements[k*2+1]=(float)(top+ln.y+ln.ascent);}
            if(!jniRichRasterUpload(en,spec,pw,ph,resolution,placements))return false;
            // 内联图:纹理/uv 权威在 C++,按排版落点组 native quad(≙ mac richImageQuads);有图时切换 richQuads 呈现。
            en.richQuads.clear();std::vector<sg::NativeQuad> imageQuads;
            for(size_t k=0;k<n&&k<laid.tokens.size();++k){
                if(inputs[k].kind!=richtext::TokenKind::Image)continue;const int idx=images[k].index;
                if(idx<0||(size_t)idx>=imgDescs.size()||!imgOk[(size_t)idx])continue;
                const float iw=(float)inputs[k].nakedWidth,ih=images[k].height,pa=images[k].placementAscent;if(iw<=0||ih<=0)continue;
                const auto& tp=laid.tokens[k];if(tp.line<0||(size_t)tp.line>=laid.lines.size())continue;const auto& ln=laid.lines[(size_t)tp.line];
                const float qx=(float)tp.x,qy=(float)(top+ln.y+ln.ascent-pa);
                sg::NativeQuad q{};q[0]=(float)imgDescs[(size_t)idx].textureId;q[1]=qx;q[2]=qy;q[3]=qx+iw;q[4]=qy;q[5]=qx+iw;q[6]=qy+ih;q[7]=qx;q[8]=qy+ih;
                for(int j=0;j<8;j++)q[9+j]=imgDescs[(size_t)idx].uv[j];
                q[17]=(float)0xFFFFFF;q[18]=1;q[19]=-1;q[20]=-1;q[22]=imgDescs[(size_t)idx].premul;imageQuads.push_back(q);
            }
            if(!imageQuads.empty()){sg::NativeQuad textQuad{};textQuad[0]=(float)en.cid;textQuad[1]=0;textQuad[2]=0;textQuad[3]=(float)width;textQuad[4]=0;textQuad[5]=(float)width;textQuad[6]=(float)height;textQuad[7]=0;textQuad[8]=(float)height;const float uv[8]={0,0,1,0,1,1,0,1};for(int i=0;i<8;i++)textQuad[9+i]=uv[i];textQuad[17]=(float)0xFFFFFF;textQuad[18]=1;textQuad[19]=-1;textQuad[20]=-1;textQuad[22]=0;en.richQuads.push_back(textQuad);en.richQuads.insert(en.richQuads.end(),imageQuads.begin(),imageQuads.end());}
            en.key=key;
        }
        en.richRequestKey=requestKey;en.richWidth=width;en.richHeight=height;en.richLineHeight=lineHeight;
        en.richContentX=contentBounds.x;en.richContentY=fullTokenBounds.y;en.richContentWidth=contentBounds.width;en.richContentHeight=fullTokenBounds.height;
        en.richTextContentX=textTokenBounds.x;en.richTextContentY=textTokenBounds.y;en.richTextContentWidth=textTokenBounds.width;en.richTextContentHeight=textTokenBounds.height;
        en.richSurfaceOriginX=surfaceOriginX;en.richSurfaceOriginY=surfaceOriginY;
        en.richAnchorWidth=horizontalFrame==4?richtext::taggedAnchorWidth(contentBounds.x,contentBounds.width,
            align=="center"?richtext::TokenAlign::Center:((align=="right"||align=="end")?richtext::TokenAlign::Right:richtext::TokenAlign::Left)):width;
        emitOuts();publishRich(node,en,s,width,height,anchorX,anchorY);return true;
    }
private:
    void release(Entry& e){if(e.cid)gDec.unregisterExternalTexture(e.cid);if(e.gl)glDeleteTextures(1,&e.gl);e={};}
};
static NativeTextSystem gNativeText;

sg::CommandReceipt freeSceneNodeFromJs(int id, uint32_t expectedGeneration) {   // host_core_common 平台接口
    const sg::CommandReceipt preflight = gSG.validateFreeNode(id, expectedGeneration);
    if (preflight != sg::CommandReceipt::Applied) return preflight;
    // 已材料化的 GObject 子节点可能被单独 dispose,而拥有它的 Native FGUI 实例还活着。把"部分
    // dispose"降级成 detach,让 Runtime::destroyTree 成为唯一的释放权威(与 mac/iOS 同语义)。
    if (gNativeFgui.isManagedNode(id)) {
        const sg::CommandReceipt detached = gSG.removeFromParent(id, expectedGeneration);
        if (!sg::commandReceiptSucceeded(detached)) return detached;
        gNativeFgui.refreshManagedDisplayVisibility(id, gSG);
        ++gNativeFguiManagedFreeDeferrals;
        return sg::CommandReceipt::ManagedFreeDeferred;
    }
    gSpinexRt.onNodeFreed(id);   // spinex 实例脱钩冻结(防节点 id 复用后 quads 写到新主人身上)
    gNativeText.detach(id);
    return gSG.freeNode(id, expectedGeneration);
}

// ======================= zip 桥(对齐 ios L866-958;zlib raw inflate) =======================
static std::unordered_map<int, std::unordered_map<std::string, macres::Bytes>> gZipCache;
static std::list<int> gZipLru;
static bool zipParseAll(const uint8_t* p, size_t n, std::unordered_map<std::string, macres::Bytes>& out) {
    if (!p || n < 22) return false;
    ssize_t eo = -1;   // EOCD:末尾向前搜 PK\x05\x06(最多回溯 64KB 注释区)
    ssize_t lo = (ssize_t)n - 22 - 65536; if (lo < 0) lo = 0;
    for (ssize_t i = (ssize_t)n - 22; i >= lo; i--)
        if (p[i]==0x50 && p[i+1]==0x4b && p[i+2]==0x05 && p[i+3]==0x06) { eo = i; break; }
    if (eo < 0) return false;
    uint16_t total; memcpy(&total, p+eo+10, 2);
    uint32_t cdOff; memcpy(&cdOff, p+eo+16, 4);
    if (total == 0xFFFF || cdOff == 0xFFFFFFFF) return false;   // zip64 → JS 兜底
    size_t off = cdOff;
    for (int i = 0; i < total; i++) {
        if (off + 46 > n || !(p[off]==0x50 && p[off+1]==0x4b && p[off+2]==0x01 && p[off+3]==0x02)) return false;
        uint16_t method; memcpy(&method, p+off+10, 2);
        uint32_t csize;  memcpy(&csize,  p+off+20, 4);
        uint32_t usize;  memcpy(&usize,  p+off+24, 4);
        uint16_t nlen, elen, clen; memcpy(&nlen, p+off+28, 2); memcpy(&elen, p+off+30, 2); memcpy(&clen, p+off+32, 2);
        uint32_t lho; memcpy(&lho, p+off+42, 4);
        if (off + 46 + nlen > n) return false;
        std::string name((const char*)p+off+46, nlen);
        off += 46u + nlen + elen + clen;
        if (name.empty() || name.back() == '/') continue;   // 目录条目跳过
        if (lho + 30 > n || !(p[lho]==0x50 && p[lho+1]==0x4b && p[lho+2]==0x03 && p[lho+3]==0x04)) return false;
        uint16_t lnlen, lelen; memcpy(&lnlen, p+lho+26, 2); memcpy(&lelen, p+lho+28, 2);
        size_t dataOff = (size_t)lho + 30 + lnlen + lelen;
        if (dataOff + csize > n) return false;
        macres::Bytes content;
        if (method == 0) content = std::make_shared<std::vector<uint8_t>>(p+dataOff, p+dataOff+csize);
        else if (method == 8) {
            auto u = std::make_shared<std::vector<uint8_t>>(usize);
            z_stream zs; memset(&zs, 0, sizeof(zs));
            zs.next_in = (Bytef*)(p+dataOff); zs.avail_in = csize;
            zs.next_out = u->data(); zs.avail_out = usize;
            if (inflateInit2(&zs, -15) != Z_OK) return false;   // -15 = raw deflate
            int r = inflate(&zs, Z_FINISH); size_t got = zs.total_out; inflateEnd(&zs);
            if (r != Z_STREAM_END || got != usize) return false;
            content = u;
        } else return false;   // 未知压缩法 → JS 兜底
        out[name] = content;
    }
    return true;
}
static JSValueRef cbZipParse(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeUndefined(ctx);
    int token = (int)JSValueToNumber(ctx, a[0], e);
    macres::Bytes d = jsToBytes(ctx, a[1], e);
    const uint64_t epoch = gHostEpoch.load();
    gLoadPool.post([token, d, epoch] {
        auto entries = std::make_shared<std::unordered_map<std::string, macres::Bytes>>();
        bool ok = d && zipParseAll(d->data(), d->size(), *entries);
        gMainQ.post([token, entries, ok, epoch] {
            if (!gCtx || epoch != gHostEpoch.load()) return;
            JSValueRef namesV = JSValueMakeNull(gCtx);
            if (ok) {
                gZipCache[token] = *entries; gZipLru.push_back(token);
                while (gZipLru.size() > 8) { gZipCache.erase(gZipLru.front()); gZipLru.pop_front(); }
                std::string j = "[";
                bool first = true;
                for (auto& kv : *entries) {
                    if (!first) j += ','; first = false;
                    j += '"';
                    for (char c : kv.first) { if (c=='"'||c=='\\') { j += '\\'; } j += c; }
                    j += '"';
                }
                j += "]";
                JSStringRef s = JSStringCreateWithUTF8CString(j.c_str());
                namesV = JSValueMakeString(gCtx, s); JSStringRelease(s);
            }
            JSValueRef args[2] = { JSValueMakeNumber(gCtx, token), namesV };
            callJsGlobal("__zipDone", 2, args);
        });
    });
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbZipGet(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeNull(ctx);
    int token = (int)JSValueToNumber(ctx, a[0], e);
    std::string name = jsToStd(ctx, a[1]);
    bool asString = argc > 2 && JSValueToNumber(ctx, a[2], e) != 0;
    auto zit = gZipCache.find(token);
    if (zit == gZipCache.end()) return JSValueMakeNull(ctx);
    auto fit = zit->second.find(name);
    if (fit == zit->second.end() || !fit->second) return JSValueMakeNull(ctx);
    const macres::Bytes& d = fit->second;
    if (asString) {
        std::string s((const char*)d->data(), d->size());
        JSStringRef js = JSStringCreateWithUTF8CString(s.c_str());
        JSValueRef v = JSValueMakeString(ctx, js); JSStringRelease(js); return v;
    }
    size_t len = d->size(); void* buf = malloc(len); if (!buf) return JSValueMakeNull(ctx);
    memcpy(buf, d->data(), len);
    JSObjectRef ab = JSObjectMakeArrayBufferWithBytesNoCopy(ctx, buf, len, [](void* p2, void*){ free(p2); }, nullptr, e);
    return ab ? (JSValueRef)ab : JSValueMakeNull(ctx);
}

// ======================= 音频桥(对齐 ios L651-810;实例池/busy/源标记语义在 Kotlin AudioBridge 复刻) =======================
static macres::Bytes loadAudioAsset(const std::string& url) {
    // An explicit remote audio URL keeps its authority and query parameters.
    // Relative and data URLs continue to use the project's asset resolver.
    if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) {
        auto bytes = fetchRemoteAsset(url);
        if (!bytes) ALOGE("[audio] remote resource failed: %s", url.c_str());
        return bytes;
    }
    return loadAsset(url.c_str());
}
static JSValueRef cbAuLoadUrl(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeNumber(ctx, -1);
    int aid = (int)JSValueToNumber(ctx, a[0], e);
    std::string url = jsToStd(ctx, a[1]);
    JNIEnv* env = jniEnv();
    if (!env || !gActivity || !gMidAuLoad) return JSValueMakeNumber(ctx, -1);
    macres::Bytes d = loadAudioAsset(url);   // Kotlin 侧按字节指纹做实例池
    if (!d) return JSValueMakeNumber(ctx, -1);
    jbyteArray arr = env->NewByteArray((jsize)d->size());
    env->SetByteArrayRegion(arr, 0, (jsize)d->size(), (const jbyte*)d->data());
    // URL Audio is retained by its JS owner across pause/replay. Protect its id
    // from the clone pool until that owner explicitly frees it (op6).
    double dur = env->CallDoubleMethod(gActivity, gMidAuLoad, -aid - 1, arr);
    env->DeleteLocalRef(arr);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return JSValueMakeNumber(ctx, -1); }
    return JSValueMakeNumber(ctx, dur);
}
static JSValueRef cbAuLoadUrlAsync(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeUndefined(ctx);
    int aid = (int)JSValueToNumber(ctx, a[0], e);
    std::string url = jsToStd(ctx, a[1]);
    const uint64_t epoch = gHostEpoch.load();
    gLoadPool.post([aid, url, epoch] {
        macres::Bytes d = loadAudioAsset(url);
        gMainQ.post([aid, d, epoch] {
            if (!gCtx || epoch != gHostEpoch.load()) return;
            double dur = -1;
            JNIEnv* env = jniEnv();
            if (env && gActivity && gMidAuLoad && d) {
                jbyteArray arr = env->NewByteArray((jsize)d->size());
                env->SetByteArrayRegion(arr, 0, (jsize)d->size(), (const jbyte*)d->data());
                dur = env->CallDoubleMethod(gActivity, gMidAuLoad, -aid - 1, arr);
                env->DeleteLocalRef(arr);
                if (env->ExceptionCheck()) { env->ExceptionClear(); dur = -1; }
            }
            JSValueRef args[2] = { JSValueMakeNumber(gCtx, aid), JSValueMakeNumber(gCtx, dur) };
            callJsGlobal("__auDone", 2, args);
        });
    });
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbAuLoadData(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2 || !JSValueIsObject(ctx, a[1])) return JSValueMakeNumber(ctx, -1);
    int aid = (int)JSValueToNumber(ctx, a[0], e);
    macres::Bytes d = jsToBytes(ctx, a[1], e);
    JNIEnv* env = jniEnv();
    if (!env || !gActivity || !gMidAuLoad || !d) return JSValueMakeNumber(ctx, -1);
    jbyteArray arr = env->NewByteArray((jsize)d->size());
    env->SetByteArrayRegion(arr, 0, (jsize)d->size(), (const jbyte*)d->data());
    double dur = env->CallDoubleMethod(gActivity, gMidAuLoad, -aid - 1, arr);   // 负 id 编码"源实例打标"(Kotlin 侧解码)
    env->DeleteLocalRef(arr);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return JSValueMakeNumber(ctx, -1); }
    return JSValueMakeNumber(ctx, dur);
}
static JSValueRef cbAuClone(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeNumber(ctx, -1);
    int nid = (int)JSValueToNumber(ctx, a[0], e);
    int sid = (int)JSValueToNumber(ctx, a[1], e);
    JNIEnv* env = jniEnv();
    if (!env || !gActivity || !gMidAuClone) return JSValueMakeNumber(ctx, -1);
    double dur = env->CallDoubleMethod(gActivity, gMidAuClone, nid, sid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return JSValueMakeNumber(ctx, -1); }
    return JSValueMakeNumber(ctx, dur);
}
static JSValueRef cbAuCtl(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeNumber(ctx, 0);
    int aid = (int)JSValueToNumber(ctx, a[0], e);
    int op  = (int)JSValueToNumber(ctx, a[1], e);
    double v1 = argc > 2 ? JSValueToNumber(ctx, a[2], e) : 0;
    double v2 = argc > 3 ? JSValueToNumber(ctx, a[3], e) : 0;
    JNIEnv* env = jniEnv();
    if (!env || !gActivity || !gMidAuCtl) return JSValueMakeNumber(ctx, 0);
    double r = env->CallDoubleMethod(gActivity, gMidAuCtl, aid, op, v1, v2);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return JSValueMakeNumber(ctx, 0); }
    return JSValueMakeNumber(ctx, r);
}
// 新 JS 世界之前释放旧世界的全部 MediaPlayer(≙ ios_app.mm shutdownAudio,iOS/mac 在 reloadGame 调;Android 统一放在
// setupGame 开头,同时覆盖 reload 与 EGL 上下文重建两条路径):新世界的 audio id 从 1 重计,旧循环音不能穿越 reload、
// 不能与新实例撞号。晚到的 ended 回调由 nativeAudioEnded 按 gHostEpoch 过滤。
static void shutdownAudio() {
    JNIEnv* env = jniEnv();
    if (!env || !gActivity || !gMidAuShutdown) return;
    env->CallVoidMethod(gActivity, gMidAuShutdown);
    if (env->ExceptionCheck()) { env->ExceptionClear(); ALOGE("[audio] shutdown bridge failed"); }
}


// ======================= 命令缓冲/场景图/图元/tween 绑定(与 ios_app.mm L235-601 逐字同源) =======================
static JSValueRef cbNativeVibrate(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                 size_t argc, const JSValueRef args[], JSValueRef*) {
    if (!argc || !gActivity || !gMidNativeVibrate) return JSValueMakeBoolean(ctx, false);
    JNIEnv* env = jniEnv();
    if (!env) return JSValueMakeBoolean(ctx, false);
    jstring value = jstr(env, jsToStd(ctx, args[0]));
    jboolean accepted = env->CallBooleanMethod(gActivity, gMidNativeVibrate, value);
    env->DeleteLocalRef(value);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe(); env->ExceptionClear();
        return JSValueMakeBoolean(ctx, false);
    }
    return JSValueMakeBoolean(ctx, accepted == JNI_TRUE);
}

static JSValueRef cbFlush(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeNumber(ctx, -1);
    size_t words = (size_t)JSValueToNumber(ctx, args[0], exc);
    if (words > 0) { gFrameHadCmds = true; gFrameHadContent = true; }
    double t0 = nowUs(); gDec.execute(gBuf, words); double dt = nowUs() - t0;
    gFrameDecodeUs += dt;
    return JSValueMakeNumber(ctx, dt);
}
// Phase D-1(与 mac 对齐):动画推进移出 draw,JS 帧序列 ADVANCE_NATIVE 显式驱动;
// draw 期从此零写入零 compute(batcher 只读 seal 后状态)。

// coverage 计数(严格冷启动闸门采样;layout: serial, escapes, mask, type, texture, draws, quads)
static JSValueRef cbNativeBatchCoverage(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    JSValueRef values[7] = {
        JSValueMakeNumber(ctx, (double)gNativeBatchSerial),
        JSValueMakeNumber(ctx, (double)gShadowBatcher.escapes),
        JSValueMakeNumber(ctx, (double)gShadowBatcher.escMask),
        JSValueMakeNumber(ctx, (double)gShadowBatcher.escType),
        JSValueMakeNumber(ctx, (double)gShadowBatcher.escTex),
        JSValueMakeNumber(ctx, (double)gShadowBatcher.drawCalls),
        JSValueMakeNumber(ctx, (double)gShadowBatcher.quads)
    };
    return JSObjectMakeArray(ctx, 7, values, nullptr);
}
// RenderTexture:渲染 native 子树进 Pixi RenderTextureSystem 当前绑定的 FBO。
static JSValueRef cbNativeRenderTexturePreflight(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 1 || eglGetCurrentContext() == EGL_NO_CONTEXT) return JSValueMakeBoolean(ctx, false);
    int root = (int)JSValueToNumber(ctx, a[0], e);
    if (root <= 0 || root >= sg::NODE_CAP || !gSG.nodes[root].alive) return JSValueMakeBoolean(ctx, false);
    if (!gShadowBatcher.ready()) {
        gShadowBatcher.texLookup=[](int id){return gDec.glTextureEnsured((uint32_t)id);};
        gShadowBatcher.init();
    }
    return JSValueMakeBoolean(ctx, gShadowBatcher.preflight(&gSG, root));
}
static JSValueRef cbNativeRenderTexture(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if(argc<11||eglGetCurrentContext()==EGL_NO_CONTEXT)return JSValueMakeBoolean(ctx,false);
    int root=(int)JSValueToNumber(ctx,a[0],e);float w=(float)JSValueToNumber(ctx,a[1],e),h=(float)JSValueToNumber(ctx,a[2],e);
    bool clear=JSValueToBoolean(ctx,a[3]);float alphaScale=(float)JSValueToNumber(ctx,a[4],e),out[6];for(int i=0;i<6;i++)out[i]=(float)JSValueToNumber(ctx,a[5+i],e);
    GLint fbo=0,vp[4]={0,0,0,0};glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);glGetIntegerv(GL_VIEWPORT,vp);
    if(root<=0||root>=sg::NODE_CAP||!gSG.nodes[root].alive||fbo==0||w<=0||h<=0)return JSValueMakeBoolean(ctx,false);
    if(!gShadowBatcher.ready()){gShadowBatcher.texLookup=[](int id){return gDec.glTextureEnsured((uint32_t)id);};gShadowBatcher.init();}
    if(clear){glDisable(GL_SCISSOR_TEST);glClearColor(0,0,0,0);glClearStencil(0);glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);}
    gShadowBatcher.frameReset();gShadowBatcher.frame(&gSG,root,w,h,out,alphaScale);
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)fbo);glViewport(vp[0],vp[1],vp[2],vp[3]);
    return JSValueMakeBoolean(ctx,gShadowBatcher.escapes==0&&gShadowBatcher.escTex==0);
}
// __zstdDecompress(srcTypedArray) -> ArrayBuffer 或 null(失败,JS 回退 fzstd)。
// 必须加 byteOffset(subarray 视图),CONTENTSIZE_UNKNOWN 用 decompressBound 求上界——两个坑与 iOS 注释同。
static JSValueRef cbZstdDecompress(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 1 || !JSValueIsObject(ctx, a[0])) return JSValueMakeNull(ctx);
    JSObjectRef srcObj = JSValueToObject(ctx, a[0], e);
    const uint8_t* srcBase = (const uint8_t*)JSObjectGetTypedArrayBytesPtr(ctx, srcObj, e);
    size_t srcLen = JSObjectGetTypedArrayByteLength(ctx, srcObj, e);
    const uint8_t* src = srcBase;   // shim 的 BytesPtr 已含视图偏移(JS_GetTypedArrayBuffer 语义),不再叠加
    if (!src || !srcLen) return JSValueMakeNull(ctx);
    unsigned long long dstSize = ZSTD_getFrameContentSize(src, srcLen);
    if (dstSize == ZSTD_CONTENTSIZE_ERROR) return JSValueMakeNull(ctx);
    if (dstSize == ZSTD_CONTENTSIZE_UNKNOWN) dstSize = ZSTD_decompressBound(src, srcLen);
    if (dstSize == 0 || dstSize == ZSTD_CONTENTSIZE_ERROR || dstSize > (256ull << 20)) return JSValueMakeNull(ctx);
    void* dst = malloc((size_t)dstSize);
    if (!dst) return JSValueMakeNull(ctx);
    size_t got = ZSTD_decompress(dst, (size_t)dstSize, src, srcLen);
    if (ZSTD_isError(got)) { free(dst); return JSValueMakeNull(ctx); }
    return JSObjectMakeArrayBufferWithBytesNoCopy(ctx, dst, got, [](void* p, void*){ free(p); }, nullptr, e);
}
static void flipMainScreenDraw(int root);
static JSValueRef cbNativePresent(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc>0) {
        int root = (int)JSValueToNumber(ctx, a[0], e);
        if (argc>=5) for (int i=0;i<4;i++) gSgClear[i] = (float)JSValueToNumber(ctx, a[1+i], e);
        // 与 mac 同款:帧相位推进与 draw 执行解耦(present 点=seal 窗口结束;PHB 停留 6 会把
        // 此后同帧业务写误计 lateMut)。
        if (gSG.buf) gSG.buf[sg::FRAME_PHASE_BASE] = 7.0f;   // B0-1:阶段7=present/draw
        if (root > 0) flipMainScreenDraw(root);   // 调用点内联绘制:提交原子性与 mac/iOS 同
    }
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbShadowFlag(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    return JSValueMakeUndefined(ctx);   // Android 不带 shadow 对拍旁路(诊断仪表,非功能路径)
}

// ======================= 同步 IO / 文本 / 异步资源 / timer / xhr =======================
static JSValueRef cbReadFile(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
    JSObjectRef obj = JSObjectMake(ctx, nullptr, nullptr);
    auto setp = [&](const char* k, JSValueRef v){ JSStringRef s = JSStringCreateWithUTF8CString(k); JSObjectSetProperty(ctx, obj, s, v, kJSPropertyAttributeNone, nullptr); JSStringRelease(s); };
    if (argc < 1) { setp("ok", JSValueMakeBoolean(ctx, false)); return obj; }
    std::string path = jsToStd(ctx, args[0]);
    macres::Bytes d = loadAsset(path.c_str());
    if (!d) { setp("ok", JSValueMakeBoolean(ctx, false)); return obj; }
    size_t len = d->size(); void* buf = malloc(len); memcpy(buf, d->data(), len);
    JSObjectRef ab = JSObjectMakeArrayBufferWithBytesNoCopy(ctx, buf, len, [](void* p, void*){ free(p); }, nullptr, exc);
    setp("ok", JSValueMakeBoolean(ctx, true)); setp("data", ab);
    return obj;
}
static JSValueRef cbLoadImage(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                              const JSValueRef a[], JSValueRef* e) {
    if (argc < 1) return JSValueMakeNull(ctx);
    std::string url = jsToStd(ctx, a[0]);
    macres::Bytes d = loadAsset(url.c_str());
    if (!d) return JSValueMakeNull(ctx);
    return decodeImageData(ctx, d, e, argc>1&&JSValueToBoolean(ctx,a[1]));
}
static JSValueRef cbDecodeImage(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeNull(ctx);
    macres::Bytes d = jsToBytes(ctx, args[0], exc);
    JSValueRef r = decodeImageData(ctx, d, exc, argc>1&&JSValueToBoolean(ctx,args[1]));
    if (JSValueIsNull(ctx, r)) ALOGE("[game] __decodeImage 失败: %zu 字节", d ? d->size() : 0);
    return r;
}
// __nativeTextUpdate(...25 参) -> metrics|null(参数重映射与 iOS 逐字同源)
static JSValueRef cbNativeTextUpdate(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<17)return JSValueMakeNull(ctx);double w=0,h=0,lh=0;int node=(int)JSValueToNumber(ctx,a[0],e);std::string text=jsToStd(ctx,a[1]);
    bool ok=gNativeText.update(node,text,jsToStd(ctx,a[2]),jsToStd(ctx,a[3]),
        jsToStd(ctx,a[4]),JSValueToNumber(ctx,a[5],e),jsToStd(ctx,a[6]),JSValueToNumber(ctx,a[7],e),
        JSValueToNumber(ctx,a[8],e),JSValueToNumber(ctx,a[9],e),jsToStd(ctx,a[10]),JSValueToNumber(ctx,a[11],e),
        JSValueToNumber(ctx,a[12],e),JSValueToNumber(ctx,a[13],e),JSValueToNumber(ctx,a[14],e),JSValueToNumber(ctx,a[15],e),
        JSValueToNumber(ctx,a[16],e),0,0,argc>19?jsToStd(ctx,a[19]):std::string("top"),argc>17?JSValueToNumber(ctx,a[17],e):0,
        argc>18?JSValueToNumber(ctx,a[18],e):0,argc>20?jsToStd(ctx,a[20]):std::string(),
        argc>21?jsToStd(ctx,a[21]):std::string(),argc>22?(int)JSValueToNumber(ctx,a[22],e):0,
        argc>23?JSValueToNumber(ctx,a[23],e):0,argc>24?JSValueToNumber(ctx,a[24],e):0,w,h,lh);
    if(!ok)return JSValueMakeNull(ctx);
    const std::string liveAlign=jsToStd(ctx,a[10]);
    const std::string liveVerticalAlign=argc>19?jsToStd(ctx,a[19]):std::string();
    gNativeFgui.claimTextFacadeVisual(node,text,(float)w,(float)h,(float)lh,gSG,
                                     liveAlign,liveVerticalAlign);
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"width",w);setJsNumber(ctx,out,"height",h);setJsNumber(ctx,out,"lineHeight",lh);return out;
}
// __nativeRichTextUpdate(node,markup,font,fill,stroke,strokeWidth,align,resolution,anchorX,anchorY,
//                        wordWrapWidth,lineHeight,boxWidth,boxHeight,verticalAlign) -> metrics|null
static JSValueRef cbNativeRichTextUpdate(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<15)return JSValueMakeNull(ctx);double w=0,h=0,lh=0,contentW=0,contentX=0,contentH=0,contentY=0,textW=0,textX=0,textH=0,textY=0;
    bool ok=gNativeText.updateRich((int)JSValueToNumber(ctx,a[0],e),jsToStd(ctx,a[1]),jsToStd(ctx,a[2]),
        jsToStd(ctx,a[3]),jsToStd(ctx,a[4]),JSValueToNumber(ctx,a[5],e),jsToStd(ctx,a[6]),
        JSValueToNumber(ctx,a[7],e),JSValueToNumber(ctx,a[8],e),JSValueToNumber(ctx,a[9],e),
        JSValueToNumber(ctx,a[10],e),JSValueToNumber(ctx,a[11],e),JSValueToNumber(ctx,a[12],e),
        JSValueToNumber(ctx,a[13],e),jsToStd(ctx,a[14]),w,h,lh,0,false,&contentW,&contentX,&contentH,&contentY,&textW,&textX,&textH,&textY);
    if(!ok)return JSValueMakeNull(ctx);JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"width",w);setJsNumber(ctx,out,"height",h);setJsNumber(ctx,out,"lineHeight",lh);setJsNumber(ctx,out,"contentWidth",contentW);setJsNumber(ctx,out,"contentX",contentX);setJsNumber(ctx,out,"contentHeight",contentH);setJsNumber(ctx,out,"contentY",contentY);setJsNumber(ctx,out,"textContentWidth",textW);setJsNumber(ctx,out,"textContentX",textX);setJsNumber(ctx,out,"textContentHeight",textH);setJsNumber(ctx,out,"textContentY",textY);return out;
}
// V2 结构化载荷读取(≙ host_core.mm richV2Str/richV2Num)
static std::string richV2Str(JSContextRef ctx,JSObjectRef o,const char* k){
    JSStringRef s=JSStringCreateWithUTF8CString(k);JSValueRef v=JSObjectGetProperty(ctx,o,s,nullptr);JSStringRelease(s);
    if(!v||JSValueIsUndefined(ctx,v)||JSValueIsNull(ctx,v))return {};return jsToStd(ctx,v);
}
// richV2Num 由 host_core_common.h 提供(三端共用)。
// __nativeTextUpdateV2(node, payload) -> metrics|null —— 普通文本/Input 的三宽度协议结构化入口(与 host_core.mm 同语义):
// natural 要求 box=0、owner-box 要求 box>0,违反或未知 = 无效协议 → null(禁止静默;JS 契约层先行校验,此为二道防线)。
static JSValueRef cbNativeTextUpdateV2(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<2||!JSValueIsObject(ctx,a[1]))return JSValueMakeNull(ctx);
    JSObjectRef p=JSValueToObject(ctx,a[1],e);if(!p)return JSValueMakeNull(ctx);
    const std::string hf=richV2Str(ctx,p,"horizontalFrame");
    const double bw=richV2Num(ctx,p,"boxWidth",0),bh=richV2Num(ctx,p,"boxHeight",0);
    const bool frameOk=(hf=="natural"&&!(bw>0))||(hf=="owner-box"&&bw>0);
    if(!frameOk){static int badPlainFrameLogLeft=8;if(badPlainFrameLogLeft-->0)ALOGE("[richtext-frame] plain V2 invalid horizontalFrame='%s' boxWidth=%.1f — reject",hf.c_str(),bw);return JSValueMakeNull(ctx);}
    const int node=(int)JSValueToNumber(ctx,a[0],e);
    const std::string text=richV2Str(ctx,p,"text");
    double w=0,h=0,lh=0;
    bool ok=gNativeText.update(node,text,richV2Str(ctx,p,"font"),richV2Str(ctx,p,"fill"),
        richV2Str(ctx,p,"stroke"),richV2Num(ctx,p,"strokeWidth",0),richV2Str(ctx,p,"shadow"),richV2Num(ctx,p,"shadowBlur",0),
        richV2Num(ctx,p,"shadowX",0),richV2Num(ctx,p,"shadowY",0),richV2Str(ctx,p,"align"),richV2Num(ctx,p,"padding",0),
        richV2Num(ctx,p,"resolution",1),richV2Num(ctx,p,"anchorX",0),richV2Num(ctx,p,"anchorY",0),
        richV2Num(ctx,p,"wrapWidth",0),richV2Num(ctx,p,"lineHeight",0),bw,bh,richV2Str(ctx,p,"vAlign"),
        richV2Num(ctx,p,"letterSpacing",0),richV2Num(ctx,p,"leading",0),
        richV2Str(ctx,p,"gradientColors"),richV2Str(ctx,p,"gradientStops"),(int)richV2Num(ctx,p,"gradientType",0),
        richV2Num(ctx,p,"ascent",0),richV2Num(ctx,p,"descent",0),w,h,lh);
    if(!ok)return JSValueMakeNull(ctx);
    gNativeFgui.claimTextFacadeVisual(node,text,(float)w,(float)h,(float)lh,gSG,richV2Str(ctx,p,"align"),richV2Str(ctx,p,"vAlign"),(int)richV2Num(ctx,p,"autoSize",-1));
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"width",w);setJsNumber(ctx,out,"height",h);setJsNumber(ctx,out,"lineHeight",lh);return out;
}
// __nativeRichTextUpdateV2(node, payload) -> metrics|null —— 三宽度布局协议结构化入口(与 host_core.mm 同语义)。
static JSValueRef cbNativeRichTextUpdateV2(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<2||!JSValueIsObject(ctx,a[1]))return JSValueMakeNull(ctx);
    JSObjectRef p=JSValueToObject(ctx,a[1],e);if(!p)return JSValueMakeNull(ctx);
    const std::string hf=richV2Str(ctx,p,"horizontalFrame");
    const int hFrame=hf=="owner-box"?1:(hf=="paragraph-box"?2:(hf=="natural"?3:(hf=="tagged-content"?4:-1)));
    if(hFrame<0){static int badFrameLogLeft=8;if(badFrameLogLeft-->0)ALOGE("[richtext-frame] V2 invalid horizontalFrame='%s' — reject",hf.c_str());return JSValueMakeNull(ctx);}
    double w=0,h=0,lh=0,contentW=0,contentX=0,contentH=0,contentY=0,textW=0,textX=0,textH=0,textY=0;
    bool ok=gNativeText.updateRich((int)JSValueToNumber(ctx,a[0],e),richV2Str(ctx,p,"markup"),richV2Str(ctx,p,"font"),
        richV2Str(ctx,p,"fill"),richV2Str(ctx,p,"stroke"),richV2Num(ctx,p,"strokeWidth",0),richV2Str(ctx,p,"align"),
        richV2Num(ctx,p,"resolution",1),richV2Num(ctx,p,"anchorX",0),richV2Num(ctx,p,"anchorY",0),
        richV2Num(ctx,p,"wrapWidth",0),richV2Num(ctx,p,"lineHeight",0),richV2Num(ctx,p,"boxWidth",0),
        richV2Num(ctx,p,"boxHeight",0),richV2Str(ctx,p,"vAlign"),w,h,lh,hFrame,richV2Num(ctx,p,"breakWords",0)!=0,
        &contentW,&contentX,&contentH,&contentY,&textW,&textX,&textH,&textY,
        richV2Num(ctx,p,"lineVerticalOffset",0),richV2Num(ctx,p,"verticalCenterHeight",0));
    if(!ok)return JSValueMakeNull(ctx);JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"width",w);setJsNumber(ctx,out,"height",h);setJsNumber(ctx,out,"lineHeight",lh);setJsNumber(ctx,out,"contentWidth",contentW);setJsNumber(ctx,out,"contentX",contentX);setJsNumber(ctx,out,"contentHeight",contentH);setJsNumber(ctx,out,"contentY",contentY);setJsNumber(ctx,out,"textContentWidth",textW);setJsNumber(ctx,out,"textContentX",textX);setJsNumber(ctx,out,"textContentHeight",textH);setJsNumber(ctx,out,"textContentY",textY);setJsNumber(ctx,out,"anchorWidth",gNativeText.richAnchorWidth((int)JSValueToNumber(ctx,a[0],e)));return out;
}
// ResourceManager 完成回调:像素永不进 JS,cid 命名宿主自有的外部 GL 纹理。旧上下文的完成一律丢弃。
static bool finishNativeImageAsync(JSGlobalContextRef requestContext, int token,
                                   uint32_t cid, int width, int height) {
    if (!gCtx || gCtx != requestContext) return false;
    const bool needsPremultiply = cid && gNativeResources.needsPremultiply(cid);
    JSValueRef callbackArgs[6] = { JSValueMakeNumber(gCtx, token), JSValueMakeNumber(gCtx, width),
        JSValueMakeNumber(gCtx, height), JSValueMakeNull(gCtx), JSValueMakeNumber(gCtx, cid),
        JSValueMakeBoolean(gCtx, needsPremultiply) };
    return callJsGlobalChecked("__imgDone", 6, callbackArgs);   // 同时传递 KTX DFD alpha 语义
}
static void deliverPublishedImage(JSGlobalContextRef requestContext, int token,
                                  uint32_t cid, int width, int height) {
    ALOGI("[image-async] token=%d cid=%u %dx%d", token, (unsigned)cid, width, height);
    if (!finishNativeImageAsync(requestContext, token, cid, width, height) && cid)
        gNativeResources.releaseAdopt(cid);
}
// __loadImageAsync(url, token) → ResourceManager 拉取+解码+分带上传 → __imgDone(token, w, h, null, cid)
static JSValueRef cbLoadImageAsync(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                   const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeUndefined(ctx);
    std::string url = jsToStd(ctx, a[0]);
    int token = (int)JSValueToNumber(ctx, a[1], e);
    JSGlobalContextRef requestContext = gCtx;
    // Pixi 的格式探测用极小的 data: URL:下一个主线程回合直接判失败,不排原生资源队列。
    if (url.rfind("data:", 0) == 0) {
        ALOGI("[image-async] data: URL 判失败 token=%d len=%zu head=%.48s", token, url.size(), url.c_str());
        gMainQ.post([requestContext, token]{ finishNativeImageAsync(requestContext, token, 0, 0, 0); });
        return JSValueMakeUndefined(ctx);
    }
    gNativeResources.requestImageURL(url, [requestContext, token, url](uint32_t cid, int width, int height) {
        if (!cid) ALOGE("[game] native image prepare 失败: %.96s", url.c_str());
        deliverPublishedImage(requestContext, token, cid, width, height);
    }, /*publishRef=*/true, argc>2&&JSValueToBoolean(ctx,a[2]));
    return JSValueMakeUndefined(ctx);
}
// __decodeImageAsync(token, arrayBuffer|typedArray) -> accepted:boolean(压缩字节只在 JS 线程拷一次)
static JSValueRef cbDecodeImageAsync(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                     const JSValueRef args[], JSValueRef* exc) {
    if (argc < 2) return JSValueMakeBoolean(ctx, false);
    int token = (int)JSValueToNumber(ctx, args[0], exc);
    macres::Bytes data = jsToBytes(ctx, args[1], exc);
    if (!data || data->empty()) return JSValueMakeBoolean(ctx, false);
    JSGlobalContextRef requestContext = gCtx;
    const size_t byteLength = data->size();
    gNativeResources.requestImageData(data, [requestContext, token, byteLength](uint32_t cid, int width, int height) {
        if (!cid) ALOGE("[game] native blob image prepare 失败: %zu 字节", byteLength);
        deliverPublishedImage(requestContext, token, cid, width, height);
    }, /*publishRef=*/true, argc>2&&JSValueToBoolean(ctx,args[2]));
    return JSValueMakeBoolean(ctx, true);
}
// __readFileAsync(url, token) → ResourceManager 后台拉取(带字节缓存/并发合流) → __fileDone(token, ArrayBuffer|null)
static JSValueRef cbReadFileAsync(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                  const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeUndefined(ctx);
    std::string url = jsToStd(ctx, a[0]);
    int token = (int)JSValueToNumber(ctx, a[1], e);
    JSGlobalContextRef requestContext = gCtx;
    gNativeResources.requestBytes(url, [requestContext, token](const macres::Bytes& data) {
        if (!gCtx || gCtx != requestContext) return;
        JSValueRef dataValue = JSValueMakeNull(gCtx);
        if (data && !data->empty()) {
            void* bytes = malloc(data->size());
            if (bytes) {
                memcpy(bytes, data->data(), data->size());
                JSValueRef error = nullptr;
                dataValue = JSObjectMakeArrayBufferWithBytesNoCopy(gCtx, bytes, data->size(),
                    [](void* p, void*) { free(p); }, nullptr, &error);
                if (error) dataValue = JSValueMakeNull(gCtx);
            }
        }
        JSValueRef callbackArgs[2] = { JSValueMakeNumber(gCtx, token), dataValue };
        callJsGlobal("__fileDone", 2, callbackArgs);
    });
    return JSValueMakeUndefined(ctx);
}


static JSValueRef cbNativeResourceRetain(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                         const JSValueRef args[], JSValueRef* exc) {
    uint32_t cid = argc ? (uint32_t)JSValueToNumber(ctx, args[0], exc) : 0;
    return JSValueMakeBoolean(ctx, cid && gNativeResources.retain(cid));
}
static JSValueRef cbNativeResourceAlive(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                        const JSValueRef args[], JSValueRef* exc) {
    uint32_t cid = argc ? (uint32_t)JSValueToNumber(ctx, args[0], exc) : 0;
    return JSValueMakeBoolean(ctx, cid && gNativeResources.alive(cid));
}
static JSValueRef cbNativeResourceRelease(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                          const JSValueRef args[], JSValueRef* exc) {
    uint32_t cid = argc ? (uint32_t)JSValueToNumber(ctx, args[0], exc) : 0;
    return JSValueMakeBoolean(ctx, cid && gNativeResources.release(cid));
}
// adopt(软)引用的两个收尾桥:transferred=首绑时性质转硬(refs 不变),releaseAdopt=退还软引用。
static JSValueRef cbNativeResourceReleaseAdopt(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                               const JSValueRef args[], JSValueRef* exc) {
    uint32_t cid = argc ? (uint32_t)JSValueToNumber(ctx, args[0], exc) : 0;
    return JSValueMakeBoolean(ctx, cid && gNativeResources.releaseAdopt(cid));
}
static JSValueRef cbNativeResourceTransferred(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                              const JSValueRef args[], JSValueRef* exc) {
    uint32_t cid = argc ? (uint32_t)JSValueToNumber(ctx, args[0], exc) : 0;
    return JSValueMakeBoolean(ctx, cid && gNativeResources.transferAdopt(cid));
}
static JSValueRef cbNativeResourceStats(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t,
                                        const JSValueRef[], JSValueRef*) {
    macres::Stats stats = gNativeResources.stats(); JSObjectRef out = JSObjectMake(ctx, nullptr, nullptr);
    setJsNumber(ctx,out,"byteHits",stats.byteHits);setJsNumber(ctx,out,"byteMisses",stats.byteMisses);setJsNumber(ctx,out,"byteInflightJoins",stats.byteInflightJoins);
    setJsNumber(ctx,out,"imageHits",stats.imageHits);setJsNumber(ctx,out,"imageMisses",stats.imageMisses);setJsNumber(ctx,out,"imageInflightJoins",stats.imageInflightJoins);
    setJsNumber(ctx,out,"decodedImages",stats.decodedImages);setJsNumber(ctx,out,"decodeFailures",stats.decodeFailures);setJsNumber(ctx,out,"uploadedImages",stats.uploadedImages);
    setJsNumber(ctx,out,"uploadedBytes",stats.uploadedBytes);setJsNumber(ctx,out,"byteCacheBytes",stats.byteCacheBytes);setJsNumber(ctx,out,"gpuCacheBytes",stats.gpuCacheBytes);
    setJsNumber(ctx,out,"pendingUploadBytes",stats.pendingUploadBytes);setJsNumber(ctx,out,"byteEntries",stats.byteEntries);setJsNumber(ctx,out,"imageEntries",stats.imageEntries);setJsNumber(ctx,out,"pendingUploads",stats.pendingUploads);
    setJsNumber(ctx,out,"evictedImages",stats.evictedImages);setJsNumber(ctx,out,"evictedBytes",stats.evictedBytes);setJsNumber(ctx,out,"retainMisses",stats.retainMisses);setJsNumber(ctx,out,"releaseMisses",stats.releaseMisses);
    setJsNumber(ctx,out,"renderFrame",stats.renderFrame);setJsNumber(ctx,out,"protectedImages",stats.protectedImages);setJsNumber(ctx,out,"publishedRefs",stats.publishedRefs);setJsNumber(ctx,out,"adoptRefs",stats.adoptRefs);return out;
}
static JSValueRef cbEvalScript(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeUndefined(ctx);
    std::string path = jsToStd(ctx, args[0]);
    macres::Bytes d = loadAsset(path.c_str());
    if (!d) { JSStringRef m = JSStringCreateWithUTF8CString((std::string("__evalScript 找不到: ")+path).c_str());
        if (exc) *exc = JSValueMakeString(ctx, m); JSStringRelease(m); return JSValueMakeUndefined(ctx); }
    OpaqueJSString src{std::string((const char*)d->data(), d->size())};
    JSStringRef url = JSStringCreateWithUTF8CString(path.c_str());
    JSEvaluateScript(gCtx, &src, nullptr, url, 1, exc);
    JSStringRelease(url);
    return JSValueMakeUndefined(ctx);
}
// ---- timer 桥:Android 无 runloop timer,自建到期堆,drawFrame 帧头泵(精度一帧,与 setTimeout 期望一致) ----
struct HostTimer { int id; double dueMs; double intervalMs; bool repeat; JSValueRef fn; };
static int gTimerId = 0;
static std::vector<HostTimer> gTimers;
static size_t gLastTimerDueCount = 0;
static int gSlowTimerId = 0;
static double gSlowTimerMs = 0;
static double monotonicMs(){ return nowUs() / 1000.0; }
static JSValueRef cbSetTimer(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1 || !JSValueIsObject(ctx, args[0])) return JSValueMakeNumber(ctx, 0);
    JSObjectRef fn = JSValueToObject(ctx, args[0], exc);
    if (!fn || !JSObjectIsFunction(ctx, fn)) return JSValueMakeNumber(ctx, 0);
    double ms = argc>1 ? JSValueToNumber(ctx, args[1], exc) : 0;
    bool repeat = argc>2 && JSValueToBoolean(ctx, args[2]);
    JSValueProtect(gCtx, fn);
    int id = ++gTimerId;
    gTimers.push_back({id, monotonicMs() + std::max(0.0, ms), std::max(0.0, ms), repeat, fn});
    return JSValueMakeNumber(ctx, id);
}
static JSValueRef cbClearTimer(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeUndefined(ctx);
    int id = (int)JSValueToNumber(ctx, args[0], exc);
    for (auto it = gTimers.begin(); it != gTimers.end(); ++it)
        if (it->id == id) { JSValueUnprotect(gCtx, it->fn); gTimers.erase(it); break; }
    return JSValueMakeUndefined(ctx);
}
// __decodeScriptUTF8(arrayBuffer, offset, length) → string | null(≙ mac/iOS native_script_utf8::decode):脚本/文本字节直接由
// 引擎(QuickJS JS_NewStringLen / V8 String::NewFromUtf8)做 UTF-8 → 内部字符串,替代 game_host TextDecoder 垫片的逐字节 String.fromCharCode 拼接
// (2026-09-18 真机采样:__abToStr 占全部 JS 时间 9%,≈2.3s/局)。非法 UTF-8 返回 null,JS 侧保留原解码策略。
static JSValueRef cbDecodeScriptUTF8(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exception) {
    if (argc != 3 || !JSValueIsObject(ctx, args[0]) || !JSValueIsNumber(ctx, args[1]) || !JSValueIsNumber(ctx, args[2]))
        return JSValueMakeNull(ctx);
    JSValueRef error = nullptr;
    if (JSValueGetTypedArrayType(ctx, args[0], &error) != kJSTypedArrayTypeArrayBuffer) return JSValueMakeNull(ctx);
    JSObjectRef buffer = JSValueToObject(ctx, args[0], &error);
    const double offset = JSValueToNumber(ctx, args[1], &error), length = JSValueToNumber(ctx, args[2], &error);
    const size_t capacity = JSObjectGetArrayBufferByteLength(ctx, buffer, &error);
    if (error) { if (exception) *exception = error; return JSValueMakeUndefined(ctx); }
    if (!std::isfinite(offset) || !std::isfinite(length) || offset < 0 || length < 0 ||
        std::floor(offset) != offset || std::floor(length) != length ||
        offset > (double)capacity || length > (double)capacity - offset) return JSValueMakeNull(ctx);
    const size_t begin = (size_t)offset, count = (size_t)length;
    const auto* bytes = static_cast<const uint8_t*>(JSObjectGetArrayBufferBytesPtr(ctx, buffer, &error));
    if (error) { if (exception) *exception = error; return JSValueMakeUndefined(ctx); }
    if (count && !bytes) return JSValueMakeNull(ctx);
    if (!native_script_utf8::isValidUtf8(bytes + begin, count)) return JSValueMakeNull(ctx);
    return jsengine::makeStringUTF8(ctx, reinterpret_cast<const char*>(bytes + begin), count);
}

// ======================= JS 热点采样(排障专用,默认关)=======================
// jsPrelude 置 globalThis.__NATIVE_JS_PROFILE=1 后开启,每 30s 把累计前 30 名倒进 logcat([jsprof] 前缀),配套 tools 里的解析靠这些行。
//   · QuickJS 没有 profiler:JS_SetInterruptHandler 的周期回调(每 10000 个函数入口/循环回边一次)里按墙钟 ≥1ms 采一次
//     `new Error().stack`(重入 JS_Eval,QuickJS 解释器可重入,inHandler 防递归),按"自身(栈顶帧)/含子调用(栈内每帧去重)"聚合,
//     并记录采样落在宿主哪个阶段(tick/jobs/timer/queue/http/pointer)。开销 ≈ 每 1ms JS 时间 20-50µs。
//   · V8 走自带的 CpuProfiler(独立采样线程 1ms 一次,精确到函数+行),每次 dump 把 profile 树折叠成同样的 self/incl 两张表;
//     阶段归属改按宿主各阶段的墙钟累计给出(采样线程看不到宿主阶段)。
struct JsProfiler {
    bool enabled = false, inHandler = false;
    double lastSampleUs = 0, lastDumpUs = 0;
    uint32_t samples = 0;
    std::unordered_map<std::string, uint32_t> self, incl, byPhase;
    std::unordered_map<std::string, double> phaseWallMs;   // 宿主各阶段墙钟累计(V8 版的 phase 行)
#if PIXI_JS_ENGINE_V8
    v8::CpuProfiler* profiler = nullptr;
#endif
};
static JsProfiler gJsProf;
static const char* gJsPhase = "other";
static inline void jsProfPhaseAdd(const char* phase, double ms) { if (gJsProf.enabled) gJsProf.phaseWallMs[phase] += ms; }
#if !PIXI_JS_ENGINE_V8
static void jsProfRecord(const char* stack) {
    // 行格式:"    at name (file:line:col)";首行是我们自己的 <jsprof> eval 帧,跳过;列号去掉以便按行聚合
    std::vector<std::string> frames;
    const char* p = stack;
    while (*p) {
        const char* e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        std::string line(p, n);
        p += n; if (*p == '\n') ++p;
        const size_t at = line.find("at ");
        if (at == std::string::npos) continue;
        line = line.substr(at + 3);
        if (line.find("<jsprof>") != std::string::npos) continue;
        const size_t paren = line.rfind(')');
        if (paren != std::string::npos) {   // 去列号:name (file:line:col) → name (file:line)
            const size_t c2 = line.rfind(':', paren), c1 = c2 == std::string::npos ? c2 : line.rfind(':', c2 - 1);
            if (c1 != std::string::npos && c2 != std::string::npos && c1 > line.find('(')) line = line.substr(0, c2) + ")";
        }
        frames.push_back(line);
    }
    if (frames.empty()) return;
    gJsProf.samples++;
    gJsProf.self[frames[0]]++;
    gJsProf.byPhase[gJsPhase]++;
    std::vector<std::string> seen;
    for (const std::string& f : frames) {
        if (std::find(seen.begin(), seen.end(), f) != seen.end()) continue;   // 递归只计一次
        seen.push_back(f); gJsProf.incl[f]++;
    }
}
static int jsProfInterrupt(JSRuntime*, void*) {
    if (!gJsProf.enabled || gJsProf.inHandler || !gCtx) return 0;
    const double now = nowUs();
    if (now - gJsProf.lastSampleUs < 1000.0) return 0;
    gJsProf.lastSampleUs = now;
    gJsProf.inHandler = true;
    JSContext* q = gCtx->q;
    JSValue v = JS_Eval(q, "new Error().stack", 17, "<jsprof>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { JSValue e = JS_GetException(q); JS_FreeValue(q, e); }
    else { const char* str = JS_ToCString(q, v); if (str) { jsProfRecord(str); JS_FreeCString(q, str); } }
    JS_FreeValue(q, v);
    gJsProf.inHandler = false;
    return 0;
}
#else
// 把 CpuProfile 树折叠成 self/incl:节点键 = 函数名 (脚本:行);"(root)/(program)/(idle)/(garbage collector)" 元节点单独计数
static void jsProfWalkV8(const v8::CpuProfileNode* node, std::vector<std::string>& path) {
    const char* fn = node->GetFunctionNameStr();
    const char* res = node->GetScriptResourceNameStr();
    std::string key = fn && *fn ? fn : "(anonymous)";
    const bool meta = key.size() > 1 && key.front() == '(' && key.back() == ')' && (!res || !*res);
    if (!meta) { key += " ("; key += res && *res ? res : "?"; key += ":" + std::to_string(node->GetLineNumber()) + ")"; }
    const unsigned hits = node->GetHitCount();
    if (hits) {
        if (meta) gJsProf.byPhase[key] += hits;
        else {
            gJsProf.samples += hits;
            gJsProf.self[key] += hits;
            std::vector<const std::string*> seen;
            bool selfSeen = false;
            for (const std::string& f : path) {
                bool dup = false;
                for (const std::string* q : seen) if (*q == f) { dup = true; break; }
                if (dup) continue;   // 递归只计一次
                seen.push_back(&f); gJsProf.incl[f] += hits;
                if (f == key) selfSeen = true;
            }
            if (!selfSeen) gJsProf.incl[key] += hits;
        }
    }
    if (!meta) path.push_back(key);
    for (int i = 0, n = node->GetChildrenCount(); i < n; ++i) jsProfWalkV8(node->GetChild(i), path);
    if (!meta) path.pop_back();
}
static v8::Local<v8::String> jsProfTitle() { return v8::String::NewFromUtf8Literal(gCtx->iso, "jsprof"); }
static void jsProfCollectV8() {   // 停掉当前窗口的 profile 折叠进累计表,再起下一窗口
    if (!gJsProf.profiler || !gCtx) return;
    v8::HandleScope hs(gCtx->iso);
    v8::CpuProfile* prof = gJsProf.profiler->StopProfiling(jsProfTitle());
    if (prof) {
        std::vector<std::string> path;
        jsProfWalkV8(prof->GetTopDownRoot(), path);
        prof->Delete();
    }
    gJsProf.profiler->StartProfiling(jsProfTitle(), false);
}
#endif
static void jsProfDump(const char* why) {
#if PIXI_JS_ENGINE_V8
    jsProfCollectV8();
#endif
    if (!gJsProf.enabled || !gJsProf.samples) return;
    auto top = [](const std::unordered_map<std::string, uint32_t>& m, size_t n) {
        std::vector<std::pair<std::string, uint32_t>> v(m.begin(), m.end());
        std::partial_sort(v.begin(), v.begin() + std::min(n, v.size()), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (v.size() > n) v.resize(n);
        return v;
    };
    ALOGI("[jsprof] ==== %s: %u samples(≈%u ms JS, %s) ====", why, gJsProf.samples, gJsProf.samples, jsengine::kName);
    std::string phases;
    for (auto& kv : top(gJsProf.byPhase, 8)) phases += kv.first + "=" + std::to_string(kv.second) + " ";
    ALOGI("[jsprof] phase %s", phases.c_str());
    if (!gJsProf.phaseWallMs.empty()) {
        std::string wall;
        for (auto& kv : gJsProf.phaseWallMs) { char b[64]; snprintf(b, sizeof(b), "%s=%.0fms ", kv.first.c_str(), kv.second); wall += b; }
        ALOGI("[jsprof] phase(wall) %s", wall.c_str());
    }
    for (auto& kv : top(gJsProf.self, 30)) ALOGI("[jsprof] self %5u %4.1f%% %s", kv.second, kv.second * 100.0 / gJsProf.samples, kv.first.c_str());
    for (auto& kv : top(gJsProf.incl, 40)) ALOGI("[jsprof] incl %5u %4.1f%% %s", kv.second, kv.second * 100.0 / gJsProf.samples, kv.first.c_str());
#if PIXI_JS_ENGINE_V8
    // native 回调按注册名的精确计时(CpuProfiler 里所有 API 回调共用 HandleApiCall 一段代码,只会显示成最后注册的那个名字)
    if (gCtx) {
        std::vector<ShimCallback*> cbs(gCtx->callbacks.begin(), gCtx->callbacks.end());
        std::partial_sort(cbs.begin(), cbs.begin() + std::min<size_t>(20, cbs.size()), cbs.end(), [](ShimCallback* a, ShimCallback* b) { return a->us > b->us; });
        double total = 0; uint64_t calls = 0;
        for (ShimCallback* cb : gCtx->callbacks) { total += cb->us; calls += cb->calls; }
        ALOGI("[jsprof] native 回调累计 %.0fms / %llu 次(含嵌套回 JS):", total / 1000.0, (unsigned long long)calls);
        for (size_t i = 0; i < std::min<size_t>(20, cbs.size()); ++i)
            if (cbs[i]->us > 0) ALOGI("[jsprof] native %8.1fms %8llu次 %6.1fµs/次 %s", cbs[i]->us / 1000.0, (unsigned long long)cbs[i]->calls, cbs[i]->us / (double)cbs[i]->calls, cbs[i]->name.c_str());
    }
#endif
}
static void jsProfMaybeEnable() {   // setupGame 末尾:prelude 已求值
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef key = JSStringCreateWithUTF8CString("__NATIVE_JS_PROFILE");
    JSValueRef v = JSObjectGetProperty(gCtx, global, key, nullptr); JSStringRelease(key);
#if PIXI_JS_ENGINE_V8
    if (gJsProf.profiler) {   // reload:上一世代的 profiler。采样线程还在跑时直接 Dispose 会让 v8:ProfEvntProc 线程 SIGABRT,先停
        v8::HandleScope hs(gCtx->iso);
        if (v8::CpuProfile* prof = gJsProf.profiler->StopProfiling(jsProfTitle())) prof->Delete();
        gJsProf.profiler->Dispose(); gJsProf.profiler = nullptr;
    }
    shimRuntime().traceNativeCalls = false;
#endif
    gJsProf = JsProfiler();
    gJsProf.enabled = v && JSValueToBoolean(gCtx, v);
    if (!gJsProf.enabled) return;
#if PIXI_JS_ENGINE_V8
    v8::HandleScope hs(gCtx->iso);
    gJsProf.profiler = v8::CpuProfiler::New(gCtx->iso);
    gJsProf.profiler->SetSamplingInterval(1000);
    gJsProf.profiler->StartProfiling(jsProfTitle(), false);
    shimRuntime().traceNativeCalls = true;
    ALOGI("[jsprof] enabled: V8 CpuProfiler 1ms sampling + native 回调按名计时");
#else
    JS_SetInterruptHandler(gCtx->rt, jsProfInterrupt, nullptr); ALOGI("[jsprof] enabled: 1ms wall-clock sampling via interrupt handler");
#endif
}

static void pumpTimers() {   // drawFrame 帧头调用;回调内新增的 timer 下一帧才触发(快照)
    gLastTimerDueCount = 0; gSlowTimerId = 0; gSlowTimerMs = 0;
    if (!gCtx || gTimers.empty()) return;
    const double now = monotonicMs();
    std::vector<HostTimer> due;
    for (size_t i = 0; i < gTimers.size();) {
        if (gTimers[i].dueMs > now) { ++i; continue; }
        HostTimer t = gTimers[i];
        if (t.repeat) { gTimers[i].dueMs = now + std::max(1.0, t.intervalMs); ++i; }
        else gTimers.erase(gTimers.begin() + (long)i);
        due.push_back(t);
    }
    for (auto& t : due) {
        ++gLastTimerDueCount;
        double timerStart = nowUs();
        // 调用期间额外持一份引用:QuickJS 的 JS_Call 不给被调函数保活(JSC 会),setInterval 回调里
        // clearInterval(自身)→ cbClearTimer 立即 Unprotect → 引用计数归零 → 正在执行的字节码被释放
        // (2026-09-03 模拟器进 M29 后 SIGSEGV 于 OP_push_atom_value,栈 pumpTimers→JS_Call)。
        JSValueProtect(gCtx, t.fn);
        JSValueRef e = nullptr;
        JSObjectCallAsFunction(gCtx, t.fn, nullptr, 0, nullptr, &e);
        double timerMs = (nowUs()-timerStart)/1000.0;
        if (timerMs > gSlowTimerMs) { gSlowTimerMs = timerMs; gSlowTimerId = t.id; }
        if (e) { std::string es = jsErrorDetail(gCtx, e); ALOGE("[timer] 异常: %s", es.c_str()); reportRuntimeError("javascript_runtime", "timer_exception", es, false); }
        if (!gCtx) break;   // 回调里触发了 reload:上下文已换,后续 timer 与引用都已由 clearAllTimers 处理
        JSValueUnprotect(gCtx, t.fn);
        if (!t.repeat) JSValueUnprotect(gCtx, t.fn);
    }
}
static void clearAllTimers() {
    if (gCtx) for (auto& t : gTimers) JSValueUnprotect(gCtx, t.fn);
    gTimers.clear();
}
static JSValueRef cbUnhandledRejection(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                       const JSValueRef args[], JSValueRef* exc) {
    if (argc >= 2) {
        std::string reason = jsToStd(ctx, args[1]);
        std::string stack;
        if (JSValueIsObject(ctx, args[1])) {
            JSObjectRef o = JSValueToObject(ctx, args[1], nullptr);
            JSStringRef sk = JSStringCreateWithUTF8CString("stack");
            JSValueRef sv = JSObjectGetProperty(ctx, o, sk, nullptr); JSStringRelease(sk);
            if (sv && !JSValueIsUndefined(ctx, sv)) stack = jsToStd(ctx, sv);
        }
        ALOGE("[js] [unhandledRejection] %s\n%s", reason.c_str(), stack.c_str());
        reportRuntimeError("javascript_runtime", "unhandled_rejection", jsErrorDetail(ctx, args[1]), false);
    }
    return JSValueMakeUndefined(ctx);
}
// ---- XMLHttpRequest 桥(Kotlin 共享 OkHttp 异步;GET 静态资源本地/LAN 命中) ----
struct HttpResult { int id, status; std::string text, headers; macres::Bytes body; };
static std::mutex gHttpMutex;
static std::vector<HttpResult> gHttpResults;
static void callXhrDone(int reqId, int status, const std::string& statusText, const std::string& respHeadersJson, const macres::Bytes& body) {
    if (!gCtx) return;
    ALOGI("[xhr-done] #%d status=%d bytes=%zu", reqId, status, body ? body->size() : (size_t)0);   // ≙ mac [xhr-done]
    JSValueRef bodyVal;
    if (body && !body->empty()) {
        size_t len = body->size(); void* buf = malloc(len); memcpy(buf, body->data(), len);
        JSValueRef e = nullptr;
        bodyVal = JSObjectMakeArrayBufferWithBytesNoCopy(gCtx, buf, len, [](void* p, void*){ free(p); }, nullptr, &e);
        if (e) bodyVal = JSValueMakeNull(gCtx);
    } else {
        JSStringRef empty = JSStringCreateWithUTF8CString(""); bodyVal = JSValueMakeString(gCtx, empty); JSStringRelease(empty);
    }
    JSStringRef stStr = JSStringCreateWithUTF8CString(statusText.c_str());
    JSStringRef rhStr = JSStringCreateWithUTF8CString(respHeadersJson.empty() ? "{}" : respHeadersJson.c_str());
    JSValueRef args[5] = { JSValueMakeNumber(gCtx, reqId), JSValueMakeNumber(gCtx, status),
                           JSValueMakeString(gCtx, stStr), JSValueMakeString(gCtx, rhStr), bodyVal };
    JSStringRelease(stStr); JSStringRelease(rhStr);
    callJsGlobal("__xhrDone", 5, args);
}
static void pumpHttpResults() {   // drawFrame 帧头泵(Kotlin io 线程回投)
    std::vector<HttpResult> batch;
    {
        std::lock_guard<std::mutex> lock(gHttpMutex);
        batch.swap(gHttpResults);
    }
    for (auto& r : batch) callXhrDone(r.id, r.status, r.text, r.headers, r.body);
}
static JSValueRef cbXhrSend(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
    if (argc < 3) return JSValueMakeUndefined(ctx);
    int reqId = (int)JSValueToNumber(ctx, args[0], exc);
    std::string method = jsToStd(ctx, args[1]);
    std::string urlStr = jsToStd(ctx, args[2]);
    std::string headersJson = (argc > 3) ? jsToStd(ctx, args[3]) : "{}";
    macres::Bytes body = (argc > 4) ? jsToBytes(ctx, args[4], exc) : nullptr;
    bool isGet = method.size() == 3 && (method[0]=='G'||method[0]=='g') && (method[1]=='E'||method[1]=='e') && (method[2]=='T'||method[2]=='t');
    if (isGet && urlStr.rfind("http://", 0) != 0 && urlStr.rfind("https://", 0) != 0) {
        const std::string rel = relOf(urlStr);
        if (!rel.empty()) {
            // 本地优先(与 mac/iOS cbXhrSend 同序):热更新删除标记 → 404(js/dev_debug.js 非 prod 环境保留,≙ shouldKeepBundledDebugAsset);
            // 日志曾让人误判 js/lobby.js 走了网络(2026-09-03 模拟器排查)。

            macres::Bytes local;


            if (!local) local = localAssetData(rel);
            if (local) {
                ALOGI("[xhr-local] #%d %zu B %.120s", reqId, local->size(), rel.c_str());   // ≙ mac [xhr-local]
                gMainQ.post([reqId, local]{ callXhrDone(reqId, 200, "OK", "{}", local); });
                return JSValueMakeUndefined(ctx);
            }
            // GET 静态资源未命中 → 改走资源镜像 gRemote/<rel>(CDN,或开发/模拟器的局域网镜像);未配置镜像时按原 URL 直连。
            if (!gRemote.empty()) urlStr = gRemote + "/" + rel;
        }
    }
    ALOGI("[xhr-net] #%d %s %.140s", reqId, method.c_str(), urlStr.c_str());   // 走网络(本地未命中/POST),≙ mac [xhr-net]
    JNIEnv* env = jniEnv();
    if (!env || !gActivity || !gMidStartHttp) {
        gMainQ.post([reqId]{ callXhrDone(reqId, 0, "", "{}", nullptr); });
        return JSValueMakeUndefined(ctx);
    }
    jstring jm = jstr(env, method.empty() ? "GET" : method), ju = jstr(env, urlStr), jh = jstr(env, headersJson);
    jbyteArray jb = nullptr;
    if (body && !body->empty()) {
        jb = env->NewByteArray((jsize)body->size());
        env->SetByteArrayRegion(jb, 0, (jsize)body->size(), (const jbyte*)body->data());
    }
    env->CallVoidMethod(gActivity, gMidStartHttp, reqId, jm, ju, jh, jb);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(jm); env->DeleteLocalRef(ju); env->DeleteLocalRef(jh);
    if (jb) env->DeleteLocalRef(jb);
    return JSValueMakeUndefined(ctx);
}

// ======================= FairyGUI 全量 native 化(native_fgui) =======================
// 与 ios_app.mm L1670-1834 同源:包解析/蓝图/实例全在 C++(cmdbuf/native_fgui.h 平台无关)。
// 平台差异:NSDictionary/NSString 缓存 → std 容器;NSXMLParser → nfgui::XmlParser(纯 C++);
// GCD → gLoadPool/gMainQ;NSString 的 UTF-16 子串 → utf16Advance(FairyGUI 包长度字段按
// JS substring 的 UTF-16 码元数计,std::string 字节计数会错位——这里按码元推进字节游标)。
struct NativeFguiPreparsed {
    std::unordered_map<std::string,std::string> entries;
    std::unique_ptr<nfgui::Package> blueprint;
    size_t compressedBytes=0; uint32_t compressedCrc=0; size_t entryCount=0;
    double inflateMs=0,xmlMs=0,blueprintMs=0;
    // hash → [(source, tree)] 预热的 XML 树(一次性消费)
    std::unordered_map<size_t,std::vector<std::pair<std::string,std::shared_ptr<nfgui::XmlNode>>>> xmlTrees;
};
static NativeFguiPreparseCounters gFguiPreparseStats;
static std::unordered_map<std::string,std::shared_ptr<NativeFguiPreparsed>> gFguiPreparseCache;   // LRU 24
static std::list<std::string> gFguiPreparseLru;
static std::unordered_map<size_t,std::vector<std::pair<std::string,std::shared_ptr<nfgui::XmlNode>>>> gFguiXmlPrewarm;
// 仅 JS 线程访问;asset worker 从不直接碰这些表。
static std::unordered_map<std::string,std::vector<NativeFguiPreparseWaiter>> gFguiPreparseInflight;

static std::shared_ptr<nfgui::XmlNode> parseNativeFguiXml(const std::string& source){
    if(source.empty())return nullptr;
    nfgui::XmlParser parser;auto root=parser.parse(source);
    return root?std::shared_ptr<nfgui::XmlNode>(std::move(root)):nullptr;
}
// UTF-8 字节流上前进 count 个 UTF-16 码元(BMP 外字符计 2),返回新字节位置;越界返回 npos。
static size_t utf16Advance(const std::string& s, size_t pos, long long count) {
    while (count > 0) {
        if (pos >= s.size()) return std::string::npos;
        unsigned char c = (unsigned char)s[pos];
        size_t bytes = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (pos + bytes > s.size()) return std::string::npos;
        count -= bytes == 4 ? 2 : 1;
        pos += bytes;
    }
    return pos;
}
// FairyGUI legacy 包 = raw deflate 包裹的 `name|UTF16长度|content` 串联。
static std::shared_ptr<NativeFguiPreparsed> parseNativeFguiArchive(const macres::Bytes& input,bool warmXml,const std::string& packageKey){
    if(!input||input->empty())return nullptr;double start=nowUs();z_stream zs;memset(&zs,0,sizeof(zs));
    zs.next_in=(Bytef*)input->data();zs.avail_in=(uInt)input->size();if(inflateInit2(&zs,-15)!=Z_OK)return nullptr;
    std::string source;source.resize(std::max((size_t)65536,input->size()*4));int result=Z_OK;
    while(result==Z_OK){if(zs.total_out>=source.size())source.resize(source.size()+std::max((size_t)65536,source.size()));zs.next_out=(Bytef*)source.data()+zs.total_out;zs.avail_out=(uInt)(source.size()-zs.total_out);result=inflate(&zs,Z_NO_FLUSH);}
    size_t produced=zs.total_out;inflateEnd(&zs);if(result!=Z_STREAM_END)return nullptr;source.resize(produced);
    auto parsed=std::make_shared<NativeFguiPreparsed>();parsed->compressedBytes=input->size();
    parsed->compressedCrc=(uint32_t)crc32(0,(const Bytef*)input->data(),(uInt)input->size());
    std::vector<std::string> xmlSources;
    size_t cursor=0,length=source.size();
    while(cursor<length){
        size_t p1=source.find('|',cursor);if(p1==std::string::npos)break;
        std::string name=source.substr(cursor,p1-cursor);cursor=p1+1;
        size_t p2=source.find('|',cursor);if(p2==std::string::npos)break;
        long long size=atoll(source.substr(cursor,p2-cursor).c_str());cursor=p2+1;
        if(size<0)break;size_t end=utf16Advance(source,cursor,size);if(end==std::string::npos)break;
        std::string value=source.substr(cursor,end-cursor);cursor=end;
        bool isXml=false;{size_t dot=name.find_last_of('.');if(dot!=std::string::npos){std::string ext=name.substr(dot+1);for(char& ch:ext)ch=(char)tolower((unsigned char)ch);isXml=ext=="xml";}}
        if(warmXml&&isXml)xmlSources.push_back(value);
        parsed->entries.emplace(std::move(name),std::move(value));
    }
    parsed->entryCount=parsed->entries.size();parsed->inflateMs=(nowUs()-start)/1000.0;
    if(parsed->entries.empty())return nullptr;
    if(warmXml&&!xmlSources.empty()){
        double xmlStart=nowUs();
        for(const std::string& xml:xmlSources){auto tree=parseNativeFguiXml(xml);if(!tree)continue;parsed->xmlTrees[std::hash<std::string>{}(xml)].push_back({xml,tree});}
        parsed->xmlMs=(nowUs()-xmlStart)/1000.0;
    }
    // 最贵的包/组件 XML 全量走查:刻意在 asset worker 上做;发布前不碰 live Runtime 表。
    double blueprintStart=nowUs();parsed->blueprint=nfgui::Runtime::buildPackage(packageKey,parsed->entries);parsed->blueprintMs=(nowUs()-blueprintStart)/1000.0;
    return parsed;
}
static void mergeNativeFguiXmlPrewarm(const std::shared_ptr<NativeFguiPreparsed>& parsed){
    if(parsed->xmlTrees.empty())return;
    for(auto& kv:parsed->xmlTrees){auto& bucket=gFguiXmlPrewarm[kv.first];gFguiPreparseStats.xmlWarmed+=kv.second.size();for(auto& rec:kv.second)bucket.push_back(std::move(rec));}
    parsed->xmlTrees.clear();
}
static std::shared_ptr<nfgui::XmlNode> takeNativeFguiXmlPrewarm(const std::string& source){
    if(source.empty()||gFguiXmlPrewarm.empty())return nullptr;
    auto bit=gFguiXmlPrewarm.find(std::hash<std::string>{}(source));if(bit==gFguiXmlPrewarm.end())return nullptr;
    auto& bucket=bit->second;
    for(size_t i=0;i<bucket.size();i++){
        if(bucket[i].first==source){auto tree=bucket[i].second;bucket.erase(bucket.begin()+(long)i);if(bucket.empty())gFguiXmlPrewarm.erase(bit);return tree;}
    }
    return nullptr;
}
static void cacheNativeFguiPreparse(const std::string& resKey,const std::shared_ptr<NativeFguiPreparsed>& parsed){
    if(resKey.empty())return;
    if(!gFguiPreparseCache.count(resKey))gFguiPreparseLru.push_back(resKey);
    else gFguiPreparseLru.remove(resKey),gFguiPreparseLru.push_back(resKey);
    gFguiPreparseCache[resKey]=parsed;
    while(gFguiPreparseLru.size()>24){gFguiPreparseCache.erase(gFguiPreparseLru.front());gFguiPreparseLru.pop_front();}
    mergeNativeFguiXmlPrewarm(parsed);
}
static JSObjectRef fguiXmlNodeToJs(JSContextRef ctx,const nfgui::XmlNode* node,JSValueRef* exc,size_t* nodeCount=nullptr){
    if(nodeCount)(*nodeCount)++;
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsProperty(ctx,out,"__nativeXml",JSValueMakeBoolean(ctx,true));setJsProperty(ctx,out,"nodeName",fguiJsString(ctx,node->name));setJsProperty(ctx,out,"type",JSValueMakeNumber(ctx,1));
    setJsProperty(ctx,out,"text",node->text.empty()?JSValueMakeNull(ctx):fguiJsString(ctx,node->text));
    JSObjectRef ao=JSObjectMake(ctx,nullptr,nullptr);for(const auto& kv:node->attrs)setJsProperty(ctx,ao,kv.first.c_str(),fguiJsString(ctx,kv.second));setJsProperty(ctx,out,"$attributes",ao);setJsProperty(ctx,out,"attributes",ao);
    std::vector<JSValueRef> values;values.reserve(node->children.size());for(const auto& child:node->children)if(child)values.push_back(fguiXmlNodeToJs(ctx,child.get(),exc,nodeCount));JSObjectRef ca=JSObjectMakeArray(ctx,values.size(),values.empty()?nullptr:values.data(),exc);setJsProperty(ctx,out,"$children",ca);setJsProperty(ctx,out,"children",ca);return out;
}
static JSValueRef cbNativeFguiXmlParse(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<1)return JSValueMakeNull(ctx);std::string source=jsToStd(ctx,a[0]);if(source.empty())return JSValueMakeNull(ctx);
    std::shared_ptr<nfgui::XmlNode> tree=takeNativeFguiXmlPrewarm(source);if(tree)gFguiPreparseStats.xmlHits++;else{gFguiPreparseStats.xmlFallbacks++;tree=parseNativeFguiXml(source);}
    if(!tree)return JSValueMakeNull(ctx);double bridgeStart=nowUs();size_t nodes=0;JSValueRef out=fguiXmlNodeToJs(ctx,tree.get(),exc,&nodes);double bridgeMs=(nowUs()-bridgeStart)/1000.0;
    gFguiPreparseStats.xmlBridgeNodes+=nodes;gFguiPreparseStats.xmlBridgeMs+=bridgeMs;gFguiPreparseStats.maxXmlBridgeMs=std::max(gFguiPreparseStats.maxXmlBridgeMs,bridgeMs);return out;
}
static JSValueRef nativeFguiEntriesToJs(JSContextRef ctx,const std::shared_ptr<NativeFguiPreparsed>& parsed,const std::string& resKey,bool cacheHit,JSValueRef* exc){
    if(!parsed)return JSValueMakeNull(ctx);const std::string& packageKey=resKey;bool registered=gNativeFgui.package(packageKey)!=nullptr;
    if(!registered&&!parsed->blueprint&&parsed->entries.empty())return JSValueMakeNull(ctx);JSObjectRef map=JSObjectMake(ctx,nullptr,nullptr);
    if(!registered&&parsed->blueprint){double publishStart=nowUs();registered=gNativeFgui.installPackage(std::move(parsed->blueprint),&gSG);gFguiPreparseStats.blueprintPublishMs+=(nowUs()-publishStart)/1000.0;if(registered)gFguiPreparseStats.blueprintPublishes++;}
    if(!registered){double buildStart=nowUs();registered=gNativeFgui.registerPackage(packageKey,parsed->entries,&gSG);gFguiPreparseStats.syncBlueprintMs+=(nowUs()-buildStart)/1000.0;gFguiPreparseStats.syncBlueprintBuilds++;}
    // 缓存蓝图能活过 JS 侧的包移除;重新 add 必须让它的 exporter id 重新生效。
    if(registered)gNativeFgui.activatePackage(packageKey);
    // 注册成功只发布极小的懒描述符 token;失败才把全部 entries 拷进 JS(慢路径)。
    if(registered&&!resKey.empty())setJsProperty(ctx,map,"__nativePackageKey",fguiJsString(ctx,resKey));
    else for(const auto& entry:parsed->entries){JSStringRef key=JSStringCreateWithUTF8CString(entry.first.c_str());JSStringRef value=JSStringCreateWithUTF8CString(entry.second.c_str());JSObjectSetProperty(ctx,map,key,JSValueMakeString(ctx,value),kJSPropertyAttributeNone,exc);JSStringRelease(value);JSStringRelease(key);}
    setJsProperty(ctx,map,"__nativeEntryCount",JSValueMakeNumber(ctx,(double)parsed->entryCount));
    setJsProperty(ctx,map,"__nativeBlueprint",JSValueMakeBoolean(ctx,registered));
    setJsProperty(ctx,map,"__nativePreparseHit",JSValueMakeBoolean(ctx,cacheHit));
    setJsProperty(ctx,map,"__nativeInflateMs",JSValueMakeNumber(ctx,parsed->inflateMs));
    // 安装的 Package 已持有描述符;丢弃重复字符串,保留 bytes/CRC/count 供缓存校验。
    if(registered){std::unordered_map<std::string,std::string>().swap(parsed->entries);}return map;
}
static JSValueRef cbNativeFguiEntry(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef*){
    if(argc<2)return JSValueMakeNull(ctx);std::string packageKey=jsToStd(ctx,a[0]),name=jsToStd(ctx,a[1]);gFguiPreparseStats.entryGets++;
    const std::string* value=gNativeFgui.descriptor(packageKey,name);if(!value){gFguiPreparseStats.entryMisses++;return JSValueMakeNull(ctx);}
    gFguiPreparseStats.entryBytes+=value->size();JSStringRef text=JSStringCreateWithUTF8CString(value->c_str());JSValueRef out=JSValueMakeString(ctx,text);JSStringRelease(text);return out;
}
static JSValueRef cbNativeFguiInflate(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<1)return JSValueMakeNull(ctx);std::string resKey=argc>1?jsToStd(ctx,a[1]):std::string();
    auto cit=resKey.empty()?gFguiPreparseCache.end():gFguiPreparseCache.find(resKey);
    if(cit!=gFguiPreparseCache.end()){auto cached=cit->second;gFguiPreparseStats.syncHits++;if(getenv("FGUI_PREPARSE_LOG"))ALOGI("[fgui-preparse/android] HIT %s entries=%zu inflate=%.2fms",resKey.c_str(),cached->entryCount,cached->inflateMs);double bridgeStart=nowUs();JSValueRef out=nativeFguiEntriesToJs(ctx,cached,resKey,true,exc);gFguiPreparseStats.inflateBridgeMs+=(nowUs()-bridgeStart)/1000.0;return out;}
    gFguiPreparseStats.syncFallbacks++;macres::Bytes input=jsToBytes(ctx,a[0],exc);if(!input||input->empty())return JSValueMakeNull(ctx);
    auto parsed=parseNativeFguiArchive(input,false,resKey);if(!parsed){gFguiPreparseStats.failures++;return JSValueMakeNull(ctx);}
    gFguiPreparseStats.syncInflateMs+=parsed->inflateMs;if(parsed->blueprint){gFguiPreparseStats.syncBlueprintMs+=parsed->blueprintMs;gFguiPreparseStats.syncBlueprintBuilds++;}gFguiPreparseStats.lastInflateMs=parsed->inflateMs;
    if(getenv("FGUI_PREPARSE_LOG"))ALOGI("[fgui-preparse/android] FALLBACK %s entries=%zu inflate=%.2fms",resKey.c_str(),parsed->entryCount,parsed->inflateMs);
    double bridgeStart=nowUs();JSValueRef out=nativeFguiEntriesToJs(ctx,parsed,resKey,false,exc);gFguiPreparseStats.inflateBridgeMs+=(nowUs()-bridgeStart)/1000.0;return out;
}
static void notifyNativeFguiPreparse(const NativeFguiPreparseWaiter& waiter,bool ok,const std::shared_ptr<NativeFguiPreparsed>& parsed){
    if(!gCtx||gCtx!=waiter.context)return;
    JSValueRef args[5]={JSValueMakeNumber(gCtx,waiter.token),JSValueMakeBoolean(gCtx,ok),JSValueMakeNumber(gCtx,parsed?parsed->inflateMs:0),JSValueMakeNumber(gCtx,parsed?parsed->xmlMs:0),JSValueMakeNumber(gCtx,parsed?(double)parsed->entryCount:0)};
    callJsGlobal("__nativeFguiPreparseDone",5,args);
}
static JSValueRef cbNativeFguiPreparse(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<3)return JSValueMakeBoolean(ctx,false);int token=(int)JSValueToNumber(ctx,args[0],exc);std::string resKey=jsToStd(ctx,args[1]);macres::Bytes data=jsToBytes(ctx,args[2],exc);
    if(resKey.empty()||!data||data->empty())return JSValueMakeBoolean(ctx,false);gFguiPreparseStats.requests++;
    uint32_t dataCrc=(uint32_t)crc32(0,(const Bytef*)data->data(),(uInt)data->size());
    NativeFguiPreparseWaiter waiter{token,gCtx};
    auto cached=gFguiPreparseCache.find(resKey);
    if(cached!=gFguiPreparseCache.end()&&cached->second->compressedBytes==data->size()&&cached->second->compressedCrc==dataCrc){
        gFguiPreparseStats.preparseCacheHits++;auto hit=cached->second;
        gMainQ.post([waiter,hit]{notifyNativeFguiPreparse(waiter,true,hit);});
        return JSValueMakeBoolean(ctx,true);
    }
    std::string workKey=resKey+"#"+std::to_string(data->size())+"#"+std::to_string(dataCrc);
    auto active=gFguiPreparseInflight.find(workKey);
    if(active!=gFguiPreparseInflight.end()){active->second.push_back(waiter);gFguiPreparseStats.deduped++;return JSValueMakeBoolean(ctx,true);}
    gFguiPreparseInflight[workKey].push_back(waiter);gFguiPreparseStats.jobs++;
    JSGlobalContextRef requestContext=gCtx;
    gLoadPool.post([data,resKey,workKey,requestContext]{
        std::shared_ptr<NativeFguiPreparsed> parsed;
        {
            aplat::CpuGate::Scoped gate(gCpuGate);
            parsed=parseNativeFguiArchive(data,true,resKey);
        }
        gMainQ.post([parsed,resKey,workKey,requestContext]{
            auto found=gFguiPreparseInflight.find(workKey);if(found==gFguiPreparseInflight.end())return;
            std::vector<NativeFguiPreparseWaiter> waiters=std::move(found->second);gFguiPreparseInflight.erase(found);
            if(gCtx!=requestContext)return;
            bool ok=parsed!=nullptr;
            if(ok){cacheNativeFguiPreparse(resKey,parsed);gFguiPreparseStats.backgroundInflateMs+=parsed->inflateMs;gFguiPreparseStats.backgroundXmlMs+=parsed->xmlMs;gFguiPreparseStats.backgroundBlueprintMs+=parsed->blueprintMs;gFguiPreparseStats.lastInflateMs=parsed->inflateMs;}
            else gFguiPreparseStats.failures++;
            for(const auto& pending:waiters)notifyNativeFguiPreparse(pending,ok,parsed);
        });
    });
    return JSValueMakeBoolean(ctx,true);
}
static JSValueRef cbNativeFguiPreparseStats(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef*){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"requests",gFguiPreparseStats.requests);setJsNumber(ctx,out,"jobs",gFguiPreparseStats.jobs);setJsNumber(ctx,out,"deduped",gFguiPreparseStats.deduped);setJsNumber(ctx,out,"preparseCacheHits",gFguiPreparseStats.preparseCacheHits);setJsNumber(ctx,out,"syncHits",gFguiPreparseStats.syncHits);setJsNumber(ctx,out,"syncFallbacks",gFguiPreparseStats.syncFallbacks);setJsNumber(ctx,out,"failures",gFguiPreparseStats.failures);setJsNumber(ctx,out,"cacheEntries",gFguiPreparseCache.size());setJsNumber(ctx,out,"inflight",gFguiPreparseInflight.size());setJsNumber(ctx,out,"xmlWarmed",gFguiPreparseStats.xmlWarmed);setJsNumber(ctx,out,"xmlHits",gFguiPreparseStats.xmlHits);setJsNumber(ctx,out,"xmlFallbacks",gFguiPreparseStats.xmlFallbacks);setJsNumber(ctx,out,"xmlBridgeNodes",gFguiPreparseStats.xmlBridgeNodes);setJsNumber(ctx,out,"entryGets",gFguiPreparseStats.entryGets);setJsNumber(ctx,out,"entryMisses",gFguiPreparseStats.entryMisses);setJsNumber(ctx,out,"entryBytes",gFguiPreparseStats.entryBytes);setJsNumber(ctx,out,"blueprintPublishes",gFguiPreparseStats.blueprintPublishes);setJsNumber(ctx,out,"syncBlueprintBuilds",gFguiPreparseStats.syncBlueprintBuilds);setJsNumber(ctx,out,"manifestGets",gFguiPreparseStats.manifestGets);setJsNumber(ctx,out,"manifestMisses",gFguiPreparseStats.manifestMisses);setJsNumber(ctx,out,"manifestItems",gFguiPreparseStats.manifestItems);setJsNumber(ctx,out,"itemGets",gFguiPreparseStats.itemGets);setJsNumber(ctx,out,"itemMisses",gFguiPreparseStats.itemMisses);setJsNumber(ctx,out,"typeGets",gFguiPreparseStats.typeGets);setJsNumber(ctx,out,"spriteGets",gFguiPreparseStats.spriteGets);setJsNumber(ctx,out,"spriteMisses",gFguiPreparseStats.spriteMisses);setJsNumber(ctx,out,"backgroundInflateMs",gFguiPreparseStats.backgroundInflateMs);setJsNumber(ctx,out,"backgroundXmlMs",gFguiPreparseStats.backgroundXmlMs);setJsNumber(ctx,out,"backgroundBlueprintMs",gFguiPreparseStats.backgroundBlueprintMs);setJsNumber(ctx,out,"syncInflateMs",gFguiPreparseStats.syncInflateMs);setJsNumber(ctx,out,"syncBlueprintMs",gFguiPreparseStats.syncBlueprintMs);setJsNumber(ctx,out,"lastInflateMs",gFguiPreparseStats.lastInflateMs);setJsNumber(ctx,out,"blueprintPublishMs",gFguiPreparseStats.blueprintPublishMs);setJsNumber(ctx,out,"inflateBridgeMs",gFguiPreparseStats.inflateBridgeMs);setJsNumber(ctx,out,"xmlBridgeMs",gFguiPreparseStats.xmlBridgeMs);setJsNumber(ctx,out,"maxXmlBridgeMs",gFguiPreparseStats.maxXmlBridgeMs);setJsNumber(ctx,out,"manifestBridgeMs",gFguiPreparseStats.manifestBridgeMs);setJsNumber(ctx,out,"maxManifestBridgeMs",gFguiPreparseStats.maxManifestBridgeMs);setJsNumber(ctx,out,"spriteBridgeMs",gFguiPreparseStats.spriteBridgeMs);return out;
}
static JSValueRef cbNativeFguiSetStrings(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef*){
    if(argc<1)return JSValueMakeBoolean(ctx,false);std::string source=jsToStd(ctx,a[0]);bool ok=gNativeFgui.setStringsSource(source);if(ok)gNativeFgui.invalidateAllReservations(gSG);return JSValueMakeBoolean(ctx,ok);
}
static JSValueRef cbNativeFguiManifest(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<1)return JSValueMakeNull(ctx);std::string key=jsToStd(ctx,a[0]);gFguiPreparseStats.manifestGets++;double started=nowUs();
    nfgui::Package* pkg=gNativeFgui.package(key);if(!pkg){gFguiPreparseStats.manifestMisses++;return JSValueMakeNull(ctx);}
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsProperty(ctx,out,"id",fguiJsString(ctx,pkg->id));setJsProperty(ctx,out,"name",fguiJsString(ctx,pkg->name));setJsProperty(ctx,out,"lazy",JSValueMakeBoolean(ctx,true));setJsNumber(ctx,out,"itemCount",pkg->resourceOrder.size());
    gFguiPreparseStats.manifestItems+=pkg->resourceOrder.size();double elapsed=(nowUs()-started)/1000.0;gFguiPreparseStats.manifestBridgeMs+=elapsed;gFguiPreparseStats.maxManifestBridgeMs=std::max(gFguiPreparseStats.maxManifestBridgeMs,elapsed);return out;
}
static JSValueRef cbNativeFguiItem(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2)return JSValueMakeNull(ctx);std::string key=jsToStd(ctx,a[0]),query=jsToStd(ctx,a[1]);bool byName=argc>2&&JSValueToBoolean(ctx,a[2]);gFguiPreparseStats.itemGets++;
    nfgui::Package* pkg=gNativeFgui.package(key);if(!pkg){gFguiPreparseStats.itemMisses++;return JSValueMakeNull(ctx);}std::string id=query;
    if(byName){auto named=pkg->resourceIdByName.find(id);if(named==pkg->resourceIdByName.end()){gFguiPreparseStats.itemMisses++;return JSValueMakeNull(ctx);}id=named->second;}
    auto found=pkg->resourcesById.find(id);if(found==pkg->resourcesById.end()){gFguiPreparseStats.itemMisses++;return JSValueMakeNull(ctx);}return nativeFguiResourceRow(ctx,found->second,exc);
}
static JSValueRef cbNativeFguiItemsByType(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2)return JSValueMakeNull(ctx);std::string key=jsToStd(ctx,a[0]);int type=(int)JSValueToNumber(ctx,a[1],exc);gFguiPreparseStats.typeGets++;nfgui::Package* pkg=gNativeFgui.package(key);if(!pkg)return JSValueMakeNull(ctx);
    std::vector<JSValueRef> values;for(const std::string& id:pkg->resourceOrder){auto found=pkg->resourcesById.find(id);if(found!=pkg->resourcesById.end()&&found->second.packageItemType==type)values.push_back(fguiJsString(ctx,id));}return JSObjectMakeArray(ctx,values.size(),values.empty()?nullptr:values.data(),exc);
}
static JSValueRef cbNativeFguiSprite(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2)return JSValueMakeNull(ctx);std::string key=jsToStd(ctx,a[0]),item=jsToStd(ctx,a[1]);gFguiPreparseStats.spriteGets++;double started=nowUs();
    const nfgui::Sprite* sprite=gNativeFgui.sprite(key,item);if(!sprite){gFguiPreparseStats.spriteMisses++;return JSValueMakeNull(ctx);}
    JSValueRef values[6]={fguiJsString(ctx,sprite->atlasId),JSValueMakeNumber(ctx,sprite->x),JSValueMakeNumber(ctx,sprite->y),JSValueMakeNumber(ctx,sprite->width),JSValueMakeNumber(ctx,sprite->height),JSValueMakeBoolean(ctx,sprite->rotated)};
    JSObjectRef out=JSObjectMakeArray(ctx,6,values,exc);gFguiPreparseStats.spriteBridgeMs+=(nowUs()-started)/1000.0;return out;
}
static JSValueRef cbNativeFguiBindAtlas(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<6)return JSValueMakeBoolean(ctx,false);std::string pkg=jsToStd(ctx,a[0]),atlas=jsToStd(ctx,a[1]);int tex=(int)JSValueToNumber(ctx,a[2],exc);gNativeFgui.invalidateReservationsForPackage(pkg,gSG);gNativeFgui.bindAtlas(pkg,atlas,tex,(float)JSValueToNumber(ctx,a[3],exc),(float)JSValueToNumber(ctx,a[4],exc),(float)JSValueToNumber(ctx,a[5],exc),&gSG);return JSValueMakeBoolean(ctx,tex>0);
}
static JSValueRef cbNativeFguiPreparePackage(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3||!JSValueIsObject(ctx,a[1])||!JSValueIsObject(ctx,a[2])||JSValueGetTypedArrayType(ctx,a[2],exc)!=kJSTypedArrayTypeFloat32Array)return JSValueMakeBoolean(ctx,false);
    std::string pkg=jsToStd(ctx,a[0]);JSObjectRef ids=JSValueToObject(ctx,a[1],exc),meta=JSValueToObject(ctx,a[2],exc);
    JSStringRef lengthKey=JSStringCreateWithUTF8CString("length");size_t count=(size_t)JSValueToNumber(ctx,JSObjectGetProperty(ctx,ids,lengthKey,exc),exc);JSStringRelease(lengthKey);
    const float* values=(const float*)JSObjectGetTypedArrayBytesPtr(ctx,meta,exc);size_t valueCount=JSObjectGetTypedArrayLength(ctx,meta,exc);if(!values||valueCount<count*4)return JSValueMakeBoolean(ctx,false);
    std::vector<std::pair<std::string,nfgui::AtlasBinding>> atlases;atlases.reserve(count);
    for(size_t i=0;i<count;i++){
        JSValueRef idValue=JSObjectGetPropertyAtIndex(ctx,ids,(unsigned)i,exc);std::string idString=jsToStd(ctx,idValue);nfgui::AtlasBinding binding;binding.textureId=(int)values[i*4];binding.width=values[i*4+1];binding.height=values[i*4+2];binding.premul=values[i*4+3];atlases.emplace_back(idString,binding);
        if(std::getenv("NATIVE_ATLAS_TRACE")){
            GLuint texture=gDec.glTexture((uint32_t)std::max(0,binding.textureId));
            ALOGI("[native-atlas] package=%s atlas=%s cid=%d gl=%u reported=%.0fx%.0f premul=%.0f",pkg.c_str(),idString.c_str(),binding.textureId,(unsigned)texture,binding.width,binding.height,binding.premul);
        }
    }
    gNativeFgui.invalidateReservationsForPackage(pkg,gSG);
    return JSValueMakeBoolean(ctx,gNativeFgui.preparePackage(pkg,atlases,&gSG));
}
static JSValueRef cbNativeFguiReserve(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3)return JSValueMakeNumber(ctx,0);std::string pkg=jsToStd(ctx,a[0]),component=jsToStd(ctx,a[1]);
    double requested=JSValueToNumber(ctx,a[2],exc);size_t target=(size_t)std::max(0.0,std::min(64.0,requested));
    size_t available=gNativeFgui.reserve(pkg,component,target,gSG,(nowUs()-gStartUs)/1000.0,gNativeBatchSerial);
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"available",available);setJsProperty(ctx,out,"block",fguiJsString(ctx,gNativeFgui.reservationBlock()));return out;
}
static JSValueRef cbNativeFguiClearReservations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef*){
    if(argc<1)gNativeFgui.invalidateAllReservations(gSG);else{std::string pkg=jsToStd(ctx,a[0]);gNativeFgui.invalidateReservationsForPackage(pkg,gSG);}return JSValueMakeUndefined(ctx);
}
static JSValueRef cbNativeFguiCreate(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2)return JSValueMakeNull(ctx);std::string pkg=jsToStd(ctx,a[0]),component=jsToStd(ctx,a[1]);std::string expectedSchema;if(argc>2&&!JSValueIsNull(ctx,a[2])&&!JSValueIsUndefined(ctx,a[2])){expectedSchema=jsToStd(ctx,a[2]);}double t0=nowUs(),ageNowMs=(t0-gStartUs)/1000.0;bool claimedReservation=false;int id=gNativeFgui.create(pkg,component,gSG,ageNowMs,&claimedReservation,gNativeBatchSerial);JSObjectRef out=fguiInstanceResultToJs(ctx,id,(nowUs()-t0)/1000.0,ageNowMs,claimedReservation,false,exc,expectedSchema.empty()?nullptr:&expectedSchema);if(out)return out;JSObjectRef failure=JSObjectMake(ctx,nullptr,nullptr);setJsProperty(ctx,failure,"compatible",JSValueMakeBoolean(ctx,false));setJsProperty(ctx,failure,"fallbackReason",fguiJsString(ctx,gNativeFgui.createFailure().empty()?"create-unavailable":gNativeFgui.createFailure()));return failure;
}


static bool fguiJsStringArray(JSContextRef ctx,JSValueRef value,size_t exact,
                              std::vector<std::string>& out,JSValueRef* exc){
    if(!JSValueIsArray(ctx,value))return false;JSObjectRef array=JSValueToObject(ctx,value,exc);if(!array)return false;
    JSStringRef key=JSStringCreateWithUTF8CString("length");double rawLength=JSValueToNumber(ctx,JSObjectGetProperty(ctx,array,key,exc),exc);JSStringRelease(key);
    if(!std::isfinite(rawLength)||rawLength<0||std::floor(rawLength)!=rawLength||rawLength>4096)return false;
    size_t length=(size_t)rawLength;if(exact!=std::numeric_limits<size_t>::max()&&length!=exact)return false;
    out.clear();out.reserve(length);for(size_t i=0;i<length;++i){JSValueRef item=JSObjectGetPropertyAtIndex(ctx,array,(unsigned)i,exc);if(!JSValueIsString(ctx,item))return false;std::string text=jsToStd(ctx,item);if(text.size()>65536)return false;out.push_back(std::move(text));}
    return !(exc&&*exc);
}
// One exact store transaction replaces 18 promote crossings, 18 reparents, controller setters and
// 18 attributed-text raster bridge calls. Every structural operand is validated before 原生文本;
// structure is published only after all fresh text surfaces exist successfully.

// Exact live update for a num_con surface created by the direct checkout commit.  Ordinary live
// fixed style arrays as commit and receive four unboxed metrics (width/height/lineHeight/scale).

static JSValueRef cbNativeFguiChild(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3)return JSValueMakeNull(ctx);int inst=(int)JSValueToNumber(ctx,a[0],exc),parent=(int)JSValueToNumber(ctx,a[1],exc);nfgui::InstanceNode* n=nullptr;if(JSValueIsNumber(ctx,a[2]))n=gNativeFgui.child(inst,parent,"",(int)JSValueToNumber(ctx,a[2],exc));else{std::string key=jsToStd(ctx,a[2]);n=gNativeFgui.child(inst,parent,key.c_str());}JSObjectRef out=fguiNodeToJs(ctx,n,exc);return out?out:JSValueMakeNull(ctx);
}
// Exact lightweight lookup for a metadata schema already validated by fguiInstanceSchemaToken.
// Returning only the numeric slot avoids rebuilding attrs/controller/transition objects for the
// same hot-store path eight more times. The pristine bit, expectedId and owner check make any
// runtime-mutated facade state fail closed; JavaScript then requests authoritative full metadata.

// Capability-checked lightweight text mutation used by closed FGUI refs. Both bitmap and 原生文本
// paths live in Runtime so semantic attrs, ownership and metrics commit or roll back together.
static JSValueRef cbNativeFguiSetTextMutation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,
                                              const JSValueRef a[],JSValueRef* exc){
    if(argc!=4||!JSValueIsNumber(ctx,a[0])||!JSValueIsNumber(ctx,a[1])||
       !JSValueIsString(ctx,a[2])||!JSValueIsNumber(ctx,a[3]))return JSValueMakeBoolean(ctx,false);
    double rawInstance=JSValueToNumber(ctx,a[0],exc),rawNode=JSValueToNumber(ctx,a[1],exc),
           rawBold=JSValueToNumber(ctx,a[3],exc);
    if((exc&&*exc)||!std::isfinite(rawInstance)||!std::isfinite(rawNode)||
       !std::isfinite(rawBold)||rawInstance<=0||rawInstance>INT_MAX||rawNode<=0||
       rawNode>INT_MAX||std::floor(rawInstance)!=rawInstance||std::floor(rawNode)!=rawNode||
       (rawBold!=0&&rawBold!=1))return JSValueMakeBoolean(ctx,false);
    std::string text=jsToStd(ctx,a[2]);
    return JSValueMakeBoolean(ctx,gNativeFgui.setTextMutation((int)rawInstance,(int)rawNode,
                                                              text,(int)rawBold,gSG));
}
// One synchronous bridge publishes last-write-wins text mutations from any compatible FGUI
// transaction. Runtime preflights capability/ownership for every row and rolls earlier rows back
// if a later surface fails; JavaScript replays singles on a false result.
static JSValueRef cbNativeFguiCommitTextMutations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,
                                                  const JSValueRef a[],JSValueRef* exc){
    const double started=nowUs();++gFguiTextMutationCalls;
    auto finish=[&](bool ok,size_t rows){
        gFguiTextMutationRows+=rows;if(!ok)++gFguiTextMutationFailures;
        gFguiTextMutationMs+=(nowUs()-started)/1000.0;return JSValueMakeBoolean(ctx,ok);
    };
    if(argc!=2||!JSValueIsObject(ctx,a[0])||!JSValueIsArray(ctx,a[1]))return finish(false,0);
    JSObjectRef words=JSValueToObject(ctx,a[0],exc);
    if(!words||(exc&&*exc)||JSValueGetTypedArrayType(ctx,a[0],exc)!=kJSTypedArrayTypeInt32Array)
        return finish(false,0);
    const size_t wordCount=JSObjectGetTypedArrayLength(ctx,words,exc);
    const int32_t* raw=(const int32_t*)JSObjectGetTypedArrayBytesPtr(ctx,words,exc);
    if((exc&&*exc)||!raw||wordCount<3||wordCount>512*3||wordCount%3)return finish(false,0);
    const size_t rowCount=wordCount/3;
    std::vector<std::string> texts;
    if(!fguiJsStringArray(ctx,a[1],rowCount,texts,exc))return finish(false,rowCount);
    std::vector<nfgui::TextMutationWrite> writes;writes.reserve(rowCount);
    for(size_t i=0;i<rowCount;++i){
        const int instance=raw[i*3],node=raw[i*3+1],bold=raw[i*3+2];
        if(instance<=0||node<=0||(bold!=0&&bold!=1))return finish(false,rowCount);
        nfgui::TextMutationWrite write;write.instanceId=instance;write.nativeId=node;
        write.boldMode=bold;write.text=std::move(texts[i]);writes.push_back(std::move(write));
    }
    return finish(gNativeFgui.commitTextMutations(writes,gSG),rowCount);
}
static JSValueRef cbNativeFguiSetController(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);std::string name=jsToStd(ctx,a[2]);bool ok=gNativeFgui.setController((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.c_str(),(int)JSValueToNumber(ctx,a[3],exc),gSG);return JSValueMakeBoolean(ctx,ok);
}
// Fixed row layout: Int32[kind,instance,target,value0,value1,value2], Float64[number0,number1],
// and one controller-name string per row.  Keeping all three arrays exact-sized lets C++ reject a
// torn/malformed transaction before Runtime sees any live mutation.
static JSValueRef cbNativeFguiCommitMutations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,
                                               const JSValueRef a[],JSValueRef* exc){
    const double started=nowUs();++gFguiOrderedMutationCalls;
    auto finish=[&](bool ok,size_t rows){
        gFguiOrderedMutationRows+=rows;if(!ok)++gFguiOrderedMutationFailures;
        gFguiOrderedMutationMs+=(nowUs()-started)/1000.0;
        // 成功返回整批的传播计数(除各行目标外被标记的其它节点数),失败返回 false;JS 侧 ok = res!==false
        return ok?JSValueMakeNumber(ctx,gNativeFgui.lastWriteFanout):JSValueMakeBoolean(ctx,false);
    };
    if(argc!=3||!JSValueIsObject(ctx,a[0])||!JSValueIsObject(ctx,a[1])||
       !JSValueIsArray(ctx,a[2]))return finish(false,0);
    if(JSValueGetTypedArrayType(ctx,a[0],exc)!=kJSTypedArrayTypeInt32Array||
       JSValueGetTypedArrayType(ctx,a[1],exc)!=kJSTypedArrayTypeFloat64Array)
        return finish(false,0);
    JSObjectRef wordObject=JSValueToObject(ctx,a[0],exc),numberObject=JSValueToObject(ctx,a[1],exc);
    const size_t wordCount=wordObject?JSObjectGetTypedArrayLength(ctx,wordObject,exc):0,
                 numberCount=numberObject?JSObjectGetTypedArrayLength(ctx,numberObject,exc):0;
    const int32_t* words=wordObject?(const int32_t*)JSObjectGetTypedArrayBytesPtr(ctx,wordObject,exc):nullptr;
    const double* numbers=numberObject?(const double*)JSObjectGetTypedArrayBytesPtr(ctx,numberObject,exc):nullptr;
    if((exc&&*exc)||!words||!numbers||wordCount<6||wordCount>2048*6||wordCount%6)
        return finish(false,0);
    const size_t rowCount=wordCount/6;
    if(numberCount!=rowCount*2)return finish(false,rowCount);
    std::vector<std::string> names;
    if(!fguiJsStringArray(ctx,a[2],rowCount,names,exc))return finish(false,rowCount);
    std::vector<nfgui::MutationWrite> writes;writes.reserve(rowCount);
    for(size_t i=0;i<rowCount;++i){
        const int kind=words[i*6],instance=words[i*6+1],target=words[i*6+2];
        if(kind<(int)nfgui::MutationKind::Controller||kind>(int)nfgui::MutationKind::DisplayRotation||
           instance<=0||target<=0||!std::isfinite(numbers[i*2])||!std::isfinite(numbers[i*2+1]))
            return finish(false,rowCount);
        nfgui::MutationWrite write;write.kind=(nfgui::MutationKind)kind;write.instanceId=instance;
        write.targetId=target;write.value0=words[i*6+3];write.value1=words[i*6+4];
        write.value2=words[i*6+5];write.number0=(float)numbers[i*2];
        write.number1=(float)numbers[i*2+1];write.name=std::move(names[i]);
        writes.push_back(std::move(write));
    }
    return finish(gNativeFgui.commitMutations(writes,gSG),rowCount);
}
static JSValueRef cbNativeFguiSetBitmapText(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<11)return JSValueMakeNull(ctx);std::string text=jsToStd(ctx,a[2]),align=jsToStd(ctx,a[7]);int vertical=(int)JSValueToNumber(ctx,a[8],exc);const char* verticalAlign=vertical==1?"middle":(vertical==2?"bottom":"top");nfgui::BitmapTextMetrics metrics;float localOffsetY=argc>12?(float)JSValueToNumber(ctx,a[12],exc):std::numeric_limits<float>::quiet_NaN();
    bool ok=gNativeFgui.setBitmapText((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),text.c_str(),(float)JSValueToNumber(ctx,a[3],exc),(uint32_t)JSValueToNumber(ctx,a[4],exc),(float)JSValueToNumber(ctx,a[5],exc),(float)JSValueToNumber(ctx,a[6],exc),align.c_str(),verticalAlign,JSValueToBoolean(ctx,a[9]),(int)JSValueToNumber(ctx,a[10],exc),gSG,&metrics,argc>11&&JSValueToBoolean(ctx,a[11]),localOffsetY);
    if(!ok)return JSValueMakeNull(ctx);JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,true));setJsNumber(ctx,out,"layoutVersion",1);setJsNumber(ctx,out,"textWidth",metrics.textWidth);setJsNumber(ctx,out,"textHeight",metrics.textHeight);setJsNumber(ctx,out,"width",metrics.width);setJsNumber(ctx,out,"height",metrics.height);setJsNumber(ctx,out,"glyphCount",metrics.glyphCount);setJsProperty(ctx,out,"trusted",JSValueMakeBoolean(ctx,metrics.trusted));setJsProperty(ctx,out,"wrote",JSValueMakeBoolean(ctx,metrics.wrote));return out;
}
static JSValueRef cbNativeFguiTransitionPlay(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<7)return JSValueMakeNumber(ctx,0);std::string name=jsToStd(ctx,a[2]);int token=gNativeFgui.playTransition((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.c_str(),JSValueToNumber(ctx,a[3],exc),(int)JSValueToNumber(ctx,a[4],exc),JSValueToNumber(ctx,a[5],exc),JSValueToBoolean(ctx,a[6]),gSG);return JSValueMakeNumber(ctx,token);
}
static JSValueRef cbNativeFguiTransitionStop(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<5)return JSValueMakeBoolean(ctx,false);std::string name=jsToStd(ctx,a[2]);bool ok=gNativeFgui.stopTransition((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.c_str(),JSValueToBoolean(ctx,a[3]),JSValueToBoolean(ctx,a[4]),gSG);return JSValueMakeBoolean(ctx,ok);
}
static JSValueRef cbNativeFguiTransitionSetValue(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);std::string name=jsToStd(ctx,a[2]),label=jsToStd(ctx,a[3]);std::vector<std::string> values;for(size_t i=4;i<argc;i++){std::string value=jsToStd(ctx,a[i]);values.push_back(value.c_str());}return JSValueMakeBoolean(ctx,gNativeFgui.setTransitionValue((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.c_str(),label.c_str(),values));
}
static JSValueRef cbNativeFguiTransitionSetTarget(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc<5)return JSValueMakeBoolean(ctx,false);std::string name=jsToStd(ctx,a[2]),label=jsToStd(ctx,a[3]),target=jsToStd(ctx,a[4]);return JSValueMakeBoolean(ctx,gNativeFgui.setTransitionTarget((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.c_str(),label.c_str(),target.c_str(),gSG));}
static JSValueRef cbNativeFguiTransitionSetDuration(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc<5)return JSValueMakeBoolean(ctx,false);std::string name=jsToStd(ctx,a[2]),label=jsToStd(ctx,a[3]);return JSValueMakeBoolean(ctx,gNativeFgui.setTransitionDuration((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.c_str(),label.c_str(),(float)JSValueToNumber(ctx,a[4],exc)));}
static JSValueRef cbNativeFguiTransitionUpdateRelations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc<6)return JSValueMakeBoolean(ctx,false);std::string name=jsToStd(ctx,a[2]),target=jsToStd(ctx,a[3]);return JSValueMakeBoolean(ctx,gNativeFgui.updateTransitionFromRelations((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.c_str(),target.c_str(),(float)JSValueToNumber(ctx,a[4],exc),(float)JSValueToNumber(ctx,a[5],exc)));}
static JSValueRef cbNativeFguiTransitionChangeRepeat(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc<4)return JSValueMakeBoolean(ctx,false);std::string name=jsToStd(ctx,a[2]);return JSValueMakeBoolean(ctx,gNativeFgui.changeTransitionRepeat((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.c_str(),(int)JSValueToNumber(ctx,a[3],exc)));}
static JSValueRef cbNativeFguiTransitionPlaying(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc<3)return JSValueMakeBoolean(ctx,false);std::string name=jsToStd(ctx,a[2]);return JSValueMakeBoolean(ctx,gNativeFgui.isTransitionPlaying((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),name.c_str()));}

// ======================= spine ZIP 预处理(preparse)+ spinex 宿主接线 =======================
// 与 ios_app.mm L2517-2831 同源;NSDictionary/NSString → std 容器,GCD → gLoadPool/gMainQ。
struct NativeSpinePrepared {
    int handle=0;
    std::string cacheKey,url,key;
    std::string skeletonFilename,skelFilename,atlasFilename,atlasText,version;
    std::vector<std::string> imageNames;
    std::unordered_map<std::string,macres::Bytes> entries;
    size_t compressedBytes=0; uint32_t inputCrc=0; size_t manifestEntries=0;
    double zipMs=0,atlasMs=0,jsonMs=0;
    int activeUsers=0;   // 在途 lease:JS 加载链 pin/unpin;>0 禁止 LRU 逐出
};
static std::unordered_map<std::string,std::shared_ptr<NativeSpinePrepared>> gSpinePreparedCache;   // LRU 128
static std::unordered_map<int,std::shared_ptr<NativeSpinePrepared>> gSpinePreparedByHandle;
static std::list<std::string> gSpinePreparedLru;
static std::unordered_map<std::string,std::vector<NativeSpinePrepareWaiter>> gSpinePrepareInflight;
static int gNextSpinePreparedHandle=1;
static uint64_t gSpinePrepareEpoch=1;
static void spinexPrewarm(const std::shared_ptr<NativeSpinePrepared>& parsed);

static std::string nativeSpineCacheKey(const std::string& url,const std::string& key){ return url+"\x1f"+key; }
static uint32_t nativeSpineInputCrc(const macres::Bytes& data){
    if(!data||data->empty())return 0;uLong crc=crc32(0L,Z_NULL,0);size_t offset=0;
    while(offset<data->size()){uInt chunk=(uInt)std::min<size_t>(data->size()-offset,UINT_MAX);crc=crc32(crc,data->data()+offset,chunk);offset+=chunk;}return (uint32_t)crc;
}
// .skel 头部版本号:int32 lowHash + int32 highHash + varint 长度 L + (L-1) 字节 version。
static std::string nativeSpineSkelVersion(const macres::Bytes& data){
    if(!data||data->size()<10)return {};
    const uint8_t* p=data->data();size_t i=8;
    int len=0,shift=0;
    while(i<data->size()&&shift<=28){uint8_t b=p[i++];len|=(b&0x7F)<<shift;if(!(b&0x80))break;shift+=7;}
    if(len<=1||i+(size_t)(len-1)>data->size())return {};
    return std::string((const char*)p+i,(size_t)(len-1));
}
static std::string lowerExt(const std::string& name){
    size_t dot=name.find_last_of('.');if(dot==std::string::npos)return {};
    std::string ext=name.substr(dot+1);for(char& c:ext)c=(char)tolower((unsigned char)c);return ext;
}
static std::string stemOf(const std::string& name){
    size_t dot=name.find_last_of('.');return dot==std::string::npos?name:name.substr(0,dot);
}
static bool iequals(const std::string& a,const std::string& b){
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();i++)if(tolower((unsigned char)a[i])!=tolower((unsigned char)b[i]))return false;
    return true;
}
// json 里 "skeleton":{..."spine":"x.y.z"...} 的轻量提取(后台线程不能碰 JS 上下文,不用 JSON 引擎)。
static bool spineJsonProbe(const macres::Bytes& data,std::string& version){
    if(!data||data->empty())return false;
    std::string s((const char*)data->data(),std::min<size_t>(data->size(),16384));
    size_t sk=s.find("\"skeleton\"");if(sk==std::string::npos)return false;
    size_t sp=s.find("\"spine\"",sk);if(sp==std::string::npos)return true;   // 有 skeleton 无版本:算命中
    size_t colon=s.find(':',sp+7);if(colon==std::string::npos)return true;
    size_t q1=s.find('"',colon+1);if(q1==std::string::npos)return true;
    size_t q2=s.find('"',q1+1);if(q2==std::string::npos)return true;
    version=s.substr(q1+1,q2-q1-1);return true;
}
static std::shared_ptr<NativeSpinePrepared> prepareNativeSpineArchive(const macres::Bytes& input,const std::string& url,const std::string& key,uint32_t inputCrc){
    if(!input||input->empty())return nullptr;double start=nowUs();
    std::unordered_map<std::string,macres::Bytes> entries;
    if(!zipParseAll(input->data(),input->size(),entries)||entries.empty()){ALOGE("[spine-preparse/android] %s zip parse failed",key.c_str());return nullptr;}
    double zipMs=(nowUs()-start)/1000.0;
    std::vector<std::string> names;names.reserve(entries.size());for(auto& kv:entries)names.push_back(kv.first);
    std::sort(names.begin(),names.end());
    std::vector<std::string> jsonNames,atlasNames,imageNames,skelNames;
    for(const std::string& name:names){if(name.find('/')!=std::string::npos)continue;std::string ext=lowerExt(name);
        if(ext=="json")jsonNames.push_back(name);else if(ext=="atlas")atlasNames.push_back(name);else if(ext=="png")imageNames.push_back(name);else if(ext=="skel")skelNames.push_back(name);}
    // 骨架来源二选一即可:.skel(优先)或 .json;还需 atlas + 至少 1 张 png。
    if((jsonNames.empty()&&skelNames.empty())||atlasNames.empty()||imageNames.empty()){ALOGE("[spine-preparse/android] %s missing skel/atlas/png",key.c_str());return nullptr;}
    std::string skeletonName,atlasName=atlasNames.front();
    for(const std::string& name:atlasNames)if(iequals(stemOf(name),key)){atlasName=name;break;}
    double atlasStart=nowUs();
    const macres::Bytes& atlasData=entries[atlasName];
    std::string atlasText=atlasData?std::string((const char*)atlasData->data(),atlasData->size()):std::string();
    double atlasMs=(nowUs()-atlasStart)/1000.0;
    if(atlasText.empty())return nullptr;
    std::vector<std::string> orderedJson=jsonNames;
    for(size_t i=0;i<orderedJson.size();i++)if(iequals(stemOf(orderedJson[i]),key)){std::string hit=orderedJson[i];orderedJson.erase(orderedJson.begin()+(long)i);orderedJson.insert(orderedJson.begin(),hit);break;}
    std::string skelName=skelNames.empty()?std::string():skelNames.front();
    for(const std::string& name:skelNames)if(iequals(stemOf(name),key)){skelName=name;break;}
    // 有 .skel 就完全不碰 .json(spinex 直接吃 .skel 字节);纯 json 老包才探测版本。
    double jsonStart=nowUs();bool haveSkeletonJson=false;std::string version;
    if(skelName.empty()){
        for(const std::string& name:orderedJson){std::string v;if(spineJsonProbe(entries[name],v)){haveSkeletonJson=true;skeletonName=name;version=v;break;}}
    }
    double jsonMs=(nowUs()-jsonStart)/1000.0;
    if(!haveSkeletonJson&&skelName.empty())return nullptr;
    if(!skelName.empty())version=nativeSpineSkelVersion(entries[skelName]);
    auto parsed=std::make_shared<NativeSpinePrepared>();
    parsed->url=url;parsed->key=key;parsed->cacheKey=nativeSpineCacheKey(url,key);
    parsed->skelFilename=skelName;parsed->skeletonFilename=skeletonName;parsed->atlasFilename=atlasName;
    parsed->atlasText=std::move(atlasText);parsed->version=version;parsed->imageNames=imageNames;
    parsed->entries=std::move(entries);parsed->compressedBytes=input->size();parsed->inputCrc=inputCrc;
    parsed->manifestEntries=jsonNames.size()+atlasNames.size()+imageNames.size();
    parsed->zipMs=zipMs;parsed->atlasMs=atlasMs;parsed->jsonMs=jsonMs;return parsed;
}
static void touchNativeSpinePrepared(const std::shared_ptr<NativeSpinePrepared>& parsed){
    if(parsed->cacheKey.empty())return;gSpinePreparedLru.remove(parsed->cacheKey);gSpinePreparedLru.push_back(parsed->cacheKey);
}
// lease 模型:prepared 的在途保活是显式引用(activeUsers,JS 加载链 pin/unpin),不再依赖
// "cap 足够大轮不到挤"的容量巧合;留存降为小热 LRU(默认 8,重建加速)。逐出从最老起跳过
// 在途者,全在途时容忍临时超容(那是必要工作集,挤掉只会断链重来更贵);unpin 归零时也补跑
// 一次,停止加载后过期 holder 不必等下一次插入才释放。上限 SPINE_PREPARED_LRU_CAP 可调
// (压测/调参;cap=1 复现"逐出→needs-reload 回源"链路)。
static void spinePreparedEvictOverflow(){
    static int lruCap=[](){const char* e=getenv("SPINE_PREPARED_LRU_CAP");int v=e?atoi(e):0;return v>0?v:8;}();
    for(auto it=gSpinePreparedLru.begin();it!=gSpinePreparedLru.end()&&gSpinePreparedLru.size()>(size_t)lruCap;){
        auto victim=gSpinePreparedCache.find(*it);
        if(victim!=gSpinePreparedCache.end()&&victim->second->activeUsers>0){++it;continue;}
        if(victim!=gSpinePreparedCache.end()){gSpinePreparedByHandle.erase(victim->second->handle);gSpinePreparedCache.erase(victim);}
        it=gSpinePreparedLru.erase(it);
        gSpinePrepareStats.evictions++;
    }
}
static void cacheNativeSpinePrepared(const std::shared_ptr<NativeSpinePrepared>& parsed){
    if(parsed->cacheKey.empty())return;
    // 同 cacheKey 换代:在途(activeUsers>0)的旧 holder 保留 byHandle 条目退休服役——旧加载链
    // 自己已非 cache 现任即自行从 byHandle 收尾。无在途者照旧立即摘除。
    auto prior=gSpinePreparedCache.find(parsed->cacheKey);
    if(prior!=gSpinePreparedCache.end()&&prior->second->activeUsers<=0)gSpinePreparedByHandle.erase(prior->second->handle);
    parsed->handle=gNextSpinePreparedHandle++;if(gNextSpinePreparedHandle<=0)gNextSpinePreparedHandle=1;
    gSpinePreparedCache[parsed->cacheKey]=parsed;gSpinePreparedByHandle[parsed->handle]=parsed;touchNativeSpinePrepared(parsed);
    spinePreparedEvictOverflow();
}
// 在途 lease 的 JS 面:pin 成功返回 true(handle 已被逐出返回 false,JS 走 fallback→回源重试);
// unpin 幂等由 JS 侧保证(unpinOnce),归零即补跑超额回收。
static JSValueRef cbNativeSpinePin(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1)return JSValueMakeBoolean(ctx,false);
    auto it=gSpinePreparedByHandle.find((int)JSValueToNumber(ctx,args[0],exc));
    if(it==gSpinePreparedByHandle.end())return JSValueMakeBoolean(ctx,false);
    it->second->activeUsers++;touchNativeSpinePrepared(it->second);
    return JSValueMakeBoolean(ctx,true);
}
// 释放内核:notify 拒收退还与 JS unpin 共用(归零收尾:退休 holder 摘 byHandle,现任走超额回收)。
static void spineReleasePreparedRef(const std::shared_ptr<NativeSpinePrepared>& p){
    if(!p||p->activeUsers<=0)return;
    p->activeUsers--;
    if(p->activeUsers==0){
        auto cur=gSpinePreparedCache.find(p->cacheKey);
        if(cur==gSpinePreparedCache.end()||cur->second!=p)gSpinePreparedByHandle.erase(p->handle);   // 退休 holder(同 key 换代被顶替):收尾释放
        else spinePreparedEvictOverflow();
    }
}
static JSValueRef cbNativeSpineUnpin(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1)return JSValueMakeUndefined(ctx);
    auto it=gSpinePreparedByHandle.find((int)JSValueToNumber(ctx,args[0],exc));
    if(it!=gSpinePreparedByHandle.end())spineReleasePreparedRef(it->second);
    return JSValueMakeUndefined(ctx);
}
static void notifyNativeSpinePreparse(const NativeSpinePrepareWaiter& waiter,bool ok,const std::shared_ptr<NativeSpinePrepared>& parsed){
    // wantsLease 的 waiter 携带一份调用方预获取的引用:交付 JS(preparseDone 按 args[7] 记账
    // 移交)或拒收即退还。非 lease waiter(legacy 链只用原始字节)无引用,args[7]=false。
    bool leaseHeld=ok&&parsed&&waiter.wantsLease;
    if(!gCtx||gCtx!=waiter.context){if(leaseHeld)spineReleasePreparedRef(parsed);return;}
    JSValueRef args[8]={JSValueMakeNumber(gCtx,waiter.token),JSValueMakeBoolean(gCtx,ok),JSValueMakeNumber(gCtx,parsed?parsed->handle:0),JSValueMakeNumber(gCtx,parsed?parsed->zipMs:0),JSValueMakeNumber(gCtx,parsed?parsed->jsonMs:0),JSValueMakeNumber(gCtx,parsed?(double)parsed->manifestEntries:0),JSValueMakeNumber(gCtx,parsed?(double)parsed->imageNames.size():0),JSValueMakeBoolean(gCtx,leaseHeld)};callJsGlobal("__nativeSpinePreparseDone",8,args);
}
static JSValueRef cbNativeSpinePreparse(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);int token=(int)JSValueToNumber(ctx,args[0],exc);std::string url=jsToStd(ctx,args[1]),key=jsToStd(ctx,args[2]);macres::Bytes data=jsToBytes(ctx,args[3],exc);
    if(url.empty()||key.empty()||!data||data->empty())return JSValueMakeBoolean(ctx,false);gSpinePrepareStats.requests++;uint32_t inputCrc=nativeSpineInputCrc(data);std::string cacheKey=nativeSpineCacheKey(url,key);
    bool wantsLease=argc>4&&JSValueToBoolean(ctx,args[4]);   // 只有声明读 holder 的消费链(prepared wrapper)才发 lease;legacy 链只用原始字节
    NativeSpinePrepareWaiter waiter{token,gCtx,wantsLease};
    auto cachedIt=gSpinePreparedCache.find(cacheKey);
    if(cachedIt!=gSpinePreparedCache.end()&&cachedIt->second->compressedBytes==data->size()&&cachedIt->second->inputCrc==inputCrc){
        gSpinePrepareStats.cacheHits++;auto cached=cachedIt->second;touchNativeSpinePrepared(cached);
        if(wantsLease)cached->activeUsers++;   // 在途 lease:入队前同步获取(排队期间的插入逐出不能挤掉它),notify 交付或退还
        gMainQ.post([waiter,cached]{notifyNativeSpinePreparse(waiter,true,cached);});
        return JSValueMakeBoolean(ctx,true);
    }
    uint64_t requestEpoch=gSpinePrepareEpoch;std::string workKey=std::to_string(requestEpoch)+"#"+cacheKey+"#"+std::to_string(data->size())+"#"+std::to_string(inputCrc);
    auto active=gSpinePrepareInflight.find(workKey);
    if(active!=gSpinePrepareInflight.end()){active->second.push_back(waiter);gSpinePrepareStats.deduped++;return JSValueMakeBoolean(ctx,true);}
    gSpinePrepareInflight[workKey].push_back(waiter);gSpinePrepareStats.jobs++;JSGlobalContextRef requestContext=gCtx;ALOGI("[spine-preparse/android] JOB %s bytes=%zu",key.c_str(),data->size());
    gLoadPool.post([data,url,key,inputCrc,workKey,requestEpoch,requestContext]{
        std::shared_ptr<NativeSpinePrepared> parsed;
        {
            aplat::CpuGate::Scoped gate(gCpuGate);
            parsed=prepareNativeSpineArchive(data,url,key,inputCrc);
        }
        gMainQ.post([parsed,key,workKey,requestEpoch,requestContext]{
            if(requestEpoch!=gSpinePrepareEpoch)return;
            auto found=gSpinePrepareInflight.find(workKey);if(found==gSpinePrepareInflight.end())return;
            std::vector<NativeSpinePrepareWaiter> waiters=std::move(found->second);gSpinePrepareInflight.erase(found);
            if(gCtx!=requestContext)return;
            bool ok=parsed!=nullptr;
            if(ok){int leaseN=0;for(const auto& w:waiters)if(w.wantsLease)leaseN++;parsed->activeUsers=leaseN;/* 在途 lease:插入前按 lease-waiter 数出生——插入触发的逐出不能挤掉自己;legacy waiter 不计 */cacheNativeSpinePrepared(parsed);gSpinePrepareStats.ready++;gSpinePrepareStats.backgroundZipMs+=parsed->zipMs;gSpinePrepareStats.backgroundAtlasMs+=parsed->atlasMs;gSpinePrepareStats.backgroundJsonMs+=parsed->jsonMs;spinexPrewarm(parsed);}else gSpinePrepareStats.failures++;
            ALOGI("[spine-preparse/android] READY %s ok=%d handle=%d entries=%zu images=%zu zip=%.2fms json=%.2fms waiters=%zu",key.c_str(),ok?1:0,parsed?parsed->handle:0,parsed?parsed->manifestEntries:0,parsed?parsed->imageNames.size():0,parsed?parsed->zipMs:0,parsed?parsed->jsonMs:0,waiters.size());
            for(const auto& pending:waiters)notifyNativeSpinePreparse(pending,ok,parsed);
        });
    });
    return JSValueMakeBoolean(ctx,true);
}
static JSValueRef cbNativeSpineManifest(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1)return JSValueMakeNull(ctx);auto pit=gSpinePreparedByHandle.find((int)JSValueToNumber(ctx,args[0],exc));if(pit==gSpinePreparedByHandle.end())return JSValueMakeNull(ctx);auto parsed=pit->second;gSpinePrepareStats.manifestGets++;touchNativeSpinePrepared(parsed);
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsProperty(ctx,out,"handle",JSValueMakeNumber(ctx,parsed->handle));setJsProperty(ctx,out,"key",fguiJsString(ctx,parsed->key));setJsProperty(ctx,out,"url",fguiJsString(ctx,parsed->url));setJsProperty(ctx,out,"skeleton",fguiJsString(ctx,parsed->skeletonFilename));setJsProperty(ctx,out,"atlas",fguiJsString(ctx,parsed->atlasFilename));setJsProperty(ctx,out,"version",fguiJsString(ctx,parsed->version));
    setJsProperty(ctx,out,"skel",parsed->skelFilename.empty()?JSValueMakeNull(ctx):fguiJsString(ctx,parsed->skelFilename));
    std::vector<JSValueRef> images;images.reserve(parsed->imageNames.size());for(const std::string& name:parsed->imageNames)images.push_back(fguiJsString(ctx,name));setJsProperty(ctx,out,"images",JSObjectMakeArray(ctx,images.size(),images.empty()?nullptr:images.data(),exc));setJsProperty(ctx,out,"entryCount",JSValueMakeNumber(ctx,(double)parsed->manifestEntries));return out;
}
static JSValueRef cbNativeSpineGet(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<3)return JSValueMakeNull(ctx);auto pit=gSpinePreparedByHandle.find((int)JSValueToNumber(ctx,args[0],exc));if(pit==gSpinePreparedByHandle.end())return JSValueMakeNull(ctx);auto parsed=pit->second;std::string name=jsToStd(ctx,args[1]);int mode=(int)JSValueToNumber(ctx,args[2],exc);touchNativeSpinePrepared(parsed);
    if(mode==1){if(name!=parsed->atlasFilename)return JSValueMakeNull(ctx);gSpinePrepareStats.atlasGets++;return fguiJsString(ctx,parsed->atlasText);}
    auto eit=parsed->entries.find(name);if(eit==parsed->entries.end()||!eit->second)return JSValueMakeNull(ctx);const macres::Bytes& data=eit->second;gSpinePrepareStats.imageGets++;void* bytes=malloc(data->size());if(!bytes)return JSValueMakeNull(ctx);memcpy(bytes,data->data(),data->size());
    JSObjectRef buffer=JSObjectMakeArrayBufferWithBytesNoCopy(ctx,bytes,data->size(),[](void* p,void*){free(p);},nullptr,exc);return buffer?(JSValueRef)buffer:JSValueMakeNull(ctx);
}
static JSValueRef cbLoadSpinePreparedImage(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<3)return JSValueMakeBoolean(ctx,false);int handle=(int)JSValueToNumber(ctx,args[0],exc),token=(int)JSValueToNumber(ctx,args[2],exc);std::string name=jsToStd(ctx,args[1]);
    auto pit=gSpinePreparedByHandle.find(handle);
    std::shared_ptr<NativeSpinePrepared> parsed=pit==gSpinePreparedByHandle.end()?nullptr:pit->second;
    macres::Bytes data;if(parsed){auto eit=parsed->entries.find(name);if(eit!=parsed->entries.end())data=eit->second;}
    if(!parsed||!data||data->empty()){gSpinePrepareStats.directImageMisses++;return JSValueMakeBoolean(ctx,false);}gSpinePrepareStats.directImageRequests++;touchNativeSpinePrepared(parsed);JSGlobalContextRef requestContext=gCtx;
    char crcBuf[16];snprintf(crcBuf,sizeof crcBuf,"%08x",parsed->inputCrc);
    std::string stableName=parsed->cacheKey+"#"+crcBuf+"#"+name;
    gNativeResources.requestImageDataNamed(stableName,data,[requestContext,token](uint32_t cid,int width,int height){deliverPublishedImage(requestContext,token,cid,width,height);},/*publishRef=*/true);
    return JSValueMakeBoolean(ctx,true);
}
static JSValueRef cbNativeSpineRecordFallback(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef*){gSpinePrepareStats.fallbacks++;return JSValueMakeUndefined(ctx);}
static JSValueRef cbNativeSpinePreparseStats(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef*){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);setJsNumber(ctx,out,"requests",gSpinePrepareStats.requests);setJsNumber(ctx,out,"jobs",gSpinePrepareStats.jobs);setJsNumber(ctx,out,"deduped",gSpinePrepareStats.deduped);setJsNumber(ctx,out,"cacheHits",gSpinePrepareStats.cacheHits);setJsNumber(ctx,out,"ready",gSpinePrepareStats.ready);setJsNumber(ctx,out,"failures",gSpinePrepareStats.failures);setJsNumber(ctx,out,"fallbacks",gSpinePrepareStats.fallbacks);setJsNumber(ctx,out,"cacheEntries",gSpinePreparedCache.size());setJsNumber(ctx,out,"inflight",gSpinePrepareInflight.size());setJsNumber(ctx,out,"evictions",gSpinePrepareStats.evictions);
    {size_t pinnedN=0;for(auto&kv:gSpinePreparedByHandle)if(kv.second->activeUsers>0)pinnedN++;setJsNumber(ctx,out,"pinned",(double)pinnedN);}   // 在途 lease 观测:泄漏=不回落
    setJsNumber(ctx,out,"manifestGets",gSpinePrepareStats.manifestGets);setJsNumber(ctx,out,"imageGets",gSpinePrepareStats.imageGets);setJsNumber(ctx,out,"directImageRequests",gSpinePrepareStats.directImageRequests);setJsNumber(ctx,out,"directImageMisses",gSpinePrepareStats.directImageMisses);setJsNumber(ctx,out,"atlasGets",gSpinePrepareStats.atlasGets);setJsNumber(ctx,out,"jsonGets",gSpinePrepareStats.jsonGets);setJsNumber(ctx,out,"preparedLoads",gSpinePrepareStats.preparedLoads);setJsNumber(ctx,out,"loadFailures",gSpinePrepareStats.loadFailures);setJsNumber(ctx,out,"skeletonCalls",gSpinePrepareStats.skeletonCalls);setJsNumber(ctx,out,"compatSkeletonCalls",gSpinePrepareStats.compatSkeletonCalls);
    setJsNumber(ctx,out,"backgroundZipMs",gSpinePrepareStats.backgroundZipMs);setJsNumber(ctx,out,"backgroundAtlasMs",gSpinePrepareStats.backgroundAtlasMs);setJsNumber(ctx,out,"backgroundJsonMs",gSpinePrepareStats.backgroundJsonMs);setJsNumber(ctx,out,"jsonBridgeMs",gSpinePrepareStats.jsonBridgeMs);setJsNumber(ctx,out,"atlasMainMs",gSpinePrepareStats.atlasMainMs);setJsNumber(ctx,out,"skeletonMainMs",gSpinePrepareStats.skeletonMainMs);setJsNumber(ctx,out,"compatSkeletonMainMs",gSpinePrepareStats.compatSkeletonMainMs);setJsNumber(ctx,out,"maxSkeletonMainMs",gSpinePrepareStats.maxSkeletonMainMs);return out;
}
// ---- spinex 宿主接线(与 ios L2676-2831 同源) ----
static bool gSpinexHostInit=false;
static void spinexEnsureHost(){
    if(gSpinexHostInit)return;
    gSpinexHostInit=true;
    gSpinexRt.store.setTexRelease([](uint32_t cid){ gNativeResources.release(cid); });
    gSpinexRt.setDynamicTextureLifetime(
        [](uint32_t cid){ return gDec.retainTexture(cid) || gNativeResources.retain(cid); },
        [](uint32_t cid){ if(!gDec.releaseTexture(cid))gNativeResources.release(cid); });
}
// 页名 → prepared 的 png → stableName(cacheKey#crc#页名) → ResourceManager 异步 cid。
static spinex::TexRequestFn spinexMakeTexRequest(const std::shared_ptr<NativeSpinePrepared>& holder){
    return [holder](const std::string& pageName,const spinex::TexStatePtr& st){
        auto eit=holder->entries.find(pageName);
        macres::Bytes png=eit==holder->entries.end()?nullptr:eit->second;
        if(!png||png->empty()){st->failed=true;ALOGE("[spinex] atlas 页 %s 无 zip 条目",pageName.c_str());return;}
        char crcBuf[16];snprintf(crcBuf,sizeof crcBuf,"%08x",holder->inputCrc);
        std::string stableName=holder->cacheKey+"#"+crcBuf+"#"+pageName;
        st->request=[png,stableName](const std::string&,const spinex::TexStatePtr& state){
            spinex::TexStatePtr sp=state;const uint64_t generation=state->generation;
            gNativeResources.requestImageDataNamed(stableName,png,[sp,generation](uint32_t cid,int width,int height){
                if(sp->abandoned||sp->generation!=generation)return;   // entry 已销毁/活跃期已换代
                if(!cid){sp->failed=true;return;}
                sp->cid=cid;sp->width=width;sp->height=height;
                if(gNativeResources.retain(cid))sp->retained=true;
            });
        };
    };
}
static std::shared_ptr<NativeSpinePrepared> spinexFindPrepared(const std::string& key){
    if(key.empty()||gSpinePreparedCache.empty())return nullptr;
    for(auto it=gSpinePreparedLru.rbegin();it!=gSpinePreparedLru.rend();++it){
        auto cit=gSpinePreparedCache.find(*it);
        if(cit!=gSpinePreparedCache.end()&&(cit->second->key==key||cit->second->cacheKey==key))return cit->second;
    }
    return nullptr;
}
// ---- 骨架预热:preparse 一就绪就在后台起解析(Atlas 仍在 JS 线程建,纯 CPU 的 readSkeletonData 去后台) ----
static std::unordered_map<std::string,std::shared_ptr<spinex::DataStore::AsyncBuild>> gSpinexPrewarm;
static void spinexPrewarm(const std::shared_ptr<NativeSpinePrepared>& parsed){
    static const bool disabled = getenv("SPINE_PREWARM_OFF") != nullptr;
    if(disabled)return;
    if(!parsed||parsed->key.empty())return;
    spinexEnsureHost();
    const std::string& k=parsed->key;
    auto pending=gSpinexPrewarm.find(k);
    if(pending!=gSpinexPrewarm.end()){
        // 同名 zspine 换代:同内容在途 job 沿用;不同内容 discardAsync 退还页图引用再起新预热。
        if(pending->second&&pending->second->inputCrc==parsed->inputCrc)return;
        gSpinexRt.store.discardAsync(pending->second);
        gSpinexPrewarm.erase(pending);
    }
    if(auto* found=gSpinexRt.store.find(k)) if(found->inputCrc==parsed->inputCrc) return;
    macres::Bytes jsonData;if(!parsed->skeletonFilename.empty()){auto jit=parsed->entries.find(parsed->skeletonFilename);if(jit!=parsed->entries.end())jsonData=jit->second;}
    macres::Bytes skelData;if(!parsed->skelFilename.empty()){auto sit=parsed->entries.find(parsed->skelFilename);if(sit!=parsed->entries.end())skelData=sit->second;}
    if((!skelData||skelData->empty())&&(!jsonData||jsonData->empty()))return;
    const std::string& atlasText=parsed->atlasText;
    // 无 .json 时留空串:parseSkeleton 的 `if(!data && jsonText && jsonText[0])` 会跳过 JSON 分支
    std::string jsonText=jsonData&&!jsonData->empty()?std::string((const char*)jsonData->data(),jsonData->size()):std::string();
    auto job=gSpinexRt.store.prepareAsync(k,atlasText.c_str(),(int)atlasText.size(),jsonText.c_str(),
                                          parsed->inputCrc,spinexMakeTexRequest(parsed),
                                          skelData&&!skelData->empty()?skelData->data():nullptr,skelData?(int)skelData->size():0);
    if(!job)return;
    gSpinexPrewarm[k]=job;
    gLoadPool.post([job]{
        aplat::CpuGate::Scoped gate(gCpuGate);
        spinex::DataStore::runAsync(job);
    });
}
static spinex::DataStore::Entry* spinexEnsureBuilt(const std::string& key,int preparedHandle,std::string* outError){
    spinexEnsureHost();
    const std::string& k=key;
    // 身份优先:preparedHandle 是"这次加载的精确身份",缺席才按短 key 从 LRU 最新候选里挑。
    std::shared_ptr<NativeSpinePrepared> parsed;
    if(preparedHandle>0){
        auto pit=gSpinePreparedByHandle.find(preparedHandle);
        if(pit!=gSpinePreparedByHandle.end()){parsed=pit->second;touchNativeSpinePrepared(parsed);}
        else {
            // 带明确身份却找不到 prepared(LRU 已逐出):拒绝退回短名猜测。
            ALOGE("[spinex] key=%s preparedHandle=%d 已被 LRU 逐出,拒绝按短名猜测",k.c_str(),preparedHandle);
            if(outError)*outError="prepared handle evicted (refusing short-key guess)";
            return nullptr;
        }
    }
    if(!parsed)parsed=spinexFindPrepared(key);
    if(!parsed){
        if(auto* found=gSpinexRt.store.find(k)){
            if(!found->jsReleased)return found;
            ALOGE("[spinex] key=%s preparse miss,复用已释放但仍存活的条目(sd=%d refs=%d live=%d)",
                    k.c_str(),found->handle,found->refs,found->liveInstances);
            return found;
        }
        if(outError)*outError="prepared entry not found (preparse cache miss)";
        return nullptr;
    }
    if(auto* found=gSpinexRt.store.find(k))
        if(found->inputCrc==parsed->inputCrc)return found;
    // 预热命中:后台已算完直接登记;还没算完就在此等它(最坏退化成原同步耗时)。
    auto pre=gSpinexPrewarm.find(k);
    if(pre!=gSpinexPrewarm.end()){
        auto job=pre->second;
        if(job&&job->inputCrc==parsed->inputCrc){
            double waitT0=nowUs();
            while(!job->finished.load(std::memory_order_acquire)) usleep(200);   // acquire:与后台 release 配对
            double waitMs=(nowUs()-waitT0)/1000.0;
            gSpinexPrewarm.erase(pre);
            spinex::DataStore::Entry* built=gSpinexRt.store.commitAsync(job,outError);
            ALOGI("[spinex] key=%s 预热%s(主线程等待 %.1fms)",k.c_str(),built?"命中":"失败",waitMs);
            if(built)return built;
        } else {
            gSpinexRt.store.discardAsync(pre->second);
            gSpinexPrewarm.erase(pre);
        }
    }
    macres::Bytes jsonData;if(!parsed->skeletonFilename.empty()){auto jit=parsed->entries.find(parsed->skeletonFilename);if(jit!=parsed->entries.end())jsonData=jit->second;}
    macres::Bytes skelData;if(!parsed->skelFilename.empty()){auto sit=parsed->entries.find(parsed->skelFilename);if(sit!=parsed->entries.end())skelData=sit->second;}
    if((!skelData||skelData->empty())&&(!jsonData||jsonData->empty())){if(outError)*outError="skeleton entry missing (.skel/.json)";return nullptr;}
    const std::string& atlasText=parsed->atlasText;
    std::string jsonText=jsonData&&!jsonData->empty()?std::string((const char*)jsonData->data(),jsonData->size()):std::string();
    spinex::TexRequestFn texRequest=spinexMakeTexRequest(parsed);
    spinex::DataStore::Entry* built=gSpinexRt.store.buildFromPrepared(k,atlasText.c_str(),(int)atlasText.size(),
                                             jsonText.c_str(),parsed->inputCrc,texRequest,outError,
                                             skelData&&!skelData->empty()?skelData->data():nullptr,skelData?(int)skelData->size():0);
    if(!built&&!parsed->version.empty()&&parsed->version.rfind("3.",0)==0)
        ALOGE("[spinex] key=%s spine %s 骨架不受支持,已回退 legacy(资源侧升版可收编)",
                k.c_str(),parsed->version.c_str());
    return built;
}

// ios_app.mm L2832-3260 spinex 绑定的 Android(QuickJS shim)转换,与 iOS 逐字同源
static JSValueRef cbSpinexBuild(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    std::string key=argc>0?jsToStd(ctx,args[0]):std::string();
    int preparedHandle=argc>1?(int)JSValueToNumber(ctx,args[1],exc):0;   // 本次加载的精确身份(可选)
    std::string error;double t0=nowUs();
    spinex::DataStore::Entry* entry=!key.empty()?spinexEnsureBuilt(key,preparedHandle,&error):nullptr;
    if(!entry){
        setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,false));
        setJsProperty(ctx,out,"error",fguiJsString(ctx,error.empty()?"bad key":error.c_str()));
        // 可回源失败:两层皆空(miss)或精确身份已被 LRU 逐出(evicted)=数据真没了,但按
        // __spineKeyUrl 登记的原始 url 重载即可自愈——重载完成信号带**新** preparedHandle 走
        // crc 裁决,不经短名猜测,与 evicted 拒猜防的错配无冲突(区别于解析失败等不可回源
        // 失败)。JS 据此触发 __spxRequestReload 并给 pending 门面,而不是锁死走 legacy。
        if(error=="prepared entry not found (preparse cache miss)"
           ||error=="prepared handle evicted (refusing short-key guess)")
            setJsProperty(ctx,out,"needsReload",JSValueMakeBoolean(ctx,true));
        return out;
    }
    gSpinexRt.store.retain(entry);   // build=retain；JS 侧 __spxRelease(sdHandle) 对称释放
    spine::SkeletonData* data=entry->data;
    setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,true));
    setJsNumber(ctx,out,"sdHandle",(double)entry->handle);
    setJsNumber(ctx,out,"refs",(double)entry->refs);
    setJsNumber(ctx,out,"inputCrc",(double)entry->inputCrc);
    setJsNumber(ctx,out,"bones",(double)data->getBones().size());
    setJsNumber(ctx,out,"slots",(double)data->getSlots().size());
    setJsNumber(ctx,out,"skins",(double)data->getSkins().size());
    setJsNumber(ctx,out,"buildMs",(nowUs()-t0)/1000.0);
    // 名字表一次性下发：JS 门面之后全部 index 通信，杜绝每帧字符串桥。
    setJsProperty(ctx,out,"animations",spinexNameArray(ctx,data->getAnimations(),exc,
        [](spine::Animation* a){return a?a->getName().buffer():"";}));
    setJsProperty(ctx,out,"events",spinexNameArray(ctx,data->getEvents(),exc,
        [](spine::EventData* e){return e?e->getName().buffer():"";}));
    setJsProperty(ctx,out,"slotNames",spinexNameArray(ctx,data->getSlots(),exc,
        [](spine::SlotData* s){return s?s->getName().buffer():"";}));
    setJsProperty(ctx,out,"skinNames",spinexNameArray(ctx,data->getSkins(),exc,
        [](spine::Skin* s){return s?s->getName().buffer():"";}));
    {
        spine::Vector<spine::Animation*>& anims=data->getAnimations();
        std::vector<JSValueRef> ds;ds.reserve(anims.size());
        for(size_t i=0;i<anims.size();++i)ds.push_back(JSValueMakeNumber(ctx,anims[i]?anims[i]->getDuration():0));
        setJsProperty(ctx,out,"durations",JSObjectMakeArray(ctx,ds.size(),ds.empty()?nullptr:ds.data(),exc));
    }
    setJsProperty(ctx,out,"boneNames",spinexNameArray(ctx,data->getBones(),exc,
        [](spine::BoneData* b){return b?b->getName().buffer():"";}));
    {
        spine::Vector<spine::SlotData*>& slots=data->getSlots();
        std::vector<JSValueRef> indices;indices.reserve(slots.size());
        for(size_t i=0;i<slots.size();++i)indices.push_back(JSValueMakeNumber(ctx,slots[i]->getBoneData().getIndex()));
        setJsProperty(ctx,out,"slotBoneIndices",JSObjectMakeArray(ctx,indices.size(),indices.empty()?nullptr:indices.data(),exc));
    }
    return out;
}
static JSValueRef cbSpinexProbe(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    auto fail=[&](const char* why)->JSValueRef{
        setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,false));
        setJsProperty(ctx,out,"error",fguiJsString(ctx,why?:"fail"));
        return out;
    };
    if(argc<3)return fail("need (key, animName, timeSec)");
    std::string key=jsToStd(ctx,args[0]),anim=jsToStd(ctx,args[1]);
    double t=JSValueToNumber(ctx,args[2],exc);
    if(key.empty()||anim.empty()||!(t>=0))return fail("bad args");
    std::string error;
    spinex::DataStore::Entry* entry=spinexEnsureBuilt(key,/*preparedHandle=*/0,&error);   // 调试探针:短 key 即可
    if(!entry)return fail(error.empty()?"build failed":error.c_str());
    spinex::Instance instance(entry->data);   // 临时实例：探针即建即弃
    if(!instance.setAnimation(0,anim.c_str(),true))return fail("animation not found");
    instance.update((float)t);
    spine::Vector<spine::Bone*>& bones=instance.skeleton()->getBones();
    std::vector<JSValueRef> rows;rows.reserve(bones.size());
    for(size_t i=0;i<bones.size();++i){
        spine::Bone* bone=bones[i];JSObjectRef row=JSObjectMake(ctx,nullptr,nullptr);
        JSStringRef s=JSStringCreateWithUTF8CString(bone->getData().getName().buffer()?:"");
        setJsProperty(ctx,row,"name",JSValueMakeString(ctx,s));JSStringRelease(s);
        setJsNumber(ctx,row,"a",bone->getA());setJsNumber(ctx,row,"b",bone->getB());
        setJsNumber(ctx,row,"c",bone->getC());setJsNumber(ctx,row,"d",bone->getD());
        setJsNumber(ctx,row,"worldX",bone->getWorldX());setJsNumber(ctx,row,"worldY",bone->getWorldY());
        rows.push_back(row);
    }
    setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,true));
    setJsNumber(ctx,out,"count",(double)rows.size());
    setJsProperty(ctx,out,"bones",JSObjectMakeArray(ctx,rows.size(),rows.empty()?nullptr:rows.data(),exc));
    return out;
}
static JSValueRef cbSpinexAttachmentInfo(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    spinex::Instance* inst=argc>=2?spinexInstArg(ctx,argc,args,exc):nullptr;
    spine::Slot* slot=nullptr;
    if(inst){
        int slotIdx=(int)JSValueToNumber(ctx,args[1],exc);
        spine::Vector<spine::Slot*>& slots=inst->skeleton()->getSlots();
        if(slotIdx>=0&&(size_t)slotIdx<slots.size())slot=slots[slotIdx];
    }
    spine::Attachment* att=slot?slot->getAttachment():nullptr;
    if(!att){setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,false));return out;}
    setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,true));
    const char* type="other";
    if(att->getRTTI().isExactly(spine::RegionAttachment::rtti)){
        type="region";
        spine::RegionAttachment* ra=(spine::RegionAttachment*)att;
        setJsNumber(ctx,out,"x",ra->getX());setJsNumber(ctx,out,"y",ra->getY());
        setJsNumber(ctx,out,"scaleX",ra->getScaleX());setJsNumber(ctx,out,"scaleY",ra->getScaleY());
        setJsNumber(ctx,out,"rotation",ra->getRotation());
        setJsNumber(ctx,out,"width",ra->getWidth());setJsNumber(ctx,out,"height",ra->getHeight());
    }
    else if(att->getRTTI().isExactly(spine::MeshAttachment::rtti))type="mesh";
    else if(att->getRTTI().isExactly(spine::ClippingAttachment::rtti))type="clipping";
    JSStringRef ts=JSStringCreateWithUTF8CString(type);
    setJsProperty(ctx,out,"type",JSValueMakeString(ctx,ts));JSStringRelease(ts);
    JSStringRef ns=JSStringCreateWithUTF8CString(att->getName().buffer()?:"");
    setJsProperty(ctx,out,"name",JSValueMakeString(ctx,ns));JSStringRelease(ns);
    return out;
}
void spinexFrameDrive() {   // host_core_common 平台接口
    double nowMs = nowUs() * 0.001;
    if (!gSpinexRt.instanceCount()) { gSpinexRt.noteIdleFrame(nowMs); return; }
    gSpinexRt.updateAll(nowMs, &gSG, gSGBuf, (float)gScreenW, (float)gScreenH);
}

// ======================= 键盘/prompt/截图/reload 桥(平台服务 → JNI Kotlin) =======================
static JSValueRef cbKbShow(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    std::string text = argc>0 ? jsToStd(ctx, a[0]) : std::string();
    int maxLen = argc>1 ? (int)JSValueToNumber(ctx, a[1], e) : 0;
    bool secure = argc>2 && JSValueToNumber(ctx, a[2], e) != 0;
    // a[3] = multiline(暂未用) a[4..7] = 输入框 x,y,w,h(游戏像素:触点命中判定 + 键盘避让)
    float rx=0,ry=0,rw=0,rh=0;
    if (argc > 7) { rx=(float)JSValueToNumber(ctx,a[4],e); ry=(float)JSValueToNumber(ctx,a[5],e); rw=(float)JSValueToNumber(ctx,a[6],e); rh=(float)JSValueToNumber(ctx,a[7],e); }
    JNIEnv* env = jniEnv();
    if (env && gActivity && gMidKbShow) {
        jstring jt = jstr(env, text);
        env->CallVoidMethod(gActivity, gMidKbShow, jt, maxLen, (jboolean)secure, rx, ry, rw, rh);
        env->DeleteLocalRef(jt);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbKbSetText(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef*) {
    if (argc > 0) {
        JNIEnv* env = jniEnv();
        if (env && gActivity && gMidKbSetText) {
            jstring jt = jstr(env, jsToStd(ctx, a[0]));
            env->CallVoidMethod(gActivity, gMidKbSetText, jt);
            env->DeleteLocalRef(jt);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbKbHide(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    JNIEnv* env = jniEnv();
    if (env && gActivity && gMidKbHide) { env->CallVoidMethod(gActivity, gMidKbHide); if (env->ExceptionCheck()) env->ExceptionClear(); }
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbKbKeep(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    JNIEnv* env = jniEnv();
    if (env && gActivity && gMidKbKeep) { env->CallVoidMethod(gActivity, gMidKbKeep); if (env->ExceptionCheck()) env->ExceptionClear(); }
    return JSValueMakeUndefined(ctx);
}
// __snapshot(quality) -> "data:image/jpeg;base64,...":整屏截图。Android 读默认 framebuffer 像素,
// Kotlin 压 JPEG(iOS 走 UIKit 合成;此处等效——native-only 主屏内容全部在 GL 里)。
static JSValueRef cbSnapshot(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    double q = argc>0 ? JSValueToNumber(ctx, a[0], e) : 0.85;
    if (!(q > 0 && q <= 1)) q = 0.85;
    JNIEnv* env = jniEnv();
    if (!env || !gActivity || !gMidSnapshot) return JSValueMakeNull(ctx);
    auto url = (jstring)env->CallObjectMethod(gActivity, gMidSnapshot, (jfloat)q);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return JSValueMakeNull(ctx); }
    if (!url) return JSValueMakeNull(ctx);
    const char* chars = env->GetStringUTFChars(url, nullptr);
    JSStringRef s = JSStringCreateWithUTF8CString(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(url, chars);
    env->DeleteLocalRef(url);
    JSValueRef ret = JSValueMakeString(ctx, s); JSStringRelease(s);
    return ret;
}
static JSValueRef cbShowReload(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef*) {
    std::string title   = argc>0 ? jsToStd(ctx, args[0]) : std::string();
    std::string content = argc>1 ? jsToStd(ctx, args[1]) : std::string();
    JNIEnv* env = jniEnv();
    if (env && gActivity && gMidShowReload) {
        jstring jt = jstr(env, title), jc = jstr(env, content);
        env->CallVoidMethod(gActivity, gMidShowReload, jt, jc);
        env->DeleteLocalRef(jt); env->DeleteLocalRef(jc);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbHideReload(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    JNIEnv* env = jniEnv();
    if (env && gActivity && gMidHideReload) { env->CallVoidMethod(gActivity, gMidHideReload); if (env->ExceptionCheck()) env->ExceptionClear(); }
    return JSValueMakeUndefined(ctx);
}
static bool setupGame();
static void reloadGame();
static JSValueRef cbReload(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    // 从 JS 内直接调用(location.reload()):延到下一帧再重建上下文,避免在旧上下文执行栈里销毁它。
    gMainQ.post([]{ reloadGame(); });
    return JSValueMakeUndefined(ctx);
}
// __nativePrompt(title, value, callback):非阻塞编辑框。callback protect 到 Kotlin 对话框完成。
static std::map<int, std::pair<JSObjectRef, JSGlobalContextRef>> gPromptCallbacks;
static int gPromptToken = 0;
static JSValueRef cbNativePrompt(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                 const JSValueRef args[], JSValueRef*) {
    std::string title = argc>0 ? jsToStd(ctx,args[0]) : std::string("Edit value");
    std::string value = argc>1 ? jsToStd(ctx,args[1]) : std::string();
    JSObjectRef callback=nullptr;
    if(argc>2&&JSValueIsObject(ctx,args[2])){
        JSObjectRef c=JSValueToObject(ctx,args[2],nullptr);
        if(c&&JSObjectIsFunction(ctx,c))callback=c;
    }
    JNIEnv* env = jniEnv();
    if(!callback||!env||!gActivity||!gMidPrompt)return JSValueMakeUndefined(ctx);
    JSValueProtect(gCtx,callback);
    int token = ++gPromptToken;
    gPromptCallbacks[token] = {callback, gCtx};
    jstring jt = jstr(env, title.empty() ? "Edit value" : title), jv = jstr(env, value), jp = jstr(env, "");
    env->CallVoidMethod(gActivity, gMidPrompt, token, jt, jv, jp);
    env->DeleteLocalRef(jt); env->DeleteLocalRef(jv); env->DeleteLocalRef(jp);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return JSValueMakeUndefined(ctx);
}
static void finishPrompt(int token, bool hasText, const std::string& text) {
    auto it = gPromptCallbacks.find(token);
    if (it == gPromptCallbacks.end()) return;
    JSObjectRef callback = it->second.first;
    JSGlobalContextRef requestContext = it->second.second;
    gPromptCallbacks.erase(it);
    if (gCtx != requestContext) return;   // reload 后旧回调作废(protect 引用随旧上下文释放)
    JSValueRef av = hasText ? (JSValueRef)nullptr : JSValueMakeNull(gCtx);
    JSStringRef s = nullptr;
    if (hasText) { s = JSStringCreateWithUTF8CString(text.c_str()); av = JSValueMakeString(gCtx, s); }
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(gCtx, callback, nullptr, 1, &av, &exc);
    if (s) JSStringRelease(s);
    JSValueUnprotect(gCtx, callback);
}

// ======================= 主屏绘制/帧循环(与 ios drawOneFrame/flipMainScreenDraw 同源) =======================
static void flipMainScreenDraw(int root) {
    if (root <= 0 || gEglSurface == EGL_NO_SURFACE) return;
    double t0 = nowUs();
    // 先发布变换再驱动动画:render 前一刻 attach 的节点不能把分配期/前父节点的世界坐标暴露给 Spine。
    // Phase D-1:动画推进已移至 ADVANCE_NATIVE(JS phase4 经 __sgAdvanceAnimations);
    // 兜底:旧 JS/其他 draw 路径未推进时按原逻辑就地推进(过渡期兼容,JS 全量升级后可删)。
    if (!gAnimAdvancedThisFrame) advanceNativeAnimationsForFrame(root);
    gAnimAdvancedThisFrame = false;   // 消费帧标志
    double computeStart = nowUs();
    // Phase D-1:主路径的最终收敛由 JS 两段式(FINAL_COMPUTE)完成,draw 零 compute;
    // 仅兜底(本帧动画在 draw 内就地推进过)时补一次收敛。
    if (gSG.buf && gSG.buf[sg::DEBT_BASE + root] > 0.5f) gSG.compute(root, sg::CM_CANONICAL_MAIN);
    gFrameComputeUs = nowUs() - computeStart;
    if (!gBatcher.ready()) {
        gBatcher.texLookup = [](int id){ return gDec.glTexture((uint32_t)id); };
        gBatcher.init();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, gRenderW, gRenderH);
    glClearColor(gSgClear[0], gSgClear[1], gSgClear[2], gSgClear[3]);
    glDisable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    double batchStart = nowUs();
    gBatcher.frame(&gSG, root, (float)gScreenW, (float)gScreenH);
    ++gNativeBatchSerial;   // fgui 预约/回收按这个序号判活与老化
    gFrameBatchUs = nowUs() - batchStart;
    gDec.drawCalls += gBatcher.drawCalls;
    gFrameDecodeUs += nowUs() - t0;
    // 不置位就会被"静止帧跳过"当成"JS 没渲染"而跳过 present。
    gFrameHadCmds = true; gFrameHadContent = true;
}
// Sandbox-only A/B control: switch equivalent shader paths without reloading the
// scene or changing animation timing. Normal launches keep the batcher's default.
static JSValueRef cbNativeSpriteSpecialization(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                               size_t argc, const JSValueRef args[], JSValueRef*) {
    if (argc) {
        gBatcher.specializeSprites = JSValueToBoolean(ctx, args[0]);
        ALOGI("[sprite-ab] specialized=%d", gBatcher.specializeSprites ? 1 : 0);
    }
    return JSValueMakeBoolean(ctx, gBatcher.specializeSprites);
}
static JSValueRef cbNativeProfilePost(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                     size_t argc, const JSValueRef args[], JSValueRef*) {
    if (argc) gBatcher.profilePost = JSValueToBoolean(ctx, args[0]);
    return JSValueMakeBoolean(ctx, gBatcher.profilePost);
}
static JSValueRef cbNativePostTargetOptimization(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                                size_t argc, const JSValueRef args[], JSValueRef*) {
    if (argc) {
        const bool enabled = JSValueToBoolean(ctx, args[0]);
        gBatcher.compactPostTargets = enabled;
        gBatcher.postTargetBanks = enabled ? 3 : 1;
        ALOGI("[post-ab] optimized=%d", enabled ? 1 : 0);
    }
    return JSValueMakeBoolean(ctx, gBatcher.compactPostTargets);
}
static void drawOneFrame() {
    if (!gCtx) return;
    double frameBegin = nowUs();
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef fn = JSStringCreateWithUTF8CString("__tick");
    JSValueRef fv = JSObjectGetProperty(gCtx, global, fn, nullptr); JSStringRelease(fn);
    if (!JSValueIsObject(gCtx, fv)) return;
    JSObjectRef tickFn = JSValueToObject(gCtx, fv, nullptr);
    if (!tickFn || !JSObjectIsFunction(gCtx, tickFn)) return;
    gDec.drawCalls = 0; gDec.texUploads = 0; gDec.texDeferred = 0; gDec.bufUploads = 0;
    gFrameDecodeUs = gFrameJSUs = gFrameComputeUs = gFrameBatchUs = gFramePresentUs = 0;
    gFrameHadCmds = false; gFramePresented = false; gFrameHadContent = false;
    // JS 之前先做完一份有上限的 ResourceManager GPU 工作量(大图集按行带切分)。
    gNativeResources.beginFrame();
    gDec.beginFrame();   // 大图上传节流:补传一张推迟的图集
    // 预约补货/transition/延时任务在 JS 之前于 C++ 推进,JS 只领完成令牌。
    gNativeFgui.advanceReservationRefills((nowUs()-gStartUs)/1000.0,gNativeBatchSerial,gSG);
    gNativeFgui.advanceTransitions((nowUs()-gStartUs)/1000.0, gSG);
    gNativeFgui.advanceTasks((nowUs()-gStartUs)/1000.0);
    // Drain naturally completed Transition tokens before the application ticker. Once C++ has
    // removed a completed timeline, a same-frame JS stop/replay must not be able to discard its
    // still-pending completion callback. Property/visibility shadows follow the same boundary.
    JSValueRef transitionNow = JSValueMakeNumber(gCtx, (nowUs()-gStartUs)/1000.0);
    callJsGlobal("__nativeFguiTransitionFrame", 1, &transitionNow);
    JSValueRef arg = JSValueMakeNumber(gCtx, (nowUs()-gStartUs)/1000.0);
    JSValueRef exc = nullptr;
    double jsStart = nowUs();
    gFramePreJSUs = jsStart - frameBegin;
    JSObjectCallAsFunction(gCtx, tickFn, nullptr, 1, &arg, &exc);
    gFrameJSUs = nowUs() - jsStart;
    if (exc) { std::string es = jsErrorDetail(gCtx, exc); ALOGE("[game] __tick 异常: %s", es.c_str()); reportRuntimeError("javascript_runtime", "tick_exception", es, false); }
    // 帧尾兜底:正常路径已在 __nativePresent 调用点内联绘制;此分支只服务 __sgFlagRender 旧语义。
    if (gPendingRoot > 0 && gEglSurface != EGL_NO_SURFACE) {
        int root = gPendingRoot; gPendingRoot = 0;
        flipMainScreenDraw(root);
    }
    // 静止帧跳过:本帧无 GL 命令 → 不 swap(EGL swap 后 buffer 内容不保证,空 swap 会闪),GPU 休息。
    if (gFrameHadCmds && gEglDisplay != EGL_NO_DISPLAY && gEglSurface != EGL_NO_SURFACE) {
        double presentStart = nowUs();
        eglSwapBuffers(gEglDisplay, gEglSurface);
        gFramePresented = true;
        gFramePresentUs = nowUs() - presentStart;
    }
}

// ======================= setupGame / reload(与 ios L3309-3661 同源顺序) =======================
static bool setupGame() {
    // 新激活的内容包只有一轮(kTrialLaunchLimit 次)JS 上下文启动机会证明自己健康;上一轮没等到
    // 同时推进内容缓存的事务代际:旧上下文在途的 store/verify/commit 从此被拒绝,不能穿越 reload。
    // 此前 Android 从未调用 → trial 计数/trial-failed 回滚/3 次拉黑/事务代际全部失效(2026-09-17 审计 P0-1)。

    // 旧世界的音频随世界消亡(首次启动为空操作)。
    shutdownAudio();
    // 建新上下文之前先推进代号:旧 worker 既不能消费也不能通知本次 setup 的 waiter。
    ++gSpinePrepareEpoch;
    gSpinePrepareInflight.clear();
    // 上下文换代:旧 JS 世界的在途 lease(activeUsers)随世界消亡,无人再 unpin——prepared
    // 三表整体清空,否则 pinned holder 永久不可逐出。数据丢失无碍:reload 罕见,重新 preparse。
    gSpinePreparedCache.clear();
    gSpinePreparedByHandle.clear();
    gSpinePreparedLru.clear();
    // 逐个丢弃而不是直接 clear:未 commit 的 job 还占着已 retain 的页图纹理。
    for (auto &kv : gSpinexPrewarm) gSpinexRt.store.discardAsync(kv.second);
    gSpinexPrewarm.clear();
    gHostEpoch.fetch_add(1);
    clearAllTimers();
    gPromptCallbacks.clear();
    gZipCache.clear(); gZipLru.clear();
    gNativeText.clear();
    gNativeResources.reset();
    macres::gDecodeFallback = decodeImageViaKotlin;   // 资源管理器 stb 失败时回退(WebP 页图)
    gNativeResources.setImageDiagnostic([](const std::string& url, bool ok, size_t bytes, const std::string& error) {
        resourceDiagnostic("decode", url, "decoder", ok, bytes, ok ? "" : "ImageDecodeFailed", error);
    });
    gNativeResources.configure(&gDec,
        [](const std::string& url) -> macres::Bytes { return loadAsset(url.c_str()); },
        [](std::function<void()> fn) { gLoadPool.post(std::move(fn)); },
        [](std::function<void()> fn) { gMainQ.post(std::move(fn)); },
        []{ gCpuGate.acquire(); }, []{ gCpuGate.release(); });
    if (gSGBuf) gNativeFgui.reset(gSG);
    // preparse 出来的是一次性的、脱离运行时的蓝图,绝不能带进新世代。
    gFguiPreparseInflight.clear();
    gFguiPreparseCache.clear();
    gFguiPreparseLru.clear();
    gFguiXmlPrewarm.clear();
    gFguiPreparseStats = {};
    gNativeTweens.clear();
    gCtx = JSGlobalContextCreate(nullptr);
    ALOGI("[game] JS engine %s %s", jsengine::kName, jsengine::version());
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSValueRef exc = nullptr;
    // 把首次 Text/Graphics/Sprite 光栅化摊到多帧(否则大弹窗一个 __tick 造 200-500 张纹理)。
    {
        JSStringRef key = JSStringCreateWithUTF8CString("__nativeRenderWorkBudget");
        JSObjectSetProperty(gCtx, global, key, JSValueMakeNumber(gCtx, 24), kJSPropertyAttributeReadOnly, nullptr);
        JSStringRelease(key);
    }
    // shell 模块分帧加载预算(ms)。
    {
        JSStringRef key = JSStringCreateWithUTF8CString("__SHELL_LOAD_BUDGET");
        JSObjectSetProperty(gCtx, global, key, JSValueMakeNumber(gCtx, 8), kJSPropertyAttributeNone, nullptr);
        JSStringRelease(key);
    }
    if (!gBuf) gBuf = (uint32_t*)malloc(kBufWords * 4);
    JSObjectRef ab = JSObjectMakeArrayBufferWithBytesNoCopy(gCtx, gBuf, kBufWords*4, nullptr, nullptr, &exc);
    JSStringRef cmdName = JSStringCreateWithUTF8CString("__cmd");
    JSObjectSetProperty(gCtx, global, cmdName, ab, kJSPropertyAttributeNone, NULL); JSStringRelease(cmdName);
    // 原生场景图共享 buffer(Float32),init + 暴露 __sg
    if (!gSGBuf) { size_t n = (size_t)sg::SHARED_FLOATS; gSGBuf = (float*)calloc(n, sizeof(float)); gSG.init(gSGBuf); }
    else gSG.init(gSGBuf);   // reload 时重置树(buffer 复用)
    // fgui 纯色图元的 1x1 白纹理:整棵树共用一个外部纹理 id,合批不被打断。
    if (!gNativeFguiWhiteTexture) {
        const uint8_t white[4] = {255,255,255,255}; GLint prior = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prior);
        glGenTextures(1, &gNativeFguiWhiteTexture); glBindTexture(GL_TEXTURE_2D, gNativeFguiWhiteTexture); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,white);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glBindTexture(GL_TEXTURE_2D,(GLuint)prior);
        gDec.registerExternalTexture(kNativeFguiWhiteCid,gNativeFguiWhiteTexture);
    }
    gNativeFgui.solidTextureId = (int)kNativeFguiWhiteCid;
    // fgui 文本节点由 C++ 直接驱动原生文本光栅化:包 XML 属性 → 字体串/度量 → 纹理,全程不经 JS。
    gNativeFgui.prepareText=[](int node,nfgui::InstanceNode& desc,nfgui::TextPrepareMode mode)->bool{
        auto attr=[&](const char* key,const char* fallback="")->std::string{auto it=desc.attrs.find(key);return it==desc.attrs.end()?std::string(fallback):it->second;};
        const bool facadeNatural=mode==nfgui::TextPrepareMode::FacadeNatural;
        std::string text=attr("text"),face=attr("font","sans-serif"),fill=attr("color","#000000"),stroke=attr("strokeColor","transparent"),align=attr("align","left");
        if(face.empty())face="sans-serif";
        const bool hasStroke=stroke!="transparent"&&!stroke.empty();
        double size=nfgui::number(attr("fontSize"),12),strokeWidth=hasStroke?(attr("strokeSize").empty()?2:nfgui::number(attr("strokeSize"))+1):0;
        double leading=nfgui::number(attr("leading"),3),letterSpacing=nfgui::number(attr("letterSpacing"));
        std::string font;
        if(facadeNatural){
            font=(attr("italic")=="true"?"italic":"normal")+std::string(" normal ")+
                (attr("bold")=="true"?"bold":"normal")+" "+std::to_string(size)+"px \""+face+"\"";
        }else{
            if(attr("italic")=="true")font+="italic ";if(attr("bold")=="true")font+="bold ";font+=std::to_string(size)+"px "+face;
        }
        const bool shrink=!facadeNatural&&desc.kind==nfgui::ObjectKind::Text&&attr("autoSize")=="shrink";
        double width=0,height=0,lineHeight=0;
        double wrap=nfgui::plainTextWrapWidth(facadeNatural,attr("singleLine")=="true",shrink,desc.width);
        double requestedLineHeight=0;
        double boxWidth=facadeNatural||shrink?0:desc.width,boxHeight=facadeNatural||shrink?0:desc.height;
        std::string vertical=facadeNatural?"top":attr("vAlign","top");
        bool ok=false;
        if(desc.kind==nfgui::ObjectKind::RichText)ok=gNativeText.updateRich(node,text,font,fill,stroke,strokeWidth,align,1,0,0,wrap,requestedLineHeight,boxWidth,boxHeight,vertical,width,height,lineHeight);
        else ok=gNativeText.update(node,text,font,fill,stroke,strokeWidth,"transparent",0,0,0,align,facadeNatural?0:2,1,0,0,wrap,requestedLineHeight,boxWidth,boxHeight,vertical,letterSpacing,leading,"","",0,0,0,width,height,lineHeight);
        if(ok&&shrink&&node>0&&node<sg::NODE_CAP&&gSGBuf){
            const auto fit=nfgui::plainTextShrinkLayout(width,height,desc.width,desc.height,align,vertical);
            float* slot=gSGBuf+(size_t)node*sg::STRIDE;
            slot[sg::RX]=fit.x;slot[sg::RY]=fit.y;slot[sg::RW]=fit.width;slot[sg::RH]=fit.height;
            gSG.markBoundsDirty(node);
        }
        if(desc.kind==nfgui::ObjectKind::Text&&attr("font").compare(0,5,"ui://")!=0){
            const bool valid=ok&&std::isfinite(width)&&std::isfinite(height)&&std::isfinite(lineHeight)&&
                width>=0&&height>=0&&lineHeight>=0&&width<=16384&&height<=16384;
            desc.nativeTextMetricsValid=valid;
            desc.nativeTextWidth=valid?(float)width:0;desc.nativeTextHeight=valid?(float)height:0;
            desc.nativeTextLineHeight=valid?(float)lineHeight:0;
        }
        return ok;
    };
    gNativeFgui.releaseNode=[](int node){gNativeText.detach(node);gSpinexRt.onNodeFreed(node);};
    {
        JSObjectRef sgab = JSObjectMakeArrayBufferWithBytesNoCopy(gCtx, gSGBuf, (size_t)sg::SHARED_FLOATS*4, nullptr, nullptr, &exc);
        JSStringRef nm = JSStringCreateWithUTF8CString("__sg");
        JSObjectSetProperty(gCtx, global, nm, sgab, kJSPropertyAttributeNone, NULL); JSStringRelease(nm);
    }
    hostCoreRegisterCommonBindings(gCtx, global);   // 三端共用绑定表(src/host/host_core_common.cpp);Android 专有的仍逐条 setFn
    if (!pixinative_environment::kProductionBuild)
        setFn(gCtx, global, "__nativeSpriteSpecialization", cbNativeSpriteSpecialization);
    if (!pixinative_environment::kProductionBuild)
        setFn(gCtx, global, "__nativeProfilePost", cbNativeProfilePost);
        setFn(gCtx, global, "__nativePostTargetOptimization", cbNativePostTargetOptimization);
    setFn(gCtx, global, "__decodeScriptUTF8", cbDecodeScriptUTF8);   // 脚本字节原生解码(≙ mac/iOS)



    if (gBootSealEnforce) { setJsNumber(gCtx, global, "__NATIVE_BOOT_SEAL_ENFORCE", 1.0); ALOGI("[native-boot] seal enforce ON (gradle PIXINATIVE_BOOT_SEAL_ENFORCE)"); }   // ≙ injectNativeBootConfiguration
    if (!gHostQuery.empty()) { setReadOnlyString(gCtx, global, "__HOST_QUERY", gHostQuery); ALOGI("[game] host query override: %s", gHostQuery.c_str()); }   // 诊断:覆盖 game_host 的 URL query
    if (!gJsPrelude.empty()) {   // 诊断前置脚本(debug 包;adb --es jsPrelude "globalThis.__LOGIN_TRACE=1;")
        JSStringRef src = JSStringCreateWithUTF8CString(gJsPrelude.c_str()); JSValueRef perr = nullptr;
        JSEvaluateScript(gCtx, src, nullptr, nullptr, 1, &perr); JSStringRelease(src);
        ALOGI("[game] js prelude %s: %.160s", perr ? "FAILED" : "ok", gJsPrelude.c_str());
        jsProfMaybeEnable();   // prelude 置了 __NATIVE_JS_PROFILE 就从这里开始采样(game_host.js 之前,覆盖整个启动)
        if (perr) reportRuntimeError("javascript_boot", "prelude_exception", jsErrorDetail(gCtx, perr), false);
    }
    if (!gAstcSupported) { JSStringRef k = JSStringCreateWithUTF8CString("__NATIVE_DISABLE_ASTC"); JSObjectSetProperty(gCtx, global, k, JSValueMakeBoolean(gCtx, true), kJSPropertyAttributeReadOnly, nullptr); JSStringRelease(k); }
    // 本地持久化 / 剪贴板 / openURL / spinex 附件(Android 缺口收口)
    setFn(gCtx, global, "__persistentStorageGet", cbPersistentStorageGet);
    setFn(gCtx, global, "__persistentStorageSet", cbPersistentStorageSet);
    setFn(gCtx, global, "__persistentStorageRemove", cbPersistentStorageRemove);
    setFn(gCtx, global, "__persistentStorageClear", cbPersistentStorageClear);
    setFn(gCtx, global, "__clipboardWrite", cbClipboardWrite);
    setFn(gCtx, global, "__nativeOpenURL", cbNativeOpenURL);
    setFn(gCtx, global, "__spxSetSlotAttachment", cbSpinexSetSlotAttachment);
    setFn(gCtx, global, "__spxUpdateAttachmentRegion", cbSpinexUpdateAttachmentRegion);
    // canvas2d(≙ canvas2d_jsc.h 的 19 个 __c2d*;Kotlin Canvas2D.kt 实现)
    setFn(gCtx, global, "__c2dCreate", cbC2dCreate);
    setFn(gCtx, global, "__c2dResize", cbC2dResize);
    setFn(gCtx, global, "__c2dClearRect", cbC2dClearRect);
    setFn(gCtx, global, "__c2dFillRect", cbC2dFillRect);
    setFn(gCtx, global, "__c2dFillText", cbC2dFillText);
    setFn(gCtx, global, "__c2dFillTextGrad", cbC2dFillTextGrad);
    setFn(gCtx, global, "__c2dStrokeText", cbC2dStrokeText);
    setFn(gCtx, global, "__c2dMeasure", cbC2dMeasure);
    setFn(gCtx, global, "__c2dGetData", cbC2dGetData);
    setFn(gCtx, global, "__c2dPutData", cbC2dPutData);
    setFn(gCtx, global, "__c2dToDataURL", cbC2dToDataURL);
    setFn(gCtx, global, "__c2dSave", cbC2dSave);
    setFn(gCtx, global, "__c2dRestore", cbC2dRestore);
    setFn(gCtx, global, "__c2dScale", cbC2dScale);
    setFn(gCtx, global, "__c2dTranslate", cbC2dTranslate);
    setFn(gCtx, global, "__c2dRotate", cbC2dRotate);
    setFn(gCtx, global, "__c2dTransform", cbC2dTransform);
    setFn(gCtx, global, "__c2dSetTransform", cbC2dSetTransform);
    setFn(gCtx, global, "__c2dResetTransform", cbC2dResetTransform);
    // 渲染杂项:九宫/平铺/位图字形/Graphics 网格 + RenderTexture 离屏合批 + native tween
    setFn(gCtx, global, "__nativeBatchCoverage", cbNativeBatchCoverage);
    setFn(gCtx, global, "__nativeRenderTexturePreflight", cbNativeRenderTexturePreflight);
    setFn(gCtx, global, "__nativeRenderTexture", cbNativeRenderTexture);
    setFn(gCtx, global, "__shadowFlag", cbShadowFlag);   // legacy alias(Android 无 shadow 旁路)
    setFn(gCtx, global, "__nativePresent", cbNativePresent);
    // spine 蒙皮桥共享 buffer(旧 __spReg 桥组已被 spinex 取代,只保留 buffer 与 mac/iOS 对齐)
    if (!gSpBuf) gSpBuf = (float*)calloc(spsk::TOTAL_F, sizeof(float));
    gSpSkin.init(gSpBuf);
    gSpSkin.insts.clear();
    gSpinexRt.resetAll();    // reload 后旧 instId/sdHandle 全部失效
    {
        JSObjectRef ab2 = JSObjectMakeArrayBufferWithBytesNoCopy(gCtx, gSpBuf, (size_t)spsk::TOTAL_F*4, nullptr, nullptr, &exc);
        JSStringRef nm2 = JSStringCreateWithUTF8CString("__spBuf");
        JSObjectSetProperty(gCtx, global, nm2, ab2, kJSPropertyAttributeNone, NULL); JSStringRelease(nm2);
    }
    if (!gSkelBatchBuf) gSkelBatchBuf = (float*)calloc(sk::BATCH_TOTAL_F, sizeof(float));
    {
        JSObjectRef skbab = JSObjectMakeArrayBufferWithBytesNoCopy(gCtx, gSkelBatchBuf, (size_t)sk::BATCH_TOTAL_F*4, nullptr, nullptr, &exc);
        JSStringRef skbnm = JSStringCreateWithUTF8CString("__skelBatchBuf");
        JSObjectSetProperty(gCtx, global, skbnm, skbab, kJSPropertyAttributeNone, NULL); JSStringRelease(skbnm);
    }
    setFn(gCtx, global, "__zstdDecompress", cbZstdDecompress);
    // spinex __spx* 桥组:与 mac/iOS 一一对应,一个不少(遗漏任一门面桥都会让 JS 侧收编类静默降级)。
    setFn(gCtx, global, "__spxBuild", cbSpinexBuild);
    setFn(gCtx, global, "__spxProbe", cbSpinexProbe);
    setFn(gCtx, global, "__spxAttachmentInfo", cbSpinexAttachmentInfo);
    // spine preparse 桥:JS 侧以 __nativeSpinePreparse 是否存在作为整套门面的安装闸门。
    setFn(gCtx, global, "__nativeSpinePreparse", cbNativeSpinePreparse);
    setFn(gCtx, global, "__nativeSpinePin", cbNativeSpinePin);
    setFn(gCtx, global, "__nativeSpineUnpin", cbNativeSpineUnpin);
    setFn(gCtx, global, "__nativeSpineManifest", cbNativeSpineManifest);
    setFn(gCtx, global, "__nativeSpineGet", cbNativeSpineGet);
    setFn(gCtx, global, "__loadSpinePreparedImage", cbLoadSpinePreparedImage);
    setFn(gCtx, global, "__nativeSpineRecordFallback", cbNativeSpineRecordFallback);
    setFn(gCtx, global, "__nativeSpinePreparseStats", cbNativeSpinePreparseStats);
    setFn(gCtx, global, "__decodeImageAsync", cbDecodeImageAsync);
    { JSStringRef key=JSStringCreateWithUTF8CString("__nativeSupportsImageSourcePMA");
      JSObjectSetProperty(gCtx,global,key,JSValueMakeBoolean(gCtx,true),kJSPropertyAttributeReadOnly,nullptr);JSStringRelease(key); }
    setFn(gCtx, global, "__nativeResourceRetain", cbNativeResourceRetain);
    setFn(gCtx, global, "__nativeResourceAlive", cbNativeResourceAlive);
    setFn(gCtx, global, "__nativeResourceRelease", cbNativeResourceRelease);
    setFn(gCtx, global, "__nativeResourceReleaseAdopt", cbNativeResourceReleaseAdopt);
    setFn(gCtx, global, "__nativeResourceTransferred", cbNativeResourceTransferred);
    setFn(gCtx, global, "__nativeResourceStats", cbNativeResourceStats);
    setFn(gCtx, global, "__nativeTextUpdate", cbNativeTextUpdate);
    setFn(gCtx, global, "__nativeRichTextUpdate", cbNativeRichTextUpdate);
    setFn(gCtx, global, "__nativeTextUpdateV2", cbNativeTextUpdateV2);        // 三宽度布局协议 V2(与 Apple 同语义,2026-09-03 接入)
    setFn(gCtx, global, "__nativeRichTextUpdateV2", cbNativeRichTextUpdateV2);
    setFn(gCtx, global, "__nativeFguiInflate", cbNativeFguiInflate);
    setFn(gCtx, global, "__nativeFguiEntry", cbNativeFguiEntry);
    setFn(gCtx, global, "__nativeFguiManifest", cbNativeFguiManifest);
    setFn(gCtx, global, "__nativeFguiItem", cbNativeFguiItem);
    setFn(gCtx, global, "__nativeFguiItemsByType", cbNativeFguiItemsByType);
    setFn(gCtx, global, "__nativeFguiSprite", cbNativeFguiSprite);
    setFn(gCtx, global, "__nativeFguiPreparse", cbNativeFguiPreparse);
    setFn(gCtx, global, "__nativeFguiPreparseStats", cbNativeFguiPreparseStats);
    setFn(gCtx, global, "__nativeFguiSetStrings", cbNativeFguiSetStrings);
    setFn(gCtx, global, "__nativeFguiXmlParse", cbNativeFguiXmlParse);
    setFn(gCtx, global, "__nativeFguiBindAtlas", cbNativeFguiBindAtlas);
    setFn(gCtx, global, "__nativeFguiPreparePackage", cbNativeFguiPreparePackage);
    setFn(gCtx, global, "__nativeFguiReserve", cbNativeFguiReserve);
    setFn(gCtx, global, "__nativeFguiClearReservations", cbNativeFguiClearReservations);
    setFn(gCtx, global, "__nativeFguiCreate", cbNativeFguiCreate);
    setFn(gCtx, global, "__nativeFguiChild", cbNativeFguiChild);
    setFn(gCtx, global, "__nativeFguiSetTextMutation", cbNativeFguiSetTextMutation);
    setFn(gCtx, global, "__nativeFguiCommitTextMutations", cbNativeFguiCommitTextMutations);
    setFn(gCtx, global, "__nativeFguiSetController", cbNativeFguiSetController);
    setFn(gCtx, global, "__nativeFguiCommitMutations", cbNativeFguiCommitMutations);
    setFn(gCtx, global, "__nativeFguiSetBitmapText", cbNativeFguiSetBitmapText);
    setFn(gCtx, global, "__nativeFguiTransitionPlay", cbNativeFguiTransitionPlay);
    setFn(gCtx, global, "__nativeFguiTransitionStop", cbNativeFguiTransitionStop);
    setFn(gCtx, global, "__nativeFguiTransitionSetValue", cbNativeFguiTransitionSetValue);
    setFn(gCtx, global, "__nativeFguiTransitionSetTarget", cbNativeFguiTransitionSetTarget);
    setFn(gCtx, global, "__nativeFguiTransitionSetDuration", cbNativeFguiTransitionSetDuration);
    setFn(gCtx, global, "__nativeFguiTransitionUpdateRelations", cbNativeFguiTransitionUpdateRelations);
    setFn(gCtx, global, "__nativeFguiTransitionChangeRepeat", cbNativeFguiTransitionChangeRepeat);
    setFn(gCtx, global, "__nativeFguiTransitionPlaying", cbNativeFguiTransitionPlaying);
    setFn(gCtx, global, "__auLoadUrl", cbAuLoadUrl);
    setFn(gCtx, global, "__auLoadUrlAsync", cbAuLoadUrlAsync);
    setFn(gCtx, global, "__auLoadData", cbAuLoadData);
    setFn(gCtx, global, "__auClone", cbAuClone);
    setFn(gCtx, global, "__auCtl", cbAuCtl);
    setFn(gCtx, global, "__nativeVibrate", cbNativeVibrate);
    setFn(gCtx, global, "__kbShow", cbKbShow);
    setFn(gCtx, global, "__kbHide", cbKbHide);
    setFn(gCtx, global, "__kbKeep", cbKbKeep);
    setFn(gCtx, global, "__kbSetText", cbKbSetText);
    setFn(gCtx, global, "__nativePrompt", cbNativePrompt);
    setFn(gCtx, global, "__snapshot", cbSnapshot);
    // 屏幕尺寸传给 game_host.js(innerWidth/innerHeight/devicePixelRatio)→ 按真实屏幕比例响应式铺满。
    auto setNum = [&](const char* k, double v){ JSStringRef s=JSStringCreateWithUTF8CString(k);
        JSObjectSetProperty(gCtx, global, s, JSValueMakeNumber(gCtx, v), kJSPropertyAttributeNone, NULL); JSStringRelease(s); };
    setNum("__screenW", gScreenW); setNum("__screenH", gScreenH); setNum("__screenDPR", gScreenDPR);
    setNum("__safeAreaLeft", gSafeAreaLeft);
    // 文本超采样下限 2x(与 mac/iOS 同):独立纹理,不改变逻辑尺寸/变换/命中/坐标系。
    setNum("__nativeTextRasterScale", 2.0);
    setNum("__nativeRichTextMetricsVersion", 4.0);   // content/textContent 双盒 + compact paragraph/anchorWidth
    {   // 标记移动端 → fgui/pixi 走触摸分支
        JSStringRef k = JSStringCreateWithUTF8CString("__isMobile");
        JSObjectSetProperty(gCtx, global, k, JSValueMakeBoolean(gCtx, true), kJSPropertyAttributeNone, NULL); JSStringRelease(k);
    }
    {
        JSStringRef key = JSStringCreateWithUTF8CString("__nativePlatform");
        JSStringRef value = JSStringCreateWithUTF8CString("android");
        JSObjectSetProperty(gCtx, global, key, JSValueMakeString(gCtx, value), kJSPropertyAttributeReadOnly, nullptr);
        JSStringRelease(value);
        JSStringRelease(key);
    }

    setFn(gCtx, global, "__flush", cbFlush);
    setFn(gCtx, global, "__loadImage", cbLoadImage);
    setFn(gCtx, global, "__loadImageAsync", cbLoadImageAsync);
    setFn(gCtx, global, "__readFileAsync", cbReadFileAsync);
    setFn(gCtx, global, "__zipParse", cbZipParse);
    setFn(gCtx, global, "__zipGet", cbZipGet);
    setFn(gCtx, global, "__decodeImage", cbDecodeImage);
    setFn(gCtx, global, "__readFile", cbReadFile);
    setFn(gCtx, global, "__evalScript", cbEvalScript);
    setFn(gCtx, global, "__setTimer", cbSetTimer);
    setFn(gCtx, global, "__clearTimer", cbClearTimer);
    setFn(gCtx, global, "__xhrSend", cbXhrSend);
    setFn(gCtx, global, "__showReload", cbShowReload);
    setFn(gCtx, global, "__hideReload", cbHideReload);
    setFn(gCtx, global, "__reload", cbReload);
    setFn(gCtx, global, "__unhandledRejection", cbUnhandledRejection);
    // 引擎的 unhandled rejection 钩子(≙ JSGlobalContextSetUnhandledRejectionCallback;QuickJS 在拒绝当下回调,V8 在微任务 checkpoint 末尾)
    jsengine::setUnhandledRejectionHandler(gCtx, [](JSContextRef c, JSValueRef reason) {
        const std::string text = jsToStd(c, reason);
        // 与 Apple 端 cbUnhandledRejection 对齐:Error 对象带上 stack(backtrace 含 文件:行),否则无法定位业务来源
        std::string stack;
        if (JSValueIsObject(c, reason)) {
            JSStringRef key = JSStringCreateWithUTF8CString("stack");
            JSValueRef sv = JSObjectGetProperty(c, JSValueToObject(c, reason, nullptr), key, nullptr);
            JSStringRelease(key);
            if (sv && JSValueIsString(c, sv)) stack = jsToStd(c, sv);
        }
        ALOGE("[js] [unhandledRejection] %s%s%s", text.empty() ? "?" : text.c_str(), stack.empty() ? "" : "\n", stack.c_str());
        reportRuntimeError("javascript_runtime", "unhandled_rejection", jsErrorDetail(c, reason), false);
    });
    // 读 game_host.js(assets)并求值
    macres::Bytes host = loadAsset("game_host.js");
    if (!host || host->empty()) { ALOGE("[game] game_host.js 缺失"); reportRuntimeError("javascript_boot", "game_host_read_failed", "game_host.js missing", true); return false; }
    {
        OpaqueJSString src{std::string((const char*)host->data(), host->size())};
        JSStringRef url = JSStringCreateWithUTF8CString("game_host.js");
        JSValueRef bootExc = nullptr;
        JSEvaluateScript(gCtx, &src, nullptr, url, 1, &bootExc);
        JSStringRelease(url);
        if (bootExc) { std::string es = jsErrorDetail(gCtx, bootExc); ALOGE("[game] game_host.js 顶层异常: %s", es.c_str()); reportRuntimeError("javascript_boot", "game_host_exception", es, true); return false; }
    }
    jsengine::pumpMicrotasks(gCtx);   // 微任务排空(JSC/V8 在调用返回前自动做,QuickJS 必须手动泵;V8 这里是帧外兜底)
    return true;
}
static void reloadGame() {
    JNIEnv* env = jniEnv();
    if (env && gActivity && gMidHideReload) { env->CallVoidMethod(gActivity, gMidHideReload); if (env->ExceptionCheck()) env->ExceptionClear(); }
    // 停所有 JS timer(其 fn 属于旧上下文,释放前必须清)
    clearAllTimers();
    if (gCtx) { JSGlobalContextRelease(gCtx); gCtx = nullptr; }
    gPendingRoot = 0;
    gNativeBatchSerial = 0;   // 新世代从头计批次(fgui 预约的 reservedAtFrame 不能跨 reload 比较)
    gDec = cmd::Decoder();    // GL 对象 id 映射清零(旧 GL 句柄泄漏可接受)
    gDec.setDefaultFbo(0);
    // 白纹理 GL 句柄跨 reload 存活,但 decoder id 表已清空 → 必须重新登记。
    if (gNativeFguiWhiteTexture) gDec.registerExternalTexture(kNativeFguiWhiteCid, gNativeFguiWhiteTexture);
    gStartUs = nowUs();
    setupGame();
}

// ======================= EGL / JNI 入口 =======================
static bool gBooted = false;
static std::mutex gFrameMutex;   // JNI 入口互斥(Choreographer 线程 vs 生命周期回调)
static bool createEglContextIfNeeded() {
    if (gEglDisplay == EGL_NO_DISPLAY) {
        gEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (gEglDisplay == EGL_NO_DISPLAY || !eglInitialize(gEglDisplay, nullptr, nullptr)) return false;
        const EGLint configAttrs[] = { EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE,EGL_WINDOW_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8,EGL_NONE };
        EGLint count = 0;
        if (!eglChooseConfig(gEglDisplay, configAttrs, &gEglConfig, 1, &count) || count != 1) return false;
    }
    if (gEglContext == EGL_NO_CONTEXT) {
        const EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE };
        gEglContext = eglCreateContext(gEglDisplay, gEglConfig, EGL_NO_CONTEXT, ctxAttrs);
    }
    return gEglContext != EGL_NO_CONTEXT;
}
// 远程 kill 开关是否可达:目录指针经 Firebase Remote Config 键或 HTTPS 指针 URL 下发(≙ host_core.mm nativeFguiRemoteKillReachable:
// Apple 看命名环境 + GoogleService-Info.plist;Android 的等价物就是这两个配置项之一非空)。

static double gWinPointerMs = 0; static int gWinPointerEvents = 0;   // perf 窗口内触摸事件的 JS 耗时(nativeTouch,帧外)
static void notifyJsResume() {
    if (!gCtx) return;
    ShimScope scope(gCtx);
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef name = JSStringCreateWithUTF8CString("__nativeResume");
    JSValueRef value = JSObjectGetProperty(gCtx, global, name, nullptr);
    JSStringRelease(name);
    if (JSValueIsObject(gCtx, value)) {
        JSObjectRef fn = JSValueToObject(gCtx, value, nullptr);
        if (fn && JSObjectIsFunction(gCtx, fn)) JSObjectCallAsFunction(gCtx, fn, nullptr, 0, nullptr, nullptr);
    }
    // 后台驻留后回前台复查内容目录与远程 kill 开关(≙ ios_app.mm sceneWillEnterForeground 同一段):否则 Android 只在启动时
    // 查一次目录;JS 侧 checkOpportunistic 自带 30 分钟节流,焦点抖动不会放大请求量。渲染器控制开关与内容下载互相独立,
    // 关了内容更新的包也要能收到前台 kill。

}
static void notifyJsResize(int width, int height) {
    if (!gCtx || width <= 0 || height <= 0) return;
    ShimScope scope(gCtx);
    JSValueRef args[3] = { JSValueMakeNumber(gCtx, width), JSValueMakeNumber(gCtx, height),
                           JSValueMakeNumber(gCtx, gScreenDPR) };
    callJsGlobal("__resizeEvent", 3, args);
    ALOGI("[game] JS viewport resized to %dx%d dpr=%.1f", width, height, gScreenDPR);
}
static void resetGLStateAfterResume() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_SCISSOR_TEST);
    gDec.invalidateBindings();
    gBatcher.frameReset();
    gShadowBatcher.frameReset();
    gPendingRoot = 0;
}
// 内容热更新配置:Kotlin 在 nativeCreate 之前调用(BuildConfig + versionCode),对应 Apple 的 Info.plist 键
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeConfigure(JNIEnv* env, jobject, jstring query, jstring prelude) {
 gHostQuery=jstringToStd(env,query);gJsPrelude=jstringToStd(env,prelude);
}
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeCreate(
        JNIEnv* env, jobject activity, jobject assets, jstring assetServerUrl, jstring filesPath) {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    gJsThreadTid = gettid();   // nativeCreate 在 UI 线程调用,JS/GL 也在这条线程上跑(Choreographer 驱动)
    gAssets = AAssetManager_fromJava(env, assets);
    env->GetJavaVM(&gVM);
    gActivity = env->NewGlobalRef(activity);
    jclass cls = env->GetObjectClass(activity);
    gMidStartHttp = env->GetMethodID(cls, "startHttp", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)V");
    gMidNativeVibrate = env->GetMethodID(cls, "nativeVibrate", "(Ljava/lang/String;)Z");
    gMidHttpFetchSync = env->GetMethodID(cls, "httpFetchSync", "(Ljava/lang/String;)[B");
    gMidStorageGet = env->GetMethodID(cls, "storageGet", "(Ljava/lang/String;)Ljava/lang/String;");
    gMidStorageSet = env->GetMethodID(cls, "storageSet", "(Ljava/lang/String;Ljava/lang/String;)V");
    gMidStorageRemove = env->GetMethodID(cls, "storageRemove", "(Ljava/lang/String;)V");
    gMidStorageClear = env->GetMethodID(cls, "storageClear", "()V");
    gMidClipboardWrite = env->GetMethodID(cls, "clipboardWrite", "(Ljava/lang/String;)Z");
    gMidOpenUrl = env->GetMethodID(cls, "openUrl", "(Ljava/lang/String;)Z");
    gMidDecodeImageArgb = env->GetMethodID(cls, "decodeImageArgb", "([BLjava/lang/String;)[I");
    const char* S = "Ljava/lang/String;";
    auto sig = [&](const char* pattern) { std::string out; for (const char* p = pattern; *p; p++) { if (*p == 'S') out += S; else out += *p; } return out; };
    gMidC2dCreate = env->GetMethodID(cls, "c2dCreate", "(II)I");
    gMidC2dResize = env->GetMethodID(cls, "c2dResize", "(III)V");
    gMidC2dClearRect = env->GetMethodID(cls, "c2dClearRect", "(IFFFF)V");
    gMidC2dFillRect = env->GetMethodID(cls, "c2dFillRect", sig("(IFFFFSF)V").c_str());
    gMidC2dFillText = env->GetMethodID(cls, "c2dFillText", sig("(ISFFSSSFSSFFF)V").c_str());
    gMidC2dFillTextGrad = env->GetMethodID(cls, "c2dFillTextGrad", sig("(ISFFSFFFFSSFSSFFF)V").c_str());
    gMidC2dStrokeText = env->GetMethodID(cls, "c2dStrokeText", sig("(ISFFSSFSFSSFSFFF)V").c_str());
    gMidC2dMeasure = env->GetMethodID(cls, "c2dMeasure", sig("(ISS)F").c_str());
    gMidC2dGetData = env->GetMethodID(cls, "c2dGetData", "(I)[B");
    gMidC2dPutData = env->GetMethodID(cls, "c2dPutData", "(I[BIIII)V");
    gMidC2dToDataURL = env->GetMethodID(cls, "c2dToDataURL", sig("(ISF)S").c_str());
    gMidC2dSave = env->GetMethodID(cls, "c2dSave", "(I)V");
    gMidC2dRestore = env->GetMethodID(cls, "c2dRestore", "(I)V");
    gMidC2dScale = env->GetMethodID(cls, "c2dScale", "(IFF)V");
    gMidC2dTranslate = env->GetMethodID(cls, "c2dTranslate", "(IFF)V");
    gMidC2dRotate = env->GetMethodID(cls, "c2dRotate", "(IF)V");
    gMidC2dTransform = env->GetMethodID(cls, "c2dTransform", "(IFFFFFF)V");
    gMidC2dSetTransform = env->GetMethodID(cls, "c2dSetTransform", "(IFFFFFF)V");
    gMidC2dResetTransform = env->GetMethodID(cls, "c2dResetTransform", "(I)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); ALOGE("[game] some Kotlin bridge methods missing"); }
    gMidAuLoad = env->GetMethodID(cls, "auLoad", "(I[B)D");
    gMidAuClone = env->GetMethodID(cls, "auClone", "(II)D");
    gMidAuCtl = env->GetMethodID(cls, "auCtl", "(IIDD)D");
    gMidAuShutdown = env->GetMethodID(cls, "auShutdown", "()V");
    gMidKbShow = env->GetMethodID(cls, "kbShow", "(Ljava/lang/String;IZFFFF)V");
    gMidKbHide = env->GetMethodID(cls, "kbHide", "()V");
    gMidKbKeep = env->GetMethodID(cls, "kbKeep", "()V");
    gMidKbSetText = env->GetMethodID(cls, "kbSetText", "(Ljava/lang/String;)V");
    gMidPrompt = env->GetMethodID(cls, "promptShow", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    gMidShowReload = env->GetMethodID(cls, "showReload", "(Ljava/lang/String;Ljava/lang/String;)V");
    gMidHideReload = env->GetMethodID(cls, "hideReload", "()V");
    gMidSnapshot = env->GetMethodID(cls, "snapshotJpegBase64", "(F)Ljava/lang/String;");
    gMidTextMeasure = env->GetMethodID(cls, "textMeasure", "(Ljava/lang/String;[F)Z");
    gMidTextRaster = env->GetMethodID(cls, "textRasterize", "(Ljava/lang/String;IIFFFF[F)Landroid/graphics/Bitmap;");
    gMidRichTokens = env->GetMethodID(cls, "richTokens", "(Ljava/lang/String;)[F");
    gMidRichRaster = env->GetMethodID(cls, "richRasterize", "(Ljava/lang/String;IIF[F)Landroid/graphics/Bitmap;");
    env->DeleteLocalRef(cls);
    if (assetServerUrl) {
        const char* url = env->GetStringUTFChars(assetServerUrl, nullptr);
        if (url) gRemote = url;
        if (url) env->ReleaseStringUTFChars(assetServerUrl, url);
    }
    if (filesPath) {
        const char* path = env->GetStringUTFChars(filesPath, nullptr);
        if (path && path[0]) {

        }
        if (path) env->ReleaseStringUTFChars(filesPath, path);
    }
    gLoadPool.start(4);
    ALOGI("[game] nativeCreate remote=%s", gRemote.c_str());
}
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeSurfaceCreated(
        JNIEnv* env, jobject, jobject surface, jint w, jint h, jint bufferW, jint bufferH, jfloat safeAreaLeftPx) {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    const bool viewportChanged = gScreenW != w || gScreenH != h;
    if (gWindow) { ANativeWindow_release(gWindow); gWindow = nullptr; }
    gWindow = ANativeWindow_fromSurface(env, surface);
    gScreenW = w; gScreenH = h; gScreenDPR = 1;   // 复刻 mac/iOS 工作配置:dpr 固定 1,布局在 native 像素空间
    W = (int)w; H = (int)h;
    gRenderW = bufferW; gRenderH = bufferH;
    gSafeAreaLeft = safeAreaLeftPx;
    if (!createEglContextIfNeeded()) { ALOGE("EGL context 创建失败"); reportRuntimeError("graphics", "egl_initialization_failed", "EGL context creation failed", true); return; }
    if (gEglSurface != EGL_NO_SURFACE) { eglDestroySurface(gEglDisplay, gEglSurface); gEglSurface = EGL_NO_SURFACE; }
    gEglSurface = eglCreateWindowSurface(gEglDisplay, gEglConfig, gWindow, nullptr);
    if (gEglSurface == EGL_NO_SURFACE || !eglMakeCurrent(gEglDisplay, gEglSurface, gEglSurface, gEglContext)) {
        // context 可能已失效(EGL_CONTEXT_LOST):销毁重建一次;JS 世界随之重启(兜底路径)
        ALOGE("EGL makeCurrent 失败,重建 context");
        if (gEglContext != EGL_NO_CONTEXT) { eglDestroyContext(gEglDisplay, gEglContext); gEglContext = EGL_NO_CONTEXT; }
        if (!createEglContextIfNeeded() || gEglSurface == EGL_NO_SURFACE ||
            !eglMakeCurrent(gEglDisplay, gEglSurface, gEglSurface, gEglContext)) { ALOGE("EGL 重建失败"); return; }
        gBatcher = sg::Batcher();
        if (gBooted) { clearAllTimers(); if (gCtx) { JSGlobalContextRelease(gCtx); gCtx = nullptr; } gBooted = false;
            gDec = cmd::Decoder(); gNativeFguiWhiteTexture = 0; gNativeText.clear(); gNativeResources.reset(); }
    }
    eglSwapInterval(gEglDisplay, 1);
    glViewport(0, 0, gRenderW, gRenderH);
    gDec.setDefaultFbo(0);
    gDec.screenW = (float)gScreenW; gDec.screenH = (float)gScreenH;
    ALOGI("[game] surface %dx%d safeLeft=%.0f GL_VERSION=%s", w, h, gSafeAreaLeft, glGetString(GL_VERSION));
    ALOGI("[render-size] layout=%dx%d buffer=%dx%d scale=%.3fx%.3f", w, h, gRenderW, gRenderH,
          (double)gRenderW / w, (double)gRenderH / h);
    {   // 资产合约是预编码 ASTC(KTX2);真机 arm64 普遍支持,模拟器的 GLES 转译层不支持 → 退回 PNG/WebP(JS 侧按 __NATIVE_DISABLE_ASTC 选变体)
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        gAstcSupported = ext && (strstr(ext, "GL_KHR_texture_compression_astc_ldr") || strstr(ext, "GL_OES_texture_compression_astc") || strstr(ext, "GL_KHR_texture_compression_astc_hdr"));
#if defined(PIXINATIVE_DISABLE_ASTC)
        gAstcSupported = false;
        ALOGI("[texture-test] ASTC forced disabled by build flag; using PNG/WebP");
#endif
        ALOGI("[game] ASTC %s", gAstcSupported ? "supported" : "NOT supported: JS 走 PNG/WebP 变体,ktx2 候选跳过");
        ALOGI("[game] GL_EXT_buffer_storage %s", ext && strstr(ext, "GL_EXT_buffer_storage") ? "supported" : "not supported");
    }
    if (!gBooted) {
        gStartUs = nowUs();
        gBooted = setupGame();
        if (!gBooted) { ALOGE("[game] setupGame 失败"); reportRuntimeError("runtime", "initial_setup_failed", "setupGame failed", true); }
    } else {
        // 与 iOS appDidBecomeActive 同语义:真实 GL 状态已变化,废弃缓存的绑定并通知 JS。
        resetGLStateAfterResume();
        if (viewportChanged) notifyJsResize(w, h);
        notifyJsResume();
    }
}
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeSurfaceChanged(JNIEnv*, jobject, jint w, jint h, jint bufferW, jint bufferH) {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    const bool viewportChanged = gScreenW != w || gScreenH != h;
    const bool bufferChanged = gRenderW != bufferW || gRenderH != bufferH;
    gScreenW = w; gScreenH = h; W = (int)w; H = (int)h;
    gRenderW = bufferW; gRenderH = bufferH;
    glViewport(0, 0, gRenderW, gRenderH);
    gDec.screenW = (float)gScreenW; gDec.screenH = (float)gScreenH;
    if (viewportChanged) notifyJsResize(w, h);
    if (viewportChanged || bufferChanged)
        ALOGI("[render-size] layout=%dx%d buffer=%dx%d scale=%.3fx%.3f", w, h, gRenderW, gRenderH,
              (double)gRenderW / w, (double)gRenderH / h);
}
// 返回值:本帧是否真正上屏(eglSwapBuffers)。Kotlin 侧据此在首帧后放行系统 SplashScreen(此前 dismissSplash 无人调用,logo 永远盖住 GL 面)。
extern "C" JNIEXPORT jboolean JNICALL Java_org_pixinative_runtime_MainActivity_nativeDrawFrame(JNIEnv*, jobject, jlong) {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    if (gEglDisplay == EGL_NO_DISPLAY || gEglSurface == EGL_NO_SURFACE || !gCtx) return JNI_FALSE;
    double t0 = nowUs();
    gFramePreJSUs = 0;
    ShimScope scope(gCtx);   // 本帧 shim 值统一在帧尾释放
    double unityWaitMs = 0;
    // 主线程队列/HTTP 结果/timer 帧头泵(iOS 由 main runloop 白送,Android 显式做)
    gJsPhase = "queue"; gMainQ.drain();
    double queueEnd = nowUs();
    gJsPhase = "http"; pumpHttpResults();
    double httpEnd = nowUs();
    gJsPhase = "timer"; pumpTimers();
    double pumpEnd = nowUs();
    if (!gCtx) return JNI_FALSE;   // 泵里可能触发了 reload 失败
    if (!gBatcher.ready()) {
        gBatcher.texLookup = [](int id){ return gDec.glTexture((uint32_t)id); };
        gBatcher.init();
    }
    gDec.batcher = &gBatcher; gDec.sgraph = &gSG;
    gDec.screenW = (float)gScreenW; gDec.screenH = (float)gScreenH;
    gBatcher.quads = 0; gBatcher.escapes = 0; gBatcher.drawCalls = 0;
    gBatcher.frameReset();   // 顶点 VBO 环形写入每帧复位
    gJsPhase = "tick"; drawOneFrame();
    double drawEnd = nowUs();
    // 微任务泵(Promise 续体;JSC/V8 在最外层调用返回时自动,QuickJS 必须显式;帧尾统一再排空一次)
    gJsPhase = "jobs";
    jsengine::pumpMicrotasks(gCtx);
    gJsPhase = "other";
    double jobsEnd = nowUs();
    if (gJsProf.enabled) {
        jsProfPhaseAdd("queue", (queueEnd - t0) / 1000.0); jsProfPhaseAdd("http", (httpEnd - queueEnd) / 1000.0);
        jsProfPhaseAdd("timer", (pumpEnd - httpEnd) / 1000.0); jsProfPhaseAdd("tick", (drawEnd - pumpEnd) / 1000.0);
        jsProfPhaseAdd("jobs", (jobsEnd - drawEnd) / 1000.0);
    }
    if (gJsProf.enabled && t0 - gJsProf.lastDumpUs > 30000000.0) { gJsProf.lastDumpUs = t0; jsProfDump("30s"); }
    // 坏帧快照:>25ms 打明细,限流 0.5s 一条
    double busy = (nowUs()-t0)/1000.0;
    // ---- QuickJS 环垃圾 GC 调度(自动 GC 阈值已抬到 256 MiB 只做兜底,见 jsc_quickjs_shim.h;V8 分代/增量 GC 自管,不走这段)----
    // 引用计数已即时回收非环垃圾,环垃圾可以攒着挑时机清:
    //   a) 重帧突发(场景加载)结束后的第一个轻帧——用户本来就在看加载/进场,停顿被掩盖,且刚销毁的场景正是环垃圾大户;
    //   b) 距上次 ≥60s 的轻帧兜底;c) 退后台时(nativeSurfaceDestroyed)白送。
    // __NATIVE_GC_POLICY=auto(jsPrelude 注入)恢复 QuickJS 默认自动阈值做 A/B。每次 GC 计时并入 perf 窗口汇总。
    static double gLastGcUs = 0, gLastHeavyUs = 0; static double gHeavyBurstMs = 0;
    static double winGcMs = 0, winGcMax = 0; static int winGcCount = 0;
    static int gGcPolicy = -1;   // -1 未读,0 auto/引擎自管,1 host
    if (gGcPolicy < 0) {
        gGcPolicy = jsengine::kHostSchedulesGC ? 1 : 0;
        JSObjectRef global = JSContextGetGlobalObject(gCtx);
        JSStringRef key = JSStringCreateWithUTF8CString("__NATIVE_GC_POLICY");
        JSValueRef v = JSObjectGetProperty(gCtx, global, key, nullptr); JSStringRelease(key);
        if (!jsengine::kHostSchedulesGC) ALOGI("[gc] policy=engine(%s 自管:分代/增量,退后台时全量)", jsengine::kName);
        else if (v && JSValueIsString(gCtx, v) && jsToStd(gCtx, v) == "auto") { gGcPolicy = 0; jsengine::setAutoGcThreshold(gCtx); ALOGI("[gc] policy=auto(QuickJS 默认阈值)"); }
        else ALOGI("[gc] policy=host(重帧突发后/60s 兜底/退后台)");
    }
    if (gGcPolicy == 1) {
        if (gLastGcUs <= 0) gLastGcUs = t0;
        const double now = nowUs();
        if (busy > 25.0) { gLastHeavyUs = now; gHeavyBurstMs += busy; }
        else if (gLastHeavyUs > 0 && now - gLastHeavyUs > 2000000.0) gHeavyBurstMs = 0;   // 2s 没有重帧:突发结束,清累计
        const double sinceGcMs = (now - gLastGcUs) / 1000.0;
        const double sinceHeavyMs = gLastHeavyUs > 0 ? (now - gLastHeavyUs) / 1000.0 : 1e9;
        const bool afterBurst = gHeavyBurstMs >= 200.0 && sinceHeavyMs >= 300.0 && sinceHeavyMs < 2000.0 && busy < 8.0 && sinceGcMs >= 15000.0;
        const bool periodic = sinceGcMs >= 60000.0 && busy < 8.0;
        const bool overdue = sinceGcMs >= 120000.0;
        if (afterBurst || periodic || overdue) {
            const double g0 = nowUs();
            jsengine::collectGarbage(gCtx);
            const double gcMs = (nowUs() - g0) / 1000.0;
            gLastGcUs = nowUs(); gHeavyBurstMs = 0;
            winGcMs += gcMs; winGcCount++; if (gcMs > winGcMax) winGcMax = gcMs;
            ALOGI("[gc] %.1fms 触发=%s 距上次 %.1fs 本帧忙 %.1fms", gcMs, afterBurst ? "重帧突发后" : (periodic ? "60s兜底" : "120s强制"), sinceGcMs / 1000.0, busy);
            busy = (nowUs()-t0)/1000.0;
        }
    }
    static double lastBadUs = 0;
    if (busy > 25.0 && t0 - lastBadUs > 500000) {
        lastBadUs = t0;
        if(gBatcher.profilePost && gBatcher.postProfile.filters){
            const auto& p=gBatcher.postProfile;
            ALOGI("[post-prof] filters=%u passes=%u max=%ux%u total=%.2f capture=%.2f prepare=%.2f draw=%.2f composite=%.2fms",
                  p.filters,p.passes,p.maxW,p.maxH,p.totalMs,p.captureMs,p.prepareMs,p.drawMs,p.compositeMs);
        }
        ALOGE("[badframe] 忙%.1fms JS%.1f unity%.1f pump%.1f{q%.1f h%.1f t%.1f due%zu/%zu slow#%d=%.1f} pre%.1f jobs%.1f compute%.1f batch%.1f{bounds%.1f walk%.1f bdata%.1f bsub%.1f drawMax%.1f up%u imm%u(st%u post%u) rec%u fb%u} sync%.1f present%.1f other%.1f draw%u tex%u(缓%u 队%zu) buf%u",
              busy, gFrameJSUs/1000.0, unityWaitMs, (pumpEnd-t0)/1000.0,
              (queueEnd-t0)/1000.0, (httpEnd-queueEnd)/1000.0, (pumpEnd-httpEnd)/1000.0,
              gLastTimerDueCount, gTimers.size(), gSlowTimerId, gSlowTimerMs, gFramePreJSUs/1000.0,
              (jobsEnd-drawEnd)/1000.0, gFrameComputeUs/1000.0, gFrameBatchUs/1000.0,
              gBatcher.boundsMs, gBatcher.walkMs, gBatcher.bufferDataMs, gBatcher.bufferSubMs, gBatcher.drawMs,
              gBatcher.uploadCalls, gBatcher.immediateBatches, gBatcher.stencilBatches,
              gBatcher.postBatches, gBatcher.recordedBatches, gBatcher.mapFallbacks,
              gBatcher.syncMs,
              gFramePresentUs/1000.0,
              std::max(0.0, busy-(gFrameJSUs+gFrameComputeUs+gFramePresentUs)/1000.0),
              (unsigned)gDec.drawCalls, (unsigned)gDec.texUploads,
              (unsigned)gDec.texDeferred, gDec.texPending(), (unsigned)gDec.bufUploads);
        if (gCtx) { JSValueRef a = JSValueMakeNumber(gCtx, busy); callJsGlobal("__badFrame", 1, &a); }
    }
    // perf 窗口(~2s):FPS 按实际上屏计
    static double winStart = 0; static int winFrames = 0, winPresents = 0, winIdle = 0; static double winBusy = 0, winUnityWait = 0;
    winFrames++; winBusy += busy; winUnityWait += unityWaitMs;
    if (gFramePresented) winPresents++; else if (!gFrameHadContent) winIdle++;
    if (winStart <= 0) winStart = t0;
    double elapsed = (nowUs()-winStart)/1000.0;
    if (elapsed >= 2000.0) {
        const uint64_t syncUs = gJsThreadSyncFetchUs.exchange(0);
        const uint32_t syncCount = gJsThreadSyncFetchCount.exchange(0);
        char extra[224] = "";
        int n = 0;
        if (winGcCount) n += snprintf(extra + n, sizeof(extra) - n, " GC%d次 %.0fms(max %.0f)", winGcCount, winGcMs, winGcMax);
        if (syncCount) n += snprintf(extra + n, sizeof(extra) - n, " 同步取资源阻塞JS %u次 %.0fms", syncCount, syncUs/1000.0);
        if (winUnityWait > 0) n += snprintf(extra + n, sizeof(extra) - n, " 建Unity栅栏 %.1fms/帧", winUnityWait / winFrames);
        if (gWinPointerEvents) { snprintf(extra + n, sizeof(extra) - n, " 触摸 %d次 %.0fms", gWinPointerEvents, gWinPointerMs); gWinPointerMs = 0; gWinPointerEvents = 0; }
        ALOGI("[perf] FPS %.0f(tick%.0f 静%d) 忙%.1fms%s", winPresents*1000.0/elapsed, winFrames*1000.0/elapsed, winIdle, winBusy/winFrames, extra);
        winStart = nowUs(); winFrames = 0; winPresents = 0; winIdle = 0; winBusy = 0; winUnityWait = 0; winGcMs = 0; winGcMax = 0; winGcCount = 0;
    }
    return gFramePresented ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeSurfaceDestroyed(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    // 只销毁 surface,保留 context/JS 世界(回前台经 nativeSurfaceCreated 恢复,iOS 后台语义)。
    if (gEglDisplay != EGL_NO_DISPLAY) {
        glFinish();   // 排空 GPU 队列
        eglMakeCurrent(gEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gEglSurface != EGL_NO_SURFACE) { eglDestroySurface(gEglDisplay, gEglSurface); gEglSurface = EGL_NO_SURFACE; }
    }
    if (gWindow) { ANativeWindow_release(gWindow); gWindow = nullptr; }
    // 退后台:JS 世界保留但不再出帧,这时清环垃圾对用户免费(≙ 帧尾 GC 调度的 c 条),回前台少一次 100ms+ 停顿。
    if (gCtx) {
        const double g0 = nowUs();
        { ShimScope scope(gCtx); jsengine::collectGarbage(gCtx); }
        ALOGI("[gc] %.1fms 触发=退后台", (nowUs() - g0) / 1000.0);
    }
}
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeDestroy(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    gHostEpoch.fetch_add(1);
    clearAllTimers();
    if (gCtx) { JSGlobalContextRelease(gCtx); gCtx = nullptr; }
    gBooted = false;
    if (gEglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(gEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gEglSurface != EGL_NO_SURFACE) eglDestroySurface(gEglDisplay, gEglSurface);
        if (gEglContext != EGL_NO_CONTEXT) eglDestroyContext(gEglDisplay, gEglContext);
        eglTerminate(gEglDisplay);
    }
    gEglSurface = EGL_NO_SURFACE; gEglContext = EGL_NO_CONTEXT; gEglDisplay = EGL_NO_DISPLAY;
    gBatcher = sg::Batcher();   // EGL context owns the mapped VBO/fences; never reuse them after teardown.
    if (gWindow) { ANativeWindow_release(gWindow); gWindow = nullptr; }
    gAssets = nullptr;
    if (gActivity) { env->DeleteGlobalRef(gActivity); gActivity = nullptr; }
    gLoadPool.shutdown();
    gMainQ.clear();
}
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeHttpComplete(
        JNIEnv* env, jobject, jint id, jint status, jstring text, jstring headers, jbyteArray body) {
    auto str = [&](jstring s) {
        if (!s) return std::string();
        const char* p = env->GetStringUTFChars(s, nullptr);
        std::string out(p ? p : "");
        if (p) env->ReleaseStringUTFChars(s, p);
        return out;
    };
    HttpResult result{(int)id, (int)status, str(text), str(headers), nullptr};
    if (body) {
        const jsize n = env->GetArrayLength(body);
        result.body = std::make_shared<std::vector<uint8_t>>((size_t)n);
        if (n) env->GetByteArrayRegion(body, 0, n, reinterpret_cast<jbyte*>(result.body->data()));
    }
    std::lock_guard<std::mutex> lock(gHttpMutex);
    gHttpResults.push_back(std::move(result));
}

extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeAudioEnded(JNIEnv*, jobject, jint aid) {
    int id = (int)aid;
    // 按世代过滤:回调在帧头才派发,reload 前 posted、reload 后才被泵到的 ended 属于旧世界,新世界同号元素不能收到它。
    const uint64_t epoch = gHostEpoch.load();
    gMainQ.post([id, epoch] {
        if (!gCtx || epoch != gHostEpoch.load()) return;
        JSValueRef arg = JSValueMakeNumber(gCtx, id);
        callJsGlobal("__auEnded", 1, &arg);
    });
}
// composing:输入法组字中(≙ iOS kbChanged 的 markedTextRange != nil),JS 据此派发 compositionstart/compositionend。
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeKbInput(JNIEnv* env, jobject, jstring text, jboolean composing) {
    if (!text) return;
    const char* value = env->GetStringUTFChars(text, nullptr);
    std::string message(value ? value : "");
    if (value) env->ReleaseStringUTFChars(text, value);
    const bool isComposing = composing == JNI_TRUE;
    gMainQ.post([message, isComposing] {
        if (!gCtx) return;
        JSStringRef s = JSStringCreateWithUTF8CString(message.c_str());
        JSValueRef args[2] = { JSValueMakeString(gCtx, s), JSValueMakeBoolean(gCtx, isComposing) };
        callJsGlobal("__kbInput", 2, args);
        JSStringRelease(s);
    });
}
// isReturn:回车提交 → JS 派发 keydown/keyup(13)再 blur;结束编辑(收起/失焦)→ 只 blur(≙ iOS __kbDone(1)/__kbDone(0))。
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeKbDone(JNIEnv*, jobject, jboolean isReturn) {
    const bool ret = isReturn == JNI_TRUE;
    gMainQ.post([ret] {
        if (!gCtx) return;
        JSValueRef arg = JSValueMakeBoolean(gCtx, ret);
        callJsGlobal("__kbDone", 1, &arg);
    });
}
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativePromptDone(JNIEnv* env, jobject, jint token, jstring text) {
    bool hasText = text != nullptr;
    std::string value;
    if (text) {
        const char* p = env->GetStringUTFChars(text, nullptr);
        value = p ? p : "";
        if (p) env->ReleaseStringUTFChars(text, p);
    }
    int t = (int)token;
    gMainQ.post([t, hasText, value] { finishPrompt(t, hasText, value); });
}
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeReloadRequested(JNIEnv*, jobject) {
    gMainQ.post([] { reloadGame(); });
}
static std::unordered_set<int> gActiveTouchPointers;
static int gPrimaryTouchPointer = -1;
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeSetVisibility(
        JNIEnv*, jobject, jboolean visible) {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    if (!gCtx) return;
    ShimScope scope(gCtx);
    JSValueRef value=JSValueMakeBoolean(gCtx,visible==JNI_TRUE);
    callJsGlobal("__nativeSetVisibility",1,&value);
    if (!visible) { gActiveTouchPointers.clear(); gPrimaryTouchPointer=-1; }
}
extern "C" JNIEXPORT void JNICALL Java_org_pixinative_runtime_MainActivity_nativeTouch(
        JNIEnv*, jobject, jint action, jint pointerId, jfloat x, jfloat y, jlong timeMs) {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    if (!gCtx) return;
    const char* type = (action == 0 || action == 5) ? "pointerdown"
                     : (action == 1 || action == 6) ? "pointerup"
                     : action == 2 ? "pointermove"
                     : action == 3 ? "pointercancel" : nullptr;
    if (!type) return;
    const bool down = action == 0 || action == 5 || action == 2;
    if (action == 0) { gActiveTouchPointers.clear(); gPrimaryTouchPointer = pointerId; }
    if (action == 0 || action == 5) gActiveTouchPointers.insert(pointerId);
    const bool primary = pointerId == gPrimaryTouchPointer;
    ShimScope scope(gCtx);
    JSStringRef ts = JSStringCreateWithUTF8CString(type);
    JSStringRef touchType = JSStringCreateWithUTF8CString("touch");
    JSValueRef args[8] = { JSValueMakeString(gCtx, ts), JSValueMakeNumber(gCtx, x), JSValueMakeNumber(gCtx, y),
                           JSValueMakeNumber(gCtx, 0), JSValueMakeNumber(gCtx, down ? 1 : 0),
                           JSValueMakeNumber(gCtx, pointerId + 2), JSValueMakeString(gCtx, touchType), JSValueMakeBoolean(gCtx, primary) };
    JSStringRelease(ts);
    JSStringRelease(touchType);
    const double p0 = nowUs();
    gJsPhase = "pointer"; callJsGlobal("__pointerEvent", 8, args); gJsPhase = "other";
    if (action == 1 || action == 6 || action == 3) { gActiveTouchPointers.erase(pointerId); if (gActiveTouchPointers.empty()) gPrimaryTouchPointer = -1; }
    const double pointerMs = (nowUs() - p0) / 1000.0;
    jsProfPhaseAdd("pointer", pointerMs);
    gWinPointerMs += pointerMs; gWinPointerEvents++;   // 帧外的主线程时间,perf 行单列(连点时它挤掉帧回调)
}
