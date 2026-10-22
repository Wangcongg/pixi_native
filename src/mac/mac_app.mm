#include "../apple/native_http.h"
// game_window —— 让真实 Pixi Native（pixi v7.4.3 商业老虎机）原样跑在我们运行时并上屏。
// 复用 s5 的 ANGLE/Metal + JSC + 命令缓冲外壳；入口改为 eval game_host.js（内部用 __evalScript 自加载
// pixi/spine/游戏 bundle）。新增 host 接口：__evalScript（加载并全局执行 JS）、__setTimer/__clearTimer
// （JSC 无原生 timer，游戏重度依赖）；__readFile/__loadImage 支持远程拉取（loading 资源在公网 CDN）。
// 每帧 vsync 调 JS __tick(tMs) 驱动 pixi ticker，提交 native 渲染树后 eglSwapBuffers 上屏。
// 用法: cd build && ./game_window         （窗口 1280x720，与 H5 QA 基准一致）
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <JavaScriptCore/JavaScriptCore.h>
#import <AVFoundation/AVFoundation.h>
#import <ImageIO/ImageIO.h>

// The Swift bridge is linked only into PixiNativeMac. Resolve this small Objective-C surface
// dynamically so the bare game_window developer target can continue to build without Swift.


#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <cstdio>
#include <array>
#include <cmath>
#include <vector>
#include <string>
#include <cstring>
#include <strings.h>
#include <climits>
#include <limits>
#include <chrono>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <zlib.h>
#include "../jsc_rooted_value_vector.h"
#include "../common/native_diag_log.h"   // 排查态日志编译期开关(默认关,-DPIXINATIVE_DIAG_LOG=1 打开)
#include "../common/native_script_utf8.h"
#include "../cmdbuf/decoder.h"
#include "../cmdbuf/scenegraph.h"
#include "../cmdbuf/native_animation_frame.h"
#include "../cmdbuf/batcher.h"
#include "../cmdbuf/native_fgui.h"
#include "../cmdbuf/spinex.h"
#include "../cmdbuf/plain_text_layout_contract.h"
#include "../cmdbuf/richtext_layout_contract.h"
#include "../cmdbuf/richtext_token_spacing.h"
#include "../cmdbuf/richtext_token_layout.h"
#include "native_resource_manager.h"
#include "../common/native_environment.h"
#include "../common/sha256.h"
#include "canvas2d_jsc.h"
#include "persistent_log.h"
#include "live_log_viewer.h"
#include "../host/host_core.h"   // Apple 两端共享的宿主核心(状态/回调/桥)

// ---- host_core 需要的平台颜色/PNG 接口(NativeTextSystem 用;mac 走 AppKit) ----
id hostPlatformColor(CGFloat r, CGFloat g, CGFloat b, CGFloat a) { return [NSColor colorWithSRGBRed:r green:g blue:b alpha:a]; }
id hostPlatformClearColor() { return NSColor.clearColor; }
bool hostPlatformWritePNG(CGImageRef image, NSString* path) {
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:image];
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return [png writeToFile:path atomically:NO];
}

#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#endif
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif

static EGLDisplay gDisplay = EGL_NO_DISPLAY;
static EGLSurface gSurface = EGL_NO_SURFACE;
static EGLContext gContext = EGL_NO_CONTEXT;
static EGLConfig gConfig = nullptr;
static CALayer* gRenderLayer = nil;
static bool gSurfaceNeedsRestore = false;
struct NativeAtlasTracePending { int cid=0,attempts=0;float reportedWidth=0,reportedHeight=0;std::string packageName,atlasId;bool dumped=false; };
static std::vector<NativeAtlasTracePending> gNativeAtlasTracePending;
static GLuint gNativeFguiWhiteTexture = 0;
static const uint32_t kNativeFguiWhiteCid = 7999999;
static sg::Batcher gBatcher;
static sg::Batcher gShadowBatcher;
static GLuint gShadowFbo = 0, gShadowTex = 0;
static int gShadowFboW = 0, gShadowFboH = 0;
static int gShadowRoot = 0;
static int gShadowShow = 0;             // 0=离屏 only,1=blit 目视;production 主屏不再复用该状态
static bool gCanPresent = true;         // 本帧窗口可见性(AppKit occlusion),present 内联绘制的闸门
static bool gNativeScreenPresented = false; // 本帧 __nativePresent 已直接画入主屏
static void flipMainScreenDraw(int root);   // mode 2 主屏绘制;定义在 renderShadow 之前
static uint32_t* gBuf = nullptr;
static const size_t kBufWords = 1u << 24; // 64MB，需容纳 4096 图集单条上传命令
// AppKit/Pixi layout stays in logical points.  The default framebuffer is sized independently in
// backing pixels so Retina screens gain density without changing layout or pointer coordinates.
int W = 1280, H = 720;             // logical viewport; GAME_W/GAME_H override points
static int gDrawableW = 1280, gDrawableH = 720;
static double gBackingScale = 1.0;
static uint64_t gRuntimeEpoch = 0;
static std::atomic<bool> gHostShuttingDown{false};
static NSWindow* gMainWindow = nil;
enum class GameCursorKind { Arrow, PointingHand, IBeam, OpenHand, ClosedHand, Crosshair, Custom };
static GameCursorKind gGameCursorKind = GameCursorKind::Arrow;
static NSCursor* gCustomCursor = nil;   // Custom 态的 CSS url() 光标(实例缓存在 cursorFromCssUrl)
static NSCursor* gameCursor() {
    switch (gGameCursorKind) {
        case GameCursorKind::PointingHand: return [NSCursor pointingHandCursor];
        case GameCursorKind::IBeam: return [NSCursor IBeamCursor];
        case GameCursorKind::OpenHand: return [NSCursor openHandCursor];
        case GameCursorKind::ClosedHand: return [NSCursor closedHandCursor];
        case GameCursorKind::Crosshair: return [NSCursor crosshairCursor];
        case GameCursorKind::Custom: if (gCustomCursor) return gCustomCursor; return [NSCursor arrowCursor];
        default: return [NSCursor arrowCursor];
    }
}
static double gFrameDecodeUs = 0;
struct FrameTiming {
    double uploadUs=0, tickUs=0, decodeUs=0, inlinePresentUs=0, computeUs=0, batchUs=0, shadowUs=0;
    double shadowSetupUs=0, shadowComputeUs=0, shadowBatchUs=0, shadowFinishUs=0;
    double shadowBindUs=0, preClearFinishUs=0, shadowClearUs=0;
    double renderUs=0, submitUs=0, swapUs=0;
};
static FrameTiming gFrameTiming;
#if !defined(PIXINATIVE_PRODUCTION)
// Opt-in diagnostic only: serialise previously submitted GPU work before the default-FBO clear.
// If a ~1s wait moves here, the GPU queue was behind; if glFinish stays fast and glClear still
// waits, ANGLE is blocked acquiring a CAMetalLayer drawable. Production compiles the branch out.
static bool gTestGlClearProbe = false;
#endif


static void dumpBackbuffer(const char* path);
static bool gDumpDone = false;   // GAME_DUMP 截图已落盘(退出闸门等它,防启动跳帧下"先退出后 dump")

static const char* gGameDir   = "../game";                        // 打过补丁的 bundle（pixi/game_host/dev.9e2ee4 等，最高优先）
static const char* gAssetsDir = "../assets";                      // 本地完整资源镜像（dev1 web 根的离线副本）
#ifndef PIXINATIVE_ASSET_BASE_URL
#define PIXINATIVE_ASSET_BASE_URL ""
#endif
// Background machine/content/audio fetches may still be using the resolved asset paths while a
// normal AppKit Quit is tearing down the UI runtime.  These are process configuration singletons;
// keep their storage alive until kernel process cleanup instead of racing C++ static destruction.
static std::string gRemote __attribute__((no_destroy)) = PIXINATIVE_ASSET_BASE_URL;
static std::string gBundledGameDir __attribute__((no_destroy));


// Sandbox/Prod 是用户验收和正式发行环境。即使 Xcode Scheme、缓存的 Build Settings
// 或命令行误传了开发机地址，也必须使用真实公网 CDN，并且禁止挂载 2.9G 开发镜像。
// 环境判定 env → Info.plist 两级：Scheme 变量只在 Xcode 点运行时存在，Archive/Finder
// 启动的发布包全靠 CONTENT_ENVIRONMENT 写进 plist 的值。


static NSString* pathUnderRoot(const char* root, NSString* relative) {
    if (!root || !relative) return nil;
    NSString* base = [[NSFileManager defaultManager]
        stringWithFileSystemRepresentation:root length:std::strlen(root)];
    return [base stringByAppendingPathComponent:relative];
}


// 本地查找：先 gGameDir/<rel>（补丁优先），再 gAssetsDir/<rel>（完整镜像）。命中返回字节，否则 nil。
static NSData* localAssetData(NSString* rel) { return loadAsset(rel.UTF8String); }


// 宿主的 60Hz frame timer 负责帧率节拍，EGL swap 不再额外等下一个 vsync。
static void configureSwapInterval(){
    EGLint minInterval=0,maxInterval=0;
    eglGetConfigAttrib(gDisplay,gConfig,EGL_MIN_SWAP_INTERVAL,&minInterval);
    eglGetConfigAttrib(gDisplay,gConfig,EGL_MAX_SWAP_INTERVAL,&maxInterval);
    EGLint interval=minInterval<=0?0:minInterval;
    EGLBoolean ok=eglSwapInterval(gDisplay,interval);
    fprintf(stderr,"[game] EGL swap interval=%d (range %d..%d, %s)\n",interval,minInterval,maxInterval,ok?"ok":"failed");
}

// 拒绝 HTTPS→HTTP 降级重定向：ATS 为局域网 dev 资产全局放开，而 content/遥测请求可能携带
// installationId/构建版本/release——若服务器 307/308 到 http，body 会被明文重发。守卫在重定向
// 发生前拦截；初始就是 http 的 dev 资产请求不受影响（只禁降级，不禁 http 本身）。
@interface NativeContentRedirectGuard : NSObject <NSURLSessionTaskDelegate>
@end
@implementation NativeContentRedirectGuard
- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
willPerformHTTPRedirection:(NSHTTPURLResponse*)response
        newRequest:(NSURLRequest*)request
 completionHandler:(void (^)(NSURLRequest*))completionHandler {
    const BOOL originalSecure =
        [[task.originalRequest.URL.scheme lowercaseString] isEqualToString:@"https"];
    const BOOL nextSecure = [[request.URL.scheme lowercaseString] isEqualToString:@"https"];
    // Refusing (nil) finishes the task with the 3xx response itself, which every caller already
    // treats as a failure (only status 200 is accepted / 2xx acknowledged).
    completionHandler((!originalSecure || nextSecure) ? request : nil);
}
@end

// 远程拉取（有界阻塞）：NSURLSession + 空闲/总计双超时，仅接受 HTTP 200。
// 旧实现 dataWithContentsOfURL 无超时且在主线程（JSC 回调里），dev 服务器挂起时整个 app 卡死只能强杀；
// 404 错误页也会被当资产写进本地缓存。completion 在 session 后台队列执行，主线程 semaphore 等待无死锁。
// finalURL（可选出参）返回重定向后的最终 URL：CDN 可能 302，content 更新路径必须复检 scheme 仍是 https。


// Foreground fallback is synchronous on the JSC thread, so retry within one bounded 24s budget:
// preserve a useful 16s first attempt, then give transient network/429/5xx failures one short
// recovery attempt. Deterministic misses such as 404 enter the existing negative cache directly.


// Machine packages are explicitly background work and may contain multi-MiB Spine/audio files.
// Give each attempt a wider weak-network budget; JS owns retry and global fairness above this
// single-attempt transport boundary.


// 取资源字节：先本地 gGameDir/<rel>，没有则远程 gRemote/<rel> 兜底。
// 远程响应只供本次运行使用，不再写回 game/。game/ 是显式补丁目录，把下载内容写进去会让
// 旧资源跨启动覆盖 assets/ 镜像，开发时即使服务器资源已经更新也仍会看到旧版本。
// data: URL 暂不支持（返回 nil）。
// 失败负缓存：同一 rel 完成一组有界尝试后，本会话不再远程拉取，避免反复堵主线程。
NSData* loadAsset(const char* path) {
 NSString* requested=[NSString stringWithUTF8String:path ?: ""];
 if([requested hasPrefix:@"https://"]||[requested hasPrefix:@"http://"])return nativeFetchResource(requested);
 NSString* rel=relOf([NSString stringWithUTF8String:path ?: ""]);
 if (!rel.length || [rel.pathComponents containsObject:@".."]) return nil;
 return [NSData dataWithContentsOfFile:[[NSString stringWithUTF8String:gGameDir] stringByAppendingPathComponent:rel]];
}

static bool initEGL(CALayer* layer) {
    gRenderLayer = layer;
    EGLint da[]={EGL_PLATFORM_ANGLE_TYPE_ANGLE,EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,EGL_NONE};
    auto getDisp=(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    gDisplay=getDisp(EGL_PLATFORM_ANGLE_ANGLE,(void*)EGL_DEFAULT_DISPLAY,da);
    EGLint mj,mn; eglInitialize(gDisplay,&mj,&mn);
    fprintf(stderr,"[game] EGL %d.%d %s\n",mj,mn,eglQueryString(gDisplay,EGL_VENDOR));
    EGLint ca[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,
                 EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,
                 EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8,EGL_NONE};
    EGLint num; eglChooseConfig(gDisplay,ca,&gConfig,1,&num);
    gSurface = eglCreateWindowSurface(gDisplay,gConfig,(__bridge EGLNativeWindowType)layer, nullptr);
    EGLint cx[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    gContext=eglCreateContext(gDisplay,gConfig,EGL_NO_CONTEXT,cx);
    if (!eglMakeCurrent(gDisplay,gSurface,gSurface,gContext)) {
        fprintf(stderr, "[game] eglMakeCurrent 失败 err=0x%x\n", eglGetError());
        return false;
    }
    configureSwapInterval();
    fprintf(stderr,"[game] GL_VERSION=%s\n",glGetString(GL_VERSION));
    return gSurface!=EGL_NO_SURFACE && gContext!=EGL_NO_CONTEXT;
}

// 保留 EGLContext（程序/纹理/缓冲都还在），仅恢复 CAMetalLayer 对应的 window surface。
// 普通 Cmd+Tab 只需重新 makeCurrent；若 ANGLE 报 surface 已失效，再原位重建 surface。
static bool restoreEGLSurface() {
    if (gDisplay == EGL_NO_DISPLAY || gContext == EGL_NO_CONTEXT || !gRenderLayer) return false;
    auto makeCurrentAndCheck = []() -> bool {
        if (gSurface == EGL_NO_SURFACE || !eglMakeCurrent(gDisplay, gSurface, gSurface, gContext)) return false;
        EGLint w = 0, h = 0;
        if (!eglQuerySurface(gDisplay, gSurface, EGL_WIDTH, &w)
            || !eglQuerySurface(gDisplay, gSurface, EGL_HEIGHT, &h) || w <= 0 || h <= 0) return false;
        configureSwapInterval();
        fprintf(stderr, "[lifecycle] EGL surface=%dx%d current=ok\n", w, h);
        return true;
    };
    if (makeCurrentAndCheck()) return true;

    EGLint firstErr = eglGetError();
    fprintf(stderr, "[lifecycle] EGL surface 失效 err=0x%x，保留 context 重建 surface\n", firstErr);
    eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (gSurface != EGL_NO_SURFACE) eglDestroySurface(gDisplay, gSurface);
    gSurface = eglCreateWindowSurface(gDisplay, gConfig,
        (__bridge EGLNativeWindowType)gRenderLayer, nullptr);
    if (!makeCurrentAndCheck()) {
        fprintf(stderr, "[lifecycle] EGL surface 重建失败 err=0x%x\n", eglGetError());
        return false;
    }
    return true;
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
    gShadowRoot = 0;
}

static void notifyJsResume() {
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef name = JSStringCreateWithUTF8CString("__nativeResume");
    JSValueRef value = JSObjectGetProperty(gCtx, global, name, nullptr);
    JSStringRelease(name);
    if (!JSValueIsObject(gCtx, value)) return;
    JSObjectRef fn = JSValueToObject(gCtx, value, nullptr);
    if (fn && JSObjectIsFunction(gCtx, fn)) JSObjectCallAsFunction(gCtx, fn, nullptr, 0, nullptr, nullptr);
}

void callJsGlobal(const char* name, int argc, const JSValueRef* args){
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef key = JSStringCreateWithUTF8CString(name);
    JSValueRef value = JSObjectGetProperty(gCtx, global, key, nullptr);
    JSStringRelease(key);
    if (!JSValueIsObject(gCtx, value)) return;
    JSObjectRef fn = JSValueToObject(gCtx, value, nullptr);
    if (fn && JSObjectIsFunction(gCtx, fn))
        JSObjectCallAsFunction(gCtx, fn, nullptr, (size_t)argc, args, nullptr);
}
// 送达真值版:返回"目标函数存在且完成了调用"。携带所有权引用的回调(publish 的 cid)必须用
// 它——__imgDone 缺失(上下文半初始化窗口)时明确返回 false,调用方据此退还引用;JS 内部抛
// 异常不判"未送达"(JS 侧走到哪不确定,所有权可能已转移,此时退还等于偷走别人的引用),
// 只限频曝光留证。
bool callJsGlobalChecked(const char* name, int argc, const JSValueRef* args){
    if (!gCtx) return false;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef key = JSStringCreateWithUTF8CString(name);
    JSValueRef value = JSObjectGetProperty(gCtx, global, key, nullptr);
    JSStringRelease(key);
    if (!JSValueIsObject(gCtx, value)) return false;
    JSObjectRef fn = JSValueToObject(gCtx, value, nullptr);
    if (!fn || !JSObjectIsFunction(gCtx, fn)) return false;
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(gCtx, fn, nullptr, (size_t)argc, args, &exc);
    if (exc) { static int excN = 0; if (++excN <= 8) fprintf(stderr, "[game] %s 回调抛异常(所有权按已送达处理,第 %d 次)\n", name, excN); }
    return true;
}


// ---- JSC 回调 ----
static JSValueRef cbFlush(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeNumber(ctx, -1);
    size_t words = (size_t)JSValueToNumber(ctx, args[0], exc);
    double t0 = nowUs(); gDec.execute(gBuf, words);
    double elapsed=nowUs()-t0; gFrameDecodeUs+=elapsed;
    return JSValueMakeNumber(ctx, elapsed);
}

sg::CommandReceipt freeSceneNodeFromJs(int id, uint32_t expectedGeneration ){
    const sg::CommandReceipt preflight = gSG.validateFreeNode(id, expectedGeneration);
    if (preflight != sg::CommandReceipt::Applied) return preflight;
    // A materialised GObject child may be disposed independently while its owning Native FGUI
    // instance remains live. JS owns that facade, not the C++-compiled slot. Releasing the slot
    // here leaves managedNodeOwners_ pointing at an id which SceneGraph immediately recycles; the
    // next component then fails its ownership index (all later popups fall back together). Keep
    // partial disposal as a detach and let Runtime::destroyTree be the single freeing authority.
    if (gNativeFgui.isManagedNode(id)) {
        const sg::CommandReceipt detached = gSG.removeFromParent(id, expectedGeneration);
        if (!sg::commandReceiptSucceeded(detached)) return detached;
        gNativeFgui.refreshManagedDisplayVisibility(id, gSG);
        ++gNativeFguiManagedFreeDeferrals;
        return sg::CommandReceipt::ManagedFreeDeferred;
    }
    gSpinexRt.onNodeFreed(id);           // spinex 实例脱钩冻结(防 slot 复用后 quads 写到新主人身上)
    gNativeText.detach(id);
    return gSG.freeNode(id, expectedGeneration);
}
// Phase D-1:native 动画统一推进(spine 帧库+spinex updateAll,含 drive 契约的前置 compute)。
// JS 在 ADVANCE_NATIVE(phase4)调用——动画写入发生在 FINAL_COMPUTE 之前,帧内闭环;
// draw 期从此零写入零 compute(batcher 只读 seal 后状态)。


static JSValueRef cbShadowFlag(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc > 0) gShadowRoot = (int)JSValueToNumber(ctx, a[0], e);
    if (argc > 1) gShadowShow = (int)JSValueToNumber(ctx, a[1], e);
    // mode 2(flip 主屏):在 __nativePresent 调用点**同步**取景绘制,不等 __tick 结束。
    // 浏览器语义是"画 render 调用时刻的状态";renderer.render 之后同一 tick 里 JS 还会改状态
    // (手动驱动的 GTimer/Ticker.shared、排在 app.ticker 之后的 gsap raf、async 工作流的微任务
    // 续体——微任务在 __tick 返回前最后统一执行,任何 JS 层顺序调整都拦不住)。若延到帧末才
    // compute+draw,这些"先显示、下一 tick 才补写坐标"的中间态会被提前一帧画上屏——这就是
    // m90004 小狗符号停轮时闪在第一格的根因(dogflash.log f=1718:present 记账时 141,545/正确,
    // 帧末快照 133,99/旧值,旧管线画的是后者)。窗口被遮挡(gCanPresent=false)时保持旧行为:
    // 只记录,drawOneFrame 会清掉 root 并跳过默认 FBO 写入。
    if (gShadowShow == 2 && gShadowRoot > 0 && gCanPresent && gContext != EGL_NO_CONTEXT) {
        int root = gShadowRoot; gShadowRoot = 0;
        flipMainScreenDraw(root);
    }
    return JSValueMakeUndefined(ctx);
}

// Production presentation contract. It has no shadow mode switch and no deferred fallback: the
// native tree is sampled at renderer.render() and synchronously drawn to the main framebuffer.
// Unsupported nodes remain explicit T_ESCAPE coverage failures; Pixi never draws the main screen.
static JSValueRef cbNativePresent(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 1) return JSValueMakeUndefined(ctx);
    int root = (int)JSValueToNumber(ctx, a[0], e);
    if(argc>=5)for(int i=0;i<4;i++)gSgClear[i]=(float)JSValueToNumber(ctx,a[1+i],e);
    // 帧相位推进与 draw 执行解耦:present 点即 seal 窗口结束。gCanPresent=false(遮挡/后台)
    // 时 draw 跳过,但 PHB 若停留在 6,此后同帧全部业务写会被误计 lateMut(实跑日志 2 万条
    // 假阳性实锤;那些写下帧正常消费,非真 late mutation)。
    if (gSG.buf) gSG.buf[sg::FRAME_PHASE_BASE] = 7.0f;   // B0-1:阶段7=present/draw
    if (root > 0 && gCanPresent && gContext != EGL_NO_CONTEXT) {
        const double presentStartUs = nowUs();
        flipMainScreenDraw(root);
        // __nativePresent runs synchronously inside JSObjectCallAsFunction(__tick). Account its
        // native scenegraph/batcher work separately so the JS bucket remains exclusive script time.
        gFrameTiming.inlinePresentUs += nowUs() - presentStartUs;
        gNativeScreenPresented = true;
    }
    return JSValueMakeUndefined(ctx);
}

// Read-only result of the most recently completed native main-screen paint.  JavaScript tree
// coverage alone cannot see failures discovered during C++ DFS (for example an unsupported nested
// post-process or a mask whose texture upload raced the frame).  The strict cold-start gate samples
// these counters as well, so T_ESCAPE=0 can no longer be a false positive while the batcher skipped
// visible content. Layout: serial, escapes, mask, type/post-process, texture, draws, quads.
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

// Render a native scenegraph subtree into the RenderTexture FBO currently bound by Pixi's
// RenderTextureSystem. Pixi still owns allocation/lifetime of that texture, while it no longer
// traverses or paints the DisplayObject tree. Arguments: root, logicalW/H, clear, alphaScale,
// followed by an affine output matrix (a,b,c,d,tx,ty).
static JSValueRef cbNativeRenderTexturePreflight(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 1 || gContext == EGL_NO_CONTEXT) return JSValueMakeBoolean(ctx, false);
    int root = (int)JSValueToNumber(ctx, a[0], e);
    if (root <= 0 || root >= sg::NODE_CAP || !gSG.nodes[root].alive) return JSValueMakeBoolean(ctx, false);
    if (!gShadowBatcher.ready()) {
        gShadowBatcher.texLookup=[](int id){return gDec.glTextureEnsured((uint32_t)id);};
        gShadowBatcher.init();
    }
    return JSValueMakeBoolean(ctx, gShadowBatcher.preflight(&gSG, root));
}

static JSValueRef cbNativeRenderTexture(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if(argc<11||gContext==EGL_NO_CONTEXT)return JSValueMakeBoolean(ctx,false);
    int root=(int)JSValueToNumber(ctx,a[0],e);float w=(float)JSValueToNumber(ctx,a[1],e),h=(float)JSValueToNumber(ctx,a[2],e);
    bool clear=JSValueToBoolean(ctx,a[3]);float alphaScale=(float)JSValueToNumber(ctx,a[4],e),out[6];for(int i=0;i<6;i++)out[i]=(float)JSValueToNumber(ctx,a[5+i],e);
    GLint fbo=0,vp[4]={0,0,0,0};glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);glGetIntegerv(GL_VIEWPORT,vp);
    if(root<=0||root>=sg::NODE_CAP||!gSG.nodes[root].alive||fbo==0||w<=0||h<=0)return JSValueMakeBoolean(ctx,false);
    if(!gShadowBatcher.ready()){gShadowBatcher.texLookup=[](int id){return gDec.glTextureEnsured((uint32_t)id);};gShadowBatcher.init();}
    if(clear){glDisable(GL_SCISSOR_TEST);glClearColor(0,0,0,0);glClearStencil(0);glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);}
    gShadowBatcher.frameReset();gShadowBatcher.frame(&gSG,root,w,h,out,alphaScale);
    // Keep Pixi's target FBO/viewport contract intact for a subsequent clear:false accumulation.
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)fbo);glViewport(vp[0],vp[1],vp[2],vp[3]);
    return JSValueMakeBoolean(ctx,gShadowBatcher.escapes==0&&gShadowBatcher.escTex==0);
}

