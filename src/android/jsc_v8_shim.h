// JSC→V8 兼容层:让 mac/iOS 宿主(JavaScriptCore 绑定风格)的回调代码原样跑在 Android V8 上。
#pragma once
// 与 jsc_quickjs_shim.h 同一张 API 面(约 40 个 JSC C 函数),由 android/jsc_shim.h 按 PIXI_JS_ENGINE_V8 二选一。
// V8 来源:cocos-engine-external 的预编译 libv8_monolith.a(tools/fetch_v8_prebuilt.sh 拉取,含 libplatform 与内嵌 snapshot)。
//
// 内存模型(与 JSC 语义对齐的关键):
//   JSC 的 JSValueRef 由 GC 管理,绑定代码从不显式释放;V8 的 Local 只在 HandleScope 内有效,Global 需显式 Reset。
//   本层沿用 quickjs shim 的 arena:每次从 native 进入 JS(ShimScope)或 JS 进入 native(trampoline)压一个 scope,
//   scope 自带 v8::HandleScope,期间创建的 ShimValue 槽直接存 Local(零开销),scope 弹出时随 HandleScope 一起失效并回收槽。
//   没有任何 scope 在栈上时(setupGame 注册期)分配的槽存 Global,Release 时统一 Reset(≙ quickjs shim 的常驻根 scope)。
//   JSValueProtect 把槽升级成 Global 并允许槽在 scope 结束后存活(timer 回调场景),JSValueUnprotect 归还;
//   计数归零且已脱离 arena 时槽体回收进池。
// 上下文模型:
//   进程只有一个 v8::Isolate(创建于第一次 JSGlobalContextCreate,所在线程即 JS 线程,Enter 一次不再 Exit);
//   每个 JSGlobalContextRef 是该 isolate 里的一个 v8::Context + 独立 MicrotaskQueue(kAuto:最外层调用返回时自动排空,
//   与 JSC 同;帧尾 jsengine::pumpMicrotasks 再显式 checkpoint 一次)。reload = 释放旧 Context、建新 Context;
//   旧 Context 的微任务队列随之丢弃,旧上下文注册的 native 函数经 alive 标记拒绝迟到调用。
//   Release 时若仍有 ShimScope 在栈上(nativeDrawFrame 帧 scope 内 reload),结构体延到最外层 scope 弹出再销毁。
#include <v8.h>
#include <libplatform/libplatform.h>
#include "quickjs_intl_polyfill.h"   // 无 Intl(或 toLocaleString 不分组)时的补丁,自探测安装
#include <android/log.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct OpaqueJSContext;
struct ShimValue {
    v8::Local<v8::Value> local;      // 非 persistent 槽:所属 scope 的 HandleScope 存活期内有效
    v8::Global<v8::Value> global;    // persistent 槽(根级分配 / protect 后)
    bool persistent = false;
    bool detached = false;           // protect 后脱离 arena,存活到 Unprotect
    int protectCount = 0;
    ShimValue* poolNext = nullptr;
};
struct OpaqueJSString { std::string s; };
typedef OpaqueJSContext* JSContextRef;
typedef OpaqueJSContext* JSGlobalContextRef;
typedef ShimValue* JSValueRef;
typedef ShimValue* JSObjectRef;
typedef OpaqueJSString* JSStringRef;
typedef void* JSClassRef;   // 仅作 JSObjectMake 第二参占位,始终传 nullptr

typedef unsigned JSPropertyAttributes;
enum {
    kJSPropertyAttributeNone = 0,
    kJSPropertyAttributeReadOnly = 1 << 1,
    kJSPropertyAttributeDontEnum = 1 << 2,
    kJSPropertyAttributeDontDelete = 1 << 3,
};
typedef enum {
    kJSTypedArrayTypeInt8Array = 0,
    kJSTypedArrayTypeInt16Array = 1,
    kJSTypedArrayTypeInt32Array = 2,
    kJSTypedArrayTypeUint8Array = 3,
    kJSTypedArrayTypeUint8ClampedArray = 4,
    kJSTypedArrayTypeUint16Array = 5,
    kJSTypedArrayTypeUint32Array = 6,
    kJSTypedArrayTypeFloat32Array = 7,
    kJSTypedArrayTypeFloat64Array = 8,
    kJSTypedArrayTypeArrayBuffer = 9,
    kJSTypedArrayTypeNone = 10,
    kJSTypedArrayTypeBigInt64Array = 11,
    kJSTypedArrayTypeBigUint64Array = 12,
} JSTypedArrayType;

typedef JSValueRef (*JSObjectCallAsFunctionCallback)(JSContextRef ctx, JSObjectRef function,
                                                     JSObjectRef thisObject, size_t argumentCount,
                                                     const JSValueRef arguments[], JSValueRef* exception);
typedef void (*JSTypedArrayBytesDeallocator)(void* bytes, void* deallocatorContext);

namespace jsengine { typedef void (*UnhandledRejectionHandler)(JSContextRef ctx, JSValueRef reason); }

#define SHIM_LOG(...) __android_log_print(ANDROID_LOG_INFO, "pixi_native", __VA_ARGS__)
#define SHIM_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "pixi_native", __VA_ARGS__)

struct ShimCallback {
    OpaqueJSContext* c; JSObjectCallAsFunctionCallback fn;
    std::string name;              // 注册名(排障:按名字计时,CpuProfiler 把所有 API 回调记在同一段 builtin 上分不开)
    uint64_t calls = 0; double us = 0;   // traceNativeCalls 打开时累计(含嵌套回 JS 的时间)
};

