#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace native_script_utf8 {

// 严格 UTF-8 校验(与下方 Apple decode 同一套规则:拒绝过长编码、代理区、>U+10FFFF、截断序列)。
// Android 宿主用它守门后直接把字节交给 QuickJS 的 JS_NewStringLen(UTF-8 原生),不必先转 UTF-16。
inline bool isValidUtf8(const uint8_t* bytes, size_t count) {
    size_t i = 0;
    while (i < count) {
        uint32_t cp = bytes[i++], minimum = 0;
        unsigned extra = 0;
        if (cp < 0x80) continue;
        if (cp >= 0xc2 && cp <= 0xdf) { cp &= 0x1f; extra = 1; minimum = 0x80; }
        else if (cp >= 0xe0 && cp <= 0xef) { cp &= 0x0f; extra = 2; minimum = 0x800; }
        else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 0x07; extra = 3; minimum = 0x10000; }
        else return false;
        if (extra > count - i) return false;
        for (unsigned j = 0; j < extra; ++j) {
            const uint8_t next = bytes[i++];
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

} // namespace native_script_utf8

#if defined(__APPLE__)
#include <JavaScriptCore/JavaScriptCore.h>

namespace native_script_utf8 {

// Script-only fast path, not a replacement for the browser TextDecoder API. Invalid UTF-8
// returns null so the caller can retain its existing decoder policy. Length-based UTF-16
// creation preserves embedded NUL, BOM and supplementary characters (no C-string truncation).
inline JSValueRef decode(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argc,
                         const JSValueRef args[], JSValueRef* exception) {
    if (argc != 3 || !JSValueIsObject(ctx, args[0]) ||
        !JSValueIsNumber(ctx, args[1]) || !JSValueIsNumber(ctx, args[2]))
        return JSValueMakeNull(ctx);
    JSValueRef error = nullptr;
    const auto type = JSValueGetTypedArrayType(ctx, args[0], &error);
    if (error) { if (exception) *exception = error; return JSValueMakeUndefined(ctx); }
    if (type != kJSTypedArrayTypeArrayBuffer) return JSValueMakeNull(ctx);
    JSObjectRef buffer = JSValueToObject(ctx, args[0], &error);
    const double offset = JSValueToNumber(ctx, args[1], &error);
    const double length = JSValueToNumber(ctx, args[2], &error);
    const size_t capacity = JSObjectGetArrayBufferByteLength(ctx, buffer, &error);
    if (error) { if (exception) *exception = error; return JSValueMakeUndefined(ctx); }
    if (!std::isfinite(offset) || !std::isfinite(length) || offset < 0 || length < 0 ||
        std::floor(offset) != offset || std::floor(length) != length ||
        offset > static_cast<double>(capacity) || length > static_cast<double>(capacity) - offset)
        return JSValueMakeNull(ctx);
    const size_t begin = static_cast<size_t>(offset), count = static_cast<size_t>(length);
    const auto* bytes = static_cast<const uint8_t*>(JSObjectGetArrayBufferBytesPtr(ctx, buffer, &error));
    if (error) { if (exception) *exception = error; return JSValueMakeUndefined(ctx); }
    if (count && !bytes) return JSValueMakeNull(ctx);

    // Do not call JSC while using its borrowed byte pointer. All input conversion/validation
    // is above; the buffer is still rooted by the synchronous callback's arguments.
    std::vector<JSChar> text;
    text.reserve(count);
    size_t i = 0;
    while (i < count) {
        uint32_t cp = bytes[begin + i++], minimum = 0;
        unsigned extra = 0;
        if (cp < 0x80) { text.push_back(static_cast<JSChar>(cp)); continue; }
        if (cp >= 0xc2 && cp <= 0xdf) { cp &= 0x1f; extra = 1; minimum = 0x80; }
        else if (cp >= 0xe0 && cp <= 0xef) { cp &= 0x0f; extra = 2; minimum = 0x800; }
        else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 0x07; extra = 3; minimum = 0x10000; }
        else return JSValueMakeNull(ctx);
        if (extra > count - i) return JSValueMakeNull(ctx);
        for (unsigned j = 0; j < extra; ++j) {
            const uint8_t next = bytes[begin + i++];
            if ((next & 0xc0) != 0x80) return JSValueMakeNull(ctx);
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return JSValueMakeNull(ctx);
        if (cp <= 0xffff) text.push_back(static_cast<JSChar>(cp));
        else {
            cp -= 0x10000;
            text.push_back(static_cast<JSChar>(0xd800 + (cp >> 10)));
            text.push_back(static_cast<JSChar>(0xdc00 + (cp & 0x3ff)));
        }
    }
    JSStringRef string = JSStringCreateWithCharacters(text.data(), text.size());
    JSValueRef result = JSValueMakeString(ctx, string);
    JSStringRelease(string);
    return result;
}

} // namespace native_script_utf8
#endif  // __APPLE__
