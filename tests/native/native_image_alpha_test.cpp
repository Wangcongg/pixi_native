#include "common/native_image_alpha.h"
#include <array>
#include <cassert>

int main() {
    // A soft glow, an opaque pixel and zero-alpha additive RGB must survive PMA
    // decoding byte-for-byte. Multiplying the glow twice changes 74 to 21.
    const std::array<uint8_t,12> pma={74,74,74,74,255,128,32,255,20,10,0,0};
    std::array<uint8_t,12> out{};
    nativeimage::copyPremultipliedRGBA(out.data(),pma.data(),3,true);
    assert(out==pma);
    const std::array<uint8_t,12> straight={255,128,0,74,255,128,32,255,20,10,0,0};
    const std::array<uint8_t,12> expected={74,37,0,74,255,128,32,255,0,0,0,0};
    nativeimage::copyPremultipliedRGBA(out.data(),straight.data(),3,false);
    assert(out==expected);
    // Requests for one URL with different source-alpha interpretations cannot
    // share an in-flight decode or a cached GPU texture.
    assert(nativeimage::decodeCacheKey("url:a.png",false)=="url:a.png");
    assert(nativeimage::decodeCacheKey("url:a.png",true)!=nativeimage::decodeCacheKey("url:a.png",false));
}