struct OpaqueJSContext {
    v8::Isolate* iso = nullptr;
    v8::Global<v8::Context> context;
    std::unique_ptr<v8::MicrotaskQueue> queue;
    std::vector<ShimValue*> arena;
    std::vector<size_t> scopeMarks;                  // 空 = 无 ShimScope 在栈上(根级:分配走 Global)
    ShimValue* pool = nullptr;
    std::vector<ShimCallback*> callbacks;            // 注册的 native 函数(随上下文销毁)
    std::unordered_set<ShimValue*> detachedSlots;    // protect 后脱离 arena 的槽,Release 时兜底 Reset
    std::unordered_map<std::string, v8::Global<v8::String>> names;   // 属性名驻留缓存(internalized string)
    struct PendingRejection { v8::Global<v8::Promise> promise; v8::Global<v8::Value> reason; };
    std::vector<PendingRejection> rejections;        // 微任务 checkpoint 结束时仍无 handler 的 rejection 才上报
    jsengine::UnhandledRejectionHandler rejectionHandler = nullptr;
    bool alive = true;                               // Release 后 false:trampoline 拒绝旧上下文的迟到调用
    bool releasePending = false;                     // Release 时仍有 scope 在栈上:延到最外层 scope 弹出再 delete

    v8::Local<v8::Context> ctx() { return context.Get(iso); }
    v8::Local<v8::Value> get(ShimValue* s) { return s->persistent ? s->global.Get(iso) : s->local; }
    ShimValue* alloc(v8::Local<v8::Value> value) {
        ShimValue* s = pool;
        if (s) pool = s->poolNext; else s = new ShimValue();
        s->detached = false; s->protectCount = 0; s->poolNext = nullptr;
        if (scopeMarks.empty()) { s->persistent = true; s->global.Reset(iso, value); s->local = v8::Local<v8::Value>(); }
        else { s->persistent = false; s->local = value; }
        arena.push_back(s);
        return s;
    }
    void recycle(ShimValue* s) { s->global.Reset(); s->persistent = false; s->local = v8::Local<v8::Value>(); s->poolNext = pool; pool = s; }
    v8::Local<v8::String> name(const std::string& s) {
        auto it = names.find(s);
        if (it != names.end()) return it->second.Get(iso);
        v8::Local<v8::String> str;
        if (!v8::String::NewFromUtf8(iso, s.data(), v8::NewStringType::kInternalized, (int)s.size()).ToLocal(&str))
            return v8::String::Empty(iso);
        if (names.size() < 8192) names[s].Reset(iso, str);
        return str;
    }
    void pushScope() { scopeMarks.push_back(arena.size()); }
    // 返回 true 表示上下文已在此处销毁(delete this),调用方不得再触碰
    bool popScope() {
        size_t mark = scopeMarks.empty() ? 0 : scopeMarks.back();
        if (!scopeMarks.empty()) scopeMarks.pop_back();
        for (size_t i = arena.size(); i > mark; --i) {
            ShimValue* s = arena[i - 1];
            if (s->protectCount > 0) { s->detached = true; detachedSlots.insert(s); }   // protect 的引用继续持有,槽存活到 Unprotect
            else recycle(s);
        }
        arena.resize(mark);
        if (releasePending && scopeMarks.empty()) { teardown(); return true; }
        return false;
    }
    void teardown();   // 最终销毁(Release 时无 scope 在栈上则立即,否则延后)
};

// ---- 进程级 V8 运行时 ----
struct ShimV8Runtime {
    std::unique_ptr<v8::Platform> platform;
    v8::ArrayBuffer::Allocator* allocator = nullptr;
    v8::Isolate* iso = nullptr;
    bool fullGcPending = false;   // 旧上下文 teardown 后请求一次全量 GC(下次 pumpMicrotasks 时做,届时旧 HandleScope 已关闭)
    bool traceNativeCalls = false;   // [jsprof] 开启时:trampoline 按注册名累计调用次数与墙钟
};
inline double shimNowUs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3; }
inline ShimV8Runtime& shimRuntime() { static ShimV8Runtime rt; return rt; }

static void shimPromiseReject(v8::PromiseRejectMessage msg);
static void shimMessage(v8::Local<v8::Message> message, v8::Local<v8::Value>);
inline v8::Isolate* shimIsolate() {
    ShimV8Runtime& rt = shimRuntime();
    if (rt.iso) return rt.iso;
    rt.platform = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(rt.platform.get());
    // --no-flush-bytecode:V8 会回收久未执行函数的字节码,游戏切场景回到冷代码时要重新编译(Cocos 同此设置);
    // --stack-size(KB):QuickJS 曾需 4MB 才过业务 bundle 的初始化递归,JS 跑在 UI 主线程(原生栈 8MB),给同样的 4MB。
    static const char kFlags[] = "--no-flush-bytecode --stack-size=4096";
    v8::V8::SetFlagsFromString(kFlags, sizeof(kFlags) - 1);
    v8::V8::Initialize();
    rt.allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = rt.allocator;
    rt.iso = v8::Isolate::New(params);
    rt.iso->Enter();
    rt.iso->SetCaptureStackTraceForUncaughtExceptions(true, 32);
    rt.iso->SetPromiseRejectCallback(shimPromiseReject);
    rt.iso->AddMessageListener(shimMessage);
    SHIM_LOG("[js] V8 %s 初始化完成", v8::V8::GetVersion());
    return rt.iso;
}

