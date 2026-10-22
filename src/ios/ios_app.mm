#include "../apple/native_http.h"
// ios_app —— 让真实 Pixi Native（pixi v7.4.3）跑在 iOS 真机上。
// 复用 game_window.mm 的命令缓冲 + JSC 桥接（timer/xhr/readFile/evalScript/canvas2d 全平台无关，原样照搬），
// 仅替换平台层：
//   · 图形：ANGLE/EGL → 原生 OpenGLES.framework + EAGLContext(GLES3) + CAEAGLLayer，present 用 presentRenderbuffer
//   · 图片解码：NSBitmapImageRep(AppKit) → ImageIO/CoreGraphics（CGImageSource，跨平台）
//   · 窗口/帧驱动：NSWindow/CVDisplayLink → UIViewController/CADisplayLink
//   · 入口：NSApplication → UIApplicationMain
//   · 输入：鼠标 → touchesBegan/Moved/Ended
//   · 资源：本地 game/ 打进 bundle（补丁 JS 优先），2.5G assets 走局域网 HTTP（AssetServerURL）
// 渲染固定 1590x960（与 macOS 一致），CAEAGLLayer 缩放铺满屏幕（letterbox），触摸按 1590x960 视图空间传入。
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <JavaScriptCore/JavaScriptCore.h>
#import <ImageIO/ImageIO.h>
#import <OpenGLES/ES3/gl.h>
#import <OpenGLES/ES3/glext.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/EAGLDrawable.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreHaptics/CoreHaptics.h>
#import <CommonCrypto/CommonDigest.h>

// here avoids depending on CMake's generated target-name-Swift header.
#include <cstdio>
#include <vector>
#include <algorithm>
#include <string>
#include <cstring>
#include <chrono>
#include <map>
#include <zlib.h>
#include "../common/native_diag_log.h"   // 排查态日志编译期开关(默认关,-DPIXINATIVE_DIAG_LOG=1 打开)
#include "../jsc_rooted_value_vector.h"
#include "../cmdbuf/decoder.h"
#include "../cmdbuf/scenegraph.h"
#include "../cmdbuf/native_animation_frame.h"
#include "../cmdbuf/batcher.h"
#include "../cmdbuf/spinex.h"    // spine 全量 native 化(官方 spine-cpp 驱动;与 src/host/host_core.mm 共用,头内钩子已 inline)
#include "../cmdbuf/native_fgui.h"          // FairyGUI 全量 native 化(纯 C++,与平台无关,与 mac 共用同一份)
#include "../cmdbuf/plain_text_layout_contract.h"
#include "../cmdbuf/richtext_layout_contract.h"
#include "../cmdbuf/richtext_token_spacing.h"
#include "../cmdbuf/richtext_token_layout.h"
#include "../mac/native_resource_manager.h" // 异步字节/图片资源属主(CoreGraphics+GCD;头内按 TARGET_OS_IPHONE 选 GL 头)
#include "../common/native_script_utf8.h"
#include "../common/native_environment.h"
#include "../common/sha256.h"
#include "../mac/canvas2d_jsc.h"
#include "../host/host_core.h"   // Apple 两端共享的宿主核心(状态/回调/桥)

// ---- host_core 需要的平台颜色/PNG 接口(NativeTextSystem 用;iOS 走 UIKit) ----
id hostPlatformColor(CGFloat r, CGFloat g, CGFloat b, CGFloat a) { return [UIColor colorWithRed:r green:g blue:b alpha:a]; }
id hostPlatformClearColor() { return UIColor.clearColor; }
bool hostPlatformWritePNG(CGImageRef image, NSString* path) {
    NSData* png = UIImagePNGRepresentation([UIImage imageWithCGImage:image]);
    return [png writeToFile:path atomically:NO];
}
static GLuint gNativeFguiWhiteTexture = 0;                // fgui 纯色图元的 1x1 白纹理
static const uint32_t kNativeFguiWhiteCid = 7999999;      // 该白纹理的 decoder 外部纹理 id(与 mac 同值)
static sg::Batcher gBatcher;        // conch P1:native sprite 合批渲染器
// M2(原生 runtime 路线图):影子 FBO 渲染器。和 conch gBatcher 隔离,画进离屏 FBO 不上屏,供 M3 与真屏对拍。
static sg::Batcher gShadowBatcher;
static GLuint gShadowFbo = 0, gShadowTex = 0;
static int gShadowFboW = 0, gShadowFboH = 0;
static int gShadowRoot = 0;         // JS __shadowFlag 设置:本帧要影子渲染的根(0=不渲染;低频调用)
static int gShadowShow = 0;         // 0=离屏only 1=离屏+blit目视 2=M6 flip:直接画到真屏(短路 pixi,省 glGet/blit)
static uint32_t* gBuf = nullptr;
// → 上传被丢弃 → GPU 纹理空(m111 bottom_layer 背景板不显示的根因)。JS encoder cap/RESERVE 自适应。
static const size_t kBufWords = 1u << 24; // 64MB
int W = 1590, H = 960;             // 兜底设计尺寸（实际运行时按屏幕覆盖）
static double gScreenW = 1590, gScreenH = 960, gScreenDPR = 1;  // 真机屏幕点数 + 缩放，传给 game_host.js
static double gSafeAreaLeft = 0;   // 刘海/灵动岛横向安全区（native 像素，= max(左,右)点数×缩放），传给 game_host.js
static double gSafeAreaBottom = 0; // Home indicator clearance in render pixels.
static double gScreenRenderScale = 1; // UIKit points -> render pixels (JS DPR remains 1).

// 资源根：gGameDir = bundle 内 game/（打过补丁的 pixi/game_host/dev，最高优先、随包离线）；
//          gRemote = 局域网静态服务（托管 2.5G assets 镜像，根 = pixi_native/assets/）。
static NSString* gGameDir = nil;     // 运行时 = [bundle]/game
static NSString* gRemote  = nil;     // 运行时读 Info.plist AssetServerURL，缺省 192.168.8.228:8787
static bool gLogNet = false;         // [xhr-net] 网络请求日志(默认关,测温干净;排网络问题时改 true)
static bool gH5AssetVersionSelected = false; // Selected by Shell loading, fixed for this process.


static NSData* bundledAssetData(NSString* rel) {
    if (rel.length == 0 || !gGameDir) return nil;
    return [NSData dataWithContentsOfFile:[gGameDir stringByAppendingPathComponent:rel]];
}
// XHR 本地查找：已激活内容包优先，然后是 bundle 内 game/<rel>。
static NSData* localAssetData(NSString* rel) { return loadAsset(rel.UTF8String); }
// 旧版本会把 LAN 资源持久化到 Library/Caches/assets-cache。资源更新后该目录仍优先于服务器，
// 会造成 App 一直使用旧图集/Spine/FGUI。新版不再读写它，启动时顺手清掉已有遗留目录。
static void removeLegacyAssetCache() {
    NSString* caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    if (caches.length == 0) return;
    NSString* root = [caches stringByAppendingPathComponent:@"assets-cache"];
    if (![[NSFileManager defaultManager] fileExistsAtPath:root]) return;
    NSError* error = nil;
    if ([[NSFileManager defaultManager] removeItemAtPath:root error:&error])
        fprintf(stderr, "[asset-cache] removed legacy cache: %s\n", root.UTF8String);
    else
        fprintf(stderr, "[asset-cache] remove failed: %s (%s)\n", root.UTF8String,
                error.localizedDescription.UTF8String ?: "unknown");
}
static NSData* fetchRemoteAssetWithCachePolicy(NSURL* url, BOOL) {
 if (!url || ![@[@"http",@"https"] containsObject:url.scheme.lowercaseString]) return nil;
 return [NSData dataWithContentsOfURL:url];
}
static NSData* fetchRemoteAsset(NSURL* url) {
    return fetchRemoteAssetWithCachePolicy(url, NO);
}
// 拒绝 HTTPS→HTTP 降级重定向：ATS 为局域网 dev 资产全局放开，而 content/遥测请求可能携带
// installationId/构建版本/release——若服务器 307/308 到 http，body 会被明文重发。守卫在重定向
// 发生前拦截；初始就是 http 的 dev 资产请求不受影响（只禁降级，不禁 http 本身）。


// Content updates move multi-MiB payloads; the shared 30s asset budget starves weak links, so
// this session keeps the 15s stall detector but allows 240s per transfer. finalURL returns the
// post-redirect URL: ATS is relaxed for LAN assets, so content must re-check https afterwards.


// 取资源字节：先 bundle game/<rel>，否则从局域网 gRemote/<rel> 拉取。奖励 UI
// 会为所有道具拼 assets/icon/large/props/... ，但服务器只为部分道具提供大图。
// 精确 URL 不存在时再试同名 assets/props/img/... ，因此新道具无需追加进 App。
// 远程字节只进入本次运行所需的 ResourceManager/纹理缓存，不再跨启动落盘。
NSData* loadAsset(const char* path) {
 NSString* requested=[NSString stringWithUTF8String:path ?: ""];
 if([requested hasPrefix:@"https://"]||[requested hasPrefix:@"http://"])return nativeFetchResource(requested);
 NSString* rel=relOf([NSString stringWithUTF8String:path ?: ""]);
 if (!rel.length || [rel.pathComponents containsObject:@".."]) return nil;
 return [NSData dataWithContentsOfFile:[gGameDir stringByAppendingPathComponent:rel]];
}

// ---- 图片解码：NSData(png/jpg/webp...) -> JS {width,height,data}（premultiplied RGBA8，bottom-up 匹配 GL）----
// 用 ImageIO（CGImageSource）解码，画进左下原点 CGBitmapContext 不翻转 → buf 天然 bottom-up（同 macOS 约定）。
// decodeImageRaw:纯解码(不碰 JSC,线程安全),供主线程同步路径与后台异步路径共用。
static unsigned char* decodeImageRaw(NSData* d, int* ow, int* oh) {
    if (!d) return nullptr;
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)d, NULL);
    if (!src) return nullptr;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    if (!img) return nullptr;
    int w = (int)CGImageGetWidth(img), h = (int)CGImageGetHeight(img);
    size_t bytes = (size_t)w*h*4;
    unsigned char* buf = (unsigned char*)malloc(bytes);
    if (!buf) { CGImageRelease(img); return nullptr; }
    memset(buf, 0, bytes);
    CGColorSpaceRef csp = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(buf, w, h, 8, w*4, csp,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (cg) {
        CGContextSetBlendMode(cg, kCGBlendModeCopy);
        CGContextDrawImage(cg, CGRectMake(0,0,w,h), img);  // CGImage top-down 画进左下原点上下文 → bottom-up
        CGContextRelease(cg);
    } else { free(buf); buf = nullptr; }
    CGColorSpaceRelease(csp); CGImageRelease(img);
    if (buf) { *ow = w; *oh = h; }
    return buf;
}
static JSValueRef decodeImageData(JSContextRef ctx, NSData* d, JSValueRef* exc, bool sourcePremultiplied=false) {
    int w = 0, h = 0;
    unsigned char* buf = sourcePremultiplied?nativeimage::decodeEncodedPMA(d.bytes,d.length,w,h):decodeImageRaw(d, &w, &h);
    if (!buf) return JSValueMakeNull(ctx);
    JSObjectRef abuf = JSObjectMakeArrayBufferWithBytesNoCopy(ctx, buf, (size_t)w*h*4, [](void* p, void*){ free(p); }, nullptr, exc);
    JSObjectRef obj = JSObjectMake(ctx, nullptr, nullptr);
    auto setp = [&](const char* k, JSValueRef v){ JSStringRef s = JSStringCreateWithUTF8CString(k); JSObjectSetProperty(ctx, obj, s, v, kJSPropertyAttributeNone, nullptr); JSStringRelease(s); };
    setp("width", JSValueMakeNumber(ctx, w)); setp("height", JSValueMakeNumber(ctx, h)); setp("data", abuf);
    return obj;
}

// ======================= JSC 桥接（与 game_window.mm 等价；平台无关）=======================
static double gFrameDecodeUs = 0;   // 本帧 native 解码/GL 提交耗时(累计各次 flush，宿主每帧清零)
static double gFrameJSUs = 0, gFrameComputeUs = 0, gFrameBatchUs = 0, gFramePresentUs = 0;
static bool gFrameHadCmds = false;   // 本帧是否有 GL 命令(静止帧跳过时 JS 不渲染 → 跳过 present,保持屏幕)
static bool gFramePresented = false; // 本帧是否真的上屏
static bool gFrameHadContent = false;// 本帧是否产生了新画面(native 渲染 或 JS 发了 GL 命令)
// FPS 必须按"实际上屏"统计:早先它数的是 CADisplayLink 回调次数,于是"画满 draw call 却跳过
// present"时 FPS 照报 60、屏幕实际只有 7fps。但要区分两种不上屏:
//   · 静止帧——本帧没有新画面(gFrameHadContent=false),跳过 present 是省电优化,不是掉帧;
//   · 掉帧——有新画面却没上屏,那才是故障。只把后者当问题报。
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


// Read-only result of the most recently completed native main-screen paint.  JavaScript tree
// coverage alone cannot see failures discovered during C++ DFS (for example an unsupported nested
// post-process or a mask whose texture upload raced the frame).  The strict cold-start gate samples
// these counters as well, so T_ESCAPE=0 can no longer be a false positive while the batcher skipped
// visible content. Layout: serial, escapes, mask, type/post-process, texture, draws, quads.
static JSValueRef cbNativeBatchCoverage(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    // iOS 生产主屏由 flipMainScreenDraw 用 gBatcher 绘制(mac 历史命名里主屏是 gShadowBatcher)。
    // 冷启动闸门/T_ESCAPE 覆盖检查必须读真正画主屏的那个 batcher,否则恒为陈旧/全零=假阴性通过。
    JSValueRef values[7] = {
        JSValueMakeNumber(ctx, (double)gNativeBatchSerial),
        JSValueMakeNumber(ctx, (double)gBatcher.escapes),
        JSValueMakeNumber(ctx, (double)gBatcher.escMask),
        JSValueMakeNumber(ctx, (double)gBatcher.escType),
        JSValueMakeNumber(ctx, (double)gBatcher.escTex),
        JSValueMakeNumber(ctx, (double)gBatcher.drawCalls),
        JSValueMakeNumber(ctx, (double)gBatcher.quads)
    };
    return JSObjectMakeArray(ctx, 7, values, nullptr);
}

// Render a native scenegraph subtree into the RenderTexture FBO currently bound by Pixi's
// RenderTextureSystem. Pixi still owns allocation/lifetime of that texture, while it no longer
// traverses or paints the DisplayObject tree. Arguments: root, logicalW/H, clear, alphaScale,
// followed by an affine output matrix (a,b,c,d,tx,ty).
static JSValueRef cbNativeRenderTexturePreflight(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 1 || ![EAGLContext currentContext]) return JSValueMakeBoolean(ctx, false);
    int root = (int)JSValueToNumber(ctx, a[0], e);
    if (root <= 0 || root >= sg::NODE_CAP || !gSG.nodes[root].alive) return JSValueMakeBoolean(ctx, false);
    if (!gShadowBatcher.ready()) {
        gShadowBatcher.texLookup=[](int id){return gDec.glTextureEnsured((uint32_t)id);};
        gShadowBatcher.init();
    }
    return JSValueMakeBoolean(ctx, gShadowBatcher.preflight(&gSG, root));
}

static JSValueRef cbNativeRenderTexture(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if(argc<11||![EAGLContext currentContext])return JSValueMakeBoolean(ctx,false);
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


// ======================= zstd 解压桥(KTX2 supercompression=zstd 的原生解压)=======================
// JS 的 fzstd 解大图集卡 100-377ms;原生 zstd 快 10-20 倍。loadKTX2.ts 改调 __zstdDecompress。
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
// __zstdDecompress(srcTypedArray) -> ArrayBuffer(解压数据)或 null(失败,JS 回退 fzstd)
static JSValueRef cbZstdDecompress(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 1 || !JSValueIsObject(ctx, a[0])) return JSValueMakeNull(ctx);
    JSObjectRef srcObj = JSValueToObject(ctx, a[0], e);
    // JSObjectGetTypedArrayBytesPtr 返回的是底层 ArrayBuffer 的起始地址,不含视图自身的
    // 漏加 offset 就等于把"整个 KTX2 文件的开头 len 字节"当成 zstd 帧 → 必然解析失败,
    // 然后静默回退到纯 JS 的 fzstd 解压(实测 65/68 次走回退,每次几十毫秒;iOS 无 JIT 下更惨)。
    const uint8_t* srcBase = (const uint8_t*)JSObjectGetTypedArrayBytesPtr(ctx, srcObj, e);
    size_t srcOff = JSObjectGetTypedArrayByteOffset(ctx, srcObj, e);
    const uint8_t* src = srcBase ? srcBase + srcOff : nullptr;
    size_t srcLen = JSObjectGetTypedArrayByteLength(ctx, srcObj, e);
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
    return JSObjectMakeArrayBufferWithBytesNoCopy(ctx, dst, got, [](void* p, void*){ free(p); }, nullptr, e);
}

