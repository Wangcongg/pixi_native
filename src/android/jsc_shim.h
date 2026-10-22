// jsc_shim.h — Android 的 JSC C API 兼容层入口:按编译期引擎选择二选一。
//   PIXI_JS_ENGINE_V8      → jsc_v8_shim.h(预编译 V8,tools/fetch_v8_prebuilt.sh)
//   默认                    → jsc_quickjs_shim.h(QuickJS,FetchContent 源码编译)
// 两份实现暴露同一张 JSC 函数面 + 同名的 jsengine:: 引擎级接口(pumpMicrotasks/collectGarbage/makeStringUTF8/...),
// 宿主(native_runtime.cpp、host_core_common)只 include 这个头,不直接碰任一引擎的 API。
#pragma once
#if defined(PIXI_JS_ENGINE_V8)
#include "jsc_v8_shim.h"
#else
#include "jsc_quickjs_shim.h"
#endif