// native 主动进入 JS 的调用点(帧循环/timer/异步回调)用它包住,保证 arena 有界;JS→native 的 trampoline 也用它
// (trampoline 时 V8 已进入被调函数的上下文,enterContext=false 省掉一次 Context Enter/Exit)。
struct ShimScope {
    OpaqueJSContext* c;
    std::optional<v8::HandleScope> hs;
    std::optional<v8::Context::Scope> cs;
    explicit ShimScope(JSContextRef ctx, bool enterContext = true) : c(ctx) {
        if (!c) return;
        hs.emplace(c->iso);
        if (enterContext) cs.emplace(c->ctx());
        c->pushScope();
    }
    ~ShimScope() {
        if (!c) return;
        // HandleScope/Context::Scope 里的句柄只依赖 isolate(进程级常驻),上下文结构体先销毁也无妨
        c->popScope();
        c = nullptr;
    }
};
// 每个 API 函数入口:无 ShimScope 在栈上时(根级)临时开 HandleScope 并进入本上下文(返回值已存 Global,不受 HandleScope 关闭影响);
// 有 scope 时什么都不做(返回的 Local 属于外层 ShimScope 的 HandleScope)。
struct ShimApiScope {
    std::optional<v8::HandleScope> hs;
    std::optional<v8::Context::Scope> cs;
    explicit ShimApiScope(OpaqueJSContext* c) {
        if (c->scopeMarks.empty()) { hs.emplace(c->iso); cs.emplace(c->ctx()); }
    }
};

// ---- 异常辅助:TryCatch 捕获转成 JSC 的 *exception 出参(无出参则吞掉,与 quickjs shim 同) ----
inline void shimCaptureException(OpaqueJSContext* c, v8::TryCatch& tc, JSValueRef* exception) {
    if (!tc.HasCaught()) return;
    if (exception) {
        v8::Local<v8::Value> e = tc.Exception();
        *exception = c->alloc(e.IsEmpty() ? v8::Local<v8::Value>(v8::Undefined(c->iso)) : e);
    }
    tc.Reset();
}
inline std::string shimUtf8(v8::Isolate* iso, v8::Local<v8::Value> v) {
    if (v.IsEmpty()) return std::string();
    v8::String::Utf8Value u(iso, v);
    return *u ? std::string(*u, (size_t)u.length()) : std::string();
}

// ---- 上下文生命周期 ----
static void shimMicrotasksCompleted(v8::Isolate*, void* data);
inline void shimIntlSetup(OpaqueJSContext* c) {
    v8::HandleScope hs(c->iso);
    v8::Local<v8::Context> ctx = c->ctx();
    v8::Context::Scope cs(ctx);
    v8::TryCatch tc(c->iso);
    v8::Local<v8::String> src;
    if (!v8::String::NewFromUtf8(c->iso, kQuickJsIntlPolyfill).ToLocal(&src)) return;
    v8::ScriptOrigin origin(c->iso, c->name("<intl-polyfill>"));
    v8::Local<v8::Script> script;
    if (v8::Script::Compile(ctx, src, &origin).ToLocal(&script)) (void)script->Run(ctx);
    if (tc.HasCaught()) SHIM_LOGE("[js] intl polyfill 求值异常: %s", shimUtf8(c->iso, tc.Exception()).c_str());
}
inline JSGlobalContextRef JSGlobalContextCreate(JSClassRef) {
    OpaqueJSContext* c = new OpaqueJSContext();
    c->iso = shimIsolate();
    v8::HandleScope hs(c->iso);
    c->queue = v8::MicrotaskQueue::New(c->iso, v8::MicrotasksPolicy::kAuto);
    c->queue->AddMicrotasksCompletedCallback(shimMicrotasksCompleted, c);
    v8::Local<v8::Context> ctx = v8::Context::New(c->iso, nullptr, v8::MaybeLocal<v8::ObjectTemplate>(),
                                                  v8::MaybeLocal<v8::Value>(), v8::DeserializeInternalFieldsCallback(),
                                                  c->queue.get());
    ctx->SetAlignedPointerInEmbedderData(1, c);   // PromiseRejectCallback 是 isolate 级的,靠它找回所属上下文
    c->context.Reset(c->iso, ctx);
    shimIntlSetup(c);   // pixi 探测 Intl.Segmenter / vendor libraries use DateTimeFormat;V8 自带 ICU 时补丁自动不装
    return c;
}
inline void OpaqueJSContext::teardown() {
    for (ShimCallback* cb : callbacks) delete cb;
    callbacks.clear();
    queue.reset();   // 旧上下文残留的微任务随队列丢弃
    for (ShimValue* s : arena) { s->global.Reset(); delete s; }   // 根级残留槽(Release 时无 scope 则是全部)
    arena.clear();
    while (pool) { ShimValue* n = pool->poolNext; delete pool; pool = n; }
    shimRuntime().fullGcPending = true;
    delete this;
}
inline void JSGlobalContextRelease(JSGlobalContextRef c) {
    if (!c || !c->alive) return;
    c->alive = false;
    // 先清所有槽的 GC 根:arena 里的(含 scope 内的 Local 槽——所属 scope 之后弹出时只会 recycle)、protect 后脱离的。
    // 脱离槽的内存不回收:宿主可能还握着指针(Release 前应已 Unprotect,漏掉的只是一个小结构体的泄漏,不做 UAF 赌注)。
    for (ShimValue* s : c->arena) { s->protectCount = 0; s->global.Reset(); s->persistent = false; }
    for (ShimValue* s : c->detachedSlots) { s->protectCount = 0; s->global.Reset(); s->persistent = false; }
    c->detachedSlots.clear();
    c->rejections.clear();
    c->names.clear();
    {
        v8::HandleScope hs(c->iso);
        c->ctx()->SetAlignedPointerInEmbedderData(1, nullptr);
    }
    c->context.Reset();
    if (c->scopeMarks.empty()) c->teardown();
    else c->releasePending = true;   // 帧 scope 内 reload:最外层 ShimScope 弹出时销毁
}

