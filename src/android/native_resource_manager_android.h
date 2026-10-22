#pragma once

// Android 版异步资源属主 —— 与 src/mac/native_resource_manager.h 逐段同构:
// 压缩字节读取与图片解码都不在 JS/GL 线程;解码像素留在 native,按行带预算上传外部 GL 纹理;
// JS 只拿 width/height + 不透明纹理 id。引用计数/驱逐/grace/上传泵与 mac 版逐字同源。
// 平台差异只有四处:NSData→shared_ptr<vector<uint8_t>>、NSString→std::string、
// ImageIO 解码→stb_image(保持 top-down + premultiply;mac 的 CGBitmapContext 内存行 0 就是图顶,与 KTX2/文本面同向,不翻转)、
// GCD→宿主注入的 postWork/postMain(aplat::WorkPool/MainQueue)+CpuGate。
// namespace 沿用 macres:宿主绑定代码(cbNativeResource*)三平台零改动。

#include <GLES3/gl3.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cmdbuf/decoder.h"
#include "cmdbuf/ktx2.h"
#include "common/native_image_residency.h"
#include "common/native_image_alpha.h"

extern "C" {
    size_t ZSTD_decompress(void* dst, size_t dstCap, const void* src, size_t srcSize);
    unsigned long long ZSTD_getFrameContentSize(const void* src, size_t srcSize);
    unsigned long long ZSTD_decompressBound(const void* src, size_t srcSize);
    unsigned ZSTD_isError(size_t code);
    const char* ZSTD_getErrorName(size_t code);
    // stb_image(implementation 在宿主 TU;这里只取声明,避免二次展开 STB_IMAGE_IMPLEMENTATION)
    unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y,
                                         int* channels_in_file, int desired_channels);
    void stbi_image_free(void* retval_from_stbi_load);
    const char* stbi_failure_reason(void);
}
#ifndef ZSTD_CONTENTSIZE_UNKNOWN
#define ZSTD_CONTENTSIZE_UNKNOWN (0ULL - 1)
#define ZSTD_CONTENTSIZE_ERROR   (0ULL - 2)
#endif

namespace macres {
// stb_image 不认的格式(WebP 等)的平台回退解码器:输出 premultiplied RGBA8 top-down(与 decode 同约定),由宿主在启动时设置。
inline unsigned char* (*gDecodeFallback)(const uint8_t* bytes, size_t len, int* width, int* height,
                                        const std::string& url, bool sourcePremultiplied) = nullptr;

using Bytes = std::shared_ptr<std::vector<uint8_t>>;   // 空指针 = 读取失败
using BytesDone = std::function<void(const Bytes&)>;
using ImageDone = std::function<void(uint32_t, int, int)>;
using Dispatch = std::function<void(std::function<void()>)>;
using ImageDiagnostic = std::function<void(const std::string&, bool, size_t, const std::string&)>;

struct Stats {
    uint64_t byteHits = 0, byteMisses = 0, byteInflightJoins = 0;
    uint64_t imageHits = 0, imageMisses = 0, imageInflightJoins = 0;
    uint64_t decodedImages = 0, decodeFailures = 0, uploadedImages = 0;
    uint64_t uploadedBytes = 0, evictedImages = 0, evictedBytes = 0;
    uint64_t retainMisses = 0, releaseMisses = 0, renderFrame = 0;
    uint64_t publishedRefs = 0;
    int adoptRefs = 0;
    size_t byteCacheBytes = 0, gpuCacheBytes = 0, pendingUploadBytes = 0;
    size_t byteEntries = 0, imageEntries = 0, pendingUploads = 0, protectedImages = 0;
};

class ResourceManager {
    struct ByteEntry {
        Bytes data;
        bool loading = true;
        uint64_t lastUse = 0;
        std::vector<BytesDone> waiters;
    };
    struct ImageEntry {
        std::string key;
        GLuint gl = 0;
        uint32_t cid = 0;
        // refs 语义(软/硬引用三来源、adoptRefs 分账、grace 保护)与 mac 版逐字一致,见其注释。
        int width = 0, height = 0, uploadedRows = 0, refs = 0, adoptRefs = 0;
        uint32_t compressedFormat = 0;   // 非 0 = ASTC/KTX2,走 glCompressedTexImage2D
        size_t compressedBytes = 0;
        bool needsPremultiply = false;   // KTX DFD 声明 straight alpha:shader 取样后需预乘
        bool loading = true, ready = false, failed = false;
        uint64_t lastUse = 0;
        uint64_t protectedUntilRenderFrame = 0;
        std::shared_ptr<unsigned char> pixels;
        std::vector<std::pair<ImageDone, bool>> waiters;
    };
    cmd::Decoder* decoder_ = nullptr;
    std::function<Bytes(const std::string&)> loader_;
    Dispatch postWork_;    // 后台并发队列(WorkPool)
    Dispatch postMain_;    // JS/GL 线程队列(MainQueue,帧头 drain)
    std::function<void()> cpuAcquire_;   // CPU 解码闸门(2 槽)
    std::function<void()> cpuRelease_;
    ImageDiagnostic imageDiagnostic_;
    std::unordered_map<std::string, std::shared_ptr<ByteEntry>> bytes_;
    std::unordered_map<std::string, std::shared_ptr<ImageEntry>> images_;
    std::unordered_map<uint32_t, std::weak_ptr<ImageEntry>> imagesByCid_;
    std::vector<std::shared_ptr<ImageEntry>> uploadQueue_;
    uint32_t nextCid_ = 9000000;   // Float32 可精确表示,与 Pixi/原生文本 id 分域
    uint64_t frame_ = 0, renderFrame_ = 0, epoch_ = 1, overBudgetWarnFrame_ = 0;
    size_t byteCacheBytes_ = 0, gpuCacheBytes_ = 0, pendingUploadBytes_ = 0;
    size_t byteCacheLimit_ = 192u << 20;
    size_t gpuCacheLimit_ = 384u << 20;
    Stats totals_;

