#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace nativeimage {
// Encoded PMA images already contain RGB * alpha. Preserve their bytes, including
// transparent additive color; straight-alpha images require exactly one multiply.
inline void copyPremultipliedRGBA(uint8_t* dst, const uint8_t* src, size_t pixels,
                                  bool sourcePremultiplied) {
    for (size_t i = 0; i < pixels; ++i) {
        const unsigned a = src[i * 4 + 3];
        for (size_t c = 0; c < 3; ++c)
            dst[i * 4 + c] = sourcePremultiplied ? src[i * 4 + c]
                : static_cast<uint8_t>((src[i * 4 + c] * a + 127) / 255);
        dst[i * 4 + 3] = static_cast<uint8_t>(a);
    }
}
inline std::string decodeCacheKey(const std::string& key, bool sourcePremultiplied) {
    return sourcePremultiplied ? "pma:" + key : key;
}
}