// ---- 微任务 / 未处理 rejection ----
static void shimMicrotasksCompleted(v8::Isolate* iso, void* data) {
    OpaqueJSContext* c = (OpaqueJSContext*)data;
    if (!c->alive || c->rejections.empty()) return;
    std::vector<OpaqueJSContext::PendingRejection> list;
    list.swap(c->rejections);
    if (!c->rejectionHandler) return;
    ShimScope scope(c);
    for (auto& r : list) c->rejectionHandler(c, c->alloc(r.reason.Get(iso)));
}
static void shimPromiseReject(v8::PromiseRejectMessage msg) {
    v8::Local<v8::Promise> p = msg.GetPromise();
    v8::Isolate* iso = p->GetIsolate();
    v8::HandleScope hs(iso);
    v8::Local<v8::Context> ctx;
    if (!p->GetCreationContext().ToLocal(&ctx)) return;
    OpaqueJSContext* c = (OpaqueJSContext*)ctx->GetAlignedPointerFromEmbedderData(1);
    if (!c || !c->alive) return;
    if (msg.GetEvent() == v8::kPromiseRejectWithNoHandler) {
        OpaqueJSContext::PendingRejection r;
        r.promise.Reset(iso, p);
        r.reason.Reset(iso, msg.GetValue().IsEmpty() ? v8::Local<v8::Value>(v8::Undefined(iso)) : msg.GetValue());
        c->rejections.push_back(std::move(r));
    } else if (msg.GetEvent() == v8::kPromiseHandlerAddedAfterReject) {
        for (size_t i = 0; i < c->rejections.size(); ++i)
            if (c->rejections[i].promise == p) { c->rejections.erase(c->rejections.begin() + (long)i); break; }
    }
}
// kAuto 排空微任务时的未捕获异常不经过任何 TryCatch,只到这里(QuickJS 版 ExecutePendingJob 循环遇异常会静默停泵)
static void shimMessage(v8::Local<v8::Message> message, v8::Local<v8::Value> error) {
    v8::Isolate* iso = message->GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    std::string where = shimUtf8(iso, message->GetScriptResourceName());
    int line = ctx.IsEmpty() ? 0 : message->GetLineNumber(ctx).FromMaybe(0);
    std::string text = shimUtf8(iso, error), stack;
    if (!ctx.IsEmpty() && !error.IsEmpty() && error->IsObject()) {
        v8::Local<v8::Value> sv;
        if (error.As<v8::Object>()->Get(ctx, v8::String::NewFromUtf8Literal(iso, "stack")).ToLocal(&sv) && sv->IsString()) stack = shimUtf8(iso, sv);
    }
    SHIM_LOGE("[js] [uncaught] %s:%d %s%s%s", where.c_str(), line, text.c_str(), stack.empty() ? "" : "\n", stack.c_str());
}

// ---- JSString ----
inline JSStringRef JSStringCreateWithUTF8CString(const char* s) { return new OpaqueJSString{std::string(s ? s : "")}; }
inline void JSStringRelease(JSStringRef s) { delete s; }
inline size_t JSStringGetMaximumUTF8CStringSize(JSStringRef s) { return s ? s->s.size() + 1 : 1; }
inline size_t JSStringGetUTF8CString(JSStringRef s, char* buffer, size_t bufferSize) {
    if (!buffer || !bufferSize) return 0;
    size_t n = s ? s->s.size() : 0;
    if (n >= bufferSize) n = bufferSize - 1;
    if (s && n) memcpy(buffer, s->s.data(), n);
    buffer[n] = 0;
    return n + 1;
}
inline size_t JSStringGetLength(JSStringRef s) { return s ? s->s.size() : 0; }   // 近似:UTF8 字节数,现有绑定未用于 UTF16 语义

// ---- JSValue 构造 ----
inline JSValueRef JSValueMakeUndefined(JSContextRef c) { ShimApiScope api(c); return c->alloc(v8::Undefined(c->iso)); }
inline JSValueRef JSValueMakeNull(JSContextRef c) { ShimApiScope api(c); return c->alloc(v8::Null(c->iso)); }
inline JSValueRef JSValueMakeBoolean(JSContextRef c, bool b) { ShimApiScope api(c); return c->alloc(v8::Boolean::New(c->iso, b)); }
inline JSValueRef JSValueMakeNumber(JSContextRef c, double d) { ShimApiScope api(c); return c->alloc(v8::Number::New(c->iso, d)); }
inline JSValueRef JSValueMakeString(JSContextRef c, JSStringRef s) {
    ShimApiScope api(c);
    v8::Local<v8::String> str;
    if (!s || !v8::String::NewFromUtf8(c->iso, s->s.data(), v8::NewStringType::kNormal, (int)s->s.size()).ToLocal(&str))
        str = v8::String::Empty(c->iso);
    return c->alloc(str);
}