// ======================= 音频桥(HTMLAudio 垫片后端:AVAudioPlayer)=======================
// pixi-sound 无 AudioContext 自动走 legacy(HTMLAudio);JS 的 Audio 垫片每实例对应一个 AVAudioPlayer。
static dispatch_queue_t gLoadQ();                                       // 前向声明(异步加载队列,定义在后)
@interface AuDelegate : NSObject <AVAudioPlayerDelegate> @end
static NSMutableDictionary<NSNumber*, AVAudioPlayer*>* gAuPlayers = nil;
static NSMutableDictionary<NSNumber*, NSData*>* gAuData = nil;
static NSMutableSet<NSNumber*>* gAuSrcIds = nil;   // 源实例(url/data 加载载体):池不可回收——吞掉源会使后续 cloneNode 失败→pixi-sound 退化为每次 url 重载(新 NSData,池失效+堆积,越玩越卡)
static NSMutableDictionary<NSString*, NSData*>* gAuUrlCache = nil;   // url→字节:游戏封装播完即销毁 sound,每次播放都重新 load(实测 40/40 全走 url 路径);同 url 复用同一 NSData → 实例池按 bytes 指针即可命中,25ms 解码归零
static NSMutableSet<AVAudioPlayer*>* gAuBusy = nil;   // 逻辑占用:play 已入音频队列但可能未执行(playing 尚 false),防池把它当空闲抢走(队列化引入的竞态,实测复用率仅 11/89 的原因)
// These collections are main-thread only.  Pending URL reads are coalesced here so a burst of
// Audio clones cannot download/read the same asset several times before the first one completes.
static NSMutableDictionary<NSNumber*, NSString*>* gAuPendingUrls = nil;
static NSMutableSet<NSString*>* gAuLoadingUrls = nil;
static uint64_t gAuLoadEpoch = 1;   // reload/shutdown 推进世代:在途异步完成一律按陈旧丢弃
static AuDelegate* gAuDelegate = nil;
static dispatch_queue_t gAuQ();
static BOOL gAuBackgrounded = NO;
static BOOL gAuInterrupted = NO;
static BOOL gAuRecovering = NO;
static uint64_t gAuSessionEpoch = 1;
static NSMutableArray<dispatch_block_t>* gAuDeferredControls = nil;
static void auSetBackgrounded(BOOL backgrounded);
static void auResumeSession();
static void auRecoverMediaServices();
static void callAuEnded(int aid) {
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef nm = JSStringCreateWithUTF8CString("__auEnded");
    JSValueRef fv = JSObjectGetProperty(gCtx, global, nm, nullptr); JSStringRelease(nm);
    if (!JSValueIsObject(gCtx, fv)) return;
    JSObjectRef fn = JSValueToObject(gCtx, fv, nullptr);
    if (!fn || !JSObjectIsFunction(gCtx, fn)) return;
    JSValueRef arg = JSValueMakeNumber(gCtx, aid);
    JSObjectCallAsFunction(gCtx, fn, nullptr, 1, &arg, nullptr);
}
@implementation AuDelegate
- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer*)player successfully:(BOOL)flag {
    // 全部工作挪主线程:delegate 回调在播放器线程,直接遍历 gAuPlayers 与主线程写字典是竞态
    dispatch_async(dispatch_get_main_queue(), ^{
        if (gAuRecovering) return; // Orphaned players cannot finish a replacement voice.
        [gAuBusy removeObject:player];   // 逻辑占用解除 → 实例可入池复用
        NSNumber* found = nil;
        for (NSNumber* k in gAuPlayers) if (gAuPlayers[k] == player) { found = k; break; }
        if (found) callAuEnded(found.intValue);
    });
}
- (void)audioSessionInterrupted:(NSNotification*)notification {
    dispatch_async(dispatch_get_main_queue(), ^{
        const NSUInteger type = [notification.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
        gAuInterrupted = type == AVAudioSessionInterruptionTypeBegan;
        if (gAuInterrupted) {
            NSArray* players = gAuPlayers.allValues;
            dispatch_async(gAuQ(), ^{ for (AVAudioPlayer* player in players) [player pause]; });
        } else if ([notification.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue] &
                   AVAudioSessionInterruptionOptionShouldResume) {
            auResumeSession();
        }
    });
}
- (void)audioServicesReset:(NSNotification*)notification {
    dispatch_async(dispatch_get_main_queue(), ^{ auRecoverMediaServices(); });
}
@end
static BOOL auActivateSession() {
    NSError* error = nil;
    AVAudioSession* session = AVAudioSession.sharedInstance;
    if (![session setCategory:AVAudioSessionCategoryAmbient error:&error] ||
        ![session setActive:YES error:&error]) {
        fprintf(stderr, "[audio] session activation failed: %s\n", error.description.UTF8String ?: "unknown");
        return NO;
    }
    return YES;
}
static void auEnsureInit() {
    if (gAuPlayers) return;
    gAuPlayers = [NSMutableDictionary dictionary];
    gAuData = [NSMutableDictionary dictionary];
    gAuSrcIds = [NSMutableSet set];
    gAuUrlCache = [NSMutableDictionary dictionary];
    gAuBusy = [NSMutableSet set];
    gAuPendingUrls = [NSMutableDictionary dictionary];
    gAuLoadingUrls = [NSMutableSet set];
    gAuDelegate = [AuDelegate new];
    gAuDeferredControls = [NSMutableArray array];
    gAuBackgrounded = UIApplication.sharedApplication.applicationState == UIApplicationStateBackground;
    NSNotificationCenter* notifications = NSNotificationCenter.defaultCenter;
    [notifications addObserver:gAuDelegate selector:@selector(audioSessionInterrupted:)
                          name:AVAudioSessionInterruptionNotification object:nil];
    [notifications addObserver:gAuDelegate selector:@selector(audioServicesReset:)
                          name:AVAudioSessionMediaServicesWereResetNotification object:nil];
    if (!gAuBackgrounded) dispatch_async(gAuQ(), ^{ auActivateSession(); });
}
static dispatch_queue_t gAuQ();   // 控制串行队列(定义在 cbAuCtl 前)
// URL I/O and AVAudioPlayer decoding/preparation must not run on the JSC/render thread.  Keep this
// separate from gAuQ(): a slow remote request must not delay pause/stop/volume operations on
// players that are already live.  也独立于 gLoadQ:音频突发不挤占图片/spine 解码槽,反之亦然。
static dispatch_queue_t auLoadQueue() {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("pixi.audio.load", DISPATCH_QUEUE_CONCURRENT);
        dispatch_set_target_queue(queue,
            dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    });
    return queue;
}
static dispatch_semaphore_t auLoadSlots() {
    static dispatch_semaphore_t slots;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ slots = dispatch_semaphore_create(2); });
    return slots;
}
static void callAuDone(int aid, double duration) {
    if (!gCtx) return;
    JSValueRef args[2] = { JSValueMakeNumber(gCtx, aid), JSValueMakeNumber(gCtx, duration) };
    callJsGlobal("__auDone", 2, args);
}
// Pure per-player work: no global cache/player collections are touched, so this is safe on
// auLoadQueue.  The delegate is intentionally attached only when the player reaches main.
// (initWithData+prepareToPlay ~25ms 同步解码——主线程做它=坏帧,这正是异步管线的全部意义。)
static AVAudioPlayer* prepareAudioPlayer(int aid, NSData* data) {
    if (!data.length) return nil;
    NSError* error = nil;
    AVAudioPlayer* player = [[AVAudioPlayer alloc] initWithData:data error:&error];
    if (!player) {
        fprintf(stderr, "[audio] #%d async decode failed: %s\n", aid,
                error.localizedDescription.UTF8String ?: "?");
        return nil;
    }
    player.enableRate = YES;
    [player prepareToPlay];
    return player;
}
// Main-thread commit for a player prepared in the background.  All existing audio collections
// stay confined to main; subsequent AVAudioPlayer controls continue to use gAuQ().
static double installPreparedAudioPlayer(int aid, NSData* data, AVAudioPlayer* player) {
    if (!data.length || !player) return -1;
    auEnsureInit();
    NSNumber* key = @(aid);
    AVAudioPlayer* previous = gAuPlayers[key];
    if (previous && previous != player) {
        // 同 id 被顶替:旧 player 若还挂在 busy 集里会成为永远停不下来的僵尸循环音
        [gAuBusy removeObject:previous];
        dispatch_async(gAuQ(), ^{ [previous stop]; });
    }
    [gAuSrcIds removeObject:key];
    player.delegate = gAuDelegate;
    gAuPlayers[key] = player;
    gAuData[key] = data;
    return player.duration;
}
static void prepareAudioIdsAsync(NSArray<NSNumber*>* audioIds, NSString* url,
                                 NSData* data, uint64_t epoch) {
    if (gAuRecovering) {
        [gAuDeferredControls addObject:^{ prepareAudioIdsAsync(audioIds, url, data, epoch); }];
        return;
    }
    for (NSNumber* key in audioIds) {
        if (epoch != gAuLoadEpoch || ![gAuPendingUrls[key] isEqualToString:url]) continue;
        int aid = key.intValue;
        const uint64_t sessionEpoch = gAuSessionEpoch;
        dispatch_async(auLoadQueue(), ^{
            @autoreleasepool {
                dispatch_semaphore_wait(auLoadSlots(), DISPATCH_TIME_FOREVER);
                AVAudioPlayer* player = prepareAudioPlayer(aid, data);
                dispatch_semaphore_signal(auLoadSlots());
                dispatch_async(dispatch_get_main_queue(), ^{
                    // A shutdown/reload or superseding request makes this completion stale.
                    NSString* pendingUrl = gAuPendingUrls[key];
                    if (epoch != gAuLoadEpoch || ![pendingUrl isEqualToString:url]) return;
                    // A decoder prepared against the previous media server is unusable even
                    // when its URL request is current. Recreate it after the reset commits.
                    if (sessionEpoch != gAuSessionEpoch || gAuRecovering) {
                        dispatch_block_t retry = ^{ prepareAudioIdsAsync(@[key], url, data, epoch); };
                        if (gAuRecovering) [gAuDeferredControls addObject:retry];
                        else retry();
                        return;
                    }
                    [gAuPendingUrls removeObjectForKey:key];
                    double duration = installPreparedAudioPlayer(aid, data, player);
                    callAuDone(aid, duration);
                });
            }
        });
    }
}
static double auMakePlayer(int aid, NSData* data) {   // 返回 duration 秒,失败 -1
    if (!data || data.length == 0) return -1;
    // 实例复用池:pixi-sound 每次播放都 clone 新实例且从不 free,而 initWithData+prepareToPlay
    // 同一份字节(NSData 指针同,__auClone 共享)且未在播放的旧实例直接复用(seek 0,零解码),
    // 复用时移除旧 aid 绑定 → 字典规模自稳在"音效种类×并发峰值",顺带治实例泄漏。
    AVAudioPlayer* p = nil; NSNumber* reuseKey = nil;
    for (NSNumber* k in gAuPlayers) {
        if (gAuRecovering || [gAuSrcIds containsObject:k]) continue;
        if (gAuData[k] && gAuData[k].bytes == data.bytes) {
            AVAudioPlayer* cand = gAuPlayers[k];
            if (cand && !cand.playing && ![gAuBusy containsObject:cand]) { p = cand; reuseKey = k; break; }
        }
    }
    if (p) {
        [gAuPlayers removeObjectForKey:reuseKey]; [gAuData removeObjectForKey:reuseKey];
        p.currentTime = 0; p.volume = 1; p.numberOfLoops = 0; p.rate = 1;
    } else {
        NSError* err = nil;
        p = [[AVAudioPlayer alloc] initWithData:data error:&err];
        if (!p) { fprintf(stderr, "[audio] #%d 解码失败: %s\n", aid, err.localizedDescription.UTF8String ?: "?"); return -1; }
        p.enableRate = YES;
        p.delegate = gAuDelegate;
        [p prepareToPlay];
    }
    gAuPlayers[@(aid)] = p; gAuData[@(aid)] = data;
    return p.duration;
}
static NSData* loadAudioAsset(NSString* url){
    NSString* scheme=[NSURL URLWithString:url].scheme.lowercaseString;
    if([scheme isEqualToString:@"https"]||[scheme isEqualToString:@"http"]){
        NSData* bytes=fetchRemoteAsset([NSURL URLWithString:url]);
        if(!bytes)fprintf(stderr,"[audio] remote resource failed: %s\n",url.UTF8String);
        return bytes;
    }
    return loadAsset(url.UTF8String);
}
// __auLoadUrl(id, url) -> duration|-1(同步:资源走本地/缓存/LAN,loadAsset 已有)
static JSValueRef cbAuLoadUrl(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeNumber(ctx, -1);
    auEnsureInit();
    int aid = (int)JSValueToNumber(ctx, a[0], e);
    NSString* url = jsToNSString(ctx, a[1]);
    NSData* d = gAuUrlCache[url];
    if (!d) { d = loadAudioAsset(url); if (d) gAuUrlCache[url] = d; }
    // url 路径不打"源"标:实测(40/40)该路径 load 完直接播、从不 cloneNode——打标会使实例永不入池,
    // 池就永不命中;url 字节缓存保证指针一致,空闲实例照常回收复用。
    return JSValueMakeNumber(ctx, auMakePlayer(aid, d));
}
// __auLoadUrlAsync(id, url) -> undefined; completion is __auDone(id, duration|-1).
// 实测 resume_scene_bgm 同步加载均 160ms 冻结主线程(关弹窗恢复 BGM 必现)。
// 字节读与 initWithData+prepareToPlay(~25ms)全部后台化,主线程只做集合登记;
// 同 URL 并发合流(一次字节读 fan-out 给所有等待 id),命中 gAuUrlCache 直接跳过 I/O
// (指针一致→实例池才可能命中);epoch+pendingUrl 双重校验丢弃跨 reload/被顶替的陈旧完成。
static JSValueRef cbAuLoadUrlAsync(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeUndefined(ctx);
    auEnsureInit();
    int aid = (int)JSValueToNumber(ctx, a[0], e);
    NSString* url = [jsToNSString(ctx, a[1]) copy];
    if (aid <= 0 || !url.length) {
        dispatch_async(dispatch_get_main_queue(), ^{ callAuDone(aid, -1); });
        return JSValueMakeUndefined(ctx);
    }
    NSNumber* key = @(aid);
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
    dispatch_async(auLoadQueue(), ^{
        @autoreleasepool {
            dispatch_semaphore_wait(auLoadSlots(), DISPATCH_TIME_FOREVER);
            NSData* data = loadAudioAsset(url);
            dispatch_semaphore_signal(auLoadSlots());
            dispatch_async(dispatch_get_main_queue(), ^{
                if (epoch != gAuLoadEpoch) return;
                [gAuLoadingUrls removeObject:url];
                NSArray<NSNumber*>* ids = [gAuPendingUrls allKeysForObject:url];
                if (!data.length) {
                    for (NSNumber* pendingKey in ids) {
                        [gAuPendingUrls removeObjectForKey:pendingKey];
                        callAuDone(pendingKey.intValue, -1);
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
// __auLoadData(id, ArrayBuffer|TypedArray) -> duration|-1(zip 内音频字节)
static JSValueRef cbAuLoadData(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2 || !JSValueIsObject(ctx, a[1])) return JSValueMakeNumber(ctx, -1);
    auEnsureInit();
    int aid = (int)JSValueToNumber(ctx, a[0], e);
    NSData* d = jsValueToData(ctx, a[1], e);
    double dur = auMakePlayer(aid, d);
    if (dur >= 0) [gAuSrcIds addObject:@(aid)];   // 源实例打标:池不可回收
    return JSValueMakeNumber(ctx, dur);
}
// __auClone(newId, srcId) -> duration|-1(共享字节,新解码器实例;多实例并发播放)
static JSValueRef cbAuClone(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeNumber(ctx, -1);
    auEnsureInit();
    int nid = (int)JSValueToNumber(ctx, a[0], e);
    int sid = (int)JSValueToNumber(ctx, a[1], e);
    return JSValueMakeNumber(ctx, auMakePlayer(nid, gAuData[@(sid)]));
}
// __auCtl(id, op, v1, v2):op 0=play(v1=loop,v2=fromSec) 1=pause 2=stop 3=volume(v1) 4=rate(v1) 5=seek(v1) 6=free;返回 currentTime(op7=get)
// 音频操作串行队列:[AVAudioPlayer play] 本身在主线程同步 ~25ms(音频单元启动),
// 异步派发(串行保序,音画误差几 ms 无感);op7(currentTime 读)保持同步直读。
static dispatch_queue_t gAuQ() {
    static dispatch_queue_t q; static dispatch_once_t o;
    dispatch_once(&o, ^{ q = dispatch_queue_create("pixi.audio", DISPATCH_QUEUE_SERIAL); });
    return q;
}
// Reset notifications invalidate every AVAudioPlayer, not just the AVAudioSession. Keep the
// JS ids, bytes and playback state, replace decoders off the render thread, then replay controls
// against the new players. A reload cancels the entire recovery via gAuLoadEpoch.
static void auRecoverMediaServices() {
    if (!gAuPlayers) return;
    ++gAuSessionEpoch;
    if (gAuRecovering) return;
    gAuRecovering = YES;
    const uint64_t epoch = gAuLoadEpoch;
    const uint64_t sessionEpoch = gAuSessionEpoch;
    NSDictionary* oldPlayers = [gAuPlayers copy];
    NSDictionary* data = [gAuData copy];
    NSSet* busy = [gAuBusy copy];
    const BOOL activate = !gAuBackgrounded && !gAuInterrupted;
    fprintf(stderr, "[audio] media services reset: rebuilding %lu players\n", (unsigned long)oldPlayers.count);
    dispatch_async(gAuQ(), ^{
        NSMutableDictionary* rebuilt = [NSMutableDictionary dictionary];
        if (activate) auActivateSession();
        for (NSNumber* key in oldPlayers) {
            AVAudioPlayer* old = oldPlayers[key];
            const NSTimeInterval position = old.currentTime;
            const float volume = old.volume, rate = old.rate;
            const NSInteger loops = old.numberOfLoops;
            old.delegate = nil;
            [old stop];
            NSError* error = nil;
            AVAudioPlayer* player = [[AVAudioPlayer alloc] initWithData:data[key] error:&error];
            if (!player) {
                fprintf(stderr, "[audio] #%d reset decode failed: %s\n", key.intValue,
                        error.description.UTF8String ?: "unknown");
                continue;
            }
            player.enableRate = YES;
            player.volume = volume; player.rate = rate; player.numberOfLoops = loops;
            player.currentTime = position;
            rebuilt[key] = player;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (epoch != gAuLoadEpoch) return;
            for (NSNumber* key in oldPlayers) {
                AVAudioPlayer* old = oldPlayers[key];
                if (gAuPlayers[key] != old) continue;
                [gAuBusy removeObject:old];
                AVAudioPlayer* player = rebuilt[key];
                if (player) {
                    player.delegate = gAuDelegate;
                    gAuPlayers[key] = player;
                    if ([busy containsObject:old]) [gAuBusy addObject:player];
                } else [gAuPlayers removeObjectForKey:key];
            }
            gAuRecovering = NO;
            // A second reset can arrive during decode. Keep the queued controls until
            // replacements have been rebuilt against the latest media server as well.
            if (sessionEpoch != gAuSessionEpoch) { auRecoverMediaServices(); return; }
            NSArray<dispatch_block_t>* pending = [gAuDeferredControls copy];
            [gAuDeferredControls removeAllObjects];
            for (dispatch_block_t control in pending) control();
            auResumeSession();
            fprintf(stderr, "[audio] media services recovery complete: %lu/%lu players\n",
                    (unsigned long)rebuilt.count, (unsigned long)oldPlayers.count);
        });
    });
}
static void auResumeSession() {
    if (!gAuPlayers || gAuBackgrounded || gAuInterrupted || gAuRecovering) return;
    NSArray* playing = gAuBusy.allObjects;
    dispatch_async(gAuQ(), ^{
        if (!auActivateSession()) return;
        for (AVAudioPlayer* player in playing) {
            if (!player.playing && ![player play])
                fprintf(stderr, "[audio] resume failed for player=%p\n", player);
        }
    });
}
static void auSetBackgrounded(BOOL backgrounded) {
    gAuBackgrounded = backgrounded;
    if (!gAuPlayers || gAuRecovering) return;
    if (!backgrounded) { auResumeSession(); return; }
    NSArray* players = gAuPlayers.allValues;
    dispatch_async(gAuQ(), ^{
        for (AVAudioPlayer* player in players) [player pause];
        NSError* error = nil;
        if (![AVAudioSession.sharedInstance setActive:NO
                withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:&error])
            fprintf(stderr, "[audio] session deactivation failed: %s\n", error.description.UTF8String ?: "unknown");
    });
}
static void auControl(int aid, int op, double v1, double v2) {
    if (op == 6) [gAuPendingUrls removeObjectForKey:@(aid)];
    if (gAuRecovering) {
        [gAuDeferredControls addObject:^{ auControl(aid, op, v1, v2); }];
        return;
    }
    AVAudioPlayer* p = gAuPlayers[@(aid)];
    if (!p) return;
    switch (op) {
        case 0: {
            [gAuBusy addObject:p];
            const BOOL suspended = gAuBackgrounded || gAuInterrupted;
            dispatch_async(gAuQ(), ^{
                p.numberOfLoops = v1 != 0 ? -1 : 0;
                if (v2 > 0) p.currentTime = v2;
                if (!suspended && ![p play])
                    fprintf(stderr, "[audio] #%d play failed\n", aid);
            }); break;
        }
        case 1: { [gAuBusy removeObject:p]; dispatch_async(gAuQ(), ^{ [p pause]; }); break; }
        case 2: { [gAuBusy removeObject:p]; dispatch_async(gAuQ(), ^{ [p pause]; p.currentTime = 0; }); break; }
        case 3: { dispatch_async(gAuQ(), ^{ p.volume = (float)std::clamp(v1, 0.0, 1.0); }); break; }
        case 4: { if (v1 >= 0.5 && v1 <= 2.0) dispatch_async(gAuQ(), ^{ p.rate = (float)v1; }); break; }
        case 5: { dispatch_async(gAuQ(), ^{ p.currentTime = std::max(0.0, v1); }); break; }
        case 6: {
            [gAuPlayers removeObjectForKey:@(aid)]; [gAuData removeObjectForKey:@(aid)];
            [gAuSrcIds removeObject:@(aid)]; [gAuBusy removeObject:p];
            dispatch_async(gAuQ(), ^{ [p stop]; }); break;
        }
    }
}
static JSValueRef cbAuCtl(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeNumber(ctx, 0);
    int aid = (int)JSValueToNumber(ctx, a[0], e);
    int op  = (int)JSValueToNumber(ctx, a[1], e);
    AVAudioPlayer* p = gAuPlayers ? gAuPlayers[@(aid)] : nil;
    double v1 = argc > 2 ? JSValueToNumber(ctx, a[2], e) : 0;
    double v2 = argc > 3 ? JSValueToNumber(ctx, a[3], e) : 0;
    if (op == 7) return JSValueMakeNumber(ctx, p ? p.currentTime : 0);
    auControl(aid, op, v1, v2);
    return JSValueMakeNumber(ctx, 0);
}

// reload/退出:推进世代(在途异步完成全部作废)+ 停播 + 清全部集合。旧世界循环 BGM 决不能
// 穿越 reload 继续响,旧 aid 也决不能撞上新世界从头计的 id。
static void shutdownAudio() {
    if (!gAuPlayers) return;
    ++gAuLoadEpoch;
    gAuRecovering = NO;
    [gAuDeferredControls removeAllObjects];
    NSArray<AVAudioPlayer*>* players = gAuPlayers.allValues;
    [gAuPlayers removeAllObjects];
    [gAuData removeAllObjects];
    [gAuSrcIds removeAllObjects];
    [gAuBusy removeAllObjects];
    [gAuUrlCache removeAllObjects];
    [gAuPendingUrls removeAllObjects];
    [gAuLoadingUrls removeAllObjects];
    dispatch_async(gAuQ(), ^{ for (AVAudioPlayer* player in players) [player stop]; });
}
// 提交原子性(与 mac cbNativePresent 同语义):在 renderer.render() 的 JS 调用点内联取景绘制。
// JS 在 render 之后、__tick 结束之前的变更(shared ticker 上的停轮/中奖工作流、微任务续体、
// 手动 GTimer)一律落到下一帧,与浏览器"render 即采样"的时序一致。此前 iOS 绑定的是帧尾
// 延迟队列(cbSgFlagRender),render 后中间态会提前一帧上屏——mac 曾以内联取景根治 m90004
static void flipMainScreenDraw(int root);
static JSValueRef cbNativePresent(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc >= 1) {
        int root = (int)JSValueToNumber(ctx, a[0], e);
        if (argc >= 5) for (int i = 0; i < 4; i++) gSgClear[i] = (float)JSValueToNumber(ctx, a[1 + i], e);
        // 与 mac 同款:帧相位推进与 draw 执行解耦(present 点=seal 窗口结束;PHB 停留 6 会把
        // 此后同帧业务写误计 lateMut)。
        if (gSG.buf) gSG.buf[sg::FRAME_PHASE_BASE] = 7.0f;
        if (root > 0) flipMainScreenDraw(root);
    }
    return JSValueMakeUndefined(ctx);
}
// M2:JS 每 N 帧调一次,标记"本帧把 root 树画进离屏影子 FBO"(供 M3 与真屏对拍;不上屏、不短路 pixi)。
static JSValueRef cbShadowFlag(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc>0) gShadowRoot = (int)JSValueToNumber(ctx, a[0], e);
    if (argc>1) gShadowShow = (int)JSValueToNumber(ctx, a[1], e);   // 0关/1blit目视/2 flip上屏
    return JSValueMakeUndefined(ctx);
}
// ======================= ZIP 桥(JSZip.loadAsync 后端:后台线程 zlib 解压)=======================
// __zipParse(token, ArrayBuffer) → 后台解析 central directory + 全条目 inflate → __zipDone(token, namesJson|null);
// __zipGet(token, name, asString) → 同步取条目(已解压,仅拷贝)。zip64/未知压缩法返回 null → JS 端回落原始 JSZip。
// 缓存 LRU 8 个 zip(游戏切场景不释放 zip 对象,防堆积)。
static NSMutableDictionary<NSNumber*, NSDictionary<NSString*, NSData*>*>* gZipCache = nil;
static NSMutableArray<NSNumber*>* gZipLru = nil;
NSDictionary<NSString*, NSData*>* zipParseAll(NSData* d){
    const uint8_t* p = (const uint8_t*)d.bytes; size_t n = d.length;
    if (n < 22) return nil;
    ssize_t eo = -1;   // EOCD:末尾向前搜 PK\x05\x06(最多回溯 64KB 注释区)
    ssize_t lo = (ssize_t)n - 22 - 65536; if (lo < 0) lo = 0;
    for (ssize_t i = (ssize_t)n - 22; i >= lo; i--)
        if (p[i]==0x50 && p[i+1]==0x4b && p[i+2]==0x05 && p[i+3]==0x06) { eo = i; break; }
    if (eo < 0) return nil;
    uint16_t total; memcpy(&total, p+eo+10, 2);
    uint32_t cdOff; memcpy(&cdOff, p+eo+16, 4);
    if (total == 0xFFFF || cdOff == 0xFFFFFFFF) return nil;   // zip64 → JS 兜底
    NSMutableDictionary* out = [NSMutableDictionary dictionary];
    size_t off = cdOff;
    for (int i = 0; i < total; i++) {
        if (off + 46 > n || !(p[off]==0x50 && p[off+1]==0x4b && p[off+2]==0x01 && p[off+3]==0x02)) return nil;
        uint16_t method; memcpy(&method, p+off+10, 2);
        uint32_t csize;  memcpy(&csize,  p+off+20, 4);
        uint32_t usize;  memcpy(&usize,  p+off+24, 4);
        uint16_t nlen, elen, clen; memcpy(&nlen, p+off+28, 2); memcpy(&elen, p+off+30, 2); memcpy(&clen, p+off+32, 2);
        uint32_t lho; memcpy(&lho, p+off+42, 4);
        if (off + 46 + nlen > n) return nil;
        NSString* name = [[NSString alloc] initWithBytes:p+off+46 length:nlen encoding:NSUTF8StringEncoding];
        off += 46u + nlen + elen + clen;
        if (!name || [name hasSuffix:@"/"]) continue;   // 目录条目跳过
        if (lho + 30 > n || !(p[lho]==0x50 && p[lho+1]==0x4b && p[lho+2]==0x03 && p[lho+3]==0x04)) return nil;
        uint16_t lnlen, lelen; memcpy(&lnlen, p+lho+26, 2); memcpy(&lelen, p+lho+28, 2);
        size_t dataOff = (size_t)lho + 30 + lnlen + lelen;
        if (dataOff + csize > n) return nil;
        NSData* content = nil;
        if (method == 0) content = [NSData dataWithBytes:p+dataOff length:csize];
        else if (method == 8) {
            NSMutableData* u = [NSMutableData dataWithLength:usize];
            z_stream zs; memset(&zs, 0, sizeof(zs));
            zs.next_in = (Bytef*)(p+dataOff); zs.avail_in = csize;
            zs.next_out = (Bytef*)u.mutableBytes; zs.avail_out = usize;
            if (inflateInit2(&zs, -15) != Z_OK) return nil;   // -15 = raw deflate
            int r = inflate(&zs, Z_FINISH); size_t got = zs.total_out; inflateEnd(&zs);
            if (r != Z_STREAM_END || got != usize) return nil;
            content = u;
        } else return nil;   // 未知压缩法 → JS 兜底
        out[name] = content;
    }
    return out;
}
static JSValueRef cbZipParse(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeUndefined(ctx);
    int token = (int)JSValueToNumber(ctx, a[0], e);
    NSData* d = jsValueToData(ctx, a[1], e);
    dispatch_async(gLoadQ(), ^{
        dispatch_semaphore_wait(assetCpuSlots(), DISPATCH_TIME_FOREVER);
        NSDictionary<NSString*, NSData*>* entries = d ? zipParseAll(d) : nil;
        dispatch_semaphore_signal(assetCpuSlots());
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!gCtx) return;
            JSValueRef namesV = JSValueMakeNull(gCtx);
            if (entries) {
                if (!gZipCache) { gZipCache = [NSMutableDictionary dictionary]; gZipLru = [NSMutableArray array]; }
                gZipCache[@(token)] = entries; [gZipLru addObject:@(token)];
                while (gZipLru.count > 8) { [gZipCache removeObjectForKey:gZipLru.firstObject]; [gZipLru removeObjectAtIndex:0]; }
                NSData* j = [NSJSONSerialization dataWithJSONObject:entries.allKeys options:0 error:nil];
                if (j) { NSString* js = [[NSString alloc] initWithData:j encoding:NSUTF8StringEncoding];
                    JSStringRef s = JSStringCreateWithUTF8CString(js.UTF8String);
                    namesV = JSValueMakeString(gCtx, s); JSStringRelease(s); }
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
    NSString* name = jsToNSString(ctx, a[1]);
    bool asString = argc > 2 && JSValueToNumber(ctx, a[2], e) != 0;
    NSData* d = gZipCache ? gZipCache[@(token)][name] : nil;
    if (!d) return JSValueMakeNull(ctx);
    if (asString) {
        NSString* s = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
        if (!s) return JSValueMakeNull(ctx);
        JSStringRef js = JSStringCreateWithUTF8CString(s.UTF8String);
        JSValueRef v = JSValueMakeString(ctx, js); JSStringRelease(js); return v;
    }
    size_t len = d.length; void* buf = malloc(len); if (!buf) return JSValueMakeNull(ctx);
    memcpy(buf, d.bytes, len);
    JSObjectRef ab = JSObjectMakeArrayBufferWithBytesNoCopy(ctx, buf, len, [](void* p2, void*){ free(p2); }, nullptr, e);
    return ab ? (JSValueRef)ab : JSValueMakeNull(ctx);
}

static JSValueRef cbLoadImage(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                              const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeNull(ctx);
    JSStringRef ps = JSValueToStringCopy(ctx, args[0], exc);
    size_t n = JSStringGetMaximumUTF8CStringSize(ps); std::vector<char> pb(n);
    JSStringGetUTF8CString(ps, pb.data(), n); JSStringRelease(ps);
    NSData* d = loadAsset(pb.data());
    JSValueRef r = decodeImageData(ctx, d, exc,argc>1&&JSValueToBoolean(ctx,args[1]));
    if (JSValueIsNull(ctx, r)) fprintf(stderr, "[game] __loadImage 失败: %.96s\n", pb.data());
    return r;
}
// ---- 异步资源加载:LAN 拉取 + 图片解码全在后台并发队列,主线程只回调 JS ----
// reload 安全:回调经主线程查 JS 端 token 表,旧上下文的 token 在新世界查不到 → 自然忽略。
static dispatch_queue_t gLoadQ() {
    static dispatch_queue_t q; static dispatch_once_t o;
    dispatch_once(&o, ^{ q = dispatch_queue_create("pixi.asset.load", DISPATCH_QUEUE_CONCURRENT); });
    return q;
}
void callJsGlobal(const char* fname, int argc, const JSValueRef* args){   // 主线程调用 JS 全局函数
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef nm = JSStringCreateWithUTF8CString(fname);
    JSValueRef fv = JSObjectGetProperty(gCtx, global, nm, nullptr); JSStringRelease(nm);
    if (!JSValueIsObject(gCtx, fv)) return;
    JSObjectRef fn = JSValueToObject(gCtx, fv, nullptr);
    if (!fn || !JSObjectIsFunction(gCtx, fn)) return;
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(gCtx, fn, nullptr, argc, args, &exc);
    if (exc) { JSStringRef es = JSValueToStringCopy(gCtx, exc, nullptr); size_t n = JSStringGetMaximumUTF8CStringSize(es);
        std::vector<char> b(n); JSStringGetUTF8CString(es, b.data(), n);
        fprintf(stderr, "[async-load] %s 回调异常: %s\n", fname, b.data()); JSStringRelease(es); }
}
// 送达真值版:返回"目标函数存在且完成了调用"。携带所有权引用的回调(publish 的 cid)必须用
// 它——__imgDone 缺失(上下文半初始化窗口)时明确返回 false,调用方据此退还引用;JS 内部抛
// 异常不判"未送达"(所有权可能已在 JS 侧转移,退还等于偷走别人的引用),沿用上面的异常曝光。
bool callJsGlobalChecked(const char* fname, int argc, const JSValueRef* args){
    if (!gCtx) return false;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef nm = JSStringCreateWithUTF8CString(fname);
    JSValueRef fv = JSObjectGetProperty(gCtx, global, nm, nullptr); JSStringRelease(nm);
    if (!JSValueIsObject(gCtx, fv)) return false;
    JSObjectRef fn = JSValueToObject(gCtx, fv, nullptr);
    if (!fn || !JSObjectIsFunction(gCtx, fn)) return false;
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(gCtx, fn, nullptr, argc, args, &exc);
    if (exc) { static int excN = 0; if (++excN <= 8) fprintf(stderr, "[async-load] %s 回调抛异常(所有权按已送达处理,第 %d 次)\n", fname, excN); }
    return true;
}


sg::CommandReceipt freeSceneNodeFromJs(int id, uint32_t expectedGeneration){
    const sg::CommandReceipt preflight = gSG.validateFreeNode(id, expectedGeneration);
    if (preflight != sg::CommandReceipt::Applied) return preflight;
    // 已材料化的 GObject 子节点可能被单独 dispose,而拥有它的 Native FGUI 实例还活着。那个门面归 JS
    // 管,C++ 编译出来的槽位不归它。在这里释放槽位会让 managedNodeOwners_ 指向一个立刻被 SceneGraph
    // 回收复用的 id;下一个组件的归属索引随即失败(此后所有弹窗一起回退)。所以把"部分 dispose"降级成
    // detach,让 Runtime::destroyTree 成为唯一的释放权威。
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

// __loadImageAsync(url, token) → ResourceManager 拉取+解码+分带上传 → __imgDone(token, w, h, null, cid)
static JSValueRef cbLoadImageAsync(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                   const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeUndefined(ctx);
    NSString* url = jsToNSString(ctx, a[0]);
    int token = (int)JSValueToNumber(ctx, a[1], e);
    JSGlobalContextRef requestContext = gCtx;
    // Pixi 的格式探测用极小的 data: URL。宿主刻意不实现 data-URI 解码;下一个主线程回合直接判失败,
    // 免得特性探测排在原生资源队列后面干等。
    if ([url hasPrefix:@"data:"]) {
        dispatch_async(dispatch_get_main_queue(), ^{ finishNativeImageAsync(requestContext, token, 0, 0, 0); });
        return JSValueMakeUndefined(ctx);
    }
    gNativeResources.requestImageURL(url, [requestContext, token, url](uint32_t cid, int width, int height) {
        if (!cid) fprintf(stderr, "[game] native image prepare 失败: %.96s\n", url.UTF8String);
        deliverPublishedImage(requestContext, token, cid, width, height);
    }, /*publishRef=*/true,argc>2&&JSValueToBoolean(ctx,a[2]));
    return JSValueMakeUndefined(ctx);
}
// __readFileAsync(url, token) → ResourceManager 后台拉取(带字节缓存/并发合流) → __fileDone(token, ArrayBuffer|null)
static JSValueRef cbReadFileAsync(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                  const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeUndefined(ctx);
    NSString* url = jsToNSString(ctx, a[0]);
    int token = (int)JSValueToNumber(ctx, a[1], e);
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


static std::string sha256Hex(NSData* data) {
    if (!data) return {};
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    static const char hex[] = "0123456789abcdef";
    std::string result(CC_SHA256_DIGEST_LENGTH * 2, '0');
    for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 15];
    }
    return result;
}


// The JS-visible global fetch is the asset-read adapter (it ignores method/body), so telemetry
// must go through a real NSURLSession POST. The task never touches gCtx, so a reload right
// after (e.g. following "activated") cannot drop it. With a token, delivery is acknowledged
// back to JS only on HTTP 2xx — the one-shot rollback report consumes its marker on that ack.


static JSValueRef cbDecodeImage(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeNull(ctx);
    NSData* d = jsValueToData(ctx, args[0], exc);
    JSValueRef r = decodeImageData(ctx, d, exc,argc>1&&JSValueToBoolean(ctx,args[1]));
    if (JSValueIsNull(ctx, r)) fprintf(stderr, "[game] __decodeImage 失败: %lu 字节\n", (unsigned long)d.length);
    return r;
}
static JSValueRef cbXhrSend(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
    if (argc < 3) return JSValueMakeUndefined(ctx);
    int reqId = (int)JSValueToNumber(ctx, args[0], exc);
    NSString* method = jsToNSString(ctx, args[1]);
    NSString* urlStr = jsToNSString(ctx, args[2]);
    NSString* headersJson = (argc > 3) ? jsToNSString(ctx, args[3]) : @"{}";
    NSData* body = (argc > 4) ? jsValueToData(ctx, args[4], exc) : nil;
    NSString* scheme=[NSURL URLWithString:urlStr].scheme.lowercaseString;
    bool explicitNetwork=[scheme isEqualToString:@"http"]||[scheme isEqualToString:@"https"];
    if (!explicitNetwork && [method caseInsensitiveCompare:@"GET"] == NSOrderedSame) {
        NSString* rel = relOf(urlStr);

        NSData* local = localAssetData(rel);
        if (local) {
            NSData* lb = local;
            dispatch_async(dispatch_get_main_queue(), ^{ callXhrDone(reqId, 200, @"OK", @"{}", lb); });
            return JSValueMakeUndefined(ctx);
        }
        // GET 静态资源未命中 bundle → 一律改走局域网镜像 gRemote/<rel>，
        if (rel.length && gRemote.length) {
            NSString* lan = [NSString stringWithFormat:@"%@/%@", gRemote, rel];
            urlStr = [lan stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLQueryAllowedCharacterSet]] ?: lan;
        }
    }
    if (gLogNet) fprintf(stderr, "[xhr-net] #%d %s %.140s\n", reqId, method.UTF8String, urlStr.UTF8String);
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
            dispatch_async(dispatch_get_main_queue(), ^{ callXhrDone(reqId, status, statusText, rhJson, respBody); });
        }];
    [task resume];
    return JSValueMakeUndefined(ctx);
}


static JSValueRef cbNativeFguiPreparePackage(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3||!JSValueIsObject(ctx,a[1])||!JSValueIsObject(ctx,a[2])||JSValueGetTypedArrayType(ctx,a[2],exc)!=kJSTypedArrayTypeFloat32Array)return JSValueMakeBoolean(ctx,false);
    NSString* pkg=jsToNSString(ctx,a[0]);JSObjectRef ids=JSValueToObject(ctx,a[1],exc),meta=JSValueToObject(ctx,a[2],exc);
    JSStringRef lengthKey=JSStringCreateWithUTF8CString("length");size_t count=(size_t)JSValueToNumber(ctx,JSObjectGetProperty(ctx,ids,lengthKey,exc),exc);JSStringRelease(lengthKey);
    const float* values=(const float*)JSObjectGetTypedArrayBytesPtr(ctx,meta,exc);size_t valueCount=JSObjectGetTypedArrayLength(ctx,meta,exc);if(!values||valueCount<count*4)return JSValueMakeBoolean(ctx,false);
    std::vector<std::pair<std::string,nfgui::AtlasBinding>> atlases;atlases.reserve(count);
    for(size_t i=0;i<count;i++){
        JSValueRef idValue=JSObjectGetPropertyAtIndex(ctx,ids,(unsigned)i,exc);NSString* idString=jsToNSString(ctx,idValue);nfgui::AtlasBinding binding;binding.textureId=(int)values[i*4];binding.width=values[i*4+1];binding.height=values[i*4+2];binding.premul=values[i*4+3];atlases.emplace_back(idString.UTF8String?:"",binding);
        if(std::getenv("NATIVE_ATLAS_TRACE")){
            // iOS 的 OpenGLES 框架只到 ES 3.0,没有 glGetTexLevelParameteriv(ES 3.1 起才有),拿不到
            // 纹理实际尺寸。这里只打 JS 上报的绑定信息;"上报尺寸 vs 实际尺寸"的对拍留在 mac 宿主做。
            GLuint texture=gDec.glTexture((uint32_t)std::max(0,binding.textureId));
            std::fprintf(stderr,"[native-atlas] package=%s atlas=%s cid=%d gl=%u reported=%.0fx%.0f premul=%.0f\n",pkg.UTF8String?:"",idString.UTF8String?:"",binding.textureId,(unsigned)texture,binding.width,binding.height,binding.premul);
        }
    }
    gNativeFgui.invalidateReservationsForPackage(pkg.UTF8String?:"",gSG);
    return JSValueMakeBoolean(ctx,gNativeFgui.preparePackage(pkg.UTF8String?:"",atlases,&gSG));
}
// __nativeOpenURL(url) → bool。window.open 桥(FAN PAGE/SUPPORT/条款等出站链接):仅放行
// http/https,交给系统浏览器。openURL 结果异步,URL 合法即乐观返回 true(≙ mac NSWorkspace 语义)。
static JSValueRef cbNativeOpenURL(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<1)return JSValueMakeBoolean(ctx,false);
    JSStringRef ps=JSValueToStringCopy(ctx,a[0],exc);if(!ps)return JSValueMakeBoolean(ctx,false);
    size_t n=JSStringGetMaximumUTF8CStringSize(ps);std::vector<char> pb(n);
    JSStringGetUTF8CString(ps,pb.data(),n);JSStringRelease(ps);
    std::string url(pb.data());
    if(url.rfind("http://",0)!=0&&url.rfind("https://",0)!=0)return JSValueMakeBoolean(ctx,false);
    NSString* s=[NSString stringWithUTF8String:url.c_str()];
    NSURL* u=s?[NSURL URLWithString:s]:nil;
    if(!u){fprintf(stderr,"[open-url] %s -> fail\n",url.c_str());return JSValueMakeBoolean(ctx,false);}
    dispatch_async(dispatch_get_main_queue(),^{
        [[UIApplication sharedApplication] openURL:u options:@{} completionHandler:^(BOOL ok){
            fprintf(stderr,"[open-url] %s -> %s\n",u.absoluteString.UTF8String?:"",ok?"ok":"fail");
        }];
    });
    return JSValueMakeBoolean(ctx,true);
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
    dispatch_async(gLoadQ(),^{
        NativeFguiPreparsed* parsed=nil;@autoreleasepool{dispatch_semaphore_wait(assetCpuSlots(),DISPATCH_TIME_FOREVER);parsed=parseNativeFguiArchive(data,true,key);dispatch_semaphore_signal(assetCpuSlots());}
        dispatch_async(dispatch_get_main_queue(),^{
            auto found=gFguiPreparseInflight.find(workKey);if(found==gFguiPreparseInflight.end())return;std::vector<NativeFguiPreparseWaiter> waiters=std::move(found->second);gFguiPreparseInflight.erase(found);
            if(gCtx!=requestContext){return;}bool ok=parsed!=nil;if(ok){cacheNativeFguiPreparse(resKey,parsed);gFguiPreparseStats.backgroundInflateMs+=parsed.inflateMs;gFguiPreparseStats.backgroundXmlMs+=parsed.xmlMs;gFguiPreparseStats.backgroundBlueprintMs+=parsed.blueprintMs;gFguiPreparseStats.lastInflateMs=parsed.inflateMs;}else gFguiPreparseStats.failures++;
            if(getenv("FGUI_PREPARSE_LOG"))fprintf(stderr,"[fgui-preparse/ios] READY %s ok=%d entries=%lu inflate=%.2fms xml=%.2fms blueprint=%.2fms waiters=%zu\n",resKey.UTF8String?:"",ok?1:0,(unsigned long)(parsed?parsed.entryCount:0),parsed?parsed.inflateMs:0,parsed?parsed.xmlMs:0,parsed?parsed.blueprintMs:0,waiters.size());
            for(const auto& pending:waiters)notifyNativeFguiPreparse(pending,ok,parsed);
        });
    });
    return JSValueMakeBoolean(ctx,true);
}


static uint64_t gSpinePrepareEpoch=1;
static void spinexPrewarm(NativeSpinePrepared* parsed);   // 定义在 spinex 段(preparse 就绪即后台起解析)


static JSValueRef cbNativeSpinePreparse(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);int token=(int)JSValueToNumber(ctx,args[0],exc);NSString* url=jsToNSString(ctx,args[1]),*key=jsToNSString(ctx,args[2]);NSData* data=jsValueToData(ctx,args[3],exc);
    if(!url.length||!key.length||!data.length)return JSValueMakeBoolean(ctx,false);gSpinePrepareStats.requests++;uint32_t inputCrc=nativeSpineInputCrc(data);NSString* cacheKey=nativeSpineCacheKey(url,key);
    bool wantsLease=argc>4&&JSValueToBoolean(ctx,args[4]);   // 只有声明读 holder 的消费链(prepared wrapper)才发 lease;legacy 链只用原始字节
    NativeSpinePrepareWaiter waiter{token,gCtx,wantsLease};NativeSpinePrepared* cached=gSpinePreparedCache[cacheKey];
    if(cached&&cached.compressedBytes==data.length&&cached.inputCrc==inputCrc){gSpinePrepareStats.cacheHits++;touchNativeSpinePrepared(cached);
        if(wantsLease)cached.activeUsers=cached.activeUsers+1;   // 在途 lease:入队前同步获取(排队期间的插入逐出不能挤掉它),notify 交付或退还
        // A prepared ZIP outlives the spinex DataStore entry. Popup/resource teardown may release
        // the final SkeletonData reference while retaining this compressed-package cache entry;
        // the next load then legitimately hits here and needs a fresh prewarm. Without restarting
        // it, binary-only (.skel) packages fall through to the synchronous rebuild path after the
        // JS skeleton cache has already been replaced, making every dynamic Spine slot empty on a
        // repeated popup open.
        spinexPrewarm(cached);
        dispatch_async(dispatch_get_main_queue(),^{notifyNativeSpinePreparse(waiter,true,cached);});return JSValueMakeBoolean(ctx,true);}
    uint64_t requestEpoch=gSpinePrepareEpoch;std::string workKey=std::to_string(requestEpoch)+"#"+(cacheKey.UTF8String?:"")+std::string("#")+std::to_string(data.length)+"#"+std::to_string(inputCrc);auto active=gSpinePrepareInflight.find(workKey);
    if(active!=gSpinePrepareInflight.end()){active->second.push_back(waiter);gSpinePrepareStats.deduped++;return JSValueMakeBoolean(ctx,true);}
    gSpinePrepareInflight[workKey].push_back(waiter);gSpinePrepareStats.jobs++;JSGlobalContextRef requestContext=gCtx;
    dispatch_async(gLoadQ(),^{NativeSpinePrepared* parsed=nil;@autoreleasepool{dispatch_semaphore_wait(assetCpuSlots(),DISPATCH_TIME_FOREVER);parsed=prepareNativeSpineArchive(data,url,key,inputCrc);dispatch_semaphore_signal(assetCpuSlots());}
        dispatch_async(dispatch_get_main_queue(),^{if(requestEpoch!=gSpinePrepareEpoch)return;auto found=gSpinePrepareInflight.find(workKey);if(found==gSpinePrepareInflight.end())return;std::vector<NativeSpinePrepareWaiter> waiters=std::move(found->second);gSpinePrepareInflight.erase(found);if(gCtx!=requestContext)return;
            bool ok=parsed!=nil;if(ok){int leaseN=0;for(const auto& w:waiters)if(w.wantsLease)leaseN++;parsed.activeUsers=leaseN;/* 在途 lease:插入前按 lease-waiter 数出生——插入触发的逐出不能挤掉自己;legacy waiter 不计 */cacheNativeSpinePrepared(parsed);gSpinePrepareStats.ready++;gSpinePrepareStats.backgroundZipMs+=parsed.zipMs;gSpinePrepareStats.backgroundAtlasMs+=parsed.atlasMs;gSpinePrepareStats.backgroundJsonMs+=parsed.jsonMs;spinexPrewarm(parsed);}else gSpinePrepareStats.failures++;
            if(getenv("SPINE_PREPARSE_LOG"))fprintf(stderr,"[spine-preparse/ios] READY %s ok=%d handle=%d entries=%lu images=%lu zip=%.2fms json=%.2fms waiters=%zu\n",key.UTF8String?:"",ok?1:0,parsed?parsed.handle:0,(unsigned long)(parsed?parsed.manifestEntries:0),(unsigned long)(parsed?parsed.imageNames.count:0),parsed?parsed.zipMs:0,parsed?parsed.jsonMs:0,waiters.size());
            for(const auto& pending:waiters)notifyNativeSpinePreparse(pending,ok,parsed);});});
    return JSValueMakeBoolean(ctx,true);
}

