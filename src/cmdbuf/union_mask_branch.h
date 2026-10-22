#pragma once

#include <cstddef>

namespace sg::union_mask_branch {

// A transform origin is not a reliable branch witness: reel symbols are commonly positioned at
// the exact seam shared by two adjacent column masks. Prefer a point inside the child's own clip,
// then a point inside its rendered subtree. The caller supplies coordinates in the same space as
// the union rectangles (including any output transform used by an offscreen pass).
inline float witnessX(float originX,
                      bool hasChildClip, float childClipX0, float childClipX1,
                      bool hasRenderBounds, float renderBoundsX0, float renderBoundsX1) {
    if (hasChildClip) return (childClipX0 + childClipX1) * 0.5f;
    if (hasRenderBounds) return (renderBoundsX0 + renderBoundsX1) * 0.5f;
    return originX;
}

template <class RectRange>
inline std::size_t nearestBranch(const RectRange& rects, float x) {
    std::size_t best = 0;
    float bestDistance = 1e30f;
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const auto& rect = rects[i];
        const float distance = x < rect.x0 ? rect.x0 - x : (x > rect.x1 ? x - rect.x1 : 0.0f);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

}  // namespace sg::union_mask_branch