// ---- JSValue 判定/转换 ----
inline bool JSValueIsUndefined(JSContextRef c, JSValueRef v) { ShimApiScope api(c); return v && c->get(v)->IsUndefined(); }
inline bool JSValueIsNull(JSContextRef c, JSValueRef v) { ShimApiScope api(c); return v && c->get(v)->IsNull(); }
inline bool JSValueIsNumber(JSContextRef c, JSValueRef v) { ShimApiScope api(c); return v && c->get(v)->IsNumber(); }
inline bool JSValueIsString(JSContextRef c, JSValueRef v) { ShimApiScope api(c); return v && c->get(v)->IsString(); }
inline bool JSValueIsObject(JSContextRef c, JSValueRef v) { ShimApiScope api(c); return v && c->get(v)->IsObject(); }
inline bool JSValueIsArray(JSContextRef c, JSValueRef v) { ShimApiScope api(c); return v && c->get(v)->IsArray(); }
inline bool JSValueToBoolean(JSContextRef c, JSValueRef v) { ShimApiScope api(c); return v && c->get(v)->BooleanValue(c->iso); }
inline double JSValueToNumber(JSContextRef c, JSValueRef v, JSValueRef* exception) {
    if (!v) return NAN;
    ShimApiScope api(c);
    v8::Local<v8::Value> x = c->get(v);
    if (x->IsNumber()) return x.As<v8::Number>()->Value();   // 热路径:native 函数的数值参数,免 TryCatch
    v8::TryCatch tc(c->iso);
    double d = NAN;
    if (!x->NumberValue(c->ctx()).To(&d)) { shimCaptureException(c, tc, exception); return NAN; }
    return d;
}
inline JSObjectRef JSValueToObject(JSContextRef c, JSValueRef v, JSValueRef* exception) { return v; }
inline JSStringRef JSValueToStringCopy(JSContextRef c, JSValueRef v, JSValueRef* exception) {
    if (!v) return new OpaqueJSString{std::string()};
    ShimApiScope api(c);
    v8::Local<v8::Value> x = c->get(v);
    if (x->IsString()) return new OpaqueJSString{shimUtf8(c->iso, x)};
    v8::TryCatch tc(c->iso);
    v8::Local<v8::String> s;
    if (!x->ToString(c->ctx()).ToLocal(&s)) { shimCaptureException(c, tc, exception); return new OpaqueJSString{std::string()}; }
    return new OpaqueJSString{shimUtf8(c->iso, s)};
}
// JSC 同形:对象 → JSON 文本(不可序列化/exception 时返回 nullptr)
inline JSStringRef JSValueCreateJSONString(JSContextRef c, JSValueRef value, unsigned indent, JSValueRef* exception) {
    if (!value) return nullptr;
    ShimApiScope api(c);
    v8::Local<v8::Value> x = c->get(value);
    if (x->IsUndefined() || x->IsFunction() || x->IsSymbol()) return nullptr;   // JSON.stringify → undefined
    v8::TryCatch tc(c->iso);
    v8::Local<v8::String> gap;
    if (indent) (void)v8::String::NewFromUtf8(c->iso, std::string(indent > 10 ? 10 : indent, ' ').c_str()).ToLocal(&gap);
    v8::Local<v8::String> out;
    if (!v8::JSON::Stringify(c->ctx(), x, gap).ToLocal(&out)) { shimCaptureException(c, tc, exception); return nullptr; }
    return new OpaqueJSString{shimUtf8(c->iso, out)};
}

// ---- 引用保持(timer 等长生命周期回调) ----
inline void JSValueProtect(JSContextRef c, JSValueRef v) {
    if (!v) return;
    if (!v->persistent) { v->global.Reset(c->iso, v->local); v->persistent = true; }
    v->protectCount++;
}
inline void JSValueUnprotect(JSContextRef c, JSValueRef v) {
    if (!v || v->protectCount <= 0) return;
    if (--v->protectCount == 0 && v->detached) { c->detachedSlots.erase(v); c->recycle(v); }
}

// ---- 对象 ----
inline JSObjectRef JSObjectMake(JSContextRef c, JSClassRef, void*) { ShimApiScope api(c); return c->alloc(v8::Object::New(c->iso)); }
inline JSObjectRef JSObjectMakeArray(JSContextRef c, size_t argc, const JSValueRef argv[], JSValueRef* exception) {
    ShimApiScope api(c);
    std::vector<v8::Local<v8::Value>> items(argc);
    for (size_t i = 0; i < argc; ++i) items[i] = argv[i] ? c->get(argv[i]) : v8::Local<v8::Value>(v8::Undefined(c->iso));
    return c->alloc(v8::Array::New(c->iso, items.data(), argc));
}
inline JSObjectRef JSContextGetGlobalObject(JSContextRef c) { ShimApiScope api(c); return c->alloc(c->ctx()->Global()); }
inline void JSObjectSetProperty(JSContextRef c, JSObjectRef obj, JSStringRef name, JSValueRef value,
                                JSPropertyAttributes attrs, JSValueRef* exception) {
    if (!obj || !name) return;
    ShimApiScope api(c);
    v8::Local<v8::Value> o = c->get(obj);
    if (!o->IsObject()) return;
    v8::Local<v8::Value> v = value ? c->get(value) : v8::Local<v8::Value>(v8::Undefined(c->iso));
    v8::TryCatch tc(c->iso);
    v8::Maybe<bool> ok = v8::Nothing<bool>();
    if (attrs) {   // JSC 语义:带属性 = 定义(只读/不可枚举/不可删),与 iOS 一致
        int pa = v8::None;
        if (attrs & kJSPropertyAttributeReadOnly) pa |= v8::ReadOnly;
        if (attrs & kJSPropertyAttributeDontEnum) pa |= v8::DontEnum;
        if (attrs & kJSPropertyAttributeDontDelete) pa |= v8::DontDelete;
        ok = o.As<v8::Object>()->DefineOwnProperty(c->ctx(), c->name(name->s), v, (v8::PropertyAttribute)pa);
    } else {
        ok = o.As<v8::Object>()->Set(c->ctx(), c->name(name->s), v);
    }
    if (ok.IsNothing()) shimCaptureException(c, tc, exception);
}
inline JSValueRef JSObjectGetProperty(JSContextRef c, JSObjectRef obj, JSStringRef name, JSValueRef* exception) {
    if (!obj || !name) return JSValueMakeUndefined(c);
    ShimApiScope api(c);
    v8::Local<v8::Value> o = c->get(obj);
    if (!o->IsObject()) return c->alloc(v8::Undefined(c->iso));
    v8::TryCatch tc(c->iso);
    v8::Local<v8::Value> r;
    if (!o.As<v8::Object>()->Get(c->ctx(), c->name(name->s)).ToLocal(&r)) { shimCaptureException(c, tc, exception); return c->alloc(v8::Undefined(c->iso)); }
    return c->alloc(r);
}
inline JSValueRef JSObjectGetPropertyAtIndex(JSContextRef c, JSObjectRef obj, unsigned index, JSValueRef* exception) {
    if (!obj) return JSValueMakeUndefined(c);
    ShimApiScope api(c);
    v8::Local<v8::Value> o = c->get(obj);
    if (!o->IsObject()) return c->alloc(v8::Undefined(c->iso));
    v8::TryCatch tc(c->iso);
    v8::Local<v8::Value> r;
    if (!o.As<v8::Object>()->Get(c->ctx(), (uint32_t)index).ToLocal(&r)) { shimCaptureException(c, tc, exception); return c->alloc(v8::Undefined(c->iso)); }
    return c->alloc(r);
}
inline void JSObjectSetPropertyAtIndex(JSContextRef c, JSObjectRef obj, unsigned index, JSValueRef value, JSValueRef* exception) {
    if (!obj) return;
    ShimApiScope api(c);
    v8::Local<v8::Value> o = c->get(obj);
    if (!o->IsObject()) return;
    v8::TryCatch tc(c->iso);
    if (o.As<v8::Object>()->Set(c->ctx(), (uint32_t)index, value ? c->get(value) : v8::Local<v8::Value>(v8::Undefined(c->iso))).IsNothing())
        shimCaptureException(c, tc, exception);
}
inline bool JSObjectIsFunction(JSContextRef c, JSObjectRef obj) { ShimApiScope api(c); return obj && c->get(obj)->IsFunction(); }
inline JSValueRef JSObjectCallAsFunction(JSContextRef c, JSObjectRef fn, JSObjectRef thisObject,
                                         size_t argc, const JSValueRef argv[], JSValueRef* exception) {
    if (!fn) return nullptr;
    ShimApiScope api(c);
    v8::Local<v8::Value> f = c->get(fn);
    if (!f->IsFunction()) {
        if (exception) *exception = c->alloc(v8::Exception::TypeError(c->name("JSObjectCallAsFunction: not a function")));
        return nullptr;
    }
    v8::Local<v8::Value> stackArgs[8];
    std::vector<v8::Local<v8::Value>> heapArgs;
    v8::Local<v8::Value>* args = stackArgs;
    if (argc > 8) { heapArgs.resize(argc); args = heapArgs.data(); }
    for (size_t i = 0; i < argc; ++i) args[i] = argv[i] ? c->get(argv[i]) : v8::Local<v8::Value>(v8::Undefined(c->iso));
    v8::TryCatch tc(c->iso);
    v8::Local<v8::Value> r;
    v8::Local<v8::Value> recv = thisObject ? c->get(thisObject) : v8::Local<v8::Value>(v8::Undefined(c->iso));
    if (!f.As<v8::Function>()->Call(c->ctx(), recv, (int)argc, args).ToLocal(&r)) { shimCaptureException(c, tc, exception); return nullptr; }
    return c->alloc(r);
}

