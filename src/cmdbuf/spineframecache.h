// spine 帧库共享缓存(Cocos SkeletonCache SHARED_CACHE 的等价物,按本架构适配)。
// 键 = (skeletonData × 皮肤 × 动画)。同 key 的第一个白色实例(baker)照常走完整管线
// (JS 驱动→C++ 骨骼→蒙皮→emitCached),宿主在 emitCached 出口按 frameIdx=floor(trackTime/FrameTime)
// 把产出的 quad 块捕进本仓(30fps 采样,边播边烘,动画补齐一整圈后 complete)。
// 其余实例 bind 后由 C++ 在 present 流程里统一推进播放头:帧号变了才把该帧 quad 数组拷贝
// 挂到实例节点(SceneGraph::setNativeQuads → T_NATIVE_MULTIQUAD 渲染通道),帧号没变什么都不做
// (30fps 采样 → 60fps 播放天然隔帧零工作)。JS 对命中实例完全退出逐帧驱动(连 state.update
// 都不跑);complete 由本仓按圈数记账,JS 每帧一次批量读取给有监听的实例合成。
// 存储是独立 C++ 堆(std::vector<NativeQuad>),绝不进 65536 上限的共享 quad 池;
// 总字节超预算(64MB)时按 lastUse LRU 逐出未被绑定的 key。
#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <cstring>
#include <cmath>
#include "scenegraph.h"

namespace spfc {

static const double FRAME_MS = 1000.0 / 30.0;   // 30fps 采样:图标 idle 视觉无损,内存较 60fps 减半

struct AnimCache {
    std::vector<std::vector<sg::NativeQuad> > frames;   // frameIdx → quad 块(QSF=32 布局与共享池一致)
    std::vector<uint8_t> filled;
    int frameCount = 0, filledCount = 0;
    bool complete = false;
    uint64_t lastUse = 0;
    size_t byteSize = 0;
    int bindings = 0;                                   // 播放中的绑定数(>0 时 LRU 跳过)
};

struct Binding {
    int key = -1;
    double accMs = 0;      // 播放头(绑定时从实例 trackTime 相位接续)
    float rate = 1;        // trackEntry.timeScale × state.timeScale
    float r = 1, g = 1, b = 1, a = 1;   // 实例骨架色(烘焙侧恒白;非白实例拷贝时乘进 q[17]/q[18])
    int lastIdx = -1;      // 上次挂帧号;-1 = 下次推进强制补拷(绑定初帧/变色/复显)
    long lastLoop = 0;
    int pendingLoops = 0;  // 未被 JS 取走的 complete 圈数
};

class FrameStore {
public:
    size_t budgetBytes = 64ull << 20;

    int keyId(const std::string& s) {
        auto it = keyIds_.find(s);
        if (it != keyIds_.end()) return it->second;
        int id = ++keySeq_; keyIds_.emplace(s, id); return id;
    }

    // 烘焙武装:baker 的 __wrSpine 在调 __spEmitCached 前调用。allowEmpty=该姿势本就无可画记录
    // (合法空帧);否则 0 quad 视为发射间隙(纹理未就绪等)不入库。
    // 返回 2=key 已完成(baker 可退休) 1=本帧需要捕获(已武装) 0=该帧已有(无需捕获)。
    int arm(int instId, int key, int frameIdx, int frameCount, bool allowEmpty) {
        if (key <= 0 || frameCount <= 0 || frameIdx < 0 || frameIdx >= frameCount) return 0;
        AnimCache& a = anims_[key];
        if (a.frameCount == 0) { a.frameCount = frameCount; a.frames.resize((size_t)frameCount); a.filled.assign((size_t)frameCount, 0); }
        else if (a.frameCount != frameCount) {           // 同名动画时长变了(热替换):旧帧作废重烘
            invalidateKey(key, nullptr);
            AnimCache& b = anims_[key];
            b.frameCount = frameCount; b.frames.resize((size_t)frameCount); b.filled.assign((size_t)frameCount, 0);
            armInst_ = instId; armKey_ = key; armFrame_ = frameIdx; armAllowEmpty_ = allowEmpty; return 1;
        }
        if (a.complete) return 2;
        if (a.filled[(size_t)frameIdx]) return 0;
        armInst_ = instId; armKey_ = key; armFrame_ = frameIdx; armAllowEmpty_ = allowEmpty;
        return 1;
    }

