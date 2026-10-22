// host_core_common.cpp — 见 host_core_common.h。函数体来自 host_core.mm(fprintf(stderr,…) → HOST_LOG,其余逐字)。
#include "host_core_common.h"

cmd::Decoder gDec;

// Apple download workers may still be inside a bounded network request when AppKit begins
// process termination.  They intentionally outlive the UI runtime and can still finish a cache
// transaction, so the process-wide cache must not participate in C++ static destruction while
// those workers are alive.  The OS reclaims it with the process.


sg::SceneGraph gSG;

nfgui::Runtime gNativeFgui;

float* gSGBuf = nullptr;

int gPendingRoot = 0;

uint64_t gNativeBatchSerial = 0; // strict coverage must include the actual C++ paint pass

float gSgClear[4] = {0,0,0,1};

spinex::Runtime gSpinexRt;        // spine 全量 native 化运行时(P1a:实例/驱动/事件;present 统一驱动)

JSGlobalContextRef gCtx = nullptr;

double gStartUs = 0;

// A frame may contain several tweens targeting the same native slot.  Preserve the stable
// numeric creation order used by the JS bridge so the last/highest tween wins deterministically;
// unordered iteration made the visible result depend on hash-table growth.
std::map<int,NativeTween> gNativeTweens;

double nowUs(){ return std::chrono::duration<double,std::micro>(
    std::chrono::high_resolution_clock::now().time_since_epoch()).count(); }

void setJsNumber(JSContextRef ctx,JSObjectRef o,const char* name,double v){JSStringRef k=JSStringCreateWithUTF8CString(name);JSObjectSetProperty(ctx,o,k,JSValueMakeNumber(ctx,v),kJSPropertyAttributeNone,nullptr);JSStringRelease(k);}
// 属性名 JSStringRef 驻留(fgui 行构建专用):每个节点要写 ~45 个固定属性名,逐次 Create/Release 是纯浪费
// (Apple 侧一次 UTF-8 解码+分配,Android shim 侧一次 new/delete std::string)。每个字面量一个函数级 static,
// C++11 保证线程安全初始化;JSStringRef 不绑定 JSContext/VM(Apple 为引用计数对象、shim 为堆上 std::string 包装),
void setJsProperty(JSContextRef ctx,JSObjectRef o,const char* key,JSValueRef v);
#ifndef PIXINATIVE_FGUI_KEY_INTERN
#define PIXINATIVE_FGUI_KEY_INTERN 1   // 0 = 逐次创建(原行为),仅供 A/B 计量对照构建;产品构建恒为 1
#endif
#if PIXINATIVE_FGUI_KEY_INTERN
#define FGUI_JS_KEY(lit) ([]() -> JSStringRef { static JSStringRef k = JSStringCreateWithUTF8CString(lit); return k; }())
static inline void setJsNumberK(JSContextRef ctx,JSObjectRef o,JSStringRef k,double v){JSObjectSetProperty(ctx,o,k,JSValueMakeNumber(ctx,v),kJSPropertyAttributeNone,nullptr);}
static inline void setJsPropertyK(JSContextRef ctx,JSObjectRef o,JSStringRef k,JSValueRef v){JSObjectSetProperty(ctx,o,k,v,kJSPropertyAttributeNone,nullptr);}
#else
#define FGUI_JS_KEY(lit) (lit)
static inline void setJsNumberK(JSContextRef ctx,JSObjectRef o,const char* k,double v){setJsNumber(ctx,o,k,v);}
static inline void setJsPropertyK(JSContextRef ctx,JSObjectRef o,const char* k,JSValueRef v){setJsProperty(ctx,o,k,v);}
#endif
// 行构建计量(常驻、每批两次 nowUs,可忽略):__nativeFguiRuntimeStats 暴露 syncRowsCalls/Rows/Us;
// gFguiRowProfileLog(Apple 非生产 env PIXINATIVE_FGUI_ROW_PROF=1)开启时每 250 批打一行均值,供 A/B 计量。
uint64_t gFguiSyncRowsCalls=0,gFguiSyncRowsRows=0;double gFguiSyncRowsUs=0;bool gFguiRowProfileLog=false;

double richV2Num(JSContextRef ctx,JSObjectRef o,const char* k,double dflt){
    JSStringRef s=JSStringCreateWithUTF8CString(k);JSValueRef v=JSObjectGetProperty(ctx,o,s,nullptr);JSStringRelease(s);
    if(!v||JSValueIsUndefined(ctx,v)||JSValueIsNull(ctx,v))return dflt;return JSValueToNumber(ctx,v,nullptr);
}

// ---- native scenegraph / Spine host contract ----
JSValueRef cbSgAlloc(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    return JSValueMakeNumber(ctx, gSG.alloc());
}

// Reserve scene slots in one JSC crossing. JS consumes the returned ids lazily while an FGUI
// component tree is being constructed; local fields still go through the shared Float32 buffer.
JSValueRef cbSgAllocBatch(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<1||!JSValueIsObject(ctx,a[0])||JSValueGetTypedArrayType(ctx,a[0],e)!=kJSTypedArrayTypeFloat32Array)return JSValueMakeNumber(ctx,0);
    JSObjectRef out=JSValueToObject(ctx,a[0],e);float* ids=(float*)JSObjectGetTypedArrayBytesPtr(ctx,out,e);
    size_t cap=JSObjectGetTypedArrayLength(ctx,out,e),n=0;if(!ids)return JSValueMakeNumber(ctx,0);
    for(;n<cap;n++){int id=gSG.alloc();if(id<0)break;ids[n]=(float)id;}
    return JSValueMakeNumber(ctx,(double)n);
}

uint64_t gNativeFguiManagedFreeDeferrals = 0;

// Int32 command stream, repeated [op,a,b,c]: 1=setParent, 2=remove, 3=free.
JSValueRef cbSgCommands(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<2||!JSValueIsObject(ctx,a[0])||JSValueGetTypedArrayType(ctx,a[0],e)!=kJSTypedArrayTypeInt32Array)return JSValueMakeNumber(ctx,0);
    JSObjectRef in=JSValueToObject(ctx,a[0],e);const int32_t* cmd=(const int32_t*)JSObjectGetTypedArrayBytesPtr(ctx,in,e);
    size_t cap=JSObjectGetTypedArrayLength(ctx,in,e),words=(size_t)std::max(0.0,JSValueToNumber(ctx,a[1],e));words=std::min(words,cap);if(!cmd)return JSValueMakeNumber(ctx,0);
    size_t done=0;for(size_t i=0;i+3<words;i+=4){int op=cmd[i],x=cmd[i+1];
        if(op==1){gSG.setParent(x,cmd[i+2],cmd[i+3]);gNativeFgui.refreshManagedDisplayVisibility(x,gSG);}
        else if(op==2){gSG.removeFromParent(x);gNativeFgui.refreshManagedDisplayVisibility(x,gSG);}
        else if(op==3)freeSceneNodeFromJs(x);
        else continue;done++;
    }
    return JSValueMakeNumber(ctx,(double)done);
}

// Generation-checked command stream. Each row is
// [op,child,parent,before,childGen,parentGen,beforeGen,reserved]. A negative receipt stops the
// batch; untouched rows remain zero so JS can retain/reconcile them explicitly.
JSValueRef cbSgCommandsV2(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<3||!JSValueIsObject(ctx,a[0])||!JSValueIsObject(ctx,a[2])||
       JSValueGetTypedArrayType(ctx,a[0],e)!=kJSTypedArrayTypeInt32Array||
       JSValueGetTypedArrayType(ctx,a[2],e)!=kJSTypedArrayTypeInt32Array)return JSValueMakeNumber(ctx,0);
    JSObjectRef in=JSValueToObject(ctx,a[0],e),out=JSValueToObject(ctx,a[2],e);
    const int32_t* cmd=(const int32_t*)JSObjectGetTypedArrayBytesPtr(ctx,in,e);
    int32_t* receipts=(int32_t*)JSObjectGetTypedArrayBytesPtr(ctx,out,e);
    const size_t cap=JSObjectGetTypedArrayLength(ctx,in,e),receiptCap=JSObjectGetTypedArrayLength(ctx,out,e);
    const double requested=JSValueToNumber(ctx,a[1],e);
    if(!cmd||!receipts||!std::isfinite(requested)||requested<0.0||std::trunc(requested)!=requested||requested>(double)cap)
        return JSValueMakeNumber(ctx,0);
    const size_t words=(size_t)requested;if((words&7u)!=0u||receiptCap<words/8u)return JSValueMakeNumber(ctx,0);
    const size_t inspected=sg::executeCommandsV2(gSG,cmd,words,receipts,receiptCap,
        [](int child,uint32_t gen){return freeSceneNodeFromJs(child,gen);},
        [](int child){gNativeFgui.refreshManagedDisplayVisibility(child,gSG);});
    return JSValueMakeNumber(ctx,(double)inspected);
}

JSValueRef cbSgFree(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 1) return JSValueMakeNumber(ctx,(int32_t)sg::CommandReceipt::InvalidOrDead);
    const int child=(int)JSValueToNumber(ctx,a[0],e);
    const uint32_t childGen=argc>1?(uint32_t)JSValueToNumber(ctx,a[1],e):0u;
    const sg::CommandReceipt receipt=(argc>1&&childGen==0u)
        ?sg::CommandReceipt::GenerationMismatch:freeSceneNodeFromJs(child,childGen);
    return JSValueMakeNumber(ctx,(int32_t)receipt);
}

JSValueRef cbSgSetParent(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 2) return JSValueMakeNumber(ctx,(int32_t)sg::CommandReceipt::InvalidOrDead);
    const int child=(int)JSValueToNumber(ctx,a[0],e),parent=(int)JSValueToNumber(ctx,a[1],e);
    const int before=argc>2?(int)JSValueToNumber(ctx,a[2],e):-1;
    const uint32_t childGen=argc>3?(uint32_t)JSValueToNumber(ctx,a[3],e):0u;
    const uint32_t parentGen=argc>4?(uint32_t)JSValueToNumber(ctx,a[4],e):0u;
    const uint32_t beforeGen=argc>5?(uint32_t)JSValueToNumber(ctx,a[5],e):0u;
    const bool checked=argc>3;
    const bool gensPresent=childGen!=0u&&(parent==0?parentGen==0u:parentGen!=0u)&&
                           (before<0?beforeGen==0u:beforeGen!=0u);
    const sg::CommandReceipt receipt=checked
        ?(gensPresent?gSG.setParentChecked(child,parent,before,childGen,parentGen,beforeGen)
                    :sg::CommandReceipt::GenerationMismatch)
        :gSG.setParentLegacyReceipt(child,parent,before);
    if(sg::commandReceiptSucceeded(receipt))gNativeFgui.refreshManagedDisplayVisibility(child,gSG);
    return JSValueMakeNumber(ctx,(int32_t)receipt);
}

JSValueRef cbSgRemove(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 1) return JSValueMakeNumber(ctx,(int32_t)sg::CommandReceipt::InvalidOrDead);
    const int child=(int)JSValueToNumber(ctx,a[0],e);
    const uint32_t childGen=argc>1?(uint32_t)JSValueToNumber(ctx,a[1],e):0u;
    const sg::CommandReceipt receipt=(argc>1&&childGen==0u)
        ?sg::CommandReceipt::GenerationMismatch:gSG.removeFromParent(child,childGen);
    if(sg::commandReceiptSucceeded(receipt))gNativeFgui.refreshManagedDisplayVisibility(child,gSG);
    return JSValueMakeNumber(ctx,(int32_t)receipt);
}

// Read-only hierarchy access for strict coverage/debug tooling and lazy Native handle discovery.
// Rendering and hierarchy storage stay entirely in SceneGraph; JavaScript only requests an
// on-demand facade view when business/event compatibility code explicitly reads `children`.
JSValueRef cbSgNativeParent(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if(argc<1)return JSValueMakeNumber(ctx,-1);int id=(int)JSValueToNumber(ctx,a[0],e);
    return JSValueMakeNumber(ctx,(id>=0&&id<sg::NODE_CAP&&gSG.nodes[id].alive)?gSG.nodes[id].parent:-1);
}

JSValueRef cbSgNativeChildCount(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if(argc<1)return JSValueMakeNumber(ctx,0);return JSValueMakeNumber(ctx,gSG.childCount((int)JSValueToNumber(ctx,a[0],e)));
}

JSValueRef cbSgNativeChildAt(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if(argc<2)return JSValueMakeNumber(ctx,-1);return JSValueMakeNumber(ctx,gSG.childAt((int)JSValueToNumber(ctx,a[0],e),(int)JSValueToNumber(ctx,a[1],e)));
}

JSValueRef cbSgNativeChildren(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    std::vector<JSValueRef> out;int id=argc>0?(int)JSValueToNumber(ctx,a[0],e):-1;
    if(id>0&&id<sg::NODE_CAP&&gSG.nodes[id].alive){const auto& children=gSG.nodes[id].children;out.reserve(children.size());
        for(int child:children)if(child>0&&child<sg::NODE_CAP&&gSG.nodes[child].alive)out.push_back(JSValueMakeNumber(ctx,child));
    }
    return JSObjectMakeArray(ctx,out.size(),out.empty()?nullptr:out.data(),e);
}

// Return one authoritative native subtree in a single JSC crossing for diagnostics and
// event-driven hierarchy repair, without recursively walking JavaScript facade views.
JSValueRef cbSgNativeSubtree(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    std::vector<int> stack, ids;int root=argc>0?(int)JSValueToNumber(ctx,a[0],e):-1;
    if(root>0&&root<sg::NODE_CAP&&gSG.nodes[root].alive)stack.push_back(root);
    while(!stack.empty()){
        int id=stack.back();stack.pop_back();ids.push_back(id);const auto& children=gSG.nodes[id].children;
        for(auto it=children.rbegin();it!=children.rend();++it)if(*it>0&&*it<sg::NODE_CAP&&gSG.nodes[*it].alive)stack.push_back(*it);
    }
    std::vector<JSValueRef> out;out.reserve(ids.size());for(int id:ids)out.push_back(JSValueMakeNumber(ctx,id));
    return JSObjectMakeArray(ctx,out.size(),out.empty()?nullptr:out.data(),e);
}

JSValueRef cbSgAdvanceAnimations(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc > 0) advanceNativeAnimationsForFrame((int)JSValueToNumber(ctx, a[0], e));
    return JSValueMakeUndefined(ctx);
}

JSValueRef cbSgDebtRollup(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc > 0) gSG.rollupDebt((int)JSValueToNumber(ctx, a[0], e));
    return JSValueMakeUndefined(ctx);
}

JSValueRef cbSgCompute(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc > 0) {
        // 四审 P2:隐式 mode 后门废止——JS 必须显式声明(扫描契约 native_compute_mode_contract_scan
        // 保证调用面;此处兜底拒绝执行,静默缺省会让契约在运行时被绕过)。
        if (argc < 2) { static int warned = 0; if (warned++ < 4)
            HOST_LOG("[sg-compute] 隐式 mode 调用被拒绝(必须显式声明 compute 模式)\n"); return JSValueMakeUndefined(ctx); }
        // 八审:isfinite→范围→trunc 顺序契约(sg::validComputeArgs)——七审版整数性检查里的
        // (int) 越界转换(1e100)先于范围校验仍是 UB,已废止。
        double rootD = JSValueToNumber(ctx, a[0], e), modeD = JSValueToNumber(ctx, a[1], e);
        bool ok = sg::validComputeArgs(rootD, modeD);
        if (!ok) { static int badm = 0; if (badm++ < 4)
            HOST_LOG("[sg-compute] 非法参数被拒绝 root=%f mode=%f(NaN/超界/非整数)\n", rootD, modeD); return JSValueMakeUndefined(ctx); }
        gSG.compute((int)rootD, (int)modeD);
    }
    return JSValueMakeUndefined(ctx);
}

JSValueRef cbSgPublishSeal(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc > 0) gSG.publishSeal((int)JSValueToNumber(ctx, a[0], e));   // P2-4:present seal 显式发布
    return JSValueMakeUndefined(ctx);
}

JSValueRef cbSgAuthorityDump(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc > 0) gSG.authorityDump((int)JSValueToNumber(ctx, a[0], e));   // Phase F-6:单节点权威状态一行应答
    return JSValueMakeUndefined(ctx);
}

JSValueRef cbSgChainWorld(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc > 0) gSG.computeChainWorld((int)JSValueToNumber(ctx, a[0], e));   // Phase F-4:live read barrier(父链惰性)
    return JSValueMakeUndefined(ctx);
}

JSValueRef cbSgUpdateBounds(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc > 0) gSG.updateBoundsFrom((int)JSValueToNumber(ctx, a[0], e),
        argc > 1 ? (JSValueToNumber(ctx, a[1], e) != 0.0) : true);   // P2-D:skipUpdate 语义透传
    return JSValueMakeUndefined(ctx);
}

JSValueRef cbSgGetLocalBounds(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    float bounds[4];
    if(argc<1 || !gSG.getLocalBounds((int)JSValueToNumber(ctx,a[0],e),bounds))return JSValueMakeNull(ctx);
    JSValueRef values[4];
    for(int i=0;i<4;i++)values[i]=JSValueMakeNumber(ctx,bounds[i]);
    return JSObjectMakeArray(ctx,4,values,e);
}

JSValueRef cbSgHit(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 3) return JSValueMakeNumber(ctx, 0);
    int n = gSG.hitTest((int)JSValueToNumber(ctx,a[0],e), (float)JSValueToNumber(ctx,a[1],e),
                        (float)JSValueToNumber(ctx,a[2],e), argc>3 && JSValueToBoolean(ctx,a[3]));
    return JSValueMakeNumber(ctx,n);
}

