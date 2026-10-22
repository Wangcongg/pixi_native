#include "cmdbuf/scenegraph.h"

#include <cassert>
#include <iostream>
#include <vector>

namespace {

void setHitArea(float* shared, int node, float x, float y, float width, float height) {
    float* slot = shared + node * sg::STRIDE;
    slot[sg::HITFLAGS] = 1.0f;
    float* local = shared + sg::BOUNDS_BASE +
        static_cast<size_t>(node) * sg::BOUNDS_STRIDE_F + 16;
    local[0] = x;
    local[1] = y;
    local[2] = x + width;
    local[3] = y + height;
}

}  // namespace

int main() {
    {
        std::vector<float> shared(sg::SHARED_FLOATS, 0.0f);
        sg::SceneGraph graph;
        graph.init(shared.data());

        const int root = graph.alloc();
        const int button = graph.alloc();
        const int decoration = graph.alloc();
        assert(root > 0 && button > 0 && decoration > 0);

        // Paint order is button, then decoration. Reverse hit traversal therefore examines the
        // decoration first, exactly like an instructional Spine/particle effect above a control.
        graph.setParent(root, 0, -1);
        graph.setParent(button, root, -1);
        graph.setParent(decoration, root, -1);

        shared[root * sg::STRIDE + sg::EVENTMODE] = 3.0f;  // static ancestor
        setHitArea(shared.data(), root, 0, 0, 100, 100);

        shared[button * sg::STRIDE + sg::EVENTMODE] = 3.0f;
        setHitArea(shared.data(), button, 0, 0, 100, 100);

        float* visual = shared.data() + decoration * sg::STRIDE;
        visual[sg::TYPE] = sg::T_SPRITE;
        visual[sg::RX] = 0.0f;
        visual[sg::RY] = 0.0f;
        visual[sg::RW] = 100.0f;
        visual[sg::RH] = 100.0f;
        visual[sg::EVENTMODE] = 0.0f;  // auto: visual-only facade
        visual[sg::HITFLAGS] = static_cast<float>(
            32 | sg::HIT_NATIVE_GEOMETRY_SELF_ONLY);

        int count = graph.hitTest(root, 50.0f, 50.0f, false);
        assert(count >= 1);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 1]) == button);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 2]) == button);

        // The contract suppresses only inherited promotion. Making the same native facade
        // explicitly interactive must restore its exact geometry as the topmost target.
        visual[sg::EVENTMODE] = 3.0f;
        count = graph.hitTest(root, 50.0f, 50.0f, false);
        assert(count >= 1);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 1]) == decoration);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 2]) == decoration);
    }

    {
        std::vector<float> shared(sg::SHARED_FLOATS, 0.0f);
        sg::SceneGraph graph;
        graph.init(shared.data());

        const int popup = graph.alloc();
        const int collectButton = graph.alloc();
        const int layoutSlot = graph.alloc();
        assert(popup > 0 && collectButton > 0 && layoutSlot > 0);

        // A queried Spine slot facade is appended after the authored controls. Its explicit
        // hitArea therefore wins reverse traversal unless native preserves that the geometry is
        // self-only. This is the popup-mask shape that previously swallowed pointerup/tap while
        // keyboard activation continued to close the popup.
        graph.setParent(popup, 0, -1);
        graph.setParent(collectButton, popup, -1);
        graph.setParent(layoutSlot, popup, -1);

        shared[popup * sg::STRIDE + sg::EVENTMODE] = 3.0f;
        setHitArea(shared.data(), popup, 0, 0, 100, 100);
        shared[collectButton * sg::STRIDE + sg::EVENTMODE] = 3.0f;
        setHitArea(shared.data(), collectButton, 35, 35, 30, 30);

        shared[layoutSlot * sg::STRIDE + sg::EVENTMODE] = 0.0f;
        setHitArea(shared.data(), layoutSlot, 0, 0, 100, 100);
        shared[layoutSlot * sg::STRIDE + sg::HITFLAGS] = static_cast<float>(
            1 | sg::HIT_NATIVE_GEOMETRY_SELF_ONLY);

        int count = graph.hitTest(popup, 50.0f, 50.0f, false);
        assert(count >= 1);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 1]) == collectButton);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 2]) == collectButton);

        // Explicitly making the slot interactive opts it back into its own synthetic hitArea.
        shared[layoutSlot * sg::STRIDE + sg::EVENTMODE] = 3.0f;
        count = graph.hitTest(popup, 50.0f, 50.0f, false);
        assert(count >= 1);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 1]) == layoutSlot);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 2]) == layoutSlot);
    }

    {
        std::vector<float> shared(sg::SHARED_FLOATS, 0.0f);
        sg::SceneGraph graph;
        graph.init(shared.data());

        const int root = graph.alloc();
        const int control = graph.alloc();
        const int compressedVisualSubtree = graph.alloc();
        assert(root > 0 && control > 0 && compressedVisualSubtree > 0);

        graph.setParent(root, 0, -1);
        graph.setParent(control, root, -1);
        graph.setParent(compressedVisualSubtree, control, -1);

        shared[root * sg::STRIDE + sg::EVENTMODE] = 0.0f;
        shared[control * sg::STRIDE + sg::EVENTMODE] = 3.0f;

        // A native renderer may collapse the visual descendants of an otherwise geometry-less
        // interactive Container into one exact-geometry node. With no SELF_ONLY marker, that node
        // stands in for the removed descendants and must promote the nearest interactive control.
        float* visual = shared.data() + compressedVisualSubtree * sg::STRIDE;
        visual[sg::TYPE] = sg::T_SPRITE;
        visual[sg::RX] = 10.0f;
        visual[sg::RY] = 20.0f;
        visual[sg::RW] = 80.0f;
        visual[sg::RH] = 60.0f;
        visual[sg::EVENTMODE] = 0.0f;
        visual[sg::HITFLAGS] = 32.0f;

        const int count = graph.hitTest(root, 50.0f, 50.0f, false);
        assert(count >= 1);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 1]) == control);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 2]) == compressedVisualSubtree);
    }

    {
        std::vector<float> shared(sg::SHARED_FLOATS, 0.0f);
        sg::SceneGraph graph;
        graph.init(shared.data());

        const int root = graph.alloc();
        const int spinButton = graph.alloc();
        const int guideSpine = graph.alloc();
        assert(root > 0 && spinButton > 0 && guideSpine > 0);

        // LuckySpin's visual hand is appended above the real spin button. Browser SpineUtils
        // disables interactiveChildren, so its SlotMesh descendants are pruned and the button
        // wins. Native compresses those meshes onto guideSpine and must retain the same result.
        graph.setParent(root, 0, -1);
        graph.setParent(spinButton, root, -1);
        graph.setParent(guideSpine, root, -1);

        shared[root * sg::STRIDE + sg::EVENTMODE] = 3.0f;
        setHitArea(shared.data(), root, 0, 0, 100, 100);
        shared[spinButton * sg::STRIDE + sg::EVENTMODE] = 3.0f;
        setHitArea(shared.data(), spinButton, 0, 0, 100, 100);

        float* guide = shared.data() + guideSpine * sg::STRIDE;
        guide[sg::TYPE] = sg::T_SPRITE;
        guide[sg::RW] = 100.0f;
        guide[sg::RH] = 100.0f;
        guide[sg::EVENTMODE] = 0.0f;  // interactive=false maps to auto in Pixi v7.
        guide[sg::INTERACTIVE_CHILDREN] = 0.0f;
        guide[sg::HITFLAGS] = static_cast<float>(
            32 | sg::HIT_NATIVE_DESCENDANT_GEOMETRY);

        int count = graph.hitTest(root, 50.0f, 50.0f, false);
        assert(count >= 1);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 1]) == spinButton);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 2]) == spinButton);

        // A H5 Container still has no self geometry merely because its eventMode is static.
        // The flattened SlotMesh triangles remain descendant geometry.
        guide[sg::EVENTMODE] = 3.0f;
        count = graph.hitTest(root, 50.0f, 50.0f, false);
        assert(count >= 1);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 1]) == spinButton);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 2]) == spinButton);

        // Re-enabling children restores the browser SlotMesh witness contract. The visual is the
        // top sibling, so its geometry promotes the nearest interactive ancestor (root).
        guide[sg::EVENTMODE] = 0.0f;
        guide[sg::INTERACTIVE_CHILDREN] = 1.0f;
        graph.markBoundsDirty(guideSpine);
        count = graph.hitTest(root, 50.0f, 50.0f, false);
        assert(count >= 1);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 1]) == root);
        assert(static_cast<int>(shared[sg::HIT_RESULT_BASE + 2]) == guideSpine);
    }

    std::cout << "native_scenegraph_inherited_geometry_hit_test: ok\n";
    return 0;
}
