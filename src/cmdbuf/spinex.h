// spinex.h — spine 全量 native 化的宿主层。
//
// P0(已对拍 22/22 PASS):官方 spine-cpp 4.2 数据构建 + 数值对拍骨架(DataStore/Instance/__spxProbe)。
// P1a:C++ 侧实例/驱动/渲染闭环 + 全部桥接口。
//   - DataStore:整型 sdHandle 化 + 引用计数 + inputCrc 失效 + 动画/事件/槽/皮肤名字表(build 一次性下发,
//     此后 JS 门面全部用 index 通信,杜绝每帧字符串桥)。
//   - NativeTextureLoader:load() 时经宿主注入的 requestFn 登记单页 rehydrate 闭包；首个实例
//     才异步解码页图(stableName → requestImageDataNamed → cid)，最后实例离场退还 GPU 引用。
//   - Runtime:实例表(instId → Instance)+ 每帧统一驱动 updateAll(present 入口调用,对标
//     MiddlewareManager::update)+ RenderCommand→NativeQuad 转换 + 事件环形队列 + 字符串表。
// P2:slotObject 骨骼跟随(__spxSlotBind/__spxSlotUnbind)。
//   - 矩阵:每帧把 slot 骨骼的 position+worldRotationX+正 worldScale 分解矩阵直写子节点局部矩阵槽
//     (LA..LTY,T_SPINE_SLOT 节点由 computeNode 直读),严格对齐 pixi-spine updateSlotObject 与旧路径
//     applySkelSlotMatrices——不能把骨骼原始仿射矩阵交给子节点,反射/shear 会镜像挂载的文字/按钮。
//   - 可见性/alpha:EFX 通道合成(Phase E:ALPHA/VISIBLE 槽=业务 base;合成因子=EFX 区段,
//     efxA=skel.a×slot.a、efxV=follow 且 attachment 空→0,由 computeNode WALPHA 派生链应用)
//     叠加在 base 之上,防每帧覆写游戏侧 hide/淡出意图。
//   - drawOrder 交错:绑定实例改按 drawOrder 逐 slot 发射(emitInterleaved),在绑定 slot 之后插
//     marker quad(q[0]=-(float)childNodeId,其余 0)。batcher T_NATIVE_MULTIQUAD 分支照 T_MULTIQUAD
//     的约定按 -q[0] 递归子节点(fromSpineMarker);子节点 TYPE 印成 T_SPINE_SLOT,children 正常序在
//     walk 顶部被跳过(防重画,同旧 __skelSlotBind 约定)。
// P3(本阶段):clz 子类收编(__SPINE_NATIVE_FULL=2 基类替换)配套。
//   - manual clock(业务喂时间):fts 的 12 个 extends Spine 子类全部 autoUpdate=false、由业务
//     tick 链条件性调 super.update(s) 表达时间闸门(IntervalSpine 间隔重播/MSpineGround 的
//     animation_interval/M111·M132 的 FPS<30 暂停/BitmapSpine 的 cacheAsBitmap 静止)。C++ 若无
//     条件自动推时间,这些闸门语义全部丢失(jackpot 数字牌会连续循环)。因此子类门面实例切
//     manual 模式(__spxSetClockMode):C++ 不再用墙钟 dt,只消费 JS 喂进来的 pendingDt
//     (__spxFeedTime/门面 update(s) 批量转发)——喂=推,不喂=冻结,与旧世界 super.update 依赖
//     完全同构。蒙皮/发射/事件仍全在 C++ present 内;父链真实隐藏时喂的时间照走，
//     只省蒙皮+发射。仅渲染 AABB 出屏不是冻结或降级依据。
//   - skeleton tint(__spxSetSkeletonColor):SpineView.setTint→this.tint→spine-pixi 语义为
//     skeleton.color RGB(alpha 不动),emit 的三级颜色乘积已含 skeleton color,设置即生效。
//
// 关键语义(与 H5 的逐 ticker 驱动对齐,见 native_scenegraph.js 驱动闸门):
//   - 不可见父链 → 自动钟冻结；可见父链上的动画持续更新 skeleton physics/world
//     transform/蒙皮/发射，不以上一帧渲染 AABB 出屏为冻结依据。AABB 是上一次发射姿态的结果，
//     不是未来姿态的输入；用它跳过几何更新会让“从屏外进入”的动画永久留在屏外。
//     manual 钟仍严格以业务喂入为准；父链真实隐藏时，有喂入只推 state 以保留事件语义。
//   - 屏内可见实例每帧完整执行 state update/apply、骨骼更新、蒙皮与发射，不做 Native 隔帧降频。
//   - 首帧纹理 cid 未就绪(异步解码中)→ 整实例冻结且丢弃预滚时间；首次成功
//     发射 setup-time 姿态后才开放业务时钟，避免外部 UI/timer 领先于空骨架。
//
// 约定:本头可被同一可执行体的多个 TU include(2026-09-02 起 src/host/host_core.mm 与平台宿主共用);
// spine::getDefaultExtension() 是 spine_cpp 静态库引用的宿主钩子,须真实发射符号(inline 在无人 ODR-use 的 TU
// 里不发射→链接缺符号),又要允许多 TU include(单 TU 的 C++ 测试、Android、Apple 双 TU)——故用弱定义:
// 每个 include 它的 TU 各发射一份等价弱符号,链接器保留一份。
#pragma once
#include "../common/native_diag_log.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <spine/SkeletonBinary.h>
#include <spine/spine.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "scenegraph.h"
#include "spinex_clock_policy.h"

// spine-cpp 要求宿主提供全局内存/IO 扩展点;默认实现走 malloc/free。
__attribute__((weak)) spine::SpineExtension *spine::getDefaultExtension() {
    return new spine::DefaultSpineExtension();
}

namespace spinex {

// AtlasPage 纹理槽。骨架预解析只建立 Atlas/SkeletonData，不得顺带上传并硬引用所有页图；
// 否则预加载但从未实例化的数百个 spine 也会把 GPU 工作集钉在预算之外。请求函数随槽保存，
// 首个 Instance 创建时 activate，最后一个 Instance 销毁时 deactivate；再次使用可重新激活。
// shared_ptr + generation 使“实例已离场但旧解码回调晚到”安全：宿主回调发现代际变化后丢弃。
struct TexState;
using TexStatePtr = std::shared_ptr<TexState>;
using TexRequestFn = std::function<void(const std::string &pageName, const TexStatePtr &state)>;
using TexRetainFn = std::function<bool(uint32_t cid)>;
using TexReleaseFn = std::function<void(uint32_t cid)>;
struct TexState {
    uint32_t cid = 0;       // native 纹理句柄(decoder id);0=未就绪
    int width = 0, height = 0;
    bool pma = false;       // atlas 页声明的预乘标志(存档)
    bool needsPremultiply = false; // 动态借用的 Pixi/KTX 纹理可能为直通 alpha；原生解码页已预乘
    bool failed = false;    // 解码/条目缺失等永久失败:该页命令跳过,不再阻塞整实例
    bool retained = false;  // 宿主已对 cid 加 ResourceManager 引用(entry 销毁时对称 release)
    bool abandoned = false; // 所属 entry 已销毁:晚到的解码回调不得再 retain(防孤儿引用泄漏)
    bool requested = false; // 本次活跃期是否已发起页图请求（同一 Entry 多实例只请求一次）
    uint64_t generation = 0;// activate/deactivate/销毁均换代，拦截上一活跃期的晚到回调
    std::string pageName;
    TexRequestFn request;   // 捕获 prepared 页图源；Entry 存活期间可按需反复 rehydrate
};

struct NativeTexture {
    std::string name;   // atlas 页图文件名("chest.png";dir 传空串时即页名,与 zip 条目名同源)
    TexStatePtr state;
};

class NativeTextureLoader : public spine::TextureLoader {
public:
    // 仅在 DataStore::buildFromPrepared 构造 Atlas 期间有效(load 由 Atlas 解析同步回调)。
    const TexRequestFn *activeRequest = nullptr;
    std::vector<TexStatePtr> *collect = nullptr;
    void load(spine::AtlasPage &page, const spine::String &path) override {
        NativeTexture *tex = new NativeTexture();
        tex->name = path.buffer() ? path.buffer() : "";
        tex->state = std::make_shared<TexState>();
        tex->state->pma = page.pma;
        tex->state->width = page.width;
        tex->state->height = page.height;
        page.texture = tex;
        if (collect) collect->push_back(tex->state);
        // 这里只让宿主把 prepared 包裁成“本页自己的轻量请求闭包”，不发起解码/上传。
        // 闭包不能直接保存整个 prepared holder，否则延迟 GPU 又会变成整包 CPU 引用堆积。
        tex->state->pageName = tex->name;
        if (activeRequest && *activeRequest) (*activeRequest)(tex->name, tex->state);
    }
    void unload(void *texture) override {
        delete static_cast<NativeTexture *>(texture);   // state 的 shared_ptr 可能仍被回调/Entry 持有
    }
};

// key → SkeletonData 存储。Atlas 必须活得比 SkeletonData 久(attachment 引用 atlas region),
// 释放顺序固定:先 SkeletonData 后 Atlas。SkeletonJson 解析器本身不需要跟随数据存活。
// P1a:整型 handle(不复用,单调递增)+ 引用计数(build=retain,__spxRelease=release;
// refs≤0 且无活体实例时销毁)+ inputCrc 失效 + Animation*/EventData* → index 反查表(事件队列用)。
class DataStore {
public:
    struct Entry {
        // Async Atlas keeps its TextureLoader pointer after promotion out of the job.
        std::shared_ptr<NativeTextureLoader> asyncTextureLoader;
        spine::Atlas *atlas = nullptr;
        spine::SkeletonData *data = nullptr;
        int handle = 0;            // 整型 sdHandle(避免每帧字符串查表);永不复用
        uint32_t inputCrc = 0;     // 构建时 prepared 包的 crc;变化即失效重建(无活体时)
        int refs = 0;              // JS 侧引用(__spxBuild++/__spxRelease--)
        int liveInstances = 0;     // 活体 Instance 数;>0 时禁止销毁/重建(SkeletonData 指针被持有)
        // 骨架 → 动画名对不上(Animation not found)→ 符号 initView reject → 轮面空格子。
        bool jsReleased = false;
        std::string key, error;
        std::vector<TexStatePtr> textures;                  // 本条目全部页纹理(销毁时对称 release cid)
        std::unordered_map<const void *, int> animIndex;    // spine::Animation* → 名字表 index
        std::unordered_map<const void *, int> eventIndex;   // spine::EventData* → 名字表 index
    };

    // 与 JS 运行环境对齐:spine-pixi 全局 Skeleton.yDown=true(Pixi y 轴向下)。spine-cpp 的
    // 对应开关是 Bone::yDown 静态量,效果同源。不设则 worldY/c/d 全部反号(P0 对拍首轮 FAIL 根因)。
    DataStore() { spine::Bone::setYDown(true); }
    ~DataStore() { clearAll(); }

    void setTexRelease(TexReleaseFn fn) { texRelease_ = std::move(fn); }

