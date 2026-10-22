#pragma once
// host_core_common.h — 三端(mac / iOS / Android)共用的宿主核心:纯 C++,只依赖 JSC C API 与 src/cmdbuf、src/common。
// 2026-09-02 由 host_core.mm 按"不碰 Foundation/CoreText/Apple 专有全局"的闭包分层而来(结构方案 S-1 的 Android 前置)。
// Apple 部分见 host_core.h / host_core.mm;Android 经 src/android/jsc_shim.h(QuickJS 或 V8 的兼容层)提供同一套 JSC C API。
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <JavaScriptCore/JavaScriptCore.h>
#if TARGET_OS_IPHONE
#define HOST_PLATFORM_TAG "ios"
#else
#define HOST_PLATFORM_TAG "mac"
#endif
#include <cstdio>
#define HOST_LOG(...) fprintf(stderr, __VA_ARGS__)
#elif defined(__ANDROID__)
#include "../android/jsc_shim.h"
#include <android/log.h>
#define HOST_PLATFORM_TAG "android"
#define HOST_LOG(...) __android_log_print(ANDROID_LOG_INFO, "pixi_native", __VA_ARGS__)
#else
#error "host_core_common: unsupported platform"
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "../cmdbuf/batcher.h"
#include "../cmdbuf/decoder.h"
#include "../cmdbuf/native_animation_frame.h"
#include "../cmdbuf/native_fgui.h"
#include "../cmdbuf/scenegraph.h"
#include "../cmdbuf/spinex.h"
#include "../jsc_rooted_value_vector.h"

// ---- 共享类型 / 共享状态(定义在 host_core_common.cpp) ----
struct NativeTween {
    int id=0,node=0,slots[4]={-1,-1,-1,-1},valueCount=0;
    int repeat=0,mode=0;                 // mode:0=curve interpolation,1=shake,2=delayed event
    bool yoyo=false,emitValues=false,started=false,paused=false;
    float from[4]={0,0,0,0},to[4]={0,0,0,0},durationMs=0,startMs=0,delayMs=0,pausedAtMs=0;
    uint32_t randomState=0;
    uint32_t nodeGen=0;   // 目标节点代际(启动时记录;advance 比对,slot 复用后的悬空 tween 自失效)
    std::vector<float> curve;
};
struct NativeFguiPreparseCounters {
    uint64_t requests=0,jobs=0,deduped=0,preparseCacheHits=0,syncHits=0,syncFallbacks=0;
    uint64_t failures=0,xmlWarmed=0,xmlHits=0,xmlFallbacks=0,xmlBridgeNodes=0,entryGets=0,entryMisses=0,entryBytes=0;
    uint64_t blueprintPublishes=0,syncBlueprintBuilds=0,manifestGets=0,manifestMisses=0,manifestItems=0,itemGets=0,itemMisses=0,typeGets=0,spriteGets=0,spriteMisses=0;
    double backgroundInflateMs=0,backgroundXmlMs=0,backgroundBlueprintMs=0,syncInflateMs=0,syncBlueprintMs=0,lastInflateMs=0;
    double blueprintPublishMs=0,inflateBridgeMs=0,xmlBridgeMs=0,maxXmlBridgeMs=0,manifestBridgeMs=0,maxManifestBridgeMs=0,spriteBridgeMs=0;
};
struct NativeFguiPreparseWaiter { int token=0; JSGlobalContextRef context=nullptr; };
struct NativeSpinePrepareCounters {
    uint64_t requests=0,jobs=0,deduped=0,cacheHits=0,ready=0,failures=0,evictions=0;
    uint64_t manifestGets=0,imageGets=0,directImageRequests=0,directImageMisses=0,atlasGets=0,jsonGets=0,fallbacks=0;
    uint64_t preparedLoads=0,loadFailures=0,skeletonCalls=0,compatSkeletonCalls=0;
    double backgroundZipMs=0,backgroundAtlasMs=0,backgroundJsonMs=0,jsonBridgeMs=0;
    double atlasMainMs=0,skeletonMainMs=0,compatSkeletonMainMs=0,maxSkeletonMainMs=0;
};
struct NativeSpinePrepareWaiter { int token=0; JSGlobalContextRef context=nullptr; bool wantsLease=false; };   // wantsLease:该消费链将读 holder,需在途引用
extern cmd::Decoder gDec;
extern sg::SceneGraph gSG;
extern nfgui::Runtime gNativeFgui;
extern float* gSGBuf;
extern int gPendingRoot;
extern uint64_t gNativeBatchSerial;
extern float gSgClear[4];
extern spinex::Runtime gSpinexRt;
extern JSGlobalContextRef gCtx;
extern double gStartUs;
extern std::map<int,NativeTween> gNativeTweens;
extern uint64_t gNativeFguiManagedFreeDeferrals;
extern uint64_t gFguiTextMutationCalls, gFguiTextMutationRows, gFguiTextMutationFailures;
extern double gFguiTextMutationMs;
extern uint64_t gFguiOrderedMutationCalls, gFguiOrderedMutationRows, gFguiOrderedMutationFailures;
extern double gFguiOrderedMutationMs;
extern uint64_t gFguiChildBatchCalls, gFguiChildBatchRows;
extern NativeSpinePrepareCounters gSpinePrepareStats;
extern bool gAnimAdvancedThisFrame;

