#pragma once

#include <cmath>

namespace sg::scissor_bounds {

struct PixelRect {
    int x0;
    int y0;
    int x1;
    int y1;

    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
};

inline bool hasArea(float x0, float y0, float x1, float y1) {
    return x1 > x0 && y1 > y0;
}

// Enclose positive-area logical coverage in integer pixels. A zero-area Pixi mask must remain
// empty: independently flooring its minimum and ceiling its maximum would otherwise turn a
// fractional zero-width/zero-height mask into a one-pixel scissor leak.
inline PixelRect enclosing(float x0, float y0, float x1, float y1) {
    const int ix0 = static_cast<int>(std::floor(x0));
    const int iy0 = static_cast<int>(std::floor(y0));
    const int ix1 = x1 > x0 ? static_cast<int>(std::ceil(x1)) : ix0;
    const int iy1 = y1 > y0 ? static_cast<int>(std::ceil(y1)) : iy0;
    return {ix0, iy0, ix1, iy1};
}

}  // namespace sg::scissor_bounds