// ---- 回调注册(trampoline) ----
inline void shimTrampoline(const v8::FunctionCallbackInfo<v8::Value>& info) {
    ShimCallback* cb = static_cast<ShimCallback*>(v8::External::Cast(*info.Data())->Value());
    OpaqueJSContext* c = cb->c;
    if (!c->alive) return;   // 旧上下文的迟到调用
    const double t0 = shimRuntime().traceNativeCalls ? shimNowUs() : 0;
    ShimScope scope(c, /*enterContext=*/false);
    const int argc = info.Length();
    ShimValue* stackArgs[8];
    std::vector<ShimValue*> heapArgs;
    ShimValue** args = stackArgs;
    if (argc > 8) { heapArgs.resize((size_t)argc); args = heapArgs.data(); }
    for (int i = 0; i < argc; ++i) args[i] = c->alloc(info[i]);
    ShimValue* thisSlot = c->alloc(info.This());
    JSValueRef exc = nullptr;
    JSValueRef ret = cb->fn(c, nullptr, thisSlot, (size_t)argc, argc ? args : nullptr, &exc);
    if (exc) info.GetIsolate()->ThrowException(c->get(exc));
    else if (ret) info.GetReturnValue().Set(c->get(ret));
    if (t0) { cb->calls++; cb->us += shimNowUs() - t0; }
}
inline JSObjectRef JSObjectMakeFunctionWithCallback(JSContextRef c, JSStringRef name,
                                                    JSObjectCallAsFunctionCallback cb) {
    ShimApiScope api(c);
    ShimCallback* data = new ShimCallback{c, cb, name ? name->s : std::string("?")};
    c->callbacks.push_back(data);
    v8::Local<v8::Function> fn;
    if (!v8::Function::New(c->ctx(), shimTrampoline, v8::External::New(c->iso, data), 0, v8::ConstructorBehavior::kThrow).ToLocal(&fn))
        return c->alloc(v8::Undefined(c->iso));
    if (name) fn->SetName(c->name(name->s));
    return c->alloc(fn);
}