// (legacy spine 蒙皮/骨骼/帧库桥 __sp*/__skel* 已随 spinex 全量接管拆除;
//  slot 矩阵回写由共享头 spinex.h 的 __spxSlotBind/updateAll 完成,2026-08-25 清仓)


// KTX2 supercompression uses zstd. Keep decompression out of the JavaScript
// fallback; the bundled single-file decoder is substantially faster on JSC.
#ifndef ZSTD_CONTENTSIZE_UNKNOWN
#define ZSTD_CONTENTSIZE_UNKNOWN (0ULL - 1)
#define ZSTD_CONTENTSIZE_ERROR   (0ULL - 2)
#endif
extern "C" {
    size_t ZSTD_decompress(void* dst, size_t dstCap, const void* src, size_t srcSize);
    unsigned long long ZSTD_getFrameContentSize(const void* src, size_t srcSize);
    unsigned long long ZSTD_decompressBound(const void* src, size_t srcSize);   // 帧头无 size 时求上界
    unsigned ZSTD_isError(size_t code);
}
static JSValueRef cbZstdDecompress(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                   const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1 || !JSValueIsObject(ctx, args[0])) return JSValueMakeNull(ctx);
    JSObjectRef srcObj = JSValueToObject(ctx, args[0], exc);
    // JSObjectGetTypedArrayBytesPtr 返回的是底层 ArrayBuffer 的起始地址,不含视图自身的
    // 漏加 offset 就等于把"整个 KTX2 文件的开头 len 字节"当成 zstd 帧 → 必然解析失败,
    // 然后静默回退到纯 JS 的 fzstd 解压(实测 65/68 次走回退,每次几十毫秒;iOS 无 JIT 下更惨)。
    const uint8_t* srcBase = (const uint8_t*)JSObjectGetTypedArrayBytesPtr(ctx, srcObj, exc);
    size_t srcOff = JSObjectGetTypedArrayByteOffset(ctx, srcObj, exc);
    const uint8_t* src = srcBase ? srcBase + srcOff : nullptr;
    size_t srcLen = JSObjectGetTypedArrayByteLength(ctx, srcObj, exc);
    if (!src || !srcLen) return JSValueMakeNull(ctx);
    // zstd 帧头里的 decompressed size 是可选字段,用流式接口压缩时通常不写(KTX2 的 level 数据正是如此)。
    // 旧写法用 `dstSize + 2 < 2` 同时拒掉了 CONTENTSIZE_ERROR((ULL)-2) 和 CONTENTSIZE_UNKNOWN((ULL)-1),
    // 每次几十毫秒;iOS 无 JIT 下更惨)。UNKNOWN 要用 ZSTD_decompressBound 求上界后正常解。
    unsigned long long dstSize = ZSTD_getFrameContentSize(src, srcLen);
    if (dstSize == ZSTD_CONTENTSIZE_ERROR) return JSValueMakeNull(ctx);
    if (dstSize == ZSTD_CONTENTSIZE_UNKNOWN) dstSize = ZSTD_decompressBound(src, srcLen);
    if (dstSize == 0 || dstSize == ZSTD_CONTENTSIZE_ERROR || dstSize > (256ull << 20)) return JSValueMakeNull(ctx);
    void* dst = malloc((size_t)dstSize);
    if (!dst) return JSValueMakeNull(ctx);
    size_t got = ZSTD_decompress(dst, (size_t)dstSize, src, srcLen);
    if (ZSTD_isError(got)) { free(dst); return JSValueMakeNull(ctx); }
    return JSObjectMakeArrayBufferWithBytesNoCopy(ctx, dst, got,
        [](void* p, void*) { free(p); }, nullptr, exc);
}

// Pure image decode that does not touch JSC, so it is safe on the asset queue.
static unsigned char* decodeImageRaw(NSData* d, int* outW, int* outH) {
    if (!d) return nullptr;
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)d, nullptr);
    if (!src) return nullptr;
    CGImageRef image = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!image) return nullptr;
    int w = (int)CGImageGetWidth(image), h = (int)CGImageGetHeight(image);
    size_t bytes = (size_t)w * h * 4;
    unsigned char* buf = (unsigned char*)malloc(bytes);
    if (!buf) { CGImageRelease(image); return nullptr; }
    memset(buf, 0, bytes);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(buf, w, h, 8, w*4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (cg) {
        // Drawing a top-down CGImage into the lower-left bitmap context yields
        // the bottom-up bytes expected by the WebGL upload shim.
        CGContextSetBlendMode(cg, kCGBlendModeCopy);
        CGContextDrawImage(cg, CGRectMake(0,0,w,h), image);
        CGContextRelease(cg);
    } else {
        free(buf); buf = nullptr;
    }
    CGColorSpaceRelease(cs);
    CGImageRelease(image);
    if (buf) { *outW = w; *outH = h; }
    return buf;
}
// NSData(png/jpg/...) -> JS {width,height,data}. Failure returns null.
static JSValueRef decodeImageNSData(JSContextRef ctx, NSData* d, JSValueRef* exc, bool sourcePremultiplied=false) {
    int w = 0, h = 0;
    unsigned char* buf = sourcePremultiplied?nativeimage::decodeEncodedPMA(d.bytes,d.length,w,h):decodeImageRaw(d, &w, &h);
    if (!buf) return JSValueMakeNull(ctx);
    size_t bytes = (size_t)w * h * 4;
    JSObjectRef abuf = JSObjectMakeArrayBufferWithBytesNoCopy(ctx, buf, bytes, [](void* p, void*){ free(p); }, nullptr, exc);
    JSObjectRef obj = JSObjectMake(ctx, nullptr, nullptr);
    auto setp = [&](const char* k, JSValueRef v){ JSStringRef s = JSStringCreateWithUTF8CString(k); JSObjectSetProperty(ctx, obj, s, v, kJSPropertyAttributeNone, nullptr); JSStringRelease(s); };
    setp("width", JSValueMakeNumber(ctx, w)); setp("height", JSValueMakeNumber(ctx, h)); setp("data", abuf);
    return obj;
}
// __loadImage(url) -> {width,height,data}：本地/远程取字节后解码。
static JSValueRef cbLoadImage(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                              const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeNull(ctx);
    JSStringRef ps = JSValueToStringCopy(ctx, args[0], exc);
    size_t n = JSStringGetMaximumUTF8CStringSize(ps); std::vector<char> pb(n);
    JSStringGetUTF8CString(ps, pb.data(), n); JSStringRelease(ps);
    NSData* d = loadAsset(pb.data());
    JSValueRef r = decodeImageNSData(ctx, d, exc,argc>1&&JSValueToBoolean(ctx,args[1]));
    if (JSValueIsNull(ctx, r)) fprintf(stderr, "[game] __loadImage 失败: %.96s\n", pb.data());
    return r;
}


// CSS 自定义光标：`url('img.png') hx hy, fallback`。图片经 relOf 归一后走 localAssetData 的
// 本地两级查找（game/ 补丁 → assets/ 镜像），同步读文件；主窗口 dpr=1，热点按 CSS 像素 1:1。
// 按整串缓存 NSCursor（含失败负缓存），刮刮卡每次 setCursor 不重复解码。
static NSCursor* cursorFromCssUrl(NSString* raw) {
    static NSMutableDictionary<NSString*, id>* cache = nil;
    if (!cache) cache = [NSMutableDictionary dictionary];
    id hit = cache[raw];
    if (hit) return hit == (id)[NSNull null] ? nil : (NSCursor*)hit;
    NSCursor* built = nil;
    NSRange open = [raw rangeOfString:@"url(" options:NSCaseInsensitiveSearch];
    NSRange close = open.location == NSNotFound ? open
        : [raw rangeOfString:@")" options:0 range:NSMakeRange(NSMaxRange(open), raw.length - NSMaxRange(open))];
    if (close.location != NSNotFound) {
        NSString* url = [raw substringWithRange:NSMakeRange(NSMaxRange(open), close.location - NSMaxRange(open))];
        url = [url stringByTrimmingCharactersInSet:
                  [NSCharacterSet characterSetWithCharactersInString:@" \t'\""]];
        NSString* tail = [raw substringFromIndex:NSMaxRange(close)];
        NSRange comma = [tail rangeOfString:@","];
        if (comma.location != NSNotFound) tail = [tail substringToIndex:comma.location];
        NSPoint hot = NSZeroPoint;
        double hx = 0, hy = 0;
        NSScanner* sc = [NSScanner scannerWithString:tail];
        if ([sc scanDouble:&hx] && [sc scanDouble:&hy]) hot = NSMakePoint(hx, hy);
        NSData* bytes = url.length ? localAssetData(relOf(url)) : nil;
        NSImage* image = bytes ? [[NSImage alloc] initWithData:bytes] : nil;
        if (image) {
            NSImageRep* rep = image.representations.firstObject;
            // png 自带 dpi 时 NSImage.size 会偏离像素数，热点就不再对得上 CSS 坐标；钉回像素尺寸。
            if (rep && rep.pixelsWide > 0 && rep.pixelsHigh > 0)
                image.size = NSMakeSize(rep.pixelsWide, rep.pixelsHigh);
            if (image.size.width > 0 && image.size.height > 0) {
                hot.x = MIN(MAX(hot.x, 0), image.size.width - 1);
                hot.y = MIN(MAX(hot.y, 0), image.size.height - 1);
                built = [[NSCursor alloc] initWithImage:image hotSpot:hot];
            }
        }
    }
    if (!built) fprintf(stderr, "[cursor/mac] url() 光标加载失败，降级 fallback: %s\n", raw.UTF8String);
    cache[raw] = built ?: (id)[NSNull null];
    return built;
}

// Pixi EventSystem writes canvas.style.cursor. The DOM shim forwards that property here so the
// native window follows the same cursor contract as Chrome instead of remaining an AppKit arrow.
static JSValueRef cbSetNativeCursor(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                    const JSValueRef args[], JSValueRef*) {
    NSString* raw = argc ? jsToNSString(ctx, args[0]) : @"default";
    NSString* value = [raw lowercaseString];
    GameCursorKind next = GameCursorKind::Arrow;
    NSCursor* custom = nil;
    if ([value rangeOfString:@"url("].location != NSNotFound) {
        custom = cursorFromCssUrl(raw);
        if (custom) next = GameCursorKind::Custom;
        else {
            // 加载失败按 CSS 语义降级：取最后一个逗号后的 fallback 关键词继续下面的映射。
            NSRange comma = [value rangeOfString:@"," options:NSBackwardsSearch];
            value = comma.location == NSNotFound ? @"default"
                : [[value substringFromIndex:NSMaxRange(comma)] stringByTrimmingCharactersInSet:
                      [NSCharacterSet whitespaceCharacterSet]];
        }
    }
    if (next != GameCursorKind::Custom) {
        if ([value isEqualToString:@"pointer"] || [value isEqualToString:@"hand"] ||
            [value isEqualToString:@"link"]) next = GameCursorKind::PointingHand;
        else if ([value isEqualToString:@"text"] || [value isEqualToString:@"vertical-text"])
            next = GameCursorKind::IBeam;
        else if ([value isEqualToString:@"grab"]) next = GameCursorKind::OpenHand;
        else if ([value isEqualToString:@"grabbing"]) next = GameCursorKind::ClosedHand;
        else if ([value isEqualToString:@"crosshair"]) next = GameCursorKind::Crosshair;
    }
    if (next == gGameCursorKind && custom == gCustomCursor) return JSValueMakeUndefined(ctx);
    gGameCursorKind = next;
    gCustomCursor = custom;
    NSCursor* cursor = gameCursor();
    [cursor set];
    if (gMainWindow && gMainWindow.contentView)
        [gMainWindow invalidateCursorRectsForView:gMainWindow.contentView];
    if (getenv("GAME_CURSOR_TRACE"))
        fprintf(stderr, "[cursor/mac] %s\n", raw.UTF8String ?: "default");
    return JSValueMakeUndefined(ctx);
}


static JSValueRef cbNativeFguiPreparePackage(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3||!JSValueIsObject(ctx,a[1])||!JSValueIsObject(ctx,a[2])||JSValueGetTypedArrayType(ctx,a[2],exc)!=kJSTypedArrayTypeFloat32Array)return JSValueMakeBoolean(ctx,false);
    NSString* pkg=jsToNSString(ctx,a[0]);JSObjectRef ids=JSValueToObject(ctx,a[1],exc),meta=JSValueToObject(ctx,a[2],exc);
    JSStringRef lengthKey=JSStringCreateWithUTF8CString("length");size_t count=(size_t)JSValueToNumber(ctx,JSObjectGetProperty(ctx,ids,lengthKey,exc),exc);JSStringRelease(lengthKey);
    const float* values=(const float*)JSObjectGetTypedArrayBytesPtr(ctx,meta,exc);size_t valueCount=JSObjectGetTypedArrayLength(ctx,meta,exc);if(!values||valueCount<count*4)return JSValueMakeBoolean(ctx,false);
    std::vector<std::pair<std::string,nfgui::AtlasBinding>> atlases;atlases.reserve(count);
    GLint previousTexture=0;if(std::getenv("NATIVE_ATLAS_TRACE"))glGetIntegerv(GL_TEXTURE_BINDING_2D,&previousTexture);
    for(size_t i=0;i<count;i++){
        JSValueRef idValue=JSObjectGetPropertyAtIndex(ctx,ids,(unsigned)i,exc);NSString* idString=jsToNSString(ctx,idValue);nfgui::AtlasBinding binding;binding.textureId=(int)values[i*4];binding.width=values[i*4+1];binding.height=values[i*4+2];binding.premul=values[i*4+3];atlases.emplace_back(idString.UTF8String?:"",binding);
        if(std::getenv("NATIVE_ATLAS_TRACE")){
            GLuint texture=gDec.glTexture((uint32_t)std::max(0,binding.textureId));GLint actualWidth=0,actualHeight=0;
            // ANGLE's GLES compatibility headers omit this ES3 entry point, so resolve it from
            // the active EGL implementation. 0x1000/0x1001 are GL_TEXTURE_WIDTH/HEIGHT.
            using GetTexLevelParameterivFn=void(*)(GLenum,GLint,GLenum,GLint*);
            static GetTexLevelParameterivFn getTexLevelParameteriv=(GetTexLevelParameterivFn)eglGetProcAddress("glGetTexLevelParameteriv");
            if(texture&&getTexLevelParameteriv){glBindTexture(GL_TEXTURE_2D,texture);getTexLevelParameteriv(GL_TEXTURE_2D,0,(GLenum)0x1000,&actualWidth);getTexLevelParameteriv(GL_TEXTURE_2D,0,(GLenum)0x1001,&actualHeight);}
            std::fprintf(stderr,"[native-atlas] package=%s atlas=%s cid=%d gl=%u reported=%.0fx%.0f actual=%dx%d premul=%.0f\n",pkg.UTF8String?:"",idString.UTF8String?:"",binding.textureId,(unsigned)texture,binding.width,binding.height,actualWidth,actualHeight,binding.premul);
            gNativeAtlasTracePending.push_back({binding.textureId,0,binding.width,binding.height,pkg.UTF8String?:"",idString.UTF8String?:"",false});
        }
    }
    if(std::getenv("NATIVE_ATLAS_TRACE"))glBindTexture(GL_TEXTURE_2D,(GLuint)previousTexture);
    gNativeFgui.invalidateReservationsForPackage(pkg.UTF8String?:"",gSG);
    return JSValueMakeBoolean(ctx,gNativeFgui.preparePackage(pkg.UTF8String?:"",atlases,&gSG));
}
// window.open 桥:业务外链(FAN PAGE/条款/隐私政策等)在浏览器为 window.open(url,'_blank')。
// host 垫片把它转到这里,交系统默认浏览器。只放行 http/https —— file:// 等一律拒绝。
static JSValueRef cbNativeOpenURL(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<1)return JSValueMakeBoolean(ctx,false);
    JSStringRef ps=JSValueToStringCopy(ctx,a[0],exc);if(!ps)return JSValueMakeBoolean(ctx,false);
    size_t n=JSStringGetMaximumUTF8CStringSize(ps);std::vector<char> pb(n);
    JSStringGetUTF8CString(ps,pb.data(),n);JSStringRelease(ps);
    std::string url(pb.data());
    if(url.rfind("http://",0)!=0&&url.rfind("https://",0)!=0)return JSValueMakeBoolean(ctx,false);
    NSString* s=[NSString stringWithUTF8String:url.c_str()];
    NSURL* u=s?[NSURL URLWithString:s]:nil;
    bool ok=u&&[[NSWorkspace sharedWorkspace] openURL:u];
    fprintf(stderr,"[open-url] %s -> %s\n",url.c_str(),ok?"ok":"fail");
    return JSValueMakeBoolean(ctx,ok);
}

// Asset bytes and image decoding must never block the JSC/render thread. The
// JavaScript side paces these completion callbacks across frames.
static dispatch_queue_t assetQueue() {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("pixi.asset.mac", DISPATCH_QUEUE_CONCURRENT); });
    return queue;
}
// __nativeFguiPreparse(token,resKey,arrayBuffer|typedArray) -> accepted:boolean.
// Requests for the same package/byte length/CRC share one worker. The matching
// __nativeFguiInflate call later consumes the immutable entry cache on main.
static JSValueRef cbNativeFguiPreparse(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<3)return JSValueMakeBoolean(ctx,false);int token=(int)JSValueToNumber(ctx,args[0],exc);NSString* resKey=jsToNSString(ctx,args[1]);NSData* data=jsValueToData(ctx,args[2],exc);if(!resKey.length||!data.length)return JSValueMakeBoolean(ctx,false);
    uint32_t dataCrc=(uint32_t)crc32(0,(const Bytef*)data.bytes,(uInt)data.length);
    gFguiPreparseStats.requests++;NativeFguiPreparseWaiter waiter{token,gCtx};NativeFguiPreparsed* cached=gFguiPreparseCache[resKey];
    if(cached&&cached.compressedBytes==data.length&&cached.compressedCrc==dataCrc){gFguiPreparseStats.preparseCacheHits++;dispatch_async(dispatch_get_main_queue(),^{notifyNativeFguiPreparse(waiter,true,cached);});return JSValueMakeBoolean(ctx,true);}
    std::string key=resKey.UTF8String?:"";std::string workKey=key+"#"+std::to_string(data.length)+"#"+std::to_string(dataCrc);auto active=gFguiPreparseInflight.find(workKey);
    if(active!=gFguiPreparseInflight.end()){active->second.push_back(waiter);gFguiPreparseStats.deduped++;return JSValueMakeBoolean(ctx,true);}
    gFguiPreparseInflight[workKey].push_back(waiter);gFguiPreparseStats.jobs++;JSGlobalContextRef requestContext=gCtx;
    dispatch_async(assetQueue(),^{
        NativeFguiPreparsed* parsed=nil;@autoreleasepool{dispatch_semaphore_wait(assetCpuSlots(),DISPATCH_TIME_FOREVER);parsed=parseNativeFguiArchive(data,true,key);dispatch_semaphore_signal(assetCpuSlots());}
        dispatch_async(dispatch_get_main_queue(),^{
            auto found=gFguiPreparseInflight.find(workKey);if(found==gFguiPreparseInflight.end())return;std::vector<NativeFguiPreparseWaiter> waiters=std::move(found->second);gFguiPreparseInflight.erase(found);
            if(gCtx!=requestContext){return;}bool ok=parsed!=nil;if(ok){cacheNativeFguiPreparse(resKey,parsed);gFguiPreparseStats.backgroundInflateMs+=parsed.inflateMs;gFguiPreparseStats.backgroundXmlMs+=parsed.xmlMs;gFguiPreparseStats.backgroundBlueprintMs+=parsed.blueprintMs;gFguiPreparseStats.lastInflateMs=parsed.inflateMs;}else gFguiPreparseStats.failures++;
            if(getenv("FGUI_PREPARSE_LOG"))fprintf(stderr,"[fgui-preparse/mac] READY %s ok=%d entries=%lu inflate=%.2fms xml=%.2fms blueprint=%.2fms waiters=%zu\n",resKey.UTF8String?:"",ok?1:0,(unsigned long)(parsed?parsed.entryCount:0),parsed?parsed.inflateMs:0,parsed?parsed.xmlMs:0,parsed?parsed.blueprintMs:0,waiters.size());
            for(const auto& pending:waiters)notifyNativeFguiPreparse(pending,ok,parsed);
        });
    });
    return JSValueMakeBoolean(ctx,true);
}
static JSValueRef cbLoadImageAsync(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                   const JSValueRef args[], JSValueRef* exc) {
    if (argc < 2) return JSValueMakeUndefined(ctx);
    NSString* url = jsToNSString(ctx, args[0]);
    int token = (int)JSValueToNumber(ctx, args[1], exc);
    JSGlobalContextRef requestContext = gCtx;
    // Pixi format probes use tiny data: URLs. The host intentionally does not
    // implement data-URI decoding; fail them on the next main turn so feature
    // detection cannot wait behind the native resource queue.
    if ([url hasPrefix:@"data:"]) {
        dispatch_async(dispatch_get_main_queue(), ^{ finishNativeImageAsync(requestContext, token, 0, 0, 0); });
        return JSValueMakeUndefined(ctx);
    }
    gNativeResources.requestImageURL(url, [requestContext, token, url](uint32_t cid, int width, int height) {
        if (!cid) fprintf(stderr, "[game] native image prepare 失败: %.96s\n", url.UTF8String);
        deliverPublishedImage(requestContext, token, cid, width, height);
    }, /*publishRef=*/true,argc>2&&JSValueToBoolean(ctx,args[2]));
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbReadFileAsync(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                  const JSValueRef args[], JSValueRef* exc) {
    if (argc < 2) return JSValueMakeUndefined(ctx);
    NSString* url = jsToNSString(ctx, args[0]);
    int token = (int)JSValueToNumber(ctx, args[1], exc);
    JSGlobalContextRef requestContext = gCtx;
    gNativeResources.requestBytes(url, [requestContext, token](NSData* data) {
        if (!gCtx || gCtx != requestContext) return;
        JSValueRef dataValue = JSValueMakeNull(gCtx);
        if (data.length) {
            void* bytes = malloc(data.length);
            if (bytes) {
                memcpy(bytes, data.bytes, data.length);
                JSValueRef error = nullptr;
                dataValue = JSObjectMakeArrayBufferWithBytesNoCopy(gCtx, bytes, data.length,
                    [](void* p, void*) { free(p); }, nullptr, &error);
                if (error) dataValue = JSValueMakeNull(gCtx);
            }
        }
        JSValueRef callbackArgs[2] = { JSValueMakeNumber(gCtx, token), dataValue };
        callJsGlobal("__fileDone", 2, callbackArgs);
    });
    return JSValueMakeUndefined(ctx);
}


// The JS-visible global fetch is the asset-read adapter (it ignores method/body), so telemetry
// must go through a real NSURLSession POST. The task never touches gCtx, so a reload right
// after (e.g. following "activated") cannot drop it. With a token, delivery is acknowledged
// back to JS only on HTTP 2xx — the one-shot rollback report consumes its marker on that ack.


// Native JSZip backend. Pure-JS inflate was responsible for 100-300ms promise
// callbacks while entering machines. Parse and inflate all entries off-thread;
// JS receives the same minimal JSZip-compatible object from game_host.js.
static NSMutableDictionary<NSNumber*, NSDictionary<NSString*, NSData*>*>* gZipCache = nil;
static NSMutableArray<NSNumber*>* gZipLru = nil;
NSDictionary<NSString*, NSData*>* zipParseAll(NSData* data){
    const uint8_t* bytes = (const uint8_t*)data.bytes;
    size_t length = data.length;
    if (length < 22) return nil;
    ssize_t eocd = -1;
    ssize_t low = (ssize_t)length - 22 - 65536;
    if (low < 0) low = 0;
    for (ssize_t i = (ssize_t)length - 22; i >= low; --i) {
        if (bytes[i] == 0x50 && bytes[i+1] == 0x4b && bytes[i+2] == 0x05 && bytes[i+3] == 0x06) {
            eocd = i; break;
        }
    }
    if (eocd < 0) return nil;
    uint16_t total = 0; uint32_t centralOffset = 0;
    memcpy(&total, bytes + eocd + 10, 2);
    memcpy(&centralOffset, bytes + eocd + 16, 4);
    if (total == 0xffff || centralOffset == 0xffffffff) return nil;
    NSMutableDictionary<NSString*, NSData*>* entries = [NSMutableDictionary dictionary];
    size_t offset = centralOffset;
    for (uint32_t index = 0; index < total; ++index) {
        if (offset + 46 > length || bytes[offset] != 0x50 || bytes[offset+1] != 0x4b
            || bytes[offset+2] != 0x01 || bytes[offset+3] != 0x02) return nil;
        uint16_t method = 0, nameLen = 0, extraLen = 0, commentLen = 0;
        uint32_t compressedSize = 0, uncompressedSize = 0, localOffset = 0;
        memcpy(&method, bytes + offset + 10, 2);
        memcpy(&compressedSize, bytes + offset + 20, 4);
        memcpy(&uncompressedSize, bytes + offset + 24, 4);
        memcpy(&nameLen, bytes + offset + 28, 2);
        memcpy(&extraLen, bytes + offset + 30, 2);
        memcpy(&commentLen, bytes + offset + 32, 2);
        memcpy(&localOffset, bytes + offset + 42, 4);
        if (offset + 46 + nameLen > length) return nil;
        NSString* name = [[NSString alloc] initWithBytes:bytes + offset + 46
                                                  length:nameLen encoding:NSUTF8StringEncoding];
        offset += 46u + nameLen + extraLen + commentLen;
        if (!name || [name hasSuffix:@"/"]) continue;
        if ((size_t)localOffset + 30 > length || bytes[localOffset] != 0x50 || bytes[localOffset+1] != 0x4b
            || bytes[localOffset+2] != 0x03 || bytes[localOffset+3] != 0x04) return nil;
        uint16_t localNameLen = 0, localExtraLen = 0;
        memcpy(&localNameLen, bytes + localOffset + 26, 2);
        memcpy(&localExtraLen, bytes + localOffset + 28, 2);
        size_t dataOffset = (size_t)localOffset + 30 + localNameLen + localExtraLen;
        if (dataOffset + compressedSize > length) return nil;
        NSData* content = nil;
        if (method == 0) {
            content = [NSData dataWithBytes:bytes + dataOffset length:compressedSize];
        } else if (method == 8) {
            // uncompressedSize comes from the (possibly corrupt/hostile) central directory:
            // cap it and verify the allocation, or inflate() writes through a NULL next_out.
            if (uncompressedSize > (512u << 20)) return nil;
            NSMutableData* inflated = [NSMutableData dataWithLength:uncompressedSize];
            if (!inflated || (uncompressedSize > 0 && !inflated.mutableBytes)) return nil;
            z_stream stream; memset(&stream, 0, sizeof(stream));
            stream.next_in = (Bytef*)(bytes + dataOffset); stream.avail_in = compressedSize;
            stream.next_out = (Bytef*)inflated.mutableBytes; stream.avail_out = uncompressedSize;
            if (inflateInit2(&stream, -15) != Z_OK) return nil;
            int result = inflate(&stream, Z_FINISH); size_t produced = stream.total_out; inflateEnd(&stream);
            if (result != Z_STREAM_END || produced != uncompressedSize) return nil;
            content = inflated;
        } else {
            return nil;
        }
        entries[name] = content;
    }
    return entries;
}


static uint64_t gSpinePrepareEpoch=1;


static void spinexPrewarm(NativeSpinePrepared* parsed);   // 定义在 spinex 段


static JSValueRef cbNativeSpinePreparse(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);int token=(int)JSValueToNumber(ctx,args[0],exc);NSString* url=jsToNSString(ctx,args[1]),*key=jsToNSString(ctx,args[2]);NSData* data=jsValueToData(ctx,args[3],exc);
    if(!url.length||!key.length||!data.length)return JSValueMakeBoolean(ctx,false);gSpinePrepareStats.requests++;uint32_t inputCrc=nativeSpineInputCrc(data);NSString* cacheKey=nativeSpineCacheKey(url,key);
    bool wantsLease=argc>4&&JSValueToBoolean(ctx,args[4]);   // 只有声明读 holder 的消费链(prepared wrapper)才发 lease;legacy 链只用原始字节
    NativeSpinePrepareWaiter waiter{token,gCtx,wantsLease};NativeSpinePrepared* cached=gSpinePreparedCache[cacheKey];
    if(cached&&cached.compressedBytes==data.length&&cached.inputCrc==inputCrc){
        gSpinePrepareStats.cacheHits++;touchNativeSpinePrepared(cached);
        if(wantsLease)cached.activeUsers=cached.activeUsers+1;   // 在途 lease:入队前同步获取(排队期间的插入逐出不能挤掉它),notify 交付或退还
        // A prepared ZIP outlives the spinex DataStore entry. Popup/resource teardown may release
        // the final SkeletonData reference while retaining this compressed-package cache entry;
        // the next load then legitimately hits here and needs a fresh prewarm. Without restarting
        // it, binary-only (.skel) packages fall through to the synchronous rebuild path after the
        // JS skeleton cache has already been replaced, making every dynamic Spine slot empty on a
        // repeated popup open.
        spinexPrewarm(cached);
        if(getenv("SPINE_PREPARSE_LOG"))fprintf(stderr,"[spine-preparse/mac] HIT %s handle=%d entries=%lu crc=%08x\n",key.UTF8String,cached.handle,(unsigned long)cached.manifestEntries,inputCrc);
        dispatch_async(dispatch_get_main_queue(),^{notifyNativeSpinePreparse(waiter,true,cached);});return JSValueMakeBoolean(ctx,true);
    }
    uint64_t requestEpoch=gSpinePrepareEpoch;std::string workKey=std::to_string(requestEpoch)+"#"+(cacheKey.UTF8String?:"")+std::string("#")+std::to_string(data.length)+"#"+std::to_string(inputCrc);auto active=gSpinePrepareInflight.find(workKey);
    if(active!=gSpinePrepareInflight.end()){active->second.push_back(waiter);gSpinePrepareStats.deduped++;if(getenv("SPINE_PREPARSE_LOG"))fprintf(stderr,"[spine-preparse/mac] DEDUP %s bytes=%lu crc=%08x waiters=%zu\n",key.UTF8String,(unsigned long)data.length,inputCrc,active->second.size());return JSValueMakeBoolean(ctx,true);}
    gSpinePrepareInflight[workKey].push_back(waiter);gSpinePrepareStats.jobs++;JSGlobalContextRef requestContext=gCtx;
    dispatch_async(assetQueue(),^{NativeSpinePrepared* parsed=nil;@autoreleasepool{dispatch_semaphore_wait(assetCpuSlots(),DISPATCH_TIME_FOREVER);parsed=prepareNativeSpineArchive(data,url,key,inputCrc);dispatch_semaphore_signal(assetCpuSlots());}
        dispatch_async(dispatch_get_main_queue(),^{if(requestEpoch!=gSpinePrepareEpoch)return;auto found=gSpinePrepareInflight.find(workKey);if(found==gSpinePrepareInflight.end())return;std::vector<NativeSpinePrepareWaiter> waiters=std::move(found->second);gSpinePrepareInflight.erase(found);if(gCtx!=requestContext)return;
            bool ok=parsed!=nil;if(ok){int leaseN=0;for(const auto& w:waiters)if(w.wantsLease)leaseN++;parsed.activeUsers=leaseN;/* 在途 lease:插入前按 lease-waiter 数出生——插入触发的逐出不能挤掉自己(cap=1 实测 43 次 invalid manifest 即此窗口);legacy waiter 不计 */cacheNativeSpinePrepared(parsed);gSpinePrepareStats.ready++;gSpinePrepareStats.backgroundZipMs+=parsed.zipMs;gSpinePrepareStats.backgroundAtlasMs+=parsed.atlasMs;gSpinePrepareStats.backgroundJsonMs+=parsed.jsonMs;spinexPrewarm(parsed);}else gSpinePrepareStats.failures++;
            if(getenv("SPINE_PREPARSE_LOG"))fprintf(stderr,"[spine-preparse/mac] READY %s ok=%d handle=%d entries=%lu images=%lu zip=%.2fms json=%.2fms waiters=%zu\n",key.UTF8String?:"",ok?1:0,parsed?parsed.handle:0,(unsigned long)(parsed?parsed.manifestEntries:0),(unsigned long)(parsed?parsed.imageNames.count:0),parsed?parsed.zipMs:0,parsed?parsed.jsonMs:0,waiters.size());
            for(const auto& pending:waiters)notifyNativeSpinePreparse(pending,ok,parsed);});});
    return JSValueMakeBoolean(ctx,true);
}