    // 由 NativeSpinePrepared 的产物构建:atlas 文本 + skeleton JSON(jsonText 需 NUL 结尾,
    // spine-cpp 的 Json 解析器按 C 字符串读)。key 已存在且 inputCrc 一致时直接复用(不重复解析);
    // inputCrc 变化:无活体 → 销毁重建(新 handle);有活体 → 保留旧数据并告警(指针安全优先)。
    // texRequest 在 Atlas 解析期对每页同步回调一次，但只配置单页延迟请求，不得发起 GPU 上传。
    // skelData/skelLen 非空时走 spine 官方二进制格式(.skel),否则退回 JSON。二进制解析比 JSON
    // 快数倍:749-slot 的 m95009_lock_jackpot 在 JSON 下要 199ms(mac)/293ms(iOS),整帧 341ms+,
    // 正好卡在 loading 94% 处把 spine 动画冻住。两条路径产出同一个 SkeletonData,可随时回退。
    // ---- 异步构建(预热用)----
    // 拆成三段:① 主线程 prepareAsync 建 Atlas(必须在主线程,因为 Atlas 解析期会同步回调
    // texRequest 去碰 ResourceManager 的非线程安全表);② 后台 runAsync 只跑 parseSkeleton;
    // ③ 主线程 commitAsync 登记 Entry。Atlas 在 ①③ 之间只被后台只读引用,不再改动。
    // 目的:m95009_lock_jackpot 这类骨架的 200ms+ 解析原本发生在 loading 94% 的主线程上,
    // 把它挪到 preparse 一完成就开跑的后台,业务真正要用时通常已经算完。
    struct AsyncBuild {
        std::string key;
        // Atlas 会保存构造时传入的 TextureLoader* 并在析构时回调 unload。
        // 预热 job 由后台队列 shared_ptr 延寿，可能比创建它的 DataStore 更晚析构；
        // 因此它不能借用 DataStore::textureLoader_。job 与晋升后的 Entry 共同持有
        // loader；两条销毁路径都先删 Atlas，再释放 loader。
        std::shared_ptr<NativeTextureLoader> textureLoader = std::make_shared<NativeTextureLoader>();
        spine::Atlas *atlas = nullptr;
        std::string jsonText;
        std::vector<unsigned char> skel;
        uint32_t inputCrc = 0;
        std::vector<TexStatePtr> textures;
        spine::SkeletonData *data = nullptr;
        std::string error;
        // 跨线程标志必须是 atomic:后台线程先写 data 再置 finished,主线程见到 finished 为真后才读
        // data。普通 bool 既无内存屏障(data 的写入可能对主线程不可见 → 拿到半初始化指针),也无
        // acquire/release(主线程可能一直读到缓存旧值 → while 空转挂死)。
        std::atomic<bool> finished{false};
        bool committed = false;     // 只在主线程读写,无需 atomic
        // 预热是"先算再说"的:业务不一定用得上每一个骨架(reload 也会丢弃未消费的 job)。
        // 没有析构就会把 Atlas + SkeletonData 一起漏掉——SkeletonData 含全部动画数据,量很大。
        // commitAsync 成功时会把两个指针置空(所有权转移给 Entry),所以这里删到的一定是没人接手的。
        // 顺序固定:先 SkeletonData 后 Atlas(attachment 引用 atlas region)。
        // 注意:纹理引用不能在这里还——释放要走 DataStore::texRelease_,析构函数够不着。
        // 未消费的 job 必须经 discardAsync() 丢弃,它会先归还纹理再销毁本对象。
        ~AsyncBuild() {
            if (data) { delete data; data = nullptr; }
            if (atlas) { delete atlas; atlas = nullptr; }
        }
        AsyncBuild() = default;
        AsyncBuild(const AsyncBuild&) = delete;             // 裸指针所有权,禁止拷贝
        AsyncBuild& operator=(const AsyncBuild&) = delete;
    };

    std::shared_ptr<AsyncBuild> prepareAsync(const std::string &key,
                                             const char *atlasText, int atlasLen,
                                             const char *jsonText,
                                             uint32_t inputCrc,
                                             const TexRequestFn &texRequest,
                                             const unsigned char *skelData, int skelLen) {
        if (!atlasText || atlasLen <= 0) return nullptr;
        auto job = std::make_shared<AsyncBuild>();
        job->key = key; job->inputCrc = inputCrc;
        if (jsonText) job->jsonText = jsonText;
        if (skelData && skelLen > 0) job->skel.assign(skelData, skelData + skelLen);
        std::vector<TexStatePtr> collected;
        job->textureLoader->activeRequest = &texRequest;
        job->textureLoader->collect = &collected;
        job->atlas = new spine::Atlas(atlasText, atlasLen, "", job->textureLoader.get(), true);
        job->textureLoader->activeRequest = nullptr;
        job->textureLoader->collect = nullptr;
        if (job->atlas->getPages().size() == 0) { delete job->atlas; job->atlas = nullptr; return nullptr; }
        job->textures = std::move(collected);
        return job;
    }
    // 后台线程执行。只读 job->atlas 与文本,不碰 DataStore。
    static void runAsync(const std::shared_ptr<AsyncBuild> &job) {
        if (!job || !job->atlas) return;
        job->data = parseSkeleton(job->key, job->atlas, job->jsonText.empty() ? nullptr : job->jsonText.c_str(),
                                  job->skel.empty() ? nullptr : job->skel.data(), (int)job->skel.size(), job->error);
        job->finished.store(true, std::memory_order_release);   // 与主线程的 acquire 配对
    }
    // 主线程执行:把后台算好的 SkeletonData 登记成正式 Entry。失败时负责回收 Atlas。
    Entry *commitAsync(const std::shared_ptr<AsyncBuild> &job, std::string *outError = nullptr) {
        if (!job || !job->finished.load(std::memory_order_acquire) || job->committed) return nullptr;
        job->committed = true;
        if (!job->data) {
            releaseJobTextures(job);   // 解析失败的 job 也已 retain 过页图,不退还即漏
            if (job->atlas) { delete job->atlas; job->atlas = nullptr; }
            if (outError) *outError = "readSkeletonData: " + (job->error.empty() ? std::string("no skeleton source") : job->error);
            return nullptr;
        }
        auto it = entries_.find(job->key);
        if (it != entries_.end() && it->second.data) {
            if (it->second.inputCrc == job->inputCrc) {   // 期间已被同步路径建好:丢弃本次结果,避免双份
                // job 的页图与在位条目同 stableName 同 cid,但 job 的 TexRequest 回调另 retain 了
                // 一份——丢弃结果必须对称退还,否则每次"预热撞同步"都让这些 cid 引用永久 +1。
                releaseJobTextures(job);
                delete job->data; job->data = nullptr;
                if (job->atlas) { delete job->atlas; job->atlas = nullptr; }
                return &it->second;
            }
            fprintf(stderr, "[spinex] key=%s 预热落地遇旧条目 crc %08x→%08x,退休让位\n",
                    job->key.c_str(), it->second.inputCrc, job->inputCrc);
            if (it->second.liveInstances > 0 || it->second.refs > 0) retireEntry(job->key);
            else destroyEntryByKey(job->key);
        }
        Entry *registered = registerEntry(job->key, job->atlas, job->data, job->inputCrc, std::move(job->textures));
        registered->asyncTextureLoader = job->textureLoader;
        job->atlas = nullptr; job->data = nullptr;   // 所有权已转移给 Entry,置空以免析构里二次释放
        return registered;
    }

    // 丢弃一个未被 commit 的预热 job(业务始终没用到该骨架 / reload 清仓 / 同名资源换代)。
    // 必须走这里而不是直接让 shared_ptr 析构：统一作废请求代际并释放页级 prepared 闭包；
    // 若未来出现预热 job 被提前激活的路径，已 retain 的 cid 也会在这里对称退还。
    void discardAsync(const std::shared_ptr<AsyncBuild> &job) {
        if (!job || job->committed) return;
        releaseJobTextures(job);   // data/atlas 由 ~AsyncBuild 负责(所有权未转移时析构自删)
        job->committed = true;   // 标记已处置,防重复丢弃
    }
    void releaseJobTextures(const std::shared_ptr<AsyncBuild> &job) {
        for (auto &t : job->textures) {
            if (!t) continue;
            ++t->generation;
            if (texRelease_ && t->retained && t->cid) { texRelease_(t->cid); t->retained = false; }
            t->abandoned = true;
            t->request = TexRequestFn();
        }
        job->textures.clear();
    }

    // 纯解析:只读 atlas 与骨架文本,不触碰任何 DataStore 状态,因此可以在后台线程执行。
    // 这是整条 build 链里唯一耗时的一段(749-slot 骨架 JSON 要 200ms+,二进制快数倍)。
    static spine::SkeletonData *parseSkeleton(const std::string &key, spine::Atlas *atlas,
                                              const char *jsonText,
                                              const unsigned char *skelData, int skelLen,
                                              std::string &parseErr) {
        spine::SkeletonData *data = nullptr;
        auto t0 = std::chrono::steady_clock::now();
        bool usedBinary = false;
        if (skelData && skelLen > 0) {
            spine::SkeletonBinary binary(atlas);
            data = binary.readSkeletonData(skelData, skelLen);
            usedBinary = true;
            if (!data) {
                parseErr = binary.getError().buffer() ? binary.getError().buffer() : "unknown";
                // 二进制失败不直接判死:同包若仍带 .json 就回退,保证资源过渡期不出现"整只 spine 消失"
                fprintf(stderr, "[spinex] key=%s .skel 解析失败(%s),回退 JSON\n", key.c_str(), parseErr.c_str());
                usedBinary = false;
            }
        }
        if (!data && jsonText && jsonText[0]) {
            spine::SkeletonJson parser(atlas);
            data = parser.readSkeletonData(jsonText);
            if (!data) parseErr = parser.getError().buffer() ? parser.getError().buffer() : "unknown";
        }
        if (data && getenv("SPINE_PREPARSE_LOG"))
            fprintf(stderr, "[spinex] key=%s 骨架解析 %s %.1fms\n", key.c_str(), usedBinary ? ".skel" : ".json",
                    std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count());
        return data;
    }

    Entry *buildFromPrepared(const std::string &key,
                             const char *atlasText, int atlasLen,
                             const char *jsonText,
                             uint32_t inputCrc,
                             const TexRequestFn &texRequest,
                             std::string *outError = nullptr,
                             const unsigned char *skelData = nullptr, int skelLen = 0) {
        auto it = entries_.find(key);
        if (it != entries_.end() && it->second.data) {
            Entry &old = it->second;
            if (old.inputCrc == inputCrc) return &old;
            // 停轮建符号失败 → 轮面空格子)。改为退休让位:旧条目改名保活(活体经 handle 继续
            // 访问,归零后 maybeDestroy 正常回收),新内容在原 key 上重建。
            if (old.liveInstances > 0 || old.refs > 0) {
                fprintf(stderr, "[spinex] key=%s inputCrc %08x→%08x 旧条目 refs=%d live=%d,退休让位重建\n",
                        key.c_str(), old.inputCrc, inputCrc, old.refs, old.liveInstances);
                retireEntry(key);
            } else {
                destroyEntryByKey(key);   // 无人持有:直接销毁重建
            }
        }
        // Modern assets are binary-only: a valid package has atlas + either .skel or JSON.
        // Requiring JSON here made the synchronous rebuild disagree with prepareAsync(), so a
        // cached package failed after its prior DataStore entry was released (for example when a
        // popup was closed and reopened). Keep both construction paths on the same input contract.
        const bool hasBinary = skelData && skelLen > 0;
        const bool hasJson = jsonText && jsonText[0];
        if (!atlasText || atlasLen <= 0 || (!hasBinary && !hasJson)) {
            if (outError) *outError = "empty atlas/skeleton input";
            return nullptr;
        }
        std::vector<TexStatePtr> collected;
        textureLoader_.activeRequest = &texRequest;
        textureLoader_.collect = &collected;
        spine::Atlas *atlas = new spine::Atlas(atlasText, atlasLen, /*dir=*/"", &textureLoader_,
                                               /*createTexture=*/true);
        textureLoader_.activeRequest = nullptr;
        textureLoader_.collect = nullptr;
        if (atlas->getPages().size() == 0) {
            delete atlas;
            if (outError) *outError = "atlas parse produced no pages";
            return nullptr;
        }
        std::string parseErr;
        spine::SkeletonData *data = parseSkeleton(key, atlas, jsonText, skelData, skelLen, parseErr);
        if (!data) {
            delete atlas;
            if (outError) *outError = "readSkeletonData: " + (parseErr.empty() ? std::string("no skeleton source") : parseErr);
            return nullptr;
        }
        return registerEntry(key, atlas, data, inputCrc, std::move(collected));
    }

