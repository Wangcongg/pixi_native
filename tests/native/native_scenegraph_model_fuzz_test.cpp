// SceneGraph model-based fuzz / invariant skeleton.
//
// The production graph and an independent shadow forest receive the same legal
// alloc/free/setParent/remove operations.  Every operation is followed by a
// full topology, generation, debt and detached-owner audit.  Failures include
// the seed and step so an intermittent sequence can be replayed exactly.
//
// This deliberately characterises today's non-sibling `before` fallback
// (append) instead of pretending the command is transactional.  Once command
// receipts are added, the model can be tightened to require an explicit
// rejection code without changing the fuzz driver.
#include "cmdbuf/scenegraph.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

enum class OwnerClass : uint8_t {
    Attached,
    DetachedPool,
    DetachedStaging,
    UnknownDetached,
    Destroyed,
};

const char* ownerName(OwnerClass owner) {
    switch (owner) {
        case OwnerClass::Attached: return "attached";
        case OwnerClass::DetachedPool: return "detached-pool";
        case OwnerClass::DetachedStaging: return "detached-staging";
        case OwnerClass::UnknownDetached: return "unknown-detached";
        case OwnerClass::Destroyed: return "destroyed";
    }
    return "invalid";
}

std::string location(uint64_t seed, size_t step) {
    std::ostringstream out;
    out << "seed=0x" << std::hex << seed << std::dec << " step=" << step;
    return out.str();
}

[[noreturn]] void fail(uint64_t seed, size_t step, const std::string& message) {
    throw std::runtime_error(location(seed, step) + ": " + message);
}

void require(bool condition, uint64_t seed, size_t step, const std::string& message) {
    if (!condition) fail(seed, step, message);
}

struct ShadowNode {
    int parent = -1;
    std::vector<int> children;
    bool alive = false;
    uint32_t generation = 1;
    OwnerClass owner = OwnerClass::Destroyed;
};

class ShadowGraph {
public:
    ShadowGraph() : nodes(sg::NODE_CAP) {
        nodes[0].alive = true;
        nodes[0].owner = OwnerClass::Attached;
    }

    int alloc(OwnerClass owner) {
        int id = -1;
        if (!freeList.empty()) {
            id = freeList.back();
            freeList.pop_back();
        } else if (highWater < sg::NODE_CAP) {
            id = highWater++;
        }
        if (id < 0) return -1;

        ShadowNode& node = nodes[id];
        node.parent = -1;
        node.children.clear();
        node.alive = true;
        ++node.generation;
        node.owner = owner;
        return id;
    }

    bool wouldCycle(int child, int parent) const {
        int guard = 0;
        for (int at = parent; at > 0 && at < highWater && guard++ < sg::NODE_CAP;
             at = nodes[at].parent) {
            if (at == child) return true;
        }
        return guard >= sg::NODE_CAP;
    }

    bool setParent(int child, int parent, int before) {
        if (!validLive(child) || !validLiveOrRoot(parent) || wouldCycle(child, parent)) return false;

        unlink(child);
        nodes[child].parent = parent;
        std::vector<int>& siblings = nodes[parent].children;
        const auto it = before >= 0 ? std::find(siblings.begin(), siblings.end(), before) : siblings.end();
        if (it == siblings.end()) siblings.push_back(child);
        else siblings.insert(it, child);
        setSubtreeOwner(child, nodes[parent].owner);
        return true;
    }

    void removeFromParent(int child, OwnerClass detachedOwner) {
        if (!validLive(child)) return;
        unlink(child);
        setSubtreeOwner(child, detachedOwner);
    }

    void freeNode(int id, OwnerClass orphanOwner) {
        if (!validLive(id)) return;

        const OwnerClass previousOwner = nodes[id].owner;
        const std::vector<int> children = nodes[id].children;
        unlink(id);
        for (int child : children) nodes[child].parent = -1;
        nodes[id].children.clear();
        nodes[id].alive = false;
        nodes[id].owner = OwnerClass::Destroyed;
        freeList.push_back(id);

        OwnerClass childOwner = previousOwner;
        if (childOwner == OwnerClass::Attached || childOwner == OwnerClass::Destroyed ||
            childOwner == OwnerClass::UnknownDetached) {
            childOwner = orphanOwner;
        }
        for (int child : children) setSubtreeOwner(child, childOwner);
    }