static JSValueRef cbLoadSpinePreparedImage(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<3)return JSValueMakeBoolean(ctx,false);int handle=(int)JSValueToNumber(ctx,args[0],exc),token=(int)JSValueToNumber(ctx,args[2],exc);NSString* name=jsToNSString(ctx,args[1]);NativeSpinePrepared* parsed=gSpinePreparedByHandle[@(handle)];NSData* data=parsed.entries[name];
    if(!parsed||!data.length){gSpinePrepareStats.directImageMisses++;return JSValueMakeBoolean(ctx,false);}gSpinePrepareStats.directImageRequests++;touchNativeSpinePrepared(parsed);JSGlobalContextRef requestContext=gCtx;
    NSString* stableName=[NSString stringWithFormat:@"%@#%08x#%@",parsed.cacheKey,parsed.inputCrc,name];
    gNativeResources.requestImageDataNamed(stableName,data,[requestContext,token,handle,name](uint32_t cid,int width,int height){deliverPublishedImage(requestContext,token,cid,width,height);},/*publishRef=*/true);
    return JSValueMakeBoolean(ctx,true);
}
static JSValueRef cbNativeSpineRecordFallback(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef*){gSpinePrepareStats.fallbacks++;return JSValueMakeUndefined(ctx);}

// 页名 → prepared 的 png → stableName(cacheKey#crc#页名) → ResourceManager 异步 cid。
// 同步路径与预热路径共用同一份,保证两条路的纹理接线完全一致。
static spinex::TexRequestFn spinexMakeTexRequest(NativeSpinePrepared* holder){
    return [holder](const std::string& pageName,const spinex::TexStatePtr& st){
        NSString* name=[NSString stringWithUTF8String:pageName.c_str()];
        NSData* png=name.length?holder.entries[name]:nil;
        if(!png.length){st->failed=true;fprintf(stderr,"[spinex] atlas 页 %s 无 zip 条目\n",pageName.c_str());return;}
        NSString* stableName=[NSString stringWithFormat:@"%@#%08x#%@",holder.cacheKey,holder.inputCrc,name];
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
// ---- 骨架预热 ----
// 原本 __spxBuild 在业务真正要用时才解析骨架:749-slot 的 m95009_lock_jackpot 要 200ms+(iOS 293ms),
// 正好落在 loading 94% 的主线程上,把 loading 的 spine 动画冻住。改成 preparse 一就绪就在后台起解析:
// Atlas 仍在主线程建(它要同步回调 texRequest 碰 ResourceManager 的非线程安全表),只有纯 CPU 的
// readSkeletonData 去后台。等业务调 __spxBuild 时通常已经算完,直接登记即可。
static std::unordered_map<std::string,std::shared_ptr<spinex::DataStore::AsyncBuild>> gSpinexPrewarm;
static void spinexPrewarm(NativeSpinePrepared* parsed){
    static const bool disabled = getenv("SPINE_PREWARM_OFF") != nullptr;   // A/B 用:置任意值即退回"用时才解析"
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
    NSData* jsonData=parsed.skeletonFilename.length?parsed.entries[parsed.skeletonFilename]:nil;
    if(!parsed.skelFilename.length&&!jsonData.length)return;   // 二者有其一即可:.skel 主用,.json 是过渡期回退
    std::string atlasText=parsed.atlasText.UTF8String?:"";
    // 无 .json 时留空串:parseSkeleton 的 `if(!data && jsonText && jsonText[0])` 会跳过 JSON 分支
    std::string jsonText=jsonData.length?std::string((const char*)jsonData.bytes,(size_t)jsonData.length):std::string();
    NSData* skelData=parsed.skelFilename.length?parsed.entries[parsed.skelFilename]:nil;
    auto job=gSpinexRt.store.prepareAsync(k,atlasText.c_str(),(int)atlasText.size(),jsonText.c_str(),
                                          (uint32_t)parsed.inputCrc,spinexMakeTexRequest(parsed),
                                          skelData.length?(const unsigned char*)skelData.bytes:nullptr,(int)skelData.length);
    if(!job)return;
    gSpinexPrewarm[k]=job;
    dispatch_async(gLoadQ(),^{
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
        if(found->inputCrc==parsed.inputCrc)return found;
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
            // 预热是低频事件(每个骨架一次),日志量很小;iOS App 没有环境变量,不能拿 getenv 门控,
            // 否则真机上永远看不到命中情况——这正是上一轮 iOS 日志里查不到预热记录的原因。
            if (!built || PIXINATIVE_DIAG_LOG)
                fprintf(stderr,"[spinex] key=%s 预热%s(主线程等待 %.1fms)\n",k.c_str(),built?"命中":"失败",waitMs);
            if(built)return built;
        } else {
            // 同名 zspine 换代:旧 crc 预热 job 作废,必须 discardAsync 退还已 retain 的页图引用。
            gSpinexRt.store.discardAsync(pre->second);
            gSpinexPrewarm.erase(pre);
        }
    }
    NSData* jsonData=parsed.skeletonFilename.length?parsed.entries[parsed.skeletonFilename]:nil;
    NSData* skelEntry=parsed.skelFilename.length?parsed.entries[parsed.skelFilename]:nil;
    if(!skelEntry.length&&!jsonData.length){if(outError)*outError="skeleton entry missing (.skel/.json)";return nullptr;}
    std::string atlasText=parsed.atlasText.UTF8String?:"";
    // 无 .json 时留空串:parseSkeleton 的 `if(!data && jsonText && jsonText[0])` 会跳过 JSON 分支
    std::string jsonText=jsonData.length?std::string((const char*)jsonData.bytes,(size_t)jsonData.length):std::string();
    spinex::TexRequestFn texRequest=spinexMakeTexRequest(parsed);
    NSData* skelData=parsed.skelFilename.length?parsed.entries[parsed.skelFilename]:nil;
    spinex::DataStore::Entry* built=gSpinexRt.store.buildFromPrepared(k,atlasText.c_str(),(int)atlasText.size(),
                                             jsonText.c_str(),(uint32_t)parsed.inputCrc,texRequest,outError,
                                             skelData.length?(const unsigned char*)skelData.bytes:nullptr,(int)skelData.length);
    if(!built&&parsed.version.length&&[parsed.version hasPrefix:@"3."])
        fprintf(stderr,"[spinex] key=%s spine %s 骨架不受支持,已回退 legacy(资源侧升版可收编)\n",
                k.c_str(),parsed.version.UTF8String);
    return built;
}
void spinexFrameDrive(){
    double nowMs = nowUs() * 0.001;
    if (!gSpinexRt.instanceCount()) { gSpinexRt.noteIdleFrame(nowMs); return; }
    gSpinexRt.updateAll(nowMs, &gSG, gSGBuf, (float)gScreenW, (float)gScreenH);
}

// ---- reload 弹窗(原生 UIKit 覆盖层，100% 还原 CDN app-reload 组件)。前向声明，实现在 GameViewController 之后。----
@class GameViewController;
static GameViewController* gRootVC = nil;
static double gLastTouchUs = 0;   // 最后触摸时间(空闲自适应帧率用)
static JSValueRef cbShowReload(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
static JSValueRef cbHideReload(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
static JSValueRef cbReload(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
static JSValueRef cbKbShow(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
static JSValueRef cbKbHide(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
static JSValueRef cbKbKeep(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
static JSValueRef cbKbSetText(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
static JSValueRef cbNativePrompt(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
static JSValueRef cbSnapshot(JSContextRef, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);

// Native -> JS always passes JSON as a value, never as interpolated source. This
static uint64_t gRuntimeEpoch = 0;


static JSValueRef cbClipboardWrite(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                   const JSValueRef args[], JSValueRef*) {
    if (argc < 1) return JSValueMakeBoolean(ctx, false);
    UIPasteboard.generalPasteboard.string = jsToNSString(ctx, args[0]) ?: @"";
    return JSValueMakeBoolean(ctx, true);
}


static bool setupGame() {
    // A newly activated content release gets one full JS-context launch to prove healthy. If the

    // 旧 JSC 上下文派出的 worker 可能稍后才完成。建新上下文之前先推进代号,让它既不能消费
    // 也不能通知属于本次 setup 的 waiter。
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
    gTimers = [NSMutableDictionary dictionary];
    gTimerFns = [NSMutableDictionary dictionary];
    gNativeText.clear();
    gNativeResources.reset();
    gNativeResources.configure(&gDec, [](NSString* url) -> NSData* {
        return loadAsset(url.UTF8String);
    });
    if (gSGBuf) gNativeFgui.reset(gSG);
    // preparse 出来的是一次性的、脱离运行时的蓝图。绝不能把已被消费的蓝图带进新的 JSC/运行时世代;
    // 旧 worker 的回调靠上下文比对自行作废。
    gFguiPreparseInflight.clear();
    gFguiPreparseCache=nil;
    gFguiPreparseLru=nil;
    gFguiXmlPrewarm=nil;
    gFguiPreparseStats={};
    gNativeTweens.clear();
    gCtx = JSGlobalContextCreate(nullptr);
    JSObjectRef global = JSContextGetGlobalObject(gCtx);


    JSValueRef exc = nullptr;

    injectNativeBootConfiguration(gCtx, global);
#if !defined(PIXINATIVE_PRODUCTION)

#endif
    // Spread expensive first-time Text/Graphics/Sprite rasterisation across frames. Large popup
    // and scene transitions otherwise create 200-500 textures in one __tick (up to 700ms).
    {
        JSStringRef key = JSStringCreateWithUTF8CString("__nativeRenderWorkBudget");
        JSObjectSetProperty(gCtx, global, key, JSValueMakeNumber(gCtx, 24),
                            kJSPropertyAttributeReadOnly, nullptr);
        JSStringRelease(key);
    }
    // shell 模块分帧加载预算(ms)。这批 26 个 bundle 原本在一个 for 里同步跑完,独占一帧
    // 1.6s(mac)/2.6s(iOS)——全程最大的单个尖峰。按 8ms 预算分帧后 mac 实测最大帧 1614→684ms、
    // 总卡顿 -42%,且总时长不变、启动路径与报错数完全一致。
    {
        JSStringRef key = JSStringCreateWithUTF8CString("__SHELL_LOAD_BUDGET");
        JSObjectSetProperty(gCtx, global, key, JSValueMakeNumber(gCtx, 8),
                            kJSPropertyAttributeNone, nullptr);
        JSStringRelease(key);
    }
    JSObjectRef ab = JSObjectMakeArrayBufferWithBytesNoCopy(gCtx, gBuf, kBufWords*4, nullptr, nullptr, &exc);
    JSStringRef cmdName = JSStringCreateWithUTF8CString("__cmd");
    JSObjectSetProperty(gCtx, global, cmdName, ab, kJSPropertyAttributeNone, NULL); JSStringRelease(cmdName);
    // 原生场景图共享 buffer(Float32),init + 暴露 __sg + 注册绑定
    if (!gSGBuf) { size_t n = (size_t)sg::SHARED_FLOATS; gSGBuf = (float*)calloc(n, sizeof(float)); gSG.init(gSGBuf); }   // 节点区+脏队列区
    else gSG.init(gSGBuf);   // reload 时重置树(buffer 复用)
    // fgui 纯色图元(Graph/遮罩/描边底)用的 1x1 白纹理:整棵树共用一个外部纹理 id,合批不被打断。
    if (!gNativeFguiWhiteTexture) {
        const uint8_t white[4] = {255,255,255,255}; GLint prior = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prior);
        glGenTextures(1, &gNativeFguiWhiteTexture); glBindTexture(GL_TEXTURE_2D, gNativeFguiWhiteTexture); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,white);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glBindTexture(GL_TEXTURE_2D,(GLuint)prior);
        gDec.registerExternalTexture(kNativeFguiWhiteCid,gNativeFguiWhiteTexture);
    }
    gNativeFgui.solidTextureId = (int)kNativeFguiWhiteCid;
    // fgui 里的文本节点由 C++ 直接驱动 CoreText 光栅化:包 XML 的属性 → 字体串/度量 → 纹理,
    // 全程不经过 JS。与 src/mac/game_window.mm 的 prepareText 逐字同源。
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
    hostCoreRegisterBindings(gCtx, global);   // 共享绑定表见 src/host/host_core.mm
    // 渲染杂项:九宫/平铺/位图字形/Graphics 网格 + RenderTexture 离屏合批 + native tween
    setFn(gCtx, global, "__nativeBatchCoverage", cbNativeBatchCoverage);
    setFn(gCtx, global, "__nativeRenderTexturePreflight", cbNativeRenderTexturePreflight);
    setFn(gCtx, global, "__nativeRenderTexture", cbNativeRenderTexture);
    setFn(gCtx, global, "__shadowFlag", cbShadowFlag);   // legacy alias
    // Production native-only presentation must use the direct main-screen queue. The old shadow
    // callback routes mode=2 through renderShadow(), whose diagnostic state save/restore path was
    // never intended to run every frame. On device that path grew to ~80ms for the lobby and was
    // also misreported as JS time, limiting CADisplayLink to roughly 10-15 fps.
    // 提交原子性:绑定调用点内联取景(cbNativePresent→flipMainScreenDraw),render 后的 JS
    // 变更(停轮工作流/微任务续体)落下一帧,与 mac/浏览器语义一致;帧尾只 presentRenderbuffer。
    setFn(gCtx, global, "__nativePresent", cbNativePresent);
    gSpinexRt.resetAll();    // spinex 同步清仓:reload 后旧 instId/sdHandle 全部失效(同 mac setup 语义)
    // (legacy __sp*/__skel* 蒙皮/骨骼桥与 __spBuf/__skelBatchBuf 共享缓冲已清仓——
    //  spinex __spx* 桥组默认接管后 JS 侧零消费,与 mac 同步,2026-08-25)
    setFn(gCtx, global, "__zstdDecompress", cbZstdDecompress);
    // spinex(spine 全量 native 化)__spx* 桥组与 game_window.mm 一一对应,一个不少
    // (遗漏任一门面桥都会让 JS 侧收编类静默降级)。数据源(preparse)未移植前 __spxBuild 恒
    // ok:false,且 JS 侧探测 __nativeSpinePreparse 缺失即不安装门面——本组注册零行为变化。
    // spine preparse 体系:zspine 整包在后台解压/解 atlas/解 skeleton,页图走 ResourceManager 异步解码。
    // JS 侧 native_spinex.js 以 __nativeSpinePreparse 是否存在作为整套门面的安装闸门。
    setFn(gCtx, global, "__nativeSpinePreparse", cbNativeSpinePreparse);
    setFn(gCtx, global, "__loadSpinePreparedImage", cbLoadSpinePreparedImage);
    setFn(gCtx, global, "__nativeSpineRecordFallback", cbNativeSpineRecordFallback);
    // ResourceManager:像素不进 JSC,JS 只拿 cid,retain/release 决定 GPU 缓存驻留
    // 语义化文本面:CoreText 光栅化 + 宿主自有 GL 纹理,JS 只拿回度量
    // 三宽度布局协议(docs/text-layout-contract-v2.md):结构化 V2 入口 + ABI 版本。JS 契约层
    // (game/host/native_richtext.js)在"V2 存在但 ABI<3"时禁用 facade,版本号与实现必须同批。
    {JSStringRef k=JSStringCreateWithUTF8CString("__nativeRichTextMetricsVersion");JSObjectSetProperty(gCtx,global,k,JSValueMakeNumber(gCtx,4),kJSPropertyAttributeReadOnly|kJSPropertyAttributeDontDelete,nullptr);JSStringRelease(k);}
    // FairyGUI native runtime 桥组(57 桥,与 mac 一一对应)
    setFn(gCtx, global, "__nativeFguiPreparse", cbNativeFguiPreparse);
    setFn(gCtx, global, "__nativeFguiPreparePackage", cbNativeFguiPreparePackage);
    setFn(gCtx, global, "__nativeOpenURL", cbNativeOpenURL);
    setFn(gCtx, global, "__auLoadUrl", cbAuLoadUrl);
    setFn(gCtx, global, "__auLoadUrlAsync", cbAuLoadUrlAsync);
    setFn(gCtx, global, "__auLoadData", cbAuLoadData);
    setFn(gCtx, global, "__auClone", cbAuClone);
    setFn(gCtx, global, "__auCtl", cbAuCtl);
    setFn(gCtx, global, "__kbShow", cbKbShow);
    setFn(gCtx, global, "__kbHide", cbKbHide);
    setFn(gCtx, global, "__kbKeep", cbKbKeep);
    setFn(gCtx, global, "__kbSetText", cbKbSetText);
    setFn(gCtx, global, "__clipboardWrite", cbClipboardWrite);
    setFn(gCtx, global, "__nativePrompt", cbNativePrompt);
    setFn(gCtx, global, "__snapshot", cbSnapshot);
    // 屏幕尺寸传给 game_host.js（innerWidth/innerHeight/devicePixelRatio）→ 游戏按真实屏幕比例响应式铺满。
    auto setNum = [&](const char* k, double v){ JSStringRef s=JSStringCreateWithUTF8CString(k);
        JSObjectSetProperty(gCtx, global, s, JSValueMakeNumber(gCtx, v), kJSPropertyAttributeNone, NULL); JSStringRelease(s); };
    setNum("__screenW", gScreenW); setNum("__screenH", gScreenH); setNum("__screenDPR", gScreenDPR);
    setNum("__safeAreaLeft", gSafeAreaLeft);   // BarHud 走 mock 路径消费（location.search 的 safearea=）
    setNum("__safeAreaBottom", gSafeAreaBottom);
    setNum("__screenRenderScale", gScreenRenderScale);
    // 文本超采样下限 2x(与 mac 同):主 drawable 限 2x 时 3x 机型的 CoreText 面 1:1 进纹理仍会被
    // 过滤采样,紧凑粗体字损失覆盖信息。超采样面是独立纹理,不改变逻辑尺寸/变换/命中/坐标系。
    setNum("__nativeTextRasterScale", MAX(2.0, (double)[UIScreen mainScreen].scale));
    {   // 标记移动端 → fgui/pixi 走触摸分支（滚动起点用事件坐标，修复列表拖动松手回弹）。
        JSStringRef k = JSStringCreateWithUTF8CString("__isMobile");
        JSObjectSetProperty(gCtx, global, k, JSValueMakeBoolean(gCtx, true), kJSPropertyAttributeNone, NULL); JSStringRelease(k);
    }
    {
        JSStringRef key = JSStringCreateWithUTF8CString("__nativePlatform");
        JSStringRef value = JSStringCreateWithUTF8CString("ios");
        JSObjectSetProperty(gCtx, global, key, JSValueMakeString(gCtx, value),
                            kJSPropertyAttributeReadOnly, nullptr);
        JSStringRelease(value);
        JSStringRelease(key);
    }


    setFn(gCtx, global, "__flush", cbFlush);
    {JSStringRef key=JSStringCreateWithUTF8CString("__nativeSupportsImageSourcePMA");JSObjectSetProperty(gCtx,global,key,JSValueMakeBoolean(gCtx,true),kJSPropertyAttributeReadOnly,nullptr);JSStringRelease(key);}
    setFn(gCtx, global, "__loadImage", cbLoadImage);
    setFn(gCtx, global, "__loadImageAsync", cbLoadImageAsync);
    setFn(gCtx, global, "__readFileAsync", cbReadFileAsync);
    setFn(gCtx, global, "__zipParse", cbZipParse);
    setFn(gCtx, global, "__zipGet", cbZipGet);
    setFn(gCtx, global, "__decodeImage", cbDecodeImage);
    setFn(gCtx, global, "__xhrSend", cbXhrSend);
    setFn(gCtx, global, "__decodeScriptUTF8", native_script_utf8::decode);   // 脚本字节原生解码(≙ mac;Android 走 QuickJS JS_NewStringLen)
    setFn(gCtx, global, "__showReload", cbShowReload);   // 显示原生 reload 弹窗(title, content)
    setFn(gCtx, global, "__hideReload", cbHideReload);
    setFn(gCtx, global, "__reload", cbReload);           // 真正 reload：重建 JSC 上下文 + 重登
    c2d::registerCanvas2D(gCtx, global);
    NSString* fontsDir = [gGameDir stringByAppendingPathComponent:@"fonts"];
    std::string fontError;
    if(!c2d::registerGameFonts(fontsDir.UTF8String,fontError)){fprintf(stderr,"[game] project font registration failed: %s\n",fontError.c_str());return false;}

    // Match the macOS debug configuration path. Xcode scheme environment variables reach both
    // Simulator and attached-device runs; TestFlight/App Store builds simply leave this unset.
    // Evaluate before game_host.js so feature gates/audits are active from module installation,
    // rather than changing mutation semantics halfway through a frame.
#if !defined(PIXINATIVE_PRODUCTION)
    if (const char* prelude = getenv("GAME_JS_PRELUDE")) {
        JSStringRef ps = JSStringCreateWithUTF8CString(prelude);
        JSStringRef pu = JSStringCreateWithUTF8CString("GAME_JS_PRELUDE");
        exc = nullptr; JSEvaluateScript(gCtx, ps, nullptr, pu, 1, &exc);
        JSStringRelease(ps); JSStringRelease(pu);
        if (exc) {
            JSStringRef es = JSValueToStringCopy(gCtx, exc, nullptr);
            size_t n = JSStringGetMaximumUTF8CStringSize(es); std::vector<char> b(n);
            JSStringGetUTF8CString(es, b.data(), n);
            fprintf(stderr,"[game] GAME_JS_PRELUDE 异常: %s\n", b.data());
            JSStringRelease(es);
            nativeLogError(@"javascript_boot", @"prelude_exception",
                                                [NSString stringWithUTF8String:b.data()] ?: @"", YES);
            return false;
        }
    }
#endif

    NSString* hostPath = [gGameDir stringByAppendingPathComponent:@"game_host.js"];
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


// ======================= EAGL + UIKit =======================
static EAGLContext* gGLContext = nil;
static GLuint gFramebuffer = 0, gColorRB = 0, gDepthStencilRB = 0;
static GLint  gDrawableW = 0, gDrawableH = 0;

// 每帧驱动 JS __tick → pixi ticker → flushPending 发命令 → present。
// M3:对拍仪表盘 —— 读影子 FBO 与真屏(pixi 输出),算逐像素散度 + 8×8 网格热区。
// 黑洞区(M2 未覆盖的 spine/text/gfx 等)= 高散度热区,直接指出下一步补哪个特性。glReadPixels 贵,低频调。
static int gShadowDiffTick = 0, gShadowDiffEvery = 3;   // 每 3 次影子渲染对拍一次(N=30 时≈每 90 帧)
static bool gShadowDiffOn = true;
static void shadowDiff(int w, int h) {
    if (w <= 0 || h <= 0) return;
    size_t n = (size_t)w * h * 4;
    static std::vector<uint8_t> A, B;
    A.resize(n); B.resize(n);
    glBindFramebuffer(GL_FRAMEBUFFER, gShadowFbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, A.data());   // 影子
    glBindFramebuffer(GL_FRAMEBUFFER, gFramebuffer);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, B.data());   // pixi 真屏
    const int GX = 8, GY = 8;
    double cell[GX*GY]; long cellN[GX*GY];
    for (int i=0;i<GX*GY;i++){ cell[i]=0; cellN[i]=0; }
    double total = 0; long tn = 0;
    const int step = 4;   // 每 4 像素采一个(降 CPU)
    for (int y=0; y<h; y+=step) for (int x=0; x<w; x+=step) {
        size_t i = ((size_t)y*w + x)*4;
        int d = abs((int)A[i]-(int)B[i]) + abs((int)A[i+1]-(int)B[i+1]) + abs((int)A[i+2]-(int)B[i+2]);
        int gi = (y*GY/h)*GX + (x*GX/w);
        cell[gi] += d; cellN[gi]++; total += d; tn++;
    }
    double avg = tn ? total/tn : 0;   // 0..765(RGB 各 0-255)
    // 最差 3 格
    int w1=-1,w2=-1,w3=-1; double v1=-1,v2=-1,v3=-1;
    for (int i=0;i<GX*GY;i++){ double v = cellN[i]? cell[i]/cellN[i] : 0;
        if (v>v1){ v3=v2;w3=w2; v2=v1;w2=w1; v1=v;w1=i; }
        else if (v>v2){ v3=v2;w3=w2; v2=v;w2=i; }
        else if (v>v3){ v3=v;w3=i; } }
    auto cellStr = [&](int gi, double v)->std::string {
        if (gi<0) return "-";
        int gx=gi%GX, gy=gi/GX; char b[64];
        snprintf(b,sizeof(b),"[格%d,%d≈(%d,%d) 差%.0f]", gx, gy, gx*w/GX, gy*h/GY, v); return b; };
    fprintf(stderr, "[shadow-diff] 平均散度 %.1f/765 (%.1f%%) 最差3格 %s %s %s\n",
            avg, 100.0*avg/765.0, cellStr(w1,v1).c_str(), cellStr(w2,v2).c_str(), cellStr(w3,v3).c_str());
    glBindFramebuffer(GL_FRAMEBUFFER, gFramebuffer);
}

// M2:把 sg 权威树的 sprite 画进离屏影子 FBO(不上屏),供 M3 与真屏逐像素对拍。
// escape=null → 复杂节点(spine/graphics/text/mask)整支跳过留黑(M3 diff 会点名,M4/M5 逐个补)。
// 用 glGet 完整存/复原 GL 状态包住 → 不污染 pixi 真屏的状态缓存(低频调用,dev 阶段可接受 glGet 同步开销)。
static void renderShadow() {
    if (gShadowRoot <= 0 || !gGLContext) return;
    int root = gShadowRoot; gShadowRoot = 0;
    int w = (int)gScreenW, h = (int)gScreenH;
    if (w <= 0 || h <= 0) return;
    // M6 flip:直接画到真屏(JS 已短路 pixi → 无 pixi 状态需保护,省 glGet/blit;逃逸节点留黑=已知 5-10%)
    if (gShadowShow == 2) {
        if (!gShadowBatcher.ready()) {
            gShadowBatcher.texLookup = [](int id){ return gDec.glTextureEnsured((uint32_t)id); };
            gShadowBatcher.init();
        }
        glBindFramebuffer(GL_FRAMEBUFFER, gFramebuffer);
        glBindVertexArray(0);
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(gSgClear[0], gSgClear[1], gSgClear[2], gSgClear[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        gSG.compute(root, sg::CM_CANONICAL_MAIN);   // D-1:draw 前收敛=canonical(隐式重载已删除)
        gShadowBatcher.frameReset();
        gShadowBatcher.frame(&gSG, root, (float)w, (float)h);
        { static int fdt=0; if(++fdt>=120){ fdt=0; fprintf(stderr,"[flip-native] quads=%u draws=%u 逃逸=%u(遮罩%u 逃生舱%u 无纹理%u)\n",
            gShadowBatcher.quads, gShadowBatcher.drawCalls, gShadowBatcher.escapes, gShadowBatcher.escMask, gShadowBatcher.escType, gShadowBatcher.escTex); } }
        gFrameHadCmds = true;   // 短路了 pixi(本帧无 pixi GL 命令),手动置 true 保证 present 上屏
        return;
    }
    // 懒建影子 FBO(尺寸变了重建);建纹理会绑到 unit,建完复原绑定避免污染 decoder 跟踪
    if (!gShadowFbo || gShadowFboW != w || gShadowFboH != h) {
        if (gShadowTex) { glDeleteTextures(1, &gShadowTex); gShadowTex = 0; }
        if (gShadowFbo) { glDeleteFramebuffers(1, &gShadowFbo); gShadowFbo = 0; }
        GLint saveTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &saveTex);
        GLint saveFbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saveFbo);
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
        if (st != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "[shadow] FBO 不完整 0x%x\n", (unsigned)st);
        gShadowFboW = w; gShadowFboH = h;
        glBindFramebuffer(GL_FRAMEBUFFER, saveFbo > 0 ? (GLuint)saveFbo : gFramebuffer);
        glBindTexture(GL_TEXTURE_2D, saveTex > 0 ? (GLuint)saveTex : 0);
        fprintf(stderr, "[shadow] FBO 建 %dx%d tex=%u fbo=%u\n", w, h, gShadowTex, gShadowFbo);
    }
    if (!gShadowBatcher.ready()) {
        gShadowBatcher.texLookup = [](int id){ return gDec.glTextureEnsured((uint32_t)id); };
        gShadowBatcher.init();
    }
    // ---- 完整 GL 状态存(glGet 一次;单一边界的干净 push)----
    GLint sProg=0,sArr=0,sElem=0,sVao=0,sFbo=0,sActive=0,sTex0=0,sVp[4]={0,0,0,0},sScis[4]={0,0,0,0};
    GLint sSrcRGB=0,sDstRGB=0,sSrcA=0,sDstA=0;
    GLboolean sBlend=glIsEnabled(GL_BLEND), sScisEn=glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM,&sProg); glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&sArr);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&sElem); glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&sVao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&sFbo); glGetIntegerv(GL_ACTIVE_TEXTURE,&sActive);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&sTex0); glGetIntegerv(GL_VIEWPORT,sVp); glGetIntegerv(GL_SCISSOR_BOX,sScis);
    glGetIntegerv(GL_BLEND_SRC_RGB,&sSrcRGB); glGetIntegerv(GL_BLEND_DST_RGB,&sDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&sSrcA); glGetIntegerv(GL_BLEND_DST_ALPHA,&sDstA);
    // ---- 画影子(独立 FBO;绑 VAO 0 避免污染 pixi 当前 VAO 的 attrib 记录)----
    glBindFramebuffer(GL_FRAMEBUFFER, gShadowFbo);
    glBindVertexArray(0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    gSG.compute(root, sg::CM_RENDER_TEXTURE);   // 六审:离屏影子对比根——不清主树债务不 ack(mac 同款语义;五审误标 canonical)
    gShadowBatcher.frameReset();
    gShadowBatcher.frame(&gSG, root, (float)w, (float)h);
    // ---- 复原 GL 状态(干净 pop)----
    glBindFramebuffer(GL_FRAMEBUFFER, sFbo > 0 ? (GLuint)sFbo : gFramebuffer);
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
    // M3:低频对拍(读 pixi 真屏须在 blit 前,否则 blit 已把影子写进真屏)
    if (gShadowDiffOn && (++gShadowDiffTick >= gShadowDiffEvery)) { gShadowDiffTick = 0; shadowDiff(w, h); }
    // M2 目视:把影子 FBO blit 到真屏(替换 pixi 输出),肉眼验证 native 渲染。默认关。
    if (gShadowShow) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gShadowFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gFramebuffer);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, gFramebuffer);
    }
}

static void flipMainScreenDraw(int root) {
    if (root <= 0 || !gFramebuffer) return;
    double t0 = nowUs();
    // Publish transforms first so a node attached immediately before renderer.render() does not
    // expose its allocation-time or previous-parent world position to Spine. Bounds remain owned
    // by Batcher::frame after animation emission; an extra bounds walk here duplicates the hottest
    // full-scene pass and is especially expensive on symbol-heavy machines.
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
        gBatcher.texLookup = [](int id){ return gDec.glTextureEnsured((uint32_t)id); };
        gBatcher.init();
        gBatcher.preparePostProcesses(512); // loading scene prewarm: no first-popup Twist shader/FBO stall
    }
    glBindFramebuffer(GL_FRAMEBUFFER, gFramebuffer);
    glViewport(0, 0, (GLsizei)gScreenW, (GLsizei)gScreenH);
    glClearColor(gSgClear[0], gSgClear[1], gSgClear[2], gSgClear[3]);
    glDisable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    double batchStart = nowUs();
    gBatcher.frame(&gSG, root, (float)gScreenW, (float)gScreenH);
    ++gNativeBatchSerial;   // 与 mac flipMainScreenDraw 同位:fgui 预约/回收按这个序号判活与老化
    gFrameBatchUs = nowUs() - batchStart;
    gDec.drawCalls += gBatcher.drawCalls;
    gFrameDecodeUs += nowUs() - t0;   // native 渲染耗时计入"解码"栏(perf 展示)
    // 本帧画面完全由 native 合批产出,JS 一条 GL 命令都没发。不置位就会被"静止帧跳过"当成
    // "JS 没渲染"而跳过 present——clear+重画照做、就是不上屏,表现为 tick 满 60 实际只有个位数。
    gFrameHadCmds = true; gFrameHadContent = true;
}
static void drawOneFrame() {
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef fn = JSStringCreateWithUTF8CString("__tick");
    JSValueRef fv = JSObjectGetProperty(gCtx, global, fn, nullptr); JSStringRelease(fn);
    if (!JSValueIsObject(gCtx, fv)) return;
    JSObjectRef tickFn = JSValueToObject(gCtx, fv, nullptr);
    if (!tickFn || !JSObjectIsFunction(gCtx, tickFn)) return;
    gDec.drawCalls = 0; gDec.texUploads = 0; gDec.texDeferred = 0; gDec.bufUploads = 0;
    gFrameDecodeUs = gFrameJSUs = gFrameComputeUs = gFrameBatchUs = gFramePresentUs = 0;
    gFrameHadCmds = false; gFramePresented = false; gFrameHadContent = false;   // 每帧开始清零
    // JS 跑之前先做完一份有上限的 ResourceManager GPU 工作量。大图集按行带切分,整张常驻后才可见。
    gNativeResources.beginFrame();
    gDec.beginFrame();   // 大图上传节流:补传一张推迟的图集(需 GL 上下文:tick 在 GL 线程)
    // 预约补货只在帧边界做;transition/延时任务同样在 JS 之前于 C++ 推进,JS 只领完成令牌。
    if (gSG.buf) gSG.buf[sg::FRAME_PHASE_BASE] = 1.0f;   // B0-1:阶段1=C++ 前段(JS 之前的 transition/refill 推进)
    gNativeFgui.advanceReservationRefills((nowUs()-gStartUs)/1000.0,gNativeBatchSerial,gSG);
    gNativeFgui.advanceTransitions((nowUs()-gStartUs)/1000.0, gSG);
    gNativeFgui.advanceTasks((nowUs()-gStartUs)/1000.0);
    // Drain naturally completed Transition tokens before the application ticker. Once C++ has
    // removed a completed timeline, a same-frame JS stop/replay must not be able to discard its
    // still-pending completion callback. Property/visibility shadows follow the same boundary.
    JSValueRef transitionNow = JSValueMakeNumber(gCtx, (nowUs()-gStartUs)/1000.0);
    callJsGlobal("__nativeFguiTransitionFrame", 1, &transitionNow);
    if (gSG.buf) gSG.buf[sg::FRAME_PHASE_BASE] = 2.0f;   // B0-1:阶段2=JS tick(业务/GSAP;细分阶段由 JS 内部推进)
    JSValueRef arg = JSValueMakeNumber(gCtx, (nowUs()-gStartUs)/1000.0);
    JSValueRef exc = nullptr;
    double jsStart = nowUs();
    JSObjectCallAsFunction(gCtx, tickFn, nullptr, 1, &arg, &exc);
    gFrameJSUs = nowUs() - jsStart;
    if (exc) {
        JSStringRef es = JSValueToStringCopy(gCtx, exc, nullptr);
        size_t n = JSStringGetMaximumUTF8CStringSize(es); std::vector<char> b(n);
        JSStringGetUTF8CString(es, b.data(), n); fprintf(stderr,"[game] __tick 异常: %s\n", b.data()); JSStringRelease(es);
        nativeLogError(@"javascript_runtime", @"tick_exception",
                                            [NSString stringWithUTF8String:b.data()] ?: @"", NO);
    }
    // 帧尾兜底:正常路径已在 __nativePresent 的 JS 调用点内联绘制(cbNativePresent →
    // flipMainScreenDraw,与 mac 同语义)。此分支只服务仍走 __sgFlagRender 旧标记语义的
    // 宿主/调试脚本;内联路径不再设置 gPendingRoot,这里自然不重复绘制。
    if (gPendingRoot > 0 && gFramebuffer) {
        int root = gPendingRoot; gPendingRoot = 0;
        flipMainScreenDraw(root);
    }
    // M2:影子 FBO 渲染(离屏,不上屏;JS 每 N 帧 __shadowFlag 一次触发)。pixi 真屏已画完,此为对拍旁路。
    renderShadow();
    // 不用 glFinish：EAGL presentRenderbuffer 自带提交/同步；每帧 glFinish 会强制 CPU 阻塞等 GPU、
    // 禁用 CPU-GPU 流水线，导致双核持续满载→严重发热。去掉后功耗大幅下降。
    // 静止帧跳过:本帧无任何 GL 命令(JS 跳过了渲染)→ 不 present(EAGL present 后 buffer 内容不保证,
    // 空 present 会闪);屏幕保持上一帧,GPU 完全休息。
    if (gFrameHadCmds) {
        double presentStart = nowUs();
        glBindRenderbuffer(GL_RENDERBUFFER, gColorRB);
        [gGLContext presentRenderbuffer:GL_RENDERBUFFER];   // 上屏（隐式 flush）
        gFramePresented = true;
        gFramePresentUs = nowUs() - presentStart;
    }
}