// ---- 共享模板(须在头文件里) ----
template<typename V,typename G>
static JSValueRef spinexNameArray(JSContextRef ctx,V& vec,JSValueRef* exc,G getName){
    jscbridge::RootedValueVector names(ctx);names.reserve(vec.size());
    for(size_t i=0;i<vec.size();++i){
        const char* s=getName(vec[i]);
        JSStringRef js=JSStringCreateWithUTF8CString(s?:"");
        names.push_back(JSValueMakeString(ctx,js));JSStringRelease(js);
    }
    return JSObjectMakeArray(ctx,names.size(),names.empty()?nullptr:names.data(),exc);
}

// ---- 平台接口(各平台宿主实现,common 只调用;Android 须提供同签名实现) ----
extern int W, H;   // 逻辑视口:mac 1280×720 / iOS 1590×960 兜底,运行时按窗口/屏幕覆盖
sg::CommandReceipt freeSceneNodeFromJs(int id, uint32_t expectedGeneration = 0);
void spinexFrameDrive();

// fgui 行构建计量(host_core_common.cpp;__nativeFguiRuntimeStats 暴露,gFguiRowProfileLog 由 Apple 非生产 env 置位)
extern uint64_t gFguiSyncRowsCalls, gFguiSyncRowsRows; extern double gFguiSyncRowsUs; extern bool gFguiRowProfileLog;

