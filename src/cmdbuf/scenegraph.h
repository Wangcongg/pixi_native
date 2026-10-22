// 原生场景图(阶段1:把 pixi 的变换遍历 updateTransform 从 JS 解释器搬到 C++)。
// 思路(同 Cocos/Laya native):每个 DisplayObject 一个 slot,本地变换分量(x/y/scale/rot/pivot/skew/alpha/visible)
// 存在 JS↔C++ 共享 Float32 buffer 里(JS setter O(1) 写、不跨边界);树结构(parent/children)由 C++ 维护
// (addChild/removeChild 低频才跨边界)。每帧 C++ 从根 DFS 遍历,算 localMatrix + worldMatrix + worldAlpha 写回 buffer,
// JS 渲染时直接读 slot 里的 worldTransform。JS 的 updateTransform 变成一次 native 调用。
#pragma once
#include "../common/native_diag_log.h"
#include <array>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sg {

// 每节点 slot 布局(Float32,STRIDE 个)。0..10 = JS 写入的输入;12..18 = C++ 算出的输出。
// 26 起 = 渲染数据(conch 化 P1:native 合批器直接消费,JS 只写不读)。
enum {
    X=0, Y=1, SX=2, SY=3, ROT=4, PVX=5, PVY=6, SKX=7, SKY=8, ALPHA=9, VISIBLE=10, TYPE=11,
    WA=12, WB=13, WC=14, WD=15, WTX=16, WTY=17, WALPHA=18, MASKI=19,   // MASKI=矩形 mask 节点 idx(0=无;rect 存在 mask 节点的 RX..RH)
    LA=20, LB=21, LC=22, LD=23, LTX=24, LTY=25,   // localMatrix 回写(pixi cacheAsBitmap/filter/tiling 会读 localTransform)
    TEXID=26, BLEND=27, RENDERABLE=28,            // 纹理(decoder id,0=未上传)、混合模式、renderable
    RX=29, RY=30, RW=31, RH=32,                   // 局部四角 rect(含 anchor/trim 修正,JS 在纹理/anchor 变化时写)
    U0=33, V0=34, U1=35, V1=36, U2=37, V2=38, U3=39, V3=40,   // 完整 8 UV(支持 atlas 旋转)
    TINT=41,                                       // tint RGB packed(float 承载 24bit 整数,1.0=白)
    QUADOFF=42, QUADCNT=43,                        // M4 多-quad 节点:quad 数据在 quad buffer 的起始 quad 号 + 个数
    PREMUL=44,                                     // M6:1=纹理非预乘(shader 内预乘,修 ASTC 白框);0=已预乘
    EVENTMODE=45, INTERACTIVE_CHILDREN=46, HITFLAGS=47, // native hit-test/mask 元数据;flags:1=hitArea,2=containsPoint,4=isMask,8=纹理 alpha mask,16=椭圆 mask,32=native emitted geometry,64=stencil geometry mask,128=FGUI overflow,256=native geometry only when self-interactive,512=native geometry compressed from descendants
    STRIDE=48
};
static const int HIT_NATIVE_GEOMETRY_SELF_ONLY = 256;
static const int HIT_NATIVE_DESCENDANT_GEOMETRY = 512;
// Drawing a cached subtree suppresses only descendant paint, never hit testing or transforms.
static const int PAINT_CACHED_SUBTREE = 1024;
// TYPE 值:0=容器(不画,只递归) 1=sprite(native 合批) 2=未覆盖节点(native-only 跳过并计数)
//         3=多-quad(9-slice/BitmapText/Graphics/Spine);4=只允许由父 Spine drawOrder 标记触发的 slot 容器;5=C++ 持有的多-quad。
// 主屏不存在 Pixi 离屏快照类型：RenderTexture 也由 Native 直接填充。
enum { T_CONTAINER=0, T_SPRITE=1, T_ESCAPE=2, T_MULTIQUAD=3, T_SPINE_SLOT=4, T_NATIVE_MULTIQUAD=5 };
static const int NODE_CAP = 1u << 15;   // 32768 节点上限(足够;老虎机大厅 ~1000)
// 脏队列(共享 buffer 追加区,节点区之后):[0]=count,[1..DIRTY_CAP]=脏节点 idx(float 承载,≤32768 精确)。
// JS 侧 local/alpha setter 写值时顺手 push;count>DIRTY_CAP 视为溢出=全脏帧(JS __SG_DIRTY=0 亦写溢出值=回退全量)。
static const int DIRTY_BASE = NODE_CAP * STRIDE;
static const int DIRTY_CAP  = 4096;
// 已算清单(compute 输出,回写脏化用):[0]=count,后接本帧真正重算 world 的 idx;溢出=count>cap → JS 回写退全量。
// 回写曾是全量遍历数千对象×6 比较/帧(sg 2.66ms 的大头),清单化后只碰真变化的 7-200 个。
static const int COMPUTED_BASE = DIRTY_BASE + 1 + DIRTY_CAP;
static const int COMPUTED_CAP  = 4096;
// M4 多-quad buffer:多-quad 节点(9-slice/BitmapText/Graphics/Spine)把各自的 quad 列表写这里,
// native 按 QUADOFF/QUADCNT 读。JS 侧使用持久块分配器,静态节点只在内容变化时重写；[0] 保存高水位用于诊断。
static const int QUAD_STRIDE_F = 32;                                  // 0..22 common quad; 23 shader kind; 24..31 shader payload
enum QuadShader { Q_SHADER_DEFAULT = 0, Q_SHADER_TILING = 1, Q_SHADER_ATLAS_CLAMP = 2 };
// TILING payload: four atlas UV corners. ATLAS_CLAMP: [24..27] = minU,minV,maxU,maxV
// at texel centres; clamp the interpolated UV, never contract the image's UV mapping.
static const int QUAD_CAP      = 65536;                               // 持久几何池上限(~6 MiB);按节点稳定分块,避免每帧全量重建
static const int QUAD_BASE     = COMPUTED_BASE + 1 + COMPUTED_CAP;
// 每节点可继承 filter 数据:alpha、blurX、blurY、colorMatrix 标志、4x5 color matrix。与 quad 池分离,避免扩大热节点 STRIDE。
// alpha, blurX/Y, color-matrix flag + matrix20, followed by native custom-filter payload.
// [24]=ColorOverlay enabled, [25..27]=RGB, [28]=strength.
// [29]=DropShadow enabled, [30..31]=offset, [32]=blur, [33..35]=RGB,
// [36]=alpha, [37]=shadowOnly, [38..39]=Noise amount/seed.
// [40]=Twist enabled, [41]=radius, [42]=angle, [43..44]=stage-space offset,
// [45]=padding. [46]=FXAA enabled. DisplacementFilter uses [47]=enabled, [48]=map texture id,
// [49..50]=pixel scale, [51..56]=stage-position -> map-UV affine matrix and [57..60]=rotation.
// KawaseBlurFilter uses [61]=enabled, [62]=kernel count, [63]=clamp, [64..65]=pixel size and
// [66..77]=up to twelve exact kernels. BlurFilter uses [78]=enabled, [79..80]=strength,
// [81]=quality, [82]=kernel size and [83]=repeat-edge. DropShadow's exact Kawase stage uses
// [84]=kernel count, [85..86]=pixel size, [87..94]=kernels and [95]=clamp.
// Ordered post stacks use [96]=count and [97..102]=Pixi-order kinds (up to six passes);
// [103] is the descriptor version. Version 2 stores one complete 20-float descriptor per pass at
// [104..223], so duplicate filters of the same family retain independent uniforms instead of
// collapsing into the last family payload above.
static const int POST_FILTER_DESC_BASE = 104;
static const int POST_FILTER_DESC_STRIDE_F = 20;
static const int POST_FILTER_DESC_CAP = 6;
static const int FILTER_STRIDE_F = POST_FILTER_DESC_BASE + POST_FILTER_DESC_STRIDE_F*POST_FILTER_DESC_CAP;
static const int FILTER_BASE     = QUAD_BASE + 1 + QUAD_CAP*QUAD_STRIDE_F;
// 每节点 bounds:自身图元 world AABB、渲染子树 AABB、自身 hit AABB、hit 子树 AABB、局部 hitArea。
// AABB 使用 minX/minY/maxX/maxY；min>max 表示空。JS 的 getBounds 直接读渲染子树，事件查询读 hit 子树。
static const int BOUNDS_STRIDE_F = 20;
static const int BOUNDS_BASE     = FILTER_BASE + NODE_CAP*FILTER_STRIDE_F;
static const int BOUNDS_META_BASE = BOUNDS_BASE + NODE_CAP*BOUNDS_STRIDE_F; // [0]=dirty,[1]=version
// native hit 结果:候选(target,witness)对 + pointermove 所需的全部 interactive 节点。
static const int HIT_RESULT_CAP  = 2048;
static const int HIT_RESULT_BASE = BOUNDS_META_BASE + 2;
static const int HIT_ALL_COUNT   = HIT_RESULT_BASE + 1 + HIT_RESULT_CAP*2;
static const int HIT_ALL_BASE    = HIT_ALL_COUNT + 1;
// 每节点 HAS_FILTER 位(1 float/节点):FILTER 区 29MB 几乎全零,batcher 热路径每节点无条件读
// fd[0..3]/[24]/[29]/[38]/[40]/[46]/[47]/[61]/[78] = 2-5 条远端 cache line 纯浪费。写入方
// (JS __wrFilters / C++ writeNodeGrayed)在 filter 状态非默认时置 1,清空时清 0;alloc 归零。
// batcher 位 0 → 跳过全部 fd 读取(继承链直传父值)。SG_FILTER_FLAG=0 关闭(退回全读)。
static const int FILTER_FLAG_BASE = HIT_ALL_BASE + HIT_RESULT_CAP;
// bounds 增量化事件队列:JS 侧内容改写(quad/纹理 rect/hitArea/hitflags/renderable)把节点 idx
// 追加到这里([0]=count,[1..]=idx),C++ updateBounds 先汇入 per-node 脏位再增量遍历;
// count>cap = 溢出 → 全脏回退。世界矩阵/结构变化由 C++ markBoundsDirty(idx) 直接标记,不走队列。
static const int BOUNDS_QUEUE_CAP  = 4096;
static const int BOUNDS_QUEUE_BASE = FILTER_FLAG_BASE + NODE_CAP;
// ===== B0-1 时序诊断区(migration-plan Phase B0):帧阶段戳记 =====
// FRAME_PHASE: 当前帧阶段编号,由宿主/JS 在阶段边界写入。
//   1=C++ 前段(transition/refill 推进) 2=JS tick(业务/GSAP) 3=native tween/transition hook
//   4=spine 驱动 5=主 compute/flush 循环 6=compute 后 7=present/draw 0=未知
// MARK_PHASE[idx]:  该节点 local 脏最后一次被标记时的帧阶段(JS __markD 与 C++
//                   markLocalInputChanged 都写)——回答"谁在哪个阶段写的"。
// CONSUME_TAG[idx]: 该节点脏最后一次被队列消费时的 phase*10+action
//                   (action: 1=markDirty 纳入本次重算 2=保留回队列 3=丢弃 detached)
//                   ——回答"被哪类 compute(主=phase5/查询=其他)如何处置"。
// 诊断区常驻零成本读写(几次数组存取);分析完成后可整段移除。
static const int FRAME_PHASE_BASE  = BOUNDS_QUEUE_BASE + 1 + BOUNDS_QUEUE_CAP;
static const int MARK_PHASE_BASE   = FRAME_PHASE_BASE + 1;
static const int CONSUME_TAG_BASE  = MARK_PHASE_BASE + NODE_CAP;
// VISIT_GEN[idx]: computeNode 对该节点的最后触达代数。正值=该代完整走过重算/下推分支;
// 负值=该代进入但在「整支干净」早停处 return;长期陈旧=DFS 根本没到过它(祖先层被剪)。
static const int VISIT_GEN_BASE    = CONSUME_TAG_BASE + NODE_CAP;
// PWTY[idx]: 该节点 world 缓存最后一次被 computeNode 写入时,所用父矩阵的 ty。verify 对比
// 「当时父 ty vs 现在父 ty」——不一致=「子重算后父移动、子未被重访」的铁证。
static const int PWTY_BASE         = VISIT_GEN_BASE + NODE_CAP;
// ===== B0-2 持久脏位模型(migration-plan Phase B0-2 实施契约) =====
// 队列/代数(dirtyLocalGen/subtreeGen)降级为单 pass 加速;正确性由以下持久状态承担:
// PDIRTY[idx]  位组合。bit1=local 持久脏(写入口 clean→dirty 置,CanonicalMain 真正算完该节点
//              且计算期间无新写入才清);bit2=local matrix 来源切换(T_SPINE_SLOT→普通节点,
//              强制从分量重建 LA..LTY,消费时清)。
// DEBT[idx]    子树债务计数(含自身):子树内 PDIRTY&1 节点数。clean→dirty 沿父链+1,确认清位
//              沿父链-1,reparent 按子树债整体转移——持久的祖先可达信号(契约2)。
// WSEQ[idx]    写版本:每次写入口 +1。清位前后比对,计算期间有新写入则不清(契约5)。
// EPOCH[0/1]   mutationEpoch(任何写入口 +1)/ackedEpoch(CanonicalMain 完成时追平)。
//              JS 收敛判断不再只看 SGB[DB](契约:B0-2 第 2 条跨边界信号)。
// LTYPE[idx]: 最后一次重建 local matrix 时的 TYPE。触达时 TYPE≠LTYPE = local 来源已切换
// (SLOT⇄普通),强制重建——自持检测,不依赖写 TYPE 的任何一方记得发信号(规则3的兜底)。
static const int LTYPE_BASE        = PWTY_BASE + NODE_CAP;
static const int PDIRTY_BASE       = LTYPE_BASE + NODE_CAP;
static const int DEBT_BASE         = PDIRTY_BASE + NODE_CAP;
static const int WSEQ_BASE         = DEBT_BASE + NODE_CAP;
static const int EPOCH_BASE        = WSEQ_BASE + NODE_CAP;
// 布局握手区(B0-3 必修点4):init() 写入,JS 装载时比对自身公式——任何一侧插区段未同步,
// 启动即报错,杜绝"置位错位一格"级静默漂移。MAGIC 含布局版本号。
// ===== Phase E:alpha/visible base/effective 拆分 =====
// ALPHA/VISIBLE 槽全树统一为业务 base(权威);spine 合成不再覆写它们,改写独立因子通道:
// EFX_ALPHA[idx] = skeletonA×slotA(默认 1) ; EFX_VIS[idx] = spineHidden?0:1(默认 1)。
// 两者在 computeNode 的 WALPHA 派生链应用(wAlpha = pAlpha×base×efxA×efxV)——WALPHA 本就是
// batcher 的子树可见性门,spineHidden→wAlpha=0 与旧「写 VISIBLE=0」剪枝等价。
// 「槽值≠lastAlpha 则猜测采纳为新 base」协议随之废除:effective 是派生结果,不是第二真值。
static const int EFX_ALPHA_BASE    = EPOCH_BASE + 2;
static const int EFX_VIS_BASE      = EFX_ALPHA_BASE + NODE_CAP;
// Phase F:world committed epoch——WA..WTY 快照「由哪个 sealed epoch 发布」(uint32 视图)。
// computeNode 写 world 时记 sealedEpoch_+1(待发布);seal 点(canonical+主根+债务归零)sealedEpoch_++
// →本轮重算节点 epoch==sealedEpoch_(已发布),未触达节点保留旧 epoch=其 world 仍是旧 seal 的合法快照。
static const int WEPOCH_BASE       = EFX_VIS_BASE + NODE_CAP;
// P1-3(复审):live read 结果区——WA..WTY 槽是 canonical committed 发布物,live barrier 不得
// 覆盖(覆盖即「最新缓存+标签」而非双语义)。链式查询结果写此 scratch:wa,wb,wc,wd,wtx,wty,
// walpha,[7]=有效标志(1=本次查询产出)。消费者(toGlobal 族)读 scratch,槽保持 committed。
static const int LIVE_SCRATCH      = WEPOCH_BASE + NODE_CAP;
// NODE_GEN is a uint32 view over the shared buffer. JS snapshots these values when it queues a
// structural command, then sends them back in the V2 row; 0 means the slot is not currently live.
static const int NODE_GEN_BASE     = LIVE_SCRATCH + 8;
// fgui facade 版本化缓存协议(docs/fgui-facade-commit-protocol.md §1):四个 u32 区段,禁止当 Float32 读写(2^24 上限)。
// 与 NODE_GEN[id] 同比防 slot 复用 ABA;JS 侧 native_scenegraph 01 片段的布局链与 R.fgui* 字段须同步。
static const int FGUI_SEQ_BASE      = NODE_GEN_BASE + NODE_CAP;      // facadeSeq[node]:facade 可见字段任一变化 +1(增量过滤器)
static const int FGUI_DIRTY_BASE    = FGUI_SEQ_BASE + NODE_CAP;      // facadeDirtyBits[node]:变化维度位图(nfgui::FacadeDirty),JS ack 按 (gen,seq) 条件清
static const int FGUI_CHILDSEQ_BASE = FGUI_DIRTY_BASE + NODE_CAP;    // childSeq[parent]:子集合/顺序变化 +1
static const int FGUI_COMMIT_BASE   = FGUI_CHILDSEQ_BASE + NODE_CAP; // instanceCommitSeq[rootNode]:一次操作全部收敛后发布(一致性屏障)
static const int LAYOUT_MAGIC      = FGUI_COMMIT_BASE + NODE_CAP;   // [0]=magic+版本 [1]=SHARED_FLOATS [2]=PDIRTY_BASE [3]=WSEQ_BASE
static const int SHARED_FLOATS   = LAYOUT_MAGIC + 4;   // 宿主按此分配共享 buffer
static const float LAYOUT_MAGIC_VALUE = 8130006.0f;   // 布局版本(复审插 NODE_GEN 递增;2026-09-03 插 FGUI_SEQ/DIRTY/CHILDSEQ/COMMIT 递增;须<2^24)
// compute 模式(契约4):显式声明,禁止凭 root 形态猜。旁路期仅记录,切换期起 CanonicalMain 才可清位。
enum ComputeMode { CM_CANONICAL_MAIN=1, CM_QUERY_NOACK=2, CM_TEMPORARY_ROOT=3, CM_RENDER_TEXTURE=4 };
// Stable receipt ABI for generation-checked SceneGraph command streams. Positive values are
// successful terminal states; negative values reject the row without mutating the graph. Zero is
// reserved for rows which were not inspected after an earlier row rejected the batch.
enum class CommandReceipt : int32_t {
    Applied = 1,
    Idempotent = 2,
    ManagedFreeDeferred = 3,
    InvalidOrDead = -1,
    GenerationMismatch = -2,
    Cycle = -3,
    BeforeNotSibling = -4,
};
static constexpr bool commandReceiptSucceeded(CommandReceipt receipt) {
    return static_cast<int32_t>(receipt) > 0;
}
// 八审:JS 桥参数校验(mac/iOS 共用,单测直测)。顺序契约:isfinite→范围→trunc 整数性——
// 任何 (int) 越界转换(如 1e100)都是 UB,必须在【范围校验通过后】才允许转 int;
// trunc 对任意有限 double 无 UB,用它做整数性判定。
static inline bool validComputeArgs(double rootD, double modeD) {
    return std::isfinite(rootD) && std::isfinite(modeD)
        && rootD > 0.0 && rootD < (double)NODE_CAP
        && modeD >= (double)CM_CANONICAL_MAIN && modeD <= (double)CM_RENDER_TEXTURE
        && std::trunc(rootD) == rootD
        && std::trunc(modeD) == modeD;
}

