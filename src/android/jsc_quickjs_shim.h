// JSC→QuickJS 兼容层:让 mac/iOS 宿主(JavaScriptCore 绑定风格)的回调代码原样跑在 Android QuickJS 上。
#pragma once
// 覆盖 ios_app.mm/game_window.mm 实际用到的 JSC API 面(约 40 个函数),不是完整 JSC 实现。
//
// 内存模型(与 JSC 语义对齐的关键):
//   JSC 的 JSValueRef 由 GC 管理,绑定代码从不显式释放;QuickJS 的 JSValue 是显式引用计数。
//   本层用 arena 弥合:每次从 native 进入回调(trampoline/ShimScope)压一个 scope,期间创建的所有
//   ShimValue 槽登记在 arena,scope 弹出时统一 JS_FreeValue。borrowed 槽(回调参数)只登记不释放。
//   JSValueProtect 在槽上追加独立引用(JS_DupValue)并允许槽在 scope 结束后存活(timer 回调场景),
//   JSValueUnprotect 归还;计数归零且已脱离 arena 时槽体回收进池。
//   Release 时若仍有 ShimScope 在栈上(nativeDrawFrame 帧 scope 内 reload),JS 世界立即释放、结构体延到最外层 scope 弹出再 delete
//   (此前直接 delete,帧尾 ShimScope 析构会 popScope 一个已释放的上下文)。
// 同一张 API 面另有 V8 实现 jsc_v8_shim.h,由 jsc_shim.h 按 PIXI_JS_ENGINE_V8 二选一;两者末尾的 jsengine:: 接口同名同义。
#pragma once
#include <quickjs.h>
#include "quickjs_intl_polyfill.h"   // QuickJS 无 Intl 的补丁(上下文创建时求值)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct ShimValue { JSValue v; bool borrowed; bool detached; int protectCount; ShimValue* poolNext; };
struct OpaqueJSString { std::string s; };
struct OpaqueJSContext;
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

struct OpaqueJSContext {
    JSRuntime* rt = nullptr;
    JSContext* q = nullptr;   // Release 后置空:此后 popScope 只做簿记
    std::vector<ShimValue*> arena;
    std::vector<size_t> scopeMarks;
    ShimValue* pool = nullptr;
    JSValue taKindFn = JS_UNDEFINED;   // typed array 类型判定辅助(JS 侧一次性注册)
    std::vector<JSObjectCallAsFunctionCallback> callbacks;
    jsengine::UnhandledRejectionHandler rejectionHandler = nullptr;
    int liveScopes = 0;             // 栈上的 ShimScope/trampoline scope 数(不含常驻根 scope)
    bool releasePending = false;    // Release 时仍有 scope 在栈上:延到最外层 scope 弹出再 delete

    ShimValue* alloc(JSValue value, bool borrowed = false) {
        ShimValue* s = pool;
        if (s) pool = s->poolNext; else s = new ShimValue();
        s->v = value; s->borrowed = borrowed; s->detached = false; s->protectCount = 0; s->poolNext = nullptr;
        arena.push_back(s);
        return s;
    }
    void recycle(ShimValue* s) { s->poolNext = pool; pool = s; }
    void pushScope() { scopeMarks.push_back(arena.size()); liveScopes++; }
    // 返回 true 表示上下文已在此处销毁(delete this),调用方不得再触碰
    bool popScope() {
        liveScopes--;
        if (!q) {   // 已 Release:JS 值早已释放,只剩结构体等最后一个 scope 退出
            if (releasePending && liveScopes <= 0) { delete this; return true; }
            return false;
        }
        size_t mark = scopeMarks.empty() ? 0 : scopeMarks.back();
        if (!scopeMarks.empty()) scopeMarks.pop_back();
        for (size_t i = arena.size(); i > mark; --i) {
            ShimValue* s = arena[i - 1];
            if (!s->borrowed) JS_FreeValue(q, s->v);
            if (s->protectCount > 0) s->detached = true;   // protect 的引用继续持有,槽存活到 Unprotect
            else recycle(s);
        }
        arena.resize(mark);
        return false;
    }
};