    // 登记一条已构建好的 Entry。同步路径(buildFromPrepared)与异步路径(commitAsync)共用,
    // 保证两条路产出的 Entry 结构完全一致。
    Entry *registerEntry(const std::string &key, spine::Atlas *atlas, spine::SkeletonData *data,
                         uint32_t inputCrc, std::vector<TexStatePtr> &&textures) {
        Entry &entry = entries_[key];
        entry.atlas = atlas;
        entry.data = data;
        entry.handle = nextHandle_++;
        entry.inputCrc = inputCrc;
        entry.refs = 0;
        entry.liveInstances = 0;
        entry.key = key;
        entry.error.clear();
        entry.textures = std::move(textures);
        spine::Vector<spine::Animation *> &anims = data->getAnimations();
        for (size_t i = 0; i < anims.size(); ++i) entry.animIndex[anims[i]] = (int)i;
        spine::Vector<spine::EventData *> &events = data->getEvents();
        for (size_t i = 0; i < events.size(); ++i) entry.eventIndex[events[i]] = (int)i;
        handleToKey_[entry.handle] = key;
        return &entry;
    }

    Entry *find(const std::string &key) {
        auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : &it->second;
    }
    Entry *findByHandle(int handle) {
        auto it = handleToKey_.find(handle);
        return it == handleToKey_.end() ? nullptr : find(it->second);
    }
    void retain(Entry *e) {
        if (e) { e->refs++; e->jsReleased = false; }   // 重新被 JS 持有:撤销"旧机台残留"标记
    }
    // 返回剩余 refs;归零且无活体实例 → 立即销毁(有活体则等最后一个实例销毁时 maybeDestroy 收尾)。
    int release(Entry *e) {
        if (!e) return -1;
        if (e->refs > 0) e->refs--;
        int left = e->refs;
        if (left == 0) e->jsReleased = true;   // 活体让销毁延后时,给盲复用路径留下"已清理"证据
        maybeDestroy(e);
        return left;
    }
    void maybeDestroy(Entry *e) {
        if (e && e->refs <= 0 && e->liveInstances == 0 && e->handle) destroyEntryByKey(e->key);
    }
    void clearAll() {
        for (auto &kv : entries_) {
            releaseTextures(kv.second);
            delete kv.second.data;    // 先数据
            delete kv.second.atlas;   // 后图集
        }
        entries_.clear();
        handleToKey_.clear();   // nextHandle_ 继续单调递增,旧 handle 永不复活
    }
    size_t size() const { return entries_.size(); }
    int totalRefs() const {   // 泄漏监控([spinex] 5s 行):全部条目 JS 引用之和
        int n = 0;
        for (auto &kv : entries_) n += kv.second.refs;
        return n;
    }

    // GPU 驻留与 SkeletonData/JS 门面生命周期分离。预热只做 CPU 解析；真正出现第一个实例
    // 才请求页图。最后实例离场即退还硬引用，但保留请求源，池化门面重新挂树时可自愈重载。
    void activateTextures(Entry *e) {
        if (!e) return;
        for (auto &t : e->textures) {
            if (!t || t->abandoned || t->requested || !t->request) continue;
            t->requested = true;
            t->failed = false;
            ++t->generation;
            t->request(t->pageName, t);
        }
    }
    void deactivateTextures(Entry *e) {
        if (!e) return;
        for (auto &t : e->textures) {
            if (!t) continue;
            ++t->generation;                  // 先换代，晚到回调不得重新 retain
            if (texRelease_ && t->retained && t->cid) texRelease_(t->cid);
            t->cid = 0;
            t->retained = false;
            t->requested = false;
            t->failed = false;
        }
    }

    bool texturesReady(const Entry *e) const {
        if (!e) return false;
        for (const auto &t : e->textures)
            if (t && !t->failed && t->cid == 0) return false;
        return true;
    }

private:
    // 退休让位:同 key 需要承载新内容,而旧条目仍被 JS refs/活体实例持有。把旧条目改名为
    // "key#retired#handle"(handle 单调递增保唯一)继续存活——活体实例经 handle→handleToKey_
    // 找到它照常访问,refs/liveInstances 归零时 maybeDestroy 按 retired key 正常回收。
    // unordered_map::extract 转移节点不搬动 Entry 本体,已发出去的 Entry*/SkeletonData* 全部有效。
    void retireEntry(const std::string &key) {
        auto it = entries_.find(key);
        if (it == entries_.end()) return;
        char rk[192];
        snprintf(rk, sizeof(rk), "%s#retired#%d", key.c_str(), it->second.handle);
        auto node = entries_.extract(it);
        node.key() = rk;
        Entry &e = node.mapped();
        e.key = rk;
        handleToKey_[e.handle] = rk;
        entries_.insert(std::move(node));
    }
    void destroyEntryByKey(const std::string &key) {
        auto it = entries_.find(key);
        if (it == entries_.end()) return;
        Entry &e = it->second;
        releaseTextures(e);
        delete e.data;    // 先数据
        delete e.atlas;   // 后图集
        handleToKey_.erase(e.handle);
        entries_.erase(it);
    }
    void releaseTextures(Entry &e) {
        for (auto &t : e.textures) {
            if (!t) continue;
            ++t->generation;
            if (texRelease_ && t->retained && t->cid) { texRelease_(t->cid); t->retained = false; }
            t->abandoned = true;   // 在途解码回调不得再 retain(cid 留在 ResourceManager 缓存里可正常 GC)
            t->request = TexRequestFn();       // 释放捕获的 prepared 页图源
        }
        e.textures.clear();
    }
    NativeTextureLoader textureLoader_;                // 必须比所有 Atlas 活得久
    std::unordered_map<std::string, Entry> entries_;   // key = 业务 spine key;值地址稳定(node-based)
    std::unordered_map<int, std::string> handleToKey_;
    int nextHandle_ = 1;
    TexReleaseFn texRelease_;
};

class Runtime;

// AnimationState 全局监听:C++ 回调仅入环形队列(绝不进 JSC),JS 每帧 __spxDrainEvents 批量拉取。
// 用 ListenerObject(而非函数指针 typedef):需携带 instId/Entry 上下文,且默认编译无 std::function 版。
class InstListener : public spine::AnimationStateListenerObject {
public:
    Runtime *rt = nullptr;
    DataStore::Entry *entry = nullptr;   // Animation*/EventData* → index 反查
    int instId = 0;
    bool muted = false;                  // 销毁期置位:析构触发的 dispose 不再入队
    void callback(spine::AnimationState *state, spine::EventType type,
                  spine::TrackEntry *trackEntry, spine::Event *event) override;
};

// 单实例:Skeleton + AnimationState(+ P1a:场景图节点绑定、事件监听、首帧 prime 位)。
// __spxProbe 仍可临时构造(只传 SkeletonData*,不绑节点不入表)。
class Instance {
public:
    explicit Instance(spine::SkeletonData *data)
        : skeleton_(new spine::Skeleton(data)),
          stateData_(new spine::AnimationStateData(data)),
          state_(new spine::AnimationState(stateData_.get())) {}

    spine::Skeleton *skeleton() { return skeleton_.get(); }
    spine::AnimationState *state() { return state_.get(); }
    spine::AnimationStateData *stateData() { return stateData_.get(); }

    bool setAnimation(size_t track, const char *name, bool loop) {
        spine::Animation *anim = skeleton_->getData()->findAnimation(name);
        if (!anim) return false;
        state_->setAnimation(track, anim, loop);
        return true;
    }

    // 与 spine-ts 驱动序列一致:state.update → state.apply → skeleton.update →
    // skeleton.updateWorldTransform(Physics.update)。物理依赖 skeleton.update(dt) 推进 skeleton.time。
    // 该序列已由 P0 对拍背书,对拍时 JS 侧必须走同一序列。
    void update(float dt) {
        advanceState(dt, false);
        skeleton_->update(dt);
        skeleton_->updateWorldTransform(spine::Physics_Update);
    }

    // AnimationState.apply is part of the clock/lifecycle contract, not just rendering: Spine
    // dispatches timeline events and Complete from apply(). Offscreen work may skip skeleton
    // physics/world transforms and quad emission, but it must not skip this half of state advance.
    void advanceState(float dt, bool detectQueuedTransition = true) {
        const uintptr_t before = detectQueuedTransition ? trackSignature() : 0;
        state_->update(dt);
        state_->apply(*skeleton_);
        // addAnimation can promote its queued TrackEntry entirely inside update(). If this was a
        // StateOnly offscreen tick, the old animation's bounds are no longer valid for the new
        // entry. Re-prime on the next frame instead of letting stale culling persist forever.
        if (detectQueuedTransition && before != trackSignature()) primed = false;
    }

    int instId = 0;
    int nodeId = 0;                       // 绑定的场景图节点(0=已脱钩:节点被 JS 释放,实例冻结)
    bool primed = false;                  // 已成功发射过 quads;manual 未喂时仍先保首帧出画面
    bool visibilityKnown = false;         // updateAll 是否已观测过可见链
    bool wasChainVisible = false;         // 捕获 hidden→visible 边沿，复显时原子重发当前姿态
    // P3 manual clock:true=不用墙钟,只消费 JS 喂的 pendingDt(clz 子类的时间闸门语义,见文件头)。
    bool manualClock = false;
    float pendingDt = 0;                  // 本帧待消费的业务时间(秒);updateAll 取走清零
    bool textureGateOpen = false;         // 首帧成功发射前不消费动画时间
    DataStore::Entry *entry = nullptr;    // 所属数据条目(名字反查/活体计数)
    InstListener listener;                // 地址稳定(Instance 经 unique_ptr 持有)

private:
    uintptr_t trackSignature() {
        spine::Vector<spine::TrackEntry *> &tracks = state_->getTracks();
        uintptr_t signature = (uintptr_t)tracks.size() + 0x9e3779b9u;
        for (size_t i = 0; i < tracks.size(); ++i) {
            const uintptr_t p = reinterpret_cast<uintptr_t>(tracks[i]);
            signature ^= p + 0x9e3779b9u + (signature << 6) + (signature >> 2);
        }
        return signature;
    }

public:

    // P2 slotObject 跟随:slotIdx(authored) → 子节点绑定。可见性/alpha 合成走 EFX 通道
    // (Phase E:base/last 影子协议已废除,ALPHA/VISIBLE 槽=业务 base,合成因子=EFX 区段)。
    struct SlotBind {
        int nodeId = 0;
        bool follow = true;               // followAttachmentTimeline:attachment 空 → 隐藏
    };
    std::map<int, SlotBind> slotBinds;    // 量级个位数;有序遍历保诊断输出稳定
    // Pixi SlotMesh scale acts on the finished, skeleton-local vertices. It must not change
    // the slot's bone (which can also drive text/buttons) or shared attachment data.
    std::map<int, std::array<float, 2>> slotMeshScales;
    bool slotsSurrendered = false;        // E-1:驱动冻结已交还合成权威(applySlotFollow 重新取回)