    bool topologyAttached(int id) const {
        if (id == 0) return nodes[0].alive;
        if (!validLive(id)) return false;
        int guard = 0;
        for (int at = id; at >= 0 && at < highWater && guard++ < sg::NODE_CAP;
             at = nodes[at].parent) {
            if (at == 0) return true;
            if (nodes[at].parent < 0) return false;
        }
        return false;
    }

    bool validateOwners(std::string* error) const {
        for (int id = 0; id < highWater; ++id) {
            const ShadowNode& node = nodes[id];
            if (!node.alive) {
                if (node.owner != OwnerClass::Destroyed) {
                    if (error) *error = "dead id " + std::to_string(id) + " owner=" + ownerName(node.owner);
                    return false;
                }
                continue;
            }

            const bool attached = topologyAttached(id);
            if (attached && node.owner != OwnerClass::Attached) {
                if (error) *error = "attached id " + std::to_string(id) + " owner=" + ownerName(node.owner);
                return false;
            }
            if (!attached && node.owner != OwnerClass::DetachedPool &&
                node.owner != OwnerClass::DetachedStaging) {
                if (error) *error = "unclassified detached id " + std::to_string(id) +
                                    " owner=" + ownerName(node.owner);
                return false;
            }
            if (node.parent > 0 && nodes[node.parent].alive && node.owner != nodes[node.parent].owner) {
                if (error) *error = "detached-owner split between parent " +
                                    std::to_string(node.parent) + " and child " + std::to_string(id);
                return false;
            }
        }
        return true;
    }

    std::vector<int> liveIds(bool includeRoot = false) const {
        std::vector<int> result;
        for (int id = includeRoot ? 0 : 1; id < highWater; ++id) {
            if (nodes[id].alive) result.push_back(id);
        }
        return result;
    }

    std::vector<int> descendants(int root) const {
        std::vector<int> result;
        if (!validLive(root)) return result;
        std::vector<int> stack(nodes[root].children.begin(), nodes[root].children.end());
        while (!stack.empty()) {
            const int id = stack.back();
            stack.pop_back();
            result.push_back(id);
            stack.insert(stack.end(), nodes[id].children.begin(), nodes[id].children.end());
        }
        return result;
    }

    bool validLive(int id) const {
        return id > 0 && id < highWater && nodes[id].alive;
    }

    bool validLiveOrRoot(int id) const {
        return id >= 0 && id < highWater && nodes[id].alive;
    }

    std::vector<ShadowNode> nodes;
    std::vector<int> freeList;
    int highWater = 1;

private:
    void unlink(int child) {
        const int parent = nodes[child].parent;
        if (parent >= 0 && parent < highWater) {
            std::vector<int>& siblings = nodes[parent].children;
            const auto it = std::find(siblings.begin(), siblings.end(), child);
            if (it != siblings.end()) siblings.erase(it);
        }
        nodes[child].parent = -1;
    }

    void setSubtreeOwner(int root, OwnerClass owner) {
        std::vector<int> stack{root};
        while (!stack.empty()) {
            const int id = stack.back();
            stack.pop_back();
            nodes[id].owner = owner;
            stack.insert(stack.end(), nodes[id].children.begin(), nodes[id].children.end());
        }
    }
};

struct Counters {
    size_t operations = 0;
    size_t cycleRejects = 0;
    size_t nonSiblingBeforeFallbacks = 0;
    size_t ownerFaultsDetected = 0;
};

class GraphPair {
public:
    GraphPair() : shared(sg::SHARED_FLOATS, 0.0f) {
        graph.init(shared.data());
    }

    int alloc(OwnerClass owner) {
        const int actual = graph.alloc();
        const int expected = model.alloc(owner);
        if (actual != expected) {
            throw std::runtime_error("alloc id mismatch actual=" + std::to_string(actual) +
                                     " expected=" + std::to_string(expected));
        }
        return actual;
    }