@interface GLView : UIView @end
@implementation GLView
+ (Class)layerClass { return [CAEAGLLayer class]; }
@end

@interface GameKeyboardTextField : UITextField
@property(nonatomic) UIEdgeInsets contentInsets;
@end
@implementation GameKeyboardTextField
- (CGRect)textRectForBounds:(CGRect)bounds { return UIEdgeInsetsInsetRect(bounds, self.contentInsets); }
- (CGRect)editingRectForBounds:(CGRect)bounds { return UIEdgeInsetsInsetRect(bounds, self.contentInsets); }
@end

@interface ReloadOverlayView : UIView @end
@implementation ReloadOverlayView
- (void)layoutSubviews {
    [super layoutSubviews];
    UIView* popup = self.subviews.firstObject;
    CGFloat naturalHeight = CGRectGetHeight(popup.bounds);
    if (naturalHeight <= 0) return;
    // Auto Layout keeps the full content height; fit the card as a whole so
    // short landscape screens retain space above and below without clipping.
    CGFloat availableHeight = CGRectGetHeight(self.safeAreaLayoutGuide.layoutFrame);
    CGFloat margin = MAX(24, availableHeight * 0.08);
    CGFloat scale = MIN(1, MAX(0, availableHeight - 2 * margin) / naturalHeight);
    popup.transform = CGAffineTransformMakeScale(scale, scale);
}
@end