    // SkeletonData/Skin 附件会被多实例共享。动态换图必须复制 Mesh，否则一个
    // 弹窗会篡改其他同骨架实例的 region/UV。copy + region + NativeTexture 都与
    // 本 Instance 同寿，Slot 持有的指针因此稳定。
    struct DynamicAttachment {
        std::unique_ptr<spine::Attachment> attachment;
        std::unique_ptr<spine::TextureRegion> region;
        NativeTexture texture;
        uint32_t retainedCid = 0;
    };
    std::unordered_map<std::string, std::unique_ptr<DynamicAttachment>> dynamicAttachments;

private:
    std::unique_ptr<spine::Skeleton> skeleton_;
    std::unique_ptr<spine::AnimationStateData> stateData_;
    std::unique_ptr<spine::AnimationState> state_;
};

// 实例运行时:每帧驱动 + RenderCommand→NativeQuad + 事件队列 + 字符串表。全部主线程(present 内)。
class Runtime {
public:
    struct LocalBounds {
        bool valid = false;
        float x = 0, y = 0, width = 0, height = 0;
        void include(float px, float py) {
            if (!std::isfinite(px) || !std::isfinite(py)) return;
            if (!valid) { x = px; y = py; valid = true; return; }
            const float right = std::max(x + width, px), bottom = std::max(y + height, py);
            x = std::min(x, px); y = std::min(y, py);
            width = right - x; height = bottom - y;
        }
    };
    // 事件 type 码(按任务约定 = spine-ts 监听器方法顺序;注意与 spine-cpp EventType 枚举不同序:
    // spine-cpp Complete=3/Dispose=4,此处 dispose=3/complete=4,onSpineEvent 显式映射)。
    enum { EVT_START = 0, EVT_INTERRUPT = 1, EVT_END = 2, EVT_DISPOSE = 3, EVT_COMPLETE = 4, EVT_EVENT = 5 };
    enum { EVENT_FLOATS = 8, EVENT_CAP = 1024, MAX_QUADS_PER_INST = 4096 };

    Runtime() {   // region 退化 quad 的固定索引(同 spine-cpp SkeletonRenderer)
        quadIndices_.add(0); quadIndices_.add(1); quadIndices_.add(2);
        quadIndices_.add(2); quadIndices_.add(3); quadIndices_.add(0);
    }

    // 动态 Mesh 直接采样 Pixi 上传的 ResourceManager cid。另持一份强引用，防止
    // Pixi TextureGC 因主屏画面由 C++ 提交而看不到活跃度，在动画中途回收 cid。
    void setDynamicTextureLifetime(TexRetainFn retain, TexReleaseFn release) {
        dynamicRetain_ = std::move(retain);
        dynamicRelease_ = std::move(release);
    }

    DataStore store;

    // ---- 实例管理 ----
    int createInstance(DataStore::Entry *entry, int nodeId) {
        if (!entry || !entry->data || nodeId <= 0 || nodeId >= sg::NODE_CAP) return 0;
        // 首个活体实例才激活页图；后续实例幂等共享同一批 TexState/cid。
        if (entry->liveInstances == 0) store.activateTextures(entry);
        int id = nextInstId_++;   // 单调递增,永不复用 → 旧 instId 悬空调用天然安全
        std::unique_ptr<Instance> inst(new Instance(entry->data));
        inst->instId = id;
        inst->nodeId = nodeId;
        inst->entry = entry;
        inst->listener.rt = this;
        inst->listener.entry = entry;
        inst->listener.instId = id;
        inst->state()->setListener(&inst->listener);
        inst->skeleton()->setToSetupPose();
        inst->skeleton()->updateWorldTransform(spine::Physics_Update);   // 首帧前姿态即可用
        entry->liveInstances++;
        instances_[id] = std::move(inst);
        return id;
    }

    bool destroyInstance(int instId, sg::SceneGraph *graph) {
        auto it = instances_.find(instId);
        if (it == instances_.end()) return false;
        Instance *inst = it->second.get();
        int node = inst->nodeId;
        DataStore::Entry *entry = inst->entry;
        inst->listener.muted = true;   // 析构链(~AnimationState)触发的 dispose 不入队
        purgeTrackKeys(instId);        // muted 析构不会再有 Dispose 回调帮我们清身份表
        // 挂载子节点归还容器语义(TYPE→T_CONTAINER,VISIBLE/ALPHA→base):门面销毁/重建后
        // 子节点可能被业务移走重用,残留 T_SPINE_SLOT 会被 walk 顶部永久跳过(不可见)。
        for (auto &kv : inst->slotBinds) restoreSlotNode(kv.second, graph);
        releaseDynamicAttachments(*inst);
        instances_.erase(it);
        purgeEvents(instId);           // 队列剔除该实例(含销毁前残留)
        if (graph && node > 0 && node < sg::NODE_CAP) graph->clearNativeQuads(node);
        if (entry) {
            if (entry->liveInstances > 0) entry->liveInstances--;
            if (entry->liveInstances == 0) store.deactivateTextures(entry);
            store.maybeDestroy(entry);   // refs 已归零的条目在最后一个实例销毁时收尾
        }
        return true;
    }

    bool setSlotMeshScale(int instId, int slotIdx, float sx, float sy, sg::SceneGraph *graph) {
        Instance *inst = find(instId);
        if (!inst || slotIdx < 0 || (size_t)slotIdx >= inst->skeleton()->getSlots().size() ||
            !std::isfinite(sx) || !std::isfinite(sy)) return false;
        const auto old = inst->slotMeshScales.find(slotIdx);
        if (old == inst->slotMeshScales.end() ? sx == 1 && sy == 1 :
            old->second[0] == sx && old->second[1] == sy) return true;
        if (sx == 1 && sy == 1) inst->slotMeshScales.erase(slotIdx);
        else inst->slotMeshScales[slotIdx] = {sx, sy};
        // Publish a layout mutation against the current pose without advancing the animation.
        inst->primed = false;
        if (graph) emit(*inst, graph);
        return true;
    }

    // ---- P2 slotObject 跟随:绑定管理(__spxSlotBind/__spxSlotUnbind 桥) ----
    // 绑定后:每帧矩阵直写 + 影子可见性合成(applySlotFollow),发射改走 drawOrder 交错(emitInterleaved)。
    bool bindSlot(int instId, int slotIdx, int nodeId, bool follow, sg::SceneGraph *graph) {
        Instance *inst = find(instId);
        if (!inst || slotIdx < 0 || (size_t)slotIdx >= inst->skeleton()->getSlots().size()) return false;
        if (nodeId <= 0 || nodeId >= sg::NODE_CAP || !graph || !graph->nodes[nodeId].alive) return false;
        Instance::SlotBind &b = inst->slotBinds[slotIdx];
        if (b.nodeId != nodeId) {
            if (b.nodeId) restoreSlotNode(b, graph);   // 换绑(GC 重挂拿了新 __i):旧节点归还容器语义
            b = Instance::SlotBind();                  // 影子重置(base 从新节点当前值重新采纳)
            b.nodeId = nodeId;
        }
        b.follow = follow;
        // Slot adoption is a publication boundary, not merely bookkeeping.  The bridge can run
        // after this frame's animation pass (pointer/microtask callbacks are allowed between
        // render phases).  Leaving the new child as an ordinary container until updateAll() runs
        // again publishes a mixed snapshot: the new Spine parent is already visible, while the
        // slot matrix and draw-order marker still belong to the previous frame.  A child at local
        // (0,0) therefore flashes at the skeleton origin for one presentation.
        //
        // Re-evaluate the current AnimationState without advancing time, acquire the slot matrix,
        // and rebuild the interleaved marker now.  The three writes become one synchronous
        // transaction for every slotObject user; the next scheduled update still consumes its
        // normal wall/manual delta.  If texture publication is not ready, TYPE remains
        // T_SPINE_SLOT, so normal tree traversal cannot expose the unpositioned child while the
        // later full pass retries emission.
        inst->primed = false;
        inst->update(0.0f);
        applySlotFollow(*inst, graph);
        if (!emit(*inst, graph)) inst->primed = false;
        return true;
    }
    bool unbindSlot(int instId, int slotIdx, sg::SceneGraph *graph) {
        Instance *inst = find(instId);
        if (!inst) return false;
        auto it = inst->slotBinds.find(slotIdx);
        if (it == inst->slotBinds.end()) return false;
        restoreSlotNode(it->second, graph);
        inst->slotBinds.erase(it);
        inst->primed = false;   // 下一帧全量重发射(去掉 marker)
        return true;
    }
    size_t slotBindCount() const {   // 泄漏/覆盖监控([spinex] 5s 行)
        size_t n = 0;
        for (auto &kv : instances_) n += kv.second->slotBinds.size();
        return n;
    }

    Instance *find(int instId) {
        auto it = instances_.find(instId);
        return it == instances_.end() ? nullptr : it->second.get();
    }

    // 对齐 spine-ts `attachment.region = region; attachment.updateRegion()`。UV 计算交给
    // 同版本 spine-cpp 4.2 MeshAttachment::updateRegion，桥层只负责纹理句柄与寿命。
    bool updateAttachmentRegion(int instId, int sourceSlotIdx, const char *attachmentName,
                                uint32_t cid, float u, float v, float u2, float v2,
                                int width, int height, int originalWidth, int originalHeight,
                                int degrees, float offsetX, float offsetY, bool needsPremultiply = false) {
        Instance *inst = find(instId);
        if (!inst || !attachmentName || !attachmentName[0] || !cid ||
            sourceSlotIdx < 0 || (size_t)sourceSlotIdx >= inst->skeleton()->getSlots().size() ||
            width <= 0 || height <= 0 || originalWidth <= 0 || originalHeight <= 0 ||
            !(u2 > u) || !(v2 > v) || !dynamicRetain_ || !dynamicRelease_) return false;
        spine::Attachment *source = inst->skeleton()->getAttachment(sourceSlotIdx, attachmentName);
        if (!source || !source->getRTTI().isExactly(spine::MeshAttachment::rtti)) return false;
        const std::string key = dynamicAttachmentKey(sourceSlotIdx, attachmentName);
        Instance::DynamicAttachment *dynamic = nullptr;
        auto found = inst->dynamicAttachments.find(key);
        if (found == inst->dynamicAttachments.end()) {
            std::unique_ptr<Instance::DynamicAttachment> made(new Instance::DynamicAttachment());
            made->attachment.reset(source->copy());
            if (!made->attachment || !made->attachment->getRTTI().isExactly(spine::MeshAttachment::rtti)) return false;
            made->region.reset(new spine::TextureRegion());
            made->texture.state = std::make_shared<TexState>();
            made->region->rendererObject = &made->texture;
            static_cast<spine::MeshAttachment *>(made->attachment.get())->setRegion(made->region.get());
            dynamic = made.get();
            inst->dynamicAttachments[key] = std::move(made);
        } else dynamic = found->second.get();
        if (!dynamic || !dynamic->region || !dynamic->texture.state) return false;
        if (dynamic->retainedCid != cid) {
            if (!dynamicRetain_(cid)) return false;
            if (dynamic->retainedCid) dynamicRelease_(dynamic->retainedCid);
            dynamic->retainedCid = cid;
            dynamic->texture.state->cid = cid;
        }
        dynamic->texture.state->needsPremultiply = needsPremultiply;
        spine::TextureRegion &region = *dynamic->region;
        region.u = u; region.v = v; region.u2 = u2; region.v2 = v2;
        region.width = width; region.height = height;
        region.originalWidth = originalWidth; region.originalHeight = originalHeight;
        region.degrees = degrees; region.offsetX = offsetX; region.offsetY = offsetY;
        static_cast<spine::MeshAttachment *>(dynamic->attachment.get())->updateRegion();
        inst->primed = false;
        return true;
    }