static JSValueRef cbLoadSpinePreparedImage(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<3)return JSValueMakeBoolean(ctx,false);int handle=(int)JSValueToNumber(ctx,args[0],exc),token=(int)JSValueToNumber(ctx,args[2],exc);NSString* name=jsToNSString(ctx,args[1]);NativeSpinePrepared* parsed=gSpinePreparedByHandle[@(handle)];NSData* data=parsed.entries[name];
    if(!parsed||!data.length){gSpinePrepareStats.directImageMisses++;return JSValueMakeBoolean(ctx,false);}gSpinePrepareStats.directImageRequests++;touchNativeSpinePrepared(parsed);JSGlobalContextRef requestContext=gCtx;
    NSString* stableName=[NSString stringWithFormat:@"%@#%08x#%@",parsed.cacheKey,parsed.inputCrc,name];
    gNativeResources.requestImageDataNamed(stableName,data,[requestContext,token,handle,name](uint32_t cid,int width,int height){if(!cid&&getenv("SPINE_PREPARSE_LOG"))fprintf(stderr,"[spine-preparse/mac] IMAGE_FAIL handle=%d %s\n",handle,name.UTF8String?:"");deliverPublishedImage(requestContext,token,cid,width,height);},/*publishRef=*/true);
    return JSValueMakeBoolean(ctx,true);
}
static JSValueRef cbNativeSpineRecordFallback(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef*){gSpinePrepareStats.fallbacks++;if(getenv("SPINE_PREPARSE_LOG")){NSString* url=argc?jsToNSString(ctx,args[0]):@"",*reason=argc>1?jsToNSString(ctx,args[1]):@"";fprintf(stderr,"[spine-preparse/mac] FALLBACK %.96s reason=%.120s\n",url.UTF8String?:"",reason.UTF8String?:"");}return JSValueMakeUndefined(ctx);}

// ---- 骨架预热 ----
// 原本 __spxBuild 在业务真正要用时才解析骨架:749-slot 的 m95009_lock_jackpot 要 200ms+,正好落在
// loading 94% 的主线程上,把 loading 的 spine 动画冻住。改成 preparse 一就绪就在后台起解析；
// Atlas 先登记页级延迟请求，纯 CPU 的 readSkeletonData 去后台。等业务调 __spxBuild 时通常
// 已经算完,直接登记即可，页图则等首个实例出现再激活。
static std::unordered_map<std::string,std::shared_ptr<spinex::DataStore::AsyncBuild>> gSpinexPrewarm;
static spinex::TexRequestFn spinexMakeTexRequestMac(NativeSpinePrepared* holder){
    return [holder](const std::string& pageName,const spinex::TexStatePtr& st){
        NSString* name=[NSString stringWithUTF8String:pageName.c_str()];
        NSData* png=name.length?holder.entries[name]:nil;
        if(!png.length){st->failed=true;fprintf(stderr,"[spinex] atlas 页 %s 无 zip 条目\n",pageName.c_str());return;}
        NSString* stableName=[NSString stringWithFormat:@"%@#%08x#%@",holder.cacheKey,holder.inputCrc,name];
        // Atlas 解析期只裁出本页闭包，不上传。闭包仅持有本页压缩数据与稳定名，不把整个 ZIP
        // holder 钉在 DataStore；首个实例由 activateTextures 调用，最后实例离场后可再次调用。
        st->request=[png,stableName](const std::string&,const spinex::TexStatePtr& state){
            spinex::TexStatePtr sp=state;const uint64_t generation=state->generation;
            gNativeResources.requestImageDataNamed(stableName,png,[sp,generation](uint32_t cid,int width,int height){
                if(sp->abandoned||sp->generation!=generation)return;
                if(!cid){sp->failed=true;return;}
                sp->cid=cid;sp->width=width;sp->height=height;
                if(gNativeResources.retain(cid))sp->retained=true;
            });
        };
    };
}
static void spinexPrewarm(NativeSpinePrepared* parsed){
    if (gHostShuttingDown.load(std::memory_order_acquire)) return;
    static const bool disabled = getenv("SPINE_PREWARM_OFF") != nullptr;
    if(disabled)return;
    if(!parsed||!parsed.key.length)return;
    spinexEnsureHost();
    std::string k=parsed.key.UTF8String?:"";
    if(k.empty())return;
    auto pending=gSpinexPrewarm.find(k);
    if(pending!=gSpinexPrewarm.end()){
        if(pending->second&&pending->second->inputCrc==(uint32_t)parsed.inputCrc)return;
        gSpinexRt.store.discardAsync(pending->second);
        gSpinexPrewarm.erase(pending);
    }
    if(auto* found=gSpinexRt.store.find(k)) if(found->inputCrc==parsed.inputCrc) return;
    NSData* skelData=parsed.skelFilename.length?parsed.entries[parsed.skelFilename]:nil;
    NSData* jsonData=parsed.skeletonFilename.length?parsed.entries[parsed.skeletonFilename]:nil;
    if(!skelData.length&&!jsonData.length)return;   // 二者有其一即可:.skel 主用,.json 是过渡期回退
    std::string atlasText=parsed.atlasText.UTF8String?:"";
    std::string jsonText=jsonData.length?std::string((const char*)jsonData.bytes,(size_t)jsonData.length):std::string();
    auto job=gSpinexRt.store.prepareAsync(k,atlasText.c_str(),(int)atlasText.size(),jsonText.c_str(),
                                          (uint32_t)parsed.inputCrc,spinexMakeTexRequestMac(parsed),
                                          skelData.length?(const unsigned char*)skelData.bytes:nullptr,(int)skelData.length);
    if(!job)return;
    gSpinexPrewarm[k]=job;
    dispatch_async(assetQueue(),^{
        dispatch_semaphore_wait(assetCpuSlots(),DISPATCH_TIME_FOREVER);
        spinex::DataStore::runAsync(job);
        dispatch_semaphore_signal(assetCpuSlots());
    });
}
spinex::DataStore::Entry* spinexEnsureBuilt(NSString* key,int preparedHandle,std::string* outError){
    spinexEnsureHost();
    std::string k=key.UTF8String?:"";
    // 身份优先:JS 带来的 preparedHandle 是"这次加载的精确身份"(gSpinePreparedByHandle 直取),
    // 只有它缺席(旧调用方/条目已被 LRU 逐出)才退回按短 key 从同名候选里挑 LRU 最新——后者在
    // 异步交错时(A 的图片链晚于 B 的 preparse 完成)会给 A 挑出 B 的包。
    NativeSpinePrepared* parsed=nil;
    if(preparedHandle>0){
        parsed=gSpinePreparedByHandle[@(preparedHandle)];
        if(parsed)touchNativeSpinePrepared(parsed);
        else {
            // 带着明确身份却找不到 prepared(LRU 已逐出):**拒绝退回短名猜测**——同名候选里的
            // JS ensureHandle 沿用现有表(瞬态失败路径);实例引用走 __spxRetain 不受影响。
            fprintf(stderr,"[spinex] key=%s preparedHandle=%d 已被 LRU 逐出,拒绝按短名猜测\n",k.c_str(),preparedHandle);
            if(outError)*outError="prepared handle evicted (refusing short-key guess)";
            return nullptr;
        }
    }
    if(!parsed)parsed=spinexFindPrepared(key);
    if(!parsed){
        // preparse 缓存 miss(LRU 逐出等):已建条目无从比对 crc。只有 JS 侧仍持有引用
        // 会拿错骨架——宁可报错让门面 ctorFallback 走 legacy(游戏会重新加载资源再来)。
        if(auto* found=gSpinexRt.store.find(k)){
            if(!found->jsReleased)return found;
            // jsReleased 但条目仍存活(refs/活体让它没被销毁):它就是该 key 的现行数据——公共资源
            // (bigwin 等)被业务清理后复用的合法场景,复用它严格优于返回失败(create null →
            // 在位条目总是最新代(旧代已 retire 改名)。保留日志供观测复用频率。
            fprintf(stderr,"[spinex] key=%s preparse miss,复用已释放但仍存活的条目(sd=%d refs=%d live=%d)\n",
                    k.c_str(),found->handle,found->refs,found->liveInstances);
            return found;
        }
        if(outError)*outError="prepared entry not found (preparse cache miss)";
        return nullptr;
    }
    if(auto* found=gSpinexRt.store.find(k))
        if(found->inputCrc==parsed.inputCrc)return found;   // crc 不一致时落到 buildFromPrepared 内的失效重建
    // 预热命中:后台已算完就直接登记(主线程 0 解析);还没算完就在此等它,等待时长 = 剩余部分,
    // 最坏退化成原来的同步耗时,不会更差。
    auto pre=gSpinexPrewarm.find(k);
    if(pre!=gSpinexPrewarm.end()){
        auto job=pre->second;
        if(job&&job->inputCrc==(uint32_t)parsed.inputCrc){
            double waitT0=nowUs();
            while(!job->finished.load(std::memory_order_acquire)) usleep(200);   // acquire:与后台 release 配对,保证 data 可见
            double waitMs=(nowUs()-waitT0)/1000.0;
            gSpinexPrewarm.erase(pre);
            spinex::DataStore::Entry* built=gSpinexRt.store.commitAsync(job,outError);   // 失败/丢弃分支内部退还页图引用
            if (!built || PIXINATIVE_DIAG_LOG)
                fprintf(stderr,"[spinex] key=%s 预热%s(主线程等待 %.1fms)\n",k.c_str(),built?"命中":"失败",waitMs);
            if(built)return built;
        } else {
            // 的页图 cid 引用——直接 erase 让 shared_ptr 析构会把这些 GPU 纹理永久占住。
            gSpinexRt.store.discardAsync(pre->second);
            gSpinexPrewarm.erase(pre);
        }
    }
    NSData* jsonData=parsed.skeletonFilename.length?parsed.entries[parsed.skeletonFilename]:nil;
    NSData* skelEntry=parsed.skelFilename.length?parsed.entries[parsed.skelFilename]:nil;
    if(!skelEntry.length&&!jsonData.length){if(outError)*outError="skeleton entry missing (.skel/.json)";return nullptr;}
    std::string atlasText=parsed.atlasText.UTF8String?:"";   // spine::Atlas 解析期读取，不留引用
    // 无 .json 时留空串:parseSkeleton 的 `if(!data && jsonText && jsonText[0])` 会跳过 JSON 分支
    std::string jsonText=jsonData.length?std::string((const char*)jsonData.bytes,(size_t)jsonData.length):std::string();
    // 纹理接线：页名 → prepared 的 png NSData → stableName（cacheKey#inputCrc#imageName）→ 异步 cid。
    // cid 就绪即 ResourceManager retain（常驻），entry 销毁时经 setTexRelease 对称 release。
    NativeSpinePrepared* holder=parsed;
    spinex::TexRequestFn texRequest=[holder](const std::string& pageName,const spinex::TexStatePtr& st){
        NSString* name=[NSString stringWithUTF8String:pageName.c_str()];
        NSData* png=name.length?holder.entries[name]:nil;
        if(!png.length){st->failed=true;fprintf(stderr,"[spinex] atlas 页 %s 无 zip 条目\n",pageName.c_str());return;}
        NSString* stableName=[NSString stringWithFormat:@"%@#%08x#%@",holder.cacheKey,holder.inputCrc,name];
        st->request=[png,stableName](const std::string&,const spinex::TexStatePtr& state){
            spinex::TexStatePtr sp=state;const uint64_t generation=state->generation;
            gNativeResources.requestImageDataNamed(stableName,png,[sp,generation](uint32_t cid,int width,int height){
                if(sp->abandoned||sp->generation!=generation)return;   // entry 已销毁/活跃期已换代
                if(!cid){sp->failed=true;if(getenv("SPINE_PREPARSE_LOG"))fprintf(stderr,"[spinex] 页图解码失败\n");return;}
                sp->cid=cid;sp->width=width;sp->height=height;
                if(gNativeResources.retain(cid))sp->retained=true;
            });
        };
    };
    // 同包若带 .skel 就优先用二进制(快数倍);解析失败时 buildFromPrepared 内部自动回退这份 JSON。
    NSData* skelData=parsed.skelFilename.length?parsed.entries[parsed.skelFilename]:nil;
    spinex::DataStore::Entry* built=gSpinexRt.store.buildFromPrepared(k,atlasText.c_str(),(int)atlasText.size(),
                                             jsonText.c_str(),(uint32_t)parsed.inputCrc,texRequest,outError,
                                             skelData.length?(const unsigned char*)skelData.bytes:nullptr,(int)skelData.length);
    // 老版本骨架点名:spine-cpp 4.2 拒析 3.x 导出(现网仅 fx_bet 3.8.99),笼统的解析失败会误导排查。
    // 明确打出"资源版本不受支持",门面 ctorFallback 回退 legacy 照常,资源侧升版导出即可收编。
    if(!built&&parsed.version.length&&[parsed.version hasPrefix:@"3."])
        fprintf(stderr,"[spinex] key=%s spine %s 骨架不受支持,已回退 legacy(资源侧升版可收编)\n",
                k.c_str(),parsed.version.UTF8String);
    return built;
}

static JSValueRef cbZipParse(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
    if (argc < 2) return JSValueMakeUndefined(ctx);
    int token = (int)JSValueToNumber(ctx, args[0], exc);
    NSData* data = jsValueToData(ctx, args[1], exc);
    dispatch_async(assetQueue(), ^{
        dispatch_semaphore_wait(assetCpuSlots(), DISPATCH_TIME_FOREVER);
        NSDictionary<NSString*, NSData*>* entries = data ? zipParseAll(data) : nil;
        dispatch_semaphore_signal(assetCpuSlots());
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!gCtx) return;
            JSValueRef namesValue = JSValueMakeNull(gCtx);
            if (entries) {
                if (!gZipCache) { gZipCache = [NSMutableDictionary dictionary]; gZipLru = [NSMutableArray array]; }
                gZipCache[@(token)] = entries; [gZipLru addObject:@(token)];
                while (gZipLru.count > 8) {
                    [gZipCache removeObjectForKey:gZipLru.firstObject];
                    [gZipLru removeObjectAtIndex:0];
                }
                NSData* jsonData = [NSJSONSerialization dataWithJSONObject:entries.allKeys options:0 error:nil];
                NSString* json = jsonData ? [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding] : nil;
                if (json) {
                    JSStringRef string = JSStringCreateWithUTF8CString(json.UTF8String);
                    namesValue = JSValueMakeString(gCtx, string); JSStringRelease(string);
                }
            }
            JSValueRef callbackArgs[2] = { JSValueMakeNumber(gCtx, token), namesValue };
            callJsGlobal("__zipDone", 2, callbackArgs);
        });
    });
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbZipGet(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                           const JSValueRef args[], JSValueRef* exc) {
    if (argc < 2) return JSValueMakeNull(ctx);
    int token = (int)JSValueToNumber(ctx, args[0], exc);
    NSString* name = jsToNSString(ctx, args[1]);
    bool asString = argc > 2 && JSValueToNumber(ctx, args[2], exc) != 0;
    NSData* data = gZipCache ? gZipCache[@(token)][name] : nil;
    if (!data) return JSValueMakeNull(ctx);
    if (asString) {
        NSString* value = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        if (!value) return JSValueMakeNull(ctx);
        JSStringRef string = JSStringCreateWithUTF8CString(value.UTF8String);
        JSValueRef result = JSValueMakeString(ctx, string); JSStringRelease(string);
        return result;
    }
    void* bytes = malloc(data.length);
    if (!bytes) return JSValueMakeNull(ctx);
    memcpy(bytes, data.bytes, data.length);
    JSObjectRef buffer = JSObjectMakeArrayBufferWithBytesNoCopy(ctx, bytes, data.length,
        [](void* p, void*) { free(p); }, nullptr, exc);
    return buffer ? (JSValueRef)buffer : JSValueMakeNull(ctx);
}

// ======================= Native audio (same JS contract as iOS) =======================
// pixi-sound uses the HTMLAudio shim in game_host.js. Each shim instance maps to an
// AVAudioPlayer, while clones share NSData and reuse idle decoded players.
@interface MacAudioDelegate : NSObject <AVAudioPlayerDelegate> @end
static NSMutableDictionary<NSNumber*, AVAudioPlayer*>* gAuPlayers = nil;
static NSMutableDictionary<NSNumber*, NSData*>* gAuData = nil;
static NSMutableSet<NSNumber*>* gAuSourceIds = nil;
static NSMutableDictionary<NSString*, NSData*>* gAuUrlCache = nil;
static NSMutableSet<AVAudioPlayer*>* gAuBusy = nil;
// These collections are main-thread only.  Pending URL reads are coalesced here so a burst of
// Audio clones cannot download/read the same asset several times before the first one completes.
static NSMutableDictionary<NSNumber*, NSString*>* gAuPendingUrls = nil;
static NSMutableSet<NSString*>* gAuLoadingUrls = nil;
static uint64_t gAuLoadEpoch = 1;
static MacAudioDelegate* gAuDelegate = nil;

// Regression runs open many real pages in quick succession. Keep muting at the Native output
// boundary so game volume state and business callbacks are exercised unchanged while the host
// emits no sound. Production/default launches remain audible unless GAME_MUTE_AUDIO is enabled.
static bool audioMutedByEnvironment() {
    const char* value = std::getenv("GAME_MUTE_AUDIO");
    return value && value[0] && std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0 && std::strcmp(value, "FALSE") != 0;
}

static dispatch_queue_t audioQueue() {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("pixi.audio.mac", DISPATCH_QUEUE_SERIAL); });
    return queue;
}

// URL I/O and AVAudioPlayer decoding/preparation must not run on the JSC/render thread.  Keep this
// separate from audioQueue(): a slow remote request must not delay pause/stop/volume operations on
// players that are already live.
static dispatch_queue_t audioLoadQueue() {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("pixi.audio.load.mac", DISPATCH_QUEUE_CONCURRENT);
        dispatch_set_target_queue(queue,
            dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    });
    return queue;
}

static dispatch_semaphore_t audioLoadSlots() {
    static dispatch_semaphore_t slots;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ slots = dispatch_semaphore_create(2); });
    return slots;
}