@interface GameViewController : UIViewController <UITextFieldDelegate> {
    GLView* _glView;
    UIImageView* _splashOverlay;
    CADisplayLink* _link;
    int _frame;
    BOOL _booted;
    CGFloat _glScale;   // 屏幕缩放：触摸点数 → native 像素（= innerWidth 空间）
    UIView* _reloadOverlay;
    UILabel* _fpsLabel; // 左上角实时 FPS/耗时/draw call
    double _fpsWinStart; int _fpsWinFrames; int _fpsWinPresents; int _fpsWinIdle; int _fpsWinDropped; long _fpsWinDraws; double _fpsWinBusy;
    double _fpsWinDecode; long _fpsWinTex; long _fpsWinBuf;
    GameKeyboardTextField* _kbField;   // 透明输入承接器:与游戏内 TextInput 对齐,让 iOS 原生长按粘贴菜单可用
    int _kbMaxLen;
    BOOL _kbWantHide;        // 触摸开始时键盘在显示:延迟到 pointerup 走完 JS 再决定收起(点击若重新聚焦输入框则取消,避免收起→重弹重置文本丢字)
    CGRect _kbFieldRect;     // 当前聚焦输入框矩形(游戏像素坐标):触点命中时不收键盘 + 键盘避让算遮挡
    CGFloat _kbTopPt;        // 键盘顶部(self.view 坐标点);0 = 键盘未显示
    BOOL _wasBackgrounded;   // 真正进过后台才在 DidBecomeActive 走恢复重置(控制中心下拉不触发 O(nodes) 重标记)
    BOOL _reloading;         // reload 重入闩:快速双击 reload 不叠加重建
    NSMapTable<UITouch*, NSNumber*>* _touchIds;
    NSInteger _nextTouchId, _primaryTouchId;
}
- (void)showReloadTitle:(NSString*)title content:(NSString*)content;
- (void)hideReload;
- (void)reloadGame;
- (void)dismissSplashAfterPresentedFrame;
- (void)kbShow:(NSString*)text maxLen:(int)maxLen secure:(BOOL)secure fieldRect:(CGRect)rectPx
      fontSize:(CGFloat)fontSizePx fontFamily:(NSString*)fontFamily
     alignment:(NSInteger)alignment insets:(UIEdgeInsets)insetsPx;