    // target slot 可与附件来源 slot 不同（Slot.setAttachment 接受任意 Attachment）。
    // 优先使用本实例的动态副本，否则使用 skin 原附件；name=null 清空。
    bool setSlotAttachment(int instId, int targetSlotIdx, int sourceSlotIdx, const char *attachmentName) {
        Instance *inst = find(instId);
        if (!inst || targetSlotIdx < 0 || (size_t)targetSlotIdx >= inst->skeleton()->getSlots().size()) return false;
        spine::Attachment *attachment = nullptr;
        if (attachmentName) {
            if (sourceSlotIdx < 0 || (size_t)sourceSlotIdx >= inst->skeleton()->getSlots().size()) return false;
            auto dynamic = inst->dynamicAttachments.find(dynamicAttachmentKey(sourceSlotIdx, attachmentName));
            attachment = dynamic == inst->dynamicAttachments.end() || !dynamic->second->retainedCid
                ? inst->skeleton()->getAttachment(sourceSlotIdx, attachmentName)
                : dynamic->second->attachment.get();
            if (!attachment) return false;
        }
        inst->skeleton()->getSlots()[targetSlotIdx]->setAttachment(attachment);
        inst->primed = false;
        return true;
    }
    size_t instanceCount() const { return instances_.size(); }
    // Read the same world matrices used by slot-follow/rendering. Queries do not run
    // AnimationState.apply, physics, or consume the pending manual-clock delta.
    bool boneWorldTransforms(int instId, float *out, size_t capacity) {
        Instance *inst = find(instId);
        if (!inst || !out) return false;
        const spine::Vector<spine::Bone *> &bones = inst->skeleton()->getBones();
        if (capacity / 6 != bones.size() || capacity % 6 != 0) return false;
        for (size_t i = 0; i < bones.size(); ++i) {
            spine::Bone &bone = *bones[i];
            float *p = out + i * 6;
            p[0] = bone.getA(); p[1] = bone.getB();
            p[2] = bone.getC(); p[3] = bone.getD();
            p[4] = bone.getWorldX(); p[5] = bone.getWorldY();
        }
        return true;
    }
    bool isDrawable(int instId) const {
        auto it = instances_.find(instId);
        return it != instances_.end() && it->second && it->second->primed;
    }

    // 场景图节点被 JS 释放(freeNode):绑定实例脱钩冻结(quads 已由 freeNode 清空;实例等 __spxDestroy)。
    // 不脱钩会在 slot 复用后把 quads 写到无关新主人身上。slotObject 绑定同理:节点 id 会被复用,
    // 不摘除会把矩阵/TYPE 写到无关新主人身上(JS 门面 tick 兜底会用新 __i 重绑)。
    void onNodeFreed(int nodeId) {
        if (nodeId <= 0) return;
        for (auto &kv : instances_) {
            if (kv.second->nodeId == nodeId) kv.second->nodeId = 0;
            auto &sb = kv.second->slotBinds;
            for (auto it = sb.begin(); it != sb.end();)
                it = (it->second.nodeId == nodeId) ? sb.erase(it) : std::next(it);
        }
    }

    // JS 世界重建(reload):旧 instId/sdHandle 全部失效,实例/条目/事件/字符串彻底清仓(纹理对称 release)。
    void resetAll() {
        for (auto &kv : instances_) kv.second->listener.muted = true;
        for (auto &kv : instances_) releaseDynamicAttachments(*kv.second);
        instances_.clear();
        events_.clear();
        store.clearAll();
        strings_.clear();
        stringIds_.clear();
        trackKeys_.clear();
        lastMs_ = 0;
    }

    // TrackEntry 指针不跨 JS 桥，但事件必须区分「同一 track 上同名动画的旧/新 entry」。
    // 返回值与生命周期事件的 eventIdx 空闲槽使用同一 key，避免旧 Dispose
    // 误删刚刚重播的新 entry；事件记录仍保持 8 float，允许 JS/宿主版本短暂交错。
    uint32_t registerTrackEntry(int instId, spine::TrackEntry *entry) {
        if (instId <= 0 || !entry) return 0;
        const uint32_t key = nextTrackKey_++;
        trackKeys_[entry] = TrackIdentity{instId, key};
        return key;
    }

    // ---- 每帧驱动(present 入口调用;对标 MiddlewareManager::update 一次驱动全部)----
    void noteIdleFrame(double nowMs) { lastMs_ = nowMs; }

    void updateAll(double nowMs, sg::SceneGraph *graph, const float *buf,
                   float viewW, float viewH) {
        double dtMs = lastMs_ > 0 ? nowMs - lastMs_ : 0;
        lastMs_ = nowMs;
        if (dtMs < 0) dtMs = 0;
        if (dtMs > 100) dtMs = 100;   // 后台/断点恢复防时间洪水(同 spineframecache.advance)
        if (instances_.empty() || !graph || !buf) return;
        const float dt = (float)(dtMs * 0.001);
        for (auto &kv : instances_) {
            Instance &inst = *kv.second;
            const int node = inst.nodeId;
            if (node <= 0 || node >= sg::NODE_CAP || !graph->nodes[node].alive)
                { surrenderSlotFollow(inst, graph); continue; }   // 脱钩:冻结(slotBinds 节点可能仍活着)
            // 父链隐藏是业务时钟闸门；上一姿态的渲染 AABB 只作诊断采样，
            // 不能决定新姿态是否生成(进场动画可能长时间从屏外向内运动)。
            const bool chainVis = chainVisible(graph, buf, node);
            // cacheAsBitmap 会把同一场景节点临时切成 sprite TYPE。隐藏期间 C++ 不发射，
            // primed 仍然为 true；如果复显帧也没有 manual delta，旧逻辑会把该 sprite 与新 Mask
            // 一起提交，形成一帧漏光。只在 hidden→visible 边沿撤销 prime，下方用 dt=0
            // 重建当前姿态和 T_NATIVE_MULTIQUAD，不推进时间、不重启动画。
            if (visibilityReactivation(chainVis, inst.visibilityKnown, inst.wasChainVisible))
                inst.primed = false;
            const bool outside = offscreen(buf, node, viewW, viewH);
            // A soft-evicted atlas rehydrates asynchronously on first use. Do not let an invisible
            // skeleton consume manual/wall-clock time during that gap. The first ready frame emits
            // the setup-time pose (dt=0), which is also the public "drawable" readiness boundary.
            const bool texturesReady = store.texturesReady(inst.entry);
            const TextureGateAction textureGate = textureGateAction(
                chainVis && texturesReady, inst.textureGateOpen, inst.pendingDt);
            if (textureGate == TextureGateAction::Waiting) {
                if (!texturesReady) statNotReady_++;
                statFrozen_++;
                surrenderSlotFollow(inst, graph);
                continue;
            }
            const bool firstDrawable = textureGate == TextureGateAction::FirstDrawable;
            // ---- P3 manual clock:clz 子类的业务时间闸门(喂=推,不喂=冻结;见文件头) ----
            if (inst.manualClock) {
                const float mdt = firstDrawable ? 0.0f : inst.pendingDt;
                inst.pendingDt = 0;
                const ClockAction action = clockAction(true, mdt, chainVis, outside, inst.primed);
                if (action == ClockAction::StateOnly) {
                    // 旧世界父链不可见时业务 tick 仍可能喂入(AutoMgr 只查自身
                    // visible)，此时只推 state 保持时间与事件连续，蒙皮+发射省掉。
                    inst.advanceState(mdt);
                    statClockOnly_++; statFrozen_++;
                    surrenderSlotFollow(inst, graph);
                    continue;
                }
                if (action == ClockAction::Frozen) {   // 未喂:业务闸门关着(interval 等待/FPS 暂停/位图静止)
                    if (!chainVis || outside) { statFrozen_++; surrenderSlotFollow(inst, graph); continue; }
                    applySlotFollow(inst, graph);   // 印章/影子仍要压住 JS 同帧覆写
                    statManualIdle_++;
                    continue;
                }
                inst.update(mdt);   // 未 primed 时 mdt 可为 0:全量跑一遍保首帧出画面(同自动钟契约)
                statFull_++;
                applySlotFollow(inst, graph);
                emit(inst, graph);
                continue;
            }
            const ClockAction action = clockAction(false, 0.0f, chainVis, outside, inst.primed);
            // 真正的父链隐藏仍冻结；可见父链一律生成完整姿态与几何。
            if (action == ClockAction::Frozen) {
                statFrozen_++;
                surrenderSlotFollow(inst, graph);
                continue;
            }
            if (action == ClockAction::StateOnly) {
                inst.advanceState(dt);
                statClockOnly_++; statFrozen_++;
                surrenderSlotFollow(inst, graph);
                continue;
            }
            inst.update(firstDrawable ? 0.0f : dt);
            statFull_++;
            applySlotFollow(inst, graph);
            emit(inst, graph);
        }
    }

    // ---- P3 clz 收编配套桥后端 ----
    // manual clock 切换(门面 __ensureInst 建立实例后按子类身份调用;幂等)。
    bool setClockMode(int instId, bool manual) {
        Instance *inst = find(instId);
        if (!inst) return false;
        inst->manualClock = manual;
        if (!manual) inst->pendingDt = 0;
        return true;
    }
    // 业务时间喂入(秒);累计到下一次 updateAll 一并消费。返回 false=实例不存在(门面下帧重试)。
    bool feedTime(int instId, float sec) {
        Instance *inst = find(instId);
        if (!inst) return false;
        if (sec > 0 && sec == sec) {   // NaN 防御
            if (sec > 10.0f) sec = 10.0f;   // 单帧钳制(同 dt 洪水防御量级)
            inst->pendingDt += sec;
        }
        return true;
    }
    // skeleton tint(spine-pixi `spine.tint` setter 同语义:只写 skeleton.color 的 RGB,alpha 不动)。
    bool setSkeletonColor(int instId, uint32_t rgb888) {
        Instance *inst = find(instId);
        if (!inst) return false;
        spine::Color &c = inst->skeleton()->getColor();
        c.r = (float)((rgb888 >> 16) & 255u) / 255.0f;
        c.g = (float)((rgb888 >> 8) & 255u) / 255.0f;
        c.b = (float)(rgb888 & 255u) / 255.0f;
        inst->primed = false;   // 颜色变了,下一帧强制全量出帧
        return true;
    }
    // Spine 4.2 Skeleton physics motion compensation. Display-object tweens move/rotate the
    // skeleton outside AnimationState, then call these methods so PhysicsConstraint keeps its
    // simulated world position instead of receiving a false impulse from the container motion.
    // Keep the operation in the shared Runtime so macOS/iOS expose one identical contract.
    bool physicsTranslate(int instId, float x, float y) {
        Instance *inst = find(instId);
        if (!inst || !std::isfinite(x) || !std::isfinite(y)) return false;
        inst->skeleton()->physicsTranslate(x, y);
        inst->primed = false;
        return true;
    }
    bool physicsRotate(int instId, float x, float y, float degrees) {
        Instance *inst = find(instId);
        if (!inst || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(degrees)) return false;
        inst->skeleton()->physicsRotate(x, y, degrees);
        inst->primed = false;
        return true;
    }
    size_t manualCount() const {   // 诊断([spinex] 5s 行):manual 模式实例数
        size_t n = 0;
        for (auto &kv : instances_) if (kv.second->manualClock) n++;
        return n;
    }