static void callAudioEnded(int audioId) {
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef name = JSStringCreateWithUTF8CString("__auEnded");
    JSValueRef value = JSObjectGetProperty(gCtx, global, name, nullptr);
    JSStringRelease(name);
    if (!JSValueIsObject(gCtx, value)) return;
    JSObjectRef callback = JSValueToObject(gCtx, value, nullptr);
    if (!callback || !JSObjectIsFunction(gCtx, callback)) return;
    JSValueRef arg = JSValueMakeNumber(gCtx, audioId);
    JSObjectCallAsFunction(gCtx, callback, nullptr, 1, &arg, nullptr);
}

static void callAudioDone(int audioId, double duration) {
    if (!gCtx) return;
    JSValueRef args[2] = { JSValueMakeNumber(gCtx, audioId),
                           JSValueMakeNumber(gCtx, duration) };
    callJsGlobal("__auDone", 2, args);
}

@implementation MacAudioDelegate
- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer*)player successfully:(BOOL)flag {
    (void)flag;
    dispatch_async(dispatch_get_main_queue(), ^{
        [gAuBusy removeObject:player];
        NSNumber* found = nil;
        for (NSNumber* key in gAuPlayers) {
            if (gAuPlayers[key] == player) { found = key; break; }
        }
        if (found) callAudioEnded(found.intValue);
    });
}
@end

static void ensureAudioInitialized() {
    if (gAuPlayers) return;
    gAuPlayers = [NSMutableDictionary dictionary];
    gAuData = [NSMutableDictionary dictionary];
    gAuSourceIds = [NSMutableSet set];
    gAuUrlCache = [NSMutableDictionary dictionary];
    gAuBusy = [NSMutableSet set];
    gAuPendingUrls = [NSMutableDictionary dictionary];
    gAuLoadingUrls = [NSMutableSet set];
    gAuDelegate = [MacAudioDelegate new];
    fprintf(stderr, "[audio/mac] AVAudioPlayer bridge initialized%s\n",
            audioMutedByEnvironment() ? " (muted by GAME_MUTE_AUDIO)" : "");
}

// Pure per-player work: no global cache/player collections are touched, so this is safe on
// audioLoadQueue.  The delegate is intentionally attached only when the player reaches main.
static AVAudioPlayer* prepareAudioPlayer(int audioId, NSData* data) {
    if (!data.length) return nil;
    NSError* error = nil;
    AVAudioPlayer* player = [[AVAudioPlayer alloc] initWithData:data error:&error];
    if (!player) {
        fprintf(stderr, "[audio/mac] #%d async decode failed: %s\n", audioId,
                error.localizedDescription.UTF8String ?: "?");
        return nil;
    }
    player.enableRate = YES;
    if (audioMutedByEnvironment()) player.volume = 0;
    [player prepareToPlay];
    return player;
}

// Main-thread commit for a player prepared in the background.  All existing audio collections
// stay confined to main; subsequent AVAudioPlayer controls continue to use audioQueue().
static double installPreparedAudioPlayer(int audioId, NSData* data, AVAudioPlayer* player) {
    if (!data.length || !player) return -1;
    ensureAudioInitialized();
    NSNumber* key = @(audioId);
    AVAudioPlayer* previous = gAuPlayers[key];
    if (previous && previous != player) {
        [gAuBusy removeObject:previous];
        dispatch_async(audioQueue(), ^{ [previous stop]; });
    }
    [gAuSourceIds removeObject:key];
    player.delegate = gAuDelegate;
    if (audioMutedByEnvironment()) player.volume = 0;
    gAuPlayers[key] = player;
    gAuData[key] = data;
    return player.duration;
}

static void prepareAudioIdsAsync(NSArray<NSNumber*>* audioIds, NSString* url,
                                 NSData* data, uint64_t epoch) {
    for (NSNumber* key in audioIds) {
        int audioId = key.intValue;
        dispatch_async(audioLoadQueue(), ^{
            @autoreleasepool {
                dispatch_semaphore_wait(audioLoadSlots(), DISPATCH_TIME_FOREVER);
                AVAudioPlayer* player = prepareAudioPlayer(audioId, data);
                dispatch_semaphore_signal(audioLoadSlots());
                dispatch_async(dispatch_get_main_queue(), ^{
                    // A shutdown/reload or superseding request makes this completion stale.
                    NSString* pendingUrl = gAuPendingUrls[key];
                    if (epoch != gAuLoadEpoch || ![pendingUrl isEqualToString:url]) return;
                    [gAuPendingUrls removeObjectForKey:key];
                    double duration = installPreparedAudioPlayer(audioId, data, player);
                    callAudioDone(audioId, duration);
                });
            }
        });
    }
}

static double makeAudioPlayer(int audioId, NSData* data) {
    if (!data || data.length == 0) return -1;
    ensureAudioInitialized();

    AVAudioPlayer* player = nil;
    NSNumber* reuseKey = nil;
    for (NSNumber* key in gAuPlayers) {
        if ([gAuSourceIds containsObject:key]) continue;
        NSData* candidateData = gAuData[key];
        AVAudioPlayer* candidate = gAuPlayers[key];
        if (candidateData && candidateData.bytes == data.bytes && candidate
            && !candidate.playing && ![gAuBusy containsObject:candidate]) {
            player = candidate;
            reuseKey = key;
            break;
        }
    }

    if (player) {
        [gAuPlayers removeObjectForKey:reuseKey];
        [gAuData removeObjectForKey:reuseKey];
        player.currentTime = 0;
        player.volume = audioMutedByEnvironment() ? 0 : 1;
        player.numberOfLoops = 0;
        player.rate = 1;
    } else {
        NSError* error = nil;
        player = [[AVAudioPlayer alloc] initWithData:data error:&error];
        if (!player) {
            fprintf(stderr, "[audio/mac] #%d decode failed: %s\n", audioId,
                    error.localizedDescription.UTF8String ?: "?");
            return -1;
        }
        player.enableRate = YES;
        player.delegate = gAuDelegate;
        if (audioMutedByEnvironment()) player.volume = 0;
        [player prepareToPlay];
    }

    gAuPlayers[@(audioId)] = player;
    gAuData[@(audioId)] = data;
    return player.duration;
}

// __auLoadUrl(id, url) -> duration|-1
static JSValueRef cbAudioLoadUrl(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                 const JSValueRef args[], JSValueRef* exc) {
    if (argc < 2) return JSValueMakeNumber(ctx, -1);
    ensureAudioInitialized();
    int audioId = (int)JSValueToNumber(ctx, args[0], exc);
    NSString* url = jsToNSString(ctx, args[1]);
    NSData* data = gAuUrlCache[url];
    if (!data) {
        data = loadAsset(url.UTF8String);
        if (data) gAuUrlCache[url] = data;
    }
    return JSValueMakeNumber(ctx, makeAudioPlayer(audioId, data));
}

// __auLoadUrlAsync(id, url) -> undefined; completion is __auDone(id, duration|-1).
// The JS shim records the id before entering this callback, so failures can use the same async
// completion path as successful loads and never force a synchronous URL read on the frame thread.
static JSValueRef cbAudioLoadUrlAsync(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                      const JSValueRef args[], JSValueRef* exc) {
    if (argc < 2) return JSValueMakeUndefined(ctx);
    ensureAudioInitialized();
    int audioId = (int)JSValueToNumber(ctx, args[0], exc);
    NSString* url = [jsToNSString(ctx, args[1]) copy];
    if (audioId <= 0 || !url.length) {
        dispatch_async(dispatch_get_main_queue(), ^{ callAudioDone(audioId, -1); });
        return JSValueMakeUndefined(ctx);
    }

    NSNumber* key = @(audioId);
    gAuPendingUrls[key] = url;
    uint64_t epoch = gAuLoadEpoch;
    NSData* cached = gAuUrlCache[url];
    if (cached.length) {
        prepareAudioIdsAsync(@[key], url, cached, epoch);
        return JSValueMakeUndefined(ctx);
    }

    // Another id waiting for this URL will be picked up when the shared byte load completes.
    if ([gAuLoadingUrls containsObject:url]) return JSValueMakeUndefined(ctx);
    [gAuLoadingUrls addObject:url];
    dispatch_async(audioLoadQueue(), ^{
        @autoreleasepool {
            dispatch_semaphore_wait(audioLoadSlots(), DISPATCH_TIME_FOREVER);
            NSData* data = loadAsset(url.UTF8String);
            dispatch_semaphore_signal(audioLoadSlots());
            dispatch_async(dispatch_get_main_queue(), ^{
                if (epoch != gAuLoadEpoch) return;
                [gAuLoadingUrls removeObject:url];
                NSArray<NSNumber*>* ids = [gAuPendingUrls allKeysForObject:url];
                if (!data.length) {
                    for (NSNumber* pendingKey in ids) {
                        [gAuPendingUrls removeObjectForKey:pendingKey];
                        callAudioDone(pendingKey.intValue, -1);
                    }
                    return;
                }
                gAuUrlCache[url] = data;
                prepareAudioIdsAsync(ids, url, data, epoch);
            });
        }
    });
    return JSValueMakeUndefined(ctx);
}

// __auLoadData(id, ArrayBuffer|TypedArray) -> duration|-1
static JSValueRef cbAudioLoadData(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                  const JSValueRef args[], JSValueRef* exc) {
    if (argc < 2 || !JSValueIsObject(ctx, args[1])) return JSValueMakeNumber(ctx, -1);
    int audioId = (int)JSValueToNumber(ctx, args[0], exc);
    NSData* data = jsValueToData(ctx, args[1], exc);
    double duration = makeAudioPlayer(audioId, data);
    if (duration >= 0) [gAuSourceIds addObject:@(audioId)];
    return JSValueMakeNumber(ctx, duration);
}

// __auClone(newId, sourceId) -> duration|-1
static JSValueRef cbAudioClone(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
    if (argc < 2) return JSValueMakeNumber(ctx, -1);
    ensureAudioInitialized();
    int newId = (int)JSValueToNumber(ctx, args[0], exc);
    int sourceId = (int)JSValueToNumber(ctx, args[1], exc);
    return JSValueMakeNumber(ctx, makeAudioPlayer(newId, gAuData[@(sourceId)]));
}

// op: 0 play(loop, from), 1 pause, 2 stop, 3 volume, 4 rate, 5 seek,
//     6 free, 7 currentTime.
static JSValueRef cbAudioControl(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                 const JSValueRef args[], JSValueRef* exc) {
    if (argc < 2) return JSValueMakeNumber(ctx, 0);
    int audioId = (int)JSValueToNumber(ctx, args[0], exc);
    int op = (int)JSValueToNumber(ctx, args[1], exc);
    AVAudioPlayer* player = gAuPlayers ? gAuPlayers[@(audioId)] : nil;
    if (!player) return JSValueMakeNumber(ctx, 0);
    double v1 = argc > 2 ? JSValueToNumber(ctx, args[2], exc) : 0;
    double v2 = argc > 3 ? JSValueToNumber(ctx, args[3], exc) : 0;

    switch (op) {
        case 0: {
            [gAuBusy addObject:player];
            dispatch_async(audioQueue(), ^{
                player.numberOfLoops = v1 != 0 ? -1 : 0;
                if (v2 > 0) player.currentTime = v2;
                if (audioMutedByEnvironment()) player.volume = 0;
                [player play];
            });
            break;
        }
        case 1: {
            [gAuBusy removeObject:player];
            dispatch_async(audioQueue(), ^{ [player pause]; });
            break;
        }
        case 2: {
            [gAuBusy removeObject:player];
            dispatch_async(audioQueue(), ^{ [player pause]; player.currentTime = 0; });
            break;
        }
        case 3: {
            dispatch_async(audioQueue(), ^{ player.volume = audioMutedByEnvironment() ? 0 : (float)std::clamp(v1, 0.0, 1.0); });
            break;
        }
        case 4: {
            if (v1 >= 0.5 && v1 <= 2.0)
                dispatch_async(audioQueue(), ^{ player.rate = (float)v1; });
            break;
        }
        case 5: {
            dispatch_async(audioQueue(), ^{ player.currentTime = std::max(0.0, v1); });
            break;
        }
        case 6: {
            [gAuPlayers removeObjectForKey:@(audioId)];
            [gAuData removeObjectForKey:@(audioId)];
            [gAuSourceIds removeObject:@(audioId)];
            [gAuBusy removeObject:player];
            dispatch_async(audioQueue(), ^{ [player stop]; });
            break;
        }
        case 7: {
            return JSValueMakeNumber(ctx, player.currentTime);
        }
    }
    return JSValueMakeNumber(ctx, 0);
}

static void shutdownAudio() {
    if (!gAuPlayers) return;
    ++gAuLoadEpoch;
    NSArray<AVAudioPlayer*>* players = gAuPlayers.allValues;
    [gAuPlayers removeAllObjects];
    [gAuData removeAllObjects];
    [gAuSourceIds removeAllObjects];
    [gAuBusy removeAllObjects];
    [gAuUrlCache removeAllObjects];
    [gAuPendingUrls removeAllObjects];
    [gAuLoadingUrls removeAllObjects];
    dispatch_async(audioQueue(), ^{ for (AVAudioPlayer* player in players) [player stop]; });
}
// __decodeImage(arrayBuffer|typedArray) -> {width,height,data}：从内存字节解码图片（.zspine/zip 内 png 无源 URL）。
static JSValueRef cbDecodeImage(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeNull(ctx);
    NSData* d = jsValueToData(ctx, args[0], exc);
    JSValueRef r = decodeImageNSData(ctx, d, exc,argc>1&&JSValueToBoolean(ctx,args[1]));
    if (JSValueIsNull(ctx, r)) fprintf(stderr, "[game] __decodeImage 失败: %lu 字节\n", (unsigned long)d.length);
    return r;
}
// 未处理的 Promise rejection 回调（被吞的 async 错误）：JSC 调用时 args=(promise, reason)。
extern "C" void JSGlobalContextSetUnhandledRejectionCallback(JSGlobalContextRef, JSObjectRef, JSValueRef*);
// Reload UI + full runtime rebuild. Declared early so the JS host callbacks can
// dispatch onto the AppDelegate without pulling the whole Cocoa app class up here.
@interface AppDelegate : NSObject <NSApplicationDelegate>
- (void)showReloadTitle:(NSString*)title content:(NSString*)content;
- (void)hideReload;
- (void)reloadGame;
- (void)shutdownGameRuntime;
@end

static AppDelegate* gAppDelegate = nil;


static JSValueRef cbClipboardWrite(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                   const JSValueRef args[], JSValueRef*) {
    if (argc < 1) return JSValueMakeBoolean(ctx, false);
    NSString* text = jsToNSString(ctx, args[0]);
    NSPasteboard* pasteboard = NSPasteboard.generalPasteboard;
    [pasteboard clearContents];
    return JSValueMakeBoolean(ctx, [pasteboard setString:(text ?: @"") forType:NSPasteboardTypeString]);
}


static JSValueRef cbShowReload(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                               const JSValueRef args[], JSValueRef*) {
    NSString* title = argc > 0 ? jsToNSString(ctx, args[0]) : @"";
    NSString* content = argc > 1 ? jsToNSString(ctx, args[1]) : @"";
    dispatch_async(dispatch_get_main_queue(), ^{
        [gAppDelegate showReloadTitle:title content:content];
    });
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbHideReload(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    dispatch_async(dispatch_get_main_queue(), ^{ [gAppDelegate hideReload]; });
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbReload(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    // Defer off the current JS stack so we never release gCtx while JS is still running on it.
    dispatch_async(dispatch_get_main_queue(), ^{ [gAppDelegate reloadGame]; });
    return JSValueMakeUndefined(ctx);
}
// Non-blocking text editor used by the native dat.gui proxy. A sheet avoids a
// nested modal run loop re-entering JavaScript while a pointer callback is active.
static JSValueRef cbNativePrompt(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                 const JSValueRef args[], JSValueRef*) {
    NSString* title = argc > 0 ? jsToNSString(ctx, args[0]) : @"Edit value";
    NSString* value = argc > 1 ? jsToNSString(ctx, args[1]) : @"";
    JSObjectRef callback = nullptr;
    if (argc > 2 && JSValueIsObject(ctx, args[2])) {
        JSObjectRef candidate = JSValueToObject(ctx, args[2], nullptr);
        if (candidate && JSObjectIsFunction(ctx, candidate)) callback = candidate;
    }
    if (!callback) return JSValueMakeUndefined(ctx);
    JSValueProtect(ctx, callback);

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = title.length ? title : @"Edit value";
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 360, 26)];
    field.stringValue = value ?: @"";
    alert.accessoryView = field;
    [alert layout];

    void (^finish)(NSModalResponse) = ^(NSModalResponse response) {
        if (!gCtx || gHostShuttingDown.load(std::memory_order_acquire)) return;
        JSValueRef answer = JSValueMakeNull(gCtx);
        JSStringRef str = nullptr;
        if (response == NSAlertFirstButtonReturn) {
            str = JSStringCreateWithUTF8CString(field.stringValue.UTF8String ?: "");
            answer = JSValueMakeString(gCtx, str);
        }
        JSValueRef exc = nullptr;
        JSObjectCallAsFunction(gCtx, callback, nullptr, 1, &answer, &exc);
        if (str) JSStringRelease(str);
        JSValueUnprotect(gCtx, callback);
        if (exc) fprintf(stderr, "[native-gui] prompt callback failed: %s\n", jsToNSString(gCtx, exc).UTF8String);
    };
    if (gMainWindow) {
        [alert beginSheetModalForWindow:gMainWindow completionHandler:finish];
        [alert.window makeFirstResponder:field];
        [field selectText:nil];
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{ finish(NSAlertSecondButtonReturn); });
    }
    return JSValueMakeUndefined(ctx);
}

// this bridge only owns the native input method session and mirrors changes back to JS.
static void callKbJs(const char* name, JSValueRef arg) {
    // Hiding the active AppKit editor synchronously emits blur/end-editing notifications.
    // Quit has already stopped JS timers; do not let that notification create new JS work.
    if (!gCtx || gHostShuttingDown.load(std::memory_order_acquire)) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef key = JSStringCreateWithUTF8CString(name);
    JSValueRef value = JSObjectGetProperty(gCtx, global, key, nullptr);
    JSStringRelease(key);
    if (!JSValueIsObject(gCtx, value)) return;
    JSObjectRef fn = JSValueToObject(gCtx, value, nullptr);
    if (fn && JSObjectIsFunction(gCtx, fn))
        JSObjectCallAsFunction(gCtx, fn, nullptr, 1, &arg, nullptr);
}

@interface NativeTextInputBridge : NSObject <NSTextFieldDelegate>
- (void)attachToView:(NSView*)view;
- (void)showText:(NSString*)text maxLen:(NSInteger)maxLen secure:(BOOL)secure multiline:(BOOL)multiline
        fieldRect:(NSRect)fieldRect fontSize:(CGFloat)fontSize fontFamily:(NSString*)fontFamily
            color:(NSUInteger)color alignment:(NSTextAlignment)alignment padding:(NSEdgeInsets)padding;
- (void)hide;
- (void)keep;
- (void)setText:(NSString*)text;
@end

@implementation NativeTextInputBridge {
    NSTextField* _plainField;
    NSSecureTextField* _secureField;
    NSTextField* _activeField;
    NSInteger _maxLen;
    BOOL _multiline;
    BOOL _suppressEnd;
    BOOL _returnSent;
    NSView* _hostView;
}
- (void)restoreArrowCursor {
    // NSTextField installs an I-beam cursor rect while its field editor is active. Merely moving
    // first responder back to MetalView does not invalidate that rect until the mouse moves again,
    // leaving the pointer looking like text input after the editor has closed.
    NSWindow* window = _hostView.window;
    if (window) {
        if (_plainField) [window invalidateCursorRectsForView:_plainField];
        if (_secureField) [window invalidateCursorRectsForView:_secureField];
        [window invalidateCursorRectsForView:_hostView];
    }
    [gameCursor() set];
    // AppKit can update the cursor once more after controlTextDidEndEditing returns. Repeat on the
    // next main-loop turn, after the field editor has actually been detached from NSTextField.
    dispatch_async(dispatch_get_main_queue(), ^{
        if (window) [window invalidateCursorRectsForView:self->_hostView];
        [gameCursor() set];
    });
}
- (NSTextField*)makeField:(BOOL)secure {
    NSTextField* field = secure
        ? (NSTextField*)[[NSSecureTextField alloc] initWithFrame:NSMakeRect(1, 1, 4, 4)]
        : [[NSTextField alloc] initWithFrame:NSMakeRect(1, 1, 4, 4)];
    field.delegate = self;
    field.enabled = YES;
    field.editable = YES;
    field.selectable = YES;
    field.bordered = NO;
    field.bezeled = NO;
    field.drawsBackground = NO;
    field.focusRingType = NSFocusRingTypeNone;
    field.textColor = NSColor.clearColor;
    field.hidden = YES;
    return field;
}
- (void)attachToView:(NSView*)view {
    if (!view || _plainField) return;
    _hostView = view;
    _plainField = [self makeField:NO];
    _secureField = (NSSecureTextField*)[self makeField:YES];
    [view addSubview:_plainField positioned:NSWindowAbove relativeTo:nil];
    [view addSubview:_secureField positioned:NSWindowAbove relativeTo:nil];
}
- (void)showText:(NSString*)text maxLen:(NSInteger)maxLen secure:(BOOL)secure multiline:(BOOL)multiline
        fieldRect:(NSRect)fieldRect fontSize:(CGFloat)fontSize fontFamily:(NSString*)fontFamily
            color:(NSUInteger)color alignment:(NSTextAlignment)alignment padding:(NSEdgeInsets)padding {
    NSTextField* next = secure ? _secureField : _plainField;
    if (!next || !next.window) return;
    _maxLen = MAX(0, maxLen);
    _multiline = multiline;
    _returnSent = NO;
    if (_activeField && _activeField != next && _activeField.currentEditor) {
        NSTextField* previous = _activeField;
        _suppressEnd = YES;
        [_activeField.window makeFirstResponder:nil];
        _suppressEnd = NO;
        previous.hidden = YES;
    }
    _activeField = next;
    NSString* primaryFont = [[fontFamily componentsSeparatedByString:@","] firstObject] ?: @"";
    primaryFont = [primaryFont stringByTrimmingCharactersInSet:
        [NSCharacterSet characterSetWithCharactersInString:@" \'\""]];
    fontSize = std::max<CGFloat>(8, fontSize);
    NSFont* font = primaryFont.length ? [NSFont fontWithName:primaryFont size:fontSize] : nil;
    next.font = font ?: [NSFont systemFontOfSize:fontSize];
    next.alignment = alignment;
    next.stringValue = text ?: @"";
    CGFloat hostHeight = _hostView.bounds.size.height;
    CGFloat x = fieldRect.origin.x + padding.left;
    CGFloat y = hostHeight - NSMaxY(fieldRect) + padding.bottom;
    CGFloat width = std::max<CGFloat>(4, fieldRect.size.width - padding.left - padding.right);
    CGFloat height = std::max<CGFloat>(4, fieldRect.size.height - padding.top - padding.bottom);
    next.frame = NSMakeRect(x, y, width, height);
    next.hidden = NO;
    BOOL focused = [next.window makeFirstResponder:next];
    NSTextView* editor = (NSTextView*)next.currentEditor;
    if (editor) {
        editor.drawsBackground = NO;
        editor.font = next.font;
        editor.alignment = alignment;
        editor.textColor = NSColor.clearColor;
        editor.insertionPointColor = [NSColor colorWithCalibratedRed:((color >> 16) & 255) / 255.0
            green:((color >> 8) & 255) / 255.0 blue:(color & 255) / 255.0 alpha:1.0];
        [editor setSelectedRange:NSMakeRange(next.stringValue.length, 0)];
    }
    fprintf(stderr, "[kb/mac] show secure=%d multiline=%d max=%ld focused=%d editor=%d key=%d rect=%.0f,%.0f %.0fx%.0f\n",
            secure ? 1 : 0, multiline ? 1 : 0, (long)_maxLen,
            focused ? 1 : 0, editor ? 1 : 0, next.window.isKeyWindow ? 1 : 0,
            x, y, width, height);
}
- (void)hide {
    NSTextField* field = _activeField;
    if (!field) { [self restoreArrowCursor]; return; }
    // Hide before ending the field-editor session so its cursor rect stops participating
    // immediately; controlTextDidEndEditing will finish the JS-side blur notification.
    field.hidden = YES;
    if (field.currentEditor) [field.window makeFirstResponder:_hostView];
    else _activeField = nil;
    [self restoreArrowCursor];
}
- (void)keep {
    if (_activeField && !_activeField.currentEditor)
        [_activeField.window makeFirstResponder:_activeField];
}
- (void)setText:(NSString*)text {
    if (!_activeField) return;
    text = text ?: @"";
    if ([_activeField.stringValue isEqualToString:text]) return;
    _activeField.stringValue = text;
    NSTextView* editor = (NSTextView*)_activeField.currentEditor;
    if (editor) [editor setSelectedRange:NSMakeRange(text.length, 0)];
}
- (NSString*)limitedText:(NSString*)text {
    if (_maxLen <= 0 || text.length <= (NSUInteger)_maxLen) return text;
    NSUInteger end = (NSUInteger)_maxLen;
    NSRange sequence = [text rangeOfComposedCharacterSequenceAtIndex:end - 1];
    if (NSMaxRange(sequence) > end) end = sequence.location;
    return [text substringToIndex:end];
}
- (void)controlTextDidChange:(NSNotification*)notification {
    NSTextField* field = notification.object;
    if (field != _activeField) return;
    NSString* text = [self limitedText:field.stringValue ?: @""];
    if (![field.stringValue isEqualToString:text]) {
        field.stringValue = text;
        NSTextView* editor = (NSTextView*)field.currentEditor;
        if (editor) [editor setSelectedRange:NSMakeRange(text.length, 0)];
    }
    JSStringRef str = JSStringCreateWithUTF8CString(text.UTF8String ?: "");
    callKbJs("__kbInput", JSValueMakeString(gCtx, str));
    JSStringRelease(str);
    fprintf(stderr, "[kb/mac] changed len=%lu\n", (unsigned long)text.length);
}
- (void)controlTextDidEndEditing:(NSNotification*)notification {
    if (_suppressEnd || notification.object != _activeField) return;
    NSTextField* field = _activeField;
    _activeField = nil;
    field.hidden = YES;
    [self restoreArrowCursor];
    NSInteger movement = [notification.userInfo[NSTextMovementUserInfoKey] integerValue];
    BOOL isReturn = movement == NSReturnTextMovement;
    fprintf(stderr, "[kb/mac] end editing return=%d\n", isReturn ? 1 : 0);
    if (isReturn && !_returnSent) callKbJs("__kbDone", JSValueMakeNumber(gCtx, 1));
    else callKbJs("__kbDone", JSValueMakeNumber(gCtx, 0));
    _returnSent = NO;
}
- (BOOL)control:(NSControl*)control textView:(NSTextView*)textView doCommandBySelector:(SEL)selector {
    (void)textView;
    if (control != _activeField) return NO;
    if (selector == @selector(insertNewline:) || selector == @selector(insertLineBreak:)) {
        if (_multiline && selector == @selector(insertLineBreak:)) return NO;
        _returnSent = YES;
        callKbJs("__kbDone", JSValueMakeNumber(gCtx, 1));
        [self hide];
        return YES;
    }
    if (selector == @selector(cancelOperation:)) {
        [self hide];
        return YES;
    }
    return NO;
}
@end