// ---- 共享函数(定义在 host_core_common.cpp) ----
double nowUs();
void setJsNumber(JSContextRef ctx,JSObjectRef o,const char* name,double v);
double richV2Num(JSContextRef ctx,JSObjectRef o,const char* k,double dflt);
JSValueRef cbSgAlloc(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
JSValueRef cbSgAllocBatch(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbSgCommands(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbSgCommandsV2(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbSgFree(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgSetParent(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgRemove(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgNativeParent(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgNativeChildCount(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgNativeChildAt(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgNativeChildren(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgNativeSubtree(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgAdvanceAnimations(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgDebtRollup(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgCompute(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgPublishSeal(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgAuthorityDump(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgChainWorld(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgUpdateBounds(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgHit(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbNativeNineSlice(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbNativeTiling(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbNativeBitmapGlyphs(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbNativeGraphicsMesh(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbNativeTweenStart(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbNativeTweenStartEx(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbNativeTweenCancel(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbNativeTweenPause(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbNativeTweenAdvance(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbNativeTweenAdvanceEx(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e);
JSValueRef cbSgFlagRender(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSgNativeQuadHit(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
JSValueRef cbSyncProgram(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
JSValueRef cbNowUs(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*);
JSValueRef cbLog(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
JSValueRef cbEvalCode(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef args[], JSValueRef* exc);
void setJsProperty(JSContextRef ctx,JSObjectRef o,const char* key,JSValueRef v);
JSValueRef fguiJsString(JSContextRef ctx,const std::string& value);
JSObjectRef fguiStringArray(JSContextRef ctx,const std::vector<std::string>& values,JSValueRef* exc);
JSValueRef nativeFguiResourceRow(JSContextRef ctx,const nfgui::Resource& r,JSValueRef* exc);
const char* fguiKindName(nfgui::ObjectKind kind);
JSObjectRef fguiNodeToJs(JSContextRef ctx,const nfgui::InstanceNode* n,JSValueRef* exc);
JSObjectRef fguiNodeSyncToJs(JSContextRef ctx,const nfgui::InstanceNode* n,JSValueRef* exc);
std::string fguiInstanceSchemaToken(const nfgui::Instance& inst);
JSObjectRef fguiInstanceResultToJs(JSContextRef ctx,int id,double nativeMs,double ageNowMs, bool claimedReservation,bool checkout,JSValueRef* exc, const std::string* expectedSchema=nullptr);
JSValueRef cbNativeFguiNode(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiNodesAll(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc, const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSyncRows(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc, const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiChildren(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc, const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiChildOrder(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiNodeRoots(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiDisplayChildCount(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiDisplayChildAt(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiDisplayChildIndex(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiAdoptDisplayContainer(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiReparent(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetNodeSize(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiNodeGeometry(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiJournalEpoch(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef*);
JSValueRef cbNativeFguiSetNodePosition(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetNodeScale(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetNodeRotation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetNodePivot(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetNodeAlpha(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetNodeDisplayPosition(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetNodeDisplayScale(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetNodeDisplayRotation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetNodeVisible(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiGetMovieClip(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiSetMovieClip(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiTransitionAdvance(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiTransitionSignals(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef* exc);
JSValueRef cbNativeFguiTransitionVisibilitySignals(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef* exc);
JSValueRef cbNativeFguiTransitionPropertySignals(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef* exc);
JSValueRef cbNativeFguiTaskAdd(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiTaskCancel(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiTaskDrain(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef* exc);
JSValueRef cbNativeFguiDestroy(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc);
JSValueRef cbNativeFguiRuntimeStats(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef*);
JSValueRef cbNativeFguiCompatibilityIssues(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef* exc);
JSValueRef cbNativeSpineRecordMain(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbNativeSpineRecordCompatSkeleton(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
spinex::Instance* spinexInstArg(JSContextRef ctx,size_t argc,const JSValueRef args[],JSValueRef* exc);
spine::Animation* spinexAnimAt(spinex::Instance* inst,int animIdx);
JSValueRef cbSpinexRelease(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexRetain(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexCreate(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexDestroy(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetAnimation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexAddAnimation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetEmptyAnimation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexClearTrack(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexClearTracks(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetTimeScale(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetTrackTimeScale(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetMix(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetDefaultMix(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetSkin(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetToSetupPose(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetSlotsToSetupPose(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSlotSetColor(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSlotSetToSetupPose(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexUpdateCache(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexPhysicsTranslate(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexPhysicsRotate(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexTrackInfo(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexIsDrawable(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetTrackTime(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetTrackLoop(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetClockMode(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexFeedTime(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexFeedTimeBatch(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetSkeletonColor(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSlotBind(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSlotUnbind(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexDrainEvents(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexString(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexLocalBounds(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSlotBounds(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexSetSlotMeshScale(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexProbeQuads(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc);
JSValueRef cbSpinexStats(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef*);
void setFn(JSContextRef ctx, JSObjectRef g, const char* name, JSObjectCallAsFunctionCallback cb);
void advanceNativeAnimationsForFrame(int root);
JSValueRef cbGlReadPixels(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e);
void emitPointer(const char* type, double x, double y, int button, int buttons,
                 double pointerId = 1, const char* pointerType = "mouse", bool primary = true, double pressure = -1);
void emitVisibility(bool visible);

// 把三端共用的回调注册到 JS 全局对象(Apple 由 hostCoreRegisterBindings 调用;Android 直接调用)。
void hostCoreRegisterCommonBindings(JSContextRef ctx, JSObjectRef global);