    // ---- P2 slotObject 跟随:矩阵直写 + EFX 合成因子(冻结实例经 surrender 交还,JS 保持全权) ----
    // E-1「驱动冻结即交还合成权威」:冻结/脱钩/纹理等待期间 applySlotFollow 不跑,挂接节点的
    // TYPE/LA/EFX 若继续持有,JS 覆写无人压回、efxV=0 冻住=无主第二真值(idx=751 凝固族实锤;
    // 复显/换父/查询用旧骨骼位置=闪现族)。进入冻结首帧软归还(复用 restoreSlotNode:尊重 JS
    // 已写 TYPE+EFX 复位+bit2+mark→canonical 回分量真值);解冻后 applySlotFollow 全量写+mark
    // 自然取回。父链隐藏期间画面不可见,归还引起的位置回退无视觉影响;复显首帧 drive 先于
    // phase5 收敛,final seal 即骨骼位置,无闪现窗口。
    void surrenderSlotFollow(Instance &inst, sg::SceneGraph *graph) {
        if (inst.slotsSurrendered) return;
        inst.slotsSurrendered = true;
        statSurrender_++;
        if (inst.slotBinds.empty() || !graph || !graph->buf) return;
        for (auto &kv : inst.slotBinds) restoreSlotNode(kv.second, graph);
    }

    // E-1 租约对偶:acquire(驱动执行=持有/取回合成权威)⇄ surrender(冻结/脱钩=交还)。
    // 计数器口径:每次「状态翻转」计 1(非每帧),浸泡里 acquire≈surrender±活跃实例数。
    void acquireSlotFollow(Instance &inst) {
        if (inst.slotsSurrendered) { inst.slotsSurrendered = false; statAcquire_++; }
    }

    void applySlotFollow(Instance &inst, sg::SceneGraph *graph) {
        acquireSlotFollow(inst);
        if (inst.slotBinds.empty() || !graph || !graph->buf) return;
        spine::Skeleton &skel = *inst.skeleton();
        spine::Vector<spine::Slot *> &slots = skel.getSlots();
        const float skelA = skel.getColor().a;
        for (auto &kv : inst.slotBinds) {
            const int slotIdx = kv.first;
            Instance::SlotBind &b = kv.second;
            const int node = b.nodeId;
            if (node <= 0 || node >= sg::NODE_CAP || !graph->nodes[node].alive) continue;
            if (slotIdx < 0 || (size_t)slotIdx >= slots.size()) continue;
            spine::Slot *slot = slots[slotIdx];
            spine::Bone &bone = slot->getBone();
            float *s = graph->buf + (size_t)node * sg::STRIDE;
            // 矩阵:严格对齐 pixi-spine updateSlotObject(position + worldRotationX + 恒正 worldScale),
            // 与旧路径 applySkelSlotMatrices 同款分解——不能把骨骼原始仿射矩阵直接交给普通 Pixi/FGUI
            // 子节点,骨骼内的反射或 shear 会把挂载的文字、按钮和列表镜像。yDown 已全局
            // setYDown(true),a/b/c/d/worldX/worldY 即最终值,无需再翻。
            const float wa = bone.getA(), wb = bone.getB(), wc = bone.getC(), wd = bone.getD();
            const float sx = sqrtf(wa * wa + wc * wc);
            const float sy = sqrtf(wb * wb + wd * wd);
            const float rot = atan2f(wc, wa);
            const float cs = cosf(rot), sn = sinf(rot);
            const float v[6] = {cs * sx, sn * sx, -sn * sy, cs * sy, bone.getWorldX(), bone.getWorldY()};
            bool changed = false;
            for (int k = 0; k < 6; k++)
                if (s[sg::LA + k] != v[k]) { changed = true; break; }
            if (changed)
                for (int k = 0; k < 6; k++) s[sg::LA + k] = v[k];
            // Phase E:base/effective 拆分(与 spineskin emitCached 同步改造)——ALPHA/VISIBLE 槽
            // 保持业务 base;合成因子写 EFX 通道,WALPHA 派生链应用;猜测采纳协议废除。
            const bool spineHidden = b.follow && slot->getAttachment() == nullptr;
            const float efxA = skelA * slot->getColor().a;
            const float efxV = spineHidden ? 0.0f : 1.0f;
            float* gb = graph->buf;
            changed = changed || (int)s[sg::TYPE] != sg::T_SPINE_SLOT
                   || gb[sg::EFX_ALPHA_BASE+node] != efxA || gb[sg::EFX_VIS_BASE+node] != efxV;
            s[sg::TYPE] = (float)sg::T_SPINE_SLOT;   // children 正常序被 walk 顶部跳过;只经 marker 绘制(防重画)
            gb[sg::EFX_ALPHA_BASE+node] = efxA;      // Phase E:合成走 EFX 通道,base 槽不动
            gb[sg::EFX_VIS_BASE+node]   = efxV;
            if (changed) graph->markLocalInputChanged(node);
        }
    }

    // 当前姿态 → NativeQuad 流 → graph.setNativeQuads。任一在用页 cid 未就绪(且未失败)→
    // 本帧整实例跳过发射(保持上帧 quads),返回 false。
    // 有 slotObject 绑定的实例走 emitInterleaved(逐 slot + marker 交错);无绑定沿用
    // SkeletonRenderer 合批路径(P1b 已实测背书,不动)。
    bool emit(Instance &inst, sg::SceneGraph *graph) {
        if (inst.nodeId <= 0) return false;
        if (!inst.slotBinds.empty() || !inst.slotMeshScales.empty()) return emitInterleaved(inst, graph);
        spine::RenderCommand *cmd = renderer_.render(*inst.skeleton());   // 单例即用即取:产物本轮立刻消费
        size_t tris = 0;
        for (spine::RenderCommand *c = cmd; c; c = c->next) {
            if (c->numIndices <= 0) continue;
            NativeTexture *tex = static_cast<NativeTexture *>(c->texture);
            if (tex && tex->state && !tex->state->failed && tex->state->cid == 0) {
                statNotReady_++;
                return false;
            }
            tris += (size_t)(c->numIndices / 3);
        }
        if (tris > (size_t)MAX_QUADS_PER_INST) tris = MAX_QUADS_PER_INST;
        std::vector<sg::NativeQuad> quads;
        quads.reserve(tris);
        for (spine::RenderCommand *c = cmd; c; c = c->next) {
            if (c->numIndices <= 0 || c->numVertices <= 0) continue;
            NativeTexture *tex = static_cast<NativeTexture *>(c->texture);
            const uint32_t cid = (tex && tex->state) ? tex->state->cid : 0;
            if (!cid) continue;   // 永久失败页:跳过该命令,其余照画
            // spine-cpp 命令内颜色恒定(batch 合并条件含 colors[0] 相等),取 [0] 即可。
            // 打包序 ARGB:(a<<24)|(r<<16)|(g<<8)|b —— 已含 skeleton×slot×attachment 三级乘积。
            float tint, alpha, dark, darkAlpha;
            colorParts(c->colors[0], c->darkColors[0], tint, alpha, dark, darkAlpha);
            const float blend = (float)blendCode(c->blendMode);
            const float *px = c->positions;
            const float *uv = c->uvs;
            const uint16_t *ix = c->indices;
            const int nv = c->numVertices;
            for (int t = 0; t + 2 < c->numIndices; t += 3)
                if (!pushTriQuad(quads, cid, px, uv, ix[t], ix[t + 1], ix[t + 2], nv,
                                 tint, alpha, blend, dark, darkAlpha, tex->state->needsPremultiply)) break;
        }
        graph->setNativeQuads(inst.nodeId, std::move(quads));
        inst.primed = true;
        statEmitted_++;
        return true;
    }

    // P2:drawOrder 逐 slot 发射(不合批,quad 数与合批路径一致),绑定 slot 之后插 marker quad
    // (q[0]=-(float)childNodeId,其余 0——batcher/bounds/hit/framecache 对 q[0]<0 的约定一致)。
    // 主体照抄 spine-cpp SkeletonRenderer::render 的逐 slot 循环(region/mesh/clipping/颜色合成),
    // 只是去掉 batchCommands(我们本来就按三角逐个出 quad,合批不带来任何收益)。
    bool emitInterleaved(Instance &inst, sg::SceneGraph *graph, LocalBounds *bounds = nullptr) {
        if (bounds) *bounds = LocalBounds();
        spine::Skeleton &skel = *inst.skeleton();
        spine::SkeletonClipping &clipper = clipper_;
        spine::Vector<spine::Slot *> &drawOrder = skel.getDrawOrder();
        const spine::Color &skc = skel.getColor();
        if (bounds && skc.a == 0) return true;
        std::vector<sg::NativeQuad> quads;
        quads.reserve(64);
        bool notReady = false, full = false;
        auto pushMarker = [&](int slotIdx) {
            if (bounds) return;
            auto bit = inst.slotBinds.find(slotIdx);
            if (bit == inst.slotBinds.end()) return;
            const int node = bit->second.nodeId;
            if (node <= 0 || node >= sg::NODE_CAP || !graph->nodes[node].alive) return;
            if (quads.size() >= (size_t)MAX_QUADS_PER_INST) { statTruncated_++; full = true; return; }
            quads.emplace_back();
            float *q = quads.back().data();
            std::memset(q, 0, sizeof(float) * sg::QUAD_STRIDE_F);
            q[0] = -(float)node;
        };
        for (size_t i = 0; i < drawOrder.size() && !notReady && !full; ++i) {
            spine::Slot &slot = *drawOrder[i];
            const int slotIdx = (int)slot.getData().getIndex();
            spine::Attachment *attachment = slot.getAttachment();
            if (!attachment) { clipper.clipEnd(slot); pushMarker(slotIdx); continue; }
            if ((slot.getColor().a == 0 || !slot.getBone().isActive()) &&
                !attachment->getRTTI().isExactly(spine::ClippingAttachment::rtti)) {
                clipper.clipEnd(slot);
                pushMarker(slotIdx);
                continue;
            }
            spine::Vector<float> *vertices = &worldVertices_;
            int verticesCount;
            spine::Vector<float> *uvs;
            spine::Vector<unsigned short> *indices;
            spine::Color *attachmentColor;
            spine::TextureRegion *region;
            if (attachment->getRTTI().isExactly(spine::RegionAttachment::rtti)) {
                spine::RegionAttachment *ra = (spine::RegionAttachment *)attachment;
                attachmentColor = &ra->getColor();
                if (attachmentColor->a == 0) { clipper.clipEnd(slot); pushMarker(slotIdx); continue; }
                worldVertices_.setSize(8, 0);
                ra->computeWorldVertices(slot, worldVertices_, 0, 2);
                verticesCount = 4;
                uvs = &ra->getUVs();
                indices = &quadIndices_;
                region = ra->getRegion();
            } else if (attachment->getRTTI().isExactly(spine::MeshAttachment::rtti)) {
                spine::MeshAttachment *mesh = (spine::MeshAttachment *)attachment;
                attachmentColor = &mesh->getColor();
                if (attachmentColor->a == 0) { clipper.clipEnd(slot); pushMarker(slotIdx); continue; }
                worldVertices_.setSize(mesh->getWorldVerticesLength(), 0);
                mesh->computeWorldVertices(slot, 0, mesh->getWorldVerticesLength(),
                                           worldVertices_.buffer(), 0, 2);
                verticesCount = (int)(mesh->getWorldVerticesLength() >> 1);
                uvs = &mesh->getUVs();
                indices = &mesh->getTriangles();
                region = mesh->getRegion();
            } else if (attachment->getRTTI().isExactly(spine::ClippingAttachment::rtti)) {
                clipper.clipStart(slot, (spine::ClippingAttachment *)attachment);
                pushMarker(slotIdx);
                continue;
            } else {
                pushMarker(slotIdx);
                continue;
            }
            NativeTexture *tex = region ? static_cast<NativeTexture *>(region->rendererObject) : nullptr;
            const uint32_t cid = (tex && tex->state) ? tex->state->cid : 0;
            if (!cid && !bounds) {
                if (tex && tex->state && !tex->state->failed) { notReady = true; statNotReady_++; }
                clipper.clipEnd(slot);   // 永久失败页:跳过该 slot 的 quads,其余照画
                pushMarker(slotIdx);
                continue;
            }
            // 颜色合成同 SkeletonRenderer::render(skeleton×slot×attachment 三级乘积 + dark)。
            const uint8_t cr = (uint8_t)(skc.r * slot.getColor().r * attachmentColor->r * 255);
            const uint8_t cg = (uint8_t)(skc.g * slot.getColor().g * attachmentColor->g * 255);
            const uint8_t cb = (uint8_t)(skc.b * slot.getColor().b * attachmentColor->b * 255);
            const uint8_t ca = (uint8_t)(skc.a * slot.getColor().a * attachmentColor->a * 255);
            const uint32_t argb = ((uint32_t)ca << 24) | ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | cb;
            uint32_t darkArgb = 0xff000000u;
            if (slot.hasDarkColor()) {
                spine::Color &dc = slot.getDarkColor();
                darkArgb = 0xff000000u | ((uint32_t)(uint8_t)(dc.r * 255) << 16) |
                           ((uint32_t)(uint8_t)(dc.g * 255) << 8) | (uint8_t)(dc.b * 255);
            }
            float tint, alpha, dark, darkAlpha;
            colorParts(argb, darkArgb, tint, alpha, dark, darkAlpha);
            const float blend = (float)blendCode(slot.getData().getBlendMode());
            if (clipper.isClipping()) {
                clipper.clipTriangles(*vertices, *indices, *uvs, 2);
                vertices = &clipper.getClippedVertices();
                verticesCount = (int)(clipper.getClippedVertices().size() >> 1);
                uvs = &clipper.getClippedUVs();
                indices = &clipper.getClippedTriangles();
            }
            // spine-pixi applies SlotMesh's Pixi transform after Spine clipping. Only this
            // attachment changes; slotObject marker transforms and sibling geometry stay intact.
            const auto scale = inst.slotMeshScales.find(slotIdx);
            if (scale != inst.slotMeshScales.end()) {
                for (size_t v = 0; v + 1 < vertices->size(); v += 2) {
                    (*vertices)[v] *= scale->second[0];
                    (*vertices)[v + 1] *= scale->second[1];
                }
            }
            if (bounds) {
                for (size_t v = 0; v + 1 < vertices->size(); v += 2)
                    bounds->include((*vertices)[v], (*vertices)[v + 1]);
                clipper.clipEnd(slot);
                continue;
            }
            const float *px = vertices->buffer();
            const float *uvp = uvs->buffer();
            const unsigned short *ix = indices->buffer();
            const int idxCount = (int)indices->size();
            for (int t = 0; t + 2 < idxCount; t += 3)
                if (!pushTriQuad(quads, cid, px, uvp, ix[t], ix[t + 1], ix[t + 2], verticesCount,
                                 tint, alpha, blend, dark, darkAlpha, tex->state->needsPremultiply)) { full = true; break; }
            clipper.clipEnd(slot);
            pushMarker(slotIdx);
        }
        clipper.clipEnd();
        if (bounds) return true;
        if (notReady) return false;   // 保持上帧 quads(时间照走),与合批路径同语义
        graph->setNativeQuads(inst.nodeId, std::move(quads));
        inst.primed = true;
        statEmitted_++;
        return true;
    }