static NativeTextInputBridge* gTextInput = nil;

static JSValueRef cbKbShow(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                           const JSValueRef args[], JSValueRef* exc) {
    NSString* text = argc > 0 ? jsToNSString(ctx, args[0]) : @"";
    NSInteger maxLen = argc > 1 ? (NSInteger)JSValueToNumber(ctx, args[1], exc) : 0;
    BOOL secure = argc > 2 && JSValueToNumber(ctx, args[2], exc) != 0;
    BOOL multiline = argc > 3 && JSValueToNumber(ctx, args[3], exc) != 0;
    NSRect fieldRect = NSMakeRect(argc > 4 ? JSValueToNumber(ctx, args[4], exc) : 1,
        argc > 5 ? JSValueToNumber(ctx, args[5], exc) : 1,
        argc > 6 ? JSValueToNumber(ctx, args[6], exc) : 4,
        argc > 7 ? JSValueToNumber(ctx, args[7], exc) : 4);
    CGFloat fontSize = argc > 8 ? JSValueToNumber(ctx, args[8], exc) : 20;
    NSString* fontFamily = argc > 9 ? jsToNSString(ctx, args[9]) : @"";
    NSUInteger color = argc > 10 ? (NSUInteger)JSValueToNumber(ctx, args[10], exc) : 0xffffff;
    NSInteger alignValue = argc > 11 ? (NSInteger)JSValueToNumber(ctx, args[11], exc) : 0;
    NSTextAlignment alignment = alignValue == 1 ? NSTextAlignmentCenter
        : (alignValue == 2 ? NSTextAlignmentRight : NSTextAlignmentLeft);
    NSEdgeInsets padding = NSEdgeInsetsMake(argc > 12 ? JSValueToNumber(ctx, args[12], exc) : 0,
        argc > 15 ? JSValueToNumber(ctx, args[15], exc) : 0,
        argc > 14 ? JSValueToNumber(ctx, args[14], exc) : 0,
        argc > 13 ? JSValueToNumber(ctx, args[13], exc) : 0);
    [gTextInput showText:text maxLen:maxLen secure:secure multiline:multiline fieldRect:fieldRect
        fontSize:fontSize fontFamily:fontFamily color:color alignment:alignment padding:padding];
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbKbHide(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t,
                           const JSValueRef[], JSValueRef*) {
    [gTextInput hide];
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbKbKeep(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t,
                           const JSValueRef[], JSValueRef*) {
    [gTextInput keep];
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbKbSetText(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                              const JSValueRef args[], JSValueRef*) {
    if (argc > 0) [gTextInput setText:jsToNSString(ctx, args[0])];
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbXhrSend(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
    const uint64_t requestEpoch = gRuntimeEpoch;
    if (argc < 3) return JSValueMakeUndefined(ctx);
    int reqId = (int)JSValueToNumber(ctx, args[0], exc);
    NSString* method = jsToNSString(ctx, args[1]);
    NSString* urlStr = jsToNSString(ctx, args[2]);   // JS 已 __resolveURL 成绝对 URL
    NSString* headersJson = (argc > 3) ? jsToNSString(ctx, args[3]) : @"{}";
    NSData* body = (argc > 4) ? jsValueToData(ctx, args[4], exc) : nil;
    // 本地优先：GET 请求若能在本地镜像命中（静态资源/js/lib），直接返回，不走网络。
    // 仅 GET —— API（login/lobby_batch 等 POST 到 192.168.8.112:9090）必须走网络（动态、且可能与本地 login/ 目录撞名）。
    if ([method caseInsensitiveCompare:@"GET"] == NSOrderedSame) {
        NSString* relative = relOf(urlStr);

        NSData* local = localAssetData(relative);

        if (!local) local = localAssetData(relative);
        if (local) {
            fprintf(stderr, "[xhr-local] #%d %lu B %.120s\n", reqId,
                    (unsigned long)local.length, relative.UTF8String);
            NSData* lb = local;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (requestEpoch == gRuntimeEpoch) callXhrDone(reqId, 200, @"OK", @"{}", lb);
            });
            return JSValueMakeUndefined(ctx);
        }
    }
    fprintf(stderr, "[xhr-net] #%d %s %.140s\n", reqId, method.UTF8String, urlStr.UTF8String);  // 走网络（本地未命中/POST）
    NSURL* url = [NSURL URLWithString:urlStr];
    if (!url) { dispatch_async(dispatch_get_main_queue(), ^{ callXhrDone(reqId, 0, @"", @"{}", nil); }); return JSValueMakeUndefined(ctx); }
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url];
    req.HTTPMethod = method.length ? method : @"GET";
    req.timeoutInterval = 30;
    NSDictionary* hdrs = [NSJSONSerialization JSONObjectWithData:[headersJson dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    if ([hdrs isKindOfClass:[NSDictionary class]]) for (id k in hdrs) [req setValue:[NSString stringWithFormat:@"%@", hdrs[k]] forHTTPHeaderField:[NSString stringWithFormat:@"%@", k]];
    if (body) req.HTTPBody = body;
    NSURLSessionDataTask* task = [[NSURLSession sharedSession] dataTaskWithRequest:req
        completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
            int status = 0; NSString* statusText = @""; NSString* rhJson = @"{}";
            if ([resp isKindOfClass:[NSHTTPURLResponse class]]) {
                NSHTTPURLResponse* hr = (NSHTTPURLResponse*)resp; status = (int)hr.statusCode;
                statusText = [NSHTTPURLResponse localizedStringForStatusCode:hr.statusCode] ?: @"";
                NSDictionary* hf = hr.allHeaderFields ?: @{};
                if ([NSJSONSerialization isValidJSONObject:hf]) { NSData* hd = [NSJSONSerialization dataWithJSONObject:hf options:0 error:nil];
                    if (hd) rhJson = [[NSString alloc] initWithData:hd encoding:NSUTF8StringEncoding] ?: @"{}"; }
            }
            if (err && status == 0) fprintf(stderr, "[xhr] %s %s err: %s\n", req.HTTPMethod.UTF8String, urlStr.UTF8String, err.localizedDescription.UTF8String);
            NSData* respBody = (status > 0) ? data : nil;
            fprintf(stderr, "[xhr-done] #%d status=%d bytes=%lu err=%s\n", reqId, status, (unsigned long)(data?data.length:0), err?err.localizedDescription.UTF8String:"none");
            dispatch_async(dispatch_get_main_queue(), ^{
                if (requestEpoch == gRuntimeEpoch) callXhrDone(reqId, status, statusText, rhJson, respBody);
            });
        }];
    [task resume];
    return JSValueMakeUndefined(ctx);
}


// eval game_host.js（其内部用 __evalScript 自加载 pixi/spine/bundle 并启动）
static JSValueRef cbSnapshot(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
static void injectRenderWorkBudget(JSGlobalContextRef ctx, JSObjectRef global) {
    // Same mechanism iOS ships with (value 24 there): popup/scene transitions mark hundreds of
    // Text/Graphics nodes dirty in one tick; without a budget the first present rasterises all of
    // them synchronously. Mac CPUs are faster, so a higher budget avoids visible progressive fill.
    JSStringRef key = JSStringCreateWithUTF8CString("__nativeRenderWorkBudget");
    JSObjectSetProperty(ctx, global, key, JSValueMakeNumber(ctx, 48), kJSPropertyAttributeReadOnly, nullptr);
    JSStringRelease(key);
}
static void injectScreenMetrics(JSGlobalContextRef ctx, JSObjectRef global) {
    // game_host.js reads these before installing window/innerWidth/innerHeight.  Previously macOS
    // never provided them, so the JS layout always used its 1590x960 fallback even when the actual
    // CAMetalLayer/backbuffer was 1280x720 (or GAME_W/GAME_H requested another size).  Full-screen
    // popups were consequently laid out 4:3-ish and clipped at the bottom, unlike the H5 client.
    // CoreText surfaces are independent textures. Keep the existing 2x quality floor on a 1x
    // display, while a Retina window follows the same backing scale as the main drawable.
    const double nativeTextRasterScale = std::max(
        2.0, gBackingScale);
    struct Metric { const char* name; double value; } metrics[] = {
        {"__screenW", static_cast<double>(W)},
        {"__screenH", static_cast<double>(H)},
        {"__screenDPR", gBackingScale},
        // Text supersampling does not change logical width, transforms, hit boxes or input space.
        {"__nativeTextRasterScale", nativeTextRasterScale},
    };
    for (const Metric& metric : metrics) {
        JSStringRef key = JSStringCreateWithUTF8CString(metric.name);
        JSObjectSetProperty(ctx, global, key, JSValueMakeNumber(ctx, metric.value),
                            kJSPropertyAttributeReadOnly, nullptr);
        JSStringRelease(key);
    }
}
static void injectNativePlatform(JSGlobalContextRef ctx, JSObjectRef global) {
 JSStringRef key=JSStringCreateWithUTF8CString("__nativePlatform"),value=JSStringCreateWithUTF8CString("mac");
 JSObjectSetProperty(ctx,global,key,JSValueMakeString(ctx,value),kJSPropertyAttributeReadOnly,nullptr);
 JSStringRelease(value);JSStringRelease(key);
}

static bool setupGame() {
    // A newly activated release receives one complete JS-context launch to prove healthy. If that

    // A worker from a replaced JSC context may still finish later. Bump the
    // generation before creating the new context so it cannot consume or
    // notify waiters belonging to this setup.
    ++gSpinePrepareEpoch;
    ++gRuntimeEpoch;
    gSpinePrepareInflight.clear();
    // 上下文换代:旧 JS 世界的在途 lease(activeUsers)随世界消亡,无人再 unpin——prepared
    // 三表整体清空,否则 pinned holder 永久不可逐出。数据丢失无碍:reload 罕见,重新 preparse。
    [gSpinePreparedCache removeAllObjects];
    [gSpinePreparedByHandle removeAllObjects];
    [gSpinePreparedLru removeAllObjects];
    // 逐个丢弃而不是直接 clear:未 commit 的 job 还占着已 retain 的页图纹理,
    // 直接析构只会释放 Atlas/SkeletonData,GPU 纹理引用会永久留下。
    for (auto &kv : gSpinexPrewarm) gSpinexRt.store.discardAsync(kv.second);
    gSpinexPrewarm.clear();
    gTimers = [[NSMutableDictionary alloc] init];
    gTimerFns = [[NSMutableDictionary alloc] init];
    gNativeText.clear();
    gNativeResources.reset();
    gNativeResources.configure(&gDec, [](NSString* url) -> NSData* {
        return loadAsset(url.UTF8String);
    });
    if (gSGBuf) gNativeFgui.reset(gSG);
    // A preparsed object owns a detached, one-shot blueprint. Never carry a consumed blueprint
    // into a replacement JSC/runtime generation; old worker callbacks are rejected by context.
    gFguiPreparseInflight.clear();
    gFguiPreparseCache=nil;
    gFguiPreparseLru=nil;
    gFguiXmlPrewarm=nil;
    gFguiPreparseStats={};
    gNativeTweens.clear();
    gCtx = JSGlobalContextCreate(nullptr);
    JSObjectRef global = JSContextGetGlobalObject(gCtx);

    injectNativePlatform(gCtx, global);


    injectNativeBootConfiguration(gCtx, global);
    injectScreenMetrics(gCtx, global);
    injectRenderWorkBudget(gCtx, global);
    // Yield only between complete Shell modules. This is not an 8ms hard limit: a single
    // module can take longer. GAME_JS_PRELUDE may set 0 for controlled legacy-path comparisons.
    {
        JSStringRef key = JSStringCreateWithUTF8CString("__SHELL_LOAD_BUDGET");
        JSObjectSetProperty(gCtx, global, key, JSValueMakeNumber(gCtx, 8), kJSPropertyAttributeNone, nullptr);
        JSStringRelease(key);
    }

    if (const char* testPopup = getenv("GAME_TEST_POPUP")) {       // perf 回归:自动开指定弹窗
        char code[256]; snprintf(code, sizeof code, "globalThis.__NATIVE_TEST_POPUP={name:'%.64s',delay:35000};", testPopup);
        JSStringRef s = JSStringCreateWithUTF8CString(code);
        JSEvaluateScript(gCtx, s, nullptr, nullptr, 0, nullptr);
        JSStringRelease(s);
    }
    if (getenv("GAME_TEST_SWIPE")) {                               // perf 回归:大厅列表自动滑动
        JSStringRef s = JSStringCreateWithUTF8CString("globalThis.__NATIVE_TEST_SWIPE={delay:30000,count:10};");
        JSEvaluateScript(gCtx, s, nullptr, nullptr, 0, nullptr);
        JSStringRelease(s);
    }
    if (getenv("GAME_SPINEX_CHECK")) {                             // spinex 对拍:host 按开关加载 native_spinex_check.js
        JSStringRef s = JSStringCreateWithUTF8CString("globalThis.__SPINEX_CHECK=1;");
        JSEvaluateScript(gCtx, s, nullptr, nullptr, 0, nullptr);
        JSStringRelease(s);
    }
    if (const char* spxFull = getenv("GAME_SPINEX_FULL")) {        // spine 全量 native 化开关(显式值透传):
        int lv = atoi(spxFull);                                    // 未设置=不注入,JS 侧默认 2(全收编,收官翻转);
        if (lv < 0) lv = 0; else if (lv > 2) lv = 2;               // 显式 0=关(回退旧 spine 路径)/1=无 clz 分流(P1b)/
        char buf[64];                                              // 2=全收编。非数字串 atoi→0=关(显式设置即降级意图)。
        snprintf(buf, sizeof(buf), "globalThis.__SPINE_NATIVE_FULL=%d;", lv);
        JSStringRef s = JSStringCreateWithUTF8CString(buf);
        JSEvaluateScript(gCtx, s, nullptr, nullptr, 0, nullptr);
        JSStringRelease(s);
    }
    JSValueRef exc = nullptr;
    JSObjectRef ab = JSObjectMakeArrayBufferWithBytesNoCopy(gCtx, gBuf, kBufWords*4, nullptr, nullptr, &exc);
    JSStringRef cmdName = JSStringCreateWithUTF8CString("__cmd");
    JSObjectSetProperty(gCtx, global, cmdName, ab, kJSPropertyAttributeNone, NULL); JSStringRelease(cmdName);
    if (!gSGBuf) { size_t n = (size_t)sg::SHARED_FLOATS; gSGBuf = (float*)calloc(n, sizeof(float)); gSG.init(gSGBuf); }
    else gSG.init(gSGBuf);
    if (!gNativeFguiWhiteTexture) {
        const uint8_t white[4] = {255,255,255,255}; GLint prior = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prior);
        glGenTextures(1, &gNativeFguiWhiteTexture); glBindTexture(GL_TEXTURE_2D, gNativeFguiWhiteTexture); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,white);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glBindTexture(GL_TEXTURE_2D,(GLuint)prior);
        gDec.registerExternalTexture(kNativeFguiWhiteCid,gNativeFguiWhiteTexture);
    }
    gNativeFgui.solidTextureId = (int)kNativeFguiWhiteCid;
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
            // Match PIXI.TextStyle::toFontString as consumed by NativeTextProxy. The CoreText
            // parser accepts the quoted family and normal variant/weight tokens verbatim.
            font=(attr("italic")=="true"?"italic":"normal")+std::string(" normal ")+
                (attr("bold")=="true"?"bold":"normal")+" "+std::to_string(size)+"px \""+face+"\"";
        }else{
            if(attr("italic")=="true")font+="italic ";if(attr("bold")=="true")font+="bold ";font+=std::to_string(size)+"px "+face;
        }
        const bool shrink=!facadeNatural&&desc.kind==nfgui::ObjectKind::Text&&attr("autoSize")=="shrink";
        double width=0,height=0,lineHeight=0;
        double wrap=nfgui::plainTextWrapWidth(facadeNatural,attr("singleLine")=="true",shrink,desc.width);
        double requestedLineHeight=0;
        // Shrink owns a natural PIXI.Text-like surface.  Baking the authored box into that texture
        // makes the later display scale shrink the empty box as well and breaks layoutAlign.
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
    hostCoreRegisterBindings(gCtx, global);   // 共享绑定表见 src/host/host_core.mm
    setFn(gCtx, global, "__setNativeCursor", cbSetNativeCursor);
    setFn(gCtx, global, "__snapshot", cbSnapshot);
    // Rich-text host/JS ABI. Version 2 guarantees that both V1/V2 metrics include contentWidth
    // and contentX separately from the raster surface. New JS must not silently run its facade on
    // an older host: doing so makes first-frame auto-fit consume the paragraph-box width.
    {JSStringRef k=JSStringCreateWithUTF8CString("__nativeRichTextMetricsVersion");JSObjectSetProperty(gCtx,global,k,JSValueMakeNumber(gCtx,4),kJSPropertyAttributeReadOnly|kJSPropertyAttributeDontDelete,nullptr);JSStringRelease(k);}
    setFn(gCtx, global, "__shadowFlag", cbShadowFlag);
    setFn(gCtx, global, "__nativePresent", cbNativePresent);
    setFn(gCtx, global, "__nativeBatchCoverage", cbNativeBatchCoverage);
    setFn(gCtx, global, "__nativeRenderTexturePreflight", cbNativeRenderTexturePreflight);
    setFn(gCtx, global, "__nativeRenderTexture", cbNativeRenderTexture);
    gSpinexRt.resetAll();              // spinex 同步清仓:旧 instId/sdHandle 全部失效(纹理 cid 对称 release)
    // (legacy __sp*/__skel* 蒙皮/骨骼/帧库桥组与 __spBuf/__skelBatchBuf 共享缓冲已清仓——
    //  spinex __spx* 桥组默认接管后 JS 侧零消费,2026-08-25)
    setFn(gCtx, global, "__zstdDecompress", cbZstdDecompress);
    setFn(gCtx, global, "__nativeFguiPreparse", cbNativeFguiPreparse);
    setFn(gCtx, global, "__nativeFguiPreparePackage", cbNativeFguiPreparePackage);
    setFn(gCtx, global, "__nativeOpenURL", cbNativeOpenURL);
    setFn(gCtx, global, "__auLoadUrl", cbAudioLoadUrl);
    setFn(gCtx, global, "__auLoadUrlAsync", cbAudioLoadUrlAsync);
    setFn(gCtx, global, "__auLoadData", cbAudioLoadData);
    setFn(gCtx, global, "__auClone", cbAudioClone);
    setFn(gCtx, global, "__auCtl", cbAudioControl);
    setFn(gCtx, global, "__kbShow", cbKbShow);
    setFn(gCtx, global, "__kbHide", cbKbHide);
    setFn(gCtx, global, "__kbKeep", cbKbKeep);
    setFn(gCtx, global, "__kbSetText", cbKbSetText);
    setFn(gCtx, global, "__clipboardWrite", cbClipboardWrite);
    {JSStringRef key=JSStringCreateWithUTF8CString("__nativeSupportsImageSourcePMA");JSObjectSetProperty(gCtx,global,key,JSValueMakeBoolean(gCtx,true),kJSPropertyAttributeReadOnly,nullptr);JSStringRelease(key);}
    setFn(gCtx, global, "__flush", cbFlush);
    setFn(gCtx, global, "__loadImage", cbLoadImage);
    setFn(gCtx, global, "__loadImageAsync", cbLoadImageAsync);
    setFn(gCtx, global, "__readFileAsync", cbReadFileAsync);
    setFn(gCtx, global, "__decodeScriptUTF8", native_script_utf8::decode);
    setFn(gCtx, global, "__nativeSpinePreparse", cbNativeSpinePreparse);
    setFn(gCtx, global, "__loadSpinePreparedImage", cbLoadSpinePreparedImage);
    setFn(gCtx, global, "__nativeSpineRecordFallback", cbNativeSpineRecordFallback);
    setFn(gCtx, global, "__zipParse", cbZipParse);
    setFn(gCtx, global, "__zipGet", cbZipGet);
    setFn(gCtx, global, "__decodeImage", cbDecodeImage);
    setFn(gCtx, global, "__xhrSend", cbXhrSend);

    setFn(gCtx, global, "__showReload", cbShowReload);
    setFn(gCtx, global, "__hideReload", cbHideReload);
    setFn(gCtx, global, "__reload", cbReload);
    setFn(gCtx, global, "__nativePrompt", cbNativePrompt);
    {   // 暴露被吞的 async 错误
        JSStringRef nm = JSStringCreateWithUTF8CString("__unhandledRejection");
        JSObjectRef fn = JSObjectMakeFunctionWithCallback(gCtx, nm, cbUnhandledRejection); JSStringRelease(nm);
        JSValueRef e2 = nullptr; JSGlobalContextSetUnhandledRejectionCallback(gCtx, fn, &e2);
    }
    c2d::registerCanvas2D(gCtx, global);
    NSString* fontsDir = pathUnderRoot(gGameDir, @"fonts");
    if ([[NSFileManager defaultManager] fileExistsAtPath:fontsDir]) {
        std::string fontError;
        if(!c2d::registerGameFonts(fontsDir.UTF8String,fontError)){
            fprintf(stderr,"[fonts] %s\n",fontError.c_str());return false;
        }
    }
    if (const char* fdir = getenv("PIXI_FONTS")) c2d::registerFonts(fdir);
#if !defined(PIXINATIVE_PRODUCTION)
    if (const char* prelude = getenv("GAME_JS_PRELUDE")) {
        JSStringRef ps = JSStringCreateWithUTF8CString(prelude);
        JSStringRef pu = JSStringCreateWithUTF8CString("GAME_JS_PRELUDE");
        exc = nullptr; JSEvaluateScript(gCtx, ps, nullptr, pu, 1, &exc);
        JSStringRelease(ps); JSStringRelease(pu);
        if (exc) {
            JSStringRef es = JSValueToStringCopy(gCtx, exc, nullptr);
            size_t n = JSStringGetMaximumUTF8CStringSize(es); std::vector<char> b(n);
            JSStringGetUTF8CString(es, b.data(), n); fprintf(stderr,"[game] GAME_JS_PRELUDE 异常: %s\n", b.data()); JSStringRelease(es);
            nativeLogError(@"javascript_boot", @"prelude_exception",
                                                [NSString stringWithUTF8String:b.data()] ?: @"", YES);
            return false;
        }
    }
#endif

    NSString* hostPath = pathUnderRoot(gGameDir, @"game_host.js");
    NSString* src = [NSString stringWithContentsOfFile:hostPath encoding:NSUTF8StringEncoding error:nil];
    if (!src) {
        fprintf(stderr,"[game] 读 game_host.js 失败: %s\n", hostPath.UTF8String);
        nativeLogError(@"javascript_boot", @"game_host_read_failed",
                                            hostPath ?: @"", YES);
        return false;
    }
    JSStringRef js = JSStringCreateWithUTF8CString([src UTF8String]);
    JSStringRef url = JSStringCreateWithUTF8CString("game_host.js");
    exc = nullptr; JSEvaluateScript(gCtx, js, nullptr, url, 1, &exc);
    JSStringRelease(js); JSStringRelease(url);
    if (exc) {
        JSStringRef es = JSValueToStringCopy(gCtx, exc, nullptr);
        size_t n = JSStringGetMaximumUTF8CStringSize(es); std::vector<char> b(n);
        JSStringGetUTF8CString(es, b.data(), n); fprintf(stderr,"[game] game_host 顶层异常: %s\n", b.data()); JSStringRelease(es);
        nativeLogError(@"javascript_boot", @"game_host_exception",
                                            [NSString stringWithUTF8String:b.data()] ?: @"", YES);
        return false;
    }
    return true;
}

void spinexFrameDrive(){
    double nowMs = nowUs() * 0.001;
    if (!gSpinexRt.instanceCount()) { gSpinexRt.noteIdleFrame(nowMs); return; }
    gSpinexRt.updateAll(nowMs, &gSG, gSGBuf, (float)W, (float)H);
}