// __nativeNineSlice(node,tex,W,H,lw,rw,top,bottom,texW,texH,flipX,flipY,u0,v0...u3,v3,tint,blend,premul)
JSValueRef cbNativeNineSlice(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<23)return JSValueMakeBoolean(ctx,false);int node=(int)JSValueToNumber(ctx,a[0],e),tex=(int)JSValueToNumber(ctx,a[1],e);
    if(node<=0||node>=sg::NODE_CAP||!gSG.nodes[node].alive)return JSValueMakeBoolean(ctx,false);
    float W=JSValueToNumber(ctx,a[2],e),H=JSValueToNumber(ctx,a[3],e),lw=JSValueToNumber(ctx,a[4],e),rw=JSValueToNumber(ctx,a[5],e);
    float th=JSValueToNumber(ctx,a[6],e),bh=JSValueToNumber(ctx,a[7],e),tw=std::max(1.0,(double)JSValueToNumber(ctx,a[8],e)),tall=std::max(1.0,(double)JSValueToNumber(ctx,a[9],e));
    bool fx=JSValueToBoolean(ctx,a[10]),fy=JSValueToBoolean(ctx,a[11]);float uv[8];for(int i=0;i<8;i++)uv[i]=JSValueToNumber(ctx,a[12+i],e);
    float tint=JSValueToNumber(ctx,a[20],e),blend=JSValueToNumber(ctx,a[21],e),premul=JSValueToNumber(ctx,a[22],e);
    if(!(W>0&&H>0&&tex>0)){gSG.clearNativeQuads(node);if(node>0)gSGBuf[node*sg::STRIDE+sg::TYPE]=sg::T_CONTAINER;return JSValueMakeBoolean(ctx,true);}
    float sw=lw+rw,sh=th+bh,msc=std::min((W>sw||sw<=0)?1.0f:W/sw,(H>sh||sh<=0)?1.0f:H/sh);
    float xs[4]={0,(fx?rw:lw)*msc,W-(fx?lw:rw)*msc,W},ys[4]={0,(fy?bh:th)*msc,H-(fy?th:bh)*msc,H};
    float xr[4]={0,lw/tw,1-rw/tw,1},yr[4]={0,th/tall,1-bh/tall,1};if(fx){xr[0]=1;xr[1]=1-rw/tw;xr[2]=lw/tw;xr[3]=0;}if(fy){yr[0]=1;yr[1]=1-bh/tall;yr[2]=th/tall;yr[3]=0;}
    auto uvAt=[&](float x,float y,float& u,float& v){float ax=1-x,ay=1-y;u=uv[0]*ax*ay+uv[2]*x*ay+uv[4]*x*y+uv[6]*ax*y;v=uv[1]*ax*ay+uv[3]*x*ay+uv[5]*x*y+uv[7]*ax*y;};
    std::vector<sg::NativeQuad> qs;qs.reserve(9);
    for(int r=0;r<3;r++)for(int c=0;c<3;c++){sg::NativeQuad q{};q[0]=tex;q[1]=xs[c];q[2]=ys[r];q[3]=xs[c+1];q[4]=ys[r];q[5]=xs[c+1];q[6]=ys[r+1];q[7]=xs[c];q[8]=ys[r+1];
        uvAt(xr[c],yr[r],q[9],q[10]);uvAt(xr[c+1],yr[r],q[11],q[12]);uvAt(xr[c+1],yr[r+1],q[13],q[14]);uvAt(xr[c],yr[r+1],q[15],q[16]);
        q[17]=tint;q[18]=1;q[19]=-1;q[20]=-1;q[21]=0;q[22]=premul;qs.push_back(q);}
    gSG.setNativeQuads(node,std::move(qs));float* s=gSGBuf+node*sg::STRIDE;s[sg::BLEND]=blend;s[sg::PREMUL]=premul;s[sg::RENDERABLE]=1;return JSValueMakeBoolean(ctx,true);
}

// __nativeTiling(node,tex,W,H,tileW,tileH,posX,posY,anchorX,anchorY,
//                u0,v0..u3,v3,tint,blend,premul)
// The host owns the generated quads. JS only keeps the texture and the small tile-transform handle.
JSValueRef cbNativeTiling(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<21)return JSValueMakeBoolean(ctx,false);
    int node=(int)JSValueToNumber(ctx,a[0],e),tex=(int)JSValueToNumber(ctx,a[1],e);
    if(node<=0||node>=sg::NODE_CAP||!gSG.nodes[node].alive)return JSValueMakeBoolean(ctx,false);
    float W=(float)JSValueToNumber(ctx,a[2],e),H=(float)JSValueToNumber(ctx,a[3],e);
    float tileW=(float)JSValueToNumber(ctx,a[4],e),tileH=(float)JSValueToNumber(ctx,a[5],e);
    float px=(float)JSValueToNumber(ctx,a[6],e),py=(float)JSValueToNumber(ctx,a[7],e);
    float anchorX=(float)JSValueToNumber(ctx,a[8],e),anchorY=(float)JSValueToNumber(ctx,a[9],e);
    float uv[8];for(int i=0;i<8;i++)uv[i]=(float)JSValueToNumber(ctx,a[10+i],e);
    float tint=(float)JSValueToNumber(ctx,a[18],e),blend=(float)JSValueToNumber(ctx,a[19],e),premul=(float)JSValueToNumber(ctx,a[20],e);
    if(!(W>0&&H>0&&tileW>0.5f&&tileH>0.5f&&tex>0)){gSG.clearNativeQuads(node);gSGBuf[node*sg::STRIDE+sg::TYPE]=sg::T_CONTAINER;return JSValueMakeBoolean(ctx,true);}
    int nx=(int)ceilf(W/tileW)+2,ny=(int)ceilf(H/tileH)+2;
    if(nx<=0||ny<=0||(size_t)nx*(size_t)ny>65536)return JSValueMakeBoolean(ctx,false);
    auto uvAt=[&](float x,float y,float& u,float& v){float ax=1.0f-x,ay=1.0f-y;u=uv[0]*ax*ay+uv[2]*x*ay+uv[4]*x*y+uv[6]*ax*y;v=uv[1]*ax*ay+uv[3]*x*ay+uv[5]*x*y+uv[7]*ax*y;};
    float originX=-anchorX*W,originY=-anchorY*H;
    float sx=fmodf(px,tileW);if(sx<0)sx+=tileW;if(sx>0.001f)sx-=tileW;
    float sy=fmodf(py,tileH);if(sy<0)sy+=tileH;if(sy>0.001f)sy-=tileH;
    std::vector<sg::NativeQuad> qs;qs.reserve((size_t)nx*(size_t)ny);
    // Integer tile indices, not float accumulation: when |origin| is large enough that
    // y+=tileH no longer changes y (float ulp > tileH), the accumulating loop never ends.
    for(int iy=0;iy<ny;iy++){float y=originY+sy+iy*tileH;if(!(y<originY+H))break;
    for(int ix=0;ix<nx;ix++){float x=originX+sx+ix*tileW;if(!(x<originX+W))break;
        float x0=std::max(x,originX),y0=std::max(y,originY),x1=std::min(x+tileW,originX+W),y1=std::min(y+tileH,originY+H);if(x1<=x0||y1<=y0)continue;
        sg::NativeQuad q{};q[0]=(float)tex;q[1]=x0;q[2]=y0;q[3]=x1;q[4]=y0;q[5]=x1;q[6]=y1;q[7]=x0;q[8]=y1;
        uvAt((x0-x)/tileW,(y0-y)/tileH,q[9],q[10]);uvAt((x1-x)/tileW,(y0-y)/tileH,q[11],q[12]);
        uvAt((x1-x)/tileW,(y1-y)/tileH,q[13],q[14]);uvAt((x0-x)/tileW,(y1-y)/tileH,q[15],q[16]);
        q[17]=tint;q[18]=1.0f;q[19]=-1.0f;q[20]=-1.0f;q[21]=0.0f;q[22]=premul;qs.push_back(q);
    }}
    gSG.setNativeQuads(node,std::move(qs));float* s=gSGBuf+node*sg::STRIDE;s[sg::BLEND]=blend;s[sg::PREMUL]=premul;s[sg::RENDERABLE]=1;return JSValueMakeBoolean(ctx,true);
}

// __nativeBitmapGlyphs(node, recordsF32, blend, distanceFieldBaseWidth)
// record(16 floats): texture,x0,y0,x1,y1,u0,v0,u1,v1,u2,v2,u3,v3,tint,alpha,premul.
JSValueRef cbNativeBitmapGlyphs(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<3||!JSValueIsObject(ctx,a[1])||JSValueGetTypedArrayType(ctx,a[1],e)!=kJSTypedArrayTypeFloat32Array)return JSValueMakeBoolean(ctx,false);
    int node=(int)JSValueToNumber(ctx,a[0],e);if(node<=0||node>=sg::NODE_CAP||!gSG.nodes[node].alive)return JSValueMakeBoolean(ctx,false);
    JSObjectRef obj=JSValueToObject(ctx,a[1],e);const float* p=(const float*)JSObjectGetTypedArrayBytesPtr(ctx,obj,e);size_t n=JSObjectGetTypedArrayLength(ctx,obj,e);
    // V8 may use a null backing-store pointer for a valid empty Float32Array. Empty glyphs
    // must reach setNativeQuads so clearing/reusing a BitmapText removes its previous geometry.
    if((n&&!p)||n%16||n/16>1000000)return JSValueMakeBoolean(ctx,false);std::vector<sg::NativeQuad> qs;qs.reserve(n/16);
    float distanceFieldWidth=argc>3?(float)JSValueToNumber(ctx,a[3],e):0.0f;
    for(size_t o=0;o<n;o+=16){int tex=(int)p[o];if(tex<=0)continue;sg::NativeQuad q{};q[0]=(float)tex;q[1]=p[o+1];q[2]=p[o+2];q[3]=p[o+3];q[4]=p[o+2];q[5]=p[o+3];q[6]=p[o+4];q[7]=p[o+1];q[8]=p[o+4];
        for(int k=0;k<8;k++)q[9+k]=p[o+5+k];q[17]=p[o+13];q[18]=p[o+14];q[19]=-1.0f;q[20]=distanceFieldWidth>0?-2.0f:-1.0f;q[21]=distanceFieldWidth;q[22]=p[o+15];qs.push_back(q);}
    gSG.setNativeQuads(node,std::move(qs));float* s=gSGBuf+node*sg::STRIDE;s[sg::BLEND]=(float)JSValueToNumber(ctx,a[2],e);s[sg::RENDERABLE]=1;return JSValueMakeBoolean(ctx,true);
}

// __nativeGraphicsMesh(node,pointsF32,indicesU32,uvsF32,batchesF32,blend,asMask)
// batches: repeated [textureId,indexStart,indexCount,tint,alpha,premul,useUV]. Pixi keeps the
// high-level Graphics API, while final triangles and their lifetime live exclusively in SceneGraph.
JSValueRef cbNativeGraphicsMesh(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<7)return JSValueMakeBoolean(ctx,false);
    int node=(int)JSValueToNumber(ctx,a[0],e);
    if(node<=0||node>=sg::NODE_CAP||!gSG.nodes[node].alive)return JSValueMakeBoolean(ctx,false);
    auto obj=[&](int i)->JSObjectRef{return JSValueIsObject(ctx,a[i])?JSValueToObject(ctx,a[i],e):nullptr;};
    JSObjectRef po=obj(1),io=obj(2),uo=obj(3),mo=obj(4);
    if(!po||!io||!uo||!mo)return JSValueMakeBoolean(ctx,false);
    if(JSValueGetTypedArrayType(ctx,a[1],e)!=kJSTypedArrayTypeFloat32Array||
       JSValueGetTypedArrayType(ctx,a[2],e)!=kJSTypedArrayTypeUint32Array||
       JSValueGetTypedArrayType(ctx,a[3],e)!=kJSTypedArrayTypeFloat32Array||
       JSValueGetTypedArrayType(ctx,a[4],e)!=kJSTypedArrayTypeFloat32Array)return JSValueMakeBoolean(ctx,false);
    const float* points=(const float*)JSObjectGetTypedArrayBytesPtr(ctx,po,e);
    const uint32_t* indices=(const uint32_t*)JSObjectGetTypedArrayBytesPtr(ctx,io,e);
    const float* uvs=(const float*)JSObjectGetTypedArrayBytesPtr(ctx,uo,e);
    const float* meta=(const float*)JSObjectGetTypedArrayBytesPtr(ctx,mo,e);
    size_t pointN=JSObjectGetTypedArrayLength(ctx,po,e),indexN=JSObjectGetTypedArrayLength(ctx,io,e);
    size_t uvN=JSObjectGetTypedArrayLength(ctx,uo,e),metaN=JSObjectGetTypedArrayLength(ctx,mo,e);
    if(!points||!indices||!uvs||!meta||(pointN&1)||metaN%7)return JSValueMakeBoolean(ctx,false);
    size_t reserve=0;
    for(size_t m=0;m<metaN;m+=7){size_t count=(size_t)std::max(0.0f,meta[m+2]);reserve+=count/3;}
    if(reserve>1000000)return JSValueMakeBoolean(ctx,false);
    std::vector<sg::NativeQuad> qs;qs.reserve(reserve);
    for(size_t m=0;m<metaN;m+=7){
        int tex=(int)meta[m],start=(int)meta[m+1],count=(int)meta[m+2];
        float tint=meta[m+3],alpha=meta[m+4],premul=meta[m+5];bool useUV=meta[m+6]>0.5f;
        if(tex<=0||start<0||count<3||(size_t)start>=indexN)continue;
        size_t end=std::min(indexN,(size_t)start+(size_t)count);
        for(size_t k=(size_t)start;k+2<end;k+=3){
            size_t i0=(size_t)indices[k]*2,i1=(size_t)indices[k+1]*2,i2=(size_t)indices[k+2]*2;
            if(i0+1>=pointN||i1+1>=pointN||i2+1>=pointN)continue;
            if(useUV&&(i0+1>=uvN||i1+1>=uvN||i2+1>=uvN))continue;
            sg::NativeQuad q{};q[0]=tex;
            q[1]=points[i0];q[2]=points[i0+1];q[3]=points[i1];q[4]=points[i1+1];
            q[5]=points[i2];q[6]=points[i2+1];q[7]=points[i2];q[8]=points[i2+1];
            if(useUV){q[9]=uvs[i0];q[10]=uvs[i0+1];q[11]=uvs[i1];q[12]=uvs[i1+1];q[13]=uvs[i2];q[14]=uvs[i2+1];q[15]=uvs[i2];q[16]=uvs[i2+1];}
            else{q[9]=0;q[10]=0;q[11]=1;q[12]=0;q[13]=1;q[14]=1;q[15]=1;q[16]=1;}
            q[17]=tint;q[18]=alpha;q[19]=-1;q[20]=-1;q[21]=0;q[22]=premul;qs.push_back(q);
        }
    }
    bool asMask=JSValueToBoolean(ctx,a[6]);gSG.setNativeQuads(node,std::move(qs));
    float* s=gSGBuf+node*sg::STRIDE;s[sg::BLEND]=(float)JSValueToNumber(ctx,a[5],e);s[sg::PREMUL]=0;s[sg::RENDERABLE]=1;
    if(asMask)s[sg::TYPE]=sg::T_CONTAINER;
    return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbNativeTweenStart(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<11)return JSValueMakeBoolean(ctx,false);
    NativeTween t;t.id=(int)JSValueToNumber(ctx,a[0],e);t.node=(int)JSValueToNumber(ctx,a[1],e);t.slots[0]=(int)JSValueToNumber(ctx,a[2],e);t.slots[1]=(int)JSValueToNumber(ctx,a[3],e);
    t.from[0]=JSValueToNumber(ctx,a[4],e);t.from[1]=JSValueToNumber(ctx,a[5],e);t.to[0]=JSValueToNumber(ctx,a[6],e);t.to[1]=JSValueToNumber(ctx,a[7],e);t.valueCount=t.slots[1]>=0?2:1;
    t.durationMs=std::max(0.1,(double)JSValueToNumber(ctx,a[8],e));t.startMs=JSValueToNumber(ctx,a[9],e);
    if(t.id<=0||t.node<=0||t.node>=sg::NODE_CAP||!gSG.nodes[t.node].alive)return JSValueMakeBoolean(ctx,false);
    t.nodeGen=gSG.generation(t.node);
    auto validSlot=[](int s){return s==sg::X||s==sg::Y||s==sg::SX||s==sg::SY||s==sg::ROT||s==sg::SKX||s==sg::SKY||s==sg::ALPHA;};
    if(!validSlot(t.slots[0])||(t.slots[1]>=0&&!validSlot(t.slots[1])))return JSValueMakeBoolean(ctx,false);
    if(JSValueIsObject(ctx,a[10])&&JSValueGetTypedArrayType(ctx,a[10],e)==kJSTypedArrayTypeFloat32Array){JSObjectRef o=JSValueToObject(ctx,a[10],e);const float* p=(const float*)JSObjectGetTypedArrayBytesPtr(ctx,o,e);size_t n=JSObjectGetTypedArrayLength(ctx,o,e);if(p&&n>=2)t.curve.assign(p,p+n);}
    if(t.curve.size()<2)t.curve={0,1};gNativeTweens[t.id]=std::move(t);return JSValueMakeBoolean(ctx,true);
}