- (void)kbHide;
- (void)kbKeep;
- (void)kbSetText:(NSString*)text;
@end

@implementation GameViewController

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }

- (void)viewDidLoad {
    [super viewDidLoad];
    gRootVC = self;
    self.view.backgroundColor = [UIColor blackColor];
    // 透明输入承接器:内容变化回传 JS,游戏内 surrogate 负责绘制文字。
    // 聚焦时会放到 Pixi 输入框上方,以便系统原生的长按/粘贴菜单有可交互的锚点。
    _kbField = [[GameKeyboardTextField alloc] initWithFrame:CGRectMake(-200, -200, 100, 30)];
    _kbField.hidden = YES;
    _kbField.backgroundColor = UIColor.clearColor;
    _kbField.textColor = UIColor.clearColor;
    _kbField.tintColor = UIColor.whiteColor;
    _kbField.autocorrectionType = UITextAutocorrectionTypeNo;
    _kbField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    _kbField.spellCheckingType = UITextSpellCheckingTypeNo;
    _kbField.returnKeyType = UIReturnKeyDone;
    _kbField.delegate = self;
    [_kbField addTarget:self action:@selector(kbChanged) forControlEvents:UIControlEventEditingChanged];
    _kbMaxLen = 0;
    // 键盘避让:键盘弹出/收起/高度变化时,若遮挡聚焦的输入框则整个 GL 视图上移(触摸经 locationInView:_glView 自动换算,不受 transform 影响)
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(kbFrameChanged:)
                                                 name:UIKeyboardWillChangeFrameNotification object:nil];
    // 前后台:后台必须停 CADisplayLink 并排空 GPU 队列(iOS 后台提交 GL 会被 watchdog 杀);
    // 恢复时废弃 decoder/batcher 的 GL 绑定缓存并通知 JS __nativeResume(重置 Pixi 状态缓存+重标记渲染)。
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(appDidEnterBackground)
                                                 name:UIApplicationDidEnterBackgroundNotification object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(appDidBecomeActive)
                                                 name:UIApplicationDidBecomeActiveNotification object:nil];
    // GL 视图全屏，自动随父视图尺寸；真正的初始化延到首次 layout（拿到最终 landscape 尺寸）。
    _glView = [[GLView alloc] initWithFrame:self.view.bounds];
    _glView.multipleTouchEnabled = YES;
    _touchIds = [NSMapTable strongToStrongObjectsMapTable];
    _nextTouchId = 2; _primaryTouchId = -1;
    _glView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    CAEAGLLayer* eaglLayer = (CAEAGLLayer*)_glView.layer;
    eaglLayer.opaque = YES;
    eaglLayer.drawableProperties = @{ kEAGLDrawablePropertyRetainedBacking:@(NO),
                                      kEAGLDrawablePropertyColorFormat:kEAGLColorFormatRGBA8 };
    [self.view addSubview:_glView];
    // 跟随 GL 视图的键盘避让位移,并在聚焦后接收输入区域内的长按手势。
    [_glView addSubview:_kbField];

    // Keep the launch artwork above the uninitialized EAGL surface.  The render loop removes it
    // after the first presented frame that contains real draw calls; AppBridge dismissSplash is
    // retained only as a compatibility escape hatch for bundles that already send that command.
    _splashOverlay = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"Splash"]];
    _splashOverlay.frame = self.view.bounds;
    _splashOverlay.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _splashOverlay.contentMode = UIViewContentModeScaleAspectFill;
    _splashOverlay.clipsToBounds = YES;
    _splashOverlay.backgroundColor = [UIColor colorWithRed:0.102 green:0.039 blue:0.180 alpha:1.0];
    _splashOverlay.tag = 0x53504C53;
    [self.view addSubview:_splashOverlay];
}

- (void)dismissSplashAfterPresentedFrame {
    if (!_splashOverlay) return;
    if (!_splashOverlay.superview) { _splashOverlay = nil; return; }
    UIImageView* splash = _splashOverlay;
    _splashOverlay = nil;   // 闩住只执行一次；动画期间收到兼容 bridge 命令也不会影响帧驱动。
    splash.userInteractionEnabled = NO;
    fprintf(stderr, "[splash/native] dismiss after first presented content frame draw=%u\n",
            (unsigned)gDec.drawCalls);
    [UIView animateWithDuration:0.2 delay:0
                        options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionCurveEaseOut
                     animations:^{ splash.alpha = 0; }
                     completion:^(__unused BOOL finished) { [splash removeFromSuperview]; }];
}