    // emitCached 返回后收帧:游标处 n 个 quad(共享池布局)。n<0=池溢出重试中,保持武装等下一次调用。
    void capture(int instId, const float* sgbuf, int quadCursor, int n) {
        if (armKey_ < 0 || armInst_ != instId || !sgbuf) return;
        if (n < 0) return;
        int key = armKey_, frame = armFrame_;
        bool allowEmpty = armAllowEmpty_;
        armInst_ = 0; armKey_ = -1; armFrame_ = -1;
        if (n == 0 && !allowEmpty) return;               // 发射间隙(有可画记录却无 quad):不烘,该帧下一圈再补
        auto it = anims_.find(key);
        if (it == anims_.end()) return;
        AnimCache& a = it->second;
        if (frame < 0 || frame >= a.frameCount || a.filled[(size_t)frame]) return;
        std::vector<sg::NativeQuad> quads((size_t)n);
        const float* qb = sgbuf + sg::QUAD_BASE + 1;
        for (int i = 0; i < n; i++) {
            const float* src = qb + (size_t)(quadCursor + i) * sg::QUAD_STRIDE_F;
            if (src[0] < 0.0f) return;                   // slotObject 占位 quad(资格判定漏网)不可共享:放弃本帧
            std::memcpy(quads[(size_t)i].data(), src, sizeof(sg::NativeQuad));
        }
        a.frames[(size_t)frame] = std::move(quads);
        a.filled[(size_t)frame] = 1; a.filledCount++;
        size_t bytes = (size_t)n * sizeof(sg::NativeQuad);
        a.byteSize += bytes; totalBytes_ += bytes;
        statBaked_++;
        if (a.filledCount >= a.frameCount) a.complete = true;
        a.lastUse = ++useTick_;
        evictIfNeeded();
    }

    // 只有 complete 的 key 才接受绑定(不播半成品,避免烘焙空洞闪帧)。
    bool bind(int node, int key, double startMs, float rate, float r, float g, float b, float a) {
        if (node <= 0 || node >= sg::NODE_CAP || key <= 0) return false;
        auto it = anims_.find(key);
        if (it == anims_.end() || !it->second.complete || it->second.frameCount <= 0) { statRefused_++; return false; }
        Binding& bd = bindings_[node];
        if (bd.key > 0 && bd.key != key) {
            auto old = anims_.find(bd.key);
            if (old != anims_.end() && old->second.bindings > 0) old->second.bindings--;
        }
        if (bd.key != key) it->second.bindings++;
        bd.key = key;
        bd.accMs = startMs >= 0 ? startMs : 0;
        bd.rate = rate; bd.r = r; bd.g = g; bd.b = b; bd.a = a;
        bd.lastIdx = -1;
        bd.lastLoop = (long)(bd.accMs / FRAME_MS) / it->second.frameCount;
        bd.pendingLoops = 0;
        it->second.lastUse = ++useTick_;
        return true;
    }

    // 骨架色/速率低频变更(JS 检测到才调,不重置播放头)。变色需重拷当前帧。
    void tune(int node, float r, float g, float b, float a, float rate) {
        auto it = bindings_.find(node);
        if (it == bindings_.end()) return;
        Binding& bd = it->second;
        if (bd.r != r || bd.g != g || bd.b != b || bd.a != a) { bd.r = r; bd.g = g; bd.b = b; bd.a = a; bd.lastIdx = -1; }
        bd.rate = rate;
    }

    // 解绑并返回播放头 accMs(JS 用它把 trackTime 补齐,避免回退正常路径时动画倒跳);未绑定返 -1。
    double unbind(int node, sg::SceneGraph* graph) {
        auto it = bindings_.find(node);
        if (it == bindings_.end()) return -1;
        double acc = it->second.accMs;
        auto ak = anims_.find(it->second.key);
        if (ak != anims_.end() && ak->second.bindings > 0) ak->second.bindings--;
        bindings_.erase(it);
        if (graph) graph->clearNativeQuads(node);
        return acc;
    }

