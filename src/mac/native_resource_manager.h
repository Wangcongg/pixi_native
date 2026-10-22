#pragma once

// Apple-platform asynchronous resource owner. Compressed bytes are read and images are
// decoded away from the JSC/render thread. Decoded pixels stay in native memory and
// are uploaded to an external GL texture in bounded row bands; JavaScript receives
// only width/height plus an opaque texture id.
//
// Everything below is CoreGraphics/GCD plus plain ES3 calls, so the same translation unit
// serves the macOS host (ANGLE's GLES3 headers) and the iOS host (the system OpenGLES
// framework). Only the header that spells those ES3 entry points differs per platform.

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <OpenGLES/ES3/gl.h>
#else
#include <GLES3/gl3.h>
#endif
#include <dispatch/dispatch.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../cmdbuf/decoder.h"
#include "../cmdbuf/ktx2.h"
#include "../common/native_image_residency.h"
#include "../common/native_image_alpha.h"
#include "../common/native_image_decode.h"

// zstd 解压(KTX2 supercompression=2)。宿主 TU 已链接 zstddeclib,这里只声明用到的入口。
extern "C" {
    size_t ZSTD_decompress(void* dst, size_t dstCap, const void* src, size_t srcSize);
    unsigned long long ZSTD_getFrameContentSize(const void* src, size_t srcSize);
    unsigned long long ZSTD_decompressBound(const void* src, size_t srcSize);
    unsigned ZSTD_isError(size_t code);
}
#ifndef ZSTD_CONTENTSIZE_UNKNOWN
#define ZSTD_CONTENTSIZE_UNKNOWN (0ULL - 1)
#define ZSTD_CONTENTSIZE_ERROR   (0ULL - 2)
#endif

namespace macres {

using BytesDone = std::function<void(NSData*)>;
using ImageDone = std::function<void(uint32_t, int, int)>;

struct Stats {
    uint64_t byteHits = 0, byteMisses = 0, byteInflightJoins = 0;
    uint64_t imageHits = 0, imageMisses = 0, imageInflightJoins = 0;
    uint64_t decodedImages = 0, decodeFailures = 0, uploadedImages = 0;
    uint64_t uploadedBytes = 0, evictedImages = 0, evictedBytes = 0;
    uint64_t retainMisses = 0, releaseMisses = 0, renderFrame = 0;
    uint64_t publishedRefs = 0;   // 发布给 JS 的所有权引用(adopt/cancel 前由交接引用持有)
    int adoptRefs = 0;            // 当前在世的软引用总量(≈加载过未绑定/未释放的 Image 存量)
    size_t byteCacheBytes = 0, gpuCacheBytes = 0, pendingUploadBytes = 0;
    size_t byteEntries = 0, imageEntries = 0, pendingUploads = 0, protectedImages = 0;
};

class ResourceManager {
    // Worker blocks must never own or dereference ResourceManager itself: the manager is a
    // process-global on both Apple hosts, while libdispatch work can outlive C++ static teardown.
    // Main-queue completions capture only a weak token and recover the owner after checking this
    // gate. shutdown() invalidates both fields before waiting for worker-only work to finish.
    struct AsyncLifetime {
        std::atomic<bool> accepting{true};
        std::atomic<ResourceManager*> owner{nullptr};
    };
    struct ByteEntry {
        __strong NSData* data = nil;
        bool loading = true;
        uint64_t lastUse = 0;
        std::vector<BytesDone> waiters;
    };
    struct ImageEntry {
        std::string key;
        GLuint gl = 0;
        uint32_t cid = 0;
        // refs 是所有权引用之和,来源有三类,互相独立、各自对称:
        //  1) publish/adopt 引用(**软**,计入 adoptRefs):向 JS(__imgDone)派发 cid 时 +1,由 JS
        //     侧 adopt(转归 Image 持有)后走三条路之一收尾——首次 glTexture 绑定 transferAdopt
        //     (性质转硬,refs 不变)、换 src/dispose releaseAdopt、token 失配 cancel(releaseAdopt);
        //  2) Pixi glTexture retain(硬):TextureSystem 里 __nativeResourceRetain,destroyTexture
        //     对称 release;
        //  3) native 内部消费者 spinex 图集页(硬):done 回调内同步 retain(与发布同调用栈,无交接
        //     窗口,因此不参与 publish),条目销毁经 setTexRelease 对称 release。
        // refs==adoptRefs = 只剩软引用(加载过却从未参与渲染的预载图):过保护期允许预算驱逐,
        // 否则 29 台马拉松里这批图会把显存钉在预算之外;驱逐后 Image 侧经 stale reload 自愈。
        int width = 0, height = 0, uploadedRows = 0, refs = 0, adoptRefs = 0;
        // ASTC/KTX2 路径:像素已是 GPU 原生压缩格式,不能按 RGBA 行带上传,也没有"行"的概念。
        uint32_t compressedFormat = 0;   // 非 0 = 走 glCompressedTexImage2D
        size_t compressedBytes = 0;
        bool needsPremultiply = false;   // KTX DFD 声明 straight alpha:shader 取样后需预乘
        bool loading = true, ready = false, failed = false;
        uint64_t lastUse = 0, failedAt = 0;
        // 交接窗口的渲染帧保护——**必要机制,不可降为 0**。adopt 软引用不参与驱逐决策
        // (refs==adoptRefs 恒可驱逐),"加载完成→首次绑定"窗口的唯一保护就是这层 grace:
        // 会陷入 加载→驱逐→transfer 失败→stale reload→再加载 的抖动风暴。
        // 语义定位:正确性由"驱逐后 stale reload 自愈"闭环保证(窗外驱逐是设计内行为,
        // 不是误删);grace 只负责加载期的稳定性(防抖动),真实渲染帧计数不受加载期
        // 资源操作数量影响。
        uint64_t protectedUntilRenderFrame = 0;
        std::shared_ptr<unsigned char> pixels;
        // 每个 waiter 记录"完成时是否随 cid 发布一个所有权引用"。跨 JS 异步边界的必须 true;
        // 在回调内同步 retain 的 native 消费者必须 false,否则引用无人认领即泄漏。
        std::vector<std::pair<ImageDone, bool>> waiters;
    };
    // Failure is a debounce window, not a session-wide verdict: one flaky HTTP timeout or 404
    // must not poison an asset until restart. After this many frames the key reloads from scratch.
    static const uint64_t kFailureRetryFrames = 300;