// Full native FGUI timeline track. Numeric interpolation, delay/repeat/yoyo and shake live in C++.
// JS receives semantic values in a compact result stream only for properties whose FGUI model must
// stay observable (size/color/controller etc). Transform-only callers may provide slots and disable
// emitValues so C++ writes the shared scene node directly.
// __nativeTweenStartEx(id,node,slotsI32,fromF32,toF32,duration,start,delay,repeat,yoyo,emit,mode,curveF32)
JSValueRef cbNativeTweenStartEx(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<13)return JSValueMakeBoolean(ctx,false);
    NativeTween t;t.id=(int)JSValueToNumber(ctx,a[0],e);t.node=(int)JSValueToNumber(ctx,a[1],e);
    auto readF32=[&](int ai,float* dst,int maxN)->int{
        if(!JSValueIsObject(ctx,a[ai])||JSValueGetTypedArrayType(ctx,a[ai],e)!=kJSTypedArrayTypeFloat32Array)return 0;
        JSObjectRef o=JSValueToObject(ctx,a[ai],e);const float* p=(const float*)JSObjectGetTypedArrayBytesPtr(ctx,o,e);size_t n=JSObjectGetTypedArrayLength(ctx,o,e);if(!p)return 0;int take=(int)std::min(n,(size_t)maxN);for(int i=0;i<take;i++)dst[i]=p[i];return take;
    };
    if(JSValueIsObject(ctx,a[2])&&JSValueGetTypedArrayType(ctx,a[2],e)==kJSTypedArrayTypeInt32Array){
        JSObjectRef o=JSValueToObject(ctx,a[2],e);const int32_t* p=(const int32_t*)JSObjectGetTypedArrayBytesPtr(ctx,o,e);size_t n=JSObjectGetTypedArrayLength(ctx,o,e);if(p)for(size_t i=0;i<std::min(n,(size_t)4);i++)t.slots[i]=p[i];
    }
    int fn=readF32(3,t.from,4),tn=readF32(4,t.to,4);t.valueCount=std::max(fn,tn);
    t.durationMs=std::max(0.1,(double)JSValueToNumber(ctx,a[5],e));t.startMs=JSValueToNumber(ctx,a[6],e);t.delayMs=std::max(0.0,(double)JSValueToNumber(ctx,a[7],e));
    t.repeat=(int)JSValueToNumber(ctx,a[8],e);t.yoyo=JSValueToBoolean(ctx,a[9]);t.emitValues=JSValueToBoolean(ctx,a[10]);t.mode=(int)JSValueToNumber(ctx,a[11],e);
    if(t.id<=0||t.valueCount<1||t.valueCount>4)return JSValueMakeBoolean(ctx,false);
    auto validSlot=[](int s){return s==sg::X||s==sg::Y||s==sg::SX||s==sg::SY||s==sg::ROT||s==sg::SKX||s==sg::SKY||s==sg::ALPHA;};
    bool hasSlot=false;for(int i=0;i<t.valueCount;i++){if(t.slots[i]>=0){if(!validSlot(t.slots[i]))return JSValueMakeBoolean(ctx,false);hasSlot=true;}}
    if(hasSlot&&(t.node<=0||t.node>=sg::NODE_CAP||!gSG.nodes[t.node].alive))return JSValueMakeBoolean(ctx,false);
    if(hasSlot)t.nodeGen=gSG.generation(t.node);
    if(JSValueIsObject(ctx,a[12])&&JSValueGetTypedArrayType(ctx,a[12],e)==kJSTypedArrayTypeFloat32Array){JSObjectRef o=JSValueToObject(ctx,a[12],e);const float* p=(const float*)JSObjectGetTypedArrayBytesPtr(ctx,o,e);size_t n=JSObjectGetTypedArrayLength(ctx,o,e);if(p&&n>=2)t.curve.assign(p,p+n);}
    if(t.curve.size()<2)t.curve={0,1};t.randomState=(uint32_t)t.id*747796405u+2891336453u;gNativeTweens[t.id]=std::move(t);return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbNativeTweenCancel(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc>0)gNativeTweens.erase((int)JSValueToNumber(ctx,a[0],e));return JSValueMakeUndefined(ctx);
}

JSValueRef cbNativeTweenPause(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<3)return JSValueMakeBoolean(ctx,false);auto it=gNativeTweens.find((int)JSValueToNumber(ctx,a[0],e));if(it==gNativeTweens.end())return JSValueMakeBoolean(ctx,false);NativeTween& t=it->second;bool pause=JSValueToBoolean(ctx,a[1]);float now=(float)JSValueToNumber(ctx,a[2],e);
    if(pause&&!t.paused){t.paused=true;t.pausedAtMs=now;}else if(!pause&&t.paused){t.startMs+=std::max(0.0f,now-t.pausedAtMs);t.paused=false;t.pausedAtMs=0;}return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbNativeTweenAdvance(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<2||!JSValueIsObject(ctx,a[1])||JSValueGetTypedArrayType(ctx,a[1],e)!=kJSTypedArrayTypeFloat32Array)return JSValueMakeNumber(ctx,0);
    float now=JSValueToNumber(ctx,a[0],e);JSObjectRef oo=JSValueToObject(ctx,a[1],e);float* out=(float*)JSObjectGetTypedArrayBytesPtr(ctx,oo,e);size_t cap=JSObjectGetTypedArrayLength(ctx,oo,e),done=0;
    if(!out)return JSValueMakeNumber(ctx,0);
    for(auto it=gNativeTweens.begin();it!=gNativeTweens.end();){NativeTween& t=it->second;
        if(t.paused){++it;continue;}
        if(t.node<=0||t.node>=sg::NODE_CAP||!gSG.nodes[t.node].alive||gSG.generation(t.node)!=t.nodeGen){it=gNativeTweens.erase(it);continue;}
        float p=std::max(0.0f,std::min(1.0f,(now-t.startMs)/t.durationMs));float fp=p*(t.curve.size()-1),eased;size_t ci=std::min(t.curve.size()-2,(size_t)fp);float cf=fp-ci;eased=t.curve[ci]+(t.curve[ci+1]-t.curve[ci])*cf;
        float* s=gSGBuf+t.node*sg::STRIDE;s[t.slots[0]]=t.from[0]+(t.to[0]-t.from[0])*eased;if(t.slots[1]>=0)s[t.slots[1]]=t.from[1]+(t.to[1]-t.from[1])*eased;gSG.markLocalInputChanged(t.node);
        if(p>=1.0f&&done<cap){out[done++]=(float)t.id;it=gNativeTweens.erase(it);}else ++it;
    }
    return JSValueMakeNumber(ctx,(double)done);
}

// Output records: [id,status,value0,value1,value2,value3], status bit0=complete, bit1=started.
JSValueRef cbNativeTweenAdvanceEx(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,const JSValueRef a[],JSValueRef* e){
    if(argc<2||!JSValueIsObject(ctx,a[1])||JSValueGetTypedArrayType(ctx,a[1],e)!=kJSTypedArrayTypeFloat32Array)return JSValueMakeNumber(ctx,0);
    float now=JSValueToNumber(ctx,a[0],e);JSObjectRef oo=JSValueToObject(ctx,a[1],e);float* out=(float*)JSObjectGetTypedArrayBytesPtr(ctx,oo,e);size_t floats=JSObjectGetTypedArrayLength(ctx,oo,e),cap=floats/6,records=0;
    if(!out)return JSValueMakeNumber(ctx,0);
    for(auto it=gNativeTweens.begin();it!=gNativeTweens.end();){NativeTween& t=it->second;
        if(t.paused){++it;continue;}
        bool hasSlot=false;for(int i=0;i<t.valueCount;i++)if(t.slots[i]>=0)hasSlot=true;
        if(hasSlot&&(t.node<=0||t.node>=sg::NODE_CAP||!gSG.nodes[t.node].alive||gSG.generation(t.node)!=t.nodeGen)){it=gNativeTweens.erase(it);continue;}
        float elapsed=now-t.startMs-t.delayMs;if(elapsed<0){++it;continue;}
        bool justStarted=!t.started;t.started=true;int totalCycles=t.repeat<0?0x7fffffff:t.repeat+1;
        int cycle=(int)floorf(elapsed/t.durationMs);bool complete=cycle>=totalCycles;float p;
        if(complete){cycle=std::max(0,totalCycles-1);p=1.0f;}else p=(elapsed-cycle*t.durationMs)/t.durationMs;
        bool reverse=t.yoyo&&((cycle&1)!=0);if(reverse)p=1.0f-p;
        float values[4]={0,0,0,0};
        if(t.mode==1){ // deterministic decaying shake; output is the current offset, not an absolute position.
            float life=complete?0.0f:std::max(0.0f,1.0f-(elapsed/(t.durationMs*totalCycles)));
            t.randomState^=t.randomState<<13;t.randomState^=t.randomState>>17;t.randomState^=t.randomState<<5;
            float rx=((float)(t.randomState&0xffff)/32767.5f-1.0f);t.randomState=t.randomState*1664525u+1013904223u;
            float ry=((float)(t.randomState&0xffff)/32767.5f-1.0f);values[0]=complete?0.0f:roundf(rx*t.from[0]*life);values[1]=complete?0.0f:roundf(ry*t.from[0]*life);
        }else if(t.mode==2){for(int i=0;i<t.valueCount;i++)values[i]=complete?t.to[i]:t.from[i];}
        else{float fp=std::max(0.0f,std::min(1.0f,p))*(t.curve.size()-1),eased;size_t ci=std::min(t.curve.size()-2,(size_t)fp);float cf=fp-ci;eased=t.curve[ci]+(t.curve[ci+1]-t.curve[ci])*cf;for(int i=0;i<t.valueCount;i++)values[i]=t.from[i]+(t.to[i]-t.from[i])*eased;}
        if(hasSlot){float* s=gSGBuf+t.node*sg::STRIDE;for(int i=0;i<t.valueCount;i++)if(t.slots[i]>=0)s[t.slots[i]]=values[i];gSG.markLocalInputChanged(t.node);}
        bool needRecord=t.emitValues||complete||justStarted;
        if(needRecord){if(records>=cap){++it;continue;}size_t o=records++*6;out[o]=(float)t.id;out[o+1]=(float)((complete?1:0)|(justStarted?2:0));for(int i=0;i<4;i++)out[o+2+i]=values[i];}
        if(complete){it=gNativeTweens.erase(it);}else ++it;
    }
    return JSValueMakeNumber(ctx,(double)records);
}

JSValueRef cbSgFlagRender(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc > 0) gPendingRoot = (int)JSValueToNumber(ctx, a[0], e);
    if (argc >= 5) for (int i = 0; i < 4; i++) gSgClear[i] = (float)JSValueToNumber(ctx, a[1 + i], e);
    return JSValueMakeUndefined(ctx);
}

// T_NATIVE_MULTIQUAD 精确命中:点(节点局部系)对 C++ 持有 quad 的三角形测试(与 JS 池版语义一致)。
JSValueRef cbSgNativeQuadHit(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if(argc<3)return JSValueMakeBoolean(ctx,false);
    int node=(int)JSValueToNumber(ctx,a[0],e);
    float px=(float)JSValueToNumber(ctx,a[1],e),py=(float)JSValueToNumber(ctx,a[2],e);
    if(node<=0||node>=sg::NODE_CAP||!gSG.nodes[node].alive)return JSValueMakeBoolean(ctx,false);
    auto inTri=[](float px,float py,float ax,float ay,float bx,float by,float cx,float cy){
        float area=(bx-ax)*(cy-ay)-(by-ay)*(cx-ax);
        if(fabsf(area)<1e-6f)return false;
        float ab=(px-bx)*(ay-by)-(ax-bx)*(py-by);
        float bc=(px-cx)*(by-cy)-(bx-cx)*(py-cy);
        float ca=(px-ax)*(cy-ay)-(cx-ax)*(py-ay);
        bool hasNeg=ab<-1e-4f||bc<-1e-4f||ca<-1e-4f;
        bool hasPos=ab>1e-4f||bc>1e-4f||ca>1e-4f;
        return !(hasNeg&&hasPos);
    };
    for(const auto& q:gSG.nodes[node].nativeQuads){
        if(q[0]<0.0f||q[18]<=1e-4f)continue;
        if(inTri(px,py,q[1],q[2],q[3],q[4],q[5],q[6])||inTri(px,py,q[1],q[2],q[5],q[6],q[7],q[8]))return JSValueMakeBoolean(ctx,true);
    }
    return JSValueMakeBoolean(ctx,false);
}

JSValueRef cbSyncProgram(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                                const JSValueRef args[], JSValueRef* exc) {
    uint32_t cid = argc>0 ? (uint32_t)JSValueToNumber(ctx, args[0], exc) : 0;
    GLuint prog = gDec.glProgram(cid);
    std::string j = "{"; GLint linkOk=0; if (prog) glGetProgramiv(prog, GL_LINK_STATUS, &linkOk);
    if (prog && !linkOk) { char log[2048]; glGetProgramInfoLog(prog,2048,nullptr,log); HOST_LOG("[game] link fail cid=%u: %s\n",cid,log); }
    j += "\"linkOk\":"; j += (linkOk?"true":"false"); j += ",";
    char nb[256]; GLint nu=0; if (prog) glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &nu);
    j += "\"uniforms\":[";
    for (GLint u=0; u<nu; ++u) { GLint sz=0; GLenum tp=0; GLsizei len=0; glGetActiveUniform(prog,u,256,&len,&sz,&tp,nb);
        if (u) j += ","; j += "{\"name\":\""; j += nb; j += "\",\"type\":"; j += std::to_string((unsigned)tp); j += ",\"size\":"; j += std::to_string(sz); j += "}"; }
    j += "],\"attributes\":["; GLint na=0; if (prog) glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, &na);
    for (GLint v=0; v<na; ++v) { GLint sz=0; GLenum tp=0; GLsizei len=0; glGetActiveAttrib(prog,v,256,&len,&sz,&tp,nb);
        GLint loc = glGetAttribLocation(prog, nb); if (v) j += ",";
        j += "{\"name\":\""; j += nb; j += "\",\"type\":"; j += std::to_string((unsigned)tp); j += ",\"size\":"; j += std::to_string(sz); j += ",\"location\":"; j += std::to_string(loc); j += "}"; }
    j += "]}";
    JSStringRef s = JSStringCreateWithUTF8CString(j.c_str());
    JSValueRef r = JSValueMakeString(ctx, s); JSStringRelease(s); return r;
}

JSValueRef cbNowUs(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
    return JSValueMakeNumber(ctx, nowUs());
}

JSValueRef cbLog(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                        const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeUndefined(ctx);
    JSStringRef s = JSValueToStringCopy(ctx, args[0], exc);
    size_t n = JSStringGetMaximumUTF8CStringSize(s); std::vector<char> b(n);
    JSStringGetUTF8CString(s, b.data(), n); HOST_LOG("[js] %s\n", b.data()); JSStringRelease(s);
    return JSValueMakeUndefined(ctx);
}

// __evalCode(codeStr, label)：全局执行一段 JS 文本（顶层 var 挂全局，同浏览器内联 <script>）。
// createjs preferXHR 模式拉到 JS 文本后注入内联脚本执行 → 走它。
JSValueRef cbEvalCode(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeUndefined(ctx);
    JSStringRef js = JSValueToStringCopy(ctx, args[0], exc);
    JSStringRef url = (argc > 1) ? JSValueToStringCopy(ctx, args[1], nullptr) : JSStringCreateWithUTF8CString("inline.js");
    JSEvaluateScript(gCtx, js, nullptr, url, 1, exc);   // *exc 设置后由调用方 JS 捕获
    JSStringRelease(js); JSStringRelease(url);
    return JSValueMakeUndefined(ctx);
}

void setJsProperty(JSContextRef ctx,JSObjectRef o,const char* key,JSValueRef v){JSStringRef k=JSStringCreateWithUTF8CString(key);JSObjectSetProperty(ctx,o,k,v,kJSPropertyAttributeNone,nullptr);JSStringRelease(k);}

JSValueRef fguiJsString(JSContextRef ctx,const std::string& value){
    JSStringRef s=JSStringCreateWithUTF8CString(value.c_str());JSValueRef out=JSValueMakeString(ctx,s);JSStringRelease(s);return out;
}

JSObjectRef fguiStringArray(JSContextRef ctx,const std::vector<std::string>& values,JSValueRef* exc){
    jscbridge::RootedValueVector js(ctx);js.reserve(values.size());for(const auto& value:values)js.push_back(fguiJsString(ctx,value));
    return JSObjectMakeArray(ctx,js.size(),js.empty()?nullptr:js.data(),exc);
}

JSValueRef nativeFguiResourceRow(JSContextRef ctx,const nfgui::Resource& r,JSValueRef* exc){
    JSValueRef values[13]={JSValueMakeNumber(ctx,r.packageItemType),fguiJsString(ctx,r.id),r.name.empty()?JSValueMakeNull(ctx):fguiJsString(ctx,r.name),r.file.empty()?JSValueMakeNull(ctx):fguiJsString(ctx,r.file),JSValueMakeNumber(ctx,r.width),JSValueMakeNumber(ctx,r.height),JSValueMakeBoolean(ctx,r.scaleByTile),JSValueMakeBoolean(ctx,r.hasScale9),JSValueMakeNumber(ctx,r.gridX),JSValueMakeNumber(ctx,r.gridY),JSValueMakeNumber(ctx,r.gridW),JSValueMakeNumber(ctx,r.gridH),JSValueMakeNumber(ctx,r.tiledSlices)};
    return JSObjectMakeArray(ctx,13,values,exc);
}

const char* fguiKindName(nfgui::ObjectKind kind){
    switch(kind){case nfgui::ObjectKind::Component:return "component";case nfgui::ObjectKind::Image:return "image";case nfgui::ObjectKind::Text:return "text";case nfgui::ObjectKind::Graph:return "graph";case nfgui::ObjectKind::Loader:return "loader";case nfgui::ObjectKind::List:return "list";case nfgui::ObjectKind::Group:return "group";case nfgui::ObjectKind::MovieClip:return "movieclip";case nfgui::ObjectKind::RichText:return "richtext";case nfgui::ObjectKind::InputText:return "inputtext";default:return "unknown";}
}

