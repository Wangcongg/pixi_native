#pragma once

#include <utility>

#include "scenegraph.h"

namespace sg {

// Native animation systems consume SceneGraph world transforms and cached bounds, then publish
// new local geometry/slot transforms back into the same graph.  A freshly attached or reparented
// node can still carry the world matrix from its previous parent until compute(root) runs.  Keep
// the required input publication next to the animation callback so a host cannot accidentally
// move Spine/frame-cache driving ahead of it while rearranging the render pipeline.
//
// The caller still performs its normal post-drive compute before batching; that second pass
// consumes local changes made by the animation callback.
template <class Drive>
inline void driveAnimationsWithPublishedTransforms(SceneGraph& graph, int root, Drive&& drive) {
    // D-1 契约:drive 前置 compute 必须 CanonicalMain——QueryNoAck 会消费错位(四审前此处隐式)。
    graph.compute(root, CM_CANONICAL_MAIN);
    std::forward<Drive>(drive)();
}

}  // namespace sg