    static uint64_t hashData(const Bytes& data) {
        uint64_t h = 1469598103934665603ull;
        if (data) for (uint8_t b : *data) { h ^= b; h *= 1099511628211ull; }
        return h;
    }
    static unsigned char* decodeKTX2(const Bytes& data, int& width, int& height,
                                     uint32_t& outFormat, size_t& outBytes,
                                     bool& outNeedsPremultiply, std::string& error) {
        width = height = 0; outFormat = 0; outBytes = 0; outNeedsPremultiply = false;
        const uint8_t* raw = data ? data->data() : nullptr;
        const size_t rawLen = data ? data->size() : 0;
        if (!raw || !ktx2::hasMagic(raw, rawLen)) return nullptr;
        ktx2::Image img = ktx2::parse(raw, rawLen);
        if (!img.ok) {
            error = "KTX2: " + img.error;
            fprintf(stderr, "[ktx2] 解析失败: %s\n", img.error.c_str());
            return nullptr;
        }
        const ktx2::Level& lv = img.levels[0];
        const uint8_t* src = raw + lv.byteOffset;
        unsigned char* out = nullptr;
        size_t outLen = 0;
        if (img.supercompression == 2) {
            unsigned long long cap = ZSTD_getFrameContentSize(src, (size_t)lv.byteLength);
            if (cap == ZSTD_CONTENTSIZE_ERROR) { error = "Zstd: invalid frame header"; return nullptr; }
            if (cap == ZSTD_CONTENTSIZE_UNKNOWN) cap = ZSTD_decompressBound(src, (size_t)lv.byteLength);
            if (!cap || cap == ZSTD_CONTENTSIZE_ERROR || cap > (256ull << 20)) { error = "Zstd: invalid decoded size " + std::to_string(cap); return nullptr; }
            out = (unsigned char*)malloc((size_t)cap);
            if (!out) { error = "KTX2: allocation failed"; return nullptr; }
            size_t got = ZSTD_decompress(out, (size_t)cap, src, (size_t)lv.byteLength);
            if (ZSTD_isError(got)) { error = std::string("Zstd: ") + ZSTD_getErrorName(got); free(out); return nullptr; }
            outLen = got;
        } else {
            outLen = (size_t)lv.byteLength;
            out = (unsigned char*)malloc(outLen);
            if (!out) { error = "KTX2: allocation failed"; return nullptr; }
            memcpy(out, src, outLen);
        }
        width = lv.width; height = lv.height;
        outFormat = img.glInternalFormat; outBytes = outLen;
        outNeedsPremultiply = !img.alphaPremultiplied;
        return out;
    }
    // PNG/JPG → premultiplied RGBA8 top-down(行 0 = 图顶,与 mac CGBitmapContext 内存、KTX2、文本面同向;stb 输出 top-down straight,只预乘不翻转。
    // 2026-09-03 模拟器实证:此前"翻成 bottom-up 对齐 CG 左下原点"是误读,导致所有 png/jpg 上下颠倒而文字/KTX2 正常)。
    static unsigned char* decode(const Bytes& data, int& width, int& height, std::string& error,
                                 const std::string& url, bool sourcePremultiplied) {
        width = height = 0;
        if (!data || data->empty()) { if (error.empty()) error = "Resource bytes unavailable"; return nullptr; }
        int w = 0, h = 0, comp = 0;
        unsigned char* src = stbi_load_from_memory(data->data(), (int)data->size(), &w, &h, &comp, 4);
        if (!src) {
            if (error.empty()) error = std::string("stb: ") + (stbi_failure_reason() ? stbi_failure_reason() : "decode failed");
            if (gDecodeFallback) {
                int fw = 0, fh = 0;
                unsigned char* fb = gDecodeFallback(data->data(), data->size(), &fw, &fh, url, sourcePremultiplied);
                if (fb) { width = fw; height = fh; error.clear(); }
                else error += "; platform decoder returned no pixels";
                return fb;
            }
            return nullptr;
        }
        if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > (16384ull * 16384ull)) { error = "Image dimensions out of bounds"; stbi_image_free(src); return nullptr; }
        const size_t rowBytes = (size_t)w * 4;
        unsigned char* pixels = (unsigned char*)malloc(rowBytes * (size_t)h);
        if (!pixels) { error = "RGBA allocation failed"; stbi_image_free(src); return nullptr; }
        nativeimage::copyPremultipliedRGBA(pixels, src, (size_t)w * h, sourcePremultiplied);
        stbi_image_free(src);
        width = w; height = h;
        return pixels;
    }
    void completeImageFailure(const std::shared_ptr<ImageEntry>& entry) {
        entry->loading = false; entry->ready = true; entry->failed = true;
        // Erase before callbacks: an explicit retry (including a reentrant one) must fetch again.
        // In-flight callers still share this entry and receive the same failure exactly once.
        auto current = images_.find(entry->key);
        if (current != images_.end() && current->second == entry) images_.erase(current);
        auto waiters = std::move(entry->waiters);
        for (auto& waiter : waiters) if (waiter.first) waiter.first(0, 0, 0);   // cid=0:不携带引用
    }
    // 先全部 +1 再逐个回调(与 mac 版同,防"边派发边上账"的引用空窗)。
    void publishToWaiters(const std::shared_ptr<ImageEntry>& entry,
                          std::vector<std::pair<ImageDone, bool>>& waiters) {
        for (auto& waiter : waiters) if (waiter.second) { entry->refs++; entry->adoptRefs++; totals_.publishedRefs++; }
        for (auto& waiter : waiters) if (waiter.first) waiter.first(entry->cid, entry->width, entry->height);
    }
    void requestImage(const std::string& key, Bytes suppliedData, const std::string& url, ImageDone done,
                      bool publishRef, bool sourcePremultiplied = false) {
        auto stableKey = std::make_shared<std::string>(nativeimage::decodeCacheKey(key, sourcePremultiplied));
        ++frame_;
        auto found = images_.find(*stableKey);
        if (found != images_.end()) {
            auto entry = found->second; entry->lastUse = frame_;
            if (entry->ready) {
                if (!entry->failed) {
                    if (publishRef) { entry->refs++; entry->adoptRefs++; totals_.publishedRefs++; }
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
        Bytes copied = std::move(suppliedData);
        auto copiedUrl = std::make_shared<std::string>(url);
        if (getenv("GAME_RESOURCE_DEBUG")) fprintf(stderr, "[native-resource] image request %s supplied=%zu\n", stableKey->c_str(), copied ? copied->size() : 0);
        postWork_([this, stableKey, entry, requestEpoch, copied, copiedUrl, sourcePremultiplied] {
            Bytes compressed = copied ? copied : (loader_ ? loader_(*copiedUrl) : Bytes());
            if (cpuAcquire_) cpuAcquire_();
            int width = 0, height = 0;
            uint32_t ktxFormat = 0; size_t ktxBytes = 0; bool needsPremultiply = false;
            std::string error;
            unsigned char* raw = decodeKTX2(compressed, width, height, ktxFormat, ktxBytes,
                                            needsPremultiply, error);
            if (!raw) { ktxFormat = 0; ktxBytes = 0; needsPremultiply = false; raw = decode(compressed, width, height, error, *copiedUrl, sourcePremultiplied); }
            if (cpuRelease_) cpuRelease_();
            std::shared_ptr<unsigned char> pixels(raw, [](unsigned char* p){ if (p) free(p); });
            const size_t inputBytes = compressed ? compressed->size() : 0;
            postMain_([this, stableKey, copiedUrl, entry, requestEpoch, pixels, width, height, ktxFormat, ktxBytes, needsPremultiply, error, inputBytes] {
                if (requestEpoch != epoch_) return;
                auto current = images_.find(*stableKey);
                if (current == images_.end() || current->second.get() != entry.get()) return;
                if (imageDiagnostic_) imageDiagnostic_(*copiedUrl, !!pixels, inputBytes, pixels ? "" : error);
                if (!pixels) { totals_.decodeFailures++; completeImageFailure(entry); return; }
                entry->width = width; entry->height = height; entry->pixels = pixels;
                entry->loading = false; entry->uploadedRows = 0;
                entry->compressedFormat = ktxFormat; entry->compressedBytes = ktxBytes;
                entry->needsPremultiply = needsPremultiply;
                const size_t bytes = ktxFormat ? ktxBytes : (size_t)width * (size_t)height * 4;
                pendingUploadBytes_ += bytes; totals_.decodedImages++;
                uploadQueue_.push_back(entry);
            });
        });
    }
    void eraseImage(const std::shared_ptr<ImageEntry>& entry) {
        if (!entry || entry->refs > entry->adoptRefs || !entry->ready) return;   // 硬引用在:绝不驱逐
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
                candidates.push_back(entry);
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b){ return a->lastUse < b->lastUse; });
        for (auto& entry : candidates) { if (gpuCacheBytes_ <= gpuCacheLimit_) break; eraseImage(entry); }
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
            size_t n = pair.second->data ? pair.second->data->size() : 0;
            byteCacheBytes_ = byteCacheBytes_ >= n ? byteCacheBytes_ - n : 0;
            totals_.evictedBytes += n; bytes_.erase(pair.first);
        }
    }

public:
    ResourceManager(cmd::Decoder* decoder = nullptr) : decoder_(decoder) {}
    void setImageDiagnostic(ImageDiagnostic callback) { imageDiagnostic_ = std::move(callback); }
    void configure(cmd::Decoder* decoder, std::function<Bytes(const std::string&)> loader,
                   Dispatch postWork, Dispatch postMain,
                   std::function<void()> cpuAcquire, std::function<void()> cpuRelease) {
        decoder_ = decoder; loader_ = std::move(loader);
        postWork_ = std::move(postWork); postMain_ = std::move(postMain);
        cpuAcquire_ = std::move(cpuAcquire); cpuRelease_ = std::move(cpuRelease);
    }
    void requestBytes(const std::string& url, BytesDone done) {
        const std::string key = url; ++frame_;
        auto found = bytes_.find(key);
        if (found != bytes_.end()) {
            auto entry = found->second; entry->lastUse = frame_;
            if (entry->loading) { totals_.byteInflightJoins++; entry->waiters.push_back(std::move(done)); }
            else { totals_.byteHits++; done(entry->data); }
            return;
        }
        totals_.byteMisses++;
        auto entry = std::make_shared<ByteEntry>(); entry->lastUse = frame_; entry->waiters.push_back(std::move(done));
        bytes_[key] = entry; const uint64_t requestEpoch = epoch_;
        auto copiedUrl = std::make_shared<std::string>(url);
        postWork_([this, key, entry, requestEpoch, copiedUrl] {
            Bytes data = loader_ ? loader_(*copiedUrl) : Bytes();
            postMain_([this, key, entry, requestEpoch, data] {
                if (requestEpoch != epoch_) return;
                auto current = bytes_.find(key);
                if (current == bytes_.end() || current->second.get() != entry.get()) return;
                entry->loading = false;
                if (data) { entry->data = data; byteCacheBytes_ += data->size(); }
                else bytes_.erase(key);   // 读失败不负缓存;回调前先擦,立即重试走 miss
                auto waiters = std::move(entry->waiters); for (auto& callback : waiters) if (callback) callback(data);
                collectBytes();
            });
        });
    }
    void requestImageURL(const std::string& url, ImageDone done, bool publishRef = false, bool sourcePremultiplied = false) {
        requestImage("url:" + url, Bytes(), url, std::move(done), publishRef, sourcePremultiplied);
    }
    void requestImageData(const Bytes& data, ImageDone done, bool publishRef = false, bool sourcePremultiplied = false) {
        char key[96]; snprintf(key, sizeof(key), "blob:%llu:%zu", (unsigned long long)hashData(data), data ? data->size() : 0);
        requestImage(key, data, std::string(), std::move(done), publishRef, sourcePremultiplied);
    }
    void requestImageDataNamed(const std::string& stableName, const Bytes& data, ImageDone done, bool publishRef = false) {
        if (stableName.empty()) { requestImageData(data, std::move(done), publishRef); return; }
        requestImage("named:" + stableName, data, std::string(), std::move(done), publishRef);
    }
    void beginFrame(size_t byteBudget = (4u << 20), int textureBudget = 2) {
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
                if (!entry->compressedFormat)
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, entry->width, entry->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            } else glBindTexture(GL_TEXTURE_2D, entry->gl);
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
    bool alive(uint32_t cid) const {
        auto it = imagesByCid_.find(cid); if (it == imagesByCid_.end()) return false;
        auto entry = it->second.lock();
        return entry && entry->ready && entry->gl != 0;
    }
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
        if (!NativeImageResidencyPolicy::hasHardReference(entry->refs, entry->adoptRefs)) {
            totals_.releaseMisses++; return false;
        }
        entry->refs--;
        if (entry->refs == 0) entry->protectedUntilRenderFrame = NativeImageResidencyPolicy::deadline(
            renderFrame_, NativeImageResidencyPolicy::kReleaseGraceFrames);
        entry->lastUse = ++frame_; collectImages(); return true;
    }
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