    cmd::Decoder* decoder_ = nullptr;
    std::function<NSData*(NSString*)> loader_;
    dispatch_queue_t ioQueue_;
    dispatch_group_t workerGroup_;
    dispatch_semaphore_t cpuSlots_;
    std::shared_ptr<AsyncLifetime> asyncLifetime_;
    std::unordered_map<std::string, std::shared_ptr<ByteEntry>> bytes_;
    std::unordered_map<std::string, std::shared_ptr<ImageEntry>> images_;
    std::unordered_map<uint32_t, std::weak_ptr<ImageEntry>> imagesByCid_;
    std::vector<std::shared_ptr<ImageEntry>> uploadQueue_;
    uint32_t nextCid_ = 9000000; // exact in Float32, separate from Pixi and native text ids
    uint64_t frame_ = 0, renderFrame_ = 0, epoch_ = 1, overBudgetWarnFrame_ = 0;
    size_t byteCacheBytes_ = 0, gpuCacheBytes_ = 0, pendingUploadBytes_ = 0;
    size_t byteCacheLimit_ = 192u << 20;
    size_t gpuCacheLimit_ = 384u << 20;
    Stats totals_;

    static std::string utf8(NSString* s) {
        return s.UTF8String ? std::string(s.UTF8String) : std::string();
    }
    bool acceptsRequests() const {
        return asyncLifetime_ && asyncLifetime_->accepting.load(std::memory_order_acquire);
    }
    static ResourceManager* liveOwner(const std::weak_ptr<AsyncLifetime>& weakLifetime) {
        auto lifetime = weakLifetime.lock();
        if (!lifetime) return nullptr;
        // Completions run on the Apple main queue, the same serial executor that owns reset(),
        // shutdown(), and all cache maps. The atomic token makes the pre-dereference lifetime
        // check independent of `this`; shutdown publishes accepting=false before owner=null.
        ResourceManager* owner = lifetime->owner.load(std::memory_order_acquire);
        if (!owner || !lifetime->accepting.load(std::memory_order_acquire)) return nullptr;
        return owner;
    }
    static uint64_t hashData(NSData* data) {
        const uint8_t* p = static_cast<const uint8_t*>(data.bytes);
        uint64_t h = 1469598103934665603ull;
        for (NSUInteger i = 0; i < data.length; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }
    // KTX2/ASTC 解码:产出的是 GPU 原生压缩数据,不是 RGBA。命中返回 true 并填 outFormat/outBytes。
    // 相比 PNG 路径省掉整个 CPU 解码,显存与上传量也降到 1/4(2048² RGBA 16MB → ASTC 4x4 4MB)。
    static unsigned char* decodeKTX2(NSData* data, int& width, int& height,
                                     uint32_t& outFormat, size_t& outBytes,
                                     bool& outNeedsPremultiply) {
        width = height = 0; outFormat = 0; outBytes = 0; outNeedsPremultiply = false;
        const uint8_t* raw = (const uint8_t*)data.bytes;
        if (!raw || !ktx2::hasMagic(raw, data.length)) return nullptr;
        ktx2::Image img = ktx2::parse(raw, data.length);
        if (!img.ok) { fprintf(stderr, "[ktx2] 解析失败: %s\n", img.error.c_str()); return nullptr; }
        // 只上传 level 0:本项目的 spine 页图与 fgui 图集都不用 mipmap(采样器是 LINEAR 非 MIPMAP)。
        const ktx2::Level& lv = img.levels[0];
        const uint8_t* src = raw + lv.byteOffset;
        unsigned char* out = nullptr;
        size_t outLen = 0;
        if (img.supercompression == 2) {
            unsigned long long cap = ZSTD_getFrameContentSize(src, (size_t)lv.byteLength);
            if (cap == ZSTD_CONTENTSIZE_ERROR) return nullptr;
            if (cap == ZSTD_CONTENTSIZE_UNKNOWN) cap = ZSTD_decompressBound(src, (size_t)lv.byteLength);
            if (!cap || cap == ZSTD_CONTENTSIZE_ERROR || cap > (256ull << 20)) return nullptr;
            out = (unsigned char*)malloc((size_t)cap);
            if (!out) return nullptr;
            size_t got = ZSTD_decompress(out, (size_t)cap, src, (size_t)lv.byteLength);
            if (ZSTD_isError(got)) { free(out); return nullptr; }
            outLen = got;
        } else {
            outLen = (size_t)lv.byteLength;
            out = (unsigned char*)malloc(outLen);
            if (!out) return nullptr;
            memcpy(out, src, outLen);
        }
        width = lv.width; height = lv.height;
        outFormat = img.glInternalFormat; outBytes = outLen;
        outNeedsPremultiply = !img.alphaPremultiplied;
        return out;
    }
    static unsigned char* decode(NSData* data, int& width, int& height, bool sourcePremultiplied=false) {
        if(sourcePremultiplied)return nativeimage::decodeEncodedPMA(data.bytes,data.length,width,height);
        width = height = 0;
        if (!data.length) return nullptr;
        CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr);
        if (!src) return nullptr;
        CGImageRef image = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
        CFRelease(src);
        if (!image) return nullptr;
        const int w = (int)CGImageGetWidth(image), h = (int)CGImageGetHeight(image);
        if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > (16384ull * 16384ull)) {
            CGImageRelease(image); return nullptr;
        }
        const size_t byteCount = (size_t)w * (size_t)h * 4;
        unsigned char* pixels = static_cast<unsigned char*>(malloc(byteCount));
        if (!pixels) { CGImageRelease(image); return nullptr; }
        memset(pixels, 0, byteCount);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef cg = CGBitmapContextCreate(pixels, w, h, 8, (size_t)w * 4, cs,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        if (cg) {
            CGContextSetBlendMode(cg, kCGBlendModeCopy);
            // Matches the existing WebGL shim: row zero is the GL lower row.
            CGContextDrawImage(cg, CGRectMake(0, 0, w, h), image);
            CGContextRelease(cg);
        } else {
            free(pixels); pixels = nullptr;
        }
        CGColorSpaceRelease(cs);
        CGImageRelease(image);
        if (pixels) { width = w; height = h; }
        return pixels;
    }
    void completeImageFailure(const std::shared_ptr<ImageEntry>& entry) {
        entry->loading = false; entry->ready = true; entry->failed = true; entry->failedAt = frame_;
        auto waiters = std::move(entry->waiters);
        for (auto& waiter : waiters) if (waiter.first) waiter.first(0, 0, 0);   // cid=0:不携带引用
    }
    // 完成派发前按 waiter 的 publish 标记上账引用。必须先全部 +1 再逐个回调:第一个回调可能
    // 同步 release(宿主侧 JS 上下文失配退还),若边派发边上账,后续 waiter 的引用会短暂为 0,
    // 又回到"发布了却还没人持有"的老缝隙。
    void publishToWaiters(const std::shared_ptr<ImageEntry>& entry,
                          std::vector<std::pair<ImageDone, bool>>& waiters) {
        for (auto& waiter : waiters) if (waiter.second) { entry->refs++; entry->adoptRefs++; totals_.publishedRefs++; }
        for (auto& waiter : waiters) if (waiter.first) waiter.first(entry->cid, entry->width, entry->height);
    }
    void requestImage(const std::string& key, NSData* suppliedData, NSString* url, ImageDone done,
                      bool publishRef, bool sourcePremultiplied=false) {
        if (!acceptsRequests()) { if (done) done(0, 0, 0); return; }
        // `key` is a reference parameter. Objective-C blocks retain captured C++
        // objects, but capturing this reference would keep only the caller's stack
        // address. Own a stable copy across both worker and main-queue blocks.
        auto stableKey = std::make_shared<std::string>(key);
        ++frame_;
        auto found = images_.find(key);
        if (found != images_.end()) {
            auto entry = found->second; entry->lastUse = frame_;
            if (entry->ready && entry->failed && frame_ > entry->failedAt + kFailureRetryFrames) {
                images_.erase(found);   // debounce elapsed → drop the negative entry, reload below
            } else if (entry->ready) {
                if (!entry->failed) {
                    if (publishRef) { entry->refs++; entry->adoptRefs++; totals_.publishedRefs++; }   // 命中即发布:先上账再回调
                    entry->protectedUntilRenderFrame = NativeImageResidencyPolicy::deadline(
                        renderFrame_, NativeImageResidencyPolicy::kPublishGraceFrames);
                }
                totals_.imageHits++; done(entry->failed ? 0 : entry->cid, entry->width, entry->height);
                return;
            } else {
                totals_.imageInflightJoins++; entry->waiters.push_back({std::move(done), publishRef});
                return;
            }
        }
        totals_.imageMisses++;
        auto entry = std::make_shared<ImageEntry>(); entry->key = *stableKey; entry->lastUse = frame_;
        entry->waiters.push_back({std::move(done), publishRef}); images_[*stableKey] = entry;
        const uint64_t requestEpoch = epoch_;
        __strong NSData* copied = suppliedData;
        __strong NSString* copiedUrl = [url copy];
        auto loader = loader_;
        dispatch_semaphore_t cpuSlots = cpuSlots_;
        std::weak_ptr<AsyncLifetime> weakLifetime = asyncLifetime_;
        if (getenv("GAME_RESOURCE_DEBUG")) fprintf(stderr, "[native-resource] image request %s supplied=%lu\n", stableKey->c_str(), (unsigned long)copied.length);
        // The group covers only this worker segment. Its main-queue completion is deliberately
        // outside the group, so shutdown on the main thread can wait without deadlocking itself.
        dispatch_group_async(workerGroup_, ioQueue_, ^{
            if (getenv("GAME_RESOURCE_DEBUG")) fprintf(stderr, "[native-resource] image worker begin %s\n", stableKey->c_str());
            NSData* compressed = copied ?: (loader ? loader(copiedUrl) : nil);
            if (getenv("GAME_RESOURCE_DEBUG")) fprintf(stderr, "[native-resource] image bytes %s = %lu\n", stableKey->c_str(), (unsigned long)compressed.length);
            dispatch_semaphore_wait(cpuSlots, DISPATCH_TIME_FOREVER);
            int width = 0, height = 0;
            // 先嗅探 KTX2(spine 页图/图集的 ASTC 形态),不是才回落 ImageIO 的 PNG/JPG 解码。
            uint32_t ktxFormat = 0; size_t ktxBytes = 0; bool needsPremultiply = false;
            unsigned char* raw = ResourceManager::decodeKTX2(
                compressed, width, height, ktxFormat, ktxBytes, needsPremultiply);
            if (!raw) {
                ktxFormat = 0; ktxBytes = 0; needsPremultiply = false;
                raw = ResourceManager::decode(compressed, width, height,sourcePremultiplied);
            }
            dispatch_semaphore_signal(cpuSlots);
            if (getenv("GAME_RESOURCE_DEBUG")) fprintf(stderr, "[native-resource] image decoded %s = %dx%d ptr=%p\n", stableKey->c_str(), width, height, raw);
            std::shared_ptr<unsigned char> pixels(raw, [](unsigned char* p){ if (p) free(p); });
            dispatch_async(dispatch_get_main_queue(), ^{
                ResourceManager* owner = ResourceManager::liveOwner(weakLifetime);
                if (!owner || requestEpoch != owner->epoch_) return;
                auto current = owner->images_.find(*stableKey);
                if (current == owner->images_.end() || current->second.get() != entry.get()) return;
                if (!pixels) {
                    owner->totals_.decodeFailures++;
                    owner->completeImageFailure(entry);
                    return;
                }
                entry->width = width; entry->height = height; entry->pixels = pixels;
                entry->loading = false; entry->uploadedRows = 0;
                entry->compressedFormat = ktxFormat; entry->compressedBytes = ktxBytes;
                entry->needsPremultiply = needsPremultiply;
                // ASTC 的显存占用按压缩后字节算(不是 w*h*4),否则 GPU 缓存预算会被高估 4 倍。
                const size_t bytes = ktxFormat ? ktxBytes : (size_t)width * (size_t)height * 4;
                owner->pendingUploadBytes_ += bytes; owner->totals_.decodedImages++;
                owner->uploadQueue_.push_back(entry);
            });
        });
    }
    void eraseImage(const std::shared_ptr<ImageEntry>& entry) {
        if (!entry || entry->refs > entry->adoptRefs || !entry->ready) return;   // 硬引用(glTexture/spinex)在:绝不驱逐
        // 与上传时的记账口径一致:ASTC 按压缩字节,否则 GPU 缓存会被错误地多扣 4 倍。
        const size_t bytes = entry->compressedFormat ? entry->compressedBytes
                                                     : (size_t)entry->width * (size_t)entry->height * 4;
        if (entry->cid) { decoder_->unregisterExternalTexture(entry->cid); imagesByCid_.erase(entry->cid); }
        if (entry->gl) glDeleteTextures(1, &entry->gl);
        gpuCacheBytes_ = gpuCacheBytes_ >= bytes ? gpuCacheBytes_ - bytes : 0;
        totals_.evictedImages++; images_.erase(entry->key);
    }
    void collectImages() {
        if (gpuCacheBytes_ <= gpuCacheLimit_) return;
        std::vector<std::shared_ptr<ImageEntry>> candidates;
        for (auto& pair : images_) {
            auto& entry = pair.second;
            if (NativeImageResidencyPolicy::canEvict(entry->ready, entry->refs, entry->adoptRefs,
                                                     renderFrame_, entry->protectedUntilRenderFrame))
                candidates.push_back(entry);   // failed entries are collectable too (gl=0, cheap no-op)
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b){ return a->lastUse < b->lastUse; });
        for (auto& entry : candidates) { if (gpuCacheBytes_ <= gpuCacheLimit_) break; eraseImage(entry); }
        // 回收后仍超预算 = 活跃工作集真的超了,或所有权引用泄漏(adopt 后没人 release)。
        // 限频告警,配合 JS 侧 imgAdopts/imgDisowns/imgFinalizes 计数定位是哪一种。
        if (gpuCacheBytes_ > gpuCacheLimit_ && renderFrame_ >= overBudgetWarnFrame_ + 600) {
            overBudgetWarnFrame_ = renderFrame_;
            size_t hardEntries=0,hardBytes=0,protectedEntries=0,protectedBytes=0;
            int hardRefs=0,adoptRefs=0;
            for (const auto& pair : images_) {
                const auto& e=pair.second;if(!e||!e->ready)continue;
                const size_t bytes=e->compressedFormat?e->compressedBytes:(size_t)e->width*(size_t)e->height*4;
                hardRefs+=std::max(0,e->refs-e->adoptRefs);adoptRefs+=std::max(0,e->adoptRefs);
                if(e->refs>e->adoptRefs){hardEntries++;hardBytes+=bytes;}
                else if(renderFrame_<=e->protectedUntilRenderFrame){protectedEntries++;protectedBytes+=bytes;}
            }
            fprintf(stderr, "[native-resource] GPU 缓存超预算且无可回收项: %.1fMB / %.1fMB "
                    "(entries=%zu hard=%zu/%.1fMB refs=%d protected=%zu/%.1fMB adoptRefs=%d)\n",
                    gpuCacheBytes_ / 1048576.0, gpuCacheLimit_ / 1048576.0, images_.size(),
                    hardEntries,hardBytes/1048576.0,hardRefs,
                    protectedEntries,protectedBytes/1048576.0,adoptRefs);
        }
    }
    void collectBytes() {
        if (byteCacheBytes_ <= byteCacheLimit_) return;
        std::vector<std::pair<std::string, std::shared_ptr<ByteEntry>>> candidates;
        for (auto& pair : bytes_) if (!pair.second->loading) candidates.push_back(pair);
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b){ return a.second->lastUse < b.second->lastUse; });
        for (auto& pair : candidates) {
            if (byteCacheBytes_ <= byteCacheLimit_) break;
            size_t n = pair.second->data.length;
            byteCacheBytes_ = byteCacheBytes_ >= n ? byteCacheBytes_ - n : 0;
            totals_.evictedBytes += n; bytes_.erase(pair.first);
        }
    }