    // 失效:骨架结构性 mutation 中招的 key(通常是 baker 的半成品)全量作废。
    void invalidateKey(int key, sg::SceneGraph* graph) {
        auto it = anims_.find(key);
        if (it == anims_.end()) return;
        totalBytes_ -= it->second.byteSize > totalBytes_ ? totalBytes_ : it->second.byteSize;
        anims_.erase(it);
        if (armKey_ == key) { armInst_ = 0; armKey_ = -1; armFrame_ = -1; armAllowEmpty_ = false; }
        for (auto b = bindings_.begin(); b != bindings_.end();) {
            if (b->second.key == key) { if (graph) graph->clearNativeQuads(b->first); b = bindings_.erase(b); }
            else ++b;
        }
    }

    // present 流程统一推进全部绑定实例(JS 完全退出)。帧号没变什么都不做;隐藏链上的节点
    // 只推时间不拷帧(lastIdx 置 -1,复显后第一次推进立即补拷)。
    void advance(double nowMs, sg::SceneGraph* graph, const float* buf) {
        double dt = lastMs_ > 0 ? nowMs - lastMs_ : 0;
        lastMs_ = nowMs;
        if (bindings_.empty()) return;
        if (dt < 0) dt = 0;
        if (dt > 100) dt = 100;                          // 后台/断点恢复防时间洪水
        useTick_++;
        for (auto it = bindings_.begin(); it != bindings_.end();) {
            Binding& bd = it->second;
            auto ak = anims_.find(bd.key);
            if (ak == anims_.end() || ak->second.frameCount <= 0) {   // 防御:key 被清(正常只来自 clearAll)
                if (graph) graph->clearNativeQuads(it->first);
                it = bindings_.erase(it);
                continue;
            }
            AnimCache& a = ak->second;
            // 产品语义:不可见/屏外的 spine 完全冻结——时间也不推进(复显从暂停点继续),
            // 而不是"时间走、渲染跳"。隐藏期 pendingLoops 也不会产生(complete 不触发)。
            if (!chainVisible(graph, buf, it->first)) { bd.lastIdx = -1; ++it; continue; }
            bd.accMs += dt * bd.rate;
            long total = (long)(bd.accMs / FRAME_MS);
            long loop = total / a.frameCount;
            int idx = (int)(total % a.frameCount);
            if (loop > bd.lastLoop) { bd.pendingLoops += (int)(loop - bd.lastLoop); bd.lastLoop = loop; }
            a.lastUse = useTick_;
            if (idx != bd.lastIdx) { copyFrame(graph, it->first, a, idx, bd); bd.lastIdx = idx; statCopies_++; }
            statPlays_++;
            ++it;
        }
    }

    // complete 圈数批量读取:out 填 [node, loops] 对,返回对数并清零计数。
    int drainLoops(float* out, int capPairs) {
        if (!out || capPairs <= 0) return 0;
        int n = 0;
        for (auto& kv : bindings_) {
            if (kv.second.pendingLoops <= 0) continue;
            if (n >= capPairs) break;
            out[n * 2] = (float)kv.first;
            out[n * 2 + 1] = (float)kv.second.pendingLoops;
            kv.second.pendingLoops = 0;
            n++;
        }
        return n;
    }

    // [keys, completeKeys, frames, bytes, bindings, refused, evicted, copies]
    void stats(double out[8]) const {
        size_t frames = 0; int completeN = 0;
        for (const auto& kv : anims_) { frames += (size_t)kv.second.filledCount; if (kv.second.complete) completeN++; }
        out[0] = (double)anims_.size(); out[1] = (double)completeN; out[2] = (double)frames; out[3] = (double)totalBytes_;
        out[4] = (double)bindings_.size(); out[5] = (double)statRefused_; out[6] = (double)statEvicted_; out[7] = (double)statCopies_;
    }