using NativeQuad = std::array<float, QUAD_STRIDE_F>;
struct Node { int parent = -1; std::vector<int> children; std::vector<NativeQuad> nativeQuads; bool alive = false; };

class SceneGraph {
public:
    float* buf = nullptr;                 // SHARED_FLOATS floats(共享给 JS;节点区 + 脏队列区)
    std::vector<Node> nodes;
    std::vector<int> freeList;
    int highWater = 1;                    // 未用过的最小 idx
    // 脏剪枝状态(世代计数免清零):dirtyLocalGen[i]==gen_ = 本帧 local/alpha 变过(需重算局部矩阵);
    // subtreeGen[i]==gen_ = 子树内存在脏节点(DFS 需进入);pending_ = C++ 侧结构变化(setParent/alloc/free)。
    std::vector<uint32_t> dirtyLocalGen, subtreeGen;
    std::vector<uint8_t> localValid_;   // local 矩阵缓存(LA..LTY 槽)是否算过——树外组装的节点 alloc 时标脏会过期作废,
                                        // 首次入树若只靠 parentDirty 会误用全 0 缓存 → world a=0 → sprite 不显示(实测弹窗/轮子消失根因)
    std::vector<int> pending_;
    std::vector<uint8_t> pendingQueued_;
    void queuePending(int idx) {
        if (idx <= 0 || idx >= NODE_CAP || pendingQueued_[idx]) return;
        pendingQueued_[idx] = 1; pending_.push_back(idx);
    }
    uint32_t gen_ = 0;
    unsigned lastComputed = 0, lastSkipped = 0;   // 统计(perf 展示可选)
    // bounds 增量化(对标 Cocos per-Model bounds 更新):
    //   boundsSelfDirty_[i]=1 → 节点自身四套 AABB 需重算(world 变过/内容改写);
    //   boundsChildDirty_[i]=1 → 子树内有脏节点,需下潜并重 unite 各子缓存;两位皆 0 → 整支直接用缓存。
    //   boundsPending_ = 未消费 self 位计数(诊断用;挂在长期离树缓存上的位使其非零,不参与判定)。
    //   全脏回退 = 代际:boundsAllGen_++ 造债(init/队列溢出/全脏 compute/无 idx 的旧签名调用),
    //   每棵树(root)各自欠一次全量,rootFullGen_[root] 记录已还;主舞台/RT/离树 root 互不误销。
    //   boundsMarkEvents_ = 收到过 per-node 信息的次数;BM 被置位却无新信息 → 未知来源,该次保守全量。
    std::vector<uint8_t> boundsSelfDirty_, boundsChildDirty_;
    std::vector<uint32_t> rootFullGen_;
    uint32_t boundsAllGen_ = 1;
    // 节点级顶点缓存代际(batcher 世界顶点段缓存的失效输入,对标 Cocos Batcher2d 的
    // changedFlags|vertDirty 门控)。markBoundsDirty(idx) 已收口全部顶点相关失效源:
    // compute 真重算 world/alpha 的节点、JS 内容改写队列(quad/纹理 rect/tint/renderable)、
    // 结构变化(setParent/alloc)、setNativeQuads/markLocalInputChanged —— 在同一入口 bump。
    // 无 idx 的全脏回退(旧签名调用/队列溢出)走 vertEpoch_ 全局代际,一次作废全部缓存。
    // freeNode 亦 bump,防 slot 复用撞上任缓存。batcher 侧命中条件 = gen 与 epoch 双相等。
    std::vector<uint32_t> vertGen_;
    // 节点代际:alloc 时 bump。跨帧持有 nodeId 的写入方(native tween/宿主级绑定)建立引用时记录
    // generation(),写入前比对——slot 复用后旧引用的写入在结构上不可能命中新主人,而不是依赖
    // 各业务 destroy 链"记得"取消。0 = 无效引用哨兵(generation() 对越界/未分配返回 0)。
    std::vector<uint32_t> nodeGen_;
    uint32_t vertEpoch_ = 1;
    unsigned long boundsMarkEvents_ = 0, boundsMarkSeen_ = 0;
    unsigned boundsPending_ = 0;
    bool boundsIncOn_ = true;
    // SG_BOUNDS_STATS=1 诊断:每 300 次 updateBounds 打一行访问节点数/全量占比(增量收益直读)。
    bool boundsStats_ = false;
    unsigned boundsVisited_ = 0;
    unsigned long bndPassN_ = 0, bndVisitSum_ = 0, bndVisitMax_ = 0, bndForceN_ = 0, bndSkipN_ = 0;
    unsigned long bndForceFullGen_ = 0, bndForceUnknown_ = 0;   // 全量原因细分:代际债 / 未知来源 BM