JSObjectRef fguiNodeToJs(JSContextRef ctx,const nfgui::InstanceNode* n,JSValueRef* exc){
    if(!n)return nullptr;JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    setJsNumberK(ctx,out,FGUI_JS_KEY("nativeId"),n->nativeId);setJsNumberK(ctx,out,FGUI_JS_KEY("parentNativeId"),n->parentNativeId);setJsNumberK(ctx,out,FGUI_JS_KEY("ownerComponentId"),n->ownerComponentId);
    // Components with native-owned overflow already have an authoritative content container and
    // clip node. Expose their numeric identities so the JS compatibility facade can adopt those
    // exact nodes instead of creating a second Pixi scrollRect/mask tree. This must stay in sync
    // with the iOS bridge: reel_view relies on the adopted content node for clipping symbols that
    // are appended dynamically after the FairyGUI component has been constructed.
    setJsNumberK(ctx,out,FGUI_JS_KEY("displayContainerId"),n->displayContainerId);setJsNumberK(ctx,out,FGUI_JS_KEY("clipMaskId"),n->clipMaskId);
    setJsNumberK(ctx,out,FGUI_JS_KEY("x"),n->x);setJsNumberK(ctx,out,FGUI_JS_KEY("y"),n->y);setJsNumberK(ctx,out,FGUI_JS_KEY("width"),n->width);setJsNumberK(ctx,out,FGUI_JS_KEY("height"),n->height);
    setJsNumberK(ctx,out,FGUI_JS_KEY("sourceWidth"),n->sourceWidth);setJsNumberK(ctx,out,FGUI_JS_KEY("sourceHeight"),n->sourceHeight);setJsNumberK(ctx,out,FGUI_JS_KEY("initWidth"),n->initWidth);setJsNumberK(ctx,out,FGUI_JS_KEY("initHeight"),n->initHeight);
    setJsNumberK(ctx,out,FGUI_JS_KEY("pivotX"),n->pivotX);setJsNumberK(ctx,out,FGUI_JS_KEY("pivotY"),n->pivotY);setJsNumberK(ctx,out,FGUI_JS_KEY("scaleX"),n->scaleX);setJsNumberK(ctx,out,FGUI_JS_KEY("scaleY"),n->scaleY);setJsNumberK(ctx,out,FGUI_JS_KEY("skewX"),n->skewX);setJsNumberK(ctx,out,FGUI_JS_KEY("skewY"),n->skewY);setJsNumberK(ctx,out,FGUI_JS_KEY("rotation"),n->rotation);setJsNumberK(ctx,out,FGUI_JS_KEY("alpha"),n->alpha);
    setJsNumberK(ctx,out,FGUI_JS_KEY("childCount"),n->children.size());setJsPropertyK(ctx,out,FGUI_JS_KEY("pivotAsAnchor"),JSValueMakeBoolean(ctx,n->pivotAsAnchor));setJsPropertyK(ctx,out,FGUI_JS_KEY("visible"),JSValueMakeBoolean(ctx,n->visible));setJsPropertyK(ctx,out,FGUI_JS_KEY("touchable"),JSValueMakeBoolean(ctx,n->touchable));setJsPropertyK(ctx,out,FGUI_JS_KEY("grayed"),JSValueMakeBoolean(ctx,n->grayed));setJsPropertyK(ctx,out,FGUI_JS_KEY("internalVisible"),JSValueMakeBoolean(ctx,!n->gearHidden||n->transitionDisplayLocks>0));
    setJsPropertyK(ctx,out,FGUI_JS_KEY("nativeTextMetricsValid"),JSValueMakeBoolean(ctx,n->nativeTextMetricsValid));setJsNumberK(ctx,out,FGUI_JS_KEY("nativeTextWidth"),n->nativeTextWidth);setJsNumberK(ctx,out,FGUI_JS_KEY("nativeTextHeight"),n->nativeTextHeight);setJsNumberK(ctx,out,FGUI_JS_KEY("nativeTextLineHeight"),n->nativeTextLineHeight);setJsPropertyK(ctx,out,FGUI_JS_KEY("nativeTextFacadeNatural"),JSValueMakeBoolean(ctx,n->nativeTextFacadeNatural));
    setJsNumberK(ctx,out,FGUI_JS_KEY("bitmapTextWidth"),n->bitmapTextWidth);setJsNumberK(ctx,out,FGUI_JS_KEY("bitmapTextHeight"),n->bitmapTextHeight);setJsNumberK(ctx,out,FGUI_JS_KEY("bitmapGlyphCount"),n->bitmapGlyphCount);setJsPropertyK(ctx,out,FGUI_JS_KEY("bitmapMetricsTrusted"),JSValueMakeBoolean(ctx,n->bitmapMetricsTrusted));
    setJsPropertyK(ctx,out,FGUI_JS_KEY("schemaStatePristine"),JSValueMakeBoolean(ctx,n->facadeSchemaPristine));
    setJsNumberK(ctx,out,FGUI_JS_KEY("mutationCaps"),n->mutationCaps);
    setJsPropertyK(ctx,out,FGUI_JS_KEY("kind"),fguiJsString(ctx,fguiKindName(n->kind)));setJsPropertyK(ctx,out,FGUI_JS_KEY("id"),fguiJsString(ctx,n->id));setJsPropertyK(ctx,out,FGUI_JS_KEY("name"),fguiJsString(ctx,n->name));setJsPropertyK(ctx,out,FGUI_JS_KEY("packageName"),fguiJsString(ctx,n->packageName));setJsPropertyK(ctx,out,FGUI_JS_KEY("sourceId"),fguiJsString(ctx,n->sourceId));setJsPropertyK(ctx,out,FGUI_JS_KEY("extension"),fguiJsString(ctx,n->extension));
    JSObjectRef attrs=JSObjectMake(ctx,nullptr,nullptr);for(const auto& kv:n->attrs)setJsProperty(ctx,attrs,kv.first.c_str(),fguiJsString(ctx,kv.second));setJsPropertyK(ctx,out,FGUI_JS_KEY("attrs"),attrs);
    jscbridge::RootedValueVector controllers(ctx);controllers.reserve(n->controllers.size());for(const auto& c:n->controllers){
        JSObjectRef co=JSObjectMake(ctx,nullptr,nullptr);setJsPropertyK(ctx,co,FGUI_JS_KEY("name"),fguiJsString(ctx,c.name));setJsNumberK(ctx,co,FGUI_JS_KEY("selectedIndex"),c.selectedIndex);setJsPropertyK(ctx,co,FGUI_JS_KEY("autoRadioGroupDepth"),JSValueMakeBoolean(ctx,c.autoRadioGroupDepth));setJsPropertyK(ctx,co,FGUI_JS_KEY("pageIds"),fguiStringArray(ctx,c.pageIds,exc));setJsPropertyK(ctx,co,FGUI_JS_KEY("pageNames"),fguiStringArray(ctx,c.pageNames,exc));
        jscbridge::RootedValueVector actions(ctx);actions.reserve(c.actions.size());for(const auto& a:c.actions){JSObjectRef ao=JSObjectMake(ctx,nullptr,nullptr);setJsPropertyK(ctx,ao,FGUI_JS_KEY("type"),fguiJsString(ctx,a.type));setJsPropertyK(ctx,ao,FGUI_JS_KEY("fromPage"),fguiJsString(ctx,a.fromPage));setJsPropertyK(ctx,ao,FGUI_JS_KEY("toPage"),fguiJsString(ctx,a.toPage));setJsPropertyK(ctx,ao,FGUI_JS_KEY("objectId"),fguiJsString(ctx,a.objectId));setJsPropertyK(ctx,ao,FGUI_JS_KEY("controller"),fguiJsString(ctx,a.controller));setJsPropertyK(ctx,ao,FGUI_JS_KEY("targetPage"),fguiJsString(ctx,a.targetPage));setJsPropertyK(ctx,ao,FGUI_JS_KEY("transition"),fguiJsString(ctx,a.transition));setJsNumberK(ctx,ao,FGUI_JS_KEY("repeat"),a.repeat);setJsNumberK(ctx,ao,FGUI_JS_KEY("delay"),a.delay);setJsPropertyK(ctx,ao,FGUI_JS_KEY("stopOnExit"),JSValueMakeBoolean(ctx,a.stopOnExit));actions.push_back(ao);}setJsPropertyK(ctx,co,FGUI_JS_KEY("actions"),JSObjectMakeArray(ctx,actions.size(),actions.empty()?nullptr:actions.data(),exc));controllers.push_back(co);
    }setJsPropertyK(ctx,out,FGUI_JS_KEY("controllers"),JSObjectMakeArray(ctx,controllers.size(),controllers.empty()?nullptr:controllers.data(),exc));
    jscbridge::RootedValueVector transitions(ctx);transitions.reserve(n->transitions.size());for(const auto& t:n->transitions){JSObjectRef tr=JSObjectMake(ctx,nullptr,nullptr);setJsPropertyK(ctx,tr,FGUI_JS_KEY("name"),fguiJsString(ctx,t.name));setJsPropertyK(ctx,tr,FGUI_JS_KEY("autoPlay"),JSValueMakeBoolean(ctx,t.autoPlay));setJsNumberK(ctx,tr,FGUI_JS_KEY("autoPlayRepeat"),t.autoPlayRepeat);setJsNumberK(ctx,tr,FGUI_JS_KEY("autoPlayDelay"),t.autoPlayDelay);setJsNumberK(ctx,tr,FGUI_JS_KEY("options"),t.options);transitions.push_back(tr);}setJsPropertyK(ctx,out,FGUI_JS_KEY("transitions"),JSObjectMakeArray(ctx,transitions.size(),transitions.empty()?nullptr:transitions.data(),exc));
    return out;
}

// Layout reconciliation never reconstructs a facade: it only refreshes the live/mutable node
// surface in an already materialised metadata object.  Re-serialising controller actions and
// transition descriptors on every controller write accounted for most of the batch bridge's
// allocation cost. Keep identity fields, publish every mutable scalar, and send attrs as a complete
// bag after any runtime mutation. Authored-pristine attrs may reuse the already-validated canonical
// bag, so merging this row remains observationally identical to fguiNodeToJs for an existing facade.
JSObjectRef fguiNodeSyncToJs(JSContextRef ctx,const nfgui::InstanceNode* n,JSValueRef* exc){
    if(!n)return nullptr;JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    setJsNumberK(ctx,out,FGUI_JS_KEY("nativeId"),n->nativeId);setJsNumberK(ctx,out,FGUI_JS_KEY("parentNativeId"),n->parentNativeId);setJsNumberK(ctx,out,FGUI_JS_KEY("ownerComponentId"),n->ownerComponentId);
    setJsNumberK(ctx,out,FGUI_JS_KEY("displayContainerId"),n->displayContainerId);setJsNumberK(ctx,out,FGUI_JS_KEY("clipMaskId"),n->clipMaskId);
    setJsNumberK(ctx,out,FGUI_JS_KEY("x"),n->x);setJsNumberK(ctx,out,FGUI_JS_KEY("y"),n->y);setJsNumberK(ctx,out,FGUI_JS_KEY("width"),n->width);setJsNumberK(ctx,out,FGUI_JS_KEY("height"),n->height);
    setJsNumberK(ctx,out,FGUI_JS_KEY("sourceWidth"),n->sourceWidth);setJsNumberK(ctx,out,FGUI_JS_KEY("sourceHeight"),n->sourceHeight);setJsNumberK(ctx,out,FGUI_JS_KEY("initWidth"),n->initWidth);setJsNumberK(ctx,out,FGUI_JS_KEY("initHeight"),n->initHeight);
    setJsNumberK(ctx,out,FGUI_JS_KEY("pivotX"),n->pivotX);setJsNumberK(ctx,out,FGUI_JS_KEY("pivotY"),n->pivotY);setJsNumberK(ctx,out,FGUI_JS_KEY("scaleX"),n->scaleX);setJsNumberK(ctx,out,FGUI_JS_KEY("scaleY"),n->scaleY);setJsNumberK(ctx,out,FGUI_JS_KEY("skewX"),n->skewX);setJsNumberK(ctx,out,FGUI_JS_KEY("skewY"),n->skewY);setJsNumberK(ctx,out,FGUI_JS_KEY("rotation"),n->rotation);setJsNumberK(ctx,out,FGUI_JS_KEY("alpha"),n->alpha);
    setJsNumberK(ctx,out,FGUI_JS_KEY("childCount"),n->children.size());setJsPropertyK(ctx,out,FGUI_JS_KEY("pivotAsAnchor"),JSValueMakeBoolean(ctx,n->pivotAsAnchor));setJsPropertyK(ctx,out,FGUI_JS_KEY("visible"),JSValueMakeBoolean(ctx,n->visible));setJsPropertyK(ctx,out,FGUI_JS_KEY("touchable"),JSValueMakeBoolean(ctx,n->touchable));setJsPropertyK(ctx,out,FGUI_JS_KEY("grayed"),JSValueMakeBoolean(ctx,n->grayed));setJsPropertyK(ctx,out,FGUI_JS_KEY("internalVisible"),JSValueMakeBoolean(ctx,!n->gearHidden||n->transitionDisplayLocks>0));
    setJsPropertyK(ctx,out,FGUI_JS_KEY("nativeTextMetricsValid"),JSValueMakeBoolean(ctx,n->nativeTextMetricsValid));setJsNumberK(ctx,out,FGUI_JS_KEY("nativeTextWidth"),n->nativeTextWidth);setJsNumberK(ctx,out,FGUI_JS_KEY("nativeTextHeight"),n->nativeTextHeight);setJsNumberK(ctx,out,FGUI_JS_KEY("nativeTextLineHeight"),n->nativeTextLineHeight);setJsPropertyK(ctx,out,FGUI_JS_KEY("nativeTextFacadeNatural"),JSValueMakeBoolean(ctx,n->nativeTextFacadeNatural));
    setJsNumberK(ctx,out,FGUI_JS_KEY("bitmapTextWidth"),n->bitmapTextWidth);setJsNumberK(ctx,out,FGUI_JS_KEY("bitmapTextHeight"),n->bitmapTextHeight);setJsNumberK(ctx,out,FGUI_JS_KEY("bitmapGlyphCount"),n->bitmapGlyphCount);setJsPropertyK(ctx,out,FGUI_JS_KEY("bitmapMetricsTrusted"),JSValueMakeBoolean(ctx,n->bitmapMetricsTrusted));
    setJsPropertyK(ctx,out,FGUI_JS_KEY("schemaStatePristine"),JSValueMakeBoolean(ctx,n->facadeSchemaPristine));setJsNumberK(ctx,out,FGUI_JS_KEY("mutationCaps"),n->mutationCaps);
    setJsPropertyK(ctx,out,FGUI_JS_KEY("kind"),fguiJsString(ctx,fguiKindName(n->kind)));setJsPropertyK(ctx,out,FGUI_JS_KEY("id"),fguiJsString(ctx,n->id));setJsPropertyK(ctx,out,FGUI_JS_KEY("name"),fguiJsString(ctx,n->name));setJsPropertyK(ctx,out,FGUI_JS_KEY("packageName"),fguiJsString(ctx,n->packageName));setJsPropertyK(ctx,out,FGUI_JS_KEY("sourceId"),fguiJsString(ctx,n->sourceId));setJsPropertyK(ctx,out,FGUI_JS_KEY("extension"),fguiJsString(ctx,n->extension));
    // A pristine node still has the exact attrs object already held by its canonical JS metadata.
    // Once any runtime mutation clears facadeSchemaPristine, publish the complete bag again; this
    // is an explicit ownership invariant, not a guessed changed-fields delta.
    if(!n->facadeSchemaPristine){JSObjectRef attrs=JSObjectMake(ctx,nullptr,nullptr);for(const auto& kv:n->attrs)setJsProperty(ctx,attrs,kv.first.c_str(),fguiJsString(ctx,kv.second));setJsPropertyK(ctx,out,FGUI_JS_KEY("attrs"),attrs);}
    // Most authored nodes have no controller. Avoid manufacturing an empty JS Array for each one;
    // the consumer proves that the canonical schema also has zero controllers before accepting it.
    if(!n->controllers.empty()){jscbridge::RootedValueVector controllerState(ctx);controllerState.reserve(n->controllers.size()*2);for(const auto& c:n->controllers){controllerState.push_back(fguiJsString(ctx,c.name));controllerState.push_back(JSValueMakeNumber(ctx,c.selectedIndex));}setJsPropertyK(ctx,out,FGUI_JS_KEY("controllerState"),JSObjectMakeArray(ctx,controllerState.size(),controllerState.data(),exc));}
    return out;
}

// A JavaScript metadata schema may be reused only while every package descriptor, atlas binding
// and translation table which shaped an instance is unchanged.  Keep the token textual: all
// revisions are uint64_t and converting them to a JSC Number would lose exactness above 2^53.
std::string fguiInstanceSchemaToken(const nfgui::Instance& inst){
    std::vector<std::string> dependencies;dependencies.reserve(inst.packageDependencies.size());
    for(const auto& dependency:inst.packageDependencies)dependencies.push_back(
        dependency.name+":"+std::to_string(dependency.generation)+":"+std::to_string(dependency.resourceRevision));
    std::sort(dependencies.begin(),dependencies.end());
    std::string token=inst.packageName+"\x1f"+inst.componentName+"\x1f"+
        std::to_string(inst.packageGeneration)+":"+std::to_string(inst.resourceRevision)+":"+
        std::to_string(inst.translationRevision);
    for(const std::string& dependency:dependencies){token.push_back('\x1e');token+=dependency;}
    return token;
}

JSObjectRef fguiInstanceResultToJs(JSContextRef ctx,int id,double nativeMs,double ageNowMs,
 bool claimedReservation,bool checkout,JSValueRef* exc,const std::string* expectedSchema) {
 nfgui::Instance* inst=gNativeFgui.instance(id);if(!inst)return nullptr;
 JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
 setJsNumber(ctx,out,"instance",id);setJsNumber(ctx,out,"rootId",inst->root);
 setJsNumber(ctx,out,"nodeCount",inst->nodes.size());setJsNumber(ctx,out,"nativeMs",nativeMs);
 setJsProperty(ctx,out,"reserved",JSValueMakeBoolean(ctx,claimedReservation));
 setJsNumber(ctx,out,"reservationAgeMs",claimedReservation?std::max(0.0,ageNowMs-inst->reservedAtMs):0);
 setJsProperty(ctx,out,"compatible",JSValueMakeBoolean(ctx,inst->compatible));
 setJsProperty(ctx,out,"fallbackReason",fguiJsString(ctx,inst->fallbackReason));
 setJsProperty(ctx,out,"schemaToken",fguiJsString(ctx,fguiInstanceSchemaToken(*inst)));
 setJsProperty(ctx,out,"schemaCached",JSValueMakeBoolean(ctx,false));
 JSObjectRef root=fguiNodeToJs(ctx,gNativeFgui.node(id,inst->root),exc);
 if(root)setJsProperty(ctx,out,"root",root);
 setJsProperty(ctx,out,"handles",JSObjectMakeArray(ctx,0,nullptr,exc));return out;
}