    bool setParent(int child, int parent, int before, Counters& counters) {
        const bool expectedApply = !model.wouldCycle(child, parent);
        const bool nonSiblingBefore = before >= 0 &&
            std::find(model.nodes[parent].children.begin(), model.nodes[parent].children.end(), before) ==
                model.nodes[parent].children.end();

        graph.setParent(child, parent, before);
        const bool applied = model.setParent(child, parent, before);
        if (applied != expectedApply) throw std::logic_error("shadow preflight disagreed with shadow mutation");
        if (!applied) ++counters.cycleRejects;
        else if (nonSiblingBefore) ++counters.nonSiblingBeforeFallbacks;
        return applied;
    }

    void removeFromParent(int child, OwnerClass detachedOwner) {
        graph.removeFromParent(child);
        model.removeFromParent(child, detachedOwner);
    }

    void freeNode(int id, OwnerClass orphanOwner) {
        graph.freeNode(id);
        model.freeNode(id, orphanOwner);
    }

    void check(uint64_t seed, size_t step) const {
        require(graph.highWater == model.highWater, seed, step, "highWater mismatch");
        require(graph.freeList == model.freeList, seed, step, "free-list order mismatch");

        for (int id = 0; id < model.highWater; ++id) {
            const sg::Node& actual = graph.nodes[id];
            const ShadowNode& expected = model.nodes[id];
            require(actual.alive == expected.alive, seed, step,
                    "alive mismatch id=" + std::to_string(id));
            require(actual.parent == expected.parent, seed, step,
                    "parent mismatch id=" + std::to_string(id) + " actual=" +
                    std::to_string(actual.parent) + " expected=" + std::to_string(expected.parent));
            require(actual.children == expected.children, seed, step,
                    "child order mismatch id=" + std::to_string(id));
            require(graph.nodeGen_[id] == expected.generation, seed, step,
                    "internal generation mismatch id=" + std::to_string(id));
            const uint32_t publicGeneration = graph.generation(id);
            const uint32_t expectedPublicGeneration = id > 0 && expected.alive ? expected.generation : 0;
            require(publicGeneration == expectedPublicGeneration, seed, step,
                    "public generation mismatch id=" + std::to_string(id));

            if (!actual.alive) {
                require(actual.children.empty(), seed, step,
                        "destroyed node retains children id=" + std::to_string(id));
                continue;
            }

            if (id == 0) require(actual.parent == -1, seed, step, "root parent changed");
            else require(actual.parent == -1 ||
                         (actual.parent >= 0 && actual.parent < model.highWater && graph.nodes[actual.parent].alive),
                         seed, step, "live node points at dead/invalid parent id=" + std::to_string(id));

            std::unordered_set<int> uniqueChildren;
            for (int child : actual.children) {
                require(child > 0 && child < model.highWater && graph.nodes[child].alive,
                        seed, step, "invalid/dead child under id=" + std::to_string(id));
                require(graph.nodes[child].parent == id, seed, step,
                        "parent/child backlink mismatch child=" + std::to_string(child));
                require(uniqueChildren.insert(child).second, seed, step,
                        "duplicate child=" + std::to_string(child) + " parent=" + std::to_string(id));
            }

            std::unordered_set<int> ancestry;
            int at = id;
            while (at >= 0) {
                require(at < model.highWater && graph.nodes[at].alive, seed, step,
                        "ancestry crosses dead/invalid node from id=" + std::to_string(id));
                require(ancestry.insert(at).second, seed, step,
                        "cycle found while walking ancestors from id=" + std::to_string(id));
                at = graph.nodes[at].parent;
            }
        }

        std::string ownerError;
        require(model.validateOwners(&ownerError), seed, step, ownerError);
        checkDebt(seed, step);
    }

    void proveOwnerFaultDetector(uint64_t seed, size_t step, Counters& counters) {
        int candidate = -1;
        for (int id : model.liveIds()) {
            if (!model.topologyAttached(id)) {
                candidate = id;
                break;
            }
        }
        if (candidate < 0) candidate = alloc(OwnerClass::DetachedStaging);
        check(seed, step);

        const OwnerClass saved = model.nodes[candidate].owner;
        model.nodes[candidate].owner = OwnerClass::UnknownDetached;
        std::string ownerError;
        require(!model.validateOwners(&ownerError), seed, step,
                "detached-owner fault injection was not detected");
        require(ownerError.find(std::to_string(candidate)) != std::string::npos, seed, step,
                "detached-owner diagnostic omitted offending id");
        ++counters.ownerFaultsDetected;
        model.nodes[candidate].owner = saved;
        check(seed, step);
    }