    void init(float* sharedBuf) {
        buf = sharedBuf; nodes.assign(NODE_CAP, Node{}); freeList.clear(); highWater = 1;
        dirtyLocalGen.assign(NODE_CAP, 0); subtreeGen.assign(NODE_CAP, 0); localValid_.assign(NODE_CAP, 0); pending_.clear(); pendingQueued_.assign(NODE_CAP, 0); gen_ = 0;
        boundsSelfDirty_.assign(NODE_CAP, 0); boundsChildDirty_.assign(NODE_CAP, 0);
        rootFullGen_.assign(NODE_CAP, 0); boundsAllGen_ = 1;   // 每棵树首个 pass 必全量
        vertGen_.assign(NODE_CAP, 1); vertEpoch_ = 1;
        nodeGen_.assign(NODE_CAP, 1);
        // 生命周期(二次复审):reload 时宿主再次 init——「本轮必须为零」的指标与快照必须清空,
        // 否则 authSnap_* 撞上重置后的 nodeGen/WSEQ 会产生旁路假阳性,残留计数污染终局判定。
        sealedEpoch_ = 0; unsealedPublish_ = 0;
        cacheAssertFails_ = 0; cacheAssertLog_ = 0;
        boundsVerifyBad_ = 0; boundsVerifyLog_ = 0;
        bypassCount_ = 0; bypassLog_ = 0; verifyBad_ = 0;
        authSnap_.clear(); authSnapSeq_.clear(); authSnapGen_.clear();   // 下次审计惰性重建=首轮只建不判
        chainScratch_.clear();
        for (int k = 0; k < 7; k++) liveW_[k] = (k==0||k==3||k==6) ? 1.0f : 0.0f;
        if (buf) buf[LIVE_SCRATCH+7] = 0.0f;
        if (buf) std::memset(buf + NODE_GEN_BASE, 0, (size_t)NODE_CAP * 5 * sizeof(uint32_t));   // NODE_GEN + fgui 四个版本区段(连续)一并清零
        boundsMarkEvents_ = 0; boundsMarkSeen_ = 0;
        boundsPending_ = 0;
        { const char* e = getenv("SG_BOUNDS_INC"); boundsIncOn_ = !(e && e[0]=='0'); }
        { const char* e = getenv("SG_BOUNDS_STATS"); boundsStats_ = e && e[0]=='1'; }
        buf[DIRTY_BASE] = 0;
        buf[BOUNDS_META_BASE] = 1;
        buf[BOUNDS_META_BASE + 1] = 0;
        buf[HIT_RESULT_BASE] = 0;
        buf[HIT_ALL_COUNT] = 0;
        buf[BOUNDS_QUEUE_BASE] = 0;
        std::memset(buf + FILTER_FLAG_BASE, 0, (size_t)NODE_CAP * sizeof(float));   // 0.0f 全零位模式
        // slot 0 保留为"根的父"= 单位变换 + alpha 1(JS 根节点的父指向它)
        float* s = buf; for (int i=0;i<STRIDE;i++) s[i]=0;
        s[SX]=1; s[SY]=1; s[ALPHA]=1; s[VISIBLE]=1; s[WA]=1; s[WD]=1; s[WALPHA]=1;
        nodes[0].alive = true;
        for (int i = 0; i < NODE_CAP; i++) { buf[EFX_ALPHA_BASE+i]=1.0f; buf[EFX_VIS_BASE+i]=1.0f; }   // Phase E:全量默认无合成
        buf[LAYOUT_MAGIC]   = LAYOUT_MAGIC_VALUE;
        buf[LAYOUT_MAGIC+1] = (float)SHARED_FLOATS;
        buf[LAYOUT_MAGIC+2] = (float)PDIRTY_BASE;
        buf[LAYOUT_MAGIC+3] = (float)WSEQ_BASE;
    }
    int alloc() {
        int idx;
        if (!freeList.empty()) { idx = freeList.back(); freeList.pop_back(); }
        else { if (highWater >= NODE_CAP) return -1; idx = highWater++; }
        Node& n = nodes[idx]; n.parent=-1; n.children.clear(); n.nativeQuads.clear(); n.alive=true;
        float* s = buf + idx*STRIDE; for (int i=0;i<STRIDE;i++) s[i]=0;
        s[SX]=1; s[SY]=1; s[ALPHA]=1; s[VISIBLE]=1; s[WA]=1; s[WD]=1; s[WALPHA]=1;
        s[RENDERABLE]=1;   // 默认可渲染(TYPE 默认 0=容器;batcher 剪 REND=0)
        s[INTERACTIVE_CHILDREN]=1;
        float* f = buf + FILTER_BASE + idx*FILTER_STRIDE_F; for(int k=0;k<FILTER_STRIDE_F;k++)f[k]=0.0f;
        f[0]=1.0f; f[4]=1.0f; f[10]=1.0f; f[16]=1.0f; f[22]=1.0f;   // matrix indices 4+(0,6,12,18)
        buf[FILTER_FLAG_BASE + idx] = 0.0f;   // 默认无 filter(写入方置位)
        clearBounds(idx);
        clearBoundsDirtyBits(idx);            // 复用 slot:上任脏位过期作废
        localValid_[idx] = 0;      // local 缓存无效(复用 slot 同样归零)
        if (!nodeGen_.empty()) {
            if (++nodeGen_[idx] == 0) ++nodeGen_[idx];   // 0 永久保留为 dead/unallocated 哨兵
            if (buf) u32(NODE_GEN_BASE+idx)=nodeGen_[idx];
        }   // 新的所有权世代:上一任的悬空引用从此比对失败
        buf[PDIRTY_BASE+idx]=0.0f; buf[DEBT_BASE+idx]=0.0f; u32(WSEQ_BASE+idx)=0u; buf[LTYPE_BASE+idx]=0.0f;   // B0-2:复用不继承上代持久状态
        buf[EFX_ALPHA_BASE+idx]=1.0f; buf[EFX_VIS_BASE+idx]=1.0f;   // Phase E:合成因子复位(无 spine 合成=1)
        u32(WEPOCH_BASE+idx)=0u;   // Phase F:新生节点无已发布快照
        queuePending(idx);   // 新节点=脏(首帧必算)
        markBoundsDirty(idx);
        persistMarkDirty(idx);     // B0-2:新节点持久脏(首帧必算的位版;此刻 parent=-1,挂树时债务随 setParent 转移)
        return idx;
    }
    // 当前所有权世代。写入方在建立跨帧引用时保存,写入前 `generation(id)==saved` 比对;
    // 不匹配即该 id 已被 free/复用,引用自失效。对无效 id 返回 0(永不等于任何已保存代际)。
    uint32_t generation(int idx) const {
        if (idx <= 0 || idx >= NODE_CAP || nodeGen_.empty() || !nodes[idx].alive) return 0;
        return nodeGen_[idx];
    }
    CommandReceipt validateFreeNode(int idx, uint32_t expectedGeneration = 0) const {
        if (idx<=0 || idx>=NODE_CAP) return CommandReceipt::InvalidOrDead;
        // Teardown can destroy a native-owned subtree before its queued JS cleanup rows flush.
        // A still-dead slot is completed cleanup; a live reused slot is the ABA mismatch below.
        if (!nodes[idx].alive) return CommandReceipt::Idempotent;
        if (expectedGeneration && generation(idx) != expectedGeneration)
            return CommandReceipt::GenerationMismatch;
        if (nodes[idx].parent >= 0) {
            const int owner = nodes[idx].parent;
            if (owner >= NODE_CAP || !nodes[owner].alive) return CommandReceipt::InvalidOrDead;
            int edges = 0;
            for (int child : nodes[owner].children) if (child == idx) ++edges;
            if (edges != 1) return CommandReceipt::InvalidOrDead;
        }
        return CommandReceipt::Applied;
    }
    CommandReceipt freeNode(int idx, uint32_t expectedGeneration = 0) {
        const CommandReceipt preflight = validateFreeNode(idx, expectedGeneration);
        if (preflight != CommandReceipt::Applied) return preflight;
        if (!vertGen_.empty()) vertGen_[idx]++;   // slot 即将复用:旧顶点缓存立刻失效(alive 翻 false 后 markBoundsDirty 不再 bump)
        // B0-2 契约7:整棵子树离场,其对祖先的全部债务随之注销;自身持久位/版本清零
        // (复用不得继承上一代脏——债务在摘链前结清)。子节点脱挂(parent=-1)保留各自 PDIRTY/DEBT:
        // 它们仍存活可被重挂,重挂时 setParent 债务转移把 subtree 债带给新祖先。
        const CommandReceipt detached = removeFromParent(idx, expectedGeneration);
        if (!commandReceiptSucceeded(detached)) return detached;   // B0-3.1:旧链债务必须先摘净
        if (buf) {
            buf[PDIRTY_BASE+idx]=0.0f; u32(WSEQ_BASE+idx)=0u;
            float childDebt = 0.0f;   // 子节点即将脱挂(parent=-1)并保留各自债;自身区段重算为子合计
            for (int c : nodes[idx].children) if (c>0 && c<NODE_CAP) childDebt += buf[DEBT_BASE+c];
            buf[DEBT_BASE+idx]=childDebt;
        }
        Node& n = nodes[idx];
        for (int c : n.children) if (c>0 && c<NODE_CAP) nodes[c].parent = -1;
        n.children.clear(); n.nativeQuads.clear(); n.alive=false;
        if (buf) { u32(NODE_GEN_BASE+idx)=0u; u32(FGUI_SEQ_BASE+idx)=0u; u32(FGUI_DIRTY_BASE+idx)=0u; u32(FGUI_CHILDSEQ_BASE+idx)=0u; u32(FGUI_COMMIT_BASE+idx)=0u; }   // slot 复用:版本槽归零,旧 seq 靠 gen 失效
        freeList.push_back(idx);
        clearBounds(idx); clearBoundsDirtyBits(idx);
        return CommandReceipt::Applied;
    }
    CommandReceipt removeFromParent(int child, uint32_t expectedGeneration = 0) {
        if (child<=0 || child>=NODE_CAP) return CommandReceipt::InvalidOrDead;
        if (!nodes[child].alive) return CommandReceipt::Idempotent;
        if (expectedGeneration && generation(child) != expectedGeneration)
            return CommandReceipt::GenerationMismatch;
        int p = nodes[child].parent;
        if (p < 0) return CommandReceipt::Idempotent;
        if (p>=NODE_CAP || !nodes[p].alive) return CommandReceipt::InvalidOrDead;
        auto& ch = nodes[p].children;
        size_t childIndex = ch.size();
        for (size_t i=0;i<ch.size();++i) if (ch[i]==child) {
            if (childIndex != ch.size()) return CommandReceipt::InvalidOrDead;
            childIndex = i;
        }
        if (childIndex == ch.size()) return CommandReceipt::InvalidOrDead;
        {
            // B0-3.1:摘链原语自带子树债务摘除——四处裸调用(removeChild 桥/fgui 发布/free/setParent)
            // 曾把带债节点摘走而旧链债不减,10 个无主债凝固在 stage 根(审计器首轮实锤)。
            // 挂/摘对偶收敛为两个原语:setParent 加新链,removeFromParent 减旧链。
            if (buf) {
                float debt = buf[DEBT_BASE+child];
                if (debt > 0.0f) {
                    int guard = 0;   // 与 markBoundsDirty 同款护栏:parent 链有环时不得死转
                    for(int q=p; q>=0&&q<NODE_CAP&&guard++<NODE_CAP; q=nodes[q].parent){buf[DEBT_BASE+q]-=debt; if(q==0)break;}
                }
            }
            ch.erase(ch.begin()+childIndex);
        }
        nodes[child].parent = -1;
        if (p>0 && p<NODE_CAP) markBoundsDirty(p);   // 旧父链的子树 unite 变小,需重并
        else markBoundsDirty(child<NODE_CAP && nodes[child].alive ? child : -1);
        return CommandReceipt::Applied;
    }
    // 把 child 挂到 parent 下;before>=0 则插到 before 之前,否则追加。
    CommandReceipt setParentChecked(int child, int parent, int before,
                                    uint32_t expectedChildGeneration,
                                    uint32_t expectedParentGeneration,
                                    uint32_t expectedBeforeGeneration,
                                    bool detectIdempotent = true) {
        if (child<=0 || child>=NODE_CAP || parent<0 || parent>=NODE_CAP ||
            before < -1 || before >= NODE_CAP) return CommandReceipt::InvalidOrDead;
        if (expectedChildGeneration && generation(child) != expectedChildGeneration)
            return CommandReceipt::GenerationMismatch;
        if (expectedParentGeneration && generation(parent) != expectedParentGeneration)
            return CommandReceipt::GenerationMismatch;
        if (!nodes[child].alive || !nodes[parent].alive) return CommandReceipt::InvalidOrDead;
        if (before >= 0) {
            if (expectedBeforeGeneration && generation(before) != expectedBeforeGeneration)
                return CommandReceipt::GenerationMismatch;
            if (!nodes[before].alive) return CommandReceipt::InvalidOrDead;
            if (before == child) {
                return nodes[child].parent == parent ? CommandReceipt::Idempotent
                                                     : CommandReceipt::BeforeNotSibling;
            }
            if (nodes[before].parent != parent) return CommandReceipt::BeforeNotSibling;
            bool foundBefore = false;
            for (int sibling : nodes[parent].children) if (sibling == before) { foundBefore = true; break; }
            if (!foundBefore) return CommandReceipt::BeforeNotSibling;
        } else if (expectedBeforeGeneration) {
            return CommandReceipt::GenerationMismatch;
        }
        // 防环收口:调用面不止 fgui reparent 桥(那条有 canReparent 预检),还有 Pixi 镜像的
        // 批量结构命令与直通桥等裸调用。id 复用竞态一旦把节点挂进自己的子树,parent 链成环,
        // 此后任何沿链上浮(债务/边界标脏)都会 100% CPU 死转——大厅联赛弹窗 ready 后整机
        // 卡死,sample 全部钉在债务上浮循环。统一在原语入口拒绝,并保留现场供根因追查。
        for (int at=parent, g=0; at>0 && at<NODE_CAP; at=nodes[at].parent) {
            if (at==child || ++g>=NODE_CAP) {
                static int cycleLog = 0;
                if (cycleLog < 8) { ++cycleLog; std::fprintf(stderr,
                    "[sg] setParent 拒绝成环: child=%d parent=%d before=%d (%s)\n",
                    child, parent, before, at==child ? "parent 在 child 子树内" : "parent 链已含环"); }
                return CommandReceipt::Cycle;
            }
            if (!nodes[at].alive) return CommandReceipt::InvalidOrDead;
        }
        const auto& oldSiblings = nodes[parent].children;
        for (size_t i=0;detectIdempotent && i<oldSiblings.size();++i) if (oldSiblings[i]==child) {
            const bool alreadyLast = before < 0 && i + 1 == oldSiblings.size();
            const bool alreadyBefore = before >= 0 && i + 1 < oldSiblings.size() && oldSiblings[i+1] == before;
            if (nodes[child].parent == parent && (alreadyLast || alreadyBefore))
                return CommandReceipt::Idempotent;
            break;
        }
        // B0-2 契约7:reparent 转移整棵脏子树债务。旧链减由 removeFromParent 原语完成,
        // 此处只负责新链加(双原语对偶,防双重减)。
        float debt = buf ? buf[DEBT_BASE+child] : 0.0f;
        const CommandReceipt detached = removeFromParent(child, expectedChildGeneration);
        if (!commandReceiptSucceeded(detached)) return detached;
        nodes[child].parent = parent;
        if (buf && debt > 0.0f) {
            int guard = 0;   // 防御纵深:入口已拒新环,历史环也不得让上浮死转
            for(int p=parent;p>=0&&p<NODE_CAP&&guard++<NODE_CAP;p=nodes[p].parent){buf[DEBT_BASE+p]+=debt;if(p==0)break;}
        }
        // B0-3 必修点2:换父本身就是持久脏(world 必变)。仅入 pending_ 时,抹队列后干净节点
        // 换父将永不重算——结构写入口与属性写入口同享置位契约(队列彻底降级为提示)。
        if (buf) persistMarkDirty(child);
        // Reparenting is a two-phase publication: the hierarchy changes here, while the child's
        // world matrix is rebuilt by compute(root). Until that rebuild, WTX/WTY still describe
        // the previous parent (effect-pool reuse is the common case). Any render/preflight path
        // that samples the tree in this interval must skip the subtree instead of drawing it at
        // the stale location. WALPHA is the batcher's existing subtree visibility gate; clear it
        // transactionally and let computeNode restore the authoritative parentAlpha*localAlpha.
        // This is generic scene-graph state, not a Spine/machine-specific delay or coordinate fix.
        if (buf) buf[(size_t)child * STRIDE + WALPHA] = 0.0f;
        queuePending(child);   // 换父=子树 world 需重算
        markBoundsDirty(child);      // 挂接后标:child self + 新父链
        auto& ch = nodes[parent].children;
        if (before>=0) { for (size_t i=0;i<ch.size();++i) if (ch[i]==before){ ch.insert(ch.begin()+i, child); return CommandReceipt::Applied; } }
        ch.push_back(child);
        return CommandReceipt::Applied;
    }
    // Legacy three-word bridge semantics: a missing/non-sibling `before` appends. Keep existing
    // native FGUI/Pixi callers source- and behaviour-compatible while V2 uses the strict checked
    // entry point above. Legacy same-position writes still execute their historical dirtying path.
    CommandReceipt setParentLegacyReceipt(int child, int parent, int before) {
        int legacyBefore = before;
        if (legacyBefore < 0) legacyBefore = -1;
        else {
            bool sibling = child != legacyBefore && parent >= 0 && parent < NODE_CAP &&
                           legacyBefore < NODE_CAP && nodes[legacyBefore].alive &&
                           nodes[legacyBefore].parent == parent;
            if (sibling) {
                sibling = false;
                for (int id : nodes[parent].children) if (id == legacyBefore) { sibling = true; break; }
            }
            if (!sibling) legacyBefore = -1;
        }
        return setParentChecked(child,parent,legacyBefore,0,0,0,false);
    }
    void setParent(int child, int parent, int before) {
        (void)setParentLegacyReceipt(child,parent,before);
    }
    int childCount(int parent) const {
        return parent>=0 && parent<NODE_CAP && nodes[parent].alive
            ? static_cast<int>(nodes[parent].children.size()) : 0;
    }
    int childAt(int parent, int index) const {
        if(parent<0 || parent>=NODE_CAP || !nodes[parent].alive || index<0) return -1;
        const auto& children=nodes[parent].children;
        return static_cast<size_t>(index)<children.size()?children[static_cast<size_t>(index)]:-1;
    }
    int childIndex(int parent, int child) const {
        if(parent<0 || parent>=NODE_CAP || !nodes[parent].alive) return -1;
        const auto& children=nodes[parent].children;
        for(size_t i=0;i<children.size();++i)if(children[i]==child)return static_cast<int>(i);
        return -1;
    }
    // 从 root 递归算世界变换(root 的父 = slot 0 = 单位)。
    // 脏剪枝:每帧仅重算"local/alpha 变过的节点 + 其子树 + 结构变化子树";干净支整支跳过。
    // 实测每帧变化 7-28 节点 vs 全树数千 → 全量 DFS 的 99% 是浪费(三角函数×2+矩阵乘×节点)。
    int lastComputeMode_ = 0;   // B0-2:最近一次 compute 的显式模式(诊断/对拍)
    int ackLog_ = 0;            // B0-2 排障:清位现场日志限流
    bool ackEnabled_ = false;   // B0-2:本次 compute 是否允许清持久位(仅 CanonicalMain 且非临时局部根)
    bool persistOn_ = true;     // B0-2:位模型消费开关(SG_PERSIST_DIRTY=0 关闭回退旧行为)
    // (四审:单参 compute 重载已删除——隐式 QueryNoAck 是契约缺口的结构性源头,所有调用必须显式声明模式。)
    void compute(int root, int mode) {
        lastComputeMode_ = mode;
        { static int envChecked=0, envOff=0;
          if(!envChecked){ envChecked=1; const char* e=std::getenv("SG_PERSIST_DIRTY"); envOff=(e&&e[0]=='0'); }
          persistOn_ = !envOff; }
        if (root<=0 || root>=NODE_CAP || !nodes[root].alive) return;
        const bool temporaryLocalRoot = nodes[root].parent > 0;
        ackEnabled_ = persistOn_ && (mode == CM_CANONICAL_MAIN) && !temporaryLocalRoot;   // B0-2 契约4/5
        gen_++;
        lastComputed = 0; lastSkipped = 0;
        // 收脏:JS 队列(local/alpha setter push)+ C++ pending(setParent/alloc);溢出(含 JS 回退开关)= 全脏。
        // count 的小数部分 0.5 = 对拍模式(JS __SG_DIRTY=2):剪枝后全量影子重算比对,不一致节点打日志。
        float rawCnt = buf[DIRTY_BASE];
        bool verify = (rawCnt - (float)(int)rawCnt) > 0.25f;
        bool allDirty = false;
        int cnt = (int)rawCnt;
        if (cnt > DIRTY_CAP) allDirty = true;
        else {
            // A Pixi frame can compute multiple independent roots (main stage, RenderTexture,
            // filters, cached subtrees). The dirty queues are shared by all of them. Consuming
            // every entry while computing just one root loses mutations that belong to another
            // root; when that root is visited later its clean ancestors prune the new child and
            // its alloc-time identity world matrix renders at (0,0). Consume only entries covered
            // by this root and retain the rest for the later root computation.
            float consPhase = buf[FRAME_PHASE_BASE]*10.0f;   // B0-1:消费者身份=当时帧阶段
            int retainedDirty = 0;
            for (int i = 0; i < cnt; i++) {
                int idx = (int)buf[DIRTY_BASE + 1 + i];
                if (belongsToRoot(idx, root)) { markDirty(idx); if(idx>0&&idx<NODE_CAP)buf[CONSUME_TAG_BASE+idx]=consPhase+1.0f; }
                // Detached (parent<0) entries are dropped: setParent() pushes the child into
                // pending_ on reattach, so the subtree is recomputed then anyway — nothing is lost.
                // 【曾经改成"保留",实测必须改回】脱树对象现在可以长期存活(removeChild != destroy,
                // 由 destroy/dispose 才释放),而 C++ 每帧仍会驱动它们(spinex updateAll 等)并
                // markLocalInputChanged → 保留就等于队列无上限增长:pending_ 每帧越扫越长、
                else if (idx > 0 && idx < NODE_CAP && nodes[idx].alive && nodes[idx].parent >= 0)
                    { buf[DIRTY_BASE + 1 + retainedDirty++] = (float)idx; buf[CONSUME_TAG_BASE+idx]=consPhase+2.0f; }
                // 丢弃 detached 脏时在节点自身留下"必重算"印记(localValid_=0 → computeNode
                // L552 视为 local dirty)。"setParent 重挂会补"只覆盖重挂那一条路径;若脏在
                // 挂树前的窗口被这里消费丢弃、而后节点以其他方式变为可达,再无人补标——
                // 节点带 alloc 时的 identity world 渲染在 (0,0)。印记使"将来首次被 DFS
                // 访问必重算"成为自持不变量,不再依赖外部承诺。多算一次的代价为零。
                else if (idx > 0 && idx < NODE_CAP && nodes[idx].alive)
                    { localValid_[idx] = 0; buf[CONSUME_TAG_BASE+idx]=consPhase+3.0f; }
            }
            size_t retainedPending = 0;
            for (int idx : pending_) {
                pendingQueued_[idx] = 0;
                if (belongsToRoot(idx, root)) { markDirty(idx); if(idx>0&&idx<NODE_CAP)buf[CONSUME_TAG_BASE+idx]=consPhase+1.0f; }
                else if (idx > 0 && idx < NODE_CAP && nodes[idx].alive && nodes[idx].parent >= 0)
                    { pendingQueued_[idx] = 1; pending_[retainedPending++] = idx; buf[CONSUME_TAG_BASE+idx]=consPhase+2.0f; }
                else if (idx > 0 && idx < NODE_CAP && nodes[idx].alive)
                    { localValid_[idx] = 0; buf[CONSUME_TAG_BASE+idx]=consPhase+3.0f; }   // 同上:丢弃即留印记
            }
            pending_.resize(retainedPending);
            buf[DIRTY_BASE] = (float)retainedDirty + ((verify && temporaryLocalRoot) ? 0.5f : 0.0f);
        }
        if (allDirty) {
            float consPhaseAD = buf[FRAME_PHASE_BASE]*10.0f;
            size_t retainedPending = 0;
            for (int idx : pending_) {
                pendingQueued_[idx] = 0;
                if (!belongsToRoot(idx, root) && idx > 0 && idx < NODE_CAP && nodes[idx].alive && nodes[idx].parent >= 0)
                    { pendingQueued_[idx] = 1; pending_[retainedPending++] = idx; buf[CONSUME_TAG_BASE+idx]=consPhaseAD+2.0f; }
                else if (!belongsToRoot(idx, root) && idx > 0 && idx < NODE_CAP && nodes[idx].alive)
                    { localValid_[idx] = 0; buf[CONSUME_TAG_BASE+idx]=consPhaseAD+3.0f; }   // 全脏帧同样:丢弃 detached 脏即留"必重算"印记
            }
            pending_.resize(retainedPending);
            // 全脏帧无需 boundsAllGen_++:computeNode 对每个真正重算 world 的节点逐个
            // markBoundsDirty(idx),per-node 位精确覆盖全脏 compute;代际债只留给真正
            // 无成员信息的场景(bounds 队列溢出/无 idx 的旧签名调用)。
            // An overflow means exact membership was lost. Attached local-root passes must not
            // clear that signal before the enclosing stage has performed its full recomputation.
            if (!temporaryLocalRoot) buf[DIRTY_BASE] = 0;
        }
        buf[COMPUTED_BASE] = allDirty ? (float)(COMPUTED_CAP + 1) : 0;   // 全脏帧直接给回写发"全量"信号
        float ident[6] = {1,0,0,1,0,0};
        computeNode(root, ident, 1.0f, allDirty);
        // Computing an attached subtree intentionally treats it as an identity-parent local root
        // (RenderTexture/filter/cache paths). Its shared world cache must be restored in the
        // enclosing root's coordinate domain before the main-stage draw.
        if (temporaryLocalRoot) queuePending(root);
        // bounds 脏化已在 computeNode 内逐节点完成(真正重算 world 的节点标 self+父链,
        // 全脏帧亦被逐节点覆盖)。此处无需再置全局位。
        // E-1 采样规则:审计点=final seal(phase5 收敛段)后的 canonical。phase4 前置 canonical
        // 处于「JS tick 半写之后、drive/收敛之前」的 mid-frame 点:渲染标注覆写 TYPE(合法不
        // mark,drive 会压回)等周期性半写在此天然不一致=第 1 类 pre-seal 瞬态,不是病。真正的
        // 裸写残留是持久不一致,任何后续 seal verify 必现形——采样点迁移不丢覆盖。
        if (verify && !allDirty && mode == CM_CANONICAL_MAIN && !temporaryLocalRoot
            && buf[FRAME_PHASE_BASE] >= 4.5f && buf[FRAME_PHASE_BASE] <= 6.5f) { verifyBad_ = 0; verifyNode(root, ident, 1.0f);
            if (mode == CM_CANONICAL_MAIN) { auditDebt(root); auditBypass(root); } }   // B0-3.1+E-2:账本与旁路同频审计
        // B0-3 必修点3:根已收敛的事实=DEBT[root]==0(本根子树债务清完)。ackedEpoch 仅在此
        // 事实成立时追平——多 root/离树债务存在时不产生假确认;epoch 只作版本保护,
        // JS 收敛判断的正确性来源是 DEBT[root](共享槽直读)。
        if (mode == CM_CANONICAL_MAIN && !temporaryLocalRoot && buf[DEBT_BASE+root] <= 0.5f)
            u32(EPOCH_BASE+1) = u32(EPOCH_BASE);
        // P2-4(复审):sealedEpoch_ 不在 compute 内推进——一帧多次干净 canonical(ph4 前置+
        // ph5 两段)会把「canonical 版本」冒充「present seal」。发布动作移至显式 publishSeal。
    }
    // 渲染后/查询前更新 native bounds。增量模式:先汇入 JS 内容改写队列,再只沿脏链下潜
    // (干净支直接用缓存的子树 AABB,不再全树递归)。回退条件:SG_BOUNDS_INC=0 / 队列溢出 /
    // 全脏 compute 帧 / BM 被置位却无任何 per-node 信息(未知来源写入,保守全算=旧行为)。
    void updateBounds(int root) {
        if (!buf) return;
        if (root<=0 || root>=NODE_CAP || !nodes[root].alive) return;
        if (!boundsIncOn_) {   // 旧行为:全局单脏位 + 全树重算
            if (buf[BOUNDS_META_BASE] == 0.0f) return;
            updateBoundsNode(root, true);
            buf[BOUNDS_META_BASE] = 0;
            buf[BOUNDS_META_BASE + 1] += 1.0f;
            return;
        }
        drainBoundsQueue();
        bool metaDirty = buf[BOUNDS_META_BASE] != 0.0f;
        bool needFull = rootFullGen_[root] != boundsAllGen_;            // 该树还欠一次全量(代际债)
        bool rootDirty = boundsSelfDirty_[root] || boundsChildDirty_[root];   // 脏链必上卷到 root,O(1) 判该树有无脏
        if (!metaDirty && !rootDirty && !needFull) { if (boundsStats_) bndSkipN_++; return; }
        // BM 被置位却没有任何新的 per-node 信息 → 未知来源写入,该次保守全量(等价旧行为)。
        bool unknownSource = metaDirty && boundsMarkEvents_ == boundsMarkSeen_;
        bool force = needFull || unknownSource;
        boundsVisited_ = 0;
        updateBoundsNode(root, force);
        if (force) rootFullGen_[root] = boundsAllGen_;                  // 只还清本树的债;其它树各自欠各自还
        boundsMarkSeen_ = boundsMarkEvents_;
        buf[BOUNDS_META_BASE] = 0;
        buf[BOUNDS_META_BASE + 1] += 1.0f;
        if (boundsStats_) {
            bndPassN_++; bndVisitSum_ += boundsVisited_; if (boundsVisited_ > bndVisitMax_) bndVisitMax_ = boundsVisited_;
            if (force) { bndForceN_++; if (needFull) bndForceFullGen_++; else bndForceUnknown_++; }
            if (bndPassN_ % 300 == 0) {
                fprintf(stderr, "[sg-bounds] pass=%lu skip=%lu 全量=%lu(债%lu 未知%lu) 平均访问=%.1f 峰值=%lu pending=%u root=%d\n",
                        bndPassN_, bndSkipN_, bndForceN_, bndForceFullGen_, bndForceUnknown_,
                        (double)bndVisitSum_/(double)bndPassN_, bndVisitMax_, boundsPending_, root);
                bndVisitSum_ = 0; bndPassN_ = 0; bndForceN_ = 0; bndSkipN_ = 0; bndVisitMax_ = 0; bndForceFullGen_ = 0; bndForceUnknown_ = 0;
            }
        }
    }
    // Phase F-5:缓存命中断言(计数形态,生产零成本)。查询若发生在「root 未收敛」态,
    // bounds/hit 会聚合陈旧 world——不修 caller(查询自身会先 barrier),只计数暴露契约违约。
    unsigned long cacheAssertFails_ = 0;   // 硬指标:查询时 root 债务未归零的次数(终局恒 0)
    int cacheAssertLog_ = 0;
    void noteQueryAssert(int root, const char* kind) {
        // 口径:只在 ph6(seal 完成后、present 前)要求收敛。ph5 flush 段中查询发生在两段式
        // 收敛完成之前(合法中间态);ph7/帧前段债务=下帧待消费写,合法 pre-seal——hitTest 用
        // 上帧 seal 树本就是 Pixi 事件语义。
        if (buf && (buf[FRAME_PHASE_BASE] < 5.5f || buf[FRAME_PHASE_BASE] > 6.5f)) return;
        if (persistOn_ && root > 0 && root < NODE_CAP && buf && buf[DEBT_BASE+root] > 0.5f) {
            cacheAssertFails_++;
            if (cacheAssertLog_ < 6) { cacheAssertLog_++;
                fprintf(stderr, "[sg-cache-assert] %s root=%d debt=%.0f ph=%d(查询于未收敛态)\n",
                        kind, root, buf[DEBT_BASE+root], (int)buf[FRAME_PHASE_BASE]); }
        }
    }
    void updateBoundsFrom(int idx, bool liveBarrier = true) {
        if (idx<=0 || idx>=NODE_CAP || !nodes[idx].alive) return;
        int root=idx, guard=0;
        while(nodes[root].parent>0 && guard++<NODE_CAP) root=nodes[root].parent;
        // P1-1(二次复审):bounds 聚合需要全子树 world,单链 scratch 覆盖不了;而 QueryNoAck
        // 写正式 WA..WTY/WEPOCH=非法发布者污染 committed。正解=barrier 升格 CANONICAL_MAIN:
        // 正式槽只由 canonical 发布的契约保持;WEPOCH=sealedEpoch_+1 即 pre-seal 标记,
        // publishSeal 仍由 phase5 显式控制(病例12:mid-frame canonical 不推进 seal);位模型下
        // mid-frame 消费/清位安全(后续写重新置位,phase5 幂等重算)。JS 侧若已先行 canonical,
        // 债务归零,此处不再二次 compute(重复计算消除)。
        // liveBarrier=false(Pixi skipUpdate=true 语义):调用方明确要旧值,不触发收敛。
        if (liveBarrier && persistOn_ && buf && buf[DEBT_BASE+root] > 0.5f) compute(root, CM_CANONICAL_MAIN);
        noteQueryAssert(root, "bounds");
        updateBounds(root);
    }
    // Layout bounds are independent of the queried node's transform and its ancestors.
    // Inverting the world AABB loses this contract when a popup is collapsed (scaleY=0),
    // and also enlarges bounds under rotation. Walk the authoritative native subtree in
    // the query's local space, including leaves that have never acquired a JS facade.
    bool getLocalBounds(int idx, float out[4]) {
        if (!buf || idx<=0 || idx>=NODE_CAP || !nodes[idx].alive) return false;
        updateBoundsFrom(idx);
        const float identity[6]={1,0,0,1,0,0};
        AABB bounds=emptyAABB();
        collectLocalBounds(idx,identity,bounds);
        if(bounds.valid){out[0]=bounds.x0;out[1]=bounds.y0;out[2]=bounds.x1-bounds.x0;out[3]=bounds.y1-bounds.y0;}
        else out[0]=out[1]=out[2]=out[3]=0;
        return true;
    }
    // 返回 broad-phase 候选数；结果写共享区。候选按 Pixi paint order 从上到下排列，JS 只做精确 shape/mask 校验。
    int hitTest(int root, float x, float y, bool collectAll) {
        buf[HIT_RESULT_BASE]=0; buf[HIT_ALL_COUNT]=0;
        if(root<=0 || root>=NODE_CAP || !nodes[root].alive) return 0;
        // P1-A(二次复审):hit 必须新鲜(输入命中),barrier 同 bounds——canonical 合法发布。
        if (persistOn_ && buf && buf[DEBT_BASE+root] > 0.5f) compute(root, CM_CANONICAL_MAIN);
        noteQueryAssert(root, "hit");
        updateBounds(root);
        hitNode(root,x,y,false,-1,collectAll);
        return (int)buf[HIT_RESULT_BASE];
    }
    // idx>0:节点 self 脏 + 整条父链 child 脏(增量遍历只走脏链,干净支直接用缓存);
    // idx<=0(旧无参签名兼容):无来源信息 → 全脏回退,行为等价旧全局单脏位。
    void markBoundsDirty(int idx = -1) {
        if (buf) buf[BOUNDS_META_BASE] = 1.0f;
        if (idx <= 0 || idx >= NODE_CAP || boundsSelfDirty_.empty()) { boundsAllGen_++; vertEpoch_++; return; }
        if (!nodes[idx].alive) return;   // 死节点不在任何树里,对 bounds 无影响
        if (!vertGen_.empty()) vertGen_[idx]++;   // 顶点段缓存失效:world/内容/结构任一变化都经此入口
        boundsMarkEvents_++;             // 收到 per-node 信息(未知来源 BM 判定用,重复标记也计)
        if (!boundsSelfDirty_[idx]) { boundsSelfDirty_[idx] = 1; boundsPending_++; }
        int guard = 0;
        for (int p = nodes[idx].parent; p > 0 && p < NODE_CAP && guard++ < NODE_CAP; p = nodes[p].parent) {
            if (boundsChildDirty_[p]) break;   // 父链已标过 → 上方必然全标过
            boundsChildDirty_[p] = 1;
        }
    }
    void clearBoundsDirtyBits(int idx) {
        if (idx <= 0 || idx >= NODE_CAP || boundsSelfDirty_.empty()) return;
        if (boundsSelfDirty_[idx]) { boundsSelfDirty_[idx] = 0; if (boundsPending_) boundsPending_--; }
        boundsChildDirty_[idx] = 0;
        rootFullGen_[idx] = 0;   // slot 复用:旧全量印章作废(该 idx 作 root 的首个 pass 必全量)
    }
    // C++ 侧改写 FILTER 区后重算 HAS_FILTER 位(JS __wrFilters 自己在写入时算)。
    // 判定集合 = batcher walk/outsideViewport/preflight 实际读取的全部开关位。
    void refreshFilterFlag(int idx) {
        if(!buf||idx<=0||idx>=NODE_CAP)return;
        const float* fd=buf+FILTER_BASE+(size_t)idx*FILTER_STRIDE_F;
        bool on = fd[0]!=1.0f||fd[1]!=0.0f||fd[2]!=0.0f||fd[3]!=0.0f||fd[24]!=0.0f||fd[29]!=0.0f
                ||fd[38]!=0.0f||fd[40]!=0.0f||fd[46]!=0.0f||fd[47]!=0.0f||fd[61]!=0.0f||fd[78]!=0.0f||fd[96]!=0.0f;
        buf[FILTER_FLAG_BASE+idx]=on?1.0f:0.0f;
    }
    void setNativeQuads(int idx, std::vector<NativeQuad>&& q) {
        if(idx<=0||idx>=NODE_CAP||!nodes[idx].alive)return;
        nodes[idx].nativeQuads=std::move(q);float* s=buf+idx*STRIDE;
        s[TYPE]=nodes[idx].nativeQuads.empty()?T_CONTAINER:T_NATIVE_MULTIQUAD;
        s[QUADCNT]=(float)nodes[idx].nativeQuads.size();markBoundsDirty(idx);
    }
    void clearNativeQuads(int idx){if(idx<=0||idx>=NODE_CAP)return;nodes[idx].nativeQuads.clear();if(buf){buf[idx*STRIDE+QUADCNT]=0;}markBoundsDirty(idx);}
    // B0-3 必修点4:WSEQ/EPOCH 用 uint32 位视图(Float32 在 2^24 后 +1 失效,破坏清位保护)。
    // 共享内存同一位置按 uint32 读写;JS 侧对应 Uint32Array(__sg) 同索引。
    uint32_t& u32(int off){ return *reinterpret_cast<uint32_t*>(buf + off); }
    void markLocalInputChanged(int idx){if(idx<=0||idx>=NODE_CAP||!nodes[idx].alive)return;
        // B0-3 ⑤:clean→dirty 才入队(位模型下队列纯提示;位在=债在=canonical 必达,重复条目只是浪费)。
        bool wasClean = !buf || !(((int)buf[PDIRTY_BASE+idx]) & 1);
        if (wasClean || !persistOn_) queuePending(idx);
        markBoundsDirty(idx);
        if(buf){buf[MARK_PHASE_BASE+idx]=buf[FRAME_PHASE_BASE];   // B0-1:记录写入阶段
            persistMarkDirty(idx);}}                              // B0-2:持久位+债务+epoch
    // B0-2 写入口统一置位:clean→dirty 才上卷债务(幂等重复写不重复计债——契约3的位版);
    // WSEQ/mutationEpoch 每写递增(清位比对与跨边界收敛信号)。
    // 债务上卷(唯一实现,沿 C++ 真父链):JS 侧 clean→dirty 也经桥调此函数——加债与清位减债
    // 必须走同一条链,否则(JS 缓存父链 vs C++ 真链不一致时)每次置/清循环净泄漏,死债累积在
    // 链顶把每帧拖成 3-pass(2026-08-13 实锤:DEBT[stage] 恒 115 的无主债)。
    void rollupDebt(int idx){
        if(idx<=0||idx>=NODE_CAP)return;
        int pd=(int)buf[PDIRTY_BASE+idx];
        if(pd&1)return;   // 已置位=债已在,幂等
        buf[PDIRTY_BASE+idx]=(float)(pd|1);
        buf[DEBT_BASE+idx]+=1.0f;   // 债务含自身
        for(int p=nodes[idx].parent;p>=0&&p<NODE_CAP;p=nodes[p].parent){buf[DEBT_BASE+p]+=1.0f;if(p==0)break;}
    }
    void persistMarkDirty(int idx){
        u32(WSEQ_BASE+idx)+=1u;
        u32(EPOCH_BASE)+=1u;
        rollupDebt(idx);
    }
private:
    struct AABB { float x0,y0,x1,y1; bool valid; };
    static AABB emptyAABB(){ return {1.0f,1.0f,-1.0f,-1.0f,false}; }
    static void include(AABB& a,float x,float y){ if(!a.valid){a={x,y,x,y,true};return;} if(x<a.x0)a.x0=x;if(y<a.y0)a.y0=y;if(x>a.x1)a.x1=x;if(y>a.y1)a.y1=y; }
    static void unite(AABB& a,const AABB& b){ if(!b.valid)return;include(a,b.x0,b.y0);include(a,b.x1,b.y1); }
    static bool contains(const float* a,float x,float y){ return a[0]<=a[2]&&a[1]<=a[3]&&x>=a[0]&&x<=a[2]&&y>=a[1]&&y<=a[3]; }
    bool belongsToRoot(int idx, int root) const {
        if (idx <= 0 || idx >= NODE_CAP || !nodes[idx].alive) return false;
        int guard = 0;
        for (int p = idx; p > 0 && p < NODE_CAP && guard++ < NODE_CAP; p = nodes[p].parent)
            if (p == root) return true;
        return false;
    }
    void writeAABB(float* d,const AABB& a){ if(a.valid){d[0]=a.x0;d[1]=a.y0;d[2]=a.x1;d[3]=a.y1;}else{d[0]=d[1]=1.0f;d[2]=d[3]=-1.0f;} }
    AABB readAABB(const float* d){ return {d[0],d[1],d[2],d[3],d[0]<=d[2]&&d[1]<=d[3]}; }
    void clearBounds(int idx){ if(!buf)return;float* d=buf+BOUNDS_BASE+(size_t)idx*BOUNDS_STRIDE_F;for(int k=0;k<4;k++){d[k]=(k<2?1.0f:-1.0f);d[4+k]=(k<2?1.0f:-1.0f);d[8+k]=(k<2?1.0f:-1.0f);d[12+k]=(k<2?1.0f:-1.0f);}for(int k=16;k<20;k++)d[k]=(k<18?1.0f:-1.0f); }
    void addTransformed(AABB& a,const float* s,float x,float y){ include(a,s[WA]*x+s[WC]*y+s[WTX],s[WB]*x+s[WD]*y+s[WTY]); }
    AABB transformedRect(const float* s,float x,float y,float w,float h){ AABB a=emptyAABB();addTransformed(a,s,x,y);addTransformed(a,s,x+w,y);addTransformed(a,s,x+w,y+h);addTransformed(a,s,x,y+h);return a; }
    AABB renderBounds(int idx,const float* m) {
        AABB bounds=emptyAABB();
        const float* s=buf+idx*STRIDE;
        auto add=[&](float x,float y){include(bounds,m[0]*x+m[2]*y+m[4],m[1]*x+m[3]*y+m[5]);};
        const int type=(int)s[TYPE];
        if(type==T_SPRITE || (type==T_ESCAPE && (s[RW]!=0.0f||s[RH]!=0.0f))){
            const float x=s[RX],y=s[RY],w=s[RW],h=s[RH];
            add(x,y);add(x+w,y);add(x+w,y+h);add(x,y+h);
        } else if(type==T_MULTIQUAD){
            const int off=(int)s[QUADOFF],cnt=(int)s[QUADCNT];
            const float* qb=buf+QUAD_BASE+1;
            if(off>=0&&cnt>0&&off+cnt<=QUAD_CAP)for(int q=0;q<cnt;q++){
                const float* v=qb+(size_t)(off+q)*QUAD_STRIDE_F;
                if(v[0]<0.0f)continue;
                for(int p=0;p<4;p++)add(v[1+p*2],v[2+p*2]);
            }
        } else if(type==T_NATIVE_MULTIQUAD){
            for(const auto& q:nodes[idx].nativeQuads){
                if(q[0]<0.0f)continue;
                for(int p=0;p<4;p++)add(q[1+p*2],q[2+p*2]);
            }
        }
        return bounds;
    }
    void collectLocalBounds(int idx,const float* m,AABB& bounds) {
        unite(bounds,renderBounds(idx,m));
        for(int child:nodes[idx].children){
            if(child<=0||child>=NODE_CAP||!nodes[child].alive)continue;
            const float* s=buf+child*STRIDE;
            if(s[VISIBLE]==0.0f||s[RENDERABLE]==0.0f)continue;
            const float next[6]={
                m[0]*s[LA]+m[2]*s[LB],m[1]*s[LA]+m[3]*s[LB],
                m[0]*s[LC]+m[2]*s[LD],m[1]*s[LC]+m[3]*s[LD],
                m[0]*s[LTX]+m[2]*s[LTY]+m[4],m[1]*s[LTX]+m[3]*s[LTY]+m[5]};
            collectLocalBounds(child,next,bounds);
        }
    }
    // 汇入 JS 侧 bounds 事件队列(内容改写方逐节点 push;溢出=全脏回退)。
    void drainBoundsQueue() {
        if (!buf) return;
        float raw = buf[BOUNDS_QUEUE_BASE];
        int n = (int)raw;
        if (n <= 0) { if (raw != 0.0f) buf[BOUNDS_QUEUE_BASE] = 0; return; }
        if (n > BOUNDS_QUEUE_CAP) { boundsAllGen_++; vertEpoch_++; buf[BOUNDS_QUEUE_BASE] = 0; return; }   // 溢出丢了成员信息:顶点缓存同样全体作废
        for (int i = 0; i < n; i++) markBoundsDirty((int)buf[BOUNDS_QUEUE_BASE + 1 + i]);
        buf[BOUNDS_QUEUE_BASE] = 0;
    }
    // force=false 增量:self 脏 → 重算自身四套 AABB;仅 child 脏 → 自身 AABB 用缓存,只重 unite
    // 各子缓存;两者皆净 → 直接返回(整支缓存有效,父读缓存合并)。force=true = 旧全量语义
    // (同时清位,保持 boundsPending_ 计数与位一致)。
    void updateBoundsNode(int idx, bool force) {
        bool selfD = !boundsSelfDirty_.empty() && boundsSelfDirty_[idx] != 0;
        bool childD = !boundsChildDirty_.empty() && boundsChildDirty_[idx] != 0;
        float* s=buf+idx*STRIDE;
        // NativeQuads is the authoritative visual payload. Layout/resize compatibility code may
        // update a legacy Pixi/FGUI Graphics descriptor after its triangles were submitted and
        // transiently write TYPE=T_CONTAINER without clearing that payload. This used to make a
        // valid modal/guide backdrop disappear after a scene resize (the High Roller guide kept
        // two black triangles and alpha=.8, but no longer entered the paint path).
        // A real stencil is identified by HITFLAGS.mask and must stay a non-painting container;
        // every other non-empty NativeQuads node is, by invariant, a drawable multiquad.
        const bool restoreNativeQuads = static_cast<int>(s[TYPE]) == T_CONTAINER &&
            !nodes[idx].nativeQuads.empty() &&
            (static_cast<int>(s[HITFLAGS]) & 4) == 0;
        if (restoreNativeQuads) {
            s[TYPE] = T_NATIVE_MULTIQUAD;
            if (!vertGen_.empty()) ++vertGen_[idx];
        }
        if (!force && !selfD && !childD && !restoreNativeQuads) return;
        boundsVisited_++;
        if (selfD) { boundsSelfDirty_[idx] = 0; if (boundsPending_) boundsPending_--; }
        if (childD) boundsChildDirty_[idx] = 0;
        float* d=buf+BOUNDS_BASE+(size_t)idx*BOUNDS_STRIDE_F;
        const bool doSelf = force || selfD || restoreNativeQuads;
        AABB self, hitSelf;
        if (doSelf) {
            self=renderBounds(idx,s+WA);
            hitSelf=self; int flags=(int)s[HITFLAGS];
            if(flags&1){float* lr=d+16;hitSelf=transformedRect(s,lr[0],lr[1],lr[2]-lr[0],lr[3]-lr[1]);}
            // A compressed renderer (notably NativeSpine) stores descendant triangles on the
            // owner's one native node. Browser EventBoundary would never visit those descendants
            // while interactiveChildren=false, so they must not keep the owner's hit tree alive.
            // An authored hitArea is true self geometry and remains authoritative above.
            else if((flags&HIT_NATIVE_DESCENDANT_GEOMETRY)&&s[INTERACTIVE_CHILDREN]==0.0f)hitSelf=emptyAABB();
            else if(!(flags&(2|32)))hitSelf=emptyAABB();
        } else {
            self=readAABB(d); hitSelf=readAABB(d+8);   // 自身没变,只有子孙脏 → 复用缓存
        }
        AABB renderTree=self, hitTree=hitSelf;
        Node& n=nodes[idx];
        for(size_t i=0;i<n.children.size();i++){
            int c=n.children[i];if(c<=0||c>=NODE_CAP||!nodes[c].alive)continue;
            updateBoundsNode(c, force);
            float* cs=buf+c*STRIDE;
            if(cs[VISIBLE]!=0.0f&&cs[RENDERABLE]!=0.0f){
                const float* cd=buf+BOUNDS_BASE+(size_t)c*BOUNDS_STRIDE_F;
                unite(renderTree,readAABB(cd+4));
                unite(hitTree,readAABB(cd+12));
            }
        }
        writeAABB(d,self);writeAABB(d+4,renderTree);writeAABB(d+8,hitSelf);writeAABB(d+12,hitTree);
    }
    void appendCandidate(int target,int witness){int n=(int)buf[HIT_RESULT_BASE];if(n>=HIT_RESULT_CAP)return;buf[HIT_RESULT_BASE+1+n*2]=(float)target;buf[HIT_RESULT_BASE+2+n*2]=(float)witness;buf[HIT_RESULT_BASE]=(float)(n+1);}
    void appendInteractive(int idx){int n=(int)buf[HIT_ALL_COUNT];if(n>=HIT_RESULT_CAP)return;buf[HIT_ALL_BASE+n]=(float)idx;buf[HIT_ALL_COUNT]=(float)(n+1);}
    void hitNode(int idx,float x,float y,bool inheritedInteractive,int activeTarget,bool collectAll){
        float* s=buf+idx*STRIDE;int mode=(int)s[EVENTMODE],flags=(int)s[HITFLAGS];
        if(s[VISIBLE]==0.0f||s[RENDERABLE]==0.0f||mode==1||(flags&4))return; // none / mask
        bool ownInteractive=mode==3||mode==4;bool effective=inheritedInteractive||ownInteractive;int target=ownInteractive?idx:activeTarget;
        if(collectAll&&ownInteractive)appendInteractive(idx);
        float* d=buf+BOUNDS_BASE+(size_t)idx*BOUNDS_STRIDE_F;
        if(!collectAll&&!contains(d+12,x,y))return;
        // A standalone FGUI root may intentionally expose runtime children beyond its small
        // authored opaque self box (flag 128). Preserve that box for self hits, but do not prune
        // the real descendant tree. Masks below remain authoritative clipping boundaries.
        if((flags&1)&&!contains(d+8,x,y)&&!(flags&128))return;
        // Official FairyGUI hit-tests against the live display-object geometry, not a world AABB.
        // For rotated/skewed rect masks, inverse-map the point into the mask's local space and
        // test the authored rect; alpha/ellipse/geometry masks still use the conservative AABB.
        int mask=(int)s[MASKI];
        if(mask>0&&mask<NODE_CAP&&nodes[mask].alive){
            float* ms=buf+mask*STRIDE;int mflags=(int)ms[HITFLAGS];
            // A native Graphics scroll mask often has no RW/RH rectangle; its real geometry lives
            // in the cached render bounds. Treating the default 0x0 fields as a simple rect prunes
            // the entire list before its interactive children can become hit candidates.
            bool simpleRect=((mflags&(8|16|32|64))==0)&&fabsf(ms[RW])>1e-6f&&fabsf(ms[RH])>1e-6f;
            if(simpleRect){
                float a=ms[WA],b=ms[WB],c=ms[WC],d=ms[WD],det=a*d-b*c;
                if(fabsf(det)>1e-8f){
                    float px=x-ms[WTX],py=y-ms[WTY],lx=(d*px-c*py)/det,ly=(-b*px+a*py)/det;
                    float x0=ms[RX],y0=ms[RY],x1=x0+ms[RW],y1=y0+ms[RH];
                    if(x1<x0)std::swap(x0,x1);if(y1<y0)std::swap(y0,y1);
                    if(lx<x0||lx>x1||ly<y0||ly>y1)return;
                }else{
                    AABB ma=transformedRect(ms,ms[RX],ms[RY],ms[RW],ms[RH]);float tmp[4];writeAABB(tmp,ma);if(!contains(tmp,x,y))return;
                }
            }else{
                const float* md=buf+BOUNDS_BASE+(size_t)mask*BOUNDS_STRIDE_F;
                AABB ma=readAABB(md+4); // render-tree AABB contains native Graphics/quad geometry
                if(!ma.valid)ma=readAABB(md);
                // Some non-rendering scroll masks intentionally have no standalone render AABB;
                // their clipped owner tree is already the conservative broad phase. An empty
                // mask cache must therefore defer instead of rejecting every point.
                if(ma.valid){float tmp[4];writeAABB(tmp,ma);if(!contains(tmp,x,y))return;}
            }
        }
        // Flag 128 only relaxes the component's authored self hitArea for overflow descendants.
        // It must not override an explicit interactiveChildren=false: FairyGUI uses that gate
        // while a ScrollPane is dragging to prevent release from activating the item underneath.
        if(s[INTERACTIVE_CHILDREN]!=0.0f){Node& n=nodes[idx];for(size_t k=n.children.size();k>0;k--){int c=n.children[k-1];if(c>0&&c<NODE_CAP&&nodes[c].alive)hitNode(c,x,y,effective,target,collectAll);}}
        // A native-only renderer can publish exact triangles so the facade itself remains
        // clickable, while its H5 semantic counterpart is a Container without containsPoint.
        // Such geometry must not become an inherited witness for an interactive ancestor: doing
        // so lets a visual effect appended above a button promote the ancestor ahead of the real
        // control. Explicitly interactive facades retain the exact same geometry and behavior.
        bool selfGeometryEligible=(flags&HIT_NATIVE_GEOMETRY_SELF_ONLY)==0||ownInteractive;
        // Geometry marked as DESCENDANT stands in for children removed by native compression.
        // It is eligible only while EventBoundary would recurse into those children. A passive
        // owner may still carry an inherited interactive mode to its auto children, so in that
        // one case the descendant witness is valid even though the owner itself is not a target.
        bool descendantGeometry=(flags&HIT_NATIVE_DESCENDANT_GEOMETRY)!=0;
        bool descendantWitness=descendantGeometry&&s[INTERACTIVE_CHILDREN]!=0.0f;
        bool descendantGeometryEligible=!descendantGeometry||descendantWitness||(flags&1);
        bool eventModeEligible=mode!=2||descendantWitness;
        if(effective&&target>0&&eventModeEligible&&selfGeometryEligible&&descendantGeometryEligible&&contains(d+8,x,y))appendCandidate(target,idx);
    }
    void markDirty(int idx) {
        if (idx <= 0 || idx >= NODE_CAP) return;
        dirtyLocalGen[idx] = gen_;
        for (int p = idx; p > 0; p = nodes[p].parent) {   // 上卷"子树内有脏"到根,DFS 据此决定是否进支
            if (subtreeGen[p] == gen_) break;
            subtreeGen[p] = gen_;
        }
    }
    void computeNode(int idx, const float* pw, float pAlpha, bool parentDirty) {
        float* s = buf + idx*STRIDE;
        // A zero-geometry placeholder can be assembled and dirtied while its component is still
        // outside the active render root. That pending dirty mark is consumed by compute(root),
        // but the detached branch is never visited, so its local/world cache remains uninitialised.
        // If business code later adds a runtime child (Spine/Particle/Text) below that placeholder,
        // only the new child is locally dirty. Walking down through an uninitialised clean parent
        // would then reuse the alloc-time identity world matrix and render the child at (0,0).
        // localValid_ is set exclusively after this node has actually participated in a compute;
        // treat a missing cache as local dirty so the first reachable descendant mutation repairs
        // the entire path generically, regardless of which FGUI component owns the slot.
        int pdBits = persistOn_ ? (int)buf[PDIRTY_BASE+idx] : 0;   // B0-2:持久位(bit1=脏 bit2=矩阵来源切换)
        bool typeDriftTop = persistOn_ && (int)buf[LTYPE_BASE+idx] != (int)s[TYPE];   // B0-2:触达即检测 local 来源切换
        bool localDirty = parentDirty || dirtyLocalGen[idx] == gen_ || !localValid_[idx] || (pdBits & 3) || typeDriftTop;
        // B0-2:进支条件并联持久债务——队列/代数丢失时位模型兜底(「标脏代/重算代错位即蒸发」的结构性根治)
        if (!localDirty && subtreeGen[idx] != gen_ && !(persistOn_ && buf[DEBT_BASE+idx] > 0.5f)) { lastSkipped++;
            buf[VISIT_GEN_BASE+idx] = -(float)(gen_ & 0xFFFFFF); return; }   // B0-1:负值=本代进入但整支干净早停
        // vg 语义(E-1 精化):此处先记「触达未重算」(负);真正写 world 后(下方)覆写为正。
        buf[VISIT_GEN_BASE+idx] = -(float)(gen_ & 0xFFFFFF);
        uint32_t seq0 = u32(WSEQ_BASE+idx);   // B0-2 契约5:算前记写版本(uint32,不回绕失效)
        float wa, wb, wc, wd, wtx, wty, wAlpha;
        if (localDirty) {
            lastComputed++;
            // localMatrix:仅 local 分量变过(或缓存从未算过)才重算;父变但 local 没变时直接复用缓存
            float la, lb, lc, ld, ltx, lty;
            if (dirtyLocalGen[idx] == gen_ || !localValid_[idx] || (pdBits & 3) || typeDriftTop) {
                localValid_[idx] = 1;
                buf[LTYPE_BASE+idx] = s[TYPE];   // B0-2:记录本次重建的 local 语义来源
                if ((int)s[TYPE] == T_SPINE_SLOT) {
                    // Native Spine 按 pixi-spine updateSlotObject 的
                    // position + worldRotationX + positive worldScale 语义预先构造这个矩阵。
                    la=s[LA]; lb=s[LB]; lc=s[LC]; ld=s[LD]; ltx=s[LTX]; lty=s[LTY];
                } else {
                    float rot=s[ROT], skx=s[SKX], sky=s[SKY];
                    float cx=cosf(rot+sky), sx=sinf(rot+sky), cy=-sinf(rot-skx), sy=cosf(rot-skx);
                    float scx=s[SX], scy=s[SY];
                    la=cx*scx; lb=sx*scx; lc=cy*scy; ld=sy*scy;
                    float pvx=s[PVX], pvy=s[PVY];
                    ltx=s[X]-(pvx*la+pvy*lc); lty=s[Y]-(pvx*lb+pvy*ld);
                    // 矩阵校验层兜底：JS 常规写路径已拦非有限值，但共享内存允许旁路直写。
                    // NaN/Inf 一旦缓存进 LA..LTY，父链任何变化都会复用毒值并传播整棵子树
                    // （消失、bounds/hit 失效），且 WALPHA<=0 剔除对 NaN 恒 false 还会流进
                    // 合批器。这里退化为零矩阵（节点及子树不渲染=与 NaN 顶点被光栅器丢弃
                    // 的表现一致但不毒化 bounds），并保持 localValid_=0 等待下一次有效写。
                    if (!(std::isfinite(la)&&std::isfinite(lb)&&std::isfinite(lc)&&std::isfinite(ld)&&std::isfinite(ltx)&&std::isfinite(lty))) {
                        la=lb=lc=ld=0.0f; ltx=lty=0.0f;
                        localValid_[idx] = 0;
                    }
                    s[LA]=la; s[LB]=lb; s[LC]=lc; s[LD]=ld; s[LTX]=ltx; s[LTY]=lty;
                }
            } else { la=s[LA]; lb=s[LB]; lc=s[LC]; ld=s[LD]; ltx=s[LTX]; lty=s[LTY]; }
            float pa=pw[0],pb=pw[1],pc=pw[2],pd=pw[3],ptx=pw[4],pty=pw[5];
            wa=la*pa+lb*pc; wb=la*pb+lb*pd; wc=lc*pa+ld*pc; wd=lc*pb+ld*pd;
            wtx=ltx*pa+lty*pc+ptx; wty=ltx*pb+lty*pd+pty;
            s[WA]=wa; s[WB]=wb; s[WC]=wc; s[WD]=wd; s[WTX]=wtx; s[WTY]=wty;
            buf[VISIT_GEN_BASE+idx] = (float)(gen_ & 0xFFFFFF);   // E-1:正值=本代真正重算 world
            u32(WEPOCH_BASE+idx) = sealedEpoch_ + 1u;   // Phase F:本快照属于下一个 seal
            buf[PWTY_BASE+idx]=pw[5];   // B0-1:记录本次重算所用父 ty
            // B0-2 契约5:仅 canonical、local/world 均已写完、且计算期间无新写入才清位;
            // bit2(来源切换)已在 local 重建中消费,一并清。清位沿父链结算债务。
            if (ackEnabled_ && (pdBits & 3) && localValid_[idx] && u32(WSEQ_BASE+idx) == seq0) {   // NaN 退化(localValid_=0)不清
                if (pdBits & 1) {
                    buf[DEBT_BASE+idx] -= 1.0f;
                    for(int p=nodes[idx].parent;p>=0&&p<NODE_CAP;p=nodes[p].parent){buf[DEBT_BASE+p]-=1.0f;if(p==0)break;}
                }
                buf[PDIRTY_BASE+idx] = 0.0f;
#if PIXINATIVE_DIAG_LOG
                if (ackLog_ < 40 && (pdBits & 2)) { ackLog_++;   // B0-2 排障:来源切换位被清的现场
                    fprintf(stderr, "[pd-ack] idx=%d pd=%d ty=%d wty=%.1f pwty=%.1f mode=%d\n",
                            idx, pdBits, (int)s[TYPE], wty, pw[5], lastComputeMode_); }
#endif
            }
            wAlpha = pAlpha * s[ALPHA] * buf[EFX_ALPHA_BASE+idx] * buf[EFX_VIS_BASE+idx]; s[WALPHA]=wAlpha;   // Phase E:base×合成因子=effective(派生,不回写 base)
            int cc2 = (int)buf[COMPUTED_BASE];   // 已算清单(回写脏化);溢出后停止记录
            if (cc2 < COMPUTED_CAP) { buf[COMPUTED_BASE + 1 + cc2] = (float)idx; buf[COMPUTED_BASE] = (float)(cc2 + 1); }
            else buf[COMPUTED_BASE] = (float)(COMPUTED_CAP + 1);
            // world 真变过 → bounds self 脏 + 父链 child 脏。DFS 先父后子:父已标时链走一步即停,均摊 O(1)。
            markBoundsDirty(idx);
        } else {
            // 自身干净但子孙有脏:world 不变,直接下推现值
            wa=s[WA]; wb=s[WB]; wc=s[WC]; wd=s[WD]; wtx=s[WTX]; wty=s[WTY]; wAlpha=s[WALPHA];
        }
        float mw[6]={wa,wb,wc,wd,wtx,wty};
        Node& n = nodes[idx];
        for (size_t i=0;i<n.children.size();++i) { int c=n.children[i]; if (c>0 && nodes[c].alive) computeNode(c, mw, wAlpha, localDirty); }
    }
    // ===== B0-3.1 债务账本审计(P1):DEBT 是 DFS 可达性契约 =====
    // 断言 DEBT[node] == 子树内(含自身) PDIRTY&1 节点数;正泄漏=永久下潜+atomic 空转,
    // 负账=DFS 提前剪枝重新漏算。递归返回(理论债,子树节点数);不一致打现场(限流)。
    int debtAuditBad_ = 0;
    struct DebtAudit { int theory; int nodes; };
    DebtAudit auditDebtNode(int idx) {
        DebtAudit r{ ((int)buf[PDIRTY_BASE+idx] & 1) ? 1 : 0, 1 };
        for (int c : nodes[idx].children) if (c > 0 && c < NODE_CAP && nodes[c].alive) {
            DebtAudit cr = auditDebtNode(c);
            r.theory += cr.theory; r.nodes += cr.nodes;
        }
        float actual = buf[DEBT_BASE+idx];
        if (debtAuditBad_ < 10 && (actual < -0.5f || (int)(actual + 0.5f) != r.theory || actual > (float)r.nodes + 0.5f)) {
            debtAuditBad_++;
            fprintf(stderr, "[debt-audit] idx=%d actual=%.0f theory=%d subtreeNodes=%d parent=%d pd=%d ty=%d\n",
                    idx, actual, r.theory, r.nodes, nodes[idx].parent,
                    (int)buf[PDIRTY_BASE+idx], (int)buf[(size_t)idx*STRIDE+TYPE]);
        }
        return r;
    }
public:
    // ===== Phase F-6:authority dump——单节点权威状态一行应答(排障入口) =====
    // 回答:域归属(canonical owner 推断)/最后写者(mark phase)/世代/写版本/持久位/债务/
    // world committed epoch(vs 当前 sealed)/bounds 脏位/EFX 合成持权。
    void authorityDump(int idx) {
        if (!buf || idx <= 0 || idx >= NODE_CAP) { fprintf(stderr, "[sg-authority] idx=%d 越界\n", idx); return; }
        float* s = buf + (size_t)idx*STRIDE;
        int ty = (int)s[TYPE], lty = (int)buf[LTYPE_BASE+idx];
        const char* owner = !nodes[idx].alive ? "freed"
                          : ty == T_SPINE_SLOT ? "spine-drive"
                          : (buf[EFX_ALPHA_BASE+idx] != 1.0f || buf[EFX_VIS_BASE+idx] != 1.0f) ? "spine-efx-residual"
                          : "js/fgui";
        uint32_t wep; std::memcpy(&wep, buf+WEPOCH_BASE+idx, 4);
        bool bDirty = !boundsSelfDirty_.empty() && boundsSelfDirty_[idx];
        fprintf(stderr, "[sg-authority] idx=%d owner=%s alive=%d parent=%d gen=%u lease=%s | lastWriter: mk=%d wseq=%u | "
                        "pd=%d debt=%.0f | world: wep=%u sealed=%u %s | bounds: selfDirty=%d | ty=%d lty=%d efxA=%.2f efxV=%.0f\n",
                idx, owner, nodes[idx].alive?1:0, nodes[idx].parent,
                nodeGen_.empty()?0u:nodeGen_[idx],
                ty==T_SPINE_SLOT?"held":"released",
                (int)buf[MARK_PHASE_BASE+idx], (unsigned)u32(WSEQ_BASE+idx),
                (int)buf[PDIRTY_BASE+idx], buf[DEBT_BASE+idx],
                wep, sealedEpoch_, wep==sealedEpoch_?"(committed)":wep==sealedEpoch_+1?"(pre-seal)":"(older-seal:未触达合法快照)",
                bDirty?1:0, ty, lty, buf[EFX_ALPHA_BASE+idx], buf[EFX_VIS_BASE+idx]);
    }

