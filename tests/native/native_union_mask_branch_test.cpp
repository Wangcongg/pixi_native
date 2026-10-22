#include "cmdbuf/union_mask_branch.h"

#include <cassert>
#include <iostream>
#include <vector>

namespace {

struct Rect {
    float x0;
    float y0;
    float x1;
    float y1;
};

}  // namespace

int main() {
    const std::vector<Rect> columns = {
        {0.0f, 0.0f, 100.0f, 300.0f},
        {100.0f, 0.0f, 200.0f, 300.0f},
        {200.0f, 0.0f, 300.0f, 300.0f},
    };

    // The old origin-only rule sees x=100 inside both columns and keeps the first match.
    assert(sg::union_mask_branch::nearestBranch(columns, 100.0f) == 0);

    // A per-symbol clip is authoritative and moves the witness inside the second reel.
    float x = sg::union_mask_branch::witnessX(
        100.0f, true, 100.0f, 200.0f, true, 100.0f, 200.0f);
    assert(x == 150.0f);
    assert(sg::union_mask_branch::nearestBranch(columns, x) == 1);

    // Plain symbol containers have no own clip; their render-subtree center is the fallback.
    x = sg::union_mask_branch::witnessX(
        200.0f, false, 0.0f, 0.0f, true, 200.0f, 300.0f);
    assert(x == 250.0f);
    assert(sg::union_mask_branch::nearestBranch(columns, x) == 2);

    // Construction-time empty nodes still retain the safe legacy origin fallback.
    x = sg::union_mask_branch::witnessX(
        42.0f, false, 0.0f, 0.0f, false, 0.0f, 0.0f);
    assert(x == 42.0f);
    assert(sg::union_mask_branch::nearestBranch(columns, x) == 0);

    std::cout << "native_union_mask_branch_test: ok\n";
    return 0;
}