public:
    ResourceManager(cmd::Decoder* decoder = nullptr) : decoder_(decoder) {
        ioQueue_ = dispatch_queue_create("pixi.resources", DISPATCH_QUEUE_CONCURRENT);
        workerGroup_ = dispatch_group_create();
        // macOS has materially more CPU/memory headroom than the minimum supported iOS devices.
        // Let small KTX2 batches prepare four-wide there; iOS keeps the conservative two-wide
        // bound that protects ImageIO/RGBA decode peaks.
        cpuSlots_ = dispatch_semaphore_create(TARGET_OS_IPHONE ? 2 : 4);
        asyncLifetime_ = std::make_shared<AsyncLifetime>();
        asyncLifetime_->owner.store(this, std::memory_order_release);
    }
    ~ResourceManager() { shutdown(); }
    void configure(cmd::Decoder* decoder, std::function<NSData*(NSString*)> loader) {
        decoder_ = decoder; loader_ = std::move(loader);
    }
    void requestBytes(NSString* url, BytesDone done) {
        if (!acceptsRequests()) { if (done) done(nil); return; }
        const std::string key = utf8(url); ++frame_;
        auto found = bytes_.find(key);
        if (found != bytes_.end()) {
            auto entry = found->second; entry->lastUse = frame_;
            if (entry->loading) { totals_.byteInflightJoins++; entry->waiters.push_back(std::move(done)); }
            else { totals_.byteHits++; done(entry->data); }
            return;
        }
        totals_.byteMisses++;
        auto entry = std::make_shared<ByteEntry>(); entry->lastUse = frame_; entry->waiters.push_back(std::move(done));
        bytes_[key] = entry; const uint64_t requestEpoch = epoch_; __strong NSString* copiedUrl = [url copy];
        auto loader = loader_;
        std::weak_ptr<AsyncLifetime> weakLifetime = asyncLifetime_;
        dispatch_group_async(workerGroup_, ioQueue_, ^{
            NSData* data = loader ? loader(copiedUrl) : nil;
            dispatch_async(dispatch_get_main_queue(), ^{
                ResourceManager* owner = ResourceManager::liveOwner(weakLifetime);
                if (!owner || requestEpoch != owner->epoch_) return;
                auto current = owner->bytes_.find(key);
                if (current == owner->bytes_.end() || current->second.get() != entry.get()) return;
                entry->loading = false;
                if (data) { entry->data = data; owner->byteCacheBytes_ += data.length; }
                else owner->bytes_.erase(key);   // never negative-cache a failed read; erase before the callbacks so an immediate retry misses
                auto waiters = std::move(entry->waiters); for (auto& callback : waiters) if (callback) callback(data);
                owner->collectBytes();
            });
        });
    }
    // publishRef=true 表示完成回调携带一个已上账的所有权引用(refs 先 +1 再派发 cid),调用方
    // 必须保证它最终被 adopt(转交 JS Image 持有)或 release(送达失败退还)。跨 JS 异步边界
    // (__imgDone→__ioQ→Pixi)的路径必须传 true;在回调内同步 retain 的 native 消费者保持 false。
    void requestImageURL(NSString* url, ImageDone done, bool publishRef = false, bool sourcePremultiplied=false) {
        requestImage(nativeimage::decodeCacheKey("url:" + utf8(url),sourcePremultiplied), nil, url, std::move(done), publishRef,sourcePremultiplied);
    }
    void requestImageData(NSData* data, ImageDone done, bool publishRef = false) {
        char key[96]; snprintf(key, sizeof(key), "blob:%llu:%lu", (unsigned long long)hashData(data), (unsigned long)data.length);
        requestImage(key, data, nil, std::move(done), publishRef);
    }
    // Prepared package entries already have a content-stable identity. Using it
    // avoids scanning a multi-megabyte compressed image on the JSC/render thread
    // merely to derive the generic blob hash.
    void requestImageDataNamed(NSString* stableName, NSData* data, ImageDone done, bool publishRef = false) {
        const std::string name = utf8(stableName);
        if (name.empty()) { requestImageData(data, std::move(done), publishRef); return; }
        requestImage("named:" + name, data, nil, std::move(done), publishRef);
    }
    void beginFrame(size_t byteBudget = (4u << 20),
                    int textureBudget = TARGET_OS_IPHONE ? 2 : 4) {
        ++frame_; ++renderFrame_;
        if (!decoder_ || uploadQueue_.empty()) { collectImages(); return; }
        GLint previous = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        size_t used = 0; int completed = 0;
        while (!uploadQueue_.empty() && completed < textureBudget) {
            auto entry = uploadQueue_.front();
            if (!entry || !entry->pixels || entry->width <= 0 || entry->height <= 0) { uploadQueue_.erase(uploadQueue_.begin()); continue; }
            if (!entry->gl) {
                glGenTextures(1, &entry->gl); entry->cid = nextCid_++;
                decoder_->registerExternalTexture(entry->cid, entry->gl); imagesByCid_[entry->cid] = entry;
                glBindTexture(GL_TEXTURE_2D, entry->gl); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                // 压缩纹理没有"先分配再逐行填"的用法:glCompressedTexImage2D 必须带完整数据一次上传。
                if (!entry->compressedFormat)
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, entry->width, entry->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            } else glBindTexture(GL_TEXTURE_2D, entry->gl);
            // ---- ASTC/KTX2:整块一次传完,不参与行带切分 ----
            // 分带上传的前提是"像素按行可切",而块压缩格式的一行不是独立单元。好在压缩后体积只有
            // RGBA 的 1/4(2048² 为 4MB),一次传完的尖峰可接受;仍计入 used 让本帧预算继续生效。
            if (entry->compressedFormat) {
                glCompressedTexImage2D(GL_TEXTURE_2D, 0, entry->compressedFormat,
                                       entry->width, entry->height, 0,
                                       (GLsizei)entry->compressedBytes, entry->pixels.get());
                used += entry->compressedBytes; totals_.uploadedBytes += entry->compressedBytes;
                pendingUploadBytes_ = pendingUploadBytes_ >= entry->compressedBytes ? pendingUploadBytes_ - entry->compressedBytes : 0;
                gpuCacheBytes_ += entry->compressedBytes;
                entry->pixels.reset(); entry->ready = true; entry->failed = false;
                entry->lastUse = frame_; totals_.uploadedImages++;
                entry->protectedUntilRenderFrame = NativeImageResidencyPolicy::deadline(
                    renderFrame_, NativeImageResidencyPolicy::kPublishGraceFrames);
                auto waiters = std::move(entry->waiters);
                publishToWaiters(entry, waiters);
                uploadQueue_.erase(uploadQueue_.begin()); completed++;
                if (used >= byteBudget) break;
                continue;
            }
            const size_t rowBytes = (size_t)entry->width * 4;
            size_t remainingBudget = used < byteBudget ? byteBudget - used : 0;
            int rows = (int)std::max<size_t>(1, remainingBudget / std::max<size_t>(1, rowBytes));
            rows = std::min(rows, entry->height - entry->uploadedRows);
            const unsigned char* source = entry->pixels.get() + (size_t)entry->uploadedRows * rowBytes;
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, entry->uploadedRows, entry->width, rows, GL_RGBA, GL_UNSIGNED_BYTE, source);
            entry->uploadedRows += rows; const size_t bandBytes = (size_t)rows * rowBytes;
            used += bandBytes; totals_.uploadedBytes += bandBytes;
            if (entry->uploadedRows >= entry->height) {
                const size_t bytes = (size_t)entry->width * (size_t)entry->height * 4;
                pendingUploadBytes_ = pendingUploadBytes_ >= bytes ? pendingUploadBytes_ - bytes : 0;
                gpuCacheBytes_ += bytes; entry->pixels.reset(); entry->ready = true; entry->failed = false;
                entry->lastUse = frame_; totals_.uploadedImages++;
                entry->protectedUntilRenderFrame = NativeImageResidencyPolicy::deadline(
                    renderFrame_, NativeImageResidencyPolicy::kPublishGraceFrames);
                auto waiters = std::move(entry->waiters);
                publishToWaiters(entry, waiters);
                uploadQueue_.erase(uploadQueue_.begin()); completed++;
            }
            if (used >= byteBudget) break;
        }
        glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
        collectImages();
    }
    // Read-only liveness probe for cached external GL wrappers. A Pixi BaseTexture can outlive
    // the native residency entry it once named; checking the registry must not manufacture a
    // reference (or refresh LRU state) merely because JavaScript is deciding whether to reuse it.
    bool alive(uint32_t cid) const {
        auto it = imagesByCid_.find(cid); if (it == imagesByCid_.end()) return false;
        auto entry = it->second.lock();
        return entry && entry->ready && entry->gl != 0;
    }
    // ASTC payloads preserve the KTX DFD alpha convention. This read-only query lets the JS
    // Image/BaseTexture bridge keep that convention even when the requested URL ended in .webp
    // and the native fallback actually loaded .ktx2.
    bool needsPremultiply(uint32_t cid) const {
        auto it = imagesByCid_.find(cid); if (it == imagesByCid_.end()) return false;
        auto entry = it->second.lock();
        return entry && entry->ready && entry->gl != 0 && entry->needsPremultiply;
    }
    bool retain(uint32_t cid) {
        auto it = imagesByCid_.find(cid); if (it == imagesByCid_.end()) { totals_.retainMisses++; return false; }
        auto entry = it->second.lock(); if (!entry) { totals_.retainMisses++; return false; }
        entry->refs++; entry->lastUse = ++frame_; return true;
    }
    bool release(uint32_t cid) {
        auto it = imagesByCid_.find(cid); if (it == imagesByCid_.end()) { totals_.releaseMisses++; return false; }
        auto entry = it->second.lock(); if (!entry) { totals_.releaseMisses++; return false; }
        // 普通口只释放 glTexture/spinex 的硬引用。没有硬引用时拒绝操作，避免重复 release
        // 把 Image 的 adopt 软引用偷走，破坏 refs>=adoptRefs 不变量。
        if (!NativeImageResidencyPolicy::hasHardReference(entry->refs, entry->adoptRefs)) {
            totals_.releaseMisses++; return false;
        }
        entry->refs--;
        if (entry->refs == 0) entry->protectedUntilRenderFrame = NativeImageResidencyPolicy::deadline(
            renderFrame_, NativeImageResidencyPolicy::kReleaseGraceFrames);
        entry->lastUse = ++frame_; collectImages(); return true;
    }
    // adopt 引用(软)的两个收尾口——与普通 retain/release 分账,让"只剩软引用"的条目可被预算驱逐:
    // transferAdopt:首次 glTexture 绑定,引用性质软→硬(refs 不变,adoptRefs-1);
    // releaseAdopt:换 src/dispose/cancel,软引用直接退还(refs-1,adoptRefs-1)。
    bool transferAdopt(uint32_t cid) {
        auto it = imagesByCid_.find(cid); if (it == imagesByCid_.end()) return false;
        auto entry = it->second.lock();
        if (!entry || !NativeImageResidencyPolicy::hasAdoptReference(entry->refs, entry->adoptRefs)) return false;
        entry->adoptRefs--; entry->lastUse = ++frame_; return true;
    }
    bool releaseAdopt(uint32_t cid) {
        auto it = imagesByCid_.find(cid); if (it == imagesByCid_.end()) { totals_.releaseMisses++; return false; }
        auto entry = it->second.lock(); if (!entry) { totals_.releaseMisses++; return false; }
        if (!NativeImageResidencyPolicy::hasAdoptReference(entry->refs, entry->adoptRefs)) {
            totals_.releaseMisses++; return false;
        }
        entry->adoptRefs--; entry->refs--;
        if (entry->refs == 0) entry->protectedUntilRenderFrame = NativeImageResidencyPolicy::deadline(
            renderFrame_, NativeImageResidencyPolicy::kReleaseGraceFrames);
        entry->lastUse = ++frame_; collectImages(); return true;
    }
    Stats stats() const {
        Stats out = totals_; out.byteCacheBytes = byteCacheBytes_; out.gpuCacheBytes = gpuCacheBytes_;
        out.pendingUploadBytes = pendingUploadBytes_; out.byteEntries = bytes_.size(); out.imageEntries = images_.size();
        out.pendingUploads = uploadQueue_.size(); out.renderFrame = renderFrame_;
        for (const auto& pair : images_) {
            const auto& entry = pair.second;
            if (entry && entry->ready && entry->refs == entry->adoptRefs
                && renderFrame_ <= entry->protectedUntilRenderFrame) out.protectedImages++;
            if (entry) out.adoptRefs += entry->adoptRefs;
        }
        return out;
    }
    // Terminal quiesce for application exit. Do not use this for JS/runtime reload: reset() keeps
    // accepting requests and relies on epoch invalidation, preserving the existing non-blocking
    // reload behaviour. The group excludes main-queue completions, so a main-thread caller waits
    // only for file/decode workers; queued completions observe the dead token and discard safely.
    void shutdown() {
        auto lifetime = asyncLifetime_;
        if (!lifetime) return;
        const bool wasAccepting = lifetime->accepting.exchange(false, std::memory_order_acq_rel);
        lifetime->owner.store(nullptr, std::memory_order_release);
        if (wasAccepting) ++epoch_;
        if (workerGroup_) dispatch_group_wait(workerGroup_, DISPATCH_TIME_FOREVER);
        if (wasAccepting) loader_ = nullptr;
    }
    void reset() {
        ++epoch_;
        for (auto& pair : images_) {
            auto& entry = pair.second;
            if (entry->cid && decoder_) decoder_->unregisterExternalTexture(entry->cid);
            if (entry->gl) glDeleteTextures(1, &entry->gl);
        }
        bytes_.clear(); images_.clear(); imagesByCid_.clear(); uploadQueue_.clear();
        byteCacheBytes_ = gpuCacheBytes_ = pendingUploadBytes_ = 0; frame_ = renderFrame_ = overBudgetWarnFrame_ = 0; totals_ = Stats{};
    }
};

} // namespace macres