    // ===== Phase F-4:live read barrier——单点坐标查询的父链 O(depth) 惰性重算 =====
    // toGlobal 族查询不再整树 compute(消费全局队列曾引发「帧中丢弃 detached 脏」竞态族):
    // 只沿父链自根向下判脏重算。位模型下「无位=缓存有效」——seal 后全链位清=零读高效;
    // mid-frame 带位节点驱动最小重算。纯查询语义(三审契约第 4 条):不清位/不 ack/不记 vg/
    // 不动 bounds;结果只写 LIVE_SCRATCH(wa..walpha+有效标志),world/WEPOCH 正式槽绝不碰——
    // 唯一例外是 local 缓存 LA..LTY(local 派生,canonical 会按同输入重建同值)。canonical 到来
    // 时位仍在→幂等重算。detached 链顶以 identity 为父(与 temporaryLocalRoot 语义一致)。
    std::vector<int> chainScratch_;     // P1-B(二次复审):复用的动态父链容器——任意深度都走
                                        // scratch,不再整树回退(回退曾绕过 scratch 契约污染 committed)。
    void computeChainWorld(int idx) {
        if (!buf || idx <= 0 || idx >= NODE_CAP || !nodes[idx].alive) return;
        chainScratch_.clear();
        int guard = 0;
        for (int q = idx; q > 0 && guard++ < NODE_CAP; q = nodes[q].parent) { chainScratch_.push_back(q); if (nodes[q].parent < 0) break; }
        std::vector<int>& chain = chainScratch_;
        int n = (int)chain.size();
        float ident[6] = {1,0,0,1,0,0};
        const float* pw = ident; float pAlpha = 1.0f;
        bool parentChanged = false;
        for (int k = n - 1; k >= 0; k--) {
            int i = chain[k];
            float* s = buf + (size_t)i*STRIDE;
            int pdBits = persistOn_ ? (int)buf[PDIRTY_BASE+i] : 0;
            bool typeDrift = persistOn_ && (int)buf[LTYPE_BASE+i] != (int)s[TYPE];
            bool localDirty = parentChanged || dirtyLocalGen[i] == gen_ || !localValid_[i] || (pdBits & 3) || typeDrift;
            float w[7];
            if (localDirty) {
                float la, lb, lc, ld, ltx, lty;
                if (dirtyLocalGen[i] == gen_ || !localValid_[i] || (pdBits & 3) || typeDrift) {
                    if ((int)s[TYPE] == T_SPINE_SLOT) {
                        la=s[LA]; lb=s[LB]; lc=s[LC]; ld=s[LD]; ltx=s[LTX]; lty=s[LTY];
                    } else {
                        float rot=s[ROT], skx=s[SKX], sky=s[SKY];
                        float cx=cosf(rot+sky), sx=sinf(rot+sky), cy=-sinf(rot-skx), sy=cosf(rot-skx);
                        la=cx*s[SX]; lb=sx*s[SX]; lc=cy*s[SY]; ld=sy*s[SY];
                        ltx=s[X]-(s[PVX]*la+s[PVY]*lc); lty=s[Y]-(s[PVX]*lb+s[PVY]*ld);
                        if (!(std::isfinite(la)&&std::isfinite(lb)&&std::isfinite(lc)&&std::isfinite(ld)&&std::isfinite(ltx)&&std::isfinite(lty))) {
                            la=lb=lc=ld=0.0f; ltx=lty=0.0f;   // NaN 退化与 computeNode 同策略;localValid_ 不动(canonical 复检)
                        }
                        // local 缓存可写(LA..LTY 属 local 派生,canonical 会按同输入重建同值);
                        // world/WEPOCH 槽绝不写——committed 发布物只属 canonical(P1-3)。
                        s[LA]=la; s[LB]=lb; s[LC]=lc; s[LD]=ld; s[LTX]=ltx; s[LTY]=lty;
                    }
                } else { la=s[LA]; lb=s[LB]; lc=s[LC]; ld=s[LD]; ltx=s[LTX]; lty=s[LTY]; }
                float pa=pw[0],pb=pw[1],pc=pw[2],pd2=pw[3],ptx=pw[4],pty=pw[5];
                w[0]=la*pa+lb*pc; w[1]=la*pb+lb*pd2; w[2]=lc*pa+ld*pc; w[3]=lc*pb+ld*pd2;
                w[4]=ltx*pa+lty*pc+ptx; w[5]=ltx*pb+lty*pd2+pty;
                w[6]=pAlpha*s[ALPHA]*buf[EFX_ALPHA_BASE+i]*buf[EFX_VIS_BASE+i];
                parentChanged = true;
            } else {
                w[0]=s[WA]; w[1]=s[WB]; w[2]=s[WC]; w[3]=s[WD]; w[4]=s[WTX]; w[5]=s[WTY]; w[6]=s[WALPHA];
            }
            std::memcpy(liveW_, w, sizeof(w));
            pw = liveW_; pAlpha = w[6];
        }
        for (int k2 = 0; k2 < 7; k2++) buf[LIVE_SCRATCH + k2] = liveW_[k2];
        buf[LIVE_SCRATCH + 7] = 1.0f;   // 有效标志
    }
    float liveW_[7] = {1,0,0,1,0,0,1};   // P1-3:链式查询的逐层世界积(实例暂存,单线程)

private:
    // ===== E-2 权威旁路审计(raw authority bypass):写入口漏标的通用结构检测器 =====
    // 契约:权威语义槽(X..SKY 分量/ALPHA/VISIBLE base/TYPE)每次写必经写闸(JS __markD 系/
    // __sgTypeW/C++ markLocalInputChanged),写闸必递增 WSEQ。seal 审计帧比对快照:
    // 槽值变了而 WSEQ 未动=有写者绕开全部写闸(bypass)——逐口接线永远可能漏,值×版本
    // 的事后对账不可能漏。快照仅诊断态(verify 同频)分配,主流程零成本。
    static const int AUTH_SLOTS = 12;   // X,Y,SX,SY,ROT,PVX,PVY,SKX,SKY,ALPHA,VISIBLE,TYPE
    std::vector<float> authSnap_;       // NODE_CAP*AUTH_SLOTS,lazy
    std::vector<uint32_t> authSnapSeq_; // NODE_CAP
    std::vector<uint32_t> authSnapGen_; // NODE_CAP:快照所属节点世代(free/alloc 复用后 wseq 归零重涨,
                                        // 前世快照 wseq 可能与今世恰好相等——换代只重建快照,不判)
    unsigned long bypassCount_ = 0;     // 硬指标:raw authority bypass(终局要求恒 0)
public:
    uint32_t sealedEpoch_ = 0;          // Phase F:已发布 present seal 计数(publishSeal 显式推进;测试/诊断可读)
    // P2-4:present seal 发布——JS 两段式收敛完成后(present 前)显式调用。条件=主根债务归零
    // (真 seal);未收敛调用不推进(计数暴露)。
    unsigned long unsealedPublish_ = 0;
    void publishSeal(int root) {
        if (root <= 0 || root >= NODE_CAP || !buf) return;
        if (persistOn_ && buf[DEBT_BASE+root] > 0.5f) { unsealedPublish_++; return; }
        sealedEpoch_++;
    }
private:

    int bypassLog_ = 0;
    void auditBypassNode(int idx) {
        const float* s = buf + (size_t)idx*STRIDE;
        const float cur[AUTH_SLOTS] = { s[X],s[Y],s[SX],s[SY],s[ROT],s[PVX],s[PVY],s[SKX],s[SKY],
                                        s[ALPHA],s[VISIBLE],s[TYPE] };
        float* snap = authSnap_.data() + (size_t)idx*AUTH_SLOTS;
        uint32_t seq = u32(WSEQ_BASE+idx);
        uint32_t gen = nodeGen_.empty() ? 0u : nodeGen_[idx];
        if (authSnapGen_[idx] == gen && authSnapSeq_[idx] == seq) {
            static const char* NM[AUTH_SLOTS] = {"X","Y","SX","SY","ROT","PVX","PVY","SKX","SKY","ALPHA","VISIBLE","TYPE"};
            for (int k = 0; k < AUTH_SLOTS; k++) {
                // TYPE 的权威语义只在 T_SPINE_SLOT 边界(local 矩阵来源切换);0↔1↔3↔5 是渲染
                // 标注(怎么画),__sgTypeW 闸对非跨界写不 mark 是设计内——与写闸置位条件同构。
                if (k == 11 && ((int)snap[k] == T_SPINE_SLOT) == ((int)cur[k] == T_SPINE_SLOT)) continue;
                if (snap[k] != cur[k]) {   // 精确位比较:合法写闸路径全走 fround,同值假写不触发
                    bypassCount_++;
                    if (bypassLog_ < 10) { bypassLog_++;
                        fprintf(stderr, "[sg-bypass] idx=%d slot=%s %.3f->%.3f wseq=%u ty=%d ph=%d(写者绕开写闸)\n",
                                idx, NM[k], snap[k], cur[k], seq, (int)s[TYPE], (int)buf[FRAME_PHASE_BASE]); }
                }
            }
        }
        for (int k = 0; k < AUTH_SLOTS; k++) snap[k] = cur[k];
        authSnapSeq_[idx] = seq;
        authSnapGen_[idx] = gen;
        Node& n = nodes[idx];
        for (size_t i=0;i<n.children.size();++i) { int c=n.children[i]; if (c>0 && nodes[c].alive) auditBypassNode(c); }
    }
    void auditBypass(int root) {
        if (authSnap_.empty()) { authSnap_.assign((size_t)NODE_CAP*AUTH_SLOTS, 0.0f);
                                 authSnapSeq_.assign(NODE_CAP, 0xFFFFFFFFu);
                                 authSnapGen_.assign(NODE_CAP, 0xFFFFFFFFu); }   // 首轮只建快照不判
        auditBypassNode(root);
    }

