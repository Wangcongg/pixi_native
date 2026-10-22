#pragma once
#include <cstddef>
namespace nativeimage {
// Decode authored PNG/JPEG bytes without color premultiplication. Caller owns malloc memory.
// Unsupported encodings and invalid images return null; there is no alpha-changing fallback.
unsigned char* decodeEncodedPMA(const void* data, size_t size, int& width, int& height);
}
