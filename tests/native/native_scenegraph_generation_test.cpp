// 节点代际契约:slot 复用后,旧引用的代际比对必须失败。
// 这是 native tween(及一切跨帧持有 nodeId 的宿主写入方)不写坏复用槽位的结构性保证——
// 进机台的 free/alloc 高峰期,旧场景未清干净的 tween 曾能对复用 id 的新节点定格终值
// (轮盘容器停在 identity/旧坐标),且 JS 影子判等使其永不修复。
#include "cmdbuf/scenegraph.h"

#include <cassert>
#include <cstring>
#include <limits>
#include <iostream>
#include <vector>

static uint32_t mirroredGeneration(const std::vector<float>& shared, int id) {
    uint32_t value = 0;
    std::memcpy(&value, shared.data() + sg::NODE_GEN_BASE + id, sizeof(value));
    return value;
}

int main() {
    std::vector<float> shared(sg::SHARED_FLOATS, 0.0f);
    sg::SceneGraph graph;
    graph.init(shared.data());

    const int a = graph.alloc();
    assert(a > 0);
    const uint32_t g1 = graph.generation(a);
    assert(g1 > 0);
    assert(graph.generation(a) == g1);   // 存活期间稳定
    assert(mirroredGeneration(shared, a) == g1); // JS 零跨桥读取同一 capability

    // 无效 id 恒 0(永不等于任何已保存代际)
    assert(graph.generation(0) == 0);
    assert(graph.generation(-1) == 0);
    assert(graph.generation(sg::NODE_CAP) == 0);

    graph.freeNode(a);
    assert(graph.generation(a) == 0);    // free 后:任何旧引用比对失败
    assert(mirroredGeneration(shared, a) == 0);

    const int b = graph.alloc();
    assert(b == a);                      // freelist 复用同一 slot
    const uint32_t g2 = graph.generation(b);
    assert(g2 > 0);
    assert(g2 != g1);                    // 新世代:上一任的引用无法冒充
    assert(mirroredGeneration(shared, b) == g2);

    // 多轮复用不回绕到旧代际
    graph.freeNode(b);
    const int c = graph.alloc();
    assert(c == a);
    assert(graph.generation(c) != g1);
    assert(graph.generation(c) != g2);

    // uint32 wrap must skip zero because zero is the permanent dead/unallocated sentinel used by
    // the V2 command bridge.  Force the otherwise unreachable boundary deterministically.
    graph.freeNode(c);
    graph.nodeGen_[c] = std::numeric_limits<uint32_t>::max();
    const int d = graph.alloc();
    assert(d == a);
    assert(graph.generation(d) == 1);
    assert(mirroredGeneration(shared, d) == 1);

    std::cout << "native_scenegraph_generation_test: ok\n";
    return 0;
}