    sg::SceneGraph graph;
    ShadowGraph model;

private:
    int theoryDebt(int id, std::vector<int>& memo, std::vector<uint8_t>& visiting,
                   uint64_t seed, size_t step) const {
        if (memo[id] >= 0) return memo[id];
        require(!visiting[id], seed, step,
                "cycle reached by debt audit id=" + std::to_string(id));
        visiting[id] = 1;
        int debt = (static_cast<int>(shared[sg::PDIRTY_BASE + id]) & 1) ? 1 : 0;
        for (int child : graph.nodes[id].children) {
            if (child > 0 && child < graph.highWater && graph.nodes[child].alive) {
                debt += theoryDebt(child, memo, visiting, seed, step);
            }
        }
        visiting[id] = 0;
        memo[id] = debt;
        return debt;
    }

    void checkDebt(uint64_t seed, size_t step) const {
        std::vector<int> memo(graph.highWater, -1);
        std::vector<uint8_t> visiting(graph.highWater, 0);
        for (int id = 0; id < graph.highWater; ++id) {
            if (!graph.nodes[id].alive) continue;
            const int expected = theoryDebt(id, memo, visiting, seed, step);
            const float actual = shared[sg::DEBT_BASE + id];
            require(actual >= -0.5f && std::fabs(actual - static_cast<float>(expected)) < 0.25f,
                    seed, step, "debt mismatch id=" + std::to_string(id) + " actual=" +
                    std::to_string(actual) + " expected=" + std::to_string(expected));
        }
    }

    std::vector<float> shared;
};

OwnerClass randomDetachedOwner(std::mt19937_64& rng) {
    return (rng() & 1u) ? OwnerClass::DetachedPool : OwnerClass::DetachedStaging;
}

int choose(const std::vector<int>& ids, std::mt19937_64& rng) {
    if (ids.empty()) return -1;
    return ids[static_cast<size_t>(rng() % ids.size())];
}

void runCommandSegmentationContract() {
    class BufferModel {
    public:
        void push() {
            // Command V2 carries op/ids plus three generations and receipt
            // metadata: 8 words per row.  The buffer grows to 8192 words so
            // the existing 1024-row publication boundary remains unchanged.
            if (words + kRowStride > kCapacityWords) flush();
            words += kRowStride;
        }
        void finish() { flush(); }
        std::vector<int> flushes;
    private:
        void flush() {
            if (!words) return;
            flushes.push_back(words / kRowStride);
            words = 0;
        }
        enum { kRowStride = 8, kCapacityWords = 8192 };
        int words = 0;
    };

    const auto run = [](int commands) {
        BufferModel buffer;
        for (int i = 0; i < commands; ++i) buffer.push();
        buffer.finish();
        return buffer.flushes;
    };

    const uint64_t seed = 0;
    require(run(1023) == std::vector<int>{1023}, seed, 1023,
            "1023 commands must remain one flush");
    require(run(1024) == std::vector<int>{1024}, seed, 1024,
            "1024 commands must exactly fill one flush");
    require(run(1025) == (std::vector<int>{1024, 1}), seed, 1025,
            "1025 commands must publish as 1024 + 1 (not atomically)");
}