// ---- 上下文生命周期 ----
static JSValue shimTaKindSetup(JSContext* q) {
    static const char* src =
        "(function(v){if(v instanceof ArrayBuffer)return 9;"
        "if(ArrayBuffer.isView(v)){var c=v.constructor;"
        "if(c===Int8Array)return 0;if(c===Int16Array)return 1;if(c===Int32Array)return 2;"
        "if(c===Uint8Array)return 3;if(c===Uint8ClampedArray)return 4;if(c===Uint16Array)return 5;"
        "if(c===Uint32Array)return 6;if(c===Float32Array)return 7;if(c===Float64Array)return 8;"
        "if(typeof BigInt64Array!=='undefined'&&c===BigInt64Array)return 11;"
        "if(typeof BigUint64Array!=='undefined'&&c===BigUint64Array)return 12;}"
        "return 10;})";
    return JS_Eval(q, src, strlen(src), "<shim-ta-kind>", JS_EVAL_TYPE_GLOBAL);
}
// Intl 补丁:在 QuickJS 上下文创建后、任何游戏脚本之前求值一次(源码在 quickjs_intl_polyfill.h,引擎无关)。
static void shimIntlSetup(JSContext* q) {
    JSValue r = JS_Eval(q, kQuickJsIntlPolyfill, strlen(kQuickJsIntlPolyfill), "<quickjs-intl-polyfill>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) { JSValue e = JS_GetException(q); JS_FreeValue(q, e); }
    JS_FreeValue(q, r);
}

inline JSGlobalContextRef JSGlobalContextCreate(JSClassRef) {
    OpaqueJSContext* c = new OpaqueJSContext();
    c->rt = JS_NewRuntime();
    // QuickJS 默认 C 栈上限 256KB:业务 bundle 的初始化递归会报 "stack overflow"(JSC 无此限制)。JS 跑在 UI 主线程(原生栈 ≥8MB),给 4MB。
    JS_SetMaxStackSize(c->rt, 4u * 1024u * 1024u);
    // QuickJS 的自动 GC = 堆比上次 GC 后再涨 50% 就在分配点上整堆 mark/sweep 一次,恰好落在结算/切场景这种分配最密的帧里。
    // 引用计数已即时回收非环垃圾,JS_RunGC 只清环:把自动阈值抬到 256 MiB 只做兜底,周期 GC 由宿主在轻帧上调度并计时
    // (native_runtime.cpp nativeDrawFrame 帧尾),让 GC 停顿可观测、可挑时机。
    JS_SetGCThreshold(c->rt, 256u * 1024u * 1024u);
    c->q = JS_NewContext(c->rt);
    JS_SetContextOpaque(c->q, c);
    c->taKindFn = shimTaKindSetup(c->q);
    shimIntlSetup(c->q);   // pixi 探测 Intl.Segmenter / vendor libraries use DateTimeFormat
    c->scopeMarks.push_back(0);   // 常驻根 scope(不计入 liveScopes):setup 阶段(注册期)创建的临时值挂这里,Release 时统一回收
    return c;
}
inline void JSGlobalContextRelease(JSGlobalContextRef c) {
    if (!c || !c->q) return;
    // 释放全部 JS 值(含仍在栈上的 scope 里的:那些 scope 之后弹出时只做簿记)
    for (size_t i = c->arena.size(); i > 0; --i) {
        ShimValue* s = c->arena[i - 1];
        if (!s->borrowed) JS_FreeValue(c->q, s->v);
        if (s->protectCount <= 0) c->recycle(s);
    }
    c->arena.clear();
    c->scopeMarks.clear();
    JS_FreeValue(c->q, c->taKindFn);
    JS_FreeContext(c->q);
    JS_FreeRuntime(c->rt);
    c->q = nullptr; c->rt = nullptr;
    while (c->pool) { ShimValue* n = c->pool->poolNext; delete c->pool; c->pool = n; }
    if (c->liveScopes > 0) c->releasePending = true;   // 帧 scope 内 reload:最外层 ShimScope 弹出时 delete
    else delete c;
}
inline OpaqueJSContext* shimCtxOf(JSContext* q) { return (OpaqueJSContext*)JS_GetContextOpaque(q); }

// native 主动进入 JS 的调用点(帧循环/timer/异步回调)用它包住,保证 arena 有界。
struct ShimScope {
    JSContextRef c;
    explicit ShimScope(JSContextRef ctx) : c(ctx) { if (c) c->pushScope(); }
    ~ShimScope() { if (c) { c->popScope(); c = nullptr; } }
};

// ---- 异常辅助:QuickJS 的 pending exception 转成 JSC 的 *exception 出参 ----
inline void shimCaptureException(JSContextRef c, JSValueRef* exception) {
    JSValue e = JS_GetException(c->q);
    if (exception) *exception = c->alloc(e);
    else JS_FreeValue(c->q, e);
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
inline JSValueRef JSValueMakeUndefined(JSContextRef c) { return c->alloc(JS_UNDEFINED); }
inline JSValueRef JSValueMakeNull(JSContextRef c) { return c->alloc(JS_NULL); }
inline JSValueRef JSValueMakeBoolean(JSContextRef c, bool b) { return c->alloc(JS_NewBool(c->q, b)); }
inline JSValueRef JSValueMakeNumber(JSContextRef c, double d) { return c->alloc(JS_NewFloat64(c->q, d)); }
inline JSValueRef JSValueMakeString(JSContextRef c, JSStringRef s) {
    return c->alloc(JS_NewStringLen(c->q, s ? s->s.data() : "", s ? s->s.size() : 0));
}

// ---- JSValue 判定/转换 ----
inline bool JSValueIsUndefined(JSContextRef c, JSValueRef v) { return v && JS_IsUndefined(v->v); }
inline bool JSValueIsNull(JSContextRef c, JSValueRef v) { return v && JS_IsNull(v->v); }
inline bool JSValueIsNumber(JSContextRef c, JSValueRef v) { return v && JS_IsNumber(v->v); }
inline bool JSValueIsString(JSContextRef c, JSValueRef v) { return v && JS_IsString(v->v); }
inline bool JSValueIsObject(JSContextRef c, JSValueRef v) { return v && JS_IsObject(v->v); }
inline bool JSValueIsArray(JSContextRef c, JSValueRef v) { return v && JS_IsArray(c->q, v->v) > 0; }
inline bool JSValueToBoolean(JSContextRef c, JSValueRef v) { return v && JS_ToBool(c->q, v->v) > 0; }
inline double JSValueToNumber(JSContextRef c, JSValueRef v, JSValueRef* exception) {
    if (!v) return NAN;
    double d = NAN;
    if (JS_ToFloat64(c->q, &d, v->v) < 0) { shimCaptureException(c, exception); return NAN; }
    return d;
}
inline JSObjectRef JSValueToObject(JSContextRef c, JSValueRef v, JSValueRef* exception) { return v; }
inline JSStringRef JSValueToStringCopy(JSContextRef c, JSValueRef v, JSValueRef* exception) {
    if (!v) return new OpaqueJSString{std::string()};
    size_t len = 0;
    const char* s = JS_ToCStringLen(c->q, &len, v->v);
    if (!s) { shimCaptureException(c, exception); return new OpaqueJSString{std::string()}; }
    OpaqueJSString* out = new OpaqueJSString{std::string(s, len)};
    JS_FreeCString(c->q, s);
    return out;
}
// JSC 同形:对象 → JSON 文本(exception 时返回 nullptr)。QuickJS 的 JS_JSONStringify 与 JSC 的 JSValueCreateJSONString 语义一致。
inline JSStringRef JSValueCreateJSONString(JSContextRef c, JSValueRef value, unsigned indent, JSValueRef* exception) {
    if (!value) return nullptr;
    JSValue json = JS_JSONStringify(c->q, value->v, JS_UNDEFINED, indent ? JS_NewInt32(c->q, (int32_t)indent) : JS_UNDEFINED);
    if (JS_IsException(json)) { shimCaptureException(c, exception); return nullptr; }
    if (JS_IsUndefined(json)) { JS_FreeValue(c->q, json); return nullptr; }
    const char* s = JS_ToCString(c->q, json);
    OpaqueJSString* out = new OpaqueJSString{std::string(s ? s : "")};
    if (s) JS_FreeCString(c->q, s);
    JS_FreeValue(c->q, json);
    return out;
}

// ---- 引用保持(timer 等长生命周期回调) ----
inline void JSValueProtect(JSContextRef c, JSValueRef v) {
    if (!v) return;
    JS_DupValue(c->q, v->v);
    v->protectCount++;
}
inline void JSValueUnprotect(JSContextRef c, JSValueRef v) {
    if (!v || v->protectCount <= 0) return;
    JS_FreeValue(c->q, v->v);
    if (--v->protectCount == 0 && v->detached) c->recycle(v);
}

// ---- 对象 ----
inline JSObjectRef JSObjectMake(JSContextRef c, JSClassRef, void*) { return c->alloc(JS_NewObject(c->q)); }
inline JSObjectRef JSObjectMakeArray(JSContextRef c, size_t argc, const JSValueRef argv[], JSValueRef* exception) {
    JSValue arr = JS_NewArray(c->q);
    for (size_t i = 0; i < argc; ++i)
        JS_SetPropertyUint32(c->q, arr, (uint32_t)i, JS_DupValue(c->q, argv[i]->v));
    return c->alloc(arr);
}
inline JSObjectRef JSContextGetGlobalObject(JSContextRef c) { return c->alloc(JS_GetGlobalObject(c->q)); }
inline void JSObjectSetProperty(JSContextRef c, JSObjectRef obj, JSStringRef name, JSValueRef value,
                                JSPropertyAttributes, JSValueRef* exception) {
    if (!obj || !name) return;
    JS_SetPropertyStr(c->q, obj->v, name->s.c_str(), JS_DupValue(c->q, value ? value->v : JS_UNDEFINED));
}
inline JSValueRef JSObjectGetProperty(JSContextRef c, JSObjectRef obj, JSStringRef name, JSValueRef* exception) {
    if (!obj || !name) return JSValueMakeUndefined(c);
    return c->alloc(JS_GetPropertyStr(c->q, obj->v, name->s.c_str()));
}
inline JSValueRef JSObjectGetPropertyAtIndex(JSContextRef c, JSObjectRef obj, unsigned index, JSValueRef* exception) {
    if (!obj) return JSValueMakeUndefined(c);
    return c->alloc(JS_GetPropertyUint32(c->q, obj->v, index));
}
inline void JSObjectSetPropertyAtIndex(JSContextRef c, JSObjectRef obj, unsigned index, JSValueRef value, JSValueRef* exception) {
    if (!obj) return;
    JS_SetPropertyUint32(c->q, obj->v, index, JS_DupValue(c->q, value ? value->v : JS_UNDEFINED));
}
inline bool JSObjectIsFunction(JSContextRef c, JSObjectRef obj) { return obj && JS_IsFunction(c->q, obj->v); }
inline JSValueRef JSObjectCallAsFunction(JSContextRef c, JSObjectRef fn, JSObjectRef thisObject,
                                         size_t argc, const JSValueRef argv[], JSValueRef* exception) {
    if (!fn) return nullptr;
    std::vector<JSValue> raw(argc);
    for (size_t i = 0; i < argc; ++i) raw[i] = argv[i]->v;
    JSValue r = JS_Call(c->q, fn->v, thisObject ? thisObject->v : JS_UNDEFINED, (int)argc, raw.data());
    if (JS_IsException(r)) { shimCaptureException(c, exception); return nullptr; }
    return c->alloc(r);
}

// ---- 回调注册(trampoline) ----
inline JSValue shimTrampoline(JSContext* q, JSValueConst this_val, int argc, JSValueConst* argv,
                              int magic, JSValue* func_data) {
    OpaqueJSContext* c = shimCtxOf(q);
    int32_t idx = -1;
    JS_ToInt32(q, &idx, func_data[0]);
    if (idx < 0 || (size_t)idx >= c->callbacks.size()) return JS_UNDEFINED;
    JSObjectCallAsFunctionCallback cb = c->callbacks[(size_t)idx];
    c->pushScope();
    std::vector<JSValueRef> args((size_t)argc);
    for (int i = 0; i < argc; ++i) args[(size_t)i] = c->alloc(argv[i], true);
    ShimValue* thisSlot = c->alloc(this_val, true);
    JSValueRef exc = nullptr;
    JSValueRef ret = cb(c, nullptr, thisSlot, (size_t)argc, argc ? args.data() : nullptr, &exc);
    JSValue out;
    if (exc) { out = JS_Throw(q, JS_DupValue(q, exc->v)); }
    else out = ret ? JS_DupValue(q, ret->v) : JS_UNDEFINED;
    c->popScope();
    return out;
}
inline JSObjectRef JSObjectMakeFunctionWithCallback(JSContextRef c, JSStringRef name,
                                                    JSObjectCallAsFunctionCallback cb) {
    c->callbacks.push_back(cb);
    JSValue data = JS_NewInt32(c->q, (int32_t)(c->callbacks.size() - 1));
    JSValue fn = JS_NewCFunctionData(c->q, shimTrampoline, 0, 0, 1, &data);
    JS_FreeValue(c->q, data);   // JS_NewCFunctionData 已 dup data
    return c->alloc(fn);
}

// ---- ArrayBuffer / TypedArray ----
struct ShimBufferDealloc { JSTypedArrayBytesDeallocator fn; void* ctx; };
inline void shimBufferFree(JSRuntime*, void* opaque, void* ptr) {
    ShimBufferDealloc* d = (ShimBufferDealloc*)opaque;
    if (d) { if (d->fn) d->fn(ptr, d->ctx); delete d; }
}
inline JSObjectRef JSObjectMakeArrayBufferWithBytesNoCopy(JSContextRef c, void* bytes, size_t byteLength,
                                                          JSTypedArrayBytesDeallocator deallocator, void* deallocatorContext,
                                                          JSValueRef* exception) {
    if (!deallocator)   // 共享常驻 buffer(__cmd/__sg 等):JS 侧不持有所有权,不挂 free
        return c->alloc(JS_NewArrayBuffer(c->q, (uint8_t*)bytes, byteLength, nullptr, nullptr, false));
    return c->alloc(JS_NewArrayBuffer(c->q, (uint8_t*)bytes, byteLength, shimBufferFree,
                                      new ShimBufferDealloc{deallocator, deallocatorContext}, false));
}
inline JSTypedArrayType JSValueGetTypedArrayType(JSContextRef c, JSValueRef v, JSValueRef* exception) {
    if (!v || !JS_IsObject(v->v)) return kJSTypedArrayTypeNone;
    JSValue arg = v->v;
    JSValue r = JS_Call(c->q, c->taKindFn, JS_UNDEFINED, 1, &arg);
    int32_t kind = kJSTypedArrayTypeNone;
    if (!JS_IsException(r)) JS_ToInt32(c->q, &kind, r);
    else JS_GetException(c->q);   // 吞掉,返回 None
    JS_FreeValue(c->q, r);
    return (JSTypedArrayType)kind;
}
inline JSObjectRef JSObjectMakeTypedArray(JSContextRef c, JSTypedArrayType type, size_t length, JSValueRef* exception) {
    JSTypedArrayEnum e;
    switch (type) {
        case kJSTypedArrayTypeInt8Array: e = JS_TYPED_ARRAY_INT8; break;
        case kJSTypedArrayTypeInt16Array: e = JS_TYPED_ARRAY_INT16; break;
        case kJSTypedArrayTypeInt32Array: e = JS_TYPED_ARRAY_INT32; break;
        case kJSTypedArrayTypeUint8Array: e = JS_TYPED_ARRAY_UINT8; break;
        case kJSTypedArrayTypeUint8ClampedArray: e = JS_TYPED_ARRAY_UINT8C; break;
        case kJSTypedArrayTypeUint16Array: e = JS_TYPED_ARRAY_UINT16; break;
        case kJSTypedArrayTypeUint32Array: e = JS_TYPED_ARRAY_UINT32; break;
        case kJSTypedArrayTypeFloat32Array: e = JS_TYPED_ARRAY_FLOAT32; break;
        case kJSTypedArrayTypeFloat64Array: e = JS_TYPED_ARRAY_FLOAT64; break;
        default: return c->alloc(JS_NULL);
    }
    JSValue len = JS_NewInt64(c->q, (int64_t)length);
    JSValue ta = JS_NewTypedArray(c->q, 1, &len, e);
    JS_FreeValue(c->q, len);
    if (JS_IsException(ta)) { shimCaptureException(c, exception); return c->alloc(JS_NULL); }
    return c->alloc(ta);
}
// 取 typed array 视图区间:返回底层指针+度量。QuickJS 的 GetTypedArrayBuffer 返回新引用的 ArrayBuffer,挂 arena。
inline bool shimTypedArrayRange(JSContextRef c, JSValueRef v, uint8_t** ptr, size_t* byteOff, size_t* byteLen, size_t* bpe) {
    if (!v) return false;
    size_t off = 0, len = 0, e = 0;
    JSValue ab = JS_GetTypedArrayBuffer(c->q, v->v, &off, &len, &e);
    if (JS_IsException(ab)) { JS_GetException(c->q); return false; }
    size_t total = 0;
    uint8_t* base = JS_GetArrayBuffer(c->q, &total, ab);
    c->alloc(ab);
    if (!base || off + len > total) return false;
    if (ptr) *ptr = base + off;
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
    size_t total = 0;
    uint8_t* p = JS_GetArrayBuffer(c->q, &total, obj->v);
    if (!p) JS_GetException(c->q);
    return p;
}
inline size_t JSObjectGetArrayBufferByteLength(JSContextRef c, JSObjectRef obj, JSValueRef* exception) {
    if (!obj) return 0;
    size_t total = 0;
    if (!JS_GetArrayBuffer(c->q, &total, obj->v)) { JS_GetException(c->q); return 0; }
    return total;
}

// ---- 脚本求值 ----
inline JSValueRef JSEvaluateScript(JSContextRef c, JSStringRef script, JSObjectRef thisObject,
                                   JSStringRef sourceURL, int startingLineNumber, JSValueRef* exception) {
    if (!script) return nullptr;
    JSValue r = JS_Eval(c->q, script->s.data(), script->s.size(),
                        sourceURL ? sourceURL->s.c_str() : "eval", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) { shimCaptureException(c, exception); return nullptr; }
    return c->alloc(r);
}

// ---- 引擎级能力(native_runtime.cpp 与 v8 shim 共用的同名接口) ----
namespace jsengine {
constexpr const char* kName = "quickjs";
constexpr bool kHostSchedulesGC = true;   // 自动 GC 阈值抬到 256 MiB 只做兜底,周期 GC 由宿主在轻帧上调度(nativeDrawFrame 帧尾)
inline const char* version() { return "bellard 2026-06"; }
// QuickJS 微任务显式排空(JSC/V8 在最外层调用返回前自动做,QuickJS 必须手动泵)
inline void pumpMicrotasks(JSContextRef c) {
    if (!c || !c->q) return;
    JSContext* pending;
    while (JS_ExecutePendingJob(c->rt, &pending) > 0) {}
}
inline void collectGarbage(JSContextRef c) { if (c && c->q) JS_RunGC(c->rt); }   // 全堆环垃圾回收
inline void setAutoGcThreshold(JSContextRef c) { if (c && c->q) JS_SetGCThreshold(c->rt, 256 * 1024); }   // 恢复 QuickJS 默认自动阈值(A/B)
inline JSValueRef makeStringUTF8(JSContextRef c, const char* bytes, size_t length) {   // 已校验的 UTF-8 → 字符串(__decodeScriptUTF8)
    return c->alloc(JS_NewStringLen(c->q, bytes, length));
}
// unhandled rejection:QuickJS 在 promise 无 handler 被拒绝的当下回调(is_handled=0),晚挂 handler 再回调一次 is_handled=1(忽略)
inline void setUnhandledRejectionHandler(JSContextRef c, UnhandledRejectionHandler h) {
    if (!c || !c->q) return;
    c->rejectionHandler = h;
    JS_SetHostPromiseRejectionTracker(c->rt, [](JSContext* qctx, JSValueConst, JSValueConst reason, int isHandled, void*) {
        if (isHandled) return;
        OpaqueJSContext* ctx = shimCtxOf(qctx);
        if (!ctx || !ctx->rejectionHandler) return;
        ShimScope scope(ctx);
        ctx->rejectionHandler(ctx, ctx->alloc(reason, true));
    }, nullptr);
}
}