    // ---- 事件队列(C++ 回调仅入队;JS 每帧批量拉取)----
    void onSpineEvent(const InstListener &li, spine::EventType type,
                      spine::TrackEntry *entry, spine::Event *event) {
        int code;
        switch (type) {
            case spine::EventType_Start: code = EVT_START; break;
            case spine::EventType_Interrupt: code = EVT_INTERRUPT; break;
            case spine::EventType_End: code = EVT_END; break;
            case spine::EventType_Dispose: code = EVT_DISPOSE; break;
            case spine::EventType_Complete: code = EVT_COMPLETE; break;
            default: code = EVT_EVENT; break;
        }
        const int track = entry ? (int)entry->getTrackIndex() : -1;
        uint32_t trackKey = 0;
        auto trackIdentity = entry ? trackKeys_.find(entry) : trackKeys_.end();
        if (trackIdentity != trackKeys_.end() && trackIdentity->second.instId == li.instId)
            trackKey = trackIdentity->second.key;
        int animIdx = -1;
        if (entry && entry->getAnimation() && li.entry) {
            auto it = li.entry->animIndex.find(entry->getAnimation());
            if (it != li.entry->animIndex.end()) animIdx = it->second;   // 空动画等表外动画 → -1
        }
        int eventIdx = -1, stringIdx = -1;
        float intVal = 0, floatVal = 0;
        if (event) {
            if (li.entry) {
                auto it = li.entry->eventIndex.find(&event->getData());
                if (it != li.entry->eventIndex.end()) eventIdx = it->second;
            }
            intVal = (float)event->getIntValue();
            floatVal = event->getFloatValue();
            const spine::String &s = event->getStringValue();
            if (s.length() > 0 && s.buffer()) stringIdx = internString(s.buffer());
        }
        // Custom Event 需要 eventIdx；其他生命周期事件该槽原本恒为 -1，复用为 trackKey
        // 可保持旧 8-float 桥协议不变。旧 JS 在非 Event 回调中本来也不读 eventIdx。
        const int eventIdentity = code == EVT_EVENT ? eventIdx : (int)trackKey;
        if (events_.size() >= (size_t)EVENT_CAP) { events_.pop_front(); droppedEvents_++; }
        events_.push_back({(float)li.instId, (float)track, (float)code, (float)animIdx,
                           (float)eventIdentity, intVal, floatVal, (float)stringIdx});
        // Dispose is the final callback for this exact TrackEntry. Erase only after publishing its
        // key so a replacement using the same animation can still reject the stale event in JS.
        if (type == spine::EventType_Dispose && trackIdentity != trackKeys_.end())
            trackKeys_.erase(trackIdentity);
    }

    // out 每条 8 float:[instId, track, type, animIdx, eventIdx|trackKey, intVal, floatVal,
    // stringIdx]。type=Event 时第 5 槽是 eventIdx，其余类型是 trackKey。trackKey=0 仅可能
    // 是 setAnimation 返回前同步产生的 Start，该事件不修改本地队列。
    // 返回写出的条数;未写完的留队(下帧续拉)。
    int drainEvents(float *out, int capEntries) {
        if (!out || capEntries <= 0) return 0;
        int n = 0;
        while (n < capEntries && !events_.empty()) {
            std::memcpy(out + (size_t)n * EVENT_FLOATS, events_.front().data(),
                        EVENT_FLOATS * sizeof(float));
            events_.pop_front();
            n++;
        }
        return n;
    }
    size_t eventCount() const { return events_.size(); }

    const std::string *stringAt(int idx) const {
        return (idx >= 0 && idx < (int)strings_.size()) ? &strings_[idx] : nullptr;
    }

    // Pixi's public width/height contract is defined in terms of the current pose's local
    // bounds. NativeSpine deliberately has no JS SlotMesh children, so the JS display-tree walk
    // is empty; expose the authoritative spine-cpp geometry bounds for responsive layout.
    bool localBounds(int instId, LocalBounds &out) {
        Instance *inst = find(instId);
        if (!inst) return false;
        if (!inst->slotMeshScales.empty()) return emitInterleaved(*inst, nullptr, &out);
        out = LocalBounds();
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        spine::RenderCommand *cmd = renderer_.render(*inst->skeleton());
        for (spine::RenderCommand *c = cmd; c; c = c->next) {
            const float *positions = c->positions;
            if (!positions || c->numVertices <= 0) continue;
            for (int i = 0; i < c->numVertices; ++i) {
                const float x = positions[i * 2], y = positions[i * 2 + 1];
                if (!std::isfinite(x) || !std::isfinite(y)) continue;
                if (!out.valid) { x0 = x1 = x; y0 = y1 = y; out.valid = true; }
                else {
                    if (x < x0) x0 = x; if (x > x1) x1 = x;
                    if (y < y0) y0 = y; if (y > y1) y1 = y;
                }
            }
        }
        if (out.valid) { out.x = x0; out.y = y0; out.width = x1 - x0; out.height = y1 - y0; }
        return true;
    }

    // Browser spine-pixi materialises every rendered region/mesh as a named SlotMesh child. Game
    // code can therefore call getChildByName(slotName), make that mesh interactive, and receive
    // pointer events over its current geometry. NativeSpine deliberately omits those render
    // meshes, so expose the authoritative current attachment AABB for a lazy JS hit-only facade.
    // `computeWorldVertices` is in skeleton-local coordinates here, exactly the coordinate space
    // of spine-pixi's SlotMesh vertices before the outer Pixi container transform is applied.
    bool slotBounds(int instId, int slotIdx, LocalBounds &out) {
        Instance *inst = find(instId);
        if (!inst) return false;
        out = LocalBounds();
        spine::Vector<spine::Slot *> &slots = inst->skeleton()->getSlots();
        if (slotIdx < 0 || (size_t)slotIdx >= slots.size()) return false;
        spine::Slot &slot = *slots[slotIdx];
        if (!slot.getBone().isActive()) return true;
        spine::Attachment *attachment = slot.getAttachment();
        if (!attachment) return true;
        spine::Vector<float> vertices;
        if (attachment->getRTTI().isExactly(spine::RegionAttachment::rtti)) {
            vertices.setSize(8, 0);
            ((spine::RegionAttachment *)attachment)->computeWorldVertices(slot, vertices, 0, 2);
        } else if (attachment->getRTTI().isExactly(spine::MeshAttachment::rtti)) {
            spine::MeshAttachment *mesh = (spine::MeshAttachment *)attachment;
            vertices.setSize(mesh->getWorldVerticesLength(), 0);
            mesh->computeWorldVertices(slot, 0, mesh->getWorldVerticesLength(),
                                       vertices.buffer(), 0, 2);
        } else {
            return true;
        }
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        for (size_t i = 0; i + 1 < vertices.size(); i += 2) {
            const float x = vertices[i], y = vertices[i + 1];
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            if (!out.valid) { x0 = x1 = x; y0 = y1 = y; out.valid = true; }
            else {
                if (x < x0) x0 = x; if (x > x1) x1 = x;
                if (y < y0) y0 = y; if (y > y1) y1 = y;
            }
        }
        if (out.valid) { out.x = x0; out.y = y0; out.width = x1 - x0; out.height = y1 - y0; }
        return true;
    }

