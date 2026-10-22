#include "cmdbuf/native_animation_frame.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}

void setPosition(sg::SceneGraph& graph, int node, float x, float y) {
    float* slot = graph.buf + static_cast<size_t>(node) * sg::STRIDE;
    slot[sg::X] = x;
    slot[sg::Y] = y;
    graph.markLocalInputChanged(node);
}

float worldX(const sg::SceneGraph& graph, int node) {
    return graph.buf[static_cast<size_t>(node) * sg::STRIDE + sg::WTX];
}

float worldY(const sg::SceneGraph& graph, int node) {
    return graph.buf[static_cast<size_t>(node) * sg::STRIDE + sg::WTY];
}

float worldAlpha(const sg::SceneGraph& graph, int node) {
    return graph.buf[static_cast<size_t>(node) * sg::STRIDE + sg::WALPHA];
}

}  // namespace

int main() {
    std::vector<float> shared(static_cast<size_t>(sg::SHARED_FLOATS), 0.0f);
    sg::SceneGraph graph;
    graph.init(shared.data());

    const int root = graph.alloc();
    const int oldParent = graph.alloc();
    const int newParent = graph.alloc();
    const int animated = graph.alloc();
    assert(root > 0 && oldParent > 0 && newParent > 0 && animated > 0);

    graph.setParent(root, 0, -1);
    graph.setParent(oldParent, root, -1);
    graph.setParent(newParent, root, -1);
    graph.setParent(animated, oldParent, -1);
    setPosition(graph, root, 3.0f, 4.0f);
    setPosition(graph, oldParent, 100.0f, 30.0f);
    setPosition(graph, newParent, 700.0f, 400.0f);
    setPosition(graph, animated, 10.0f, 5.0f);
    graph.compute(root, sg::CM_CANONICAL_MAIN);   // 隐式单参重载已删除(四审 P2)
    assert(near(worldX(graph, animated), 113.0f));
    assert(near(worldY(graph, animated), 39.0f));

    // Model an effect-pool reuse: the Spine node is reparented and positioned for a new symbol,
    // but its cached world transform still points at the previous symbol until publication.
    graph.setParent(animated, newParent, -1);
    setPosition(graph, animated, 20.0f, 8.0f);
    assert(near(worldX(graph, animated), 113.0f));
    assert(near(worldY(graph, animated), 39.0f));
    // The old world matrix may remain cached until publication, but it must not be drawable.
    // This closes every bypass path, rather than relying on each host/render entry remembering
    // to perform the pre-drive compute in exactly the right order.
    assert(near(worldAlpha(graph, animated), 0.0f));

    bool drove = false;
    sg::driveAnimationsWithPublishedTransforms(graph, root, [&] {
        drove = true;
        // The animation runtime must see the new parent on its first drive, never the pooled
        // object's previous world transform.
        assert(near(worldX(graph, animated), 723.0f));
        assert(near(worldY(graph, animated), 412.0f));
        assert(near(worldAlpha(graph, animated), 1.0f));

        // Model slot-follow/animation output. The ordinary post-drive compute consumes this.
        setPosition(graph, animated, 25.0f, 9.0f);
    });
    assert(drove);

    graph.compute(root, sg::CM_CANONICAL_MAIN);   // 隐式单参重载已删除(四审 P2)
    assert(near(worldX(graph, animated), 728.0f));
    assert(near(worldY(graph, animated), 413.0f));

    std::cout << "native_animation_frame_test: ok\n";
    return 0;
}