// ---- ArrayBuffer / TypedArray ----
struct ShimBufferDealloc { JSTypedArrayBytesDeallocator fn; void* ctx; };
inline void shimBackingStoreDeleter(void* data, size_t, void* opaque) {
    ShimBufferDealloc* d = (ShimBufferDealloc*)opaque;
    if (d) { if (d->fn) d->fn(data, d->ctx); delete d; }
}
inline JSObjectRef JSObjectMakeArrayBufferWithBytesNoCopy(JSContextRef c, void* bytes, size_t byteLength,
                                                          JSTypedArrayBytesDeallocator deallocator, void* deallocatorContext,
                                                          JSValueRef* exception) {
    ShimApiScope api(c);
    if (!bytes || !byteLength) return c->alloc(v8::ArrayBuffer::New(c->iso, 0));
    // 共享常驻 buffer(__cmd/__sg 等,deallocator 为空):JS 侧不持有所有权;否则释放时回调 deallocator。
    // 注意:V8 开 sandbox 时外部内存不能做 backing store(ApiCheck 直接 abort),预编译库须是 v8_enable_sandbox=false。
    std::unique_ptr<v8::BackingStore> bs = deallocator
        ? v8::ArrayBuffer::NewBackingStore(bytes, byteLength, shimBackingStoreDeleter, new ShimBufferDealloc{deallocator, deallocatorContext})
        : v8::ArrayBuffer::NewBackingStore(bytes, byteLength, v8::BackingStore::EmptyDeleter, nullptr);
    return c->alloc(v8::ArrayBuffer::New(c->iso, std::shared_ptr<v8::BackingStore>(std::move(bs))));
}
inline JSTypedArrayType JSValueGetTypedArrayType(JSContextRef c, JSValueRef v, JSValueRef* exception) {
    if (!v) return kJSTypedArrayTypeNone;
    ShimApiScope api(c);
    v8::Local<v8::Value> x = c->get(v);
    if (x->IsArrayBuffer()) return kJSTypedArrayTypeArrayBuffer;
    if (!x->IsTypedArray()) return kJSTypedArrayTypeNone;
    if (x->IsInt8Array()) return kJSTypedArrayTypeInt8Array;
    if (x->IsInt16Array()) return kJSTypedArrayTypeInt16Array;
    if (x->IsInt32Array()) return kJSTypedArrayTypeInt32Array;
    if (x->IsUint8Array()) return kJSTypedArrayTypeUint8Array;
    if (x->IsUint8ClampedArray()) return kJSTypedArrayTypeUint8ClampedArray;
    if (x->IsUint16Array()) return kJSTypedArrayTypeUint16Array;
    if (x->IsUint32Array()) return kJSTypedArrayTypeUint32Array;
    if (x->IsFloat32Array()) return kJSTypedArrayTypeFloat32Array;
    if (x->IsFloat64Array()) return kJSTypedArrayTypeFloat64Array;
    if (x->IsBigInt64Array()) return kJSTypedArrayTypeBigInt64Array;
    if (x->IsBigUint64Array()) return kJSTypedArrayTypeBigUint64Array;
    return kJSTypedArrayTypeNone;
}
inline JSObjectRef JSObjectMakeTypedArray(JSContextRef c, JSTypedArrayType type, size_t length, JSValueRef* exception) {
    ShimApiScope api(c);
    size_t bpe;
    switch (type) {
        case kJSTypedArrayTypeInt8Array: case kJSTypedArrayTypeUint8Array: case kJSTypedArrayTypeUint8ClampedArray: bpe = 1; break;
        case kJSTypedArrayTypeInt16Array: case kJSTypedArrayTypeUint16Array: bpe = 2; break;
        case kJSTypedArrayTypeInt32Array: case kJSTypedArrayTypeUint32Array: case kJSTypedArrayTypeFloat32Array: bpe = 4; break;
        case kJSTypedArrayTypeFloat64Array: bpe = 8; break;
        default: return c->alloc(v8::Null(c->iso));
    }
    v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(c->iso, length * bpe);
    v8::Local<v8::Value> ta;
    switch (type) {
        case kJSTypedArrayTypeInt8Array: ta = v8::Int8Array::New(ab, 0, length); break;
        case kJSTypedArrayTypeInt16Array: ta = v8::Int16Array::New(ab, 0, length); break;
        case kJSTypedArrayTypeInt32Array: ta = v8::Int32Array::New(ab, 0, length); break;
        case kJSTypedArrayTypeUint8Array: ta = v8::Uint8Array::New(ab, 0, length); break;
        case kJSTypedArrayTypeUint8ClampedArray: ta = v8::Uint8ClampedArray::New(ab, 0, length); break;
        case kJSTypedArrayTypeUint16Array: ta = v8::Uint16Array::New(ab, 0, length); break;
        case kJSTypedArrayTypeUint32Array: ta = v8::Uint32Array::New(ab, 0, length); break;
        case kJSTypedArrayTypeFloat32Array: ta = v8::Float32Array::New(ab, 0, length); break;
        default: ta = v8::Float64Array::New(ab, 0, length); break;
    }
    return c->alloc(ta);
}
// 取 typed array 视图区间:返回底层指针+度量(指针指向视图起点,与 quickjs shim / JSC 一致)。
inline bool shimTypedArrayRange(JSContextRef c, JSValueRef v, uint8_t** ptr, size_t* byteOff, size_t* byteLen, size_t* bpe) {
    if (!v) return false;
    ShimApiScope api(c);
    v8::Local<v8::Value> x = c->get(v);
    if (!x->IsArrayBufferView()) return false;
    v8::Local<v8::ArrayBufferView> view = x.As<v8::ArrayBufferView>();
    v8::Local<v8::ArrayBuffer> ab = view->Buffer();   // 堆内小 typed array 在此物化为堆外 backing store,之后指针稳定
    uint8_t* base = (uint8_t*)ab->Data();
    const size_t off = view->ByteOffset(), len = view->ByteLength(), total = ab->ByteLength();
    if ((!base && len) || off + len > total) return false;
    size_t e = 1;
    if (x->IsTypedArray()) {
        const size_t n = x.As<v8::TypedArray>()->Length();
        e = n ? len / n : (x->IsFloat64Array() || x->IsBigInt64Array() || x->IsBigUint64Array() ? 8
                         : (x->IsInt32Array() || x->IsUint32Array() || x->IsFloat32Array()) ? 4
                         : (x->IsInt16Array() || x->IsUint16Array()) ? 2 : 1);
    }
    if (ptr) *ptr = base ? base + off : nullptr;
    if (byteOff) *byteOff = off;
    if (byteLen) *byteLen = len;
    if (bpe) *bpe = e ? e : 1;
    return true;
}
inline void* JSObjectGetTypedArrayBytesPtr(JSContextRef c, JSObjectRef obj, JSValueRef* exception) {
    uint8_t* p = nullptr;
    return shimTypedArrayRange(c, obj, &p, nullptr, nullptr, nullptr) ? p : nullptr;
}
inline size_t JSObjectGetTypedArrayLength(JSContextRef c, JSObjectRef obj, JSValueRef* exception) {
    size_t len = 0, bpe = 1;
    return shimTypedArrayRange(c, obj, nullptr, nullptr, &len, &bpe) ? len / bpe : 0;
}
inline size_t JSObjectGetTypedArrayByteLength(JSContextRef c, JSObjectRef obj, JSValueRef* exception) {
    size_t len = 0;
    return shimTypedArrayRange(c, obj, nullptr, nullptr, &len, nullptr) ? len : 0;
}
inline size_t JSObjectGetTypedArrayByteOffset(JSContextRef c, JSObjectRef obj, JSValueRef* exception) {
    size_t off = 0;
    return shimTypedArrayRange(c, obj, nullptr, &off, nullptr, nullptr) ? off : 0;
}
inline void* JSObjectGetArrayBufferBytesPtr(JSContextRef c, JSObjectRef obj, JSValueRef* exception) {
    if (!obj) return nullptr;
    ShimApiScope api(c);
    v8::Local<v8::Value> x = c->get(obj);
    if (!x->IsArrayBuffer()) return nullptr;
    return x.As<v8::ArrayBuffer>()->Data();
}
inline size_t JSObjectGetArrayBufferByteLength(JSContextRef c, JSObjectRef obj, JSValueRef* exception) {
    if (!obj) return 0;
    ShimApiScope api(c);
    v8::Local<v8::Value> x = c->get(obj);
    if (!x->IsArrayBuffer()) return 0;
    return x.As<v8::ArrayBuffer>()->ByteLength();
}