// 首次拿到最终全屏尺寸后再建 GL：渲染分辨率 = 屏幕点数 × 屏幕缩放（native 像素），游戏按真实屏幕比例响应式铺满。
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    // 键盘避让上移期间(transform 非 identity)不能设 frame:对带 transform 的视图设置 frame,
    // UIKit 按"变换后 frame=bounds"反推 center,会把平移抵消掉 → 视图上移永远不生效。
    if (CGAffineTransformIsIdentity(_glView.transform))
        _glView.frame = self.view.bounds;   // 全屏，无 letterbox
    if (_booted) return;
    CGSize pts = self.view.bounds.size;      // 屏幕点数（landscape）
    if (pts.width < 1 || pts.height < 1) return;
    // 渲染分辨率上限 2x:3x 机型满分辨率填充率过高;1.5x 试过(GPU -44%)但用户反馈不够清晰,回 2.0。
    CGFloat scale = MIN([UIScreen mainScreen].scale, 2.0);
    _glView.contentScaleFactor = scale;
    _glView.layer.contentsScale = scale;
    _glScale = scale;

    // dpr 固定 1（复刻 macOS 工作配置，绕开 dpr≠1 的文本/坐标垫片路径）；
    // innerWidth/innerHeight = 屏幕点数×缩放 = native 像素，整局在 native 像素空间布局，drawable 正好匹配。
    // 1.5x 下点数×scale 可能非整(如 393×1.5=589.5):drawable 会取整,gScreenW/H 必须与其一致,否则
    // framebuffer 尺寸/viewport/JS 布局三方错位。floor 到整数。
    gScreenW = floor(pts.width * scale); gScreenH = floor(pts.height * scale); gScreenDPR = 1;
    // 安全区：横屏刘海/灵动岛在左或右，游戏统一映射到左侧(BarHud unified=max(左,右))。点数×缩放 → native 像素(与 innerHeight 同单位)。
    UIEdgeInsets si = self.view.safeAreaInsets;
    gSafeAreaLeft = MAX(si.left, si.right) * scale;
    gSafeAreaBottom = si.bottom * scale;
    gScreenRenderScale = scale;
    fprintf(stderr, "[game] safeAreaInsets pts L%.0f T%.0f R%.0f B%.0f → safeAreaLeft(px)=%.0f safeAreaBottom(px)=%.0f\n",
            si.left, si.top, si.right, si.bottom, gSafeAreaLeft, gSafeAreaBottom);

    gGLContext = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    if (!gGLContext || ![EAGLContext setCurrentContext:gGLContext]) {
        fprintf(stderr, "[game] EAGL GLES3 上下文创建失败\n");
        nativeLogError(@"graphics", @"eagl_context_create_failed",
                                            @"iOS EAGL GLES3 context creation failed", YES);
        return;
    }
    fprintf(stderr, "[game] GL_VERSION=%s\n", glGetString(GL_VERSION));

    // 默认帧缓冲：color renderbuffer 绑到 CAEAGLLayer + depth/stencil（mask 需 stencil）。drawable = 点数×scale。
    glGenFramebuffers(1, &gFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, gFramebuffer);
    glGenRenderbuffers(1, &gColorRB);
    glBindRenderbuffer(GL_RENDERBUFFER, gColorRB);
    [gGLContext renderbufferStorage:GL_RENDERBUFFER fromDrawable:(CAEAGLLayer*)_glView.layer];
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, gColorRB);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &gDrawableW);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &gDrawableH);
    glGenRenderbuffers(1, &gDepthStencilRB);
    glBindRenderbuffer(GL_RENDERBUFFER, gDepthStencilRB);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, gDrawableW, gDrawableH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,   GL_RENDERBUFFER, gDepthStencilRB);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gDepthStencilRB);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    fprintf(stderr, "[game] 默认FBO %dx%d (pts %.0fx%.0f dpr %.1f) status=0x%x\n",
            gDrawableW, gDrawableH, pts.width, pts.height, scale, st);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        nativeLogError(@"graphics", @"framebuffer_incomplete",
            [NSString stringWithFormat:@"default framebuffer status=0x%x size=%dx%d",
                                       st, gDrawableW, gDrawableH], NO);
    }
    gDec.setDefaultFbo(gFramebuffer);

    gStartUs = nowUs();
    if (!setupGame()) {
        fprintf(stderr, "[game] setupGame 失败\n");
        nativeLogError(@"runtime", @"initial_setup_failed",
                                            @"iOS initial game setup failed", YES);
        return;
    }
    _booted = YES;

    // Sandbox 即使使用 Release 构建也保留实时指标；只有 prod 不创建该视图。
    if (shouldShowNativeDebugUI()) {
        _fpsLabel = [[UILabel alloc] init];
        _fpsLabel.font = [UIFont fontWithName:@"Menlo" size:12] ?: [UIFont systemFontOfSize:12];
        _fpsLabel.textColor = [UIColor greenColor];
        _fpsLabel.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
        _fpsLabel.textAlignment = NSTextAlignmentLeft;
        _fpsLabel.text = @" FPS --  --ms  draw -- ";
        _fpsLabel.layer.cornerRadius = 3; _fpsLabel.clipsToBounds = YES;
        _fpsLabel.translatesAutoresizingMaskIntoConstraints = NO;
        [self.view addSubview:_fpsLabel];
        UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
        [NSLayoutConstraint activateConstraints:@[
            [_fpsLabel.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-6],
            [_fpsLabel.topAnchor constraintEqualToAnchor:safe.topAnchor constant:4],
            [_fpsLabel.heightAnchor constraintEqualToConstant:18],
        ]];
    }
    _fpsWinStart = 0; _fpsWinFrames = 0; _fpsWinPresents = 0; _fpsWinIdle = 0; _fpsWinDropped = 0; _fpsWinDraws = 0; _fpsWinBusy = 0;

    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick)];
    // 限 60fps：ProMotion 机型默认按 120Hz 触发，游戏无需 120fps，砍一半直接大幅降功耗/发热。
    if (@available(iOS 15.0, *)) _link.preferredFrameRateRange = CAFrameRateRangeMake(30, 60, 60);
    else _link.preferredFramesPerSecond = 60;
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}


- (void)appDidEnterBackground {
    emitVisibility(false);
    [_touchIds removeAllObjects]; _primaryTouchId=-1;
    auSetBackgrounded(YES);
    [UIApplication sharedApplication].idleTimerDisabled = NO;
    _wasBackgrounded = YES;
    _link.paused = YES;
    if (gGLContext && [EAGLContext currentContext] == gGLContext) glFinish();   // 排空 GPU 队列,防后台提交被 watchdog 杀
}