uint64_t gFguiTextMutationCalls=0,gFguiTextMutationRows=0,
                gFguiTextMutationFailures=0;

double gFguiTextMutationMs=0;

uint64_t gFguiOrderedMutationCalls=0,gFguiOrderedMutationRows=0,
                gFguiOrderedMutationFailures=0;

double gFguiOrderedMutationMs=0;


JSValueRef cbNativeFguiNode(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2)return JSValueMakeNull(ctx);auto* n=gNativeFgui.node((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc));JSObjectRef out=fguiNodeToJs(ctx,n,exc);return out?out:JSValueMakeNull(ctx);
}

uint64_t gFguiChildBatchCalls=0,gFguiChildBatchRows=0;

// __nativeFguiNodesAll(instance, liveIds?) -> Array<nodeMeta> | null. One JSC crossing for the
// exact materialised facade ids requested by JS. The optional argument keeps old bundles working.
// each row built by the shared fguiNodeToJs so the field surface is byte-identical to the
// per-node bridge. Consumer: syncFguiLayoutFacades' full facade read-back, which previously paid
// one crossing per node (~38µs each on an 84-node component). Pure call coalescing — no dirty
// tracking, no caching, no skipped rows; ids missing from the returned set (cross-instance
// externals) fall back to the per-node bridge on the JS side.
JSValueRef cbNativeFguiNodesAll(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,
                                       const JSValueRef a[],JSValueRef* exc){
    if(argc<1||!JSValueIsNumber(ctx,a[0]))return JSValueMakeNull(ctx);
    const int instanceId=(int)JSValueToNumber(ctx,a[0],exc);
    nfgui::Instance* inst=gNativeFgui.instance(instanceId);
    if(!inst)return JSValueMakeNull(ctx);
    JSObjectRef requested=nullptr;size_t requestedCount=0;const int32_t* requestedIds=nullptr;
    if(argc>=2&&JSValueIsObject(ctx,a[1])){
        requested=JSValueToObject(ctx,a[1],exc);
        if(!requested||JSValueGetTypedArrayType(ctx,a[1],exc)!=kJSTypedArrayTypeInt32Array)return JSValueMakeNull(ctx);
        requestedCount=JSObjectGetTypedArrayLength(ctx,requested,exc);
        if(requestedCount>8192)return JSValueMakeNull(ctx);
        const uint8_t* requestedBase=(const uint8_t*)JSObjectGetTypedArrayBytesPtr(ctx,requested,exc);
        const size_t requestedOffset=JSObjectGetTypedArrayByteOffset(ctx,requested,exc);
        if((exc&&*exc)||(requestedCount&&!requestedBase))return JSValueMakeNull(ctx);
        requestedIds=(const int32_t*)(requestedBase+requestedOffset);
    }else{
        if(inst->byNativeId.size()>8192)return JSValueMakeNull(ctx);
        requestedCount=inst->byNativeId.size();
    }
    jscbridge::RootedValueVector rows(ctx);rows.reserve(requestedCount);
    auto append=[&](nfgui::InstanceNode* node)->bool{
        if(!node)return true;
        JSObjectRef row=fguiNodeToJs(ctx,node,exc);
        if((exc&&*exc)||!row)return false;
        rows.push_back(row);
        return true;
    };
    if(requested){
        for(size_t i=0;i<requestedCount;i++)if(!append(gNativeFgui.node(instanceId,requestedIds[i])))return JSValueMakeNull(ctx);
    }else for(const auto& kv:inst->byNativeId){
        if(!append(kv.second))return JSValueMakeNull(ctx);
    }
    return JSObjectMakeArray(ctx,rows.size(),rows.empty()?nullptr:rows.data(),exc);
}

// __nativeFguiSyncRows(instance, liveIds) -> Array<mutable node metadata> | null.
// This is a serialization-only optimisation for an existing facade.  Missing ids are omitted and
// JS falls back to __nativeFguiNode; a row is accepted only when a complete canonical metadata
// object already exists, so this compact ABI can never be used to construct a facade.
JSValueRef cbNativeFguiSyncRows(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,
                                      const JSValueRef a[],JSValueRef* exc){
    if(argc!=2||!JSValueIsNumber(ctx,a[0])||!JSValueIsObject(ctx,a[1]))return JSValueMakeNull(ctx);
    const int instanceId=(int)JSValueToNumber(ctx,a[0],exc);
    if(!gNativeFgui.instance(instanceId))return JSValueMakeNull(ctx);
    JSObjectRef requested=JSValueToObject(ctx,a[1],exc);
    if(!requested||JSValueGetTypedArrayType(ctx,a[1],exc)!=kJSTypedArrayTypeInt32Array)return JSValueMakeNull(ctx);
    const size_t count=JSObjectGetTypedArrayLength(ctx,requested,exc);
    if(count>8192)return JSValueMakeNull(ctx);
    const uint8_t* base=(const uint8_t*)JSObjectGetTypedArrayBytesPtr(ctx,requested,exc);
    const size_t offset=JSObjectGetTypedArrayByteOffset(ctx,requested,exc);
    if((exc&&*exc)||(count&&!base))return JSValueMakeNull(ctx);
    const int32_t* ids=(const int32_t*)(base+offset);
    const double t0=nowUs();
    jscbridge::RootedValueVector rows(ctx);rows.reserve(count);
    for(size_t i=0;i<count;i++){
        nfgui::InstanceNode* node=gNativeFgui.node(instanceId,ids[i]);
        if(!node)continue;
        JSObjectRef row=fguiNodeSyncToJs(ctx,node,exc);
        if((exc&&*exc)||!row)return JSValueMakeNull(ctx);
        rows.push_back(row);
    }
    JSValueRef out=JSObjectMakeArray(ctx,rows.size(),rows.empty()?nullptr:rows.data(),exc);
    gFguiSyncRowsCalls++;gFguiSyncRowsRows+=rows.size();gFguiSyncRowsUs+=nowUs()-t0;
    if(gFguiRowProfileLog){
        // 一次性:属性名 JSStringRef 创建+释放的单位成本——驻留省下的正是它 × 每行 ~45 个固定属性名,与场景/行成分无关
        static bool benched=false;
        if(!benched){benched=true;const double b0=nowUs();for(int i=0;i<20000;i++){JSStringRef k=JSStringCreateWithUTF8CString("parentNativeId");JSStringRelease(k);}
            const double per=(nowUs()-b0)/20000.0;HOST_LOG("[fgui-rows] bench key create+release=%.0fns → x45/row=%.2fus (PIXINATIVE_FGUI_KEY_INTERN=%d)\n",per*1000.0,per*45.0,(int)PIXINATIVE_FGUI_KEY_INTERN);}
        // 每 250 批打一行:累计均值 + 本窗口增量均值(增量不受早期场景污染,A/B 对照看增量)
        static uint64_t winRows=0;static double winUs=0;
        if((gFguiSyncRowsCalls%250)==0){
            const uint64_t dRows=gFguiSyncRowsRows-winRows;const double dUs=gFguiSyncRowsUs-winUs;
            HOST_LOG("[fgui-rows] calls=%llu rows=%llu avg=%.2fus/row | 本窗口 rows=%llu avg=%.2fus/row %.1fus/call\n",(unsigned long long)gFguiSyncRowsCalls,(unsigned long long)gFguiSyncRowsRows,
                     gFguiSyncRowsRows?gFguiSyncRowsUs/(double)gFguiSyncRowsRows:0.0,(unsigned long long)dRows,dRows?dUs/(double)dRows:0.0,dUs/250.0);
            winRows=gFguiSyncRowsRows;winUs=gFguiSyncRowsUs;
        }
    }
    return out;
}

// __nativeFguiChildren(instance,parent) returns immutable metadata for all direct semantic
// children in one JSC crossing. Extension constructors still materialise only the GObjects they
// ask for; this merely avoids rebuilding the same parent lookup/controller/attrs bridge several
// times while Button/ProgressBar/custom constructFromXML binds its named roles.
JSValueRef cbNativeFguiChildren(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,
                                       const JSValueRef a[],JSValueRef* exc){
    ++gFguiChildBatchCalls;
    if(argc!=2||!JSValueIsNumber(ctx,a[0])||!JSValueIsNumber(ctx,a[1]))
        return JSObjectMakeArray(ctx,0,nullptr,exc);
    const int instanceId=(int)JSValueToNumber(ctx,a[0],exc),
              parentId=(int)JSValueToNumber(ctx,a[1],exc);
    nfgui::InstanceNode* parent=gNativeFgui.node(instanceId,parentId);
    if(!parent||parent->children.size()>4096)return JSObjectMakeArray(ctx,0,nullptr,exc);
    jscbridge::RootedValueVector rows(ctx);rows.reserve(parent->children.size());
    for(int childId:parent->children){
        JSObjectRef row=fguiNodeToJs(ctx,gNativeFgui.node(instanceId,childId),exc);
        if((exc&&*exc)||!row)return JSObjectMakeArray(ctx,0,nullptr,exc);
        rows.push_back(row);
    }
    gFguiChildBatchRows+=rows.size();
    return JSObjectMakeArray(ctx,rows.size(),rows.empty()?nullptr:rows.data(),exc);
}

// __nativeFguiNodeRoots(Int32Array ids) -> Array<number>:每个节点所属实例的 root nativeId(不受管理=0)。
// 覆盖审计用:提交审计必须按 C++ 实际实例 root 归组(thin/external 节点的提交发布在其所属实例 root 上)。
JSValueRef cbNativeFguiNodeRoots(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc!=1||!JSValueIsObject(ctx,a[0]))return JSValueMakeNull(ctx);
    JSObjectRef requested=JSValueToObject(ctx,a[0],exc);
    if(!requested||JSValueGetTypedArrayType(ctx,a[0],exc)!=kJSTypedArrayTypeInt32Array)return JSValueMakeNull(ctx);
    const size_t count=JSObjectGetTypedArrayLength(ctx,requested,exc);
    if(count>8192)return JSValueMakeNull(ctx);
    const uint8_t* base=(const uint8_t*)JSObjectGetTypedArrayBytesPtr(ctx,requested,exc);
    const size_t offset=JSObjectGetTypedArrayByteOffset(ctx,requested,exc);
    if((exc&&*exc)||(count&&!base))return JSValueMakeNull(ctx);
    const int32_t* ids=(const int32_t*)(base+offset);
    jscbridge::RootedValueVector out(ctx);out.reserve(count);
    for(size_t i=0;i<count;i++)out.push_back(JSValueMakeNumber(ctx,gNativeFgui.instanceRootForNode(ids[i])));
    return JSObjectMakeArray(ctx,out.size(),out.empty()?nullptr:out.data(),exc);
}
// __nativeFguiChildOrder(instanceId,parentId) -> Array<number>:语义子节点 id 顺序(不做行转换)。覆盖审计用:
// 紧凑行只有 childCount,同父内重排必须比顺序才能对拍 childSeq。
JSValueRef cbNativeFguiChildOrder(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc!=2||!JSValueIsNumber(ctx,a[0])||!JSValueIsNumber(ctx,a[1]))return JSObjectMakeArray(ctx,0,nullptr,exc);
    nfgui::InstanceNode* parent=gNativeFgui.node((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc));
    if(!parent||parent->children.size()>4096)return JSObjectMakeArray(ctx,0,nullptr,exc);
    jscbridge::RootedValueVector out(ctx);out.reserve(parent->children.size());
    for(int childId:parent->children)out.push_back(JSValueMakeNumber(ctx,childId));
    return JSObjectMakeArray(ctx,out.size(),out.empty()?nullptr:out.data(),exc);
}

// Layout: root, icon, num_con, spine_icon.  Runtime validates the exact six-node component and
// checkout lifecycle first, so no partial role table can cross into JavaScript.


// Runtime validates the complete exact role table before this callback allocates or publishes a
// TypedArray, so JavaScript can never observe a partially matched package revision.


JSValueRef cbNativeFguiDisplayChildCount(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2||!gNativeFgui.instance((int)JSValueToNumber(ctx,a[0],exc)))return JSValueMakeNumber(ctx,0);
    return JSValueMakeNumber(ctx,gSG.childCount((int)JSValueToNumber(ctx,a[1],exc)));
}

JSValueRef cbNativeFguiDisplayChildAt(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3||!gNativeFgui.instance((int)JSValueToNumber(ctx,a[0],exc)))return JSValueMakeNumber(ctx,-1);
    return JSValueMakeNumber(ctx,gSG.childAt((int)JSValueToNumber(ctx,a[1],exc),(int)JSValueToNumber(ctx,a[2],exc)));
}

JSValueRef cbNativeFguiDisplayChildIndex(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3||!gNativeFgui.instance((int)JSValueToNumber(ctx,a[0],exc)))return JSValueMakeNumber(ctx,-1);
    return JSValueMakeNumber(ctx,gSG.childIndex((int)JSValueToNumber(ctx,a[1],exc),(int)JSValueToNumber(ctx,a[2],exc)));
}

JSValueRef cbNativeFguiAdoptDisplayContainer(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3)return JSValueMakeBoolean(ctx,false);return JSValueMakeBoolean(ctx,gNativeFgui.adoptDisplayContainer((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),(int)JSValueToNumber(ctx,a[2],exc),gSG));
}

JSValueRef cbNativeFguiReparent(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<5)return JSValueMakeBoolean(ctx,false);return JSValueMakeBoolean(ctx,gNativeFgui.reparent((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),(int)JSValueToNumber(ctx,a[2],exc),(int)JSValueToNumber(ctx,a[3],exc),(int)JSValueToNumber(ctx,a[4],exc),gSG));
}

JSValueRef cbNativeFguiSetNodeSize(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);bool ok=gNativeFgui.setNodeSize((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),(float)JSValueToNumber(ctx,a[2],exc),(float)JSValueToNumber(ctx,a[3],exc),gSG);return JSValueMakeBoolean(ctx,ok);
}

JSValueRef cbNativeFguiNodeGeometry(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2)return JSValueMakeNull(ctx);
    float g[6];
    if(!gNativeFgui.nodeGeometry((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),g))return JSValueMakeNull(ctx);
    JSValueRef vals[6];for(int k=0;k<6;k++)vals[k]=JSValueMakeNumber(ctx,g[k]);
    return JSObjectMakeArray(ctx,6,vals,nullptr);
}

JSValueRef cbNativeFguiJournalEpoch(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef*){
    return JSValueMakeNumber(ctx,(double)gNativeFgui.journalEpoch());   // B1 版本屏障:等值 setter 廉价比对,变化才 drain 影子
}

JSValueRef cbNativeFguiSetNodePosition(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);bool ok=gNativeFgui.setNodePosition((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),(float)JSValueToNumber(ctx,a[2],exc),(float)JSValueToNumber(ctx,a[3],exc),gSG);
    // 成功返回传播计数(0 = 只动了目标;>0 = relations/group 成员也动了,JS push 模式据此立即同步 facade),失败返回 false
    return ok?JSValueMakeNumber(ctx,gNativeFgui.lastWriteFanout):JSValueMakeBoolean(ctx,false);
}

JSValueRef cbNativeFguiClearNodeRelations(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2)return JSValueMakeBoolean(ctx,false);
    return JSValueMakeBoolean(ctx,gNativeFgui.clearNodeRelations((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),gSG));
}

JSValueRef cbNativeFguiSetNodeScale(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);bool ok=gNativeFgui.setNodeScale((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),(float)JSValueToNumber(ctx,a[2],exc),(float)JSValueToNumber(ctx,a[3],exc),gSG);return JSValueMakeBoolean(ctx,ok);
}

JSValueRef cbNativeFguiSetNodeRotation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3)return JSValueMakeBoolean(ctx,false);bool ok=gNativeFgui.setNodeRotation((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),(float)JSValueToNumber(ctx,a[2],exc),gSG);return JSValueMakeBoolean(ctx,ok);
}

JSValueRef cbNativeFguiSetNodePivot(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<5)return JSValueMakeBoolean(ctx,false);bool ok=gNativeFgui.setNodePivot((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),(float)JSValueToNumber(ctx,a[2],exc),(float)JSValueToNumber(ctx,a[3],exc),JSValueToBoolean(ctx,a[4]),gSG);return JSValueMakeBoolean(ctx,ok);
}

JSValueRef cbNativeFguiSetNodeAlpha(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3)return JSValueMakeBoolean(ctx,false);bool ok=gNativeFgui.setNodeAlpha((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),(float)JSValueToNumber(ctx,a[2],exc),gSG);return JSValueMakeBoolean(ctx,ok);
}

JSValueRef cbNativeFguiSetNodeDisplayPosition(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);bool ok=gNativeFgui.setNodeDisplayPosition((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),(float)JSValueToNumber(ctx,a[2],exc),(float)JSValueToNumber(ctx,a[3],exc),gSG);return JSValueMakeBoolean(ctx,ok);
}

JSValueRef cbNativeFguiSetNodeDisplayScale(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);bool ok=gNativeFgui.setNodeDisplayScale((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),(float)JSValueToNumber(ctx,a[2],exc),(float)JSValueToNumber(ctx,a[3],exc),gSG);return JSValueMakeBoolean(ctx,ok);
}

JSValueRef cbNativeFguiSetNodeDisplayRotation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3)return JSValueMakeBoolean(ctx,false);bool ok=gNativeFgui.setNodeDisplayRotation((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),(float)JSValueToNumber(ctx,a[2],exc),gSG);return JSValueMakeBoolean(ctx,ok);
}

JSValueRef cbNativeFguiSetNodeVisible(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<3)return JSValueMakeBoolean(ctx,false);bool ok=gNativeFgui.setNodeVisible((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),JSValueToBoolean(ctx,a[2]),gSG,argc>3&&JSValueToBoolean(ctx,a[3]));return JSValueMakeBoolean(ctx,ok);
}

JSValueRef cbNativeFguiGetMovieClip(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<2)return JSValueMakeNumber(ctx,-1);return JSValueMakeNumber(ctx,gNativeFgui.movieClipState((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc)));
}

