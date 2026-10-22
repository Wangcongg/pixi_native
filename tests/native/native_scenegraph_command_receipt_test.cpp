// Generation-checked structural mutation receipt contract.
#include "cmdbuf/scenegraph.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using Receipt = sg::CommandReceipt;

static void expectTopology(const sg::SceneGraph& graph, int child, int parent,
                           const std::vector<int>& children) {
    assert(graph.nodes[child].parent == parent);
    if (parent >= 0) assert(graph.nodes[parent].children == children);
}

int main() {
    static_assert(static_cast<int32_t>(Receipt::Applied) == 1);
    static_assert(static_cast<int32_t>(Receipt::Idempotent) == 2);
    static_assert(static_cast<int32_t>(Receipt::ManagedFreeDeferred) == 3);
    static_assert(static_cast<int32_t>(Receipt::InvalidOrDead) == -1);
    static_assert(static_cast<int32_t>(Receipt::GenerationMismatch) == -2);
    static_assert(static_cast<int32_t>(Receipt::Cycle) == -3);
    static_assert(static_cast<int32_t>(Receipt::BeforeNotSibling) == -4);

    std::vector<float> shared(sg::SHARED_FLOATS, 0.0f);
    sg::SceneGraph graph;
    graph.init(shared.data());

    const int parent = graph.alloc();
    const int otherParent = graph.alloc();
    const int child = graph.alloc();
    const int before = graph.alloc();
    assert(parent > 0 && otherParent > 0 && child > 0 && before > 0);
    graph.setParent(parent, 0, -1);
    graph.setParent(otherParent, 0, -1);
    graph.setParent(before, parent, -1);

    const uint32_t parentGen = graph.generation(parent);
    const uint32_t otherParentGen = graph.generation(otherParent);
    const uint32_t childGen = graph.generation(child);
    const uint32_t beforeGen = graph.generation(before);
    const uint32_t* sharedU32 = reinterpret_cast<const uint32_t*>(shared.data());
    assert(sharedU32[sg::NODE_GEN_BASE + child] == childGen);

    assert(graph.setParentChecked(child, parent, before,
                                  childGen, parentGen, beforeGen) == Receipt::Applied);
    expectTopology(graph, child, parent, {child, before});
    assert(graph.setParentChecked(child, parent, before,
                                  childGen, parentGen, beforeGen) == Receipt::Idempotent);
    expectTopology(graph, child, parent, {child, before});

    // Every capability is independently checked, and rejection is side-effect free.
    assert(graph.setParentChecked(child, otherParent, -1,
                                  childGen + 1, otherParentGen, 0) == Receipt::GenerationMismatch);
    expectTopology(graph, child, parent, {child, before});
    assert(graph.setParentChecked(child, otherParent, -1,
                                  childGen, otherParentGen + 1, 0) == Receipt::GenerationMismatch);
    expectTopology(graph, child, parent, {child, before});
    assert(graph.setParentChecked(child, parent, before,
                                  childGen, parentGen, beforeGen + 1) == Receipt::GenerationMismatch);
    expectTopology(graph, child, parent, {child, before});

    // A live `before` owned by another parent is not silently converted to append on V2.
    assert(graph.setParentChecked(child, parent, otherParent,
                                  childGen, parentGen, otherParentGen) == Receipt::BeforeNotSibling);
    expectTopology(graph, child, parent, {child, before});

    // The legacy three-argument API intentionally keeps its historical append fallback.
    graph.setParent(child, parent, otherParent);
    expectTopology(graph, child, parent, {before, child});

    // Parent-under-child would form a cycle and must leave both edges untouched.
    assert(graph.setParentChecked(parent, child, -1,
                                  parentGen, childGen, 0) == Receipt::Cycle);
    expectTopology(graph, parent, 0, {parent, otherParent});
    expectTopology(graph, child, parent, {before, child});

    assert(graph.removeFromParent(child, childGen + 1) == Receipt::GenerationMismatch);
    expectTopology(graph, child, parent, {before, child});
    assert(graph.removeFromParent(child, childGen) == Receipt::Applied);
    assert(graph.nodes[child].parent == -1);
    assert(graph.removeFromParent(child, childGen) == Receipt::Idempotent);

    assert(graph.freeNode(child, childGen + 1) == Receipt::GenerationMismatch);
    assert(graph.nodes[child].alive);
    assert(graph.freeNode(child, childGen) == Receipt::Applied);
    assert(!graph.nodes[child].alive);
    assert(graph.generation(child) == 0);
    assert(sharedU32[sg::NODE_GEN_BASE + child] == 0u);
    assert(graph.freeNode(child, childGen) == Receipt::Idempotent);
    assert(graph.removeFromParent(child, childGen) == Receipt::Idempotent);

    const int reused = graph.alloc();
    assert(reused == child);
    const uint32_t reusedGen = graph.generation(reused);
    assert(reusedGen != childGen);
    assert(sharedU32[sg::NODE_GEN_BASE + reused] == reusedGen);
    assert(graph.setParentChecked(reused, otherParent, -1,
                                  childGen, otherParentGen, 0) == Receipt::GenerationMismatch);
    assert(graph.nodes[reused].parent == -1);
    assert(graph.setParentChecked(reused, otherParent, -1,
                                  reusedGen, otherParentGen, 0) == Receipt::Applied);

    // V2 reports each inspected row, stops at the first rejection, and leaves the tail at zero.
    int32_t batch[] = {
        2, reused, 0, -1, static_cast<int32_t>(reusedGen), 0, 0, 0,
        1, reused, parent, -1, static_cast<int32_t>(reusedGen),
           static_cast<int32_t>(parentGen), 0, 0,
        1, reused, parent, otherParent, static_cast<int32_t>(reusedGen),
           static_cast<int32_t>(parentGen), static_cast<int32_t>(otherParentGen), 0,
        3, reused, 0, -1, static_cast<int32_t>(reusedGen), 0, 0, 0,
    };
    int32_t receipts[4] = {99, 99, 99, 99};
    int freeCalls = 0, refreshCalls = 0;
    const size_t inspected = sg::executeCommandsV2(
        graph, batch, sizeof(batch) / sizeof(batch[0]), receipts, 4,
        [&](int id, uint32_t gen) { ++freeCalls; return graph.freeNode(id, gen); },
        [&](int) { ++refreshCalls; });
    assert(inspected == 3);
    assert(receipts[0] == static_cast<int32_t>(Receipt::Applied));
    assert(receipts[1] == static_cast<int32_t>(Receipt::Applied));
    assert(receipts[2] == static_cast<int32_t>(Receipt::BeforeNotSibling));
    assert(receipts[3] == 0);
    assert(freeCalls == 0 && refreshCalls == 2);
    assert(graph.nodes[reused].parent == parent);

    // Managed-free is a successful terminal state; a missing capability rejects before callback.
    int32_t managedRow[] = {3, reused, 0, -1, static_cast<int32_t>(reusedGen), 0, 0, 0};
    receipts[0] = 99;
    assert(sg::executeCommandsV2(graph, managedRow, 8, receipts, 1,
        [&](int, uint32_t) { ++freeCalls; return Receipt::ManagedFreeDeferred; },
        [&](int) { ++refreshCalls; }) == 1);
    assert(receipts[0] == static_cast<int32_t>(Receipt::ManagedFreeDeferred));
    assert(freeCalls == 1);
    managedRow[4] = 0; receipts[0] = 99;
    assert(sg::executeCommandsV2(graph, managedRow, 8, receipts, 1,
        [&](int, uint32_t) { ++freeCalls; return Receipt::Applied; },
        [&](int) { ++refreshCalls; }) == 1);
    assert(receipts[0] == static_cast<int32_t>(Receipt::GenerationMismatch));
    assert(freeCalls == 1);

    std::cout << "native_scenegraph_command_receipt_test: ok\n";
    return 0;
}
