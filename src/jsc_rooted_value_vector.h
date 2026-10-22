#pragma once

// JSC C API:Apple 用系统 JavaScriptCore;Android 由 src/android/jsc_shim.h(QuickJS/V8 兼容层)提供同一套接口(host_core_common 三端共用)
#if __has_include(<JavaScriptCore/JavaScriptCore.h>)
#include <JavaScriptCore/JavaScriptCore.h>
#else
#include "android/jsc_shim.h"
#endif

#include <cstddef>
#include <vector>

namespace jscbridge {

// JavaScriptCore only discovers JSValueRefs held by JavaScript objects or the native stack.
// std::vector stores its elements on the C++ heap, so values accumulated there must be protected
// until the destination JS array/object owns them. This wrapper keeps that rooting balanced even
// when a bridge returns early after receiving a JavaScript exception.
class RootedValueVector {
public:
    explicit RootedValueVector(JSContextRef context) : context_(context) {}

    ~RootedValueVector() {
        clear();
    }

    void clear() noexcept {
        for (auto it = values_.rbegin(); it != values_.rend(); ++it) {
            if (*it) JSValueUnprotect(context_, *it);
        }
        values_.clear();
    }

    RootedValueVector(const RootedValueVector&) = delete;
    RootedValueVector& operator=(const RootedValueVector&) = delete;
    RootedValueVector(RootedValueVector&&) = delete;
    RootedValueVector& operator=(RootedValueVector&&) = delete;

    void reserve(std::size_t count) { values_.reserve(count); }

    void push_back(JSValueRef value) {
        // vector growth cannot run JSC GC. Protect immediately afterwards while value is still a
        // native-stack argument, before returning it to heap-only storage.
        values_.push_back(value);
        if (value) JSValueProtect(context_, value);
    }

    bool empty() const { return values_.empty(); }
    std::size_t size() const { return values_.size(); }
    const JSValueRef* data() const { return values_.data(); }
    JSValueRef operator[](std::size_t index) const { return values_[index]; }

private:
    JSContextRef context_;
    std::vector<JSValueRef> values_;
};

} // namespace jscbridge