void runDirectedContracts(Counters& counters) {
    const uint64_t seed = 0x5347444952454354ULL; // "SGDIRECT"
    GraphPair pair;
    const int top = pair.alloc(OwnerClass::DetachedPool);
    const int middle = pair.alloc(OwnerClass::DetachedPool);
    const int deep = pair.alloc(OwnerClass::DetachedPool);
    const int otherParent = pair.alloc(OwnerClass::DetachedStaging);
    const int moving = pair.alloc(OwnerClass::DetachedStaging);
    const int staging = pair.alloc(OwnerClass::DetachedStaging);

    pair.setParent(top, 0, -1, counters);
    pair.setParent(middle, top, -1, counters);
    pair.setParent(deep, middle, -1, counters);
    pair.setParent(otherParent, 0, -1, counters);
    pair.setParent(moving, otherParent, -1, counters);
    pair.check(seed, 1);

    const std::vector<int> rootBeforeCycle = pair.graph.nodes[0].children;
    const int topParentBeforeCycle = pair.graph.nodes[top].parent;
    const bool cycleApplied = pair.setParent(top, deep, -1, counters);
    require(!cycleApplied, seed, 2, "ancestor-to-descendant reparent was not rejected");
    require(pair.graph.nodes[0].children == rootBeforeCycle &&
            pair.graph.nodes[top].parent == topParentBeforeCycle,
            seed, 2, "cycle rejection changed topology");
    pair.check(seed, 2);

    // `middle` is live but is not a direct child of root.  Current native
    // semantics ignore it as `before` and append `moving` to root.
    const bool moved = pair.setParent(moving, 0, middle, counters);
    require(moved, seed, 3, "non-sibling before fallback unexpectedly rejected command");
    require(!pair.graph.nodes[0].children.empty() && pair.graph.nodes[0].children.back() == moving,
            seed, 3, "non-sibling before did not use the current append fallback");
    pair.check(seed, 3);

    pair.removeFromParent(middle, OwnerClass::DetachedStaging);
    pair.freeNode(otherParent, OwnerClass::DetachedPool);
    pair.check(seed, 4);

    // Keep at least one intentionally detached live subtree and prove that an
    // unclassified detached object is visible to the invariant, rather than
    // asserting the incorrect live == attached relation.
    (void)staging;
    pair.proveOwnerFaultDetector(seed, 5, counters);
}

bool tryIntentionalCycle(GraphPair& pair, std::mt19937_64& rng, Counters& counters) {
    std::vector<int> candidates = pair.model.liveIds();
    std::shuffle(candidates.begin(), candidates.end(), rng);
    for (int child : candidates) {
        const std::vector<int> descendants = pair.model.descendants(child);
        if (descendants.empty()) continue;
        const int parent = choose(descendants, rng);
        const bool applied = pair.setParent(child, parent, -1, counters);
        if (applied) throw std::logic_error("intentional cycle unexpectedly applied in shadow model");
        return true;
    }
    return false;
}

bool tryNonSiblingBefore(GraphPair& pair, std::mt19937_64& rng, Counters& counters) {
    const std::vector<int> live = pair.model.liveIds();
    if (live.size() < 2) return false;
    for (int attempt = 0; attempt < 24; ++attempt) {
        const int child = choose(live, rng);
        std::vector<int> parents = pair.model.liveIds(true);
        parents.erase(std::remove(parents.begin(), parents.end(), child), parents.end());
        const int parent = choose(parents, rng);
        if (parent < 0 || pair.model.wouldCycle(child, parent)) continue;

        std::vector<int> foreign;
        for (int id : live) {
            if (id != child && pair.model.nodes[id].parent != parent) foreign.push_back(id);
        }
        if (foreign.empty()) continue;
        pair.setParent(child, parent, choose(foreign, rng), counters);
        return true;
    }
    return false;
}