static void mainDrawableTransform(float out[6]) {
    const float sx = W > 0 ? static_cast<float>(gDrawableW) / static_cast<float>(W) : 1.0f;
    const float sy = H > 0 ? static_cast<float>(gDrawableH) / static_cast<float>(H) : 1.0f;
    out[0] = sx; out[1] = 0.0f;
    out[2] = 0.0f; out[3] = sy;
    out[4] = 0.0f; out[5] = 0.0f;
}


// mode 2 主屏绘制:从 __nativePresent(cbShadowFlag)内联调用 —— 取"present 调用时刻"的场景状态,
// 与浏览器 renderer.render 语义对齐。renderShadow 里保留同一段作为兜底(正常帧 root 已被消费,不会重复画)。
static void flipMainScreenDraw(int root) {
    const int logicalW = W, logicalH = H;
    const int pixelW = gDrawableW, pixelH = gDrawableH;
    if (root <= 0 || logicalW <= 0 || logicalH <= 0 || pixelW <= 0 || pixelH <= 0 ||
        gContext == EGL_NO_CONTEXT) return;
    // Publish this render call's local transforms before any native animation runtime reads the
    // node's world transform. A node can be created, attached and positioned immediately before
    // renderer.render(); until compute() runs its WX/WY can still describe allocation time or the
    // previous parent used by an effect-pool checkout. Driving Spine from that stale transform
    // makes its first pose appear in the old symbol region before the next frame catches up.
    //
    // Do not precompute subtree bounds here: Batcher::frame owns the authoritative bounds pass
    // after animation emission. Repeating a full bounds walk before every frame doubles scene-walk
    // work on large machines and turns the correctness prepass into a frame-rate regression.
    // Phase D-1:动画推进已移至 ADVANCE_NATIVE(JS phase4 经 __sgAdvanceAnimations);
    // 兜底:旧 JS/其他 draw 路径未推进时按原逻辑就地推进(过渡期兼容,JS 全量升级后可删)。
    if (!gAnimAdvancedThisFrame) advanceNativeAnimationsForFrame(root);
    gAnimAdvancedThisFrame = false;   // 消费帧标志
    double shadowPart = nowUs();
    if (!gShadowBatcher.ready()) {
        gShadowBatcher.texLookup = [](int id){ return gDec.glTextureEnsured((uint32_t)id); };   // 画前补传节流队列中的同名纹理,防首帧采样空纹理
        gShadowBatcher.init();
        gShadowBatcher.preparePostProcesses(512); // loading scene prewarm: no first-popup Twist shader/FBO stall
    }
    double shadowBindStart = nowUs();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    glViewport(0, 0, pixelW, pixelH);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(gSgClear[0], gSgClear[1],
                 gSgClear[2], gSgClear[3]);
    gFrameTiming.shadowBindUs += nowUs() - shadowBindStart;
#if !defined(PIXINATIVE_PRODUCTION)
    if (gTestGlClearProbe) {
        const double preClearFinishStart = nowUs();
        glFinish();
        gFrameTiming.preClearFinishUs += nowUs() - preClearFinishStart;
    }
#endif
    double shadowClearStart = nowUs();
    glClear(GL_COLOR_BUFFER_BIT);
    gFrameTiming.shadowClearUs += nowUs() - shadowClearStart;
    gFrameTiming.shadowSetupUs += nowUs() - shadowPart;
    shadowPart = nowUs();
    // Phase D-1:主路径的最终收敛由 JS 两段式(FINAL_COMPUTE)完成,draw 零 compute;
    // 仅兜底(本帧动画在 draw 内就地推进过)时补一次收敛。
    if (gSG.buf && gSG.buf[sg::DEBT_BASE + root] > 0.5f) gSG.compute(root, sg::CM_CANONICAL_MAIN);
    gFrameTiming.shadowComputeUs += nowUs() - shadowPart;
    gShadowBatcher.frameReset();
    shadowPart = nowUs();
    // Batcher geometry is authored in logical Pixi coordinates. Scale it into the physical
    // framebuffer before projection; this also promotes scissor masks and post-process targets to
    // backing pixels instead of stretching a completed 1x frame in the compositor.
    float output[6]; mainDrawableTransform(output);
    gShadowBatcher.frame(&gSG, root, (float)pixelW, (float)pixelH, output);
    ++gNativeBatchSerial;
    gFrameTiming.shadowBatchUs += nowUs() - shadowPart;
    shadowPart = nowUs();
    static int fdt = 0;
    static const bool reportFlip = getenv("GAME_PROF") != nullptr;
    if (reportFlip && ++fdt >= 120) {
        fdt = 0;
        fprintf(stderr, "[flip-native/mac] quads=%u draws=%u 逃逸=%u(遮罩%u 逃生舱%u 无纹理%u) cull=%u\n",
                gShadowBatcher.quads, gShadowBatcher.drawCalls, gShadowBatcher.escapes,
                gShadowBatcher.escMask, gShadowBatcher.escType, gShadowBatcher.escTex,
                gShadowBatcher.culled);
    }
    gFrameTiming.shadowFinishUs += nowUs() - shadowPart;
}

static void renderShadow() {
    if (gShadowRoot <= 0 || gContext == EGL_NO_CONTEXT) return;
    int root = gShadowRoot; gShadowRoot = 0;
    int w = W, h = H;
    if (w <= 0 || h <= 0) return;

    if (gShadowShow == 2) {
        // 兜底:正常帧 cbShadowFlag 已内联画完并清掉 gShadowRoot,走不到这里;
        // 只有 present 时被遮挡等特殊情形才会残留 root(drawOneFrame 的 !canPresent 分支也会清)。
        flipMainScreenDraw(root);
        return;
    }

    if (!gShadowFbo || gShadowFboW != w || gShadowFboH != h) {
        if (gShadowTex) { glDeleteTextures(1, &gShadowTex); gShadowTex = 0; }
        if (gShadowFbo) { glDeleteFramebuffers(1, &gShadowFbo); gShadowFbo = 0; }
        GLint saveTex = 0, saveFbo = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &saveTex);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saveFbo);
        glGenTextures(1, &gShadowTex);
        glBindTexture(GL_TEXTURE_2D, gShadowTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &gShadowFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, gShadowFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gShadowTex, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "[shadow/mac] FBO 不完整 0x%x\n", (unsigned)st);
        gShadowFboW = w; gShadowFboH = h;
        glBindFramebuffer(GL_FRAMEBUFFER, saveFbo > 0 ? (GLuint)saveFbo : 0);
        glBindTexture(GL_TEXTURE_2D, saveTex > 0 ? (GLuint)saveTex : 0);
        fprintf(stderr, "[shadow/mac] FBO 建 %dx%d tex=%u fbo=%u\n", w, h, gShadowTex, gShadowFbo);
    }
    if (!gShadowBatcher.ready()) {
        gShadowBatcher.texLookup = [](int id){ return gDec.glTextureEnsured((uint32_t)id); };   // 画前补传节流队列中的同名纹理,防首帧采样空纹理
        gShadowBatcher.init();
    }

    GLint sProg=0,sArr=0,sElem=0,sVao=0,sFbo=0,sActive=0,sTex0=0,sVp[4]={0,0,0,0},sScis[4]={0,0,0,0};
    GLint sSrcRGB=0,sDstRGB=0,sSrcA=0,sDstA=0;
    GLboolean sBlend=glIsEnabled(GL_BLEND), sScisEn=glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM,&sProg); glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&sArr);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&sElem); glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&sVao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&sFbo); glGetIntegerv(GL_ACTIVE_TEXTURE,&sActive);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&sTex0); glGetIntegerv(GL_VIEWPORT,sVp); glGetIntegerv(GL_SCISSOR_BOX,sScis);
    glGetIntegerv(GL_BLEND_SRC_RGB,&sSrcRGB); glGetIntegerv(GL_BLEND_DST_RGB,&sDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&sSrcA); glGetIntegerv(GL_BLEND_DST_ALPHA,&sDstA);

    glBindFramebuffer(GL_FRAMEBUFFER, gShadowFbo);
    glBindVertexArray(0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    gSG.compute(root, sg::CM_RENDER_TEXTURE);   // B0-2:离屏对比根,不 ack 主舞台债务
    gShadowBatcher.frameReset();
    gShadowBatcher.frame(&gSG, root, (float)w, (float)h);

    glBindFramebuffer(GL_FRAMEBUFFER, sFbo > 0 ? (GLuint)sFbo : 0);
    glUseProgram(sProg > 0 ? (GLuint)sProg : 0);
    glBindBuffer(GL_ARRAY_BUFFER, sArr > 0 ? (GLuint)sArr : 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sElem > 0 ? (GLuint)sElem : 0);
    glBindVertexArray(sVao > 0 ? (GLuint)sVao : 0);
    glActiveTexture((GLenum)(sActive ? sActive : GL_TEXTURE0));
    glBindTexture(GL_TEXTURE_2D, sTex0 > 0 ? (GLuint)sTex0 : 0);
    glViewport(sVp[0], sVp[1], sVp[2], sVp[3]);
    glScissor(sScis[0], sScis[1], sScis[2], sScis[3]);
    if (sScisEn) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (sBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFuncSeparate((GLenum)sSrcRGB, (GLenum)sDstRGB, (GLenum)sSrcA, (GLenum)sDstA);

    if (gShadowShow) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gShadowFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, w, h, 0, 0, gDrawableW, gDrawableH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

// 调试：把当前后缓冲 readPixels 出来写 PNG（headless 验证渲染输出）
static void dumpBackbuffer(const char* path) {
    int w = gDrawableW, h = gDrawableH;
    std::vector<uint8_t> px((size_t)w*h*4), flip((size_t)w*h*4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y=0; y<h; ++y) memcpy(&flip[(size_t)y*w*4], &px[(size_t)(h-1-y)*w*4], (size_t)w*4);
    NSBitmapImageRep* rep=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bytesPerRow:w*4 bitsPerPixel:32];
    memcpy([rep bitmapData], flip.data(), (size_t)w*h*4);
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
        writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
    fprintf(stderr, "[game] dumped backbuffer -> %s\n", path);
}

// __snapshot(quality) -> "data:image/jpeg;base64,...":整屏截图(GL 后备缓冲,行序翻转),与 iOS 同契约。
// 替代 pixi extract 全屏截图链(PixiUtils.screenShot 发邮件等用途)。
static JSValueRef cbSnapshot(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    double q = argc>0 ? JSValueToNumber(ctx, a[0], e) : 0.85;
    if (!(q > 0 && q <= 1)) q = 0.85;
    int w = gDrawableW, h = gDrawableH;
    if (w<=0 || h<=0) return JSValueMakeNull(ctx);
    GLint prevFbo=0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::vector<uint8_t> px((size_t)w*h*4), flip((size_t)w*h*4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    for (int yy=0; yy<h; ++yy) memcpy(&flip[(size_t)yy*w*4], &px[(size_t)(h-1-yy)*w*4], (size_t)w*4);
    NSBitmapImageRep* rep=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bytesPerRow:w*4 bitsPerPixel:32];
    memcpy([rep bitmapData], flip.data(), flip.size());
    NSData* d=[rep representationUsingType:NSBitmapImageFileTypeJPEG properties:@{NSImageCompressionFactor:@(q)}];
    if (!d.length) return JSValueMakeNull(ctx);
    NSString* url=[NSString stringWithFormat:@"data:image/jpeg;base64,%@", [d base64EncodedStringWithOptions:0]];
    JSStringRef s=JSStringCreateWithUTF8CString(url.UTF8String);
    JSValueRef ret=JSValueMakeString(ctx,s);JSStringRelease(s);
    return ret;
}

static bool dumpTextureLevel0(GLuint texture,int width,int height,const char* path){
    if(!texture||width<=0||height<=0||!path)return false;GLint previousFbo=0,previousTexture=0;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&previousFbo);glGetIntegerv(GL_TEXTURE_BINDING_2D,&previousTexture);
    GLuint fbo=0;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    bool complete=glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;if(complete){
        std::vector<uint8_t> pixels((size_t)width*height*4),flipped((size_t)width*height*4);glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
        for(int y=0;y<height;++y)std::memcpy(flipped.data()+(size_t)y*width*4,pixels.data()+(size_t)(height-1-y)*width*4,(size_t)width*4);
        NSBitmapImageRep* rep=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:width*4 bitsPerPixel:32];
        std::memcpy([rep bitmapData],flipped.data(),flipped.size());[[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
        std::fprintf(stderr,"[native-atlas-dump] texture=%u size=%dx%d path=%s\n",(unsigned)texture,width,height,path);
    }
    if(!complete)std::fprintf(stderr,"[native-atlas-dump] direct attachment incomplete texture=%u status=0x%x\n",(unsigned)texture,(unsigned)glCheckFramebufferStatus(GL_FRAMEBUFFER));
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)previousFbo);glBindTexture(GL_TEXTURE_2D,(GLuint)previousTexture);if(fbo)glDeleteFramebuffers(1,&fbo);return complete;
}

// ASTC textures are sampleable but not color-renderable, so attaching their level zero to an FBO
// is expected to fail.  For diagnostics, sample the source through a tiny shader into an RGBA8
// target, then read that target.  This path is reached only with NATIVE_ATLAS_DUMP and has no
// production-frame cost.
static bool dumpTextureSampled(GLuint texture,int width,int height,const char* path){
    if(!texture||width<=0||height<=0||!path)return false;
    GLint previousFbo=0,previousProgram=0,previousArray=0,previousTexture=0,previousActive=0,previousViewport[4]={0,0,0,0};
    GLboolean previousBlend=glIsEnabled(GL_BLEND),previousScissor=glIsEnabled(GL_SCISSOR_TEST),previousStencil=glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&previousFbo);glGetIntegerv(GL_CURRENT_PROGRAM,&previousProgram);glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&previousArray);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&previousActive);glGetIntegerv(GL_TEXTURE_BINDING_2D,&previousTexture);glGetIntegerv(GL_VIEWPORT,previousViewport);
    const char* vertexSource="attribute vec2 p;attribute vec2 t;varying vec2 uv;void main(){gl_Position=vec4(p,0.0,1.0);uv=t;}";
    const char* fragmentSource="precision highp float;varying vec2 uv;uniform sampler2D image;void main(){gl_FragColor=texture2D(image,uv);}";
    auto compile=[](GLenum kind,const char* source){GLuint shader=glCreateShader(kind);glShaderSource(shader,1,&source,nullptr);glCompileShader(shader);GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);if(!ok){char log[512]={0};glGetShaderInfoLog(shader,sizeof(log),nullptr,log);std::fprintf(stderr,"[native-atlas-dump] shader compile failed: %s\n",log);glDeleteShader(shader);return (GLuint)0;}return shader;};
    GLuint vs=compile(GL_VERTEX_SHADER,vertexSource),fs=compile(GL_FRAGMENT_SHADER,fragmentSource),program=0,vbo=0,target=0,fbo=0;bool ok=false;
    if(vs&&fs){program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glBindAttribLocation(program,0,"p");glBindAttribLocation(program,1,"t");glLinkProgram(program);GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);if(linked){
        // Triangle strip order is bottom-left,bottom-right,top-left,top-right. Map the PNG's top
        // row to source v=0 so exported coordinates match FairyGUI/Pixi atlas-frame coordinates.
        const float vertices[16]={-1,-1,0,1, 1,-1,1,1, -1,1,0,0, 1,1,1,0};
        glGenTextures(1,&target);glBindTexture(GL_TEXTURE_2D,target);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE){glViewport(0,0,width,height);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glDisable(GL_STENCIL_TEST);glUseProgram(program);glUniform1i(glGetUniformLocation(program,"image"),0);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STATIC_DRAW);glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,16,(void*)0);glEnableVertexAttribArray(1);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,16,(void*)8);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glFinish();
            std::vector<uint8_t> pixels((size_t)width*height*4),flipped((size_t)width*height*4);glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());for(int y=0;y<height;++y)std::memcpy(flipped.data()+(size_t)y*width*4,pixels.data()+(size_t)(height-1-y)*width*4,(size_t)width*4);
            NSBitmapImageRep* rep=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:width*4 bitsPerPixel:32];std::memcpy([rep bitmapData],flipped.data(),flipped.size());ok=[[[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] copy] writeToFile:[NSString stringWithUTF8String:path] atomically:YES];if(ok)std::fprintf(stderr,"[native-atlas-dump] sampled texture=%u size=%dx%d path=%s\n",(unsigned)texture,width,height,path);
        }
    }}
    if(vbo)glDeleteBuffers(1,&vbo);if(fbo)glDeleteFramebuffers(1,&fbo);if(target)glDeleteTextures(1,&target);if(program)glDeleteProgram(program);if(vs)glDeleteShader(vs);if(fs)glDeleteShader(fs);
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)previousFbo);glUseProgram((GLuint)previousProgram);glBindBuffer(GL_ARRAY_BUFFER,(GLuint)previousArray);glActiveTexture((GLenum)previousActive);glBindTexture(GL_TEXTURE_2D,(GLuint)previousTexture);glViewport(previousViewport[0],previousViewport[1],previousViewport[2],previousViewport[3]);if(previousBlend)glEnable(GL_BLEND);else glDisable(GL_BLEND);if(previousScissor)glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);if(previousStencil)glEnable(GL_STENCIL_TEST);else glDisable(GL_STENCIL_TEST);return ok;
}

// [tex-probe] 黑图仲裁:GAME_TEXPROBE=WxH 锁定的纹理,每 120 帧 FBO 附着 + glReadPixels 采 4 个点,
// 回答"GPU 上这张纹理到底有没有像素数据"——没数据是上传链的病,有数据是采样/绘制侧的病。
static void texReadbackProbe() {
    if (!gDec.probeTex_) return;
    static int fc = 0;
    if ((++fc % 120) != 1) return;
    // 采样器状态:MIN_FILTER 是 mipmap 模式(0x2700-0x2703)而 mip 链缺失 → GLES 采样 (0,0,0,1) 纯黑,
    // 但 level0 回读仍有数据 —— 正是"数据在,画出来黑"的裁决关键。
    GLint saveTex = 0, minf = 0, magf = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saveTex);
    glBindTexture(GL_TEXTURE_2D, gDec.probeTex_);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magf);
    glBindTexture(GL_TEXTURE_2D, (GLuint)saveTex);
    fprintf(stderr, "[tex-probe] glTex=%u min=0x%x mag=0x%x%s\n", (unsigned)gDec.probeTex_,
            (unsigned)minf, (unsigned)magf,
            (minf >= 0x2700 && minf <= 0x2703) ? " (mipmap采样模式!)" : "");
    GLint saveFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saveFbo);
    GLuint fbo = 0; glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gDec.probeTex_, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[tex-probe] glTex=%u FBO 不完整 status=0x%x(纹理未分配/格式不可附着)\n",
                (unsigned)gDec.probeTex_, (unsigned)st);
    } else {
        int w = gDec.probeW_, h = gDec.probeH_;
        int pts[4][2] = { {w/8, h/8}, {w/2, h/2}, {w*3/4, h/4}, {w/4, h*3/4} };
        uint8_t px[4][4]; int nz = 0;
        for (int i = 0; i < 4; i++) {
            glReadPixels(pts[i][0], pts[i][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[i]);
            if (px[i][0] | px[i][1] | px[i][2] | px[i][3]) nz++;
        }
        fprintf(stderr, "[tex-probe] glTex=%u %dx%d 非零点=%d/4 RGBA=(%u,%u,%u,%u)(%u,%u,%u,%u)(%u,%u,%u,%u)(%u,%u,%u,%u)\n",
                (unsigned)gDec.probeTex_, w, h, nz,
                px[0][0],px[0][1],px[0][2],px[0][3], px[1][0],px[1][1],px[1][2],px[1][3],
                px[2][0],px[2][1],px[2][2],px[2][3], px[3][0],px[3][1],px[3][2],px[3][3]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saveFbo);
    glDeleteFramebuffers(1, &fbo);
}

// 每帧调 JS globalThis.__tick(tMs) 驱动 pixi ticker（执行 pending raf 回调 + flushPending 发命令）。
static void drawOneFrame(bool canPresent) {
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef fn = JSStringCreateWithUTF8CString("__tick");
    JSValueRef fv = JSObjectGetProperty(gCtx, global, fn, nullptr); JSStringRelease(fn);
    if (!JSValueIsObject(gCtx, fv)) return;
    JSObjectRef tickFn = JSValueToObject(gCtx, fv, nullptr);
    if (!tickFn || !JSObjectIsFunction(gCtx, tickFn)) return;
    gFrameDecodeUs=0; gFrameTiming=FrameTiming{}; gNativeScreenPresented=false;
    gDec.drawCalls = 0; gDec.texUploads = 0; gDec.texDeferred = 0; gDec.bufUploads = 0;
    double phaseStart=nowUs();
    // Finish a bounded amount of Native ResourceManager GPU work before JS runs.
    // Large atlases are split into row bands and become visible only after the
    // complete texture is resident, so no partially-uploaded frame can flash.
    gNativeResources.beginFrame();
    gDec.beginFrame();
    if(std::getenv("NATIVE_ATLAS_TRACE")&&!gNativeAtlasTracePending.empty()){
        using GetTexLevelParameterivFn=void(*)(GLenum,GLint,GLenum,GLint*);
        static GetTexLevelParameterivFn getTexLevelParameteriv=(GetTexLevelParameterivFn)eglGetProcAddress("glGetTexLevelParameteriv");
        GLint previousTexture=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&previousTexture);
        for(auto it=gNativeAtlasTracePending.begin();it!=gNativeAtlasTracePending.end();){
            GLuint texture=gDec.glTexture((uint32_t)std::max(0,it->cid));GLint actualWidth=0,actualHeight=0;
            if(texture&&getTexLevelParameteriv){glBindTexture(GL_TEXTURE_2D,texture);getTexLevelParameteriv(GL_TEXTURE_2D,0,(GLenum)0x1000,&actualWidth);getTexLevelParameteriv(GL_TEXTURE_2D,0,(GLenum)0x1001,&actualHeight);}

            if((actualWidth>0&&actualHeight>0)||++it->attempts>=120){
                std::fprintf(stderr,"[native-atlas-ready] package=%s atlas=%s cid=%d gl=%u reported=%.0fx%.0f actual=%dx%d attempts=%d\n",it->packageName.c_str(),it->atlasId.c_str(),it->cid,(unsigned)texture,it->reportedWidth,it->reportedHeight,actualWidth,actualHeight,it->attempts);it=gNativeAtlasTracePending.erase(it);
            }else ++it;
        }
        glBindTexture(GL_TEXTURE_2D,(GLuint)previousTexture);
    }
    gFrameTiming.uploadUs=nowUs()-phaseStart;
    // Rebuild at most one inactive FGUI reservation per actually presented frame. This happens
    // before JS business code runs, but a reservation stamped with the current presentation serial
    // cannot be claimed until a later frame, so close/reopen in one tick never sees a half-settled
    // component. No JavaScript timer or display-tree traversal participates in this maintenance.
    if (gSG.buf) gSG.buf[sg::FRAME_PHASE_BASE] = 1.0f;   // B0-1:阶段1=C++ 前段(JS 之前的 transition/refill 推进)
    gNativeFgui.advanceReservationRefills((nowUs()-gStartUs)/1000.0,gNativeBatchSerial,gSG);
    // FGUI transitions are scheduled and applied entirely in C++ before JavaScript runs.
    gNativeFgui.advanceTransitions((nowUs()-gStartUs)/1000.0, gSG);
    gNativeFgui.advanceTasks((nowUs()-gStartUs)/1000.0);
    // A naturally completed transition is removed from the C++ active set above. Deliver its
    // token before business code gets a chance to stop/replay/dispose the JS facade; otherwise
    // stop() can erase the callback record and the later prerender drain sees an orphan token.
    // This also keeps the JS property/visibility shadows current at the browser-equivalent
    // boundary: transition update first, application ticker second.
    JSValueRef transitionNow = JSValueMakeNumber(gCtx, (nowUs()-gStartUs)/1000.0);
    callJsGlobal("__nativeFguiTransitionFrame", 1, &transitionNow);
    if (gSG.buf) gSG.buf[sg::FRAME_PHASE_BASE] = 2.0f;   // B0-1:阶段2=JS tick(业务/GSAP;细分阶段由 JS 内部推进)
    JSValueRef arg = JSValueMakeNumber(gCtx, (nowUs()-gStartUs)/1000.0);
    JSValueRef exc = nullptr;
    const double inlinePresentBeforeTickUs = gFrameTiming.inlinePresentUs;
    phaseStart=nowUs();
    JSObjectCallAsFunction(gCtx, tickFn, nullptr, 1, &arg, &exc);
    const double tickWallUs = nowUs()-phaseStart;
    const double tickInlinePresentUs = std::max(0.0, gFrameTiming.inlinePresentUs-inlinePresentBeforeTickUs);
    gFrameTiming.tickUs=std::max(0.0, tickWallUs-tickInlinePresentUs);
    if (exc) {
        JSStringRef es = JSValueToStringCopy(gCtx, exc, nullptr);
        size_t n = JSStringGetMaximumUTF8CStringSize(es); std::vector<char> b(n);
        JSStringGetUTF8CString(es, b.data(), n); fprintf(stderr,"[game] __tick 异常: %s\n", b.data()); JSStringRelease(es);
        nativeLogError(@"javascript_runtime", @"tick_exception",
                                            [NSString stringWithUTF8String:b.data()] ?: @"", NO);
    }
    // A fully occluded CAMetalLayer cannot supply a drawable. Its default behavior is to make the
    // first framebuffer write wait up to one second. Keep JS/native scene state advancing, but do
    // not touch the default framebuffer until AppKit says at least part of the window is visible.
    if (!canPresent) {
        gPendingRoot = 0;
        gShadowRoot = 0;
        return;
    }
    phaseStart=nowUs();
    if (gPendingRoot > 0) {
        int root = gPendingRoot; gPendingRoot = 0;
        double partStart=nowUs();
        gSG.compute(root, sg::CM_CANONICAL_MAIN);   // B0-2:主渲染路径 canonical
        gFrameTiming.computeUs+=nowUs()-partStart;
        if (!gBatcher.ready()) {
            gBatcher.texLookup = [](int id){ return gDec.glTextureEnsured((uint32_t)id); };   // 画前补传节流队列中的同名纹理,防首帧采样空纹理
            gBatcher.init();
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, gDrawableW, gDrawableH);
        glClearColor(gSgClear[0], gSgClear[1],
                 gSgClear[2], gSgClear[3]);
        glDisable(GL_SCISSOR_TEST);
        glClear(GL_COLOR_BUFFER_BIT);
        partStart=nowUs();
        float output[6]; mainDrawableTransform(output);
        gBatcher.frame(&gSG, root, (float)gDrawableW, (float)gDrawableH, output);
        gFrameTiming.batchUs+=nowUs()-partStart;
        gDec.drawCalls += gBatcher.drawCalls;
    }
    double shadowStart=nowUs();
    // Diagnostic-only offscreen comparison. Production __nativePresent already drew synchronously
    // and never leaves a root for this phase.
    renderShadow();
    gFrameTiming.shadowUs=nowUs()-shadowStart;
    texReadbackProbe();   // [tex-probe] GAME_TEXPROBE 设置时定期回读锁定纹理(默认关,零开销)
    // Normal production rendering happened synchronously in __nativePresent during __tick; the
    // tail still owns legacy/deferred shadow work. Combine both without charging inline work twice.
    gFrameTiming.renderUs=gFrameTiming.inlinePresentUs+(nowUs()-phaseStart);
    phaseStart=nowUs();
    glFlush();                           // EGL/Metal 异步提交；生命周期边界仍用 glFinish
    gFrameTiming.submitUs=nowUs()-phaseStart;
    gFrameTiming.decodeUs=gFrameDecodeUs;
    // GAME_DUMP：在 swap 前 readPixels 后缓冲（双缓冲下 swap 后读会读到空 back buffer → 全白）。
    // fc 只数实际绘制帧,会落后 _frame(启动/重载跳帧);≥ + 闩锁保证必 dump,退出闸门看 gDumpDone。
    { static const char* dp = getenv("GAME_DUMP");
      static int at = getenv("GAME_DUMP_AT") ? atoi(getenv("GAME_DUMP_AT")) : 240;
      static int fc = 0; if (dp && !gDumpDone && ++fc >= at) { gDumpDone = true; dumpBackbuffer(dp); } }
    phaseStart=nowUs();
    if (!eglSwapBuffers(gDisplay, gSurface)) {
        EGLint err = eglGetError();
        fprintf(stderr, "[lifecycle] eglSwapBuffers 失败 err=0x%x\n", err);
        gSurfaceNeedsRestore = true;
    }
    gFrameTiming.swapUs=nowUs()-phaseStart;
}