    void auditDebt(int root) {
        if (root <= 0 || root >= NODE_CAP || !nodes[root].alive || !buf) return;
        debtAuditBad_ = 0;
        auditDebtNode(root);
        if (debtAuditBad_ > 0)
            fprintf(stderr, "[debt-audit] 本轮不平衡 %d 处(root=%d DEBT[root]=%.0f)\n", debtAuditBad_, root, buf[DEBT_BASE+root]);
    }
    // 对拍:全量重算(不写 buf),与剪枝结果比较;不一致=漏标节点,打 idx/父/local/两版 world(限 8 条/帧)
    int verifyBad_ = 0;
    void verifyNode(int idx, const float* pw, float pAlpha) {
        float* s = buf + idx*STRIDE;
        // T_SPINE_SLOT 的 local 真值在 LA..LTY(C++ 骨骼驱动直写,computeNode 同款分支);
        // 从分量重组会把骨骼位移当"漏标"误报,且每帧 8 条限额被误报占满,真漏标被挤出。
        float la, lb, lc, ld, ltx, lty;
        if ((int)s[TYPE] == T_SPINE_SLOT) {
            la=s[LA]; lb=s[LB]; lc=s[LC]; ld=s[LD]; ltx=s[LTX]; lty=s[LTY];
        } else {
            float rot=s[ROT], skx=s[SKX], sky=s[SKY];
            float cx=cosf(rot+sky), sx=sinf(rot+sky), cy=-sinf(rot-skx), sy=cosf(rot-skx);
            la=cx*s[SX]; lb=sx*s[SX]; lc=cy*s[SY]; ld=sy*s[SY];
            ltx=s[X]-(s[PVX]*la+s[PVY]*lc); lty=s[Y]-(s[PVX]*lb+s[PVY]*ld);
        }
        float wa=la*pw[0]+lb*pw[2], wb=la*pw[1]+lb*pw[3], wc=lc*pw[0]+ld*pw[2], wd=lc*pw[1]+ld*pw[3];
        float wtx=ltx*pw[0]+lty*pw[2]+pw[4], wty=ltx*pw[1]+lty*pw[3]+pw[5];
        float wAlpha = pAlpha * s[ALPHA] * buf[EFX_ALPHA_BASE+idx] * buf[EFX_VIS_BASE+idx];   // Phase E:对拍与派生链同公式
        // 阈值需容忍浮点尾差:剪枝路径复用缓存 world、影子路径从 local 连乘,运算顺序不同天然差 1e-4 级
        // (首版 0.001/0.05px 阈值产生 1615 条假阳性,把真漏标挤出每帧 8 条限额)。真漏标是几百 px/整数 alpha 级。
        if (verifyBad_ < 8 && (fabsf(wtx-s[WTX])>2.0f || fabsf(wty-s[WTY])>2.0f
            || fabsf(wa-s[WA])>0.02f || fabsf(wAlpha-s[WALPHA])>0.05f)) {
            verifyBad_++;
            // B0-1:mk=最后写入阶段;cs=最后消费(phase*10+action,action 1算/2留/3丢);
            // gen=节点代;lv=localValid;inP=parent.children 是否含本节点(0=树不对称:上卷可标、
            // DFS 永达不到=「凝固族」结构实锤);dl=dirtyLocalGen 距当前代差(0=本代已标)。
            // brk=沿 parent 链向根逐层校验「child 在 parent.children 里」,报第一个断点的
            // child id(0=整链对称)。belongsToRoot 沿 parent 链判定、DFS 沿 children 下行——
            // 祖先层断链会让「消费判定通过、下行永远不达」,节点 world 代代剪枝凝固。
            int brk = 0;
            { int c2 = idx, guard = 0;
              while (guard++ < 64) {
                  int p = nodes[c2].parent; if (p < 0 || p >= NODE_CAP) break;
                  bool found = false;
                  for (int cc : nodes[p].children) if (cc == c2) { found = true; break; }
                  if (!found) { brk = c2; break; }
                  c2 = p; if (c2 == 0) break;
              } }
            int inP = (brk == 0) ? 1 : 0;
            fprintf(stderr, "[sg-verify] 漏标 idx=%d parent=%d local=(%.1f,%.1f rot=%.3f sx=%.3f) 剪枝(tx=%.1f ty=%.1f a=%.4f al=%.3f) 正确(tx=%.1f ty=%.1f a=%.4f al=%.3f) mk=%d cs=%d gen=%u lv=%d inP=%d brk=%d dl=%d\n",
                    idx, nodes[idx].parent, s[X], s[Y], s[ROT], s[SX],
                    s[WTX], s[WTY], s[WA], s[WALPHA], wtx, wty, wa, wAlpha,
                    (int)buf[MARK_PHASE_BASE+idx], (int)buf[CONSUME_TAG_BASE+idx],
                    (nodeGen_.empty()?0u:nodeGen_[idx]), (int)localValid_[idx],
                    inP, brk, (int)(gen_ - dirtyLocalGen[idx]));
            { int par = nodes[idx].parent;
              fprintf(stderr, "[sg-verify]   vg=%d(now=%u) pwty=%.1f(now=%.1f) pvg=%d psg=%d sg=%d ty=%d\n",
                      (int)buf[VISIT_GEN_BASE+idx], (unsigned)(gen_ & 0xFFFFFF), buf[PWTY_BASE+idx], pw[5],
                      (par>=0&&par<NODE_CAP)?(int)buf[VISIT_GEN_BASE+par]:-9,
                      (par>=0&&par<NODE_CAP)?(int)(gen_-subtreeGen[par]):-9,
                      (int)(gen_-subtreeGen[idx]), (int)s[TYPE]);
              fprintf(stderr, "[sg-verify]   pd=%d debt=%.0f pdebt=%.0f ph=%d cm=%d wseq=%u efxA=%.2f efxV=%.0f\n",
                      (int)buf[PDIRTY_BASE+idx], buf[DEBT_BASE+idx],
                      (par>=0&&par<NODE_CAP)?buf[DEBT_BASE+par]:-9.0f,
                      (int)buf[FRAME_PHASE_BASE], lastComputeMode_,
                      (unsigned)*reinterpret_cast<uint32_t*>(buf+WSEQ_BASE+idx),
                      buf[EFX_ALPHA_BASE+idx], buf[EFX_VIS_BASE+idx]);
              fprintf(stderr, "[sg-verify]   wep=%u sealed=%u\n",
                      (unsigned)*reinterpret_cast<uint32_t*>(buf+WEPOCH_BASE+idx), sealedEpoch_); }
        }
        // P2-5(复审):bounds 来源可证——对「声称干净」(selfDirty=0)的简单矩形节点,用重组
        // world 变换 RX..RH 四角与缓存 self AABB 对拍。漏 markBoundsDirty 的陈旧 bounds 在此
        // 现形(cacheAssertFails_ 只保证查询时 root 收敛,不验 bounds 自身来源)。
        if ((int)s[TYPE] == 1 && s[RW] != 0.0f && s[RH] != 0.0f
            && !boundsSelfDirty_.empty() && !boundsSelfDirty_[idx] && s[WALPHA] > 0.0f) {
            const float* bd = buf + BOUNDS_BASE + (size_t)idx*BOUNDS_STRIDE_F;
            if (bd[0] <= bd[2]) {   // 缓存 valid 才比(空 AABB=尚未聚合,不判)
                float x0=s[RX], y0=s[RY], x1=x0+s[RW], y1=y0+s[RH];
                float cx[4]={x0,x1,x0,x1}, cy[4]={y0,y0,y1,y1};
                float mnx=1e30f,mny=1e30f,mxx=-1e30f,mxy=-1e30f;
                for (int k=0;k<4;k++){ float px=cx[k]*wa+cy[k]*wc+wtx, py=cx[k]*wb+cy[k]*wd+wty;
                    if(px<mnx)mnx=px; if(px>mxx)mxx=px; if(py<mny)mny=py; if(py>mxy)mxy=py; }
                if (fabsf(mnx-bd[0])>2.0f || fabsf(mny-bd[1])>2.0f || fabsf(mxx-bd[2])>2.0f || fabsf(mxy-bd[3])>2.0f) {
                    boundsVerifyBad_++;
                    if (boundsVerifyLog_ < 6) { boundsVerifyLog_++;
                        fprintf(stderr, "[sg-bounds-verify] idx=%d 缓存(%.1f,%.1f..%.1f,%.1f) 重组(%.1f,%.1f..%.1f,%.1f) ty=%d(声称干净的 bounds 与 world 重组不符)\n",
                                idx, bd[0], bd[1], bd[2], bd[3], mnx, mny, mxx, mxy, (int)s[TYPE]); }
                }
            }
        }
        float mw[6]={wa,wb,wc,wd,wtx,wty};
        Node& n = nodes[idx];
        for (size_t i=0;i<n.children.size();++i) { int c=n.children[i]; if (c>0 && nodes[c].alive) verifyNode(c, mw, wAlpha); }
    }
    unsigned long boundsVerifyBad_ = 0;   // P2-5 硬指标:bounds 来源违约(终局恒 0)
    int boundsVerifyLog_ = 0;
};