// ---- 脚本求值 ----
inline JSValueRef JSEvaluateScript(JSContextRef c, JSStringRef script, JSObjectRef thisObject,
                                   JSStringRef sourceURL, int startingLineNumber, JSValueRef* exception) {
    if (!script) return nullptr;
    ShimApiScope api(c);
    v8::TryCatch tc(c->iso);
    v8::Local<v8::String> src;
    if (!v8::String::NewFromUtf8(c->iso, script->s.data(), v8::NewStringType::kNormal, (int)script->s.size()).ToLocal(&src)) {
        if (exception) *exception = c->alloc(v8::Exception::RangeError(c->name("JSEvaluateScript: source too large")));
        return nullptr;
    }
    v8::Local<v8::String> url;
    if (!v8::String::NewFromUtf8(c->iso, sourceURL ? sourceURL->s.c_str() : "eval").ToLocal(&url)) url = c->name("eval");
    v8::ScriptOrigin origin(c->iso, url, startingLineNumber > 1 ? startingLineNumber - 1 : 0);
    v8::Local<v8::Script> compiled;
    if (!v8::Script::Compile(c->ctx(), src, &origin).ToLocal(&compiled)) {
        // 编译期异常(SyntaxError)的位置只在 Message 里,Error 对象的 stack 不带:补一条日志方便定位
        if (tc.HasCaught() && !tc.Message().IsEmpty())
            SHIM_LOGE("[js] 编译失败 %s:%d %s", sourceURL ? sourceURL->s.c_str() : "eval",
                      tc.Message()->GetLineNumber(c->ctx()).FromMaybe(0), shimUtf8(c->iso, tc.Exception()).c_str());
        shimCaptureException(c, tc, exception);
        return nullptr;
    }
    v8::Local<v8::Value> r;
    if (!compiled->Run(c->ctx()).ToLocal(&r)) { shimCaptureException(c, tc, exception); return nullptr; }
    return c->alloc(r);
}

// ---- 引擎级能力(native_runtime.cpp 与 quickjs shim 共用的同名接口) ----
namespace jsengine {
constexpr const char* kName = "v8";
constexpr bool kHostSchedulesGC = false;   // V8 分代/增量 GC 自管,宿主不做帧尾全堆 GC 调度
inline const char* version() { return v8::V8::GetVersion(); }
// 显式排空本上下文的微任务(kAuto 已在最外层调用返回时自动做;帧尾再兜底一次,并顺带处理旧上下文销毁后的全量 GC)
inline void pumpMicrotasks(JSContextRef c) {
    if (!c || !c->alive) return;
    ShimApiScope api(c);
    c->queue->PerformCheckpoint(c->iso);
    ShimV8Runtime& rt = shimRuntime();
    if (rt.fullGcPending) { rt.fullGcPending = false; c->iso->LowMemoryNotification(); }
}
inline void collectGarbage(JSContextRef c) { if (c && c->alive) c->iso->LowMemoryNotification(); }   // 全量 GC(退后台时)
inline void setAutoGcThreshold(JSContextRef) {}   // QuickJS 专用(恢复默认自动阈值),V8 无对应
inline JSValueRef makeStringUTF8(JSContextRef c, const char* bytes, size_t length) {   // 已校验的 UTF-8 → 字符串(__decodeScriptUTF8)
    ShimApiScope api(c);
    v8::Local<v8::String> s;
    if (!v8::String::NewFromUtf8(c->iso, bytes, v8::NewStringType::kNormal, (int)length).ToLocal(&s)) return c->alloc(v8::Null(c->iso));
    return c->alloc(s);
}
inline void setUnhandledRejectionHandler(JSContextRef c, UnhandledRejectionHandler h) { if (c) c->rejectionHandler = h; }
}
