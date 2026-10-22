// 命令解码器：读命令流，按 opcode 分发执行真实 GLES 调用。
// 维护 client_id -> GLuint 映射（createXxx 在此真正 glGen/glCreate）。
#pragma once
#include "protocol.h"
#include "batcher.h"   // conch P1:QuadRefBatch 段经 Batcher 流式合批
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
#include <OpenGLES/ES3/gl.h>   // iOS 原生 GLES3
#include <OpenGLES/ES3/glext.h>
#else
#include <GLES3/gl3.h>         // macOS：ANGLE 提供 VAO 等 WebGL2/GLES3 核心
#endif
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace cmd {

class Decoder {
    std::vector<GLuint> shaders_{0}, programs_{0}, buffers_{0}, textures_{0}, vaos_{0}, framebuffers_{0}, renderbuffers_{0};
    std::vector<GLint>  uniformLocs_{0};
    // Host-owned textures (native text/glyph atlases) use virtual client ids without growing the
    // dense WebGL id vector. Their GL lifetime is owned by the host subsystem that registered them.
    std::unordered_map<uint32_t, GLuint> externalTextures_;
    // Native Spine may borrow Pixi/KTX textures. A JS delete relinquishes Pixi's ownership;
    // keep the GL texture and pending uploads alive until the last native attachment releases it.
    struct TextureLease { size_t refs = 0; bool deletePending = false; };
    std::unordered_map<uint32_t, TextureLease> textureLeases_;

    void deleteTextureNow(uint32_t cid) {
        if (cid >= textures_.size() || !textures_[cid]) return;
        const GLuint texture = textures_[cid];
        purgePending(texture, -1);
        mipTex_.erase(texture);
        mipDirty_.erase(texture);
        glDeleteTextures(1, &texture);
        textures_[cid] = 0;
        for (auto &bound : boundTex_) if (bound == texture) bound = 0;
    }

    void deleteTexture(uint32_t cid) {
        auto it = textureLeases_.find(cid);
        if (it != textureLeases_.end()) it->second.deletePending = true;
        else deleteTextureNow(cid);
    }

    template <class V>
    static void put(V& v, uint32_t id, typename V::value_type val) {
        if (id >= v.size()) v.resize(id + 1, 0);
        v[id] = val;
    }
    static float asF(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

public:
    // 解码度量
    size_t lastCmdCount = 0;
    size_t drawCalls = 0;   // DrawArrays/DrawElements 累计（宿主每帧开始清零 → 读到每帧 draw call 数）
    size_t texUploads = 0;  // 真实 glTexImage2D 上传数（本帧,含即时 + beginFrame 补传;宿主每帧清零）
    size_t texDeferred = 0; // 本帧推迟入队数（节流命中,非真实上传;宿主每帧清零）
    // [tex-probe] 黑图仲裁:GAME_TEXPROBE=WxH 时记录最后一张该尺寸纹理的真实 GLuint,宿主定期 FBO 回读其像素。
    GLuint probeTex_ = 0; int probeW_ = 0, probeH_ = 0;
    void probeInit() { static bool done=false; if (done) return; done=true;
        const char* e = getenv("GAME_TEXPROBE"); if (e) sscanf(e, "%dx%d", &probeW_, &probeH_); }
    size_t bufUploads = 0;  // BufferData/BufferSubData 累计
    bool dbg = false;   // 逐命令打印 op+参数 与 GL error（排错用）
    // pixi 把"上屏"建模为绑定 null framebuffer；映射到平台默认 FBO（窗口=0，某些 ANGLE surface 非 0）。
    GLuint defaultFbo_ = 0;
    // iOS EAGL 下"默认帧缓冲"是绑定 CAEAGLLayer 的那个 FBO（非 0），启动时由宿主设置。macOS 仍为 0。
    void setDefaultFbo(GLuint f) { defaultFbo_ = f; }
    // conch P1 注入(宿主 setupGame 时设):QuadRefBatch 需要场景图与合批器
    sg::Batcher* batcher = nullptr;
    sg::SceneGraph* sgraph = nullptr;
    float screenW = 0, screenH = 0;
    // 超出的连数据推迟到后续帧(beginFrame 补传)。GLES 语义:level0 未定义的纹理采样为黑——
    // loading 遮罩期黑一帧无感,数据到即完整。mipmap 已由 JS 侧全局关闭(否则推迟后 GenerateMipmap 空跑)。
    struct PendingTex { GLuint glTex; GLenum target; GLint level, ifmt; GLsizei w, h; GLenum fmt, type; bool genMip = false; std::vector<uint8_t> data; };
    // [tex-mip] mipmap 链完整性:节流推迟破坏了浏览器的同步顺序(texImage2D→generateMipmap)。
    // GenerateMipmap 打在还没 level0 的纹理上=INVALID_OP 空跑(mip 缺失→采样(0,0,0,1) 纯黑);
    // 分块路径 null 分配后立即 GenerateMipmap 生成的是全零 mip(缩小采样=黑)。
    // 对策:①有 pending base 的 GenerateMipmap 打标随补传重放;②做过 mip 的纹理 level0 被改写 → 下帧重生成。
    std::unordered_set<GLuint> mipTex_;    // 历史上执行过 GenerateMipmap 的真实纹理
    std::unordered_set<GLuint> mipDirty_;  // mip 链已过期(level0 后改写),beginFrame 重生成
    std::deque<PendingTex> pendingTex_;                // deque:pop_front O(1)(vector.erase(begin()) 是 O(n) 搬移)
    size_t frameTexBytes_ = 0;                         // 当帧已上传字节
    int    frameTexCount_ = 0;                         // 当帧已上传张数(实测每张 TexImage 有 3-5ms 固定驱动开销,40 张小图=150-200ms——张数主导,字节预算单独失效)
    static const size_t TEX_FRAME_BUDGET = 12u << 20;  // 12MB/帧
    static const int    TEX_FRAME_MAXN   = 6;          // 6 张/帧(≥128KB 的)
    GLenum curUnit_ = 0;                               // ActiveTexture 跟踪(推迟需知道 TexImage 目标纹理 + 补传后恢复绑定)
    GLuint boundTex_[16] = {0};
    size_t texPending() const { return pendingTex_.size(); }   // 当前队列深度(live,宿主 badframe 读)
    void beginFrame() {                  // 宿主每帧 decode 前调:按预算(字节+张数双限)补传 pending 纹理
        frameTexBytes_ = 0; frameTexCount_ = 0;
        if (pendingTex_.empty() && mipDirty_.empty()) return;
        // 恢复目标用 decoder 自维护的绑定态,免 glGetIntegerv 的 GPU 同步往返(命令流下帧会重绑,但 pixi 纹理缓存
        // 可能跳过冗余绑定 → 必须恢复到 beginFrame 前的绑定,否则 pixi 用错纹理)。curUnit_/boundTex_ 由 decode 维护。
        GLuint save = (curUnit_ < 16) ? boundTex_[curUnit_] : 0;
        while (!pendingTex_.empty() && frameTexBytes_ < TEX_FRAME_BUDGET && frameTexCount_ < TEX_FRAME_MAXN) {
            PendingTex& pt = pendingTex_.front();
            glBindTexture(pt.target, pt.glTex);
            while (glGetError() != GL_NO_ERROR) {}   // 清残留错误,下面的检查才归因于本次上传
            glTexImage2D(pt.target, pt.level, pt.ifmt, pt.w, pt.h, 0, pt.fmt, pt.type, pt.data.data());
            if (pt.genMip) { glGenerateMipmap(pt.target); mipDirty_.erase(pt.glTex); }   // 重放浏览器同步顺序:上传后立刻建 mip 链
            else if (mipTex_.count(pt.glTex)) mipDirty_.insert(pt.glTex);                // 有 mip 历史但本条没带标 → 链过期,稍后重生成
            GLenum ge = glGetError();
            static int cuLogN = 0;   // 补传诊断:前 12 条 + 所有出错条目
            if (ge != GL_NO_ERROR || cuLogN < 12) { ++cuLogN;
                fprintf(stderr, "[tex-catchup] glTex=%u %dx%d %zuKB mip=%d err=0x%x queue=%zu\n",
                        (unsigned)pt.glTex, (int)pt.w, (int)pt.h, pt.data.size()>>10, (int)pt.genMip, (unsigned)ge, pendingTex_.size()-1); }
            frameTexBytes_ += pt.data.size(); frameTexCount_++; ++texUploads;   // 真实上传计数
            pendingTex_.pop_front();
        }
        if (!mipDirty_.empty()) {              // level0 在建链后被改写(分块 TexSubImage2D 带/重上传) → mip 链重生成
            for (GLuint t : mipDirty_) { glBindTexture(GL_TEXTURE_2D, t); glGenerateMipmap(GL_TEXTURE_2D); }
            static int rgLogN = 0;
            if (rgLogN < 8) { ++rgLogN; fprintf(stderr, "[tex-mip] 重生成过期 mip 链 n=%zu\n", mipDirty_.size()); }
            mipDirty_.clear();
        }
        glBindTexture(GL_TEXTURE_2D, save);            // 恢复(boundTex_ 未变,GL 与跟踪态一致)
    }

    // 同步查询桥用：client_id → 真实 GLuint（程序 init 期反射 active uniforms/attribs）。
    GLuint glProgram(uint32_t cid) const { return cid < programs_.size() ? programs_[cid] : 0; }
    GLuint glTexture(uint32_t cid) const {
        if (cid < textures_.size()) return textures_[cid];
        auto it = externalTextures_.find(cid); return it == externalTextures_.end() ? 0 : it->second;
    }
    void registerExternalTexture(uint32_t cid, GLuint tex) { if (cid && tex) externalTextures_[cid] = tex; }
    void unregisterExternalTexture(uint32_t cid) { externalTextures_.erase(cid); }
    // Only the dense command-buffer texture table is owned here. External textures retain
    // through their host ResourceManager, never through a numeric-id heuristic.
    bool retainTexture(uint32_t cid) {
        if (!cid || cid >= textures_.size() || !textures_[cid]) return false;
        auto &lease = textureLeases_[cid];
        if (lease.deletePending) return false;
        ++lease.refs;
        return true;
    }
    bool releaseTexture(uint32_t cid) {
        auto it = textureLeases_.find(cid);
        if (it == textureLeases_.end()) return false;
        if (--it->second.refs == 0) {
            const bool deletePending = it->second.deletePending;
            textureLeases_.erase(it);
            if (deletePending) deleteTextureNow(cid);
        }
        return true;
    }
    // batcher 采样前用:本帧真的要画的纹理若还在节流队列,画之前当场补传——否则该 quad 采样的是
    // 没有 level0 数据的纹理(GLES 未定义,实际黑/空)。大厅图标/按钮懒加载的 spine 图集(2048² RGBA
    // 单张 16MB,一张就超 12MB/帧预算 → 必定入队)在"静态图→spine"切换帧上就表现为一帧空白;
    // 浏览器 texImage2D 同步,永远看不到这帧。没被本帧绘制到的预加载纹理继续排队,
    GLuint glTextureEnsured(uint32_t cid) {
        GLuint t = glTexture(cid);
        if (t && !pendingTex_.empty()) flushPendingFor(t);
        // [tex-lookup] 绘制侧映射仲裁:绘制解析到的正是探针纹理 → client↔real 映射无病(没有此行=映射断裂)
        if (t && probeW_ && t == probeTex_) {
            static int lkN = 0;
            if (lkN < 6) { ++lkN; fprintf(stderr, "[tex-lookup] cid=%u -> glTex=%u (=probe目标,绘制侧映射正确)\n", cid, (unsigned)t); }
        }
        return t;
    }

    // 节流队列生命周期防护。pendingTex_ 记录的是真实 GLuint：
    // (a) DeleteTexture 后名字会被 glGenTextures 复用 → 队列里的旧图集数据会补传进新纹理（切场景后贴图张冠李戴/黑图）；
    // (b) 同一纹理"先入队后立即重传"→ beginFrame 晚于立即上传执行,旧数据反而最后生效。
    // 删除时清同名全部条目；立即上传前清同名同 level 条目。
    void purgePending(GLuint glTex, GLint level /* -1=全部 level */) {
        if (pendingTex_.empty() || !glTex) return;
        for (auto it = pendingTex_.begin(); it != pendingTex_.end();) {
            if (it->glTex == glTex && (level < 0 || it->level == level)) it = pendingTex_.erase(it);
            else ++it;
        }
    }
    // TexSubImage2D 打在还没真实分配(base TexImage2D 仍在队列)的纹理上是未定义行为：先把该纹理 pending 全部补传。
    void flushPendingFor(GLuint glTex) {
        if (pendingTex_.empty() || !glTex) return;
        GLuint save = (curUnit_ < 16) ? boundTex_[curUnit_] : 0;
        bool any = false;
        for (auto it = pendingTex_.begin(); it != pendingTex_.end();) {
            if (it->glTex == glTex) {
                glBindTexture(it->target, it->glTex);
                while (glGetError() != GL_NO_ERROR) {}
                glTexImage2D(it->target, it->level, it->ifmt, it->w, it->h, 0, it->fmt, it->type, it->data.data());
                if (it->genMip) { glGenerateMipmap(it->target); mipDirty_.erase(it->glTex); }
                else if (mipTex_.count(it->glTex)) mipDirty_.insert(it->glTex);
                GLenum ge = glGetError();
                static int enLogN = 0;   // 画前当场补传诊断:前 12 条 + 所有出错条目
                if (ge != GL_NO_ERROR || enLogN < 12) { ++enLogN;
                    fprintf(stderr, "[tex-ensure] glTex=%u %dx%d %zuKB mip=%d err=0x%x\n",
                            (unsigned)it->glTex, (int)it->w, (int)it->h, it->data.size()>>10, (int)it->genMip, (unsigned)ge); }
                ++texUploads; any = true;
                it = pendingTex_.erase(it);
            } else ++it;
        }
        if (any) glBindTexture(GL_TEXTURE_2D, save);
    }

    // surface 恢复时宿主会直接重置 active unit/texture binding，原来的跟踪态不再可信。
    // 下一帧 Pixi 命令流会重新建立绑定；这里清零可避免 pending 上传恢复到错误纹理。
    void invalidateBindings() {
        curUnit_ = 0;
        std::memset(boundTex_, 0, sizeof(boundTex_));
    }

    // 仅用于平台恢复诊断；不要放进逐帧路径（glIsTexture 会触发驱动查询）。
    void textureValidity(size_t& live, size_t& invalid) const {
        live = 0; invalid = 0;
        for (size_t i = 1; i < textures_.size(); ++i) {
            if (!textures_[i]) continue;
            ++live;
            if (!glIsTexture(textures_[i])) ++invalid;
        }
    }

    void execute(const uint32_t* p, size_t words) {
        size_t i = 0, n = 0;
        while (i < words) {
            uint32_t h = p[i];
            Op op = headerOp(h);
            uint32_t len = headerWords(h);
            const uint32_t* a = &p[i + 1]; // 参数起点
            switch (op) {
            case Op::CreateShader: put(shaders_, a[0], glCreateShader(a[1])); break;
            case Op::ShaderSource: {
                const char* src = (const char*)&a[2];
                GLint slen = (GLint)a[1];
                glShaderSource(shaders_[a[0]], 1, &src, &slen);
                break;
            }
            case Op::CompileShader: {
                GLuint s = shaders_[a[0]];
                glCompileShader(s);
                GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
                if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, nullptr, log); fprintf(stderr, "[dec] shader compile fail: %s\n", log); }
                break;
            }
            case Op::CreateProgram: put(programs_, a[0], glCreateProgram()); break;
            case Op::AttachShader: glAttachShader(programs_[a[0]], shaders_[a[1]]); break;
            case Op::BindAttribLocation: {
                // 流: programCid(a0), index(a1), byteLen(a2), name(a3..)
                std::string name((const char*)&a[3], a[2]);
                glBindAttribLocation(programs_[a[0]], a[1], name.c_str());
                break;
            }
            case Op::LinkProgram: {
                GLuint pr = programs_[a[0]];
                glLinkProgram(pr);
                GLint ok = 0; glGetProgramiv(pr, GL_LINK_STATUS, &ok);
                if (!ok) { char log[512]; glGetProgramInfoLog(pr, 512, nullptr, log); fprintf(stderr, "[dec] link fail: %s\n", log); }
                break;
            }
            case Op::UseProgram: glUseProgram(programs_[a[0]]); break;
            case Op::GetUniformLocation: {
                std::string name((const char*)&a[3], a[2]);
                put(uniformLocs_, a[0], glGetUniformLocation(programs_[a[1]], name.c_str()));
                break;
            }
            case Op::Uniform1i: glUniform1i(uniformLocs_[a[0]], (GLint)a[1]); break;
            case Op::CreateBuffer: { GLuint b = 0; glGenBuffers(1, &b); put(buffers_, a[0], b); break; }
            case Op::BindBuffer: glBindBuffer(a[0], buffers_[a[1]]); break;
            case Op::BufferData: ++bufUploads; glBufferData(a[0], (GLsizeiptr)a[2], &a[3], a[1]); break;
            case Op::CreateTexture: { GLuint t = 0; glGenTextures(1, &t); put(textures_, a[0], t); break; }
            case Op::BindTexture: {
                // Native ResourceManager textures deliberately use sparse virtual client ids
                // (9,000,000+).  They are registered in externalTextures_ and must never be
                // used as indexes into the dense WebGL texture vector.  Pixi's TextureSystem
                // still emits an ordinary bindTexture for the adopted GLTexture wrapper, so
                // resolve both dense and external ids through the same lookup here.
                GLuint texture = glTexture(a[1]);
                glBindTexture(a[0], texture);
                if (a[0] == GL_TEXTURE_2D && curUnit_ < 16) boundTex_[curUnit_] = texture;
                break;
            }
            case Op::TexImage2D: {
                const void* pix = a[7] ? (const void*)&a[8] : nullptr;
                // [tex-probe] 记录目标尺寸纹理的真实 GLuint(覆盖 即时/推迟/null分配 三条路径——记录点在分流之前)
                probeInit();
                if (probeW_ && (int)a[3] == probeW_ && (int)a[4] == probeH_ && curUnit_ < 16 && boundTex_[curUnit_]) {
                    if (probeTex_ != boundTex_[curUnit_])
                        fprintf(stderr, "[tex-probe] 锁定 glTex=%u %dx%d (%s)\n",
                                (unsigned)boundTex_[curUnit_], probeW_, probeH_, pix ? "带数据" : "null分配");
                    probeTex_ = boundTex_[curUnit_];
                }
                // 上传节流(字节预算):当帧超预算后,≥1MB 的纹理连数据推迟(见 beginFrame 补传);
                // RT 分配(pix=null)与小图(文本重绘)不受影响。GLES 未定义 level0 采样黑,loading 遮罩下无感。
                if (pix) {
                    size_t comps2 = (a[5]==GL_RGBA)?4 : (a[5]==GL_RGB)?3 : 2;
                    size_t tb2 = (a[6]==GL_FLOAT)?4 : (a[6]==GL_UNSIGNED_BYTE)?1 : 2;
                    size_t bytes2 = (size_t)a[3]*a[4]*comps2*tb2;
                    if (bytes2 >= (128u<<10)
                        && (frameTexCount_ >= TEX_FRAME_MAXN || frameTexBytes_ + bytes2 > TEX_FRAME_BUDGET)
                        && curUnit_ < 16 && boundTex_[curUnit_]) {
                        purgePending(boundTex_[curUnit_], (GLint)a[1]);   // 同纹理同 level 旧条目作废,只留最新
                        PendingTex pt; pt.glTex = boundTex_[curUnit_]; pt.target = a[0]; pt.level = (GLint)a[1];
                        pt.ifmt = (GLint)a[2]; pt.w = (GLsizei)a[3]; pt.h = (GLsizei)a[4]; pt.fmt = a[5]; pt.type = a[6];
                        pt.data.assign((const uint8_t*)pix, (const uint8_t*)pix + bytes2);
                        pendingTex_.push_back(std::move(pt));
                        ++texDeferred;   // 推迟入队,非真实上传
                        static int deferLogN = 0;   // 诊断:哪些纹理被节流(前 40 条;排查"某元素首帧黑/空"时看这里)
                        if (deferLogN < 40) { ++deferLogN;
                            fprintf(stderr, "[tex-defer] tex=%u %dx%d %zuKB queue=%zu\n",
                                    (unsigned)boundTex_[curUnit_], (int)a[3], (int)a[4], bytes2>>10, pendingTex_.size()); }
                        break;
                    }
                    frameTexBytes_ += bytes2;
                    if (bytes2 >= (128u<<10)) frameTexCount_++;
                }
                if (curUnit_ < 16 && boundTex_[curUnit_]) purgePending(boundTex_[curUnit_], (GLint)a[1]);   // 立即上传:队列中同名同 level 旧数据不得晚于本次生效
                ++texUploads;   // 真实上传(即时路径;beginFrame 补传路径单独 ++)
                std::vector<uint8_t> zero;
                if (!pix) {
                    // WebGL 规范：null 上传 = 零初始化(透明)；GLES 默认是未定义垃圾。
                    // pixi 的 RT 纹理池依赖此零初始化 → 显式补零，否则滤镜/RT 非渲染区出垃圾噪点。
                    int comps = (a[5]==GL_RGBA)?4 : (a[5]==GL_RGB)?3 : (a[5]==GL_LUMINANCE_ALPHA||a[5]==GL_RG)?2 : 1;
                    int tb = (a[6]==GL_FLOAT)?4 : (a[6]==GL_UNSIGNED_BYTE)?1 : 2;
                    zero.assign((size_t)a[3]*a[4]*comps*tb, 0); pix = zero.data();
                }
                bool bigTex = (size_t)a[3]*a[4] >= (1024u*1024u);   // ≥1M 像素的分配/上传检查 GL 错误(32MB 大图集分配失败会静默黑)
                if (bigTex) { while (glGetError() != GL_NO_ERROR) {} }
                glTexImage2D(a[0], a[1], a[2], a[3], a[4], 0, a[5], a[6], pix);
                if (a[1] == 0 && curUnit_ < 16 && boundTex_[curUnit_] && mipTex_.count(boundTex_[curUnit_]))
                    mipDirty_.insert(boundTex_[curUnit_]);   // level0 重上传 → 旧 mip 链过期
                if (bigTex) {
                    GLenum ge = glGetError();
                    static int imLogN = 0;
                    if (ge != GL_NO_ERROR || imLogN < 12) { ++imLogN;
                        fprintf(stderr, "[tex-im] glTex=%u %dx%d %s err=0x%x\n",
                                (unsigned)(curUnit_<16 ? boundTex_[curUnit_] : 0), (int)a[3], (int)a[4],
                                a[7] ? "带数据" : "null分配", (unsigned)ge); }
                }
                break;
            }
            case Op::TexParameteri: glTexParameteri(a[0], a[1], (GLint)a[2]); break;
            case Op::ActiveTexture: glActiveTexture(a[0]); curUnit_ = a[0] - GL_TEXTURE0; break;
            case Op::VertexAttribPointer:
                glVertexAttribPointer(a[0], a[1], a[2], a[3] ? GL_TRUE : GL_FALSE, a[4], (const void*)(intptr_t)a[5]);
                break;
            case Op::EnableVertexAttribArray: glEnableVertexAttribArray(a[0]); break;
            case Op::Viewport: glViewport((GLint)a[0], (GLint)a[1], (GLsizei)a[2], (GLsizei)a[3]); break;
            case Op::ClearColor: glClearColor(asF(a[0]), asF(a[1]), asF(a[2]), asF(a[3])); break;
            case Op::Clear: glClear(a[0]); break;
            case Op::Enable: glEnable(a[0]); break;
            case Op::Disable: glDisable(a[0]); break;
            case Op::DrawArrays: ++drawCalls; glDrawArrays(a[0], (GLint)a[1], (GLsizei)a[2]); break;
            // ---- S4: pixi v7 sprite 路径 ----
            case Op::BufferDataSize: {
                // WebGL 规范:size-only bufferData 必须清零;GLES 语义是"内容未定义"。Pixi 先按
                // 容量分配、后 bufferSubData 部分填充,draw 触及未写入区时垃圾顶点会画出随机
                // 退化三角/细线(DESIGN.md "stencil 遮罩间歇细灰线"的根因)。与 TexImage2D(null)
                // 的显式补零(下方 RT 路径)是同一规范要求的孪生情形。
                std::vector<uint8_t> z((size_t)a[1], 0);
                glBufferData(a[0], (GLsizeiptr)a[1], z.data(), a[2]);
                break;
            }
            case Op::BufferSubData: ++bufUploads; glBufferSubData(a[0], (GLintptr)a[1], (GLsizeiptr)a[2], &a[3]); break;
            case Op::DrawElements: ++drawCalls; glDrawElements(a[0], (GLsizei)a[1], a[2], (const void*)(intptr_t)a[3]); break;
            case Op::CreateVertexArray: { GLuint v = 0; glGenVertexArrays(1, &v); put(vaos_, a[0], v); break; }
            case Op::BindVertexArray: glBindVertexArray(a[0] ? vaos_[a[0]] : 0); break;
            case Op::CreateFramebuffer: { GLuint f = 0; glGenFramebuffers(1, &f); put(framebuffers_, a[0], f); break; }
            case Op::BindFramebuffer: glBindFramebuffer(a[0], a[1] ? framebuffers_[a[1]] : defaultFbo_); break;
            case Op::FramebufferTexture2D:
                // Keep the same sparse-id rule as BindTexture.  ResourceManager images are
                // normally sampled only, but accepting them here makes the decoder boundary
                // safe if Pixi temporarily attaches an adopted texture during a render pass.
                glFramebufferTexture2D(a[0], a[1], a[2], glTexture(a[3]), (GLint)a[4]);
                break;
            // ---- renderbuffer（depth/stencil 附件）----
            case Op::CreateRenderbuffer: { GLuint r = 0; glGenRenderbuffers(1, &r); put(renderbuffers_, a[0], r); break; }
            case Op::BindRenderbuffer: glBindRenderbuffer(a[0], a[1] ? renderbuffers_[a[1]] : 0); break;
            case Op::RenderbufferStorage: glRenderbufferStorage(a[0], a[1], (GLsizei)a[2], (GLsizei)a[3]); break;
            case Op::FramebufferRenderbuffer: glFramebufferRenderbuffer(a[0], a[1], a[2], a[3] ? renderbuffers_[a[3]] : 0); break;
            case Op::Uniformf: {
                GLint loc = uniformLocs_[a[0]]; uint32_t comps = a[1]; const GLfloat* f = (const GLfloat*)&a[2];
                if (comps==1) glUniform1f(loc,f[0]); else if (comps==2) glUniform2f(loc,f[0],f[1]);
                else if (comps==3) glUniform3f(loc,f[0],f[1],f[2]); else glUniform4f(loc,f[0],f[1],f[2],f[3]);
                break; }
            case Op::Uniformfv: {
                GLint loc = uniformLocs_[a[0]]; uint32_t comps = a[1], count = a[2]; const GLfloat* f = (const GLfloat*)&a[3];
                if (comps==1) glUniform1fv(loc,count,f); else if (comps==2) glUniform2fv(loc,count,f);
                else if (comps==3) glUniform3fv(loc,count,f); else glUniform4fv(loc,count,f);
                break; }
            case Op::DeleteShader: if (a[0] < shaders_.size() && shaders_[a[0]]) { glDeleteShader(shaders_[a[0]]); shaders_[a[0]] = 0; } break;
            case Op::GenerateMipmap: {
                // 浏览器语义:generateMipmap 紧跟 texImage2D 同步执行。base 还在节流队列时立即执行=空跑
                // (INVALID_OP,mip 缺失→缩小采样纯黑) → 打标随补传重放,顺序对齐浏览器。
                GLuint bt = (curUnit_ < 16) ? boundTex_[curUnit_] : 0;
                bool defer = false;
                if (bt) {
                    mipTex_.insert(bt);
                    for (auto& pt : pendingTex_)
                        if (pt.glTex == bt && pt.level == 0) { pt.genMip = true; defer = true; }
                }
                if (defer) {
                    static int dmLogN = 0;
                    if (dmLogN < 8) { ++dmLogN; fprintf(stderr, "[tex-mip] GenerateMipmap 随节流推迟 glTex=%u\n", (unsigned)bt); }
                } else glGenerateMipmap(a[0]);
                break;
            }
            case Op::Flush: glFlush(); break;
            case Op::PixelStorei: glPixelStorei(a[0], (GLint)a[1]); break;
            case Op::FrontFace: glFrontFace(a[0]); break;
            case Op::DepthMask: glDepthMask(a[0] ? GL_TRUE : GL_FALSE); break;
            case Op::BlendFunc: glBlendFunc(a[0], a[1]); break;
            case Op::BlendFuncSeparate: glBlendFuncSeparate(a[0], a[1], a[2], a[3]); break;
            case Op::BlendEquationSeparate: glBlendEquationSeparate(a[0], a[1]); break;
            case Op::Uniform4f: glUniform4f(uniformLocs_[a[0]], asF(a[1]), asF(a[2]), asF(a[3]), asF(a[4])); break;
            case Op::Uniform1iv: glUniform1iv(uniformLocs_[a[0]], (GLsizei)a[1], (const GLint*)&a[2]); break;
            case Op::UniformMatrix3fv: glUniformMatrix3fv(uniformLocs_[a[0]], (GLsizei)a[2], a[1] ? GL_TRUE : GL_FALSE, (const GLfloat*)&a[3]); break;
            case Op::UniformMatrix4fv: glUniformMatrix4fv(uniformLocs_[a[0]], (GLsizei)a[2], a[1] ? GL_TRUE : GL_FALSE, (const GLfloat*)&a[3]); break;
            // ---- S10: Mask ----
            case Op::Scissor: glScissor((GLint)a[0], (GLint)a[1], (GLsizei)a[2], (GLsizei)a[3]); break;
            case Op::StencilFunc: glStencilFunc(a[0], (GLint)a[1], a[2]); break;
            case Op::StencilOp: glStencilOp(a[0], a[1], a[2]); break;
            case Op::ColorMask: glColorMask(a[0]?GL_TRUE:GL_FALSE, a[1]?GL_TRUE:GL_FALSE, a[2]?GL_TRUE:GL_FALSE, a[3]?GL_TRUE:GL_FALSE); break;
            case Op::ClearStencil: glClearStencil((GLint)a[0]); break;
            // ---- 资源删除（释放 GL 句柄，槽位清 0；id 不复用故无别名问题）----
            case Op::DeleteBuffer: if (a[0] < buffers_.size() && buffers_[a[0]]) { glDeleteBuffers(1, &buffers_[a[0]]); buffers_[a[0]] = 0; } break;
            case Op::DeleteTexture: deleteTexture(a[0]); break;
            case Op::DeleteFramebuffer: if (a[0] < framebuffers_.size() && framebuffers_[a[0]]) { glDeleteFramebuffers(1, &framebuffers_[a[0]]); framebuffers_[a[0]] = 0; } break;
            case Op::DeleteVertexArray: if (a[0] < vaos_.size() && vaos_[a[0]]) { glDeleteVertexArrays(1, &vaos_[a[0]]); vaos_[a[0]] = 0; } break;
            case Op::DeleteProgram: if (a[0] < programs_.size() && programs_[a[0]]) { glDeleteProgram(programs_[a[0]]); programs_[a[0]] = 0; } break;
            case Op::DeleteRenderbuffer: if (a[0] < renderbuffers_.size() && renderbuffers_[a[0]]) { glDeleteRenderbuffers(1, &renderbuffers_[a[0]]); renderbuffers_[a[0]] = 0; } break;
            // conch P1:Quad 引用流段——从场景图 slot 组顶点合批(继承当前 scissor/stencil 状态)
            case Op::QuadRefBatch: {
                uint32_t qn = a[0];
                if (batcher && sgraph && batcher->ready() && qn > 0) {
                    batcher->escapes++;   // P1 下借用 escapes 统计"段数"(perf 行 escN;段多=逃逸打断多)
                    batcher->beginStream(sgraph, screenW, screenH);
                    for (uint32_t q = 0; q < qn; q++) batcher->emitByIdx((int)a[1 + q]);
                    batcher->endStream();
                    drawCalls += batcher->lastStreamDraws;
                }
                break;
            }
            // a: target,level,xoff,yoff,w,h,format,type, a[8]=payload长度词(0=无数据则跳过), a[9..]=像素字节
            case Op::TexSubImage2D: ++texUploads; if (a[8]) {
                if (curUnit_ < 16 && boundTex_[curUnit_]) flushPendingFor(boundTex_[curUnit_]);   // base 还在节流队列 → 先补传再打 sub
                size_t subBytes = (size_t)a[4]*a[5]*4;               // 近似(按 RGBA8);仅用于日志门槛
                bool bigSub = subBytes >= (1u<<20);                   // 分带大上传(chunked 路径)逐带检查 GL 错误
                if (bigSub) { while (glGetError() != GL_NO_ERROR) {} }
                glTexSubImage2D(a[0], a[1], (GLint)a[2], (GLint)a[3], (GLsizei)a[4], (GLsizei)a[5], a[6], a[7], (const void*)&a[9]);
                if (a[1] == 0 && curUnit_ < 16 && boundTex_[curUnit_] && mipTex_.count(boundTex_[curUnit_]))
                    mipDirty_.insert(boundTex_[curUnit_]);   // 分块带改写 level0 → mip 链过期,下帧重生成
                if (bigSub) {
                    GLenum ge = glGetError();
                    static int subLogN = 0;
                    if (ge != GL_NO_ERROR || subLogN < 16) { ++subLogN;
                        fprintf(stderr, "[tex-sub] glTex=%u off=%d,%d %dx%d %zuKB err=0x%x\n",
                                (unsigned)(curUnit_<16 ? boundTex_[curUnit_] : 0), (int)a[2], (int)a[3],
                                (int)a[4], (int)a[5], subBytes>>10, (unsigned)ge); }
                }
            } break;
            // a: target,level,internalFormat(ASTC),w,h, a[5]=payload字节数(imageSize), a[6..]=压缩数据。ASTC 图小,不节流,直接传。
            case Op::CompressedTexImage2D: ++texUploads; if (a[5]) {
                while (glGetError() != GL_NO_ERROR) {}   // 清掉之前的错误
                glCompressedTexImage2D(a[0], a[1], a[2], (GLsizei)a[3], (GLsizei)a[4], 0, (GLsizei)a[5], (const void*)&a[6]);
                GLenum ge = glGetError();
                if (ge != GL_NO_ERROR) fprintf(stderr, "[dec] compressedTex %ux%u fmt=0x%x size=%u GL_ERR=0x%x\n", (unsigned)a[3], (unsigned)a[4], (unsigned)a[2], (unsigned)a[5], (unsigned)ge);
            } break;
            default: break;
            }
            if (dbg) {
                GLenum e = glGetError();
                if (e != GL_NO_ERROR || true) {
                    fprintf(stderr, "[dec] op=%2u len=%u a=[%u %u %u %u]%s\n",
                            (unsigned)op, len, len>1?a[0]:0, len>2?a[1]:0, len>3?a[2]:0, len>4?a[3]:0,
                            e!=GL_NO_ERROR ? "  <<< GL ERROR" : "");
                }
            }
            i += len; ++n;
        }
        lastCmdCount = n;
    }
};

} // namespace cmd