JSValueRef cbNativeFguiSetMovieClip(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);return JSValueMakeBoolean(ctx,gNativeFgui.setMovieClipState((int)JSValueToNumber(ctx,a[0],exc),(int)JSValueToNumber(ctx,a[1],exc),(int)JSValueToNumber(ctx,a[2],exc),(int)JSValueToNumber(ctx,a[3],exc),gSG));
}

JSValueRef cbNativeFguiTransitionAdvance(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){
    (void)argc;(void)a;auto done=gNativeFgui.takeCompletedTransitions();if(done.empty())return JSValueMakeNull(ctx);std::vector<JSValueRef> values;values.reserve(done.size());for(int token:done)values.push_back(JSValueMakeNumber(ctx,token));return JSObjectMakeArray(ctx,values.size(),values.data(),exc);
}

JSValueRef cbNativeFguiTransitionSignals(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef* exc){auto signals=gNativeFgui.takeTransitionSignals();if(signals.empty())return JSValueMakeNull(ctx);jscbridge::RootedValueVector out(ctx);out.reserve(signals.size()*4);for(const auto& signal:signals){out.push_back(JSValueMakeNumber(ctx,signal.token));out.push_back(JSValueMakeNumber(ctx,signal.componentId));out.push_back(fguiJsString(ctx,signal.transitionName));out.push_back(fguiJsString(ctx,signal.label));}return JSObjectMakeArray(ctx,out.size(),out.data(),exc);}

JSValueRef cbNativeFguiTransitionVisibilitySignals(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef* exc){auto signals=gNativeFgui.takeTransitionVisibilitySignals();if(signals.empty())return JSValueMakeNull(ctx);std::vector<JSValueRef> out;out.reserve(signals.size()*3);for(const auto& signal:signals){out.push_back(JSValueMakeNumber(ctx,signal.instanceId));out.push_back(JSValueMakeNumber(ctx,signal.nativeId));out.push_back(JSValueMakeBoolean(ctx,signal.visible));}return JSObjectMakeArray(ctx,out.size(),out.data(),exc);}

JSValueRef cbNativeFguiTransitionPropertySignals(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef* exc){auto signals=gNativeFgui.takeTransitionPropertySignals();if(signals.empty())return JSValueMakeNull(ctx);jscbridge::RootedValueVector out(ctx);out.reserve(signals.size()*6);for(const auto& signal:signals){out.push_back(JSValueMakeNumber(ctx,signal.instanceId));out.push_back(JSValueMakeNumber(ctx,signal.nativeId));out.push_back(JSValueMakeNumber(ctx,static_cast<int>(signal.kind)));out.push_back(JSValueMakeNumber(ctx,signal.value0));out.push_back(JSValueMakeNumber(ctx,signal.value1));out.push_back(fguiJsString(ctx,signal.text));}return JSObjectMakeArray(ctx,out.size(),out.data(),exc);}

JSValueRef cbNativeFguiTaskAdd(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc<4)return JSValueMakeBoolean(ctx,false);gNativeFgui.scheduleTask((int)JSValueToNumber(ctx,a[0],exc),JSValueToNumber(ctx,a[1],exc),(int)JSValueToNumber(ctx,a[2],exc),JSValueToNumber(ctx,a[3],exc));return JSValueMakeBoolean(ctx,true);}

JSValueRef cbNativeFguiTaskCancel(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc>0)gNativeFgui.cancelTask((int)JSValueToNumber(ctx,a[0],exc));return JSValueMakeUndefined(ctx);}

JSValueRef cbNativeFguiTaskDrain(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef* exc){auto done=gNativeFgui.takeCompletedTasks();if(done.empty())return JSValueMakeNull(ctx);std::vector<JSValueRef> values;values.reserve(done.size());for(int token:done)values.push_back(JSValueMakeNumber(ctx,token));return JSObjectMakeArray(ctx,values.size(),values.data(),exc);}

JSValueRef cbNativeFguiDestroy(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef a[],JSValueRef* exc){if(argc>0)gNativeFgui.destroy((int)JSValueToNumber(ctx,a[0],exc),gSG,gNativeBatchSerial,(nowUs()-gStartUs)/1000.0);return JSValueMakeUndefined(ctx);}

JSValueRef cbNativeFguiRuntimeStats(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef*){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);nfgui::CoverageStats coverage=gNativeFgui.coverage(gSG);
    const nfgui::TemplateStats& templates=gNativeFgui.templateStats();
    const nfgui::ReservationStats& reservations=gNativeFgui.reservationStats();
    const nfgui::CheckoutStats& checkouts=gNativeFgui.checkoutStats();
    const nfgui::TextOwnershipStats& textOwnership=gNativeFgui.textOwnershipStats();
    setJsNumber(ctx,out,"packages",gNativeFgui.packageCount());setJsNumber(ctx,out,"preparedPackages",gNativeFgui.preparedPackageCount());setJsNumber(ctx,out,"instances",gNativeFgui.instanceCount());setJsNumber(ctx,out,"scheduledTasks",gNativeFgui.scheduledTaskCount());setJsNumber(ctx,out,"movieClipResources",gNativeFgui.movieClipResourceCount());setJsNumber(ctx,out,"movieClipNodes",gNativeFgui.movieClipNodeCount());setJsNumber(ctx,out,"fguiChildBatchCalls",gFguiChildBatchCalls);setJsNumber(ctx,out,"fguiChildBatchRows",gFguiChildBatchRows);setJsNumber(ctx,out,"managedFreeDeferrals",gNativeFguiManagedFreeDeferrals);
    setJsNumber(ctx,out,"facadeMarks",(double)nfgui::FacadeSeqTracker::marks);setJsNumber(ctx,out,"facadeCommits",(double)nfgui::FacadeSeqTracker::commits);setJsNumber(ctx,out,"facadeUnscoped",(double)nfgui::FacadeSeqTracker::unscoped);setJsNumber(ctx,out,"facadeDeferred",(double)nfgui::FacadeSeqTracker::deferred);setJsNumber(ctx,out,"facadePending",(double)nfgui::FacadeSeqTracker::pending.size());setJsNumber(ctx,out,"facadeOrphans",(double)nfgui::FacadeSeqTracker::orphans);setJsNumber(ctx,out,"facadeDroppedDead",(double)nfgui::FacadeSeqTracker::droppedDead);
    setJsNumber(ctx,out,"templateHits",templates.hits);setJsNumber(ctx,out,"templateMisses",templates.misses);setJsNumber(ctx,out,"templateClonedNodes",templates.clonedNodes);setJsNumber(ctx,out,"templateCloneMs",templates.cloneMs);
    setJsNumber(ctx,out,"reservedInstances",gNativeFgui.reservedInstanceCount());setJsNumber(ctx,out,"reservationPolicies",gNativeFgui.reservationPolicyCount());setJsNumber(ctx,out,"reservationPolicyLive",gNativeFgui.reservationPolicyLiveCount());setJsNumber(ctx,out,"pendingReservationRefills",gNativeFgui.pendingReservationRefillCount());setJsNumber(ctx,out,"reservationBuilt",reservations.built);setJsNumber(ctx,out,"reservationBuiltNodes",reservations.builtNodes);setJsNumber(ctx,out,"reservationClaimed",reservations.claimed);setJsNumber(ctx,out,"reservationMisses",reservations.misses);setJsNumber(ctx,out,"reservationStaleDiscarded",reservations.staleDiscarded);setJsNumber(ctx,out,"reservationInvalidated",reservations.invalidated);setJsNumber(ctx,out,"reservationInvalidatedNodes",reservations.invalidatedNodes);setJsNumber(ctx,out,"reservationFailed",reservations.failed);setJsNumber(ctx,out,"reservationBlocked",reservations.blocked);setJsProperty(ctx,out,"reservationBlock",fguiJsString(ctx,gNativeFgui.reservationBlock()));setJsNumber(ctx,out,"reservationBuildMs",reservations.buildMs);setJsNumber(ctx,out,"reservationClaimMs",reservations.claimMs);setJsNumber(ctx,out,"reservationRefillQueued",reservations.refillQueued);setJsNumber(ctx,out,"reservationRefillBuilt",reservations.refillBuilt);setJsNumber(ctx,out,"reservationRefillBlocked",reservations.refillBlocked);setJsNumber(ctx,out,"reservationRefillCanceled",reservations.refillCanceled);setJsNumber(ctx,out,"reservationRefillFrames",reservations.refillFrames);setJsNumber(ctx,out,"reservationRefillBuildMs",reservations.refillBuildMs);setJsNumber(ctx,out,"reservationRefillMaxMs",reservations.refillMaxMs);
    setJsNumber(ctx,out,"checkedOutInstances",gNativeFgui.checkedOutInstanceCount());setJsNumber(ctx,out,"checkoutPrepared",checkouts.prepared);setJsNumber(ctx,out,"checkoutPromoted",checkouts.promoted);setJsNumber(ctx,out,"checkoutCanceled",checkouts.canceled);
    setJsNumber(ctx,out,"textFacadePublished",textOwnership.published);setJsNumber(ctx,out,"textFacadePreservedRefreshes",textOwnership.preservedRefreshes);setJsNumber(ctx,out,"textFacadeNativeRevokes",textOwnership.nativeRevokes);
    setJsNumber(ctx,out,"syncRowsCalls",(double)gFguiSyncRowsCalls);setJsNumber(ctx,out,"syncRowsRows",(double)gFguiSyncRowsRows);setJsNumber(ctx,out,"syncRowsUs",gFguiSyncRowsUs);
    setJsNumber(ctx,out,"fguiTextMutationCalls",gFguiTextMutationCalls);setJsNumber(ctx,out,"fguiTextMutationRows",gFguiTextMutationRows);setJsNumber(ctx,out,"fguiTextMutationFailures",gFguiTextMutationFailures);setJsNumber(ctx,out,"fguiTextMutationMs",gFguiTextMutationMs);setJsNumber(ctx,out,"fguiOrderedMutationCalls",gFguiOrderedMutationCalls);setJsNumber(ctx,out,"fguiOrderedMutationRows",gFguiOrderedMutationRows);setJsNumber(ctx,out,"fguiOrderedMutationFailures",gFguiOrderedMutationFailures);setJsNumber(ctx,out,"fguiOrderedMutationMs",gFguiOrderedMutationMs);
    setJsNumber(ctx,out,"nodes",coverage.nodes);setJsNumber(ctx,out,"nonEmptyTextNodes",coverage.nonEmptyTextNodes);setJsNumber(ctx,out,"bitmapTextNodes",coverage.bitmapTextNodes);setJsNumber(ctx,out,"textFailures",coverage.textFailures);setJsNumber(ctx,out,"bitmapTextFailures",coverage.bitmapTextFailures);
    jscbridge::RootedValueVector samples(ctx);samples.reserve(coverage.failureSamples.size());for(const auto& sample:coverage.failureSamples)samples.push_back(fguiJsString(ctx,sample));JSObjectRef sampleArray=JSObjectMakeArray(ctx,samples.size(),samples.data(),nullptr);JSStringRef sampleKey=JSStringCreateWithUTF8CString("failureSamples");JSObjectSetProperty(ctx,out,sampleKey,sampleArray,kJSPropertyAttributeNone,nullptr);JSStringRelease(sampleKey);return out;
}

JSValueRef cbNativeFguiCompatibilityIssues(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef* exc){
    std::vector<nfgui::CompatibilityIssue> issues=gNativeFgui.compatibilityIssues();
    jscbridge::RootedValueVector rows(ctx);rows.reserve(issues.size());
    for(const auto& issue:issues){
        JSObjectRef row=JSObjectMake(ctx,nullptr,nullptr);
        setJsProperty(ctx,row,"packageName",fguiJsString(ctx,issue.packageName));
        setJsProperty(ctx,row,"componentName",fguiJsString(ctx,issue.componentName));
        setJsProperty(ctx,row,"reason",fguiJsString(ctx,issue.reason));
        rows.push_back(row);
    }
    return JSObjectMakeArray(ctx,rows.size(),rows.empty()?nullptr:rows.data(),exc);
}


// Returns the raw marker text ("release\nsequence\nreason") — the unique token the JS client reports
// and must present back to the consume bridge below.


NativeSpinePrepareCounters gSpinePrepareStats;

JSValueRef cbNativeSpineRecordMain(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<4)return JSValueMakeUndefined(ctx);double atlasMs=JSValueToNumber(ctx,args[1],exc),skeletonMs=JSValueToNumber(ctx,args[2],exc);bool ok=JSValueToBoolean(ctx,args[3]);gSpinePrepareStats.atlasMainMs+=atlasMs;gSpinePrepareStats.skeletonMainMs+=skeletonMs;if(skeletonMs>0){gSpinePrepareStats.skeletonCalls++;gSpinePrepareStats.maxSkeletonMainMs=std::max(gSpinePrepareStats.maxSkeletonMainMs,skeletonMs);}if(ok)gSpinePrepareStats.preparedLoads++;else gSpinePrepareStats.loadFailures++;return JSValueMakeUndefined(ctx);
}

JSValueRef cbNativeSpineRecordCompatSkeleton(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1)return JSValueMakeUndefined(ctx);double elapsed=JSValueToNumber(ctx,args[0],exc);gSpinePrepareStats.compatSkeletonCalls++;gSpinePrepareStats.skeletonCalls++;gSpinePrepareStats.compatSkeletonMainMs+=elapsed;gSpinePrepareStats.skeletonMainMs+=elapsed;gSpinePrepareStats.maxSkeletonMainMs=std::max(gSpinePrepareStats.maxSkeletonMainMs,elapsed);return JSValueMakeUndefined(ctx);
}

// ---- P1a 实例管理/state 门面/事件桥（全部数字通信；动画等一律用 __spxBuild 下发的名字表 index）----
spinex::Instance* spinexInstArg(JSContextRef ctx,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1)return nullptr;
    return gSpinexRt.find((int)JSValueToNumber(ctx,args[0],exc));
}

spine::Animation* spinexAnimAt(spinex::Instance* inst,int animIdx){
    if(!inst||animIdx<0)return nullptr;
    spine::Vector<spine::Animation*>& anims=inst->skeleton()->getData()->getAnimations();
    return (size_t)animIdx<anims.size()?anims[animIdx]:nullptr;
}

// __spxRelease(sdHandle) → 剩余 refs（-1=handle 不存在）。归零且无活体实例时销毁条目（纹理对称 release）。
JSValueRef cbSpinexRelease(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1)return JSValueMakeNumber(ctx,-1);
    spinex::DataStore::Entry* entry=gSpinexRt.store.findByHandle((int)JSValueToNumber(ctx,args[0],exc));
    return JSValueMakeNumber(ctx,entry?(double)gSpinexRt.store.release(entry):-1);
}

// __spxRetain(sdHandle) → bool。按已知身份直接加引用(门面构造的 per-instance 引用):
// 不经过 prepared 包,preparse LRU 逐出与短名歧义都无关;条目已销毁返回 false,JS 回退重建。
JSValueRef cbSpinexRetain(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1)return JSValueMakeBoolean(ctx,false);
    spinex::DataStore::Entry* entry=gSpinexRt.store.findByHandle((int)JSValueToNumber(ctx,args[0],exc));
    if(!entry||!entry->data)return JSValueMakeBoolean(ctx,false);
    gSpinexRt.store.retain(entry);
    return JSValueMakeBoolean(ctx,true);
}

// __spxCreate(sdHandle,nodeId) → instId（0=失败）。实例绑定现有场景图节点，skeleton 初始 setToSetupPose。
JSValueRef cbSpinexCreate(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<2)return JSValueMakeNumber(ctx,0);
    spinex::DataStore::Entry* entry=gSpinexRt.store.findByHandle((int)JSValueToNumber(ctx,args[0],exc));
    int node=(int)JSValueToNumber(ctx,args[1],exc);
    if(!entry||node<=0||node>=sg::NODE_CAP||!gSG.nodes[node].alive)return JSValueMakeNumber(ctx,0);
    return JSValueMakeNumber(ctx,(double)gSpinexRt.createInstance(entry,node));
}

// __spxDestroy(instId)：节点 quads 清空 + 事件队列剔除该实例 + 数据条目活体计数收尾。
JSValueRef cbSpinexDestroy(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1)return JSValueMakeBoolean(ctx,false);
    return JSValueMakeBoolean(ctx,gSpinexRt.destroyInstance((int)JSValueToNumber(ctx,args[0],exc),&gSG));
}

// __spxSetAnimation(instId,track,animIdx,loop) → trackKey（0=失败）。trackKey=递增序号（TrackEntry
// 指针不跨桥；JS 门面用 (instId,track) 定位，trackKey 供事件/门面关联）。
JSValueRef cbSpinexSetAnimation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=argc>=4?spinexInstArg(ctx,argc,args,exc):nullptr;
    if(!inst)return JSValueMakeNumber(ctx,0);
    int track=(int)JSValueToNumber(ctx,args[1],exc);
    spine::Animation* anim=spinexAnimAt(inst,(int)JSValueToNumber(ctx,args[2],exc));
    if(track<0||!anim)return JSValueMakeNumber(ctx,0);
    spinex::fenceTrackReplacement(inst->pendingDt);
    spine::TrackEntry* te=inst->state()->setAnimation((size_t)track,anim,JSValueToBoolean(ctx,args[3]));
    if(!te)return JSValueMakeNumber(ctx,0);
    inst->primed=false;   // 换动画后下一帧重新建立首帧输出
    return JSValueMakeNumber(ctx,(double)gSpinexRt.registerTrackEntry(inst->instId,te));
}

JSValueRef cbSpinexAddAnimation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=argc>=5?spinexInstArg(ctx,argc,args,exc):nullptr;
    if(!inst)return JSValueMakeNumber(ctx,0);
    int track=(int)JSValueToNumber(ctx,args[1],exc);
    spine::Animation* anim=spinexAnimAt(inst,(int)JSValueToNumber(ctx,args[2],exc));
    if(track<0||!anim)return JSValueMakeNumber(ctx,0);
    spine::TrackEntry* te=inst->state()->addAnimation((size_t)track,anim,JSValueToBoolean(ctx,args[3]),
                                                      (float)JSValueToNumber(ctx,args[4],exc));
    return JSValueMakeNumber(ctx,te?(double)gSpinexRt.registerTrackEntry(inst->instId,te):0);
}