// Trackpad and mouse-wheel input does not enter AppKit's mouseDragged path. Forward it as a
// browser WheelEvent so FairyGUI's DOMEventManager can route it to the ScrollPane under the cursor.
static void emitWheel(double x, double y, double deltaX, double deltaY) {
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef fnName = JSStringCreateWithUTF8CString("__wheelEvent");
    JSValueRef value = JSObjectGetProperty(gCtx, global, fnName, nullptr);
    JSStringRelease(fnName);
    if (!JSValueIsObject(gCtx, value)) return;
    JSObjectRef fn = JSValueToObject(gCtx, value, nullptr);
    if (!fn || !JSObjectIsFunction(gCtx, fn)) return;
    JSValueRef args[5] = {
        JSValueMakeNumber(gCtx, x), JSValueMakeNumber(gCtx, y),
        JSValueMakeNumber(gCtx, deltaX), JSValueMakeNumber(gCtx, deltaY),
        JSValueMakeNumber(gCtx, 0) // DOM_DELTA_PIXEL
    };
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(gCtx, fn, nullptr, 5, args, &exc);
    if (exc) {
        JSStringRef error = JSValueToStringCopy(gCtx, exc, nullptr);
        size_t size = JSStringGetMaximumUTF8CStringSize(error);
        std::vector<char> text(size);
        JSStringGetUTF8CString(error, text.data(), size);
        fprintf(stderr, "[game] __wheelEvent 异常: %s\n", text.data());
        JSStringRelease(error);
    }
}

// Keep the browser-compatible logical viewport and DPR in lockstep with the AppKit content view.
// Pointer coordinates remain in points; Pixi and the CAMetalLayer independently allocate backing
// pixels at `dpr`, matching a browser canvas with autoDensity enabled.
static void emitViewportResize(int width, int height, double dpr) {
    if (!gCtx || width <= 0 || height <= 0) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef fnName = JSStringCreateWithUTF8CString("__resizeEvent");
    JSValueRef value = JSObjectGetProperty(gCtx, global, fnName, nullptr);
    JSStringRelease(fnName);
    if (!JSValueIsObject(gCtx, value)) return;
    JSObjectRef fn = JSValueToObject(gCtx, value, nullptr);
    if (!fn || !JSObjectIsFunction(gCtx, fn)) return;
    JSValueRef args[3] = { JSValueMakeNumber(gCtx, width), JSValueMakeNumber(gCtx, height),
                          JSValueMakeNumber(gCtx, dpr) };
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(gCtx, fn, nullptr, 3, args, &exc);
    if (exc) {
        JSStringRef error = JSValueToStringCopy(gCtx, exc, nullptr);
        size_t size = JSStringGetMaximumUTF8CStringSize(error);
        std::vector<char> text(size);
        JSStringGetUTF8CString(error, text.data(), size);
        fprintf(stderr, "[game] __resizeEvent 异常: %s\n", text.data());
        JSStringRelease(error);
    }
}

// Convert AppKit key events to the browser KeyboardEvent shape consumed by pixi-keyboard.
static BOOL emitKeyboard(const char* type, NSEvent* event) {
    if (!gCtx || !event) return NO;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef fnName = JSStringCreateWithUTF8CString("__keyboardEvent");
    JSValueRef value = JSObjectGetProperty(gCtx, global, fnName, nullptr);
    JSStringRelease(fnName);
    if (!JSValueIsObject(gCtx, value)) return NO;
    JSObjectRef fn = JSValueToObject(gCtx, value, nullptr);
    if (!fn || !JSObjectIsFunction(gCtx, fn)) return NO;

    NSString* key = event.characters ?: @"";
    NSString* code = @"Unidentified";
    NSInteger browserKeyCode = 0;
    switch (event.keyCode) {
        case 49:  key = @" ";          code = @"Space";      browserKeyCode = 32; break;
        case 36:
        case 76:  key = @"Enter";      code = @"Enter";      browserKeyCode = 13; break;
        case 48:  key = @"Tab";        code = @"Tab";        browserKeyCode = 9;  break;
        case 51:  key = @"Backspace";  code = @"Backspace";  browserKeyCode = 8;  break;
        case 53:  key = @"Escape";     code = @"Escape";     browserKeyCode = 27; break;
        case 123: key = @"ArrowLeft";  code = @"ArrowLeft";  browserKeyCode = 37; break;
        case 126: key = @"ArrowUp";    code = @"ArrowUp";    browserKeyCode = 38; break;
        case 124: key = @"ArrowRight"; code = @"ArrowRight"; browserKeyCode = 39; break;
        case 125: key = @"ArrowDown";  code = @"ArrowDown";  browserKeyCode = 40; break;
        default: {
            NSString* chars = event.charactersIgnoringModifiers ?: key;
            if (chars.length > 0) {
                unichar ch = [chars characterAtIndex:0];
                if (ch >= 'a' && ch <= 'z') ch = (unichar)(ch - 'a' + 'A');
                if (ch >= 'A' && ch <= 'Z') {
                    browserKeyCode = ch;
                    code = [NSString stringWithFormat:@"Key%C", ch];
                } else if (ch >= '0' && ch <= '9') {
                    browserKeyCode = ch;
                    code = [NSString stringWithFormat:@"Digit%C", ch];
                } else if (ch < 128) {
                    browserKeyCode = ch;
                }
            }
            break;
        }
    }

    JSStringRef typeString = JSStringCreateWithUTF8CString(type);
    JSStringRef keyString = JSStringCreateWithUTF8CString(key.UTF8String ?: "");
    JSStringRef codeString = JSStringCreateWithUTF8CString(code.UTF8String ?: "");
    NSEventModifierFlags flags = event.modifierFlags;
    JSValueRef args[9] = {
        JSValueMakeString(gCtx, typeString), JSValueMakeString(gCtx, keyString),
        JSValueMakeString(gCtx, codeString), JSValueMakeNumber(gCtx, browserKeyCode),
        JSValueMakeBoolean(gCtx, event.isARepeat),
        JSValueMakeBoolean(gCtx, (flags & NSEventModifierFlagOption) != 0),
        JSValueMakeBoolean(gCtx, (flags & NSEventModifierFlagControl) != 0),
        JSValueMakeBoolean(gCtx, (flags & NSEventModifierFlagCommand) != 0),
        JSValueMakeBoolean(gCtx, (flags & NSEventModifierFlagShift) != 0)
    };
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(gCtx, fn, nullptr, 9, args, &exc);
    JSStringRelease(typeString);
    JSStringRelease(keyString);
    JSStringRelease(codeString);
    if (exc) {
        JSStringRef error = JSValueToStringCopy(gCtx, exc, nullptr);
        size_t size = JSStringGetMaximumUTF8CStringSize(error);
        std::vector<char> text(size);
        JSStringGetUTF8CString(error, text.data(), size);
        fprintf(stderr, "[game] __keyboardEvent 异常: %s\n", text.data());
        JSStringRelease(error);
    }
    return YES;
}

@interface MetalView : NSView
- (void)syncRetinaBackingAndNotify:(BOOL)notify;
- (void)cancelActivePointers;
@end
@implementation MetalView {
    NSTrackingArea* _track;
    NSUInteger _pressedButtons;
}
- (CALayer*)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)isOpaque { return NO; }
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }   // 窗口失焦时首次点击直接进游戏，不被系统吃掉当激活
- (void)syncRetinaBackingAndNotify:(BOOL)notify {
    const int logicalW = std::max(1, (int)std::lround(self.bounds.size.width));
    const int logicalH = std::max(1, (int)std::lround(self.bounds.size.height));
    CGFloat scale = self.window ? self.window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
    if (!(scale > 0.0) || !std::isfinite((double)scale)) scale = 1.0;
    // QA/A-B hook: exercise Retina rendering and measure fill-rate on a machine whose currently
    // connected displays are all 1x. Normal launches always follow NSWindow.backingScaleFactor.
    if (const char* forced = getenv("GAME_BACKING_SCALE")) {
        char* end = nullptr;
        const double value = std::strtod(forced, &end);
        if (end != forced && std::isfinite(value) && value >= 1.0 && value <= 3.0) scale = value;
    }
    const int pixelW = std::max(1, (int)std::lround(logicalW * scale));
    const int pixelH = std::max(1, (int)std::lround(logicalH * scale));

    CAMetalLayer* metal = [self.layer isKindOfClass:[CAMetalLayer class]] ? (CAMetalLayer*)self.layer : nil;
    if (metal) {
        metal.contentsScale = scale;
        metal.drawableSize = CGSizeMake(pixelW, pixelH);
    }

    const bool changed = logicalW != W || logicalH != H || pixelW != gDrawableW ||
                         pixelH != gDrawableH || std::fabs(gBackingScale - (double)scale) > 0.0001;
    W = logicalW; H = logicalH;
    gDrawableW = pixelW; gDrawableH = pixelH; gBackingScale = scale;
    if (notify && changed) emitViewportResize(W, H, gBackingScale);
    if (changed && (notify || getenv("GAME_RESIZE_TRACE")))
        fprintf(stderr, "[resize/mac] viewport=%dx%d dpr=%.2f drawable=%dx%d\n",
                W, H, gBackingScale, gDrawableW, gDrawableH);
}
- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self syncRetinaBackingAndNotify:YES];
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    // Moving the window between Retina and non-Retina displays does not necessarily resize it.
    [self syncRetinaBackingAndNotify:YES];
}
- (void)resetCursorRects {
    [super resetCursorRects];
    // The game canvas owns an explicit arrow rect. Without one, AppKit may keep using the shared
    // NSTextView field editor's last I-beam rect after the transparent native input is hidden.
    [self addCursorRect:self.bounds cursor:gameCursor()];
}
- (void)updateTrackingAreas {
    if (_track) [self removeTrackingArea:_track];
    _track = [[NSTrackingArea alloc] initWithRect:self.bounds
        // Without NSTrackingEnabledDuringMouseDrag AppKit suppresses mouseExited while the
        // primary button is held.  If the button is then released outside this window, Pixi/FGUI
        // never sees the pointerout that cancels ScrollPane dragging, so the list starts following
        // ordinary mouseMoved events when the pointer comes back in.
        options:(NSTrackingMouseMoved|NSTrackingMouseEnteredAndExited|NSTrackingActiveInKeyWindow|
                 NSTrackingInVisibleRect|NSTrackingEnabledDuringMouseDrag)
        owner:self userInfo:nil];
    [self addTrackingArea:_track];
    [super updateTrackingAreas];
}
- (CGPoint)cvt:(NSEvent*)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    return CGPointMake(p.x, self.bounds.size.height - p.y);
}
- (BOOL)cancelActivePointersAt:(CGPoint)p {
    NSUInteger pressed = _pressedButtons;
    if (!pressed) return NO;
    _pressedButtons = 0;
    if (getenv("GAME_INPUT_TRACE"))
        fprintf(stderr,"[input] boundary-cancel view=(%.1f,%.1f) buttons=%lu\n",
                p.x,p.y,(unsigned long)pressed);
    NSUInteger remaining = pressed;
    if (pressed & 1) {
        remaining &= ~((NSUInteger)1);
        emitPointer("pointerboundarycancel", p.x, p.y, 0, (int)remaining);
    }
    if (pressed & 2) {
        remaining &= ~((NSUInteger)2);
        emitPointer("pointerboundarycancel", p.x, p.y, 2, (int)remaining);
    }
    // Clear Pixi's hover path as well. This is needed when mouseDragged detects the boundary
    // before AppKit happens to deliver the tracking area's mouseExited callback.
    emitPointer("pointerout", p.x, p.y, -1, 0);
    return YES;
}
- (void)cancelActivePointers {
    if (!_pressedButtons || !self.window) return;
    NSPoint windowPoint = [self.window convertPointFromScreen:NSEvent.mouseLocation];
    NSPoint viewPoint = [self convertPoint:windowPoint fromView:nil];
    [self cancelActivePointersAt:CGPointMake(viewPoint.x, self.bounds.size.height - viewPoint.y)];
}
- (void)mouseMoved:(NSEvent*)e     {
    CGPoint p=[self cvt:e];
    // Recovery path for a mouseUp consumed by another app/window before AppKit sent mouseExited.
    // Release the stale logical press before this ordinary hover move reaches FairyGUI.
    NSUInteger physical = NSEvent.pressedMouseButtons & 3;
    if (_pressedButtons && (physical & _pressedButtons) != _pressedButtons)
        [self cancelActivePointersAt:p];
    emitPointer("pointermove", p.x, p.y, -1, 0);
}
- (void)mouseDragged:(NSEvent*)e   {
    if (!(_pressedButtons & 1)) return;
    CGPoint p=[self cvt:e];
    if (p.x < 0 || p.y < 0 || p.x >= self.bounds.size.width || p.y >= self.bounds.size.height) {
        [self cancelActivePointersAt:p];
        return;
    }
    emitPointer("pointermove", p.x, p.y, 0, 1);
}
- (void)mouseDown:(NSEvent*)e      {
    [self.window makeFirstResponder:self]; CGPoint p=[self cvt:e];
    _pressedButtons |= 1;
    if (getenv("GAME_INPUT_TRACE")) fprintf(stderr,"[input] pointerdown view=(%.1f,%.1f) screen=(%.1f,%.1f) viewSize=%.1fx%.1f\n",p.x,p.y,e.locationInWindow.x,e.locationInWindow.y,self.bounds.size.width,self.bounds.size.height);
    emitPointer("pointerdown", p.x, p.y, 0, 1);
}
- (void)mouseUp:(NSEvent*)e        {
    if (!(_pressedButtons & 1)) return;
    _pressedButtons &= ~((NSUInteger)1);
    CGPoint p=[self cvt:e]; emitPointer("pointerup", p.x, p.y, 0, (int)_pressedButtons);
}
- (void)scrollWheel:(NSEvent*)e    {
    CGPoint p=[self cvt:e];
    // AppKit is positive for scrolling up; WheelEvent.deltaY is positive for scrolling down.
    emitWheel(p.x, p.y, -e.scrollingDeltaX, -e.scrollingDeltaY);
}
- (void)rightMouseDown:(NSEvent*)e { _pressedButtons |= 2; CGPoint p=[self cvt:e]; emitPointer("pointerdown", p.x, p.y, 2, (int)_pressedButtons); }
- (void)rightMouseUp:(NSEvent*)e   {
    if (!(_pressedButtons & 2)) return;
    _pressedButtons &= ~((NSUInteger)2);
    CGPoint p=[self cvt:e]; emitPointer("pointerup", p.x, p.y, 2, (int)_pressedButtons);
}
- (void)mouseExited:(NSEvent*)e    {
    CGPoint p=[self cvt:e];
    if (![self cancelActivePointersAt:p]) emitPointer("pointerout", p.x, p.y, -1, 0);
}
- (void)keyDown:(NSEvent*)e        { if (!emitKeyboard("keydown", e)) [super keyDown:e]; }
- (void)keyUp:(NSEvent*)e          { if (!emitKeyboard("keyup", e)) [super keyUp:e]; }
@end

@interface AppDelegate () {
    NSWindow* _window; MetalView* _view; NSTextField* _fpsLabel;
    NSView* _reloadOverlay; NSButton* _reloadButton;
    MacLiveLogWindowController* _liveLogWindow;
    NSTimer* _frameTimer; int _frame; BOOL _renderActive; BOOL _reloading;
    BOOL _runtimeShutdown;
    id<NSObject> _testPerformanceActivity;
}
- (void)tick;
- (void)frameTimerTick:(NSTimer*)timer;
- (void)onReloadTap:(id)sender;
- (void)showLiveLog:(id)sender;
- (void)openLogFolder:(id)sender;
@end