    // 场景切换等主动清仓:全部帧数据+绑定作废(JS 侧 __spCacheClearAll 先逐实例 drop 再调这里)。
    void clearAll(sg::SceneGraph* graph) {
        for (auto& kv : bindings_) if (graph) graph->clearNativeQuads(kv.first);
        bindings_.clear(); anims_.clear(); totalBytes_ = 0;
        armInst_ = 0; armKey_ = -1; armFrame_ = -1; armAllowEmpty_ = false;
    }

    // JS 世界重建(reload):旧世界的 key 序号/骨架 id 全部失效,必须彻底归零。
    void resetAll(sg::SceneGraph* graph) {
        clearAll(graph);
        keyIds_.clear(); keySeq_ = 0; lastMs_ = 0; useTick_ = 0;
        statBaked_ = statCopies_ = statPlays_ = statRefused_ = statEvicted_ = 0;
    }

private:
    std::unordered_map<std::string, int> keyIds_;
    std::unordered_map<int, AnimCache> anims_;
    std::unordered_map<int, Binding> bindings_;          // nodeId → 播放绑定
    int keySeq_ = 0;
    uint64_t useTick_ = 0;
    size_t totalBytes_ = 0;
    double lastMs_ = 0;
    int armInst_ = 0, armKey_ = -1, armFrame_ = -1;
    bool armAllowEmpty_ = false;
    unsigned long statBaked_ = 0, statCopies_ = 0, statPlays_ = 0, statRefused_ = 0, statEvicted_ = 0;

    static bool chainVisible(sg::SceneGraph* graph, const float* buf, int node) {
        if (!graph || !buf) return true;
        int guard = 0;
        for (int i = node; i > 0 && i < sg::NODE_CAP && guard++ < sg::NODE_CAP; i = graph->nodes[i].parent) {
            if (!graph->nodes[i].alive) return false;
            if (buf[(size_t)i * sg::STRIDE + sg::VISIBLE] == 0.0f) return false;
        }
        return true;
    }

    void copyFrame(sg::SceneGraph* graph, int node, const AnimCache& a, int idx, const Binding& bd) {
        if (!graph || idx < 0 || idx >= (int)a.frames.size()) return;
        std::vector<sg::NativeQuad> out = a.frames[(size_t)idx];   // 值拷贝:节点持有独立数据,共享帧只读
        bool white = std::fabs(bd.r - 1) < 0.004f && std::fabs(bd.g - 1) < 0.004f && std::fabs(bd.b - 1) < 0.004f;
        bool opaque = std::fabs(bd.a - 1) < 0.004f;
        if (!white || !opaque) {
            for (auto& q : out) {
                if (!white) {
                    uint32_t t = (uint32_t)q[17];
                    int R = (int)(((t >> 16) & 255) * bd.r + 0.5f), G = (int)(((t >> 8) & 255) * bd.g + 0.5f), B = (int)((t & 255) * bd.b + 0.5f);
                    if (R > 255) R = 255; if (R < 0) R = 0;
                    if (G > 255) G = 255; if (G < 0) G = 0;
                    if (B > 255) B = 255; if (B < 0) B = 0;
                    q[17] = (float)((R << 16) | (G << 8) | B);
                }
                if (!opaque) { float al = q[18] * bd.a; q[18] = al < 0 ? 0 : (al > 1 ? 1 : al); }
            }
        }
        graph->setNativeQuads(node, std::move(out));
    }

    void evictIfNeeded() {
        while (totalBytes_ > budgetBytes) {
            int victim = -1; uint64_t oldest = ~0ull;
            for (const auto& kv : anims_) {
                if (kv.second.bindings > 0) continue;    // 播放中的 key 不逐出
                if (kv.second.lastUse < oldest) { oldest = kv.second.lastUse; victim = kv.first; }
            }
            if (victim < 0) break;
            auto it = anims_.find(victim);
            totalBytes_ -= it->second.byteSize > totalBytes_ ? totalBytes_ : it->second.byteSize;
            if (armKey_ == victim) { armInst_ = 0; armKey_ = -1; armFrame_ = -1; }
            anims_.erase(it);
            statEvicted_++;
        }
    }
};

} // namespace spfc