    // ---- 对拍/调试探针(P1b 视觉验证用;不推进时间)----
    struct QuadProbe {
        int quadCount = 0;      // 当前姿态可产出的 quad(三角)数
        int texNotReady = 0;    // cid 未就绪的命令数
        bool primed = false;    // 是否已成功发射过
        int sampleN = 0;        // 前 4 个 quad 的三点 xy 摘要
        float sample[24] = {0};
    };
    bool probeQuads(int instId, QuadProbe &out) {
        Instance *inst = find(instId);
        if (!inst) return false;
        out = QuadProbe();
        out.primed = inst->primed;
        spine::RenderCommand *cmd = renderer_.render(*inst->skeleton());
        for (spine::RenderCommand *c = cmd; c; c = c->next) {
            if (c->numIndices <= 0) continue;
            NativeTexture *tex = static_cast<NativeTexture *>(c->texture);
            if (tex && tex->state && tex->state->cid == 0) out.texNotReady++;
            for (int t = 0; t + 2 < c->numIndices; t += 3) {
                if (out.sampleN < 4) {
                    const int i0 = c->indices[t], i1 = c->indices[t + 1], i2 = c->indices[t + 2];
                    if (i0 < c->numVertices && i1 < c->numVertices && i2 < c->numVertices) {
                        float *s = out.sample + out.sampleN * 6;
                        s[0] = c->positions[i0 * 2]; s[1] = c->positions[i0 * 2 + 1];
                        s[2] = c->positions[i1 * 2]; s[3] = c->positions[i1 * 2 + 1];
                        s[4] = c->positions[i2 * 2]; s[5] = c->positions[i2 * 2 + 1];
                        out.sampleN++;
                    }
                }
                out.quadCount++;
            }
        }
        return true;
    }

    // 诊断计数(__spxStats)
    unsigned long statFull_ = 0, statFrozen_ = 0, statNotReady_ = 0;
    unsigned long statEmitted_ = 0, statTruncated_ = 0;
    unsigned long statManualIdle_ = 0;   // P3:manual 实例"未喂"帧数(业务闸门关闭中)
    unsigned long statAcquire_ = 0, statSurrender_ = 0;   // E-1 租约翻转计数(状态翻转计1,非每帧)
    unsigned long statClockOnly_ = 0;    // 屏外/隐藏但有业务喂入时:只推 state,不蒙皮/发射
    size_t droppedEvents_ = 0;

private:
    struct TrackIdentity {
        int instId = 0;
        uint32_t key = 0;
    };

    static std::string dynamicAttachmentKey(int slotIdx, const char *name) {
        return std::to_string(slotIdx) + "\n" + (name ? name : "");
    }
    void releaseDynamicAttachments(Instance &inst) {
        if (dynamicRelease_) for (auto &kv : inst.dynamicAttachments) {
            Instance::DynamicAttachment *dynamic = kv.second.get();
            if (dynamic && dynamic->retainedCid) {
                dynamicRelease_(dynamic->retainedCid);
                dynamic->retainedCid = 0;
            }
        }
        inst.dynamicAttachments.clear();
    }
    // 挂载子节点归还容器语义:TYPE→T_CONTAINER + VISIBLE/ALPHA 恢复 base(JS 中途写过的值先采纳)。
    // 不恢复会让 walk 顶部永久跳过该节点(业务把子节点移走重用后不可见)。
    int restoreLog_ = 0;
    void restoreSlotNode(Instance::SlotBind &b, sg::SceneGraph *graph) {
        const int node = b.nodeId;
        if (!graph || !graph->buf || node <= 0 || node >= sg::NODE_CAP || !graph->nodes[node].alive) return;
        float *s = graph->buf + (size_t)node * sg::STRIDE;
        // TYPE 已被 JS 覆写回普通类型时只尊重现值不改写;但归还动作不能跳过——早退版本是
        // E-1 可见族凝固的实锤根因:LA 骨骼残留矩阵成为无主「第二真值」(无位/无债/无队列,
        // 祖先早停永不触达,LTYPE 兜底触达不到),verify 每帧 mismatch;EFX 残留则是复显失踪。
#if PIXINATIVE_DIAG_LOG
        if (restoreLog_ < 24) { restoreLog_++;   // E-1 排障:归还现场(哪个节点/何种类型/EFX 状态)
            fprintf(stderr, "[spx-restore] node=%d ty=%d efxV=%.0f pd=%d\n", node, (int)s[sg::TYPE],
                    graph->buf[sg::EFX_VIS_BASE+node], (int)graph->buf[sg::PDIRTY_BASE+node]); }
#endif
        if ((int)s[sg::TYPE] == sg::T_SPINE_SLOT)
            s[sg::TYPE] = (float)sg::T_CONTAINER;
        // Phase E:base 槽从未被合成覆写,归还无需「恢复」;只复位 EFX 因子(合成随绑定终止)。
        graph->buf[sg::EFX_ALPHA_BASE+node] = 1.0f;
        graph->buf[sg::EFX_VIS_BASE+node]   = 1.0f;
        // B0-2 契约6:TYPE 从 T_SPINE_SLOT 归还为普通节点=local matrix 来源改变。置持久位 bit2,
        // 强制下一次 canonical compute 从 X/Y/ROT/SCALE 分量重建 LA..LTY,不复用 Spine 残留矩阵
        // (B0-1 实锤病例:归还的 mark 若在标脏代/重算代错位中蒸发,LA 残留即永久凝固)。
        graph->buf[sg::PDIRTY_BASE + node] = (float)(((int)graph->buf[sg::PDIRTY_BASE + node]) | 2);
        graph->markLocalInputChanged(node);
    }

    // 单三角 → 退化 quad(spineskin writeTriQuad 同款布局:第 4 点重复第 3 点)。
    // 返回 false=到达实例上限(statTruncated_);越界索引静默跳过(与合批路径一致)。
    bool pushTriQuad(std::vector<sg::NativeQuad> &quads, uint32_t cid,
                     const float *px, const float *uv, int i0, int i1, int i2, int nv,
                     float tint, float alpha, float blend, float dark, float darkAlpha, bool needsPremultiply) {
        if (i0 >= nv || i1 >= nv || i2 >= nv) return true;
        if (quads.size() >= (size_t)MAX_QUADS_PER_INST) { statTruncated_++; return false; }
        quads.emplace_back();
        float *q = quads.back().data();
        std::memset(q, 0, sizeof(float) * sg::QUAD_STRIDE_F);
        q[0] = (float)cid;
        q[1] = px[i0 * 2]; q[2] = px[i0 * 2 + 1];
        q[3] = px[i1 * 2]; q[4] = px[i1 * 2 + 1];
        q[5] = px[i2 * 2]; q[6] = px[i2 * 2 + 1];
        q[7] = q[5]; q[8] = q[6];
        q[9] = uv[i0 * 2]; q[10] = uv[i0 * 2 + 1];
        q[11] = uv[i1 * 2]; q[12] = uv[i1 * 2 + 1];
        q[13] = uv[i2 * 2]; q[14] = uv[i2 * 2 + 1];
        q[15] = q[13]; q[16] = q[14];
        q[17] = tint;      // RGB 打包 float(r<<16|g<<8|b)
        q[18] = alpha;
        q[19] = blend;     // spine BlendMode 枚举与 Pixi 混合码同构(0..3 直传)
        q[20] = dark;      // -1=非双色;≥0=twoColor dark RGB(注意别落进 <-1.5 的 distanceField 区)
        q[21] = darkAlpha;
        q[22] = needsPremultiply ? 1.0f : 0.0f; // 原生页已预乘；借用直通 alpha 纹理需 shader 预乘
        q[23] = 0.0f;      // shader kind(tiling 等)必须清零
        return true;
    }

    // ARGB(+dark ARGB) → quad 颜色四件套。dark:纯黑(≡无 dark)归并 -1,省 twoColor 状态切换。
    static void colorParts(uint32_t argb, uint32_t darkArgb,
                           float &tint, float &alpha, float &dark, float &darkAlpha) {
        tint = (float)(argb & 0xffffffu);
        alpha = (float)((argb >> 24) & 255u) / 255.0f;
        const bool hasDark = (darkArgb & 0xffffffu) != 0;
        dark = hasDark ? (float)(darkArgb & 0xffffffu) : -1.0f;
        darkAlpha = hasDark ? (float)((darkArgb >> 24) & 255u) / 255.0f : 1.0f;
    }

    static int blendCode(spine::BlendMode m) {
        // spine BlendMode(Normal=0/Additive=1/Multiply=2/Screen=3)与我们(=Pixi)混合码同构;
        // 现网 JS spine 路径即 slot.data.blendMode 原值直传(native_scenegraph.js),保持一致。
        // NPM 变体(17/18/19)不用:纹理经 native 解码恒预乘。
        switch (m) {
            case spine::BlendMode_Additive: return 1;
            case spine::BlendMode_Multiply: return 2;
            case spine::BlendMode_Screen: return 3;
            default: return 0;
        }
    }

    // 照抄 spineframecache.h:父链任一不可见/已释放 → false。
    static bool chainVisible(sg::SceneGraph *graph, const float *buf, int node) {
        if (!graph || !buf) return true;
        int guard = 0;
        for (int i = node; i > 0 && i < sg::NODE_CAP && guard++ < sg::NODE_CAP; i = graph->nodes[i].parent) {
            if (!graph->nodes[i].alive) return false;
            if (buf[(size_t)i * sg::STRIDE + sg::VISIBLE] == 0.0f) return false;
        }
        return true;
    }
    // 渲染子树 AABB 完全出视口(pad 64)→ true。空/未算 bounds 保守驱动(参照 batcher outsideViewport
    // 的读法与 JS 闸门的 64px pad;bounds 为上一帧 compute 产物,滞后 1 帧由 pad 吸收)。
    static bool offscreen(const float *buf, int node, float vw, float vh) {
        const float *d = buf + sg::BOUNDS_BASE + (size_t)node * sg::BOUNDS_STRIDE_F + 4;
        const float x0 = d[0], y0 = d[1], x1 = d[2], y1 = d[3];
        if (x0 > x1 || y0 > y1) return false;
        const float pad = 64.0f;
        return x1 < -pad || x0 > vw + pad || y1 < -pad || y0 > vh + pad;
    }

    int internString(const char *s) {
        if (!s || !s[0]) return -1;
        auto it = stringIds_.find(s);
        if (it != stringIds_.end()) return it->second;
        const int idx = (int)strings_.size();
        strings_.push_back(s);
        stringIds_[strings_.back()] = idx;
        return idx;
    }

    void purgeEvents(int instId) {
        for (auto it = events_.begin(); it != events_.end();)
            it = ((int)(*it)[0] == instId) ? events_.erase(it) : it + 1;
    }

    void purgeTrackKeys(int instId) {
        for (auto it = trackKeys_.begin(); it != trackKeys_.end();)
            it = (it->second.instId == instId) ? trackKeys_.erase(it) : std::next(it);
    }

    std::unordered_map<int, std::unique_ptr<Instance>> instances_;
    std::unordered_map<const spine::TrackEntry *, TrackIdentity> trackKeys_;
    TexRetainFn dynamicRetain_;
    TexReleaseFn dynamicRelease_;
    std::deque<std::array<float, EVENT_FLOATS>> events_;
    std::vector<std::string> strings_;                 // 事件字符串参数 intern 表(__spxString(idx) 取回)
    std::unordered_map<std::string, int> stringIds_;
    spine::SkeletonRenderer renderer_;   // 单例:render() 内部缓冲在下一次 render 前已消费完,全程主线程
    spine::SkeletonClipping clipper_;    // emitInterleaved 专用(与 renderer_ 内置的互不相扰)
    spine::Vector<float> worldVertices_; // emitInterleaved 顶点暂存(即用即取,主线程)
    spine::Vector<unsigned short> quadIndices_;   // region 固定索引 {0,1,2,2,3,0}
    double lastMs_ = 0;
    int nextInstId_ = 1;
    uint32_t nextTrackKey_ = 1;
};

inline void InstListener::callback(spine::AnimationState *, spine::EventType type,
                                   spine::TrackEntry *trackEntry, spine::Event *event) {
    if (rt && !muted) rt->onSpineEvent(*this, type, trackEntry, event);
}

}  // namespace spinex
