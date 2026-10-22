#include "cmdbuf/scenegraph.h"
#include <cassert>
#include <vector>
#include <cmath>

int main() {
    std::vector<float> shared(sg::SHARED_FLOATS, 0.0f);
    sg::SceneGraph graph; graph.init(shared.data());
    int stage=graph.alloc(), pool=graph.alloc(), a=graph.alloc(), b=graph.alloc();
    graph.setParent(a,pool,-1); graph.setParent(b,pool,-1);
    // A detached effect still has attached children. Repeated slot-order writes must
    // not accumulate one pending entry per frame while a different root is rendered.
    for(int frame=0;frame<20000;frame++) {
        graph.setParent(a,pool,-1); graph.setParent(b,pool,-1);
        graph.compute(stage,sg::CM_CANONICAL_MAIN);
        assert(graph.pending_.size() <= 2);
    }
    shared[pool*sg::STRIDE+sg::X]=30;
    shared[a*sg::STRIDE+sg::X]=7;
    graph.markLocalInputChanged(pool); graph.markLocalInputChanged(a);
    graph.setParent(pool,stage,-1);
    graph.compute(stage,sg::CM_CANONICAL_MAIN);
    assert(std::abs(shared[a*sg::STRIDE+sg::WTX]-37)<0.001f);
    assert(graph.pending_.empty());
    // Independent offscreen roots retain their own queued mutations.
    int rt=graph.alloc(), c=graph.alloc();graph.setParent(c,rt,-1);
    shared[c*sg::STRIDE+sg::X]=91;graph.markLocalInputChanged(c);
    for(int i=0;i<100;i++)graph.compute(stage,sg::CM_CANONICAL_MAIN);
    graph.compute(rt,sg::CM_CANONICAL_MAIN);
    assert(std::abs(shared[c*sg::STRIDE+sg::WTX]-91)<0.001f);
    // Queue membership survives free/reuse without suppressing new writes.
    graph.setParent(c,rt,-1);graph.freeNode(c);int next=graph.alloc();assert(next==c);
    graph.setParent(next,stage,-1);shared[next*sg::STRIDE+sg::X]=42;
    graph.markLocalInputChanged(next);graph.compute(stage,sg::CM_CANONICAL_MAIN);
    assert(std::abs(shared[next*sg::STRIDE+sg::WTX]-42)<0.001f);
}