JSValueRef cbSpinexSetEmptyAnimation(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=argc>=3?spinexInstArg(ctx,argc,args,exc):nullptr;
    if(!inst)return JSValueMakeBoolean(ctx,false);
    int track=(int)JSValueToNumber(ctx,args[1],exc);
    if(track<0)return JSValueMakeBoolean(ctx,false);
    spinex::fenceTrackReplacement(inst->pendingDt);
    inst->state()->setEmptyAnimation((size_t)track,(float)JSValueToNumber(ctx,args[2],exc));
    inst->primed=false;
    return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbSpinexClearTrack(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=argc>=2?spinexInstArg(ctx,argc,args,exc):nullptr;
    if(!inst)return JSValueMakeBoolean(ctx,false);
    int track=(int)JSValueToNumber(ctx,args[1],exc);
    if(track<0)return JSValueMakeBoolean(ctx,false);
    spinex::fenceTrackReplacement(inst->pendingDt);
    inst->state()->clearTrack((size_t)track);
    inst->primed=false;
    return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbSpinexClearTracks(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=spinexInstArg(ctx,argc,args,exc);
    if(!inst)return JSValueMakeBoolean(ctx,false);
    spinex::fenceTrackReplacement(inst->pendingDt);
    inst->state()->clearTracks();
    inst->primed=false;
    return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbSpinexSetTimeScale(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=argc>=2?spinexInstArg(ctx,argc,args,exc):nullptr;
    if(!inst)return JSValueMakeBoolean(ctx,false);
    inst->state()->setTimeScale((float)JSValueToNumber(ctx,args[1],exc));
    return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbSpinexSetTrackTimeScale(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=argc>=3?spinexInstArg(ctx,argc,args,exc):nullptr;
    if(!inst)return JSValueMakeBoolean(ctx,false);
    int track=(int)JSValueToNumber(ctx,args[1],exc);
    spine::TrackEntry* te=track>=0?inst->state()->getCurrent((size_t)track):nullptr;
    if(!te)return JSValueMakeBoolean(ctx,false);
    te->setTimeScale((float)JSValueToNumber(ctx,args[2],exc));
    return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbSpinexSetMix(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=argc>=4?spinexInstArg(ctx,argc,args,exc):nullptr;
    if(!inst)return JSValueMakeBoolean(ctx,false);
    spine::Animation* from=spinexAnimAt(inst,(int)JSValueToNumber(ctx,args[1],exc));
    spine::Animation* to=spinexAnimAt(inst,(int)JSValueToNumber(ctx,args[2],exc));
    if(!from||!to)return JSValueMakeBoolean(ctx,false);
    inst->stateData()->setMix(from,to,(float)JSValueToNumber(ctx,args[3],exc));
    return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbSpinexSetDefaultMix(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=argc>=2?spinexInstArg(ctx,argc,args,exc):nullptr;
    if(!inst)return JSValueMakeBoolean(ctx,false);
    inst->stateData()->setDefaultMix((float)JSValueToNumber(ctx,args[1],exc));
    return JSValueMakeBoolean(ctx,true);
}

// __spxSetSkin(instId,skinIdx|-1)：-1=清皮肤。皮肤 index 对应 build 下发的 skinNames。
JSValueRef cbSpinexSetSkin(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=argc>=2?spinexInstArg(ctx,argc,args,exc):nullptr;
    if(!inst)return JSValueMakeBoolean(ctx,false);
    int skinIdx=(int)JSValueToNumber(ctx,args[1],exc);
    if(skinIdx<0){inst->skeleton()->setSkin((spine::Skin*)nullptr);inst->primed=false;return JSValueMakeBoolean(ctx,true);}
    spine::Vector<spine::Skin*>& skins=inst->skeleton()->getData()->getSkins();
    if((size_t)skinIdx>=skins.size())return JSValueMakeBoolean(ctx,false);
    inst->skeleton()->setSkin(skins[skinIdx]);
    inst->primed=false;
    return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbSpinexSetToSetupPose(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=spinexInstArg(ctx,argc,args,exc);
    if(!inst)return JSValueMakeBoolean(ctx,false);
    inst->skeleton()->setToSetupPose();
    inst->primed=false;
    return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbSpinexSetSlotsToSetupPose(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=spinexInstArg(ctx,argc,args,exc);
    if(!inst)return JSValueMakeBoolean(ctx,false);
    inst->skeleton()->setSlotsToSetupPose();
    inst->primed=false;
    return JSValueMakeBoolean(ctx,true);
}

// __spxSlotSetColor(instId, slotIndex, r,g,b,a) → bool。spine-ts 业务直写 slot.color.a(如
// PopupCoinSurgePickerGame 用 color.a=0 隐藏 PICKME 提示);渲染管线已按 slot alpha 合成/剔除
// (spinex.h:1001/1086),此桥补上写入口。
JSValueRef cbSpinexSlotSetColor(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=spinexInstArg(ctx,argc,args,exc);
    if(!inst||argc<6)return JSValueMakeBoolean(ctx,false);
    int slotIdx=(int)JSValueToNumber(ctx,args[1],exc);
    spine::Vector<spine::Slot*>& slots=inst->skeleton()->getSlots();
    if(slotIdx<0||(size_t)slotIdx>=slots.size())return JSValueMakeBoolean(ctx,false);
    float r=(float)JSValueToNumber(ctx,args[2],exc),g=(float)JSValueToNumber(ctx,args[3],exc);
    float b=(float)JSValueToNumber(ctx,args[4],exc),a=(float)JSValueToNumber(ctx,args[5],exc);
    slots[slotIdx]->getColor().set(r,g,b,a);
    inst->primed=false;
    return JSValueMakeBoolean(ctx,true);
}

// __spxSlotSetToSetupPose(instId, slotIndex) → bool。spine-ts Slot.setToSetupPose 的单 slot
// 版本(颜色+attachment 恢复 setup;PopupCoinSurgePickerGame 用它藏/复位提示 slot)。
JSValueRef cbSpinexSlotSetToSetupPose(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=spinexInstArg(ctx,argc,args,exc);
    if(!inst||argc<2)return JSValueMakeBoolean(ctx,false);
    int slotIdx=(int)JSValueToNumber(ctx,args[1],exc);
    spine::Vector<spine::Slot*>& slots=inst->skeleton()->getSlots();
    if(slotIdx<0||(size_t)slotIdx>=slots.size())return JSValueMakeBoolean(ctx,false);
    slots[slotIdx]->setToSetupPose();
    inst->primed=false;
    return JSValueMakeBoolean(ctx,true);
}

// __spxUpdateCache(instId) → bool。与 spine-ts Skeleton.updateCache 同语义：皮肤、骨骼或
// 约束集变化后重建 active/update 顺序。这不是绘制缓存，不能在 JS 门面做空实现。
JSValueRef cbSpinexUpdateCache(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=spinexInstArg(ctx,argc,args,exc);
    if(!inst)return JSValueMakeBoolean(ctx,false);
    inst->skeleton()->updateCache();
    inst->primed=false;
    return JSValueMakeBoolean(ctx,true);
}

// Spine 4.2 Skeleton.physicsTranslate/physicsRotate. These are motion compensation inputs for
// every PhysicsConstraint, not scene-node transforms; the JS caller has already moved its Pixi
// container and supplies the corresponding skeleton-local delta.
JSValueRef cbSpinexPhysicsTranslate(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<3)return JSValueMakeBoolean(ctx,false);
    return JSValueMakeBoolean(ctx,gSpinexRt.physicsTranslate(
        (int)JSValueToNumber(ctx,args[0],exc),(float)JSValueToNumber(ctx,args[1],exc),
        (float)JSValueToNumber(ctx,args[2],exc)));
}

JSValueRef cbSpinexPhysicsRotate(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);
    return JSValueMakeBoolean(ctx,gSpinexRt.physicsRotate(
        (int)JSValueToNumber(ctx,args[0],exc),(float)JSValueToNumber(ctx,args[1],exc),
        (float)JSValueToNumber(ctx,args[2],exc),(float)JSValueToNumber(ctx,args[3],exc)));
}

// __spxTrackInfo(instId,track) → {ok,animIdx,trackTime,duration,loop,timeScale}。TrackEntry 轻量读。
JSValueRef cbSpinexTrackInfo(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    spinex::Instance* inst=argc>=2?spinexInstArg(ctx,argc,args,exc):nullptr;
    int track=inst?(int)JSValueToNumber(ctx,args[1],exc):-1;
    spine::TrackEntry* te=(inst&&track>=0)?inst->state()->getCurrent((size_t)track):nullptr;
    if(!te){setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,false));return out;}
    int animIdx=-1;
    if(te->getAnimation()&&inst->entry){
        auto it=inst->entry->animIndex.find(te->getAnimation());
        if(it!=inst->entry->animIndex.end())animIdx=it->second;
    }
    setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,true));
    setJsNumber(ctx,out,"animIdx",(double)animIdx);
    setJsNumber(ctx,out,"trackTime",te->getTrackTime());
    setJsNumber(ctx,out,"duration",te->getAnimation()?te->getAnimation()->getDuration():0);
    setJsProperty(ctx,out,"loop",JSValueMakeBoolean(ctx,te->getLoop()));
    setJsNumber(ctx,out,"timeScale",te->getTimeScale());
    return out;
}

// __spxIsDrawable(instId) -> bool. True only after the current native instance has emitted one
// complete quad snapshot; resource decode/upload readiness alone is not enough for UI sequencing.
JSValueRef cbSpinexIsDrawable(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    int inst=argc>0?(int)JSValueToNumber(ctx,args[0],exc):0;
    return JSValueMakeBoolean(ctx,inst>0&&gSpinexRt.isDrawable(inst));
}

JSValueRef cbSpinexSetTrackTime(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=argc>=3?spinexInstArg(ctx,argc,args,exc):nullptr;
    if(!inst)return JSValueMakeBoolean(ctx,false);
    int track=(int)JSValueToNumber(ctx,args[1],exc);
    spine::TrackEntry* te=track>=0?inst->state()->getCurrent((size_t)track):nullptr;
    if(!te)return JSValueMakeBoolean(ctx,false);
    te->setTrackTime((float)JSValueToNumber(ctx,args[2],exc));
    inst->primed=false;   // 显式改时间=显式改姿态，下一帧强制出帧
    return JSValueMakeBoolean(ctx,true);
}

JSValueRef cbSpinexSetTrackLoop(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=argc>=3?spinexInstArg(ctx,argc,args,exc):nullptr;
    if(!inst)return JSValueMakeBoolean(ctx,false);
    int track=(int)JSValueToNumber(ctx,args[1],exc);
    spine::TrackEntry* te=track>=0?inst->state()->getCurrent((size_t)track):nullptr;
    if(!te)return JSValueMakeBoolean(ctx,false);
    te->setLoop(JSValueToBoolean(ctx,args[2]));
    return JSValueMakeBoolean(ctx,true);
}

// ---- P3 clz 子类收编桥 ----
// __spxSetClockMode(instId,manual) → bool。manual=1:C++ 不再用墙钟推时间,只消费 __spxFeedTime*
// 喂进来的业务时间(clz 子类 super.update(s) 的时间闸门语义,见 spinex.h 文件头)。
JSValueRef cbSpinexSetClockMode(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<2)return JSValueMakeBoolean(ctx,false);
    return JSValueMakeBoolean(ctx,gSpinexRt.setClockMode((int)JSValueToNumber(ctx,args[0],exc),
        JSValueToNumber(ctx,args[1],exc)!=0));
}

// __spxFeedTime(instId,sec) → bool(单发,诊断/兜底用;常规路径走批量)。
JSValueRef cbSpinexFeedTime(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<2)return JSValueMakeBoolean(ctx,false);
    return JSValueMakeBoolean(ctx,gSpinexRt.feedTime((int)JSValueToNumber(ctx,args[0],exc),
        (float)JSValueToNumber(ctx,args[1],exc)));
}

// __spxFeedTimeBatch(Float32Array [instId,sec]×n, n) → 实际喂入条数。门面 tick 每帧一次批量下发,
JSValueRef cbSpinexFeedTimeBatch(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<2||!JSValueIsObject(ctx,args[0])||JSValueGetTypedArrayType(ctx,args[0],exc)!=kJSTypedArrayTypeFloat32Array)
        return JSValueMakeNumber(ctx,0);
    JSObjectRef o=JSValueToObject(ctx,args[0],exc);
    float* p=(float*)JSObjectGetTypedArrayBytesPtr(ctx,o,exc);
    size_t len=JSObjectGetTypedArrayLength(ctx,o,exc);
    int n=(int)JSValueToNumber(ctx,args[1],exc);
    if(!p||n<=0||(size_t)(n*2)>len)return JSValueMakeNumber(ctx,0);
    int fed=0;
    for(int i=0;i<n;i++)
        if(gSpinexRt.feedTime((int)p[i*2],p[i*2+1]))fed++;
    return JSValueMakeNumber(ctx,(double)fed);
}

// __spxSetSkeletonColor(instId,rgb888) → bool。spine-pixi `spine.tint` setter 同语义(只写 RGB)。
JSValueRef cbSpinexSetSkeletonColor(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<2)return JSValueMakeBoolean(ctx,false);
    double v=JSValueToNumber(ctx,args[1],exc);
    if(!(v>=0))v=0xffffff;
    return JSValueMakeBoolean(ctx,gSpinexRt.setSkeletonColor((int)JSValueToNumber(ctx,args[0],exc),
        (uint32_t)v&0xffffffu));
}

// ---- P2 slotObject 跟随桥 ----
// __spxSlotBind(instId,slotIdx,childNodeId[,follow=1]) → bool。绑定后 C++ 每帧把 slot 骨骼世界矩阵
// 写进子节点局部矩阵槽(T_SPINE_SLOT),可见性/alpha 走 EFX 合成因子通道,drawOrder 用
// marker quad(-nodeId)交错绘制。节点未 alive 时返回 false(JS tick 兜底下帧重试)。
JSValueRef cbSpinexSlotBind(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<3)return JSValueMakeBoolean(ctx,false);
    bool follow=argc>3?JSValueToBoolean(ctx,args[3]):true;
    return JSValueMakeBoolean(ctx,gSpinexRt.bindSlot((int)JSValueToNumber(ctx,args[0],exc),
        (int)JSValueToNumber(ctx,args[1],exc),(int)JSValueToNumber(ctx,args[2],exc),follow,&gSG));
}

JSValueRef cbSpinexSlotUnbind(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<2)return JSValueMakeBoolean(ctx,false);
    return JSValueMakeBoolean(ctx,gSpinexRt.unbindSlot((int)JSValueToNumber(ctx,args[0],exc),
        (int)JSValueToNumber(ctx,args[1],exc),&gSG));
}

// __spxDrainEvents(Float32Array out) → 条数。8 float/条：
// [instId, track, type(start=0/interrupt=1/end=2/dispose=3/complete=4/event=5), animIdx, eventIdx,
//  intVal, floatVal, stringIdx(-1=无；__spxString(idx) 取回字符串)]。
JSValueRef cbSpinexDrainEvents(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1||!JSValueIsObject(ctx,args[0])||JSValueGetTypedArrayType(ctx,args[0],exc)!=kJSTypedArrayTypeFloat32Array)
        return JSValueMakeNumber(ctx,0);
    JSObjectRef o=JSValueToObject(ctx,args[0],exc);
    float* out=(float*)JSObjectGetTypedArrayBytesPtr(ctx,o,exc);
    size_t len=JSObjectGetTypedArrayLength(ctx,o,exc);
    if(!out||len<(size_t)spinex::Runtime::EVENT_FLOATS)return JSValueMakeNumber(ctx,0);
    return JSValueMakeNumber(ctx,(double)gSpinexRt.drainEvents(out,(int)(len/spinex::Runtime::EVENT_FLOATS)));
}

JSValueRef cbSpinexString(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<1)return JSValueMakeNull(ctx);
    const std::string* s=gSpinexRt.stringAt((int)JSValueToNumber(ctx,args[0],exc));
    if(!s)return JSValueMakeNull(ctx);
    JSStringRef js=JSStringCreateWithUTF8CString(s->c_str());
    JSValueRef v=JSValueMakeString(ctx,js);JSStringRelease(js);
    return v;
}

// NativeSpine has no JS SlotMesh facade. Return the current spine-cpp pose bounds so Pixi's
// width/height setters retain their normal responsive-layout semantics.
JSValueRef cbSpinexLocalBounds(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    spinex::Runtime::LocalBounds bounds;
    if(argc<1||!gSpinexRt.localBounds((int)JSValueToNumber(ctx,args[0],exc),bounds)||!bounds.valid){
        setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,false));
        return out;
    }
    setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,true));
    setJsNumber(ctx,out,"x",bounds.x);setJsNumber(ctx,out,"y",bounds.y);
    setJsNumber(ctx,out,"width",bounds.width);setJsNumber(ctx,out,"height",bounds.height);
    return out;
}

JSValueRef cbSpinexSetSlotMeshScale(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    if(argc<4)return JSValueMakeBoolean(ctx,false);
    return JSValueMakeBoolean(ctx,gSpinexRt.setSlotMeshScale((int)JSValueToNumber(ctx,args[0],exc),
        (int)JSValueToNumber(ctx,args[1],exc),(float)JSValueToNumber(ctx,args[2],exc),
        (float)JSValueToNumber(ctx,args[3],exc),&gSG));
}

