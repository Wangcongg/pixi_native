#pragma once

#include <cstdint>
#include <limits>

namespace macres {

// A native image becomes visible to JavaScript before Pixi necessarily binds it. During a large
// scene load, hundreds of resource/cache operations can happen in that gap, so residency age must
// be measured in actual rendered frames rather than ResourceManager operation sequence numbers.
struct NativeImageResidencyPolicy {
    static constexpr uint64_t kPublishGraceFrames = 300;
    static constexpr uint64_t kReleaseGraceFrames = 120;

    static uint64_t deadline(uint64_t renderFrame, uint64_t graceFrames) {
        const uint64_t max = std::numeric_limits<uint64_t>::max();
        return renderFrame > max - graceFrames ? max : renderFrame + graceFrames;
    }

    // 分类型引用必须保持 0 <= adoptRefs <= refs。普通 release 只能消费硬引用，
    // releaseAdopt/transferAdopt 只能消费软引用；任何一侧的重复释放都不能偷走另一侧。
    static bool hasHardReference(int refs, int adoptRefs) {
        return refs > 0 && adoptRefs >= 0 && refs > adoptRefs;
    }

    static bool hasAdoptReference(int refs, int adoptRefs) {
        return refs > 0 && adoptRefs > 0 && refs >= adoptRefs;
    }

    // refs 分两种性质:adoptRefs 是 JS Image 的 adopt 引用(软——图从未参与首次渲染时,只有
    // 这类引用;浏览器里预解码位图根本不占 GPU,对应地这里允许预算驱逐,复用时 stale reload
    // 重建),其余是 glTexture/spinex 的硬引用(在渲染,绝不可驱逐)。refs==adoptRefs(含 0==0)
    // 即"无硬引用",过保护期即可驱逐——被驱逐条目的 Image 侧 owned 标志由 transfer/retain
    // 失败路径自愈。
    static bool canEvict(bool ready, int refs, int adoptRefs, uint64_t renderFrame,
                         uint64_t protectedUntilRenderFrame) {
        return ready && refs >= 0 && adoptRefs >= 0 && refs == adoptRefs
            && renderFrame > protectedUntilRenderFrame;
    }
};

} // namespace macres