void runFuzz(uint64_t seed, size_t steps, Counters& totals) {
    GraphPair pair;
    std::mt19937_64 rng(seed);
    Counters counters;

    std::vector<int> initial;
    for (int i = 0; i < 12; ++i) initial.push_back(pair.alloc(randomDetachedOwner(rng)));
    pair.setParent(initial[0], 0, -1, counters);
    pair.setParent(initial[1], initial[0], -1, counters);
    pair.setParent(initial[2], initial[0], -1, counters);
    pair.setParent(initial[3], 0, -1, counters);
    pair.setParent(initial[4], initial[3], -1, counters);
    pair.setParent(initial[5], initial[4], -1, counters);
    pair.check(seed, 0);

    for (size_t step = 1; step <= steps; ++step) {
        const std::vector<int> live = pair.model.liveIds();
        const uint64_t op = rng() % 100;

        if ((op < 20 && live.size() < 128) || live.empty()) {
            pair.alloc(randomDetachedOwner(rng));
        } else if (op < 35 && live.size() > 4) {
            pair.freeNode(choose(live, rng), randomDetachedOwner(rng));
        } else if (op < 70 && !live.empty()) {
            const int child = choose(live, rng);
            std::vector<int> parents = pair.model.liveIds(true);
            parents.erase(std::remove(parents.begin(), parents.end(), child), parents.end());
            const int parent = choose(parents, rng);
            if (parent >= 0) {
                int before = -1;
                const uint64_t beforeKind = rng() % 4;
                if (beforeKind == 1 && !pair.model.nodes[parent].children.empty()) {
                    std::vector<int> siblings = pair.model.nodes[parent].children;
                    siblings.erase(std::remove(siblings.begin(), siblings.end(), child), siblings.end());
                    before = choose(siblings, rng);
                } else if (beforeKind >= 2) {
                    std::vector<int> foreign;
                    for (int id : live) {
                        if (id != child && pair.model.nodes[id].parent != parent) foreign.push_back(id);
                    }
                    before = choose(foreign, rng);
                }
                pair.setParent(child, parent, before, counters);
            }
        } else if (op < 87 && !live.empty()) {
            pair.removeFromParent(choose(live, rng), randomDetachedOwner(rng));
        } else if (op < 94) {
            if (!tryIntentionalCycle(pair, rng, counters) && live.size() < 128) {
                pair.alloc(randomDetachedOwner(rng));
            }
        } else {
            if (!tryNonSiblingBefore(pair, rng, counters) && !live.empty()) {
                pair.removeFromParent(choose(live, rng), randomDetachedOwner(rng));
            }
        }

        ++counters.operations;
        pair.check(seed, step);
    }

    pair.proveOwnerFaultDetector(seed, steps + 1, counters);
    require(counters.cycleRejects > 0, seed, steps + 2,
            "fuzz run never exercised cycle rejection");
    require(counters.nonSiblingBeforeFallbacks > 0, seed, steps + 2,
            "fuzz run never exercised non-sibling before fallback");

    totals.operations += counters.operations;
    totals.cycleRejects += counters.cycleRejects;
    totals.nonSiblingBeforeFallbacks += counters.nonSiblingBeforeFallbacks;
    totals.ownerFaultsDetected += counters.ownerFaultsDetected;
}

uint64_t parseU64(const char* text) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (!text[0] || !end || *end) throw std::invalid_argument("invalid integer: " + std::string(text));
    return static_cast<uint64_t>(value);
}

size_t parseSize(const char* text) {
    const uint64_t value = parseU64(text);
    if (!value || value > std::numeric_limits<size_t>::max()) {
        throw std::invalid_argument("invalid step count: " + std::string(text));
    }
    return static_cast<size_t>(value);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 3) {
            std::cerr << "usage: " << argv[0] << " [seed] [steps]\n";
            return 2;
        }

        runCommandSegmentationContract();
        Counters totals;
        runDirectedContracts(totals);

        const size_t steps = argc >= 3 ? parseSize(argv[2]) : 2500;
        if (argc >= 2) {
            runFuzz(parseU64(argv[1]), steps, totals);
        } else {
            const uint64_t seeds[] = {
                0x0000000000000001ULL,
                0x9e3779b97f4a7c15ULL,
                0xd1b54a32d192ed03ULL,
                0x94d049bb133111ebULL,
                0x6a09e667f3bcc909ULL,
                0xbb67ae8584caa73bULL,
                0x3c6ef372fe94f82bULL,
                0xa54ff53a5f1d36f1ULL,
            };
            for (uint64_t seed : seeds) runFuzz(seed, steps, totals);
        }

        std::cout << "native_scenegraph_model_fuzz_test: ok"
                  << " operations=" << totals.operations
                  << " cycleRejects=" << totals.cycleRejects
                  << " nonSiblingBeforeFallbacks=" << totals.nonSiblingBeforeFallbacks
                  << " ownerFaultsDetected=" << totals.ownerFaultsDetected << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native_scenegraph_model_fuzz_test: FAIL: " << error.what() << '\n';
        return 1;
    }
}