// Platform-neutral executor for the V2 structural-command ABI. The host supplies the free
// callback because Native FGUI-managed nodes use detach/defer rather than releasing their slot.
// A rejected row is included in the inspected count; later rows stay zero and are not executed.
template <typename FreeFn, typename RefreshFn>
size_t executeCommandsV2(SceneGraph& graph, const int32_t* commands, size_t words,
                         int32_t* receipts, size_t receiptCapacity,
                         FreeFn freeNode, RefreshFn refreshVisibility) {
    if (!commands || !receipts || (words & 7u) != 0u || receiptCapacity < words / 8u) return 0;
    const size_t rows = words / 8u;
    for (size_t row = 0; row < rows; ++row) receipts[row] = 0;
    size_t inspected = 0;
    for (size_t row = 0; row < rows; ++row) {
        const int32_t* r = commands + row * 8u;
        const int op = r[0], child = r[1], parent = r[2], before = r[3];
        const uint32_t childGen = static_cast<uint32_t>(r[4]);
        const uint32_t parentGen = static_cast<uint32_t>(r[5]);
        const uint32_t beforeGen = static_cast<uint32_t>(r[6]);
        CommandReceipt receipt = CommandReceipt::InvalidOrDead;
        if (r[7] != 0) {
            receipt = CommandReceipt::InvalidOrDead;
        } else if (op == 1) {
            const bool gensPresent = childGen != 0u
                && (parent == 0 ? parentGen == 0u : parentGen != 0u)
                && (before < 0 ? beforeGen == 0u : beforeGen != 0u);
            receipt = gensPresent
                ? graph.setParentChecked(child, parent, before, childGen, parentGen, beforeGen)
                : CommandReceipt::GenerationMismatch;
        } else if (op == 2) {
            receipt = childGen ? graph.removeFromParent(child, childGen)
                               : CommandReceipt::GenerationMismatch;
        } else if (op == 3) {
            receipt = childGen ? freeNode(child, childGen)
                               : CommandReceipt::GenerationMismatch;
        }
        receipts[row] = static_cast<int32_t>(receipt);
        ++inspected;
        if (commandReceiptSucceeded(receipt) && (op == 1 || op == 2)) refreshVisibility(child);
        if (!commandReceiptSucceeded(receipt)) break;
    }
    return inspected;
}

} // namespace sg