@implementation AppDelegate
- (void)installApplicationMenu {
    if (!maclog::directoryPath().length) return;
    NSMenu* menuBar = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    [menuBar addItem:appMenuItem];

    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"Pixi Native"];
    NSMenuItem* liveLogs = [[NSMenuItem alloc] initWithTitle:@"实时日志…"
                                                      action:@selector(showLiveLog:)
                                               keyEquivalent:@"l"];
    liveLogs.target = self;
    liveLogs.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [appMenu addItem:liveLogs];
    NSMenuItem* logFolder = [[NSMenuItem alloc] initWithTitle:@"打开日志文件夹"
                                                       action:@selector(openLogFolder:)
                                                keyEquivalent:@"l"];
    logFolder.target = self;
    logFolder.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [appMenu addItem:logFolder];
    [appMenu addItem:NSMenuItem.separatorItem];
    NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:@"退出 Pixi Native"
                                                  action:@selector(terminate:)
                                           keyEquivalent:@"q"];
    quit.target = NSApp;
    [appMenu addItem:quit];
    appMenuItem.submenu = appMenu;

    // Native text fields dispatch the standard editing actions through the application menu's
    // responder chain. Without an Edit menu, NSTextField instances used by the GM prompt can type
    // normally but Command-V/Command-C have no key-equivalent action to invoke.
    NSMenuItem* editMenuItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    [menuBar addItem:editMenuItem];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"编辑"];
    [editMenu addItemWithTitle:@"撤销" action:@selector(undo:) keyEquivalent:@"z"];
    NSMenuItem* redo = [editMenu addItemWithTitle:@"重做" action:@selector(redo:) keyEquivalent:@"z"];
    redo.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [editMenu addItem:NSMenuItem.separatorItem];
    [editMenu addItemWithTitle:@"剪切" action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"复制" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"粘贴" action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"全选" action:@selector(selectAll:) keyEquivalent:@"a"];
    editMenuItem.submenu = editMenu;

    NSApp.mainMenu = menuBar;
}
- (void)showLiveLog:(id)sender {
    (void)sender;
    NSString* path = maclog::currentFilePath();
    if (!path.length) return;
    if (!_liveLogWindow) {
        _liveLogWindow = [[MacLiveLogWindowController alloc] initWithLogPath:path];
    }
    [_liveLogWindow showLiveLogWindow];
}
- (void)openLogFolder:(id)sender {
    (void)sender;
    NSString* path = maclog::currentFilePath();
    if (!path.length) return;
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[
        [NSURL fileURLWithPath:path]
    ]];
}
- (NSFont*)reloadFont:(CGFloat)size bold:(BOOL)bold {
    NSFont* font=bold?[NSFont boldSystemFontOfSize:size]:[NSFont systemFontOfSize:size];
    return font;
}
- (void)showReloadTitle:(NSString*)title content:(NSString*)content {
    if (!_window || !_view) {
        fprintf(stderr, "[reload/mac] showReload ignored: window not ready\n");
        return;
    }
    if (_reloadOverlay) [self hideReload];
    title = title.length ? title : @"Sorry";
    content = content.length ? content : @"Network disconnected. Please try again.";

    // Match CDN app-reload / iOS overlay: dim mask + white rounded card + logo + REFRESH.
    NSView* overlay = [[NSView alloc] initWithFrame:_view.bounds];
    overlay.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    overlay.wantsLayer = YES;
    overlay.layer.backgroundColor = [[NSColor colorWithCalibratedWhite:0 alpha:0.5] CGColor];

    NSView* popup = [[NSView alloc] initWithFrame:NSZeroRect];
    popup.wantsLayer = YES;
    popup.layer.backgroundColor = NSColor.whiteColor.CGColor;
    popup.layer.cornerRadius = 20;
    popup.translatesAutoresizingMaskIntoConstraints = NO;
    [overlay addSubview:popup];

    NSImage* logoImage = [[NSImage alloc] initWithContentsOfFile:
        pathUnderRoot(gGameDir, @"reload/fts_loading_logo.png")];
    NSImageView* logo = [[NSImageView alloc] initWithFrame:NSZeroRect];
    logo.image = logoImage;
    logo.imageScaling = NSImageScaleProportionallyUpOrDown;
    logo.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField* titleLbl = [NSTextField labelWithString:title];
    titleLbl.font = [self reloadFont:24 bold:YES];
    titleLbl.textColor = NSColor.blackColor;
    titleLbl.alignment = NSTextAlignmentCenter;
    titleLbl.maximumNumberOfLines = 0;
    titleLbl.lineBreakMode = NSLineBreakByWordWrapping;
    titleLbl.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField* contentLbl = [NSTextField wrappingLabelWithString:content];
    contentLbl.font = [self reloadFont:16 bold:NO];
    contentLbl.textColor = NSColor.blackColor;
    contentLbl.alignment = NSTextAlignmentCenter;
    contentLbl.translatesAutoresizingMaskIntoConstraints = NO;

    NSButton* btn = [[NSButton alloc] initWithFrame:NSZeroRect];
    btn.title = @"REFRESH";
    btn.bezelStyle = NSBezelStyleRounded;
    btn.bordered = NO;
    btn.wantsLayer = YES;
    btn.layer.backgroundColor = [[NSColor colorWithCalibratedRed:237/255.0 green:103/255.0 blue:85/255.0 alpha:1.0] CGColor];
    btn.layer.cornerRadius = 5;
    btn.layer.shadowColor = btn.layer.backgroundColor;
    btn.layer.shadowOffset = CGSizeMake(0, -6);
    btn.layer.shadowRadius = 13;
    btn.layer.shadowOpacity = 0.6;
    btn.font = [self reloadFont:16 bold:NO];
    btn.contentTintColor = NSColor.whiteColor;
    NSMutableAttributedString* attr = [[NSMutableAttributedString alloc] initWithString:@"REFRESH"];
    [attr addAttribute:NSForegroundColorAttributeName value:NSColor.whiteColor range:NSMakeRange(0, attr.length)];
    [attr addAttribute:NSFontAttributeName value:btn.font range:NSMakeRange(0, attr.length)];
    btn.attributedTitle = attr;
    btn.target = self;
    btn.action = @selector(onReloadTap:);
    btn.translatesAutoresizingMaskIntoConstraints = NO;
    _reloadButton = btn;

    [popup addSubview:logo];
    [popup addSubview:titleLbl];
    [popup addSubview:contentLbl];
    [popup addSubview:btn];

    const CGFloat pad = 30, padBottom = 40;
    [NSLayoutConstraint activateConstraints:@[
        [popup.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
        [popup.centerYAnchor constraintEqualToAnchor:overlay.centerYAnchor],
        [popup.widthAnchor constraintEqualToAnchor:overlay.widthAnchor multiplier:0.5],

        [logo.topAnchor constraintEqualToAnchor:popup.topAnchor constant:pad],
        [logo.centerXAnchor constraintEqualToAnchor:popup.centerXAnchor],
        [logo.widthAnchor constraintEqualToConstant:122],
        [logo.heightAnchor constraintEqualToConstant:122.0*195.0/218.0],

        [titleLbl.topAnchor constraintEqualToAnchor:logo.bottomAnchor constant:15],
        [titleLbl.leadingAnchor constraintEqualToAnchor:popup.leadingAnchor constant:pad],
        [titleLbl.trailingAnchor constraintEqualToAnchor:popup.trailingAnchor constant:-pad],

        [contentLbl.topAnchor constraintEqualToAnchor:titleLbl.bottomAnchor constant:10],
        [contentLbl.leadingAnchor constraintEqualToAnchor:popup.leadingAnchor constant:pad],
        [contentLbl.trailingAnchor constraintEqualToAnchor:popup.trailingAnchor constant:-pad],

        [btn.topAnchor constraintEqualToAnchor:contentLbl.bottomAnchor constant:60],
        [btn.centerXAnchor constraintEqualToAnchor:popup.centerXAnchor],
        [btn.widthAnchor constraintEqualToConstant:200],
        [btn.heightAnchor constraintEqualToConstant:48],
        [btn.bottomAnchor constraintEqualToAnchor:popup.bottomAnchor constant:-padBottom],
    ]];

    [_view addSubview:overlay positioned:NSWindowAbove relativeTo:nil];
    _reloadOverlay = overlay;
    fprintf(stderr, "[reload/mac] 弹窗已显示: %s / %s\n", title.UTF8String, content.UTF8String);
}
- (void)hideReload {
    if (_reloadOverlay) {
        [_reloadOverlay removeFromSuperview];
        _reloadOverlay = nil;
        _reloadButton = nil;
    }
}
- (void)onReloadTap:(id)sender {
    (void)sender;
    NSButton* btn = _reloadButton;
    if (!btn) { [self reloadGame]; return; }
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
        ctx.duration = 0.08;
        btn.animator.alphaValue = 0.85;
    } completionHandler:^{
        btn.alphaValue = 1.0;
        [self reloadGame];
    }];
}
- (void)reloadGame {
    if (_runtimeShutdown) return;
    if (_reloading) {
        fprintf(stderr, "[reload/mac] reload already in progress\n");
        return;
    }
    _reloading = YES;

    ++gRuntimeEpoch;
    fprintf(stderr, "[reload/mac] 重建 JSC 上下文 + 重新登录...\n");
    [self hideReload];
    [gTextInput hide];

    // 1) Stop every timer owned by the dying JS world. Unprotect their callbacks while the
    //    old context is still alive, or the entire old JS heap stays protected after release.
    if (gTimers) {
        for (NSTimer* t in gTimers.allValues) [t invalidate];
        [gTimers removeAllObjects];
    }
    if (gTimerFns) {
        if (gCtx) for (NSValue* v in gTimerFns.allValues) JSValueUnprotect(gCtx, (JSObjectRef)v.pointerValue);
        [gTimerFns removeAllObjects];
    }

    // 2) Drop audio / zip / pending present state so old callbacks cannot talk to new JS.
    shutdownAudio();
    if (gZipCache) [gZipCache removeAllObjects];
    if (gZipLru) [gZipLru removeAllObjects];
    gPendingRoot = 0;
    gShadowRoot = 0;
    gNativeScreenPresented = false;
    gNativeBatchSerial = 0;
    gNativeTweens.clear();

    // 3) Release the old JS context (after timers are gone).
    if (gCtx) {
        JSGlobalContextRelease(gCtx);
        gCtx = nullptr;
    }

    // 4) Reset GL command decoder id maps. Old GL handles may leak; acceptable for reload.
    gDec = cmd::Decoder();
    if (gNativeFguiWhiteTexture) {
        gDec.registerExternalTexture(kNativeFguiWhiteCid, gNativeFguiWhiteTexture);
    }

    // 5) Fresh bootstrap: new gCtx, re-run game_host, re-login.
    gStartUs = nowUs();
    if (!setupGame()) {
        fprintf(stderr, "[reload/mac] setupGame 失败\n");
        nativeLogError(@"runtime", @"reload_setup_failed",
                                            @"macOS runtime reload setup failed", NO);
    }
    _reloading = NO;
    fprintf(stderr, "[reload/mac] reload complete\n");
}
- (void)frameTimerTick:(NSTimer*)timer { (void)timer; [self tick]; }
- (void)tick {
    if (!_renderActive) return;
    if (gSurfaceNeedsRestore) {
        if (!restoreEGLSurface()) return;
        resetGLStateAfterResume();
        notifyJsResume();
        gSurfaceNeedsRestore = false;
    }
    double t0 = nowUs();
    if (!gBatcher.ready() && gContext != EGL_NO_CONTEXT) {
        gBatcher.texLookup = [](int id){ return gDec.glTextureEnsured((uint32_t)id); };   // 画前补传节流队列中的同名纹理,防首帧采样空纹理
        gBatcher.init();
    }
    gDec.batcher = &gBatcher; gDec.sgraph = &gSG;
    gDec.screenW = (float)W; gDec.screenH = (float)H;
    gBatcher.quads = 0; gBatcher.escapes = 0; gBatcher.drawCalls = 0;
    gBatcher.frameReset();
    BOOL canPresent = _window && _window.isVisible &&
        ((_window.occlusionState & NSWindowOcclusionStateVisible) != 0);
#if !defined(PIXINATIVE_PRODUCTION)
    // Automated matrix runs must keep exercising the real draw path even when another developer
    // window temporarily covers the game. This debug-only capability is not a production
    // visibility policy; without it an unattended all-machine audit becomes ticker-only as soon
    // as macOS marks the window occluded.
    if (const char* forcePresent = std::getenv("GAME_TEST_FORCE_PRESENT"))
        if (std::atoi(forcePresent) == 1) canPresent = YES;
#endif
    // Backbuffer dump tests intentionally run without depending on desktop occlusion.
    if (getenv("GAME_DUMP")) canPresent = YES;
    static BOOL wasPresentable = YES;
    if (canPresent != wasPresentable) {
        fprintf(stderr, "[lifecycle] drawable %s; JS ticker continues\n", canPresent ? "visible" : "occluded");
        wasPresentable = canPresent;
    }
    gCanPresent = canPresent;   // __nativePresent(cbShadowFlag)内联绘制的闸门,须在 __tick 前就位
    drawOneFrame(canPresent);
    _frame++;
    double busyUs=nowUs()-t0;
    static double fpsLabelStartUs=0;
    static int fpsLabelFrames=0;
    if (fpsLabelStartUs <= 0) fpsLabelStartUs = t0;
    fpsLabelFrames++;
    double fpsLabelElapsedUs = nowUs() - fpsLabelStartUs;
    if (_fpsLabel && fpsLabelElapsedUs >= 500000.0) {
        _fpsLabel.stringValue = [NSString stringWithFormat:@"FPS: %.0f",
            fpsLabelFrames * 1000000.0 / fpsLabelElapsedUs];
        fpsLabelStartUs = nowUs();
        fpsLabelFrames = 0;
    }
    static const bool reportPerf=getenv("GAME_PROF")!=nullptr;
    static double windowStartUs=0,sumBusy=0,sumUpload=0,sumTick=0,sumDecode=0,sumRender=0,sumSubmit=0,sumSwap=0;
    static double frameRing[240]; static int ringCount=0,ringIndex=0,windowFrames=0,windowSlow=0;
    if(reportPerf){
        if(windowStartUs<=0) windowStartUs=t0;
        sumBusy+=busyUs; sumUpload+=gFrameTiming.uploadUs; sumTick+=gFrameTiming.tickUs; sumDecode+=gFrameTiming.decodeUs;
        sumRender+=gFrameTiming.renderUs; sumSubmit+=gFrameTiming.submitUs; sumSwap+=gFrameTiming.swapUs;
        frameRing[ringIndex]=busyUs/1000.0; ringIndex=(ringIndex+1)%240; if(ringCount<240) ringCount++;
        if(busyUs>16667.0) windowSlow++;
        windowFrames++;
    }
    // Long-frame diagnostics stay cheap in normal builds. Detailed JS hooks are only called when
    // GAME_JS_PRELUDE enables __PROF, so profiling itself cannot make every transition slower.
    static double lastBadUs=0;
    if(busyUs>25000.0 && t0-lastBadUs>500000.0){
        lastBadUs=t0;
        double jsUs=std::max(0.0,gFrameTiming.tickUs-gFrameTiming.decodeUs);
        // 秒级"JS"帧几乎不可能是真实脚本执行:要么主线程被同步 IO 卡住(紧邻的 [fetch-slow]
        // 会指认 URL),要么进程被系统挂起(App Nap/调试器)。标注出来防止误读成脚本性能问题。
        if(busyUs>1000000.0)
            fprintf(stderr,"[badframe/mac] ⚠️ 秒级长帧(%.1fs):优先查相邻 [fetch-slow](同步网络阻塞)或系统挂起,而非脚本性能\n",busyUs/1000000.0);
        fprintf(stderr,"[badframe/mac] %.1fms JS%.1f decode%.1f upload%.1f native%.1f(compute%.1f batch%.1f shadow%.1f=setup%.1f[bind%.1f preClearFinish%.1f clear%.1f]/compute%.1f/batch%.1f/finish%.1f) submit%.1f swap%.1f "
                       "draw%u tex%u(defer%u pending%zu) buf%u | mb %.1f/%.1f/%.1f/%.1f/%.1f(%d,%d,%d,%d) "
                       "sb %.1f/%.1f/%.1f/%.1f/%.1f(%d,%d,%d,%d)\n",
                busyUs/1000.0,jsUs/1000.0,gFrameTiming.decodeUs/1000.0,gFrameTiming.uploadUs/1000.0,
                gFrameTiming.renderUs/1000.0,gFrameTiming.computeUs/1000.0,gFrameTiming.batchUs/1000.0,gFrameTiming.shadowUs/1000.0,
                gFrameTiming.shadowSetupUs/1000.0,gFrameTiming.shadowBindUs/1000.0,
                gFrameTiming.preClearFinishUs/1000.0,gFrameTiming.shadowClearUs/1000.0,
                gFrameTiming.shadowComputeUs/1000.0,
                gFrameTiming.shadowBatchUs/1000.0,gFrameTiming.shadowFinishUs/1000.0,
                gFrameTiming.submitUs/1000.0,gFrameTiming.swapUs/1000.0,
                (unsigned)gDec.drawCalls,(unsigned)gDec.texUploads,(unsigned)gDec.texDeferred,
                gDec.texPending(),(unsigned)gDec.bufUploads,
                gBatcher.boundsMs,gBatcher.walkMs,gBatcher.bufferDataMs,gBatcher.bufferSubMs,gBatcher.drawMs,
                gBatcher.slowBlend,gBatcher.slowPremul,gBatcher.slowTwoColor,gBatcher.slowMaskWrite,
                gShadowBatcher.boundsMs,gShadowBatcher.walkMs,
                gShadowBatcher.bufferDataMs,gShadowBatcher.bufferSubMs,gShadowBatcher.drawMs,
                gShadowBatcher.slowBlend,gShadowBatcher.slowPremul,gShadowBatcher.slowTwoColor,gShadowBatcher.slowMaskWrite);
        static const bool detailed=getenv("GAME_PROF")!=nullptr;
        if(detailed){JSValueRef a=JSValueMakeNumber(gCtx,busyUs/1000.0);callJsGlobal("__badFrame",1,&a);}
    }
    if(reportPerf && windowFrames>=120){
        double elapsedUs=nowUs()-windowStartUs, inv=1.0/windowFrames;
        double sorted[240]; memcpy(sorted,frameRing,sizeof(double)*ringCount); std::sort(sorted,sorted+ringCount);
        unsigned draws=gNativeScreenPresented?gShadowBatcher.drawCalls:gDec.drawCalls;
        fprintf(stderr,"[perf/mac] FPS %.1f 忙%.2fms JS%.2f+解码%.2f+上传%.2f native%.2f submit%.2f swap%.2f "
                       "p50 %.2f p95 %.2f p99 %.2f max %.2f thermal%ld draw%u quad%u slow%u/%d\n",
                windowFrames*1000000.0/elapsedUs,sumBusy*inv/1000.0,std::max(0.0,sumTick-sumDecode)*inv/1000.0,
                sumDecode*inv/1000.0,sumUpload*inv/1000.0,sumRender*inv/1000.0,sumSubmit*inv/1000.0,sumSwap*inv/1000.0,
                sorted[ringCount/2],sorted[std::min(ringCount-1,(int)(ringCount*.95))],
                sorted[std::min(ringCount-1,(int)(ringCount*.99))],sorted[ringCount-1],
                (long)NSProcessInfo.processInfo.thermalState,draws,gShadowBatcher.quads,
                windowSlow,windowFrames);
        windowStartUs=nowUs(); windowFrames=0; windowSlow=0; sumBusy=sumUpload=sumTick=sumDecode=sumRender=sumSubmit=sumSwap=0;
    }
    // GAME_DUMP_AT 帧：dump 在 drawOneFrame(swap 前)完成后退出（headless 验证）。
    // _frame 每 tick 递增而 dump 的 fc 只数绘制帧,必须等 gDumpDone(否则启动跳帧会先退出后 dump)。
    static const char* dp = getenv("GAME_DUMP");
    static int dumpAt = getenv("GAME_DUMP_AT") ? atoi(getenv("GAME_DUMP_AT")) : 240;
    if (dp && _frame >= dumpAt && gDumpDone) [NSApp terminate:nil];
}
- (void)applicationDidFinishLaunching:(NSNotification*)n {
    gAppDelegate = self;

#if !defined(PIXINATIVE_PRODUCTION)
    if (const char* probe = std::getenv("GAME_TEST_GL_CLEAR_PROBE")) {
        gTestGlClearProbe = std::atoi(probe) == 1;
        if (gTestGlClearProbe)
            fprintf(stderr, "[gl-clear-probe/mac] enabled: pre-clear glFinish timing active\n");
    }
    // Keep the debug matrix process out of App Nap independently from drawable policy. Audit may
    // force a covered window to render for correctness coverage; perf must obey raw AppKit
    // visibility, otherwise a fully occluded CAMetalLayer can block its first glClear for ~1s.
    bool needsPerformanceActivity = false;
    if (const char* activity = std::getenv("GAME_TEST_PERFORMANCE_ACTIVITY"))
        needsPerformanceActivity = std::atoi(activity) == 1;
    // Backward-compatible guard for older harnesses where force-present implied the activity.
    if (const char* forcePresent = std::getenv("GAME_TEST_FORCE_PRESENT"))
        needsPerformanceActivity = needsPerformanceActivity || std::atoi(forcePresent) == 1;
    if (needsPerformanceActivity) {
        _testPerformanceActivity = [NSProcessInfo.processInfo
            beginActivityWithOptions:(NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical)
            reason:@"Pixi Native spin matrix"];
        fprintf(stderr, "[spin-matrix-host] App Nap disabled for test run\n");
    }
#endif
    [self installApplicationMenu];
    NSRect f = NSMakeRect(0,0,W,H);
    _window = [[NSWindow alloc] initWithContentRect:f
        styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable) backing:NSBackingStoreBuffered defer:NO];
    [_window setTitle:@"Pixi Native — pixi native runtime"];
    [_window setContentMinSize:NSMakeSize(640, 360)];
    // Preserve the launch viewport's content ratio while the user resizes the window. This is
    // 16:9 by default and also respects GAME_W/GAME_H overrides used by diagnostics.
    [_window setContentAspectRatio:NSMakeSize(W, H)];
    [_window setPreservesContentDuringLiveResize:YES];
    gMainWindow = _window;
    [_window center];
    _view = [[MetalView alloc] initWithFrame:f]; _view.wantsLayer = YES;
    NSView* rootView = [[NSView alloc] initWithFrame:f];
    rootView.wantsLayer = YES;
    [_window setContentView:rootView];
    _view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [rootView addSubview:_view];
    _view.layer.opaque = NO;
    [_window makeKeyAndOrderFront:nil];
    // The window now has its final screen/backingScaleFactor. Configure the layer before ANGLE
    // creates its EGLSurface so the very first drawable is Retina-sized.
    [_view syncRetinaBackingAndNotify:NO];
    gTextInput = [[NativeTextInputBridge alloc] init];
    [gTextInput attachToView:_view];
    if (shouldShowNativeDebugUI()) {
        _fpsLabel = [NSTextField labelWithString:@"FPS: --"];
        _fpsLabel.frame = NSMakeRect(7, H - 29, 110, 24);
        _fpsLabel.font = [NSFont boldSystemFontOfSize:18];
        _fpsLabel.textColor = [NSColor colorWithCalibratedRed:0.32 green:1.0 blue:0.47 alpha:1.0];
        _fpsLabel.drawsBackground = NO;
        _fpsLabel.bordered = NO;
        _fpsLabel.editable = NO;
        _fpsLabel.selectable = NO;
        _fpsLabel.autoresizingMask = NSViewMinYMargin;
        _fpsLabel.wantsLayer = YES;
        _fpsLabel.layer.shadowColor = NSColor.blackColor.CGColor;
        _fpsLabel.layer.shadowOpacity = 1.0;
        _fpsLabel.layer.shadowRadius = 1.0;
        _fpsLabel.layer.shadowOffset = CGSizeMake(1.0, -1.0);
        [_view addSubview:_fpsLabel positioned:NSWindowAbove relativeTo:nil];
    }
    [_window setAcceptsMouseMovedEvents:YES];
    [_window makeFirstResponder:_view];

    if (!initEGL(_view.layer)) {
        fprintf(stderr,"[game] initEGL 失败\n");
        nativeLogError(@"graphics", @"egl_initialization_failed",
                                            @"macOS EGL initialization failed", YES);
        [NSApp terminate:nil];
        return;
    }
    gStartUs = nowUs();
    if (!setupGame()) {
        fprintf(stderr,"[game] setupGame 失败\n");
        nativeLogError(@"runtime", @"initial_setup_failed",
                                            @"macOS initial game setup failed", YES);
        [NSApp terminate:nil];
        return;
    }

    _renderActive = YES;
    _frameTimer=[NSTimer timerWithTimeInterval:1.0/60.0 target:self selector:@selector(frameTimerTick:) userInfo:nil repeats:YES];
    _frameTimer.tolerance=0.001;
    [[NSRunLoop mainRunLoop] addTimer:_frameTimer forMode:NSRunLoopCommonModes];
    // Optional regression hook: resize an already-running scene, rather than merely starting at
    // a different viewport. This exercises the same AppKit -> JS -> FairyGUI path as live dragging.
    if (const char* resizeTo = getenv("GAME_TEST_RESIZE")) {
        int testWidth = 0, testHeight = 0, delayMs = 30000;
        if (std::sscanf(resizeTo, "%dx%d@%d", &testWidth, &testHeight, &delayMs) >= 2 &&
            testWidth > 0 && testHeight > 0) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delayMs * NSEC_PER_MSEC),
                           dispatch_get_main_queue(), ^{
                [_window setContentSize:NSMakeSize(testWidth, testHeight)];
            });
        }
    }
    fprintf(stderr,"[game] native frame timer=60fps\n");
    if (getenv("MAC_SHOW_LIVE_LOG")) [self showLiveLog:nil];
    [NSApp activateIgnoringOtherApps:YES];
}
- (void)applicationWillResignActive:(NSNotification*)notification {
    (void)notification;

    [_view cancelActivePointers];
    // Losing keyboard focus on macOS is not a rendering suspension. Keep the JS ticker,
    // Spine animations and native scene submission running while another app is active.
    // A genuinely invalid CAMetalLayer is detected by eglSwapBuffers and repaired in tick().
    fprintf(stderr, "[lifecycle] inactive: rendering continues\n");
}
- (void)applicationDidBecomeActive:(NSNotification*)notification {
    (void)notification;
    if (_runtimeShutdown) return;


    // Desktop sessions run for days; without this, the boot-time check is the only moment a hot
    // update can ever be discovered. The JS side throttles to one catalog request per 30 min.
    // Runtime renderer controls use the pointer resolver even when downloadable content is off.
    // Keep foreground rollback reachable for long-lived signed sessions.

    if (!_frameTimer || gContext == EGL_NO_CONTEXT) return;
    // Focus changes no longer stop the renderer, so do not reset GL/Pixi state here. If a real
    // surface loss happened while inactive, the normal tick recovery path handles it exactly once.
    _renderActive = YES;
    _frameTimer.fireDate=[NSDate date];
    fprintf(stderr, "[lifecycle] active: rendering was continuous\n");
}
- (void)shutdownGameRuntime {
    // NSApplication's normal terminate path calls exit() after applicationWillTerminate.  Do not
    // leave GL-backed C++ globals to cross-translation-unit static destruction order: NativeText,
    // FGUI and Spinex all call back into Decoder/ResourceManager while releasing their contents.
    if (_runtimeShutdown) return;
    _runtimeShutdown = YES;

    gHostShuttingDown.store(true, std::memory_order_release);



    // Stop every producer before tearing down the JS world or its native consumers.
    _renderActive = NO;
    [_frameTimer invalidate];
    _frameTimer = nil;
    ++gRuntimeEpoch;
    ++gSpinePrepareEpoch;
    if (gTimers) {
        for (NSTimer* timer in gTimers.allValues) [timer invalidate];
        [gTimers removeAllObjects];
    }
    if (gTimerFns) {
        if (gCtx) {
            for (NSValue* value in gTimerFns.allValues)
                JSValueUnprotect(gCtx, (JSObjectRef)value.pointerValue);
        }
        [gTimerFns removeAllObjects];
    }

    if (_testPerformanceActivity) {
        [NSProcessInfo.processInfo endActivity:_testPerformanceActivity];
        _testPerformanceActivity = nil;
    }
    [_liveLogWindow stop];
    _liveLogWindow = nil;
    [gTextInput hide];
    shutdownAudio();
    // Stop accepting resource work and wait only for worker segments, never main-queue
    // completions. The quit gate above cancels network waits so draining cannot wait for their
    // full download timeout. Parser/audio queues likewise must finish before static teardown.
    gNativeResources.shutdown();
    dispatch_barrier_sync(assetQueue(), ^{});
    dispatch_barrier_sync(audioLoadQueue(), ^{});
    dispatch_sync(audioQueue(), ^{});
    if (gZipCache) [gZipCache removeAllObjects];
    if (gZipLru) [gZipLru removeAllObjects];
    gPendingRoot = 0;
    gShadowRoot = 0;
    gNativeScreenPresented = false;
    gNativeBatchSerial = 0;
    gNativeTweens.clear();

    // Invalidate background generations and return any prewarm texture leases while all of their
    // native owners are still alive. Late main-queue completions now fail their epoch/context gate.
    gSpinePrepareInflight.clear();
    [gSpinePreparedCache removeAllObjects];
    [gSpinePreparedByHandle removeAllObjects];
    [gSpinePreparedLru removeAllObjects];
    for (auto& pair : gSpinexPrewarm) gSpinexRt.store.discardAsync(pair.second);
    gSpinexPrewarm.clear();
    gFguiPreparseInflight.clear();
    gFguiPreparseCache = nil;
    gFguiPreparseLru = nil;
    gFguiXmlPrewarm = nil;

    // Timer callbacks have been unprotected, so the context can now be destroyed without a late
    // native callback retaining or dereferencing objects from the dead JS world.
    if (gCtx) {
        JSGlobalContextRelease(gCtx);
        gCtx = nullptr;
    }

    // Preserve the dependency order. FGUI's releaseNode callback touches NativeText and Spinex;
    // Spinex releases ResourceManager texture references; text/resources touch Decoder and GL.
    if (gDisplay != EGL_NO_DISPLAY && gContext != EGL_NO_CONTEXT &&
        eglGetCurrentContext() != gContext && gSurface != EGL_NO_SURFACE) {
        eglMakeCurrent(gDisplay, gSurface, gSurface, gContext);
    }
    gNativeFgui.reset(gSG);
    gSpinexRt.resetAll();
    gNativeText.clear();
    gNativeResources.reset();

    if (gNativeFguiWhiteTexture) {
        gDec.unregisterExternalTexture(kNativeFguiWhiteCid);
        glDeleteTextures(1, &gNativeFguiWhiteTexture);
        gNativeFguiWhiteTexture = 0;
    }
    if (gShadowTex) {
        glDeleteTextures(1, &gShadowTex);
        gShadowTex = 0;
    }
    if (gShadowFbo) {
        glDeleteFramebuffers(1, &gShadowFbo);
        gShadowFbo = 0;
    }
    gShadowFboW = gShadowFboH = 0;
    gDec = cmd::Decoder();

    // ANGLE owns the remaining decoder-created GL objects. Destroy it only after every external
    // texture owner above has been emptied, and clear all handles so this method stays idempotent.
    EGLDisplay display = gDisplay;
    EGLSurface surface = gSurface;
    EGLContext context = gContext;
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
        eglReleaseThread();
    }
    gSurface = EGL_NO_SURFACE;
    gContext = EGL_NO_CONTEXT;
    gDisplay = EGL_NO_DISPLAY;
    gConfig = nullptr;
    gRenderLayer = nil;
    gSurfaceNeedsRestore = false;
    gCanPresent = false;
    gMainWindow = nil;

    fprintf(stderr, "=== Pixi Native session ended ===\n");
    maclog::shutdown();
}
- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    [self shutdownGameRuntime];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)s { return YES; }
@end

int main(int argc, char** argv) {
 @autoreleasepool {
  NSString* root=[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"game"];
  if(argc>1)root=[NSString stringWithUTF8String:argv[1]];
  gBundledGameDir=root.fileSystemRepresentation;
  gGameDir=gBundledGameDir.c_str();gAssetsDir=gGameDir;
  gBuf=(uint32_t*)malloc(kBufWords*4);
  NSApplication* app=NSApplication.sharedApplication;
  [app setActivationPolicy:NSApplicationActivationPolicyRegular];
  AppDelegate* delegate=[AppDelegate new];app.delegate=delegate;
  [app run];[delegate shutdownGameRuntime];
 }
 return 0;
}