// __spxBoneWorldTransforms(instId) → Float32Array [a,b,c,d,worldX,worldY] per bone.
JSValueRef cbSpinexBoneWorldTransforms(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    spinex::Instance* inst=spinexInstArg(ctx,argc,args,exc);
    if(!inst)return JSValueMakeNull(ctx);
    const size_t count=inst->skeleton()->getBones().size()*6;
    JSObjectRef output=JSObjectMakeTypedArray(ctx,kJSTypedArrayTypeFloat32Array,count,exc);
    float* values=output?(float*)JSObjectGetTypedArrayBytesPtr(ctx,output,exc):nullptr;
    if((exc&&*exc)||!values||!gSpinexRt.boneWorldTransforms(inst->instId,values,count))return JSValueMakeNull(ctx);
    return output;
}

// __spxSlotBounds(instId,slotIdx) → current region/mesh AABB in skeleton-local coordinates.
// This restores spine-pixi's named SlotMesh hit contract without recreating its render meshes.
JSValueRef cbSpinexSlotBounds(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    spinex::Runtime::LocalBounds bounds;
    if(argc<2||!gSpinexRt.slotBounds((int)JSValueToNumber(ctx,args[0],exc),
        (int)JSValueToNumber(ctx,args[1],exc),bounds)||!bounds.valid){
        setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,false));
        return out;
    }
    setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,true));
    setJsNumber(ctx,out,"x",bounds.x);setJsNumber(ctx,out,"y",bounds.y);
    setJsNumber(ctx,out,"width",bounds.width);setJsNumber(ctx,out,"height",bounds.height);
    return out;
}

// __spxProbeQuads(instId) → {ok,quadCount,texNotReady,primed,sample:[前4个quad的三点xy]}（调试桥）。
JSValueRef cbSpinexProbeQuads(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t argc,const JSValueRef args[],JSValueRef* exc){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    spinex::Runtime::QuadProbe probe;
    if(argc<1||!gSpinexRt.probeQuads((int)JSValueToNumber(ctx,args[0],exc),probe)){
        setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,false));
        return out;
    }
    setJsProperty(ctx,out,"ok",JSValueMakeBoolean(ctx,true));
    setJsNumber(ctx,out,"quadCount",(double)probe.quadCount);
    setJsNumber(ctx,out,"texNotReady",(double)probe.texNotReady);
    setJsProperty(ctx,out,"primed",JSValueMakeBoolean(ctx,probe.primed));
    std::vector<JSValueRef> vals;vals.reserve((size_t)probe.sampleN*6);
    for(int i=0;i<probe.sampleN*6;i++)vals.push_back(JSValueMakeNumber(ctx,probe.sample[i]));
    setJsProperty(ctx,out,"sample",JSObjectMakeArray(ctx,vals.size(),vals.empty()?nullptr:vals.data(),exc));
    return out;
}

JSValueRef cbSpinexStats(JSContextRef ctx,JSObjectRef,JSObjectRef,size_t,const JSValueRef[],JSValueRef*){
    JSObjectRef out=JSObjectMake(ctx,nullptr,nullptr);
    setJsNumber(ctx,out,"instances",(double)gSpinexRt.instanceCount());
    setJsNumber(ctx,out,"entries",(double)gSpinexRt.store.size());
    setJsNumber(ctx,out,"full",(double)gSpinexRt.statFull_);
    setJsNumber(ctx,out,"frozen",(double)gSpinexRt.statFrozen_);
    setJsNumber(ctx,out,"notReady",(double)gSpinexRt.statNotReady_);
    setJsNumber(ctx,out,"emitted",(double)gSpinexRt.statEmitted_);
    setJsNumber(ctx,out,"truncated",(double)gSpinexRt.statTruncated_);
    setJsNumber(ctx,out,"eventsQueued",(double)gSpinexRt.eventCount());
    setJsNumber(ctx,out,"eventsDropped",(double)gSpinexRt.droppedEvents_);
    setJsNumber(ctx,out,"refs",(double)gSpinexRt.store.totalRefs());       // 泄漏监控:全部条目 JS 引用之和
    setJsNumber(ctx,out,"binds",(double)gSpinexRt.slotBindCount());        // slotObject 活跃绑定数
    setJsNumber(ctx,out,"manual",(double)gSpinexRt.manualCount());         // P3:manual clock 实例数(clz 子类)
    setJsNumber(ctx,out,"manualIdle",(double)gSpinexRt.statManualIdle_);   // P3:manual 未喂帧数(业务闸门关闭)
    setJsNumber(ctx,out,"clockOnly",(double)gSpinexRt.statClockOnly_);     // 屏外仅推 state(保 complete/event)
    return out;
}

void setFn(JSContextRef ctx, JSObjectRef g, const char* name, JSObjectCallAsFunctionCallback cb) {
    JSStringRef s = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx, g, s, JSObjectMakeFunctionWithCallback(ctx, s, cb), kJSPropertyAttributeNone, NULL);
    JSStringRelease(s);
}

bool gAnimAdvancedThisFrame = false;   // Phase D-1:本帧动画已在 ADVANCE_NATIVE 推进(draw 兜底判断)

void advanceNativeAnimationsForFrame(int root) {
    if (root <= 0) return;
    // drive 契约展开(native_animation_frame.h 语义:先发布 world 再驱动)。前置 compute 必须用
    // CanonicalMain——它是主帧序列 ADVANCE_NATIVE 的一环;旧签名缺省 QueryNoAck 会以查询身份
    // 消费全局队列,与 FINAL_COMPUTE 形成消费错位(D-1 首验 50 条 mk=4 mismatch 的来源)。
    gSG.compute(root, sg::CM_CANONICAL_MAIN);
    spinexFrameDrive();
    gAnimAdvancedThisFrame = true;
}

// __glReadPixels(x,y,w,h) -> ArrayBuffer(RGBA,GL 行序):pixi Extract 截图用,与 iOS 同契约。
// JS 侧先 flushPending 再调,读的是当前绑定 FBO(extract 已 bind 到目标 renderTexture)。
JSValueRef cbGlReadPixels(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef a[], JSValueRef* e) {
    if (argc < 4) return JSValueMakeNull(ctx);
    int x=(int)JSValueToNumber(ctx,a[0],e), y=(int)JSValueToNumber(ctx,a[1],e);
    int w=(int)JSValueToNumber(ctx,a[2],e), h=(int)JSValueToNumber(ctx,a[3],e);
    if (w<=0 || h<=0 || (long)w*h > 16384L*16384L) return JSValueMakeNull(ctx);
    size_t bytes = (size_t)w*h*4;
    unsigned char* out = (unsigned char*)malloc(bytes);
    if (!out) return JSValueMakeNull(ctx);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    return JSObjectMakeArrayBufferWithBytesNoCopy(ctx, out, bytes, [](void* p, void*){ free(p); }, nullptr, e);
}

// ---- 输入：NSView 鼠标 → JS __pointerEvent ----
void emitPointer(const char* type, double x, double y, int button, int buttons,
                 double pointerId, const char* pointerType, bool primary, double pressure) {
    if (!gCtx) return;
    JSObjectRef global = JSContextGetGlobalObject(gCtx);
    JSStringRef fn = JSStringCreateWithUTF8CString("__pointerEvent");
    JSValueRef v = JSObjectGetProperty(gCtx, global, fn, nullptr); JSStringRelease(fn);
    if (!JSValueIsObject(gCtx, v)) return;
    JSObjectRef f = JSValueToObject(gCtx, v, nullptr);
    if (!f || !JSObjectIsFunction(gCtx, f)) return;
    JSStringRef ts = JSStringCreateWithUTF8CString(type);
    JSStringRef ps = JSStringCreateWithUTF8CString(pointerType);
    JSValueRef args[9] = { JSValueMakeString(gCtx, ts), JSValueMakeNumber(gCtx, x), JSValueMakeNumber(gCtx, y),
                           JSValueMakeNumber(gCtx, button), JSValueMakeNumber(gCtx, buttons), JSValueMakeNumber(gCtx,pointerId),
                           JSValueMakeString(gCtx,ps), JSValueMakeBoolean(gCtx,primary), JSValueMakeNumber(gCtx,pressure<0?(buttons?0.5:0):pressure) };
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(gCtx, f, nullptr, 9, args, &exc);
    JSStringRelease(ts);
    JSStringRelease(ps);
    if (exc) { JSStringRef es = JSValueToStringCopy(gCtx, exc, nullptr); size_t n = JSStringGetMaximumUTF8CStringSize(es);
        std::vector<char> b(n); JSStringGetUTF8CString(es, b.data(), n); HOST_LOG("[game] __pointerEvent 异常: %s\n", b.data()); JSStringRelease(es); }
}

void emitVisibility(bool visible) {
    if (!gCtx) return;
    JSStringRef name=JSStringCreateWithUTF8CString("__nativeSetVisibility");
    JSValueRef value=JSObjectGetProperty(gCtx,JSContextGetGlobalObject(gCtx),name,nullptr);
    JSStringRelease(name);
    if (!JSValueIsObject(gCtx,value)) return;
    JSObjectRef fn=JSValueToObject(gCtx,value,nullptr);
    if (!fn||!JSObjectIsFunction(gCtx,fn)) return;
    JSValueRef arg=JSValueMakeBoolean(gCtx,visible),exc=nullptr;
    JSObjectCallAsFunction(gCtx,fn,nullptr,1,&arg,&exc);
    if (exc) { JSStringRef message=JSValueToStringCopy(gCtx,exc,nullptr);size_t n=JSStringGetMaximumUTF8CStringSize(message);std::vector<char> text(n);JSStringGetUTF8CString(message,text.data(),n);HOST_LOG("[game] visibility event failed: %s\n",text.data());JSStringRelease(message); }
}

// ---- 三端共用的绑定表 ----
namespace {
struct HostCommonBinding { const char* name; JSObjectCallAsFunctionCallback cb; };
const HostCommonBinding kHostCoreCommonBindings[] = {
    {"__sgAlloc", cbSgAlloc},
    {"__sgAllocBatch", cbSgAllocBatch},
    {"__sgCommands", cbSgCommands},
    {"__sgCommandsV2", cbSgCommandsV2},
    {"__sgFree", cbSgFree},
    {"__sgSetParent", cbSgSetParent},
    {"__sgRemove", cbSgRemove},
    {"__sgNativeParent", cbSgNativeParent},
    {"__sgNativeChildCount", cbSgNativeChildCount},
    {"__sgNativeChildAt", cbSgNativeChildAt},
    {"__sgNativeChildren", cbSgNativeChildren},
    {"__sgNativeSubtree", cbSgNativeSubtree},
    {"__sgCompute", cbSgCompute},
    {"__sgChainWorld", cbSgChainWorld},
    {"__sgAuthorityDump", cbSgAuthorityDump},
    {"__sgPublishSeal", cbSgPublishSeal},
    {"__sgDebtRollup", cbSgDebtRollup},
    {"__sgAdvanceAnimations", cbSgAdvanceAnimations},
    {"__sgUpdateBounds", cbSgUpdateBounds},
    {"__sgGetLocalBounds", cbSgGetLocalBounds},
    {"__sgHit", cbSgHit},
    {"__nativeNineSlice", cbNativeNineSlice},
    {"__glReadPixels", cbGlReadPixels},
    {"__nativeTiling", cbNativeTiling},
    {"__nativeBitmapGlyphs", cbNativeBitmapGlyphs},
    {"__nativeGraphicsMesh", cbNativeGraphicsMesh},
    {"__nativeTweenStart", cbNativeTweenStart},
    {"__nativeTweenStartEx", cbNativeTweenStartEx},
    {"__nativeTweenCancel", cbNativeTweenCancel},
    {"__nativeTweenPause", cbNativeTweenPause},
    {"__nativeTweenAdvance", cbNativeTweenAdvance},
    {"__nativeTweenAdvanceEx", cbNativeTweenAdvanceEx},
    {"__sgFlagRender", cbSgFlagRender},
    {"__sgNativeQuadHit", cbSgNativeQuadHit},
    {"__nativeFguiNode", cbNativeFguiNode},
    {"__nativeFguiNodesAll", cbNativeFguiNodesAll},
    {"__nativeFguiNodesByIds", cbNativeFguiNodesAll},
    {"__nativeFguiSyncRows", cbNativeFguiSyncRows},
    {"__nativeFguiChildren", cbNativeFguiChildren},
    {"__nativeFguiChildOrder", cbNativeFguiChildOrder},
    {"__nativeFguiDisplayChildCount", cbNativeFguiDisplayChildCount},
    {"__nativeFguiDisplayChildAt", cbNativeFguiDisplayChildAt},
    {"__nativeFguiDisplayChildIndex", cbNativeFguiDisplayChildIndex},
    {"__nativeFguiAdoptDisplayContainer", cbNativeFguiAdoptDisplayContainer},
    {"__nativeFguiReparent", cbNativeFguiReparent},
    {"__nativeFguiSetNodeSize", cbNativeFguiSetNodeSize},
    {"__nativeFguiSetNodePosition", cbNativeFguiSetNodePosition},
    {"__nativeFguiClearNodeRelations", cbNativeFguiClearNodeRelations},
    {"__nativeFguiNodeGeometry", cbNativeFguiNodeGeometry},
    {"__nativeFguiJournalEpoch", cbNativeFguiJournalEpoch},
    {"__nativeFguiSetNodeScale", cbNativeFguiSetNodeScale},
    {"__nativeFguiSetNodeRotation", cbNativeFguiSetNodeRotation},
    {"__nativeFguiSetNodePivot", cbNativeFguiSetNodePivot},
    {"__nativeFguiSetNodeAlpha", cbNativeFguiSetNodeAlpha},
    {"__nativeFguiSetNodeDisplayPosition", cbNativeFguiSetNodeDisplayPosition},
    {"__nativeFguiSetNodeDisplayScale", cbNativeFguiSetNodeDisplayScale},
    {"__nativeFguiSetNodeDisplayRotation", cbNativeFguiSetNodeDisplayRotation},
    {"__nativeFguiSetNodeVisible", cbNativeFguiSetNodeVisible},
    {"__nativeFguiGetMovieClip", cbNativeFguiGetMovieClip},
    {"__nativeFguiSetMovieClip", cbNativeFguiSetMovieClip},
    {"__nativeFguiTransitionAdvance", cbNativeFguiTransitionAdvance},
    {"__nativeFguiTransitionSignals", cbNativeFguiTransitionSignals},
    {"__nativeFguiTransitionVisibilitySignals", cbNativeFguiTransitionVisibilitySignals},
    {"__nativeFguiTransitionPropertySignals", cbNativeFguiTransitionPropertySignals},
    {"__nativeFguiTaskAdd", cbNativeFguiTaskAdd},
    {"__nativeFguiTaskCancel", cbNativeFguiTaskCancel},
    {"__nativeFguiTaskDrain", cbNativeFguiTaskDrain},
    {"__nativeFguiDestroy", cbNativeFguiDestroy},
    {"__nativeFguiNodeRoots", cbNativeFguiNodeRoots},
    {"__nativeFguiRuntimeStats", cbNativeFguiRuntimeStats},
    {"__nativeFguiCompatibilityIssues", cbNativeFguiCompatibilityIssues},
    {"__syncProgram", cbSyncProgram},
    {"__nowUs", cbNowUs},
    {"__log", cbLog},
    {"__nativeSpineRecordMain", cbNativeSpineRecordMain},
    {"__nativeSpineRecordCompatSkeleton", cbNativeSpineRecordCompatSkeleton},
    {"__spxRelease", cbSpinexRelease},
    {"__spxRetain", cbSpinexRetain},
    {"__spxCreate", cbSpinexCreate},
    {"__spxDestroy", cbSpinexDestroy},
    {"__spxSetAnimation", cbSpinexSetAnimation},
    {"__spxAddAnimation", cbSpinexAddAnimation},
    {"__spxSetEmptyAnimation", cbSpinexSetEmptyAnimation},
    {"__spxClearTrack", cbSpinexClearTrack},
    {"__spxClearTracks", cbSpinexClearTracks},
    {"__spxSetTimeScale", cbSpinexSetTimeScale},
    {"__spxSetTrackTimeScale", cbSpinexSetTrackTimeScale},
    {"__spxSetMix", cbSpinexSetMix},
    {"__spxSetDefaultMix", cbSpinexSetDefaultMix},
    {"__spxSetSkin", cbSpinexSetSkin},
    {"__spxSetToSetupPose", cbSpinexSetToSetupPose},
    {"__spxSetSlotsToSetupPose", cbSpinexSetSlotsToSetupPose},
    {"__spxSlotSetToSetupPose", cbSpinexSlotSetToSetupPose},
    {"__spxSlotSetColor", cbSpinexSlotSetColor},
    {"__spxUpdateCache", cbSpinexUpdateCache},
    {"__spxPhysicsTranslate", cbSpinexPhysicsTranslate},
    {"__spxPhysicsRotate", cbSpinexPhysicsRotate},
    {"__spxTrackInfo", cbSpinexTrackInfo},
    {"__spxIsDrawable", cbSpinexIsDrawable},
    {"__spxSetTrackTime", cbSpinexSetTrackTime},
    {"__spxSetTrackLoop", cbSpinexSetTrackLoop},
    {"__spxDrainEvents", cbSpinexDrainEvents},
    {"__spxString", cbSpinexString},
    {"__spxLocalBounds", cbSpinexLocalBounds},
    {"__spxBoneWorldTransforms", cbSpinexBoneWorldTransforms},
    {"__spxSlotBounds", cbSpinexSlotBounds},
    {"__spxSetSlotMeshScale", cbSpinexSetSlotMeshScale},
    {"__spxProbeQuads", cbSpinexProbeQuads},
    {"__spxStats", cbSpinexStats},
    {"__spxSlotBind", cbSpinexSlotBind},
    {"__spxSlotUnbind", cbSpinexSlotUnbind},
    {"__spxSetClockMode", cbSpinexSetClockMode},
    {"__spxFeedTime", cbSpinexFeedTime},
    {"__spxFeedTimeBatch", cbSpinexFeedTimeBatch},
    {"__spxSetSkeletonColor", cbSpinexSetSkeletonColor},
    {"__evalCode", cbEvalCode},
};
}  // namespace
void hostCoreRegisterCommonBindings(JSContextRef ctx, JSObjectRef global) {
    for (const HostCommonBinding& b : kHostCoreCommonBindings) setFn(ctx, global, b.name, b.cb);
}