- (void)appDidBecomeActive {
    emitVisibility(true);
    auSetBackgrounded(NO);
    // Keep the game visible during hands-off animation/loading. Restore this after every
    // foreground transition because appDidEnterBackground releases the system idle lock.
    [UIApplication sharedApplication].idleTimerDisabled = YES;
    if (!_wasBackgrounded) { _link.paused = NO; return; }
    _wasBackgrounded = NO;
    if (gGLContext) {
        [EAGLContext setCurrentContext:gGLContext];
        // 与 mac resetGLStateAfterResume 同语义:真实 GL 状态可能已被系统动过,废弃缓存的绑定
        glBindFramebuffer(GL_FRAMEBUFFER, gFramebuffer);
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
    if (gCtx) {
        JSObjectRef global = JSContextGetGlobalObject(gCtx);
        JSStringRef name = JSStringCreateWithUTF8CString("__nativeResume");
        JSValueRef value = JSObjectGetProperty(gCtx, global, name, nullptr);
        JSStringRelease(name);
        if (JSValueIsObject(gCtx, value)) {
            JSObjectRef fn = JSValueToObject(gCtx, value, nullptr);
            if (fn && JSObjectIsFunction(gCtx, fn)) JSObjectCallAsFunction(gCtx, fn, nullptr, 0, nullptr, nullptr);
        }
        // Backgrounded apps otherwise only ever see the boot-time content check; the JS side
        // throttles this to one catalog request per 30 minutes, so focus flapping stays cheap.
        // Runtime renderer controls are resolved by the same pointer bridge, but are deliberately
        // independent of package downloads. A build may ship content overlays disabled while
        // frame coalescing is enabled; it still needs to receive a foreground kill switch.

    }
    _link.paused = NO;
}

- (void)tick {
    double t0 = nowUs();
    // conch P1:QuadRefBatch 依赖注入(batcher 惰性 init 需 GL 上下文;screenW/H 供投影)+ 帧计数清零
    if (!gBatcher.ready() && gGLContext) {
        gBatcher.texLookup = [](int id){ return gDec.glTextureEnsured((uint32_t)id); };
        gBatcher.init();
    }
    gDec.batcher = &gBatcher; gDec.sgraph = &gSG;
    gDec.screenW = (float)gScreenW; gDec.screenH = (float)gScreenH;
    gBatcher.quads = 0; gBatcher.escapes = 0; gBatcher.drawCalls = 0;
    gBatcher.frameReset();   // 顶点 VBO 环形写入每帧复位(首段 orphan 换存储,防跨帧隐式同步)
    drawOneFrame();
    // 系统 LaunchScreen 后的原生 UIImageView 只负责遮住尚未初始化的 EAGL surface。
    // 不能依赖业务 JS 主动发 dismissSplash：现有包从未发送该命令，日志中大厅已完成
    // splash_fadeout 后遮罩仍会永久盖住游戏。也不能仅看 present——纹理上传帧可能 draw=0。
    if (gFramePresented && gDec.drawCalls > 0 && _splashOverlay)
        [self dismissSplashAfterPresentedFrame];
    _frame++;
    // FPS 统计窗口(~0.5s 刷新一次标签，读均值稳定)
    double busy = (nowUs()-t0)/1000.0;   // 本帧 CPU 忙时间(ms)
    // 帧时环形缓冲(240 帧 ≈4s):百分位统计(平均值会掩盖 spike)
    static double ftRing[240]; static int ftN = 0, ftI = 0;
    ftRing[ftI] = busy; ftI = (ftI+1)%240; if (ftN<240) ftN++;
    // 坏帧快照:>25ms 当帧打 native 数据 + 同步调 JS __badFrame(场景标签/计数);限流 0.5s 一条
    static double lastBadUs = 0;
    if (busy > 25.0 && t0 - lastBadUs > 500000) {
        lastBadUs = t0;
        fprintf(stderr, "[badframe] 忙%.1fms JS%.1f compute%.1f batch%.1f{bounds%.1f walk%.1f bdata%.1f bsub%.1f drawMax%.1f} present%.1f other%.1f draw%u tex%u(推%u 待%zu) buf%u\n",
                busy, gFrameJSUs/1000.0, gFrameComputeUs/1000.0, gFrameBatchUs/1000.0,
                gBatcher.boundsMs, gBatcher.walkMs, gBatcher.bufferDataMs,
                gBatcher.bufferSubMs, gBatcher.drawMs,
                gFramePresentUs/1000.0,
                std::max(0.0, busy-(gFrameJSUs+gFrameComputeUs+gFrameBatchUs+gFramePresentUs)/1000.0),
                (unsigned)gDec.drawCalls, (unsigned)gDec.texUploads,
                (unsigned)gDec.texDeferred, gDec.texPending(), (unsigned)gDec.bufUploads);
        if (gCtx) { JSValueRef a = JSValueMakeNumber(gCtx, busy); callJsGlobal("__badFrame", 1, &a); }
    }
    _fpsWinFrames++;
    if (gFramePresented) _fpsWinPresents++;
    else if (gFrameHadContent) _fpsWinDropped++;   // 有新画面却没上屏 = 真故障
    else _fpsWinIdle++;                            // 画面无变化 = 正常省电跳过
    _fpsWinDraws += (long)gDec.drawCalls; _fpsWinBusy += busy;
    _fpsWinDecode += gFrameDecodeUs/1000.0; _fpsWinTex += (long)gDec.texUploads; _fpsWinBuf += (long)gDec.bufUploads;
    if (_fpsWinStart <= 0) _fpsWinStart = t0;
    double elapsed = (nowUs()-_fpsWinStart)/1000.0;
    if (elapsed >= 500.0 && _fpsLabel) {
        // FPS = 实际上屏次数/秒(不是 CADisplayLink 回调次数),两者不一致会显式标出来。
        double fps = _fpsWinPresents * 1000.0 / elapsed;
        double tickFps = _fpsWinFrames * 1000.0 / elapsed;
        double avgBusy = _fpsWinBusy / _fpsWinFrames;
        double avgDecode = _fpsWinDecode / _fpsWinFrames;   // native 解码/GL
        double avgJS = avgBusy - avgDecode;                 // JS(pixi 遍历+编码)
        long avgDraws = _fpsWinDraws / _fpsWinFrames, avgTex = _fpsWinTex / _fpsWinFrames, avgBuf = _fpsWinBuf / _fpsWinFrames;
        // 画面静止是常态(大厅待机/弹窗不动),不该显示成异常。只有真掉帧才加感叹号。
        _fpsLabel.text = _fpsWinDropped > 0
            ? [NSString stringWithFormat:@"FPS %.0f 掉帧%d!  %.1fms  draw %ld", fps, _fpsWinDropped, avgBusy, avgDraws]
            : (_fpsWinIdle > 0
               ? [NSString stringWithFormat:@"FPS %.0f 静%d  %.1fms  draw %ld", fps, _fpsWinIdle, avgBusy, avgDraws]
               : [NSString stringWithFormat:@"FPS %.0f  %.1fms  draw %ld", fps, avgBusy, avgDraws]);
        char fpsTag[64];
        if (_fpsWinDropped > 0)      snprintf(fpsTag, sizeof fpsTag, "FPS %.0f(tick%.0f 掉帧%d)", fps, tickFps, _fpsWinDropped);
        else if (_fpsWinIdle > 0)    snprintf(fpsTag, sizeof fpsTag, "FPS %.0f(静止%d/%d帧)", fps, _fpsWinIdle, _fpsWinFrames);
        else                         snprintf(fpsTag, sizeof fpsTag, "FPS %.0f", fps);
        // 屏幕标签每 0.5s 更新;日志每 4 个窗口(≈2s)一行——测试期日志量降 4 倍,趋势不丢。
        static int perfLogWin = 0;
        if ((perfLogWin++ % 4) == 0)
        fprintf(stderr, "[perf] %s  忙%.1fms(JS%.1f + 解码%.1f)  draw%ld tex%ld buf%ld  conch(quad%u esc%u)  sg脏(算%u 跳%u)\n",
                fpsTag, avgBusy, avgJS, avgDecode, avgDraws, avgTex, avgBuf, gBatcher.quads, gBatcher.escapes,
                gSG.lastComputed, gSG.lastSkipped);
        _fpsWinStart = nowUs(); _fpsWinFrames = 0; _fpsWinPresents = 0; _fpsWinIdle = 0; _fpsWinDropped = 0; _fpsWinDraws = 0; _fpsWinBusy = 0;
        _fpsWinDecode = 0; _fpsWinTex = 0; _fpsWinBuf = 0;
        // 帧时百分位(环形缓冲近 4s):排查态才打,坏帧本身由 [badframe] 覆盖
        if (PIXINATIVE_DIAG_LOG && ftN >= 60) {
            static double tmp[240]; memcpy(tmp, ftRing, sizeof(double)*ftN);
            std::sort(tmp, tmp+ftN);
            fprintf(stderr, "[perfp] 帧时 p50=%.1f p95=%.1f p99=%.1f max=%.1fms (n=%d)\n",
                    tmp[ftN/2], tmp[(int)(ftN*0.95)], tmp[(int)(ftN*0.99)], tmp[ftN-1], ftN);
        }
    }
}

// 触摸 → pointer：屏幕坐标换算回 1590x960 视图空间（_glView 自身坐标系即 1590x960，直接 locationInView）。
- (CGPoint)pt:(UITouch*)t { CGPoint p=[t locationInView:_glView]; return CGPointMake(p.x*_glScale, p.y*_glScale); } // 点数→native像素(=innerWidth空间)
// ---- 文本输入(隐形 UITextField 承接键盘,文本回传 JS 的 __kbInput/__kbDone)----
static void callKbJs(const char* fname, JSValueRef arg, JSValueRef second = nullptr) {
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef nm = JSStringCreateWithUTF8CString(fname);
    JSValueRef fv = JSObjectGetProperty(gCtx, global, nm, nullptr); JSStringRelease(nm);
    if (!JSValueIsObject(gCtx, fv)) return;
    JSObjectRef fn = JSValueToObject(gCtx, fv, nullptr);
    JSValueRef args[] = {arg, second};
    if (fn && JSObjectIsFunction(gCtx, fn))
        JSObjectCallAsFunction(gCtx, fn, nullptr, second ? 2 : 1, args, nullptr);
}
- (void)kbShow:(NSString*)text maxLen:(int)maxLen secure:(BOOL)secure fieldRect:(CGRect)rectPx
      fontSize:(CGFloat)fontSizePx fontFamily:(NSString*)fontFamily
     alignment:(NSInteger)alignment insets:(UIEdgeInsets)insetsPx {
    _kbWantHide = NO;   // 点击落在输入框上重新聚焦:取消 touchesEnded 的延迟收起
    _kbMaxLen = maxLen;
    _kbFieldRect = rectPx;
    _kbField.secureTextEntry = secure;
    // The game draws the visible string, but UIKit places the caret using its own font
    // metrics. Match the game's font and padding so the caret follows the same glyphs.
    NSString* nativeFontName = fontFamily;
    CGFloat fontSizePt = (_glScale > 0 && std::isfinite(fontSizePx) && fontSizePx > 0)
        ? fontSizePx / _glScale : 17;
    _kbField.font = [UIFont fontWithName:nativeFontName size:fontSizePt]
        ?: [UIFont systemFontOfSize:fontSizePt];
    _kbField.textAlignment = alignment == 1 ? NSTextAlignmentCenter
        : (alignment == 2 ? NSTextAlignmentRight : NSTextAlignmentLeft);
    if (_glScale > 0)
        _kbField.contentInsets = UIEdgeInsetsMake(insetsPx.top / _glScale,
                                                  insetsPx.left / _glScale,
                                                  insetsPx.bottom / _glScale,
                                                  insetsPx.right / _glScale);
    _kbField.text = text ?: @"";
    if (_glScale > 0 && rectPx.size.width > 0 && rectPx.size.height > 0) {
        CGRect fieldFrame = CGRectMake(rectPx.origin.x / _glScale,
                                       rectPx.origin.y / _glScale,
                                       rectPx.size.width / _glScale,
                                       rectPx.size.height / _glScale);
        // 输入框在游戏内较矮;保留视觉中心,给长按提供足够的触控高度。
        if (fieldFrame.size.height < 44) {
            fieldFrame.origin.y -= (44 - fieldFrame.size.height) / 2;
            fieldFrame.size.height = 44;
        }
        _kbField.frame = fieldFrame;
    } else {
        _kbField.frame = CGRectMake(-200, -200, 100, 30);
    }
    _kbField.hidden = NO;
    [_kbField becomeFirstResponder];
    // JS TextInput starts a new focus session with its selection at the end.
    // This UITextField is reused across popups, so reset UIKit's retained selection too.
    UITextPosition* end = _kbField.endOfDocument;
    _kbField.selectedTextRange = [_kbField textRangeFromPosition:end toPosition:end];
    [self kbUpdateShift:0.25];   // 键盘已在显示时切换输入框:通知不会再发,这里重算避让
}
- (void)kbHide {
    _kbWantHide = NO;
    if (_kbField.isFirstResponder) [_kbField resignFirstResponder];
    _kbField.hidden = YES;
}
- (void)kbKeep { _kbWantHide = NO; }   // JS 重复聚焦当前输入框(会话未变):仅取消延迟收起,不动文本
// JS 侧 value 写入(游戏截断/过滤后写回)同步到键盘缓冲:程序设置 text 不触发 EditingChanged,无回环
- (void)kbSetText:(NSString*)text {
    if (_kbField.isFirstResponder && ![_kbField.text isEqualToString:(text ?: @"")]) _kbField.text = text ?: @"";
}
- (void)kbFrameChanged:(NSNotification*)n {
    CGRect kf = [self.view convertRect:[n.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue] fromView:nil];
    _kbTopPt = (kf.origin.y >= self.view.bounds.size.height - 1) ? 0 : kf.origin.y;   // 移出屏幕 = 收起
    double dur = [n.userInfo[UIKeyboardAnimationDurationUserInfoKey] doubleValue];
    fprintf(stderr, "[kb-shift] frameChanged kf.y=%.0f viewH=%.0f → kbTop=%.0fpt dur=%.2f\n",
            kf.origin.y, self.view.bounds.size.height, _kbTopPt, dur);
    [self kbUpdateShift:(dur > 0 ? dur : 0.25)];
}
- (void)kbUpdateShift:(double)dur {
    CGFloat shift = 0;
    CGFloat bottomPx = CGRectGetMaxY(_kbFieldRect);
    if (_kbTopPt > 0 && bottomPx > 0) {
        CGFloat fieldBottomPt = bottomPx / _glScale + 12;   // 游戏像素→屏幕点,+12pt 边距
        if (fieldBottomPt > _kbTopPt) shift = fieldBottomPt - _kbTopPt;
    }
    CGAffineTransform t = shift > 0 ? CGAffineTransformMakeTranslation(0, -shift) : CGAffineTransformIdentity;
    if (CGAffineTransformEqualToTransform(_glView.transform, t)) return;
    fprintf(stderr, "[kb-shift] fieldBottom=%.0fpx(%.0fpt) kbTop=%.0fpt → shift=%.0fpt\n",
            bottomPx, bottomPx / _glScale, _kbTopPt, shift);
    [UIView animateWithDuration:dur delay:0 options:UIViewAnimationOptionCurveEaseOut
                     animations:^{ self->_glView.transform = t; } completion:nil];
}
- (void)kbChanged {
    if (!gCtx) return;
    BOOL composing = _kbField.markedTextRange != nil;
    fprintf(stderr, "[kb] changed length=%lu composing=%d\n",
            (unsigned long)_kbField.text.length, composing);
    JSStringRef s = JSStringCreateWithUTF8CString((_kbField.text ?: @"").UTF8String);
    callKbJs("__kbInput", JSValueMakeString(gCtx, s), JSValueMakeBoolean(gCtx, composing));
    JSStringRelease(s);
}
- (BOOL)textFieldShouldReturn:(UITextField*)tf {
    callKbJs("__kbDone", JSValueMakeNumber(gCtx, 1));
    [tf resignFirstResponder];
    return NO;
}
- (void)textFieldDidEndEditing:(UITextField*)tf {
    tf.hidden = YES;
    callKbJs("__kbDone", JSValueMakeNumber(gCtx, 0));   // return 路径已清 active,JS 端幂等
}
- (BOOL)textField:(UITextField*)tf shouldChangeCharactersInRange:(NSRange)r replacementString:(NSString*)s {
    if (_kbMaxLen <= 0) return YES;
    NSUInteger newLen = tf.text.length - r.length + s.length;
    return newLen <= (NSUInteger)_kbMaxLen;
}

- (void)touchesBegan:(NSSet<UITouch*>*)ts withEvent:(UIEvent*)e {
    gLastTouchUs = nowUs();
    CGPoint p=[self pt:ts.anyObject];
    // 点画面收键盘,但:1) 触点命中当前输入框矩形 → 不收(库经 setTimeout 才 re-focus,同步链里等不到);
    // 2) 其余延迟到 pointerup 走完 JS 再执行(JS 重新聚焦会经 __kbShow/__kbKeep 取消)。
    // 立即 resign 会走"收起→重弹+文本重置",覆盖键盘会话里已输入的字符(丢字 bug)。
    _kbWantHide = _kbField.isFirstResponder && !CGRectContainsPoint(CGRectInset(_kbFieldRect, -20, -20), p);
    [self emitTouches:ts type:"pointerdown" buttons:1];
}
- (void)emitTouches:(NSSet<UITouch*>*)touches type:(const char*)type buttons:(int)buttons {
    for (UITouch* touch in touches) {
        NSNumber* identity = [_touchIds objectForKey:touch];
        if (!identity && strcmp(type,"pointerdown")==0) {
            identity = @(_nextTouchId++);
            if (_touchIds.count==0) _primaryTouchId=identity.integerValue;
            [_touchIds setObject:identity forKey:touch];
        }
        if (!identity) continue;
        CGPoint p=[self pt:touch];
        double pressure=buttons?(touch.maximumPossibleForce>0?touch.force/touch.maximumPossibleForce:0.5):0;
        emitPointer(type,p.x,p.y,0,buttons,identity.doubleValue,touch.type==UITouchTypeStylus?"pen":"touch",identity.integerValue==_primaryTouchId,pressure);
        if (!buttons) [_touchIds removeObjectForKey:touch];
    }
    if (_touchIds.count==0) _primaryTouchId=-1;
}
- (void)touchesMoved:(NSSet<UITouch*>*)ts withEvent:(UIEvent*)e {
    gLastTouchUs = nowUs();
    [self emitTouches:ts type:"pointermove" buttons:1];
}
- (void)touchesEnded:(NSSet<UITouch*>*)ts withEvent:(UIEvent*)e {
    gLastTouchUs = nowUs();
    [self emitTouches:ts type:"pointerup" buttons:0];   // synchronous keyboard focus updates
    if (_kbWantHide) [self kbHide];   // JS 未重新聚焦 → 点了输入框以外,收键盘
}
- (void)touchesCancelled:(NSSet<UITouch*>*)ts withEvent:(UIEvent*)e {
    _kbWantHide = NO;   // 触摸被系统取消:不收键盘
    [self emitTouches:ts type:"pointercancel" buttons:0];
}

// ===== reload 弹窗（100% 还原 CDN app-reload：遮罩 rgba(0,0,0,.5) + 白底圆角弹窗 + logo + 标题 + 内容 + 红色 REFRESH 按钮）=====
- (UIFont*)nativeFont:(CGFloat)size bold:(BOOL)bold {
    return bold ? [UIFont boldSystemFontOfSize:size] : [UIFont systemFontOfSize:size];
}
- (void)showReloadTitle:(NSString*)title content:(NSString*)content {
    if (_reloadOverlay) [self hideReload];
    title   = title.length   ? title   : @"Application error";
    content = content.length ? content : @"The application reported an error. See the runtime log.";

    // 遮罩：全屏 rgba(0,0,0,0.5)，拦截触摸
    UIView* overlay = [[ReloadOverlayView alloc] initWithFrame:self.view.bounds];
    overlay.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    overlay.backgroundColor = [UIColor colorWithWhite:0 alpha:0.5];
    overlay.userInteractionEnabled = YES;

    // 弹窗：白底、圆角 20、以宽 50% 排版,按安全区高度等比适配并居中
    UIView* popup = [UIView new];
    popup.backgroundColor = [UIColor whiteColor];
    popup.layer.cornerRadius = 20;
    popup.translatesAutoresizingMaskIntoConstraints = NO;
    [overlay addSubview:popup];

    UILabel* titleLbl = [UILabel new];
    titleLbl.text = title; titleLbl.textColor = [UIColor blackColor];
    titleLbl.font = [self nativeFont:24 bold:YES];         // reload_title: 24px, 700
    titleLbl.textAlignment = NSTextAlignmentCenter; titleLbl.numberOfLines = 0;
    titleLbl.translatesAutoresizingMaskIntoConstraints = NO;

    UILabel* contentLbl = [UILabel new];
    contentLbl.text = content; contentLbl.textColor = [UIColor blackColor];
    contentLbl.font = [self nativeFont:16 bold:NO];
    contentLbl.textAlignment = NSTextAlignmentCenter; contentLbl.numberOfLines = 0;
    contentLbl.translatesAutoresizingMaskIntoConstraints = NO;

    // 按钮 REFRESH：红底 #ed6755、圆角 5、宽 200、白字 16、阴影
    UIButton* btn = [UIButton buttonWithType:UIButtonTypeCustom];
    [btn setTitle:@"REFRESH" forState:UIControlStateNormal];
    [btn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    btn.titleLabel.font = [self nativeFont:16 bold:NO];
    btn.backgroundColor = [UIColor colorWithRed:237/255.0 green:103/255.0 blue:85/255.0 alpha:1.0];
    btn.layer.cornerRadius = 5;
    btn.layer.shadowColor = btn.backgroundColor.CGColor;
    btn.layer.shadowOffset = CGSizeMake(0, 6); btn.layer.shadowRadius = 13; btn.layer.shadowOpacity = 0.6;
    btn.translatesAutoresizingMaskIntoConstraints = NO;
    [btn addTarget:self action:@selector(onReloadTap:) forControlEvents:UIControlEventTouchUpInside];

    [popup addSubview:titleLbl]; [popup addSubview:contentLbl]; [popup addSubview:btn];

    // 布局(points≈CSS px)：padding 30/30/40，img 122(下距15)，title，content(下距60)，button(宽200,高≈48)
    CGFloat pad = 30, padBottom = 40;
    [NSLayoutConstraint activateConstraints:@[
        [popup.centerXAnchor constraintEqualToAnchor:overlay.safeAreaLayoutGuide.centerXAnchor],
        [popup.centerYAnchor constraintEqualToAnchor:overlay.safeAreaLayoutGuide.centerYAnchor],
        [popup.widthAnchor constraintEqualToAnchor:overlay.widthAnchor multiplier:0.5],

        [titleLbl.topAnchor constraintEqualToAnchor:popup.topAnchor constant:pad],
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

    [self.view addSubview:overlay];
    _reloadOverlay = overlay;
    fprintf(stderr, "[reload] 弹窗已显示: %s / %s\n", title.UTF8String, content.UTF8String);
}
- (void)hideReload {
    if (_reloadOverlay) { [_reloadOverlay removeFromSuperview]; _reloadOverlay = nil; }
}
- (void)onReloadTap:(UIButton*)b {
    // 按下缩放反馈(:active scale .95)后 reload
    [UIView animateWithDuration:0.08 animations:^{ b.transform = CGAffineTransformMakeScale(0.95,0.95); }
        completion:^(BOOL fin){ b.transform = CGAffineTransformIdentity; [self reloadGame]; }];
}
- (void)reloadGame {
    if (_reloading) {
        fprintf(stderr, "[reload] reload already in progress\n");
        return;
    }
    _reloading = YES;
    fprintf(stderr, "[reload] 重建 JSC 上下文 + 重新登录...\n");
    [self hideReload];
    [self kbHide];
    // 1) 停所有 JS timer(其 fn 属于旧上下文，释放前必须清)。趁旧上下文还活着解除全部
    //    JSValueProtect,否则整个旧 JS 堆经保护集常驻(reload 一次泄一个世界)。
    for (NSTimer* t in gTimers.allValues) [t invalidate];
    [gTimers removeAllObjects];
    if (gTimerFns) {
        if (gCtx) for (NSValue* v in gTimerFns.allValues) JSValueUnprotect(gCtx, (JSObjectRef)v.pointerValue);
        [gTimerFns removeAllObjects];
    }
    // 2) 音频/zip/骨骼/tween 世代状态全清:旧世界的回调与缓存决不能与新 JS 对话(旧循环 BGM
    //    穿越 reload、旧 zip token 撞新世界同号、instId 复用拿到旧 slot 状态,均在此掐断)。
    shutdownAudio();
    if (gZipCache) [gZipCache removeAllObjects];
    if (gZipLru) [gZipLru removeAllObjects];
    gNativeTweens.clear();
    // 3) 释放旧 JS 上下文
    if (gCtx) { JSGlobalContextRelease(gCtx); gCtx = nullptr; }
    // 3) 重置命令解码器(GL 对象 id 映射清零；旧 GL 句柄泄漏，reload 可接受)，默认 FBO 仍是 EAGL 的 framebuffer
    gPendingRoot = 0; gShadowRoot = 0;
    gNativeBatchSerial = 0;   // 新世代从头计批次:fgui 预约的 reservedAtFrame 属于旧世代,不能跨 reload 比较
    gDec = cmd::Decoder();
    gDec.setDefaultFbo(gFramebuffer);
    // 白纹理的 GL 句柄跨 reload 存活(它不属于任何 JS 世代),但 decoder 的 id 表已被清空,
    // 必须重新登记,否则新世代里 fgui 的所有纯色图元都会采样到 0 号纹理。
    if (gNativeFguiWhiteTexture) {
        gDec.registerExternalTexture(kNativeFguiWhiteCid, gNativeFguiWhiteTexture);
    }
    // 4) 重新 bootstrap(新 gCtx，重跑 game_host → 全新登录)
    gStartUs = nowUs();
    if (!setupGame()) {
        fprintf(stderr, "[reload] setupGame 失败\n");
        nativeLogError(@"runtime", @"reload_setup_failed",
                                            @"iOS runtime reload setup failed", NO);
    }
    _reloading = NO;
    fprintf(stderr, "[reload] reload complete\n");
}
@end

// ---- reload host 函数(JS 调用 → 切主线程操作 UIKit)----
static JSValueRef cbKbShow(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    NSString* text = argc>0 ? jsToNSString(ctx, a[0]) : @"";
    int maxLen = argc>1 ? (int)JSValueToNumber(ctx, a[1], e) : 0;
    BOOL secure = argc>2 && JSValueToNumber(ctx, a[2], e) != 0;
    // a[3] = multiline(暂未用) a[4..7] = 输入框 x,y,w,h(游戏像素:触点命中判定 + 键盘避让)
    CGRect rect = CGRectZero;
    if (argc > 7) rect = CGRectMake(JSValueToNumber(ctx, a[4], e), JSValueToNumber(ctx, a[5], e),
                                    JSValueToNumber(ctx, a[6], e), JSValueToNumber(ctx, a[7], e));
    CGFloat fontSize = argc > 8 ? JSValueToNumber(ctx, a[8], e) : 0;
    NSString* fontFamily = argc > 9 ? jsToNSString(ctx, a[9]) : @"";
    NSInteger alignment = argc > 11 ? (NSInteger)JSValueToNumber(ctx, a[11], e) : 0;
    UIEdgeInsets insets = UIEdgeInsetsZero;
    if (argc > 15) insets = UIEdgeInsetsMake(JSValueToNumber(ctx, a[12], e),
                                            JSValueToNumber(ctx, a[15], e),
                                            JSValueToNumber(ctx, a[14], e),
                                            JSValueToNumber(ctx, a[13], e));
    if (gRootVC) [gRootVC kbShow:text maxLen:maxLen secure:secure fieldRect:rect
                        fontSize:fontSize fontFamily:fontFamily alignment:alignment insets:insets];
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbKbSetText(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef*) {
    if (gRootVC && argc > 0) [gRootVC kbSetText:jsToNSString(ctx, a[0])];
    return JSValueMakeUndefined(ctx);
}
// __snapshot(quality) -> "data:image/jpeg;base64,...":整屏截图(UIKit 合成内容,所见即所得)。
// 替代 pixi extract 全屏截图链(generateTexture/renderTexture/readPixels),不触碰 GL 与 C++ 场景图。
static JSValueRef cbSnapshot(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    double q = argc>0 ? JSValueToNumber(ctx, a[0], e) : 0.85;
    if (!(q > 0 && q <= 1)) q = 0.85;
    if (!gRootVC) return JSValueMakeNull(ctx);
    UIView* v = gRootVC.view;
    UIGraphicsImageRendererFormat* fmt = [UIGraphicsImageRendererFormat preferredFormat];
    fmt.scale = 1;   // 邮件附件用途,1x 足够,base64 体积小
    UIGraphicsImageRenderer* r = [[UIGraphicsImageRenderer alloc] initWithBounds:v.bounds format:fmt];
    UIImage* img = [r imageWithActions:^(UIGraphicsImageRendererContext* c){
        [v drawViewHierarchyInRect:v.bounds afterScreenUpdates:NO];   // 当前屏幕合成内容(含 GL layer)
    }];
    NSData* d = img ? UIImageJPEGRepresentation(img, q) : nil;
    if (!d.length) return JSValueMakeNull(ctx);
    NSString* url = [NSString stringWithFormat:@"data:image/jpeg;base64,%@", [d base64EncodedStringWithOptions:0]];
    JSStringRef s = JSStringCreateWithUTF8CString(url.UTF8String);
    JSValueRef ret = JSValueMakeString(ctx, s); JSStringRelease(s);
    return ret;
}
static JSValueRef cbKbHide(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    if (gRootVC) [gRootVC kbHide];
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbKbKeep(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    if (gRootVC) [gRootVC kbKeep];
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbShowReload(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef*) {
    NSString* title   = argc>0 ? jsToNSString(ctx, args[0]) : @"";
    NSString* content = argc>1 ? jsToNSString(ctx, args[1]) : @"";
    dispatch_async(dispatch_get_main_queue(), ^{ [gRootVC showReloadTitle:title content:content]; });
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbHideReload(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    dispatch_async(dispatch_get_main_queue(), ^{ [gRootVC hideReload]; });
    return JSValueMakeUndefined(ctx);
}
static JSValueRef cbReload(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    // 从 JS 内直接调用(如 location.reload())：延到下一轮 runloop 再重建上下文，避免在旧上下文执行栈里销毁它。
    dispatch_async(dispatch_get_main_queue(), ^{ [gRootVC reloadGame]; });
    return JSValueMakeUndefined(ctx);
}
// Non-blocking editor used by native_dat_gui.js. Keep the JS callback protected until UIKit
// completes, mirroring the macOS sheet contract and avoiding a nested JS run loop.
static JSValueRef cbNativePrompt(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                 const JSValueRef args[], JSValueRef*) {
    NSString* title = argc>0 ? jsToNSString(ctx,args[0]) : @"Edit value";
    NSString* value = argc>1 ? jsToNSString(ctx,args[1]) : @"";
    JSObjectRef callback=nullptr;
    if(argc>2&&JSValueIsObject(ctx,args[2])){
        JSObjectRef c=JSValueToObject(ctx,args[2],nullptr);
        if(c&&JSObjectIsFunction(ctx,c))callback=c;
    }
    if(!callback||!gRootVC)return JSValueMakeUndefined(ctx);
    JSValueProtect(ctx,callback);
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController* a=[UIAlertController alertControllerWithTitle:(title.length?title:@"Edit value")
            message:nil preferredStyle:UIAlertControllerStyleAlert];
        [a addTextFieldWithConfigurationHandler:^(UITextField* f){f.text=value?:@"";}];
        void (^finish)(NSString*)=^(NSString* answer){
            if(gCtx==ctx){
                JSValueRef av=JSValueMakeNull(ctx);JSStringRef s=nullptr;
                if(answer){s=JSStringCreateWithUTF8CString(answer.UTF8String?:"");av=JSValueMakeString(ctx,s);}
                JSValueRef exc=nullptr;JSObjectCallAsFunction(ctx,callback,nullptr,1,&av,&exc);
                if(s)JSStringRelease(s);
                JSValueUnprotect(ctx,callback);
            }
        };
        [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction*){finish(nil);}]];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction*){finish(a.textFields.firstObject.text?:@"");}]];
        [gRootVC presentViewController:a animated:YES completion:nil];
    });
    return JSValueMakeUndefined(ctx);
}

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)opts {


    // A game should not dim and auto-lock while it is running in the foreground.
    app.idleTimerDisabled = YES;
    // 资源根：bundle/game（补丁 JS）+ Info.plist AssetServerURL（局域网 assets）。
    removeLegacyAssetCache();
    gGameDir = [[NSBundle mainBundle].resourcePath stringByAppendingPathComponent:@"game"];
    return YES;
}
- (void)applicationWillTerminate:(UIApplication*)application {
    (void)application;

}
@end

// iOS 13+ window ownership belongs to the scene delegate.  Keeping the window
// removes UIKit's legacy-lifecycle warning on launch.
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong,nonatomic) UIWindow* window;
@end
@implementation SceneDelegate
- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
    options:(UISceneConnectionOptions*)connectionOptions {
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = [[GameViewController alloc] init];
    [self.window makeKeyAndVisible];

    // 冷启动就是被 fb<AppID>:// 拉起来的场景:URL 随连接选项到达,而不是走 openURLContexts:。
}
// Facebook Standard Login 会切到 Facebook App,结果经自定义 scheme 回跳。UIScene 应用里
// 这类回调只落在 scene delegate 上(AppDelegate 的 application:openURL:options: 不会被调用),
// 所以在这里转交 FBSDK;SDK 不认领的 URL 目前没有其它消费者。


- (void)sceneDidBecomeActive:(UIScene*)scene {

    // Resource mappings are selected by the Shell loading barrier and remain fixed for
    // this process. Foregrounding must not swap descriptor/atlas versions during a scene load.
}
- (void)sceneWillResignActive:(UIScene*)scene {

}
- (void)sceneWillEnterForeground:(UIScene*)scene {
    (void)scene;

}
- (void)sceneDidEnterBackground:(UIScene*)scene {
    (void)scene;


}
@end

int main(int argc, char** argv) {
    @autoreleasepool {

        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
