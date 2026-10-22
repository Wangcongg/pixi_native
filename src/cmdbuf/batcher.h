// conch P1:native sprite 合批渲染器。
// 按树序(paint order)DFS 场景图:TYPE=1 sprite 用 C++ 算好的 world 矩阵 × 局部 rect 出 quad,
// TYPE=3 多-quad 直接发 GLES;TYPE=2 为未覆盖节点,native-only flip 下跳过并计数。
// 未覆盖节点只计入 coverage 并跳过；主屏不存在回调 JS/Pixi 的渲染通道。
// JS 只写 slot 不读——无访问器读桥,这是 Laya conch 架构的关键。
#pragma once
#include "scenegraph.h"
#include "scissor_bounds.h"
#include "union_mask_branch.h"
#include <functional>
#include <vector>
#include <unordered_set>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cmath>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if TARGET_OS_IPHONE
#include <OpenGLES/ES3/gl.h>
#else
#include <GLES3/gl3.h>
#endif
#if defined(__ANDROID__)
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#endif

namespace sg {

class Batcher {
public:
    std::function<GLuint(int)> texLookup;   // client 纹理 id → GLuint(decoder 表)
    unsigned drawCalls = 0;                 // 每帧统计
    unsigned escapes   = 0;
    unsigned quads     = 0;
    unsigned escMask=0, escType=0, escTex=0;   // 逃逸原因细分:非矩形遮罩 / TYPE=2逃生舱 / 纹理未上传
    unsigned culled = 0;                 // viewport cull 跳过的子树根节点数
    double boundsMs=0, walkMs=0, bufferDataMs=0, bufferSubMs=0, drawMs=0;
    double syncMs=0;
    unsigned uploadCalls=0, mapFallbacks=0, immediateBatches=0, stencilBatches=0, postBatches=0, recordedBatches=0;
    int slowBlend=-1, slowPremul=-1, slowTwoColor=-1, slowMaskWrite=-1;
    // Allows pixel-conformance tests to compare the two programs on the same scene.
    bool specializeSprites = true;
#if defined(__ANDROID__)
    bool compactPostTargets = true;
    int postTargetBanks = 3;
#else
    bool compactPostTargets = false;
    int postTargetBanks = 1;
#endif
    // Explicit diagnostic counters; disabled during ordinary FPS measurements.
    bool profilePost = false;
    struct PostProfile {
        double totalMs=0, captureMs=0, prepareMs=0, drawMs=0, compositeMs=0;
        unsigned filters=0, passes=0, maxW=0, maxH=0;
    } postProfile;

    bool ready() const { return prog_ != 0; }

    // Read-only coverage check used before binding/clearing a RenderTexture target.  The native
    // scene graph already owns the authoritative parent/children structure, render types, masks
    // and texture ids, so asking JavaScript to walk the compatibility DisplayObject tree here is
    // both redundant and particularly expensive during popup/cache/filter creation.
    //
    // texLookup may lazily upload a decoded texture, but this function never changes the target
    // framebuffer or emits geometry. Returning false rejects the offscreen pass without partially
    // overwriting the destination RenderTexture; the JS host has no Pixi display-tree fallback.
    bool preflight(SceneGraph* g, int root) {
        if (!g || root <= 0 || root >= NODE_CAP || !g->nodes[root].alive) return false;
        SceneGraph* old = sg_;
        sg_ = g;
        bool ok = preflightNode(root, false);
        sg_ = old;
        return ok;
    }

    // Compile the post shader and allocate a reusable capacity while the loading scene is still
    // visible.  This moves the driver's one-time shader/FBO stall out of the first popup/machine
    // animation frame. Larger filters grow the capacity geometrically, never every-frame resize.
    // Three targets are prepared because nested filters must never sample from the texture they
    // are currently rendering into.  Additional levels are allocated lazily (up to the explicit
    // safety limit in renderPost), while ordinary single-filter scenes keep using target zero.
    bool preparePostProcesses(int capacity=512) {
        if(!ensurePostTarget(0,capacity,capacity)||!ensurePostTarget(1,capacity,capacity)||!ensurePostTarget(2,capacity,capacity))return false;
        // ANGLE/Metal can defer its real pipeline compilation until the first draw even after a
        // successful link. Warm that draw with color writes disabled so the cost stays on loading.
        GLint fbo=0,vp[4]={0},prog=0,arr=0,vao=0,active=0,tex=0,tex1=0;GLboolean color[4]={1,1,1,1};
        GLboolean blend=glIsEnabled(GL_BLEND),scissor=glIsEnabled(GL_SCISSOR_TEST),stencil=glIsEnabled(GL_STENCIL_TEST);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);glGetIntegerv(GL_VIEWPORT,vp);glGetIntegerv(GL_CURRENT_PROGRAM,&prog);glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&arr);glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex);glActiveTexture(GL_TEXTURE1);glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex1);glGetBooleanv(GL_COLOR_WRITEMASK,color);
        glBindVertexArray(0);glUseProgram(postProg_);glBindBuffer(GL_ARRAY_BUFFER,postVbo_);glEnableVertexAttribArray((GLuint)postPos_);glVertexAttribPointer((GLuint)postPos_,2,GL_FLOAT,GL_FALSE,16,(void*)0);glEnableVertexAttribArray((GLuint)postUV_);glVertexAttribPointer((GLuint)postUV_,2,GL_FLOAT,GL_FALSE,16,(void*)8);
        float warmWeights[8]={1,0,0,0,0,0,0,0};glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,postTargets_[0].tex);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,postTargets_[0].tex);glUniform1f(postRadius_,0);glUniform1f(postAngle_,0);glUniform2f(postOffset_,0,0);glUniform4f(postRect_,0,0,1,1);glUniform4f(postCoordRect_,0,0,1,1);glUniform2f(postTarget_,1,1);glUniform2f(postTexScale_,1,1);glUniform1f(postMode_,1);glUniform2f(postInvTex_,1,1);glUniform2f(postDispScale_,0,0);glUniform4f(postDispMapLinear_,1,0,0,1);glUniform2f(postDispMapTranslate_,0,0);glUniform4f(postDispRotation_,1,0,0,1);glUniform2f(postKawaseOffset_,0,0);glUniform1f(postKawaseClamp_,1);glUniform2f(postGaussianAxis_,0,0);glUniform1f(postGaussianRadius_,0);glUniform1fv(postGaussianWeights_,8,warmWeights);glUniform1f(postGaussianClamp_,1);glUniform2f(postShadowOffset_,0,0);glUniform3f(postShadowColor_,0,0,0);glUniform1f(postShadowAlpha_,1);
        glViewport(0,0,1,1);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glDisable(GL_STENCIL_TEST);glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glFinish();
        glColorMask(color[0],color[1],color[2],color[3]);if(blend)glEnable(GL_BLEND);else glDisable(GL_BLEND);if(scissor)glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);if(stencil)glEnable(GL_STENCIL_TEST);else glDisable(GL_STENCIL_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)fbo);glViewport(vp[0],vp[1],vp[2],vp[3]);glUseProgram((GLuint)prog);glBindVertexArray((GLuint)vao);glBindBuffer(GL_ARRAY_BUFFER,(GLuint)arr);glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,(GLuint)tex1);glActiveTexture((GLenum)active);glBindTexture(GL_TEXTURE_2D,(GLuint)tex);
        return true;
    }

    void init() {
        if (prog_) return;
        const char* vs =
            "attribute vec2 aPos; attribute vec2 aUV; attribute vec4 aColor;\n"
            "uniform vec4 uProj;\n"   // (2/w, -2/h, -1, 1) 屏幕像素 → clip,y 向下
            "varying highp vec2 vUV; varying vec4 vColor;\n"
            "void main(){ gl_Position = vec4(aPos.x*uProj.x+uProj.z, aPos.y*uProj.y+uProj.w, 0.0, 1.0);\n"
            "  vUV=aUV; vColor=aColor; }\n";
        const char* fs =
            "precision mediump float;\n"
            "varying highp vec2 vUV; varying vec4 vColor; uniform sampler2D uTex; uniform float uPremul; uniform float uTwoColor; uniform vec4 uDark; uniform float uDistanceField; uniform float uFWidth; uniform float uTiling; uniform highp vec2 uTileUV0; uniform highp vec2 uTileUV1; uniform highp vec2 uTileUV2; uniform highp vec2 uTileUV3; uniform float uCMOn; uniform mat4 uCM; uniform vec4 uCMOff; uniform float uOverlayOn; uniform vec3 uOverlayColor; uniform float uOverlayAlpha; uniform float uShadowOn; uniform vec3 uShadowColor; uniform float uShadowAlpha; uniform float uNoise; uniform float uNoiseSeed; uniform float uMaskWrite;\n"
            "float noiseRand(vec2 co){return fract(sin(dot(co,vec2(12.9898,78.233)))*43758.5453);}\n"
            "void main(){ highp vec2 sampleUV=vUV; if(uTiling>1.5){sampleUV=clamp(sampleUV,uTileUV0,uTileUV1);} else if(uTiling>0.5){highp vec2 p=fract(vUV);highp vec2 a=mix(uTileUV0,uTileUV1,p.x);highp vec2 b=mix(uTileUV3,uTileUV2,p.x);sampleUV=mix(a,b,p.y);} vec4 c = texture2D(uTex, sampleUV);\n"
            "  if (uMaskWrite > 1.5) { vec2 p=vUV*2.0-1.0; if (dot(p,p)>1.0) discard; }\n"
            "  else if (uMaskWrite > 0.5 && c.a <= 0.01) discard;\n"
            "  vec4 outc; if (uDistanceField > 0.5) {\n"
            "    float med=c.r+c.g+c.b-min(c.r,min(c.g,c.b))-max(c.r,max(c.g,c.b)); med=min(med,c.a);\n"
            "    float cov=clamp(uFWidth*(med-0.5)+0.5,0.0,1.0); if(med<0.01)cov=0.0;else if(med>0.99)cov=1.0;\n"
            "    float va=max(vColor.a,0.00001); float luma=dot(vColor.rgb/va,vec3(0.299,0.587,0.114)); float gamma=mix(1.0,1.0/2.2,luma); cov=pow(vColor.a*cov,gamma);\n"
            "    outc=vec4((vColor.rgb/va)*cov,cov);\n"
            "  } else {\n"
            "  c.rgb = mix(c.rgb, c.rgb*c.a, uPremul);\n"   // uPremul=1(非预乘纹理)→ shader 内预乘,修透明区白框
            "  if (uTwoColor > 0.5) {\n"                    // spine twoColor tint:vColor=直值 light,uDark=dark(premult tex 公式)
            "    outc.a = c.a * vColor.a;\n"
            // spine-ts PMA 语义:dark 贡献项须乘最终 alpha(darkColor.rgb *= finalAlpha),否则
            // tintBlack slot 淡出时 RGB 分量大于 alpha,暗部呈加色发亮/脏色。
            "    outc.rgb = ((c.a - 1.0)*uDark.a + 1.0 - c.rgb)*uDark.rgb*vColor.a + c.rgb*vColor.rgb;\n"
            "  } else { outc = c * vColor; } }\n"
            "  if (uShadowOn > 0.5) outc=vec4(uShadowColor*outc.a*uShadowAlpha,outc.a*uShadowAlpha); else {\n"
            "    if (uOverlayOn > 0.5) outc.rgb=mix(outc.rgb,uOverlayColor,outc.a*uOverlayAlpha);\n"
            "    if (uCMOn > 0.5) { vec4 straight=outc; if(straight.a>0.0) straight.rgb/=straight.a; outc=uCM*straight+uCMOff; outc.rgb*=outc.a; }\n"
            "    if (uNoise > 0.0 && outc.a > 0.0) { float d=(noiseRand(gl_FragCoord.xy*uNoiseSeed)-0.5)*uNoise; outc.rgb=clamp(outc.rgb/outc.a+d,0.0,1.0)*outc.a; } }\n"
            "  gl_FragColor=outc; }\n";
        prog_ = link(vs, fs);
        uProj_ = glGetUniformLocation(prog_, "uProj");
        uPremul_ = glGetUniformLocation(prog_, "uPremul");
        uTwoColor_ = glGetUniformLocation(prog_, "uTwoColor");
        uDark_ = glGetUniformLocation(prog_, "uDark");
        uDistanceField_ = glGetUniformLocation(prog_, "uDistanceField");
        uFWidth_ = glGetUniformLocation(prog_, "uFWidth");
        uTiling_ = glGetUniformLocation(prog_, "uTiling");
        uTileUV_[0] = glGetUniformLocation(prog_, "uTileUV0");uTileUV_[1] = glGetUniformLocation(prog_, "uTileUV1");uTileUV_[2] = glGetUniformLocation(prog_, "uTileUV2");uTileUV_[3] = glGetUniformLocation(prog_, "uTileUV3");
        uCMOn_ = glGetUniformLocation(prog_, "uCMOn");
        uCM_ = glGetUniformLocation(prog_, "uCM");
        uCMOff_ = glGetUniformLocation(prog_, "uCMOff");
        uOverlayOn_ = glGetUniformLocation(prog_, "uOverlayOn");uOverlayColor_ = glGetUniformLocation(prog_, "uOverlayColor");uOverlayAlpha_ = glGetUniformLocation(prog_, "uOverlayAlpha");
        uShadowOn_ = glGetUniformLocation(prog_, "uShadowOn");uShadowColor_ = glGetUniformLocation(prog_, "uShadowColor");uShadowAlpha_ = glGetUniformLocation(prog_, "uShadowAlpha");
        uNoise_ = glGetUniformLocation(prog_, "uNoise");uNoiseSeed_ = glGetUniformLocation(prog_, "uNoiseSeed");
        uMaskWrite_ = glGetUniformLocation(prog_, "uMaskWrite");
        GLint uTex = glGetUniformLocation(prog_, "uTex");
        aPos_ = glGetAttribLocation(prog_, "aPos");
        aUV_ = glGetAttribLocation(prog_, "aUV");
        aColor_ = glGetAttribLocation(prog_, "aColor");
        // Ordinary sprites and Spine tinting need neither discard nor the filter/MSDF/tiling
        // branches. Keep their arithmetic identical, but let mobile drivers compile a smaller
        // program. Selection is by the complete recorded state, never by device/quality level.
        const char* spriteFs =
            "precision mediump float;\n"
            "varying highp vec2 vUV; varying vec4 vColor; uniform sampler2D uTex;\n"
            "uniform float uPremul; uniform float uTwoColor; uniform vec4 uDark;\n"
            "void main(){ vec4 c=texture2D(uTex,vUV); c.rgb=mix(c.rgb,c.rgb*c.a,uPremul);\n"
            "  vec4 outc; if(uTwoColor>0.5){ outc.a=c.a*vColor.a;\n"
            "    outc.rgb=((c.a-1.0)*uDark.a+1.0-c.rgb)*uDark.rgb*vColor.a+c.rgb*vColor.rgb;\n"
            "  } else { outc=c*vColor; } gl_FragColor=outc; }\n";
        spriteProg_=link(vs,spriteFs,aPos_,aUV_,aColor_);
        if(!spriteProg_){std::fprintf(stderr,"[batcher] required sprite program failed to link\n");std::abort();}
        spriteProj_=glGetUniformLocation(spriteProg_,"uProj");
        spritePremul_=glGetUniformLocation(spriteProg_,"uPremul");
        spriteTwoColor_=glGetUniformLocation(spriteProg_,"uTwoColor");
        spriteDark_=glGetUniformLocation(spriteProg_,"uDark");
        glUseProgram(spriteProg_);glUniform1i(glGetUniformLocation(spriteProg_,"uTex"),0);
        glUseProgram(prog_);
        glUniform1i(uTex, 0);
        glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
#if defined(__ANDROID__)
        const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        auto bufferStorage = reinterpret_cast<PFNGLBUFFERSTORAGEEXTPROC>(eglGetProcAddress("glBufferStorageEXT"));
        if (extensions && strstr(extensions, "GL_EXT_buffer_storage") && bufferStorage) {
            const GLsizeiptr size = PERSISTENT_REGIONS*MAX_QUADS*4*STRIDE_B;
            const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
            bufferStorage(GL_ARRAY_BUFFER, size, nullptr, flags);
            if (glGetError() == GL_NO_ERROR) {
                persistentVbo_ = glMapBufferRange(GL_ARRAY_BUFFER, 0, size, flags);
                if (!persistentVbo_) glGetError();
            }
            if (!persistentVbo_) {
                glDeleteBuffers(1, &vbo_); glGenBuffers(1, &vbo_); glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            }
        }
#endif
        if (!persistentVbo_) glBufferData(GL_ARRAY_BUFFER, MAX_QUADS*4*STRIDE_B, nullptr, GL_STREAM_DRAW);
        glGenBuffers(1, &ibo_);
        std::vector<GLushort> idx(MAX_QUADS*6);
        for (int q=0;q<MAX_QUADS;q++) {
            GLushort v = (GLushort)(q*4);
            idx[q*6+0]=v; idx[q*6+1]=v+1; idx[q*6+2]=v+2; idx[q*6+3]=v; idx[q*6+4]=v+2; idx[q*6+5]=v+3;
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size()*2), idx.data(), GL_STATIC_DRAW);
        verts_.reserve(MAX_QUADS*4*STRIDE_F);   // 录制模式帧级累积上限=环容量(回绕点强制回放),原 reserve 已是峰值
        recorded_.reserve(256);
        fprintf(stderr, "[batcher] init prog=%u aPos=%d aUV=%d aColor=%d persistent=%d\n", prog_, aPos_, aUV_, aColor_, persistentVbo_!=nullptr);
    }

    // 一帧:宿主已 bind 默认 FBO + viewport + clear。
    void frame(SceneGraph* g, int root, float w, float h, const float* outputTransform=nullptr, float outputAlphaScale=1.0f) {
        sg_ = g; drawCalls=0; escapes=0; quads=0; escMask=0; escType=0; escTex=0; culled=0;
        boundsMs=walkMs=bufferDataMs=bufferSubMs=drawMs=0;
        uploadCalls=mapFallbacks=immediateBatches=stencilBatches=postBatches=recordedBatches=0;
        slowBlend=slowPremul=slowTwoColor=slowMaskWrite=-1;
        // JS 在 present 前已写完纹理/quad；同帧生成 world/subtree bounds，供下一次输入和 getBounds O(1) 读取。
        auto bt0=std::chrono::steady_clock::now();
        g->updateBounds(root);
        boundsMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-bt0).count();
        screenH_ = h; targetLogicalW_=w; targetLogicalH_=h; scissors_.clear(); stencilDepth_=0;maskGeomStack_.clear();postDepth_=0;twistSkipNode_=-1;
        glGetIntegerv(GL_VIEWPORT, targetViewport_);
        outputTransformOn_=outputTransform!=nullptr;outputAlphaScale_=outputAlphaScale;
        if(outputTransform)memcpy(outputTransform_,outputTransform,sizeof(outputTransform_));
        else{outputTransform_[0]=outputTransform_[3]=1.0f;outputTransform_[1]=outputTransform_[2]=outputTransform_[4]=outputTransform_[5]=0.0f;}
        // Viewport culling margin: keep slightly off-screen nodes so soft edges / blur padding
        // still rasterise. Set GAME_NO_CULL=1 to disable for A/B.
        cullEnabled_ = getenv("GAME_NO_CULL") == nullptr;
        cullPad_ = 64.0f;
        cullX0_ = -cullPad_; cullY0_ = -cullPad_;
        cullX1_ = w + cullPad_; cullY1_ = h + cullPad_;
        proj_[0]=2.0f/w; proj_[1]=-2.0f/h; proj_[2]=-1.0f; proj_[3]=1.0f;
        setupState();
        // 批录制(对标 Cocos Batcher2d 录制 + uploadBuffers + UIPhase 回放):全屏 frame 的普通
        // quad 流延迟到帧末一次 map 上传 + 逐批回放;mask/post 等需要立即出图的区段自动降级即时。
        // 上帧异常残留(已录未回放)整体丢弃,防止 verts_ 与槽位错位。
        if (!recorded_.empty() || verts_.size() != (size_t)quadCount_*4*STRIDE_F) { recorded_.clear(); verts_.clear(); quadCount_ = 0; vertsResetSeq_++; }
        frameRecording_ = recordFlagEnabled();
        recBaseSlot_ = streamBase_;
        auto wt0=std::chrono::steady_clock::now();
        walk(root);
        replayPending();   // 帧末统一上传+回放(即时模式下等价于旧 flush)
        frameRecording_ = false;
        walkMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wt0).count();
        if (vcStatsOn() && ++vcFrames_ % 300 == 0 && (vcHit_ + vcMiss_ + vcSkip_)) {
            unsigned long tot = vcHit_ + vcMiss_;
            fprintf(stderr, "[vert-cache] 300帧 hit=%lu miss=%lu skip=%lu 复用quad=%lu 命中率=%.1f%% 对拍一致=%lu 不一致=%lu\n",
                    vcHit_, vcMiss_, vcSkip_, vcHitQuads_, tot ? 100.0 * (double)vcHit_ / (double)tot : 0.0, vcVerify_, vcDiff_);
            vcHit_ = vcMiss_ = vcSkip_ = vcHitQuads_ = vcVerify_ = vcDiff_ = 0;
        }
        glDisable(GL_SCISSOR_TEST);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    // ---- conch P1 流式模式:JS 主导遍历,decoder 按 QuadRefBatch 段喂节点号。与 pixi 命令流交错,
    //      必须继承当前 scissor/stencil 状态(段处于 pixi mask push/pop 之间时,quad 要受同样裁剪),
    //      并解绑 VAO(否则 attrib pointer 会改写 pixi 当前 VAO 的记录状态)。----
    unsigned lastStreamDraws = 0;
    // 顶点 VBO 环形写入:每段续写不同偏移 + 每帧首段 orphan 换存储。
    // 覆盖写同一偏移会迫使驱动等待 GPU 消费完上一段(隐式同步 stall,实测把解码从 3ms 顶到 12ms+)。
    int streamBase_ = 0;   // 本段顶点槽起点(quad 单位)
    int framePos_ = 0;     // 帧内已写 quad 槽
    bool orphan_ = true;
    void frameReset() {
        postRootSequence_=0;
        syncMs=0;
        postProfile=PostProfile();
        if (persistentVbo_) {
            auto t=std::chrono::steady_clock::now();
            advancePersistentRegion();
            syncMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
        }
        framePos_ = 0; streamBase_ = 0; orphan_ = true;
    }   // 宿主每帧调
    void beginStream(SceneGraph* g, float w, float h) {
        sg_ = g; lastStreamDraws = 0;
        frameRecording_ = false;   // stream 段与 pixi 命令流交错,endStream 返回前必须画完:恒即时
        outputTransformOn_=false;outputAlphaScale_=1.0f;
        activeFilterAlpha_=1.0f;activeBlurX_=activeBlurY_=0.0f;activeCMOn_=false;activeOverlayOn_=false;activeShadowOn_=false;activeNoise_=0.0f;activeNoiseSeed_=0.0f;
        streamBase_ = framePos_; quadCount_ = 0; verts_.clear(); vertsResetSeq_++;
        proj_[0]=2.0f/w; proj_[1]=-2.0f/h; proj_[2]=-1.0f; proj_[3]=1.0f;
        glBindVertexArray(0);                    // 关键:防污染 pixi 当前 VAO
        glUseProgram(prog_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
        glEnableVertexAttribArray((GLuint)aPos_);
        glVertexAttribPointer((GLuint)aPos_, 2, GL_FLOAT, GL_FALSE, STRIDE_B, (void*)(intptr_t)persistentRegionBase());
        glEnableVertexAttribArray((GLuint)aUV_);
        glVertexAttribPointer((GLuint)aUV_, 2, GL_FLOAT, GL_FALSE, STRIDE_B, (void*)(intptr_t)(persistentRegionBase()+8));
        glEnableVertexAttribArray((GLuint)aColor_);
        glVertexAttribPointer((GLuint)aColor_, 4, GL_UNSIGNED_BYTE, GL_TRUE, STRIDE_B, (void*)(intptr_t)(persistentRegionBase()+16));
        glUniform4fv(uProj_, 1, proj_);
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_BLEND);
        // 不动 scissor/stencil/depth:继承 pixi 命令流当前状态(mask 对本段 quad 生效)
        curBlend_ = -1; curTex_ = 0; curTexId_ = 0; curPremul_ = -1; curTwoColor_ = -1; curDistanceField_=-1;curTiling_=-1;curCMOn_=true;curOverlayOn_=true;curShadowOn_=true;curNoise_=-1.0f;curNoiseSeed_=-1.0f; setBlend(0); setPremul(0); setTwoColor(0);setDistanceField(0,0);setTiling(nullptr); setColorMatrix(false,nullptr);setOverlay(false,nullptr);setShadow(false,nullptr);setNoise(0.0f,0.0f);
    }
    void emitByIdx(int idx) {
        if (idx <= 0 || idx >= NODE_CAP || !sg_ || !sg_->nodes[idx].alive) return;
        float* s = sg_->buf + idx*STRIDE;
        if (s[WALPHA] <= 0.0f) return;           // JS 侧已判 visible/renderable/TEXID,这里兜底
        GLuint gl = ((int)s[TEXID]) > 0 && texLookup ? texLookup((int)s[TEXID]) : 0;
        if (!gl) return;                          // 纹理竞态窗口:本帧跳过(下帧 JS 重判)
        emitQuad(s, gl);
    }
    void endStream() {
        unsigned d0 = drawCalls;
        flush();
        lastStreamDraws = drawCalls - d0;
        // 不恢复 GL 状态:pixi 后续命令流显式设置一切;JS 侧在段后 renderer.reset() 清其状态缓存
    }

    // 建立 native batcher 所需的完整 GL 状态。
    void setupState() {
        glUseProgram(prog_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
        glEnableVertexAttribArray((GLuint)aPos_);
        glVertexAttribPointer((GLuint)aPos_, 2, GL_FLOAT, GL_FALSE, STRIDE_B, (void*)(intptr_t)persistentRegionBase());
        glEnableVertexAttribArray((GLuint)aUV_);
        glVertexAttribPointer((GLuint)aUV_, 2, GL_FLOAT, GL_FALSE, STRIDE_B, (void*)(intptr_t)(persistentRegionBase()+8));
        glEnableVertexAttribArray((GLuint)aColor_);
        glVertexAttribPointer((GLuint)aColor_, 4, GL_UNSIGNED_BYTE, GL_TRUE, STRIDE_B, (void*)(intptr_t)(persistentRegionBase()+16));
        glUniform4fv(uProj_, 1, proj_);
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        applyScissor();   // 恢复当前 mask 裁剪栈(逃生舱里 pixi 会重置 scissor)
        curBlend_ = -1;   // 强制重设
        curTex_ = 0;
        curTexId_ = 0;
        curPremul_ = -1;
        curTwoColor_ = -1;
        curDistanceField_ = -1;
        curTiling_ = -1;
        curMaskWrite_ = -1;
        activeFilterAlpha_=1.0f;activeBlurX_=activeBlurY_=0.0f;activeCMOn_=false;activeOverlayOn_=false;activeShadowOn_=false;activeNoise_=0.0f;activeNoiseSeed_=0.0f;curCMOn_=true;curOverlayOn_=true;curShadowOn_=true;curNoise_=-1.0f;curNoiseSeed_=-1.0f;setColorMatrix(false,nullptr);setOverlay(false,nullptr);setShadow(false,nullptr);setNoise(0.0f,0.0f);
        setBlend(0);
        setPremul(0);
        setTwoColor(0);
        setDistanceField(0,0);
        setTiling(nullptr);
        setMaskWrite(0);
    }

private:
    double postClock() const {
        return profilePost ? std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now().time_since_epoch()).count() : 0;
    }
    static const int MAX_QUADS = 8192;
    static const int STRIDE_F = 5;              // x,y,u,v,rgba(packed)
    static const int STRIDE_B = STRIDE_F*4;
    SceneGraph* sg_ = nullptr;
    GLuint prog_=0, vbo_=0, ibo_=0;
    GLuint spriteProg_=0;
    GLint spriteProj_=-1,spritePremul_=-1,spriteTwoColor_=-1,spriteDark_=-1;
    static const int PERSISTENT_REGIONS=3;
    void* persistentVbo_=nullptr;
    bool persistentUsed_=false;
    int persistentRegion_=0;
    GLsync persistentFences_[PERSISTENT_REGIONS]={nullptr,nullptr,nullptr};

    size_t persistentRegionBase() const {
        return persistentVbo_ ? (size_t)persistentRegion_*MAX_QUADS*4*STRIDE_B : 0;
    }
    void advancePersistentRegion() {
        if (persistentUsed_) {
            persistentFences_[persistentRegion_] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (!persistentFences_[persistentRegion_]) glFinish();
            else glFlush();
            persistentUsed_ = false;
        }
        persistentRegion_ = (persistentRegion_+1)%PERSISTENT_REGIONS;
        GLsync& fence = persistentFences_[persistentRegion_];
        if (fence) {
            for (;;) {
                GLenum status = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
                if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) break;
                if (status == GL_WAIT_FAILED) { glFinish(); break; }
            }
            glDeleteSync(fence); fence = nullptr;
        }
    }
    void pointAtPersistentRegion() {
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glVertexAttribPointer((GLuint)aPos_, 2, GL_FLOAT, GL_FALSE, STRIDE_B, (void*)(intptr_t)persistentRegionBase());
        glVertexAttribPointer((GLuint)aUV_, 2, GL_FLOAT, GL_FALSE, STRIDE_B, (void*)(intptr_t)(persistentRegionBase()+8));
        glVertexAttribPointer((GLuint)aColor_, 4, GL_UNSIGNED_BYTE, GL_TRUE, STRIDE_B, (void*)(intptr_t)(persistentRegionBase()+16));
    }
    GLint uProj_=-1, uPremul_=-1, uTwoColor_=-1, uDark_=-1, uDistanceField_=-1, uFWidth_=-1, uTiling_=-1, uTileUV_[4]={-1,-1,-1,-1}, uCMOn_=-1, uCM_=-1, uCMOff_=-1, uOverlayOn_=-1, uOverlayColor_=-1, uOverlayAlpha_=-1, uShadowOn_=-1, uShadowColor_=-1, uShadowAlpha_=-1, uNoise_=-1, uNoiseSeed_=-1, uMaskWrite_=-1, aPos_=-1, aUV_=-1, aColor_=-1;
    int curPremul_=-1;   // 当前批的预乘模式(切换时 flush + 重设 uPremul)
    int curDistanceField_=-1; float curFWidth_=-1.0f;
    int curTiling_=-1; float curTileUV_[8]={0};
    int curMaskWrite_=-1;
    int curTwoColor_=-1; uint32_t curDark_=0xFFFFFFFF; int curDarkA_=-1; float curDarkAF_=-1.0f;   // twoColor 状态(切换时 flush);AF=精确浮点影子供录制快照
    float activeFilterAlpha_=1.0f, activeBlurX_=0.0f, activeBlurY_=0.0f, activeCM_[20]={0}, curCM_[20]={0};
    bool activeCMOn_=false, curCMOn_=false;
    bool activeOverlayOn_=false,curOverlayOn_=false;float activeOverlay_[4]={0},curOverlay_[4]={0};
    bool activeShadowOn_=false,curShadowOn_=false;float activeShadow_[6]={0},curShadow_[4]={0};
    float activeNoise_=0.0f,activeNoiseSeed_=0.0f,curNoise_=-1.0f,curNoiseSeed_=-1.0f;
    float proj_[4] = {0,0,0,0};
    bool outputTransformOn_=false;
    float outputTransform_[6]={1,0,0,1,0,0};
    float outputAlphaScale_=1.0f;
    float targetLogicalW_=0.0f,targetLogicalH_=0.0f;
    GLint targetViewport_[4]={0,0,0,0};  // Buffer pixels, independently of layout coordinates.
    std::vector<float> verts_;
    int quadCount_ = 0;
    GLuint curTex_ = 0;
    int curTexId_ = 0;   // curTex_ 的 client 纹理 id 影子(顶点缓存段存 id 而非 GLuint:纹理重建后重解析,防悬挂)
    int curBlend_ = -1;
    struct SRect { float x0,y0,x1,y1; };
    struct PostTarget { GLuint fbo=0,tex=0,stencil=0; int w=0,h=0; uint64_t used=0; };
    GLuint postProg_=0,postVbo_=0;
    std::vector<PostTarget> postTargets_;
    std::vector<PostTarget> postLegacyTargets_;
    std::vector<std::vector<PostTarget>> postSizedTargets_;
    uint64_t postTargetUse_=0;
    GLint postPos_=-1,postUV_=-1,postRadius_=-1,postAngle_=-1,postOffset_=-1,postRect_=-1,postCoordRect_=-1,postTarget_=-1,postTexScale_=-1,postMode_=-1,postInvTex_=-1,postDispScale_=-1,postDispMapLinear_=-1,postDispMapTranslate_=-1,postDispRotation_=-1,postKawaseOffset_=-1,postKawaseClamp_=-1,postGaussianAxis_=-1,postGaussianRadius_=-1,postGaussianWeights_=-1,postGaussianClamp_=-1,postShadowOffset_=-1,postShadowColor_=-1,postShadowAlpha_=-1;
    unsigned postRootSequence_=0;int postRootBank_=0;
    int postDepth_=0,twistSkipNode_=-1;bool postLogged_=false,nestedPostLogged_=false;

    void mapOutput(float& x,float& y) const {
        if(!outputTransformOn_)return;
        float ox=outputTransform_[0]*x+outputTransform_[2]*y+outputTransform_[4];
        y=outputTransform_[1]*x+outputTransform_[3]*y+outputTransform_[5];x=ox;
    }

    static void composeCM(const float* outer,const float* inner,float* out){
        for(int r=0;r<4;r++){int ro=r*5;for(int c=0;c<4;c++){float v=0;for(int k=0;k<4;k++)v+=outer[ro+k]*inner[k*5+c];out[ro+c]=v;}
            float v=outer[ro+4];for(int k=0;k<4;k++)v+=outer[ro+k]*inner[k*5+4];out[ro+4]=v;}
    }

    bool preflightTexture(int texId) const {
        return texId > 0 && texLookup && texLookup(texId) != 0;
    }

    // HAS_FILTER 位开关:SG_FILTER_FLAG=0 退回"每节点无条件读 FILTER 区"旧行为(A/B 对拍用)。
    static bool filterFlagEnabled() {
        static const bool on = [](){ const char* e = getenv("SG_FILTER_FLAG"); return !(e && e[0]=='0'); }();
        return on;
    }

    bool preflightMaskGeometry(int idx, bool geometryMask) const {
        if (idx <= 0 || idx >= NODE_CAP || !sg_->nodes[idx].alive) return false;
        const float* m = sg_->buf + idx * STRIDE;
        if (!geometryMask) return preflightTexture((int)m[TEXID]);
        const auto& native = sg_->nodes[idx].nativeQuads;
        if (!native.empty()) {
            for (const auto& q : native) if (preflightTexture((int)q[0])) return true;
            return false;
        }
        int off = (int)m[QUADOFF], cnt = (int)m[QUADCNT];
        if (off < 0 || cnt <= 0 || off + cnt > QUAD_CAP) return false;
        const float* qb = sg_->buf + QUAD_BASE + 1;
        for (int q = 0; q < cnt; ++q) {
            const float* mq = qb + (size_t)(off + q) * QUAD_STRIDE_F;
            if (mq[0] >= 0.0f && preflightTexture((int)mq[0])) return true;
        }
        return false;
    }

    // Axis-aligned rectangle masks stay on the cheap scissor path. Anything rotated, skewed,
    // or carrying explicit geometry/alpha/ellipse flags must go through stencil instead of
    // escaping as "non-rect".
    static bool preflightRectMask(const float* m) {
        return fabsf(m[WB]) < 0.01f && fabsf(m[WC]) < 0.01f && m[RW] >= 0.0f && m[RH] >= 0.0f;
    }
    bool hasMaskGeometry(int idx, const float* m) const {
        if (idx > 0 && idx < NODE_CAP && !sg_->nodes[idx].nativeQuads.empty()) return true;
        int cnt = (int)m[QUADCNT];
        return cnt > 0 && (int)m[QUADOFF] >= 0;
    }
    bool isGeometryMask(int idx, const float* m, int flags) const {
        if ((flags & 64) != 0) return true;
        if ((flags & (8 | 16)) != 0) return false;   // alpha / ellipse sample the mask texture
        return !preflightRectMask(m) && hasMaskGeometry(idx, m);
    }

    bool preflightNode(int idx, bool fromSpineMarker) const {
        if (idx <= 0 || idx >= NODE_CAP || !sg_->nodes[idx].alive) return false;
        const float* s = sg_->buf + idx * STRIDE;
        // Mask display objects are geometry inputs for their owner's scissor/stencil pass, never
        // ordinary paint nodes.  Keep this invariant here as well as in RENDERABLE: a late Pixi
        // facade refresh can legitimately rebuild the Graphics payload and transiently publish
        // renderable=1, but must not turn the solid white mask shape into visible content.
        if (s[VISIBLE] == 0.0f || s[WALPHA] <= 0.0f || s[RENDERABLE] == 0.0f ||
            (((int)s[HITFLAGS]) & 4) != 0) return true;
        int type = (int)s[TYPE];
        if (type == T_SPINE_SLOT && !fromSpineMarker) return true;
        if (type == T_ESCAPE) return false;
        if (!filterFlagEnabled() || sg_->buf[FILTER_FLAG_BASE + idx] != 0.0f) {   // HAS_FILTER 位 0 → 无 filter 资源可查
            const float* fd=sg_->buf+FILTER_BASE+(size_t)idx*FILTER_STRIDE_F;
            if(fd[103]>=2.0f){int count=std::max(0,std::min(POST_FILTER_DESC_CAP,(int)fd[96]));for(int i=0;i<count;i++){const float* pd=fd+POST_FILTER_DESC_BASE+i*POST_FILTER_DESC_STRIDE_F;if((int)pd[0]==9&&!preflightTexture((int)pd[1]))return false;}}
            else if(fd[47]>0.5f&&!preflightTexture((int)fd[48]))return false;
        }

        int maskIdx = (int)s[MASKI];
        if (maskIdx > 0) {
            if (maskIdx >= NODE_CAP || !sg_->nodes[maskIdx].alive) return false;
            const float* m = sg_->buf + maskIdx * STRIDE;
            int flags = (int)m[HITFLAGS];
            bool geometryMask = isGeometryMask(maskIdx, m, flags);
            // Rotated/skewed rect masks that still have a solid white fill can write stencil via
            // the ordinary sprite/quad path; force that path instead of the old "non-rect" escape.
            bool stencilMask = geometryMask || (flags & (8 | 16)) != 0 || !preflightRectMask(m);
            if (stencilMask) {
                bool forceGeometry = geometryMask || (flags & 64) != 0 || hasMaskGeometry(maskIdx, m);
                if (!preflightMaskGeometry(maskIdx, forceGeometry) && !preflightTexture((int)m[TEXID])) return false;
            } else {
                int rectChildren = 0;
                const auto& maskChildren = sg_->nodes[maskIdx].children;
                if (maskChildren.size() > 1) {
                    for (int child : maskChildren) {
                        if (child > 0 && child < NODE_CAP && sg_->nodes[child].alive &&
                            preflightRectMask(sg_->buf + child * STRIDE)) ++rectChildren;
                    }
                }
                // Batcher::walk only selects the per-column union path when at least two usable
                // child rectangles exist; otherwise it falls back to the mask object's own rect.
                if (rectChildren <= 1 && !preflightRectMask(m) && !hasMaskGeometry(maskIdx, m) && !preflightTexture((int)m[TEXID])) return false;
            }
        }

        if (type == T_SPRITE) {
            if (!preflightTexture((int)s[TEXID])) return false;
        } else if (type == T_MULTIQUAD) {
            int off = (int)s[QUADOFF], cnt = (int)s[QUADCNT];
            if (off < 0 || cnt < 0 || off + cnt > QUAD_CAP) return false;
            const float* qb = sg_->buf + QUAD_BASE + 1;
            for (int q = 0; q < cnt; ++q) {
                const float* mq = qb + (size_t)(off + q) * QUAD_STRIDE_F;
                if (mq[0] < 0.0f) {
                    int child = (int)(-mq[0]);
                    if (child > 0 && child < NODE_CAP && sg_->nodes[child].alive &&
                        sg_->nodes[child].parent == idx && !preflightNode(child, true)) return false;
                } else if (!preflightTexture((int)mq[0])) return false;
            }
        } else if (type == T_NATIVE_MULTIQUAD) {
            for (const auto& q : sg_->nodes[idx].nativeQuads) {
                if (q[0] < 0.0f) {   // spinex slotObject marker:按 -q[0] 预检子节点(与 T_MULTIQUAD 同约定)
                    int child = (int)(-q[0]);
                    if (child > 0 && child < NODE_CAP && sg_->nodes[child].alive &&
                        sg_->nodes[child].parent == idx && !preflightNode(child, true)) return false;
                } else if (!preflightTexture((int)q[0])) return false;
            }
        } else if (type != T_CONTAINER && type != T_SPINE_SLOT) {
            return false;
        }

        for (int child : sg_->nodes[idx].children) {
            if (child > 0 && child < NODE_CAP && sg_->nodes[child].alive &&
                !preflightNode(child, false)) return false;
        }
        return true;
    }

    bool ensurePostTarget(int depth,int w,int h) {
        if(depth<0||w<=0||h<=0)return false;
        if(postProg_==0){
            const char* vs=
                "precision mediump float; attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV; uniform vec4 uRect; uniform vec2 uTarget;\n"
                "void main(){vec2 p=uRect.xy+aPos*uRect.zw;gl_Position=vec4(p.x*2.0/uTarget.x-1.0,1.0-p.y*2.0/uTarget.y,0.0,1.0);vUV=aUV;}\n";
            const char* fs=
                "precision mediump float; varying vec2 vUV; uniform sampler2D uTex;\n"
                "uniform float uRadius; uniform float uAngle; uniform vec2 uOffset; uniform vec4 uRect; uniform vec4 uCoordRect; uniform vec2 uTexScale; uniform float uMode; uniform vec2 uInvTex;\n"
                "uniform sampler2D uMap; uniform vec2 uDispScale; uniform vec4 uDispMapLinear; uniform vec2 uDispMapTranslate; uniform vec4 uDispRotation; uniform vec2 uKawaseOffset; uniform float uKawaseClamp; uniform vec2 uGaussianAxis; uniform float uGaussianRadius; uniform float uGaussianWeights[8]; uniform float uGaussianClamp; uniform vec2 uPostShadowOffset; uniform vec3 uPostShadowColor; uniform float uPostShadowAlpha;\n"
                "vec4 mainSample(vec2 uv){return texture2D(uTex,clamp(uv,uInvTex*.5,uTexScale-uInvTex*.5));}\n"
                "vec4 kawaseSample(vec2 uv){return uKawaseClamp>.5?mainSample(uv):texture2D(uTex,uv);}\n"
                "vec4 kawase(vec2 uv){vec2 o=uKawaseOffset*uInvTex;return .25*(kawaseSample(uv+vec2(-o.x,o.y))+kawaseSample(uv+vec2(o.x,o.y))+kawaseSample(uv+vec2(o.x,-o.y))+kawaseSample(uv-vec2(o.x,o.y)));}\n"
                "vec4 gaussianSample(vec2 uv){return uGaussianClamp>.5?mainSample(uv):texture2D(uTex,uv);}\n"
                "vec4 gaussian(vec2 uv){vec4 c=gaussianSample(uv)*uGaussianWeights[0];for(int i=1;i<8;i++){if(float(i)<=uGaussianRadius){vec2 o=uGaussianAxis*float(i)*uInvTex;c+=(gaussianSample(uv-o)+gaussianSample(uv+o))*uGaussianWeights[i];}}return c;}\n"
                "vec4 shadowTint(vec2 uv){vec4 s=mainSample(uv-uPostShadowOffset*uInvTex);float a=s.a*uPostShadowAlpha;return vec4(uPostShadowColor*a,a);}\n"
                "vec4 fxaa(vec2 uv){vec3 nw=mainSample(uv+vec2(-1.0,-1.0)*uInvTex).rgb,ne=mainSample(uv+vec2(1.0,-1.0)*uInvTex).rgb,sw=mainSample(uv+vec2(-1.0,1.0)*uInvTex).rgb,se=mainSample(uv+vec2(1.0,1.0)*uInvTex).rgb,m=mainSample(uv).rgb;vec3 l=vec3(0.299,0.587,0.114);float ln=dot(nw,l),le=dot(ne,l),ls=dot(sw,l),lse=dot(se,l),lm=dot(m,l),lo=min(lm,min(min(ln,le),min(ls,lse))),hi=max(lm,max(max(ln,le),max(ls,lse)));vec2 d=vec2(-((ln+le)-(ls+lse)),(ln+ls)-(le+lse));float r=1.0/(min(abs(d.x),abs(d.y))+max((ln+le+ls+lse)*0.03125,0.0078125));d=clamp(d*r,vec2(-8.0),vec2(8.0))*uInvTex;vec4 a=.5*(mainSample(uv+d*(1.0/3.0-.5))+mainSample(uv+d*(2.0/3.0-.5)));vec4 b=a*.5+.25*(mainSample(uv+d*-.5)+mainSample(uv+d*.5));float lb=dot(b.rgb,l);return lb<lo||lb>hi?a:b;}\n"
                "void main(){vec2 coord=uCoordRect.xy+vUV*uCoordRect.zw;vec2 p=coord-uOffset;float dist=length(p);\n"
                " if(uMode<1.5&&uRadius>0.0&&dist<uRadius){float r=(uRadius-dist)/uRadius;float a=r*r*uAngle;float s=sin(a),c=cos(a);p=vec2(p.x*c-p.y*s,p.x*s+p.y*c);coord=p+uOffset;}\n"
                " if(uMode>2.5){vec2 mapUV=vec2(uDispMapLinear.x*coord.x+uDispMapLinear.z*coord.y,uDispMapLinear.y*coord.x+uDispMapLinear.w*coord.y)+uDispMapTranslate;vec2 m=texture2D(uMap,mapUV).xy-vec2(.5);coord+=vec2(uDispRotation.x*m.x+uDispRotation.z*m.y,uDispRotation.y*m.x+uDispRotation.w*m.y)*uDispScale;}\n"
                " vec2 uv=(coord-uCoordRect.xy)/uCoordRect.zw;uv=vec2(uv.x*uTexScale.x,(1.0-uv.y)*uTexScale.y);gl_FragColor=uMode>5.5?shadowTint(uv):(uMode>4.5?gaussian(uv):(uMode>3.5?kawase(uv):((uMode>1.5&&uMode<2.5)?fxaa(uv):mainSample(uv))));}\n";
            postProg_=link(vs,fs);if(!postProg_)return false;
            postPos_=glGetAttribLocation(postProg_,"aPos");postUV_=glGetAttribLocation(postProg_,"aUV");
            postRadius_=glGetUniformLocation(postProg_,"uRadius");postAngle_=glGetUniformLocation(postProg_,"uAngle");postOffset_=glGetUniformLocation(postProg_,"uOffset");postRect_=glGetUniformLocation(postProg_,"uRect");postCoordRect_=glGetUniformLocation(postProg_,"uCoordRect");postTarget_=glGetUniformLocation(postProg_,"uTarget");postTexScale_=glGetUniformLocation(postProg_,"uTexScale");postMode_=glGetUniformLocation(postProg_,"uMode");postInvTex_=glGetUniformLocation(postProg_,"uInvTex");postDispScale_=glGetUniformLocation(postProg_,"uDispScale");postDispMapLinear_=glGetUniformLocation(postProg_,"uDispMapLinear");postDispMapTranslate_=glGetUniformLocation(postProg_,"uDispMapTranslate");postDispRotation_=glGetUniformLocation(postProg_,"uDispRotation");postKawaseOffset_=glGetUniformLocation(postProg_,"uKawaseOffset");postKawaseClamp_=glGetUniformLocation(postProg_,"uKawaseClamp");postGaussianAxis_=glGetUniformLocation(postProg_,"uGaussianAxis");postGaussianRadius_=glGetUniformLocation(postProg_,"uGaussianRadius");postGaussianWeights_=glGetUniformLocation(postProg_,"uGaussianWeights");postGaussianClamp_=glGetUniformLocation(postProg_,"uGaussianClamp");postShadowOffset_=glGetUniformLocation(postProg_,"uPostShadowOffset");postShadowColor_=glGetUniformLocation(postProg_,"uPostShadowColor");postShadowAlpha_=glGetUniformLocation(postProg_,"uPostShadowAlpha");
            glUseProgram(postProg_);glUniform1i(glGetUniformLocation(postProg_,"uTex"),0);glUniform1i(glGetUniformLocation(postProg_,"uMap"),1);
            const float quad[16]={0,0,0,0, 1,0,1,0, 0,1,0,1, 1,1,1,1};
            glGenBuffers(1,&postVbo_);glBindBuffer(GL_ARRAY_BUFFER,postVbo_);glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
        }
        if((int)postTargets_.size()<=depth)postTargets_.resize((size_t)depth+1);
        PostTarget* target=nullptr;
        int allocW=512,allocH=512;
        if(compactPostTargets){
            // Keep transparent texels beyond the active viewport for unclamped blur samples.
            allocW=64;allocH=64;while(allocW<w||(allocW==w&&w<512))allocW*=2;while(allocH<h||(allocH==h&&h<512))allocH*=2;
            if((int)postSizedTargets_.size()<=depth)postSizedTargets_.resize((size_t)depth+1);
            auto& pool=postSizedTargets_[depth];
            for(auto& entry:pool)if(entry.w==allocW&&entry.h==allocH){target=&entry;break;}
            if(!target){
                // Retain at most one large allocation per depth; small size classes may coexist.
                if(allocW>512||allocH>512)for(auto& entry:pool)if(entry.w>512||entry.h>512){
                    target=&entry;allocW=std::max(allocW,entry.w);allocH=std::max(allocH,entry.h);break;
                }
            }
            if(!target){
                // Several width/height classes can alternate within one frame. Keeping only
                // two or three would repeatedly evict and allocate textures for animated sizes.
                if(pool.size()<8){pool.emplace_back();target=&pool.back();}
                else target=&*std::min_element(pool.begin(),pool.end(),[](const PostTarget& a,const PostTarget& b){return a.used<b.used;});
            }
        }else{
            if((int)postLegacyTargets_.size()<=depth)postLegacyTargets_.resize((size_t)depth+1);
            target=&postLegacyTargets_[depth];
            allocW=std::max(512,target->w);allocH=std::max(512,target->h);while(allocW<w)allocW*=2;while(allocH<h)allocH*=2;
        }
        PostTarget& p=*target;p.used=++postTargetUse_;
        if(p.fbo&&p.w==allocW&&p.h==allocH){postTargets_[depth]=p;return true;}
        if(p.stencil){glDeleteRenderbuffers(1,&p.stencil);p.stencil=0;}
        if(p.tex){glDeleteTextures(1,&p.tex);p.tex=0;}
        if(p.fbo){glDeleteFramebuffers(1,&p.fbo);p.fbo=0;}
        GLint oldFbo=0,oldTex=0,oldRb=0;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&oldFbo);glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTex);glGetIntegerv(GL_RENDERBUFFER_BINDING,&oldRb);
        glGenTextures(1,&p.tex);glBindTexture(GL_TEXTURE_2D,p.tex);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,allocW,allocH,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glGenRenderbuffers(1,&p.stencil);glBindRenderbuffer(GL_RENDERBUFFER,p.stencil);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,allocW,allocH);
        glGenFramebuffers(1,&p.fbo);glBindFramebuffer(GL_FRAMEBUFFER,p.fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,p.tex,0);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,p.stencil);
        bool ok=glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)oldFbo);glBindTexture(GL_TEXTURE_2D,(GLuint)oldTex);glBindRenderbuffer(GL_RENDERBUFFER,(GLuint)oldRb);
        if(!ok){fprintf(stderr,"[batcher] post FBO[%d] incomplete %dx%d\n",depth,allocW,allocH);glDeleteFramebuffers(1,&p.fbo);glDeleteTextures(1,&p.tex);glDeleteRenderbuffers(1,&p.stencil);p=PostTarget();return false;}
        p.w=allocW;p.h=allocH;postTargets_[depth]=p;return true;
    }

    void restoreClipState() {
        applyScissor();
        if(stencilDepth_>0){glEnable(GL_STENCIL_TEST);glStencilMask(0x00);glStencilFunc(GL_EQUAL,stencilDepth_,0xFF);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);}
        else glDisable(GL_STENCIL_TEST);
    }

    bool renderPost(int idx,const float* fd,float parentFilterAlpha,float parentBlurX,float parentBlurY,const float* parentCM,bool parentCMOn,bool fromSpineMarker,const float* parentOverlay,bool parentOverlayOn,bool shadowPass,float parentNoise,float parentNoiseSeed) {
        // Capture once, then run every enabled post filter through a three-target ping-pong pool in
        // the exact order supplied by Pixi. Nested filtered children finish before these scratch
        // targets are reused, so recursion depth and stack length remain independently bounded.
        const double profileStart=postClock();
        const int filterDepth=postDepth_;if(filterDepth>=8)return false;
        GLuint passMapTex=0;if(fd[103]<2.0f&&fd[47]>0.5f){passMapTex=texLookup?texLookup((int)fd[48]):0;if(!passMapTex)return false;}
        // 已录批必须先画到父目标:下面立刻切 FBO 做离屏捕获,且滤镜合成结果会即时绘回父目标,
        // 若父目标上"更早的内容"仍在录制列表里,帧末回放会把它们盖到合成结果上(z 序错乱)。
        replayPending();GLint parentFbo=0,parentVP[4]={0,0,0,0};glGetIntegerv(GL_FRAMEBUFFER_BINDING,&parentFbo);glGetIntegerv(GL_VIEWPORT,parentVP);
        float parentLW=targetLogicalW_,parentLH=targetLogicalH_;if(parentLW<=0||parentLH<=0)return false;
        const float* bd=sg_->buf+BOUNDS_BASE+(size_t)idx*BOUNDS_STRIDE_F+4;if(bd[0]>bd[2]||bd[1]>bd[3])return false;
        float baseOut[6]={1,0,0,1,0,0};if(outputTransformOn_)memcpy(baseOut,outputTransform_,sizeof(baseOut));
        auto baseMap=[&](float x,float y,float& ox,float& oy){ox=baseOut[0]*x+baseOut[2]*y+baseOut[4];oy=baseOut[1]*x+baseOut[3]*y+baseOut[5];};
        float xs[4]={bd[0],bd[2],bd[2],bd[0]},ys[4]={bd[1],bd[1],bd[3],bd[3]},rx0=1e30f,ry0=1e30f,rx1=-1e30f,ry1=-1e30f;
        for(int k=0;k<4;k++){float x,y;baseMap(xs[k],ys[k],x,y);rx0=fminf(rx0,x);ry0=fminf(ry0,y);rx1=fmaxf(rx1,x);ry1=fmaxf(ry1,y);}
        float outScale=(hypotf(baseOut[0],baseOut[1])+hypotf(baseOut[2],baseOut[3]))*.5f,pad=fd[45]*outScale;
        rx0=floorf(fmaxf(0.0f,rx0-pad));ry0=floorf(fmaxf(0.0f,ry0-pad));rx1=ceilf(fminf(parentLW,rx1+pad));ry1=ceilf(fminf(parentLH,ry1+pad));
        float regionW=rx1-rx0,regionH=ry1-ry0;if(regionW<=0||regionH<=0)return true;
        int w=std::max(1,(int)ceilf(regionW*parentVP[2]/parentLW)),h=std::max(1,(int)ceilf(regionH*parentVP[3]/parentLH));
        const float pixelScaleX=w/regionW,pixelScaleY=h/regionH;
        // Independent small effects rotate among three pools so the next capture does not
        // overwrite a texture still being sampled by the preceding composite. Large effects
        // share bank zero to retain the existing upper bound for large framebuffer allocations.
        if(filterDepth==0)postRootBank_=(w<=512&&h<=512)?postRootSequence_++%std::max(1,std::min(3,postTargetBanks)):0;
        const int targetDepth=filterDepth+postRootBank_*11;
        if(profilePost){postProfile.filters++;postProfile.maxW=std::max(postProfile.maxW,(unsigned)w);postProfile.maxH=std::max(postProfile.maxH,(unsigned)h);}
        if(parentFbo<0||!ensurePostTarget(targetDepth,w,h))return false;
        const GLuint sourceFbo=postTargets_[targetDepth].fbo,sourceTex=postTargets_[targetDepth].tex;const int sourceW=postTargets_[targetDepth].w,sourceH=postTargets_[targetDepth].h;
        std::vector<SRect> savedScissors=scissors_;int savedStencil=stencilDepth_,savedSkip=twistSkipNode_;std::vector<uint8_t> savedGeomStack;savedGeomStack.swap(maskGeomStack_);bool savedOutputOn=outputTransformOn_;float savedOutput[6];memcpy(savedOutput,outputTransform_,sizeof(savedOutput));float savedProj[4];memcpy(savedProj,proj_,sizeof(savedProj));float savedScreenH=screenH_;unsigned esc0=escapes,tex0=escTex,mask0=escMask;
        postDepth_++;twistSkipNode_=idx;scissors_.clear();stencilDepth_=0;outputTransformOn_=true;memcpy(outputTransform_,baseOut,sizeof(baseOut));outputTransform_[4]-=rx0;outputTransform_[5]-=ry0;targetLogicalW_=regionW;targetLogicalH_=regionH;screenH_=regionH;proj_[0]=2.0f/regionW;proj_[1]=-2.0f/regionH;proj_[2]=-1.0f;proj_[3]=1.0f;
        targetViewport_[0]=targetViewport_[1]=0;targetViewport_[2]=w;targetViewport_[3]=h;
        const double captureStart=postClock();
        glBindFramebuffer(GL_FRAMEBUFFER,sourceFbo);glViewport(0,0,w,h);glDisable(GL_SCISSOR_TEST);glDisable(GL_STENCIL_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glClearColor(0,0,0,0);glClearStencil(0);glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);setupState();
        walk(idx,parentFilterAlpha,parentBlurX,parentBlurY,parentCM,parentCMOn,fromSpineMarker,parentOverlay,parentOverlayOn,shadowPass,parentNoise,parentNoiseSeed);flush();postDepth_--;twistSkipNode_=savedSkip;
        if(profilePost)postProfile.captureMs+=postClock()-captureStart;
        // escTex (texture not yet uploaded) must NOT fail the whole capture: a lazily-loading
        // icon inside a filtered popup would blank the ENTIRE popup for a frame. The missing
        // quad is simply absent this frame and fills in when its texture lands. Structural
        // escapes (unsupported feature / mask) still fail closed.
        bool ok=escapes==esc0&&escMask==mask0;(void)tex0;outputTransformOn_=savedOutputOn;memcpy(outputTransform_,savedOutput,sizeof(savedOutput));memcpy(proj_,savedProj,sizeof(savedProj));targetLogicalW_=parentLW;targetLogicalH_=parentLH;screenH_=savedScreenH;memcpy(targetViewport_,parentVP,sizeof(targetViewport_));glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)parentFbo);glViewport(parentVP[0],parentVP[1],parentVP[2],parentVP[3]);scissors_.swap(savedScissors);stencilDepth_=savedStencil;maskGeomStack_.swap(savedGeomStack);
        if(!ok){setupState();restoreClipState();return false;}
        if(!ensurePostTarget(targetDepth+1,w,h)||!ensurePostTarget(targetDepth+2,w,h)||!ensurePostTarget(targetDepth+3,w,h)){setupState();restoreClipState();return false;}
        // Every failure below happens after one of the scratch FBOs may have been bound.  Always
        // restore the caller's target and clip state before reporting an escape; otherwise one bad
        // descriptor can leave the rest of the frame drawing into an invisible temporary texture.
        auto failPost=[&]()->bool{glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)parentFbo);glViewport(parentVP[0],parentVP[1],parentVP[2],parentVP[3]);setupState();restoreClipState();return false;};
        PostTarget scratch[3]={postTargets_[targetDepth+1],postTargets_[targetDepth+2],postTargets_[targetDepth+3]};
        auto choose=[&](GLuint avoidA,GLuint avoidB)->const PostTarget*{for(int i=0;i<3;i++)if(scratch[i].tex!=avoidA&&scratch[i].tex!=avoidB)return &scratch[i];return nullptr;};
        auto prepare=[&](const PostTarget& dst,GLuint src,int srcW,int srcH,float mode,bool clear){const double prepareStart=postClock();glBindFramebuffer(GL_FRAMEBUFFER,dst.fbo);glViewport(0,0,w,h);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glDisable(GL_STENCIL_TEST);if(clear){glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);}glUseProgram(postProg_);glBindBuffer(GL_ARRAY_BUFFER,postVbo_);glEnableVertexAttribArray((GLuint)postPos_);glVertexAttribPointer((GLuint)postPos_,2,GL_FLOAT,GL_FALSE,16,(void*)0);glEnableVertexAttribArray((GLuint)postUV_);glVertexAttribPointer((GLuint)postUV_,2,GL_FLOAT,GL_FALSE,16,(void*)8);glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,passMapTex);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,src);glUniform4f(postRect_,0,0,regionW,regionH);glUniform4f(postCoordRect_,rx0,ry0,regionW,regionH);glUniform2f(postTarget_,regionW,regionH);glUniform2f(postTexScale_,(float)w/srcW,(float)h/srcH);glUniform2f(postInvTex_,1.0f/srcW,1.0f/srcH);glUniform1f(postMode_,mode);glUniform1f(postRadius_,0);glUniform1f(postAngle_,0);glUniform2f(postOffset_,0,0);glUniform2f(postDispScale_,0,0);glUniform4f(postDispMapLinear_,1,0,0,1);glUniform2f(postDispMapTranslate_,0,0);glUniform4f(postDispRotation_,1,0,0,1);glUniform2f(postKawaseOffset_,0,0);glUniform1f(postKawaseClamp_,1);glUniform2f(postGaussianAxis_,0,0);glUniform1f(postGaussianRadius_,0);glUniform1f(postGaussianClamp_,1);glUniform2f(postShadowOffset_,0,0);glUniform3f(postShadowColor_,0,0,0);glUniform1f(postShadowAlpha_,1);if(profilePost)postProfile.prepareMs+=postClock()-prepareStart;};
        auto draw=[&](){const double drawStart=postClock();glDrawArrays(GL_TRIANGLE_STRIP,0,4);drawCalls++;quads++;if(profilePost){postProfile.drawMs+=postClock()-drawStart;postProfile.passes++;}};
        auto weightsFor=[&](int kernel,float* weights,int& radiusN){for(int i=0;i<8;i++)weights[i]=0;radiusN=(kernel-1)/2;switch(kernel){case 7:{float a[4]={.214607f,.189879f,.131514f,.071303f};memcpy(weights,a,sizeof(a));break;}case 9:{float a[5]={.20236f,.179044f,.124009f,.067234f,.028532f};memcpy(weights,a,sizeof(a));break;}case 11:{float a[6]={.198596f,.175713f,.121703f,.065984f,.028002f,.0093f};memcpy(weights,a,sizeof(a));break;}case 13:{float a[7]={.197641f,.174868f,.121117f,.065666f,.027867f,.009255f,.002406f};memcpy(weights,a,sizeof(a));break;}case 15:{float a[8]={.197448f,.174697f,.120999f,.065602f,.02784f,.009246f,.002403f,.000489f};memcpy(weights,a,sizeof(a));break;}default:{float a[3]={.250301f,.221461f,.153388f};memcpy(weights,a,sizeof(a));radiusN=2;break;}}};
        struct OrderedPost { int kind; const float* p; };
        std::vector<OrderedPost> order;
        int encoded=fd[103]>0.5f?std::max(0,std::min(POST_FILTER_DESC_CAP,(int)fd[96])):0;
        for(int i=0;i<encoded;i++){
            const float* p=fd[103]>=2.0f?fd+POST_FILTER_DESC_BASE+i*POST_FILTER_DESC_STRIDE_F:nullptr;
            order.push_back({p?(int)p[0]:(int)fd[97+i],p});
        }
        if(order.empty()){
            if(fd[29]>0.5f)order.push_back({5,nullptr});if(fd[78]>0.5f)order.push_back({2,nullptr});
            if(fd[61]>0.5f)order.push_back({10,nullptr});if(fd[40]>0.5f)order.push_back({7,nullptr});
            if(fd[46]>0.5f)order.push_back({8,nullptr});if(fd[47]>0.5f)order.push_back({9,nullptr});
        }
        GLuint current=sourceTex;int currentW=sourceW,currentH=sourceH;
        for(const OrderedPost& post:order){
            int kind=post.kind;const float* p=post.p;passMapTex=0;
            if(kind==2||kind==10){
                bool gaussian=kind==2;
                float gx=p?p[1]:fd[79],gy=p?p[2]:fd[80];
                int quality=gaussian?std::max(1,std::min(8,(int)(p?p[3]:fd[81]))):1;
                int xPasses=gaussian&&gx>0?quality:0,yPasses=gaussian&&gy>0?quality:0;
                int passes=gaussian?xPasses+yPasses:std::max(1,std::min(12,(int)(p?p[1]:fd[62])));
                for(int pass=0;pass<passes;pass++){
                    const PostTarget* dst=choose(current,0);if(!dst)return failPost();
                    prepare(*dst,current,currentW,currentH,gaussian?5.0f:4.0f,true);
                    if(gaussian){float weights[8];int radiusN;weightsFor((int)(p?p[4]:fd[82]),weights,radiusN);bool horizontal=pass<xPasses;glUniform2f(postGaussianAxis_,horizontal?gx/quality*pixelScaleX:0,horizontal?0:gy/quality*pixelScaleY);glUniform1f(postGaussianRadius_,(float)radiusN);glUniform1fv(postGaussianWeights_,8,weights);glUniform1f(postGaussianClamp_,p?p[5]:fd[83]);}
                    else{float kernel=(p?p[5+pass]:fd[66+pass])+.5f;glUniform2f(postKawaseOffset_,kernel*(p?p[3]:fd[64])*pixelScaleX,kernel*(p?p[4]:fd[65])*pixelScaleY);glUniform1f(postKawaseClamp_,p?p[2]:fd[63]);}
                    draw();current=dst->tex;currentW=dst->w;currentH=dst->h;
                }
            }
            else if(kind==5){
                const GLuint original=current;const int originalW=currentW,originalH=currentH;const PostTarget* tint=choose(original,0);if(!tint)return failPost();
                prepare(*tint,original,originalW,originalH,6,true);
                float dx=p?p[1]:fd[30],dy=p?p[2]:fd[31],shadowDX=baseOut[0]*dx+baseOut[2]*dy,shadowDY=baseOut[1]*dx+baseOut[3]*dy;
                glUniform2f(postShadowOffset_,shadowDX*pixelScaleX,-shadowDY*pixelScaleY);glUniform3f(postShadowColor_,p?p[3]:fd[33],p?p[4]:fd[34],p?p[5]:fd[35]);glUniform1f(postShadowAlpha_,p?p[6]:fd[36]);draw();
                GLuint blurred=tint->tex;int blurredW=tint->w,blurredH=tint->h;int passes=std::max(1,std::min(8,(int)(p?p[8]:fd[84])));
                for(int pass=0;pass<passes;pass++){const PostTarget* dst=choose(blurred,original);if(!dst)dst=choose(blurred,0);if(!dst)return failPost();prepare(*dst,blurred,blurredW,blurredH,4,true);float kernel=(p?p[12+pass]:fd[87+pass])+.5f;glUniform2f(postKawaseOffset_,kernel*(p?p[9]:fd[85])*pixelScaleX,kernel*(p?p[10]:fd[86])*pixelScaleY);glUniform1f(postKawaseClamp_,p?p[11]:fd[95]);draw();blurred=dst->tex;blurredW=dst->w;blurredH=dst->h;}
                const PostTarget* combined=choose(blurred,original);if(!combined)return failPost();prepare(*combined,blurred,blurredW,blurredH,0,true);draw();
                if((p?p[7]:fd[37])<.5f){prepare(*combined,original,originalW,originalH,0,false);glEnable(GL_BLEND);glBlendEquation(GL_FUNC_ADD);glBlendFuncSeparate(GL_ONE,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);draw();glDisable(GL_BLEND);}
                current=combined->tex;currentW=combined->w;currentH=combined->h;
            }
            else if(kind==7||kind==8||kind==9){
                float twistRadius=p?p[1]:fd[41],twistAngle=p?p[2]:fd[42],twistX=p?p[3]:fd[43],twistY=p?p[4]:fd[44];
                float ox=0,oy=0,ox1=0,oy1=0,radius=0;if(kind==7){baseMap(twistX,twistY,ox,oy);baseMap(twistX+twistRadius,twistY,ox1,oy1);radius=hypotf(ox1-ox,oy1-oy);}
                float mapA=0,mapB=0,mapC=0,mapD=0,mapTX=0,mapTY=0,dispA=0,dispB=0,dispC=0,dispD=0;
                if(kind==9){
                    int mapId=(int)(p?p[1]:fd[48]);passMapTex=texLookup?texLookup(mapId):0;if(!passMapTex)return failPost();
                    float sx=p?p[2]:fd[49],sy=p?p[3]:fd[50];mapA=p?p[4]:fd[51];mapB=p?p[5]:fd[52];mapC=p?p[6]:fd[53];mapD=p?p[7]:fd[54];mapTX=p?p[8]:fd[55];mapTY=p?p[9]:fd[56];
                    float outDet=baseOut[0]*baseOut[3]-baseOut[1]*baseOut[2];if(fabsf(outDet)<1e-8f)return failPost();
                    float ia=baseOut[3]/outDet,ib=-baseOut[1]/outDet,ic=-baseOut[2]/outDet,id=baseOut[0]/outDet,itx=-(ia*baseOut[4]+ic*baseOut[5]),ity=-(ib*baseOut[4]+id*baseOut[5]);
                    float a=mapA*ia+mapC*ib,b=mapB*ia+mapD*ib,c=mapA*ic+mapC*id,d=mapB*ic+mapD*id,tx=mapA*itx+mapC*ity+mapTX,ty=mapB*itx+mapD*ity+mapTY;mapA=a;mapB=b;mapC=c;mapD=d;mapTX=tx;mapTY=ty;
                    float ra=p?p[10]:fd[57],rb=p?p[11]:fd[58],rc=p?p[12]:fd[59],rd=p?p[13]:fd[60];dispA=baseOut[0]*sx*ra+baseOut[2]*sy*rb;dispB=baseOut[1]*sx*ra+baseOut[3]*sy*rb;dispC=baseOut[0]*sx*rc+baseOut[2]*sy*rd;dispD=baseOut[1]*sx*rc+baseOut[3]*sy*rd;
                }
                const PostTarget* dst=choose(current,0);if(!dst)return failPost();prepare(*dst,current,currentW,currentH,kind==7?1.0f:(kind==8?2.0f:3.0f),true);
                if(kind==7){glUniform1f(postRadius_,radius);glUniform1f(postAngle_,twistAngle);glUniform2f(postOffset_,ox,oy);}
                else if(kind==9){glUniform2f(postDispScale_,1,1);glUniform4f(postDispMapLinear_,mapA,mapB,mapC,mapD);glUniform2f(postDispMapTranslate_,mapTX,mapTY);glUniform4f(postDispRotation_,dispA,dispB,dispC,dispD);}
                draw();current=dst->tex;currentW=dst->w;currentH=dst->h;
            }
            else return failPost();
        }
        const double compositeStart=postClock();
        glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)parentFbo);glViewport(parentVP[0],parentVP[1],parentVP[2],parentVP[3]);glUseProgram(postProg_);glBindBuffer(GL_ARRAY_BUFFER,postVbo_);glEnableVertexAttribArray((GLuint)postPos_);glVertexAttribPointer((GLuint)postPos_,2,GL_FLOAT,GL_FALSE,16,(void*)0);glEnableVertexAttribArray((GLuint)postUV_);glVertexAttribPointer((GLuint)postUV_,2,GL_FLOAT,GL_FALSE,16,(void*)8);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,current);glUniform4f(postRect_,rx0,ry0,regionW,regionH);glUniform4f(postCoordRect_,rx0,ry0,regionW,regionH);glUniform2f(postTarget_,parentLW,parentLH);glUniform2f(postTexScale_,(float)w/currentW,(float)h/currentH);glUniform2f(postInvTex_,1.0f/currentW,1.0f/currentH);glUniform1f(postMode_,0);glUniform1f(postRadius_,0);glUniform2f(postDispScale_,0,0);glUniform2f(postKawaseOffset_,0,0);glUniform2f(postGaussianAxis_,0,0);glUniform1f(postGaussianRadius_,0);curBlend_=-1;setBlend((int)(sg_->buf[idx*STRIDE+BLEND]));restoreClipState();draw();
        if(!postLogged_){postLogged_=true;fprintf(stderr,"[batcher] native ordered post-process stack active (filters=%zu %dx%d of %dx%d)\n",order.size(),w,h,parentVP[2],parentVP[3]);}if(filterDepth>0&&!nestedPostLogged_){nestedPostLogged_=true;fprintf(stderr,"[batcher] nested native post-process active (depth=%d)\n",filterDepth+1);}setupState();restoreClipState();if(profilePost){postProfile.compositeMs+=postClock()-compositeStart;if(filterDepth==0)postProfile.totalMs+=postClock()-profileStart;}return true;
    }

    void walk(int idx, float parentFilterAlpha=1.0f, float parentBlurX=0.0f, float parentBlurY=0.0f, const float* parentCM=nullptr, bool parentCMOn=false, bool fromSpineMarker=false, const float* parentOverlay=nullptr, bool parentOverlayOn=false, bool shadowPass=false, float parentNoise=0.0f, float parentNoiseSeed=0.0f) {
        float* s = sg_->buf + idx*STRIDE;
        // 对齐 pixi Container.render 的剪枝:!visible || worldAlpha<=0 || !renderable → 整子树不画。
        // HITFLAGS.4 is the stronger native invariant for a mask display object.  The owner later
        // emits this same node explicitly through emitMaskShape(), with color writes disabled.
        if (s[VISIBLE]==0.0f || s[WALPHA]<=0.0f || s[RENDERABLE]==0.0f ||
            (((int)s[HITFLAGS])&4)!=0) return;
        int type = (int)s[TYPE];
        if (type == T_SPINE_SLOT && !fromSpineMarker) return;
        // Viewport cull using the render-subtree AABB computed at frame start. Nested post-
        // process passes and Spine marker children keep their own local targets, so skip them.
        if (cullEnabled_ && postDepth_==0 && !fromSpineMarker && outsideViewport(idx)) {
            culled++;
            return;
        }
        // HAS_FILTER 位:FILTER 区 ~29MB 几乎全零,无 filter 节点跳过全部 fd 读取(每节点省 2-5 条
        // 远端 cache line);继承链(alpha/blur/CM/overlay/noise)直接下传父值,post 判定短路 false。
        const bool hasFilter = !filterFlagEnabled() || sg_->buf[FILTER_FLAG_BASE + idx] != 0.0f;
        const float* fd = sg_->buf + FILTER_BASE + (size_t)idx*FILTER_STRIDE_F;
        float nodeFilterAlpha, nodeBlurX, nodeBlurY, nodeCM[20];
        bool nodeCMOn, nodeOverlayOn; float nodeOverlay[4]={0,0,0,0};
        float nodeNoise, nodeNoiseSeed;
        if (hasFilter) {
            nodeFilterAlpha = parentFilterAlpha * fd[0];
            nodeBlurX = parentBlurX + fd[1]; nodeBlurY = parentBlurY + fd[2];
            bool localCMOn=fd[3]>0.5f;nodeCMOn=parentCMOn||localCMOn;
            if(parentCMOn&&localCMOn)composeCM(parentCM,fd+4,nodeCM);else if(parentCMOn)memcpy(nodeCM,parentCM,sizeof(nodeCM));else if(localCMOn)memcpy(nodeCM,fd+4,sizeof(nodeCM));
            bool localOverlayOn=fd[24]>0.5f;nodeOverlayOn=localOverlayOn||parentOverlayOn;if(localOverlayOn)memcpy(nodeOverlay,fd+25,sizeof(nodeOverlay));else if(parentOverlayOn)memcpy(nodeOverlay,parentOverlay,sizeof(nodeOverlay));
            float localNoise=std::max(0.0f,std::min(1.0f,fd[38]));
            nodeNoise=std::min(1.0f,parentNoise+localNoise);
            nodeNoiseSeed=localNoise>0.0f?fd[39]:parentNoiseSeed;
        } else {
            nodeFilterAlpha=parentFilterAlpha; nodeBlurX=parentBlurX; nodeBlurY=parentBlurY;
            nodeCMOn=parentCMOn; if(parentCMOn)memcpy(nodeCM,parentCM,sizeof(nodeCM));
            nodeOverlayOn=parentOverlayOn; if(parentOverlayOn)memcpy(nodeOverlay,parentOverlay,sizeof(nodeOverlay));
            nodeNoise=parentNoise; nodeNoiseSeed=parentNoiseSeed;
        }
        activeFilterAlpha_=nodeFilterAlpha;activeBlurX_=nodeBlurX;activeBlurY_=nodeBlurY;activeCMOn_=nodeCMOn;if(nodeCMOn)memcpy(activeCM_,nodeCM,sizeof(nodeCM));
        activeOverlayOn_=nodeOverlayOn;if(nodeOverlayOn)memcpy(activeOverlay_,nodeOverlay,sizeof(nodeOverlay));
        activeNoise_=nodeNoise;activeNoiseSeed_=nodeNoiseSeed;
        // 矩形 mask(fgui 滚动列表等):scissor 包住整个节点绘制。MReelsMask 由多个分栏
        // Graphics 组成，按 owner 的直接子树选择对应列，不能合并成一个大矩形。
        int maskIdx = (int)s[MASKI];
        bool scissored = false, alphaMasked = false, maskGeometry = false;
        std::vector<SRect> unionMaskRects;
        if (maskIdx > 0 && maskIdx < NODE_CAP && sg_->nodes[maskIdx].alive) {
            float* m = sg_->buf + maskIdx*STRIDE;
            Node& mn = sg_->nodes[maskIdx];
            int maskFlags = (int)m[HITFLAGS];
            bool alphaMask = (maskFlags & 8) != 0;
            bool ellipseMask = (maskFlags & 16) != 0;
            maskGeometry = isGeometryMask(maskIdx, m, maskFlags);
            // Prefer exact stencil for rotated/skewed rectangles and arbitrary geometry.
            // Keep multi-column reel masks on the per-column scissor path when they are still AABB.
            bool rotatedOrSkewed = !preflightRectMask(m);
            bool stencilMask = alphaMask || ellipseMask || maskGeometry || rotatedOrSkewed;
            if (stencilMask) {
                // Prefer explicit geometry quads; otherwise fall back to the solid sprite/rect fill.
                bool forceGeometry = maskGeometry || hasMaskGeometry(maskIdx, m);
                alphaMasked = pushAlphaMask(maskIdx, forceGeometry);
                if (!alphaMasked && !forceGeometry) alphaMasked = pushAlphaMask(maskIdx, false);
                if (!alphaMasked && hasMaskGeometry(maskIdx, m)) alphaMasked = pushAlphaMask(maskIdx, true);
                if (!alphaMasked) { reportMaskEscape(idx,maskIdx,"stencil-shape");escMask++; doEscape(idx); return; }
            } else if (mn.children.size()>1) for (size_t mi=0;mi<mn.children.size();++mi) {
                int mc=mn.children[mi]; SRect mr;
                if (mc>0 && mc<NODE_CAP && sg_->nodes[mc].alive && maskRect(mc,mr)) unionMaskRects.push_back(mr);
            }
            if (!stencilMask && unionMaskRects.size()<=1) {
                SRect mr;
                if (maskRect(maskIdx,mr)) {
                    scissored=true;
                    if (!pushScissor(mr.x0,mr.y0,mr.x1,mr.y1)) { popScissor(); return; }
                }
                else if (hasMaskGeometry(maskIdx, m) || preflightTexture((int)m[TEXID])) {
                    // Last-chance stencil instead of permanently skipping the masked subtree.
                    alphaMasked = pushAlphaMask(maskIdx, hasMaskGeometry(maskIdx, m));
                    if (!alphaMasked) { reportMaskEscape(idx,maskIdx,"non-rect");escMask++; doEscape(idx); return; }
                } else { reportMaskEscape(idx,maskIdx,"non-rect");escMask++; doEscape(idx); return; }
                unionMaskRects.clear();
            }
        }
        if(hasFilter&&(fd[29]>0.5f||fd[40]>0.5f||fd[46]>0.5f||fd[47]>0.5f||fd[61]>0.5f||fd[78]>0.5f)){static const bool postDiag=getenv("GAME_POST_DIAG")!=nullptr;static int postDiagN=0;if(postDiag&&postDiagN++<16){const char* kind=fd[29]>0.5f?"DropShadow":(fd[78]>0.5f?"GaussianBlur":(fd[61]>0.5f?"KawaseBlur":(fd[47]>0.5f?"Displacement":(fd[46]>0.5f?"FXAA":"Twist"))));int passes=fd[29]>0.5f?(int)fd[84]:(fd[78]>0.5f?(int)fd[81]*((fd[79]>0)+(fd[80]>0)):(int)fd[62]);fprintf(stderr,"[post-diag] node=%d depth=%d skip=%d kind=%s radius=%.1f angle=%.2f map=%d passes=%d\n",idx,postDepth_,twistSkipNode_,kind,fd[41],fd[42],(int)fd[48],passes);}}
        if(hasFilter&&(fd[29]>0.5f||fd[40]>0.5f||fd[46]>0.5f||fd[47]>0.5f||fd[61]>0.5f||fd[78]>0.5f)&&twistSkipNode_!=idx){
            if(!renderPost(idx,fd,parentFilterAlpha,parentBlurX,parentBlurY,parentCM,parentCMOn,fromSpineMarker,parentOverlay,parentOverlayOn,shadowPass,parentNoise,parentNoiseSeed)){escType++;doEscape(idx);}
            if(scissored)popScissor();if(alphaMasked)popAlphaMask(maskIdx, maskGeometry);return;
        }
        if (type == T_ESCAPE) { escType++; doEscape(idx); if (scissored) popScissor(); if(alphaMasked)popAlphaMask(maskIdx, maskGeometry); return; }
        // Union (multi-column) mask mode clips only children per-column below; the owner's OWN
        // primitives would escape the mask entirely. Confine them to the union's bounding box —
        // not exact (gaps between columns leak) but strictly better, and reel containers today
        // have no own content so现网行为不变。
        bool ownScissored = false, ownClipVisible = true;
        if (!unionMaskRects.empty()) {
            bool ownContent = (type==T_SPRITE && (int)s[TEXID]>0)
                || (type==T_MULTIQUAD && (int)s[QUADCNT]>0)
                || (type==T_NATIVE_MULTIQUAD && !sg_->nodes[idx].nativeQuads.empty());
            if (ownContent) {
                SRect u = unionMaskRects[0];
                for (size_t ri=1;ri<unionMaskRects.size();++ri){const SRect& r=unionMaskRects[ri];u.x0=fminf(u.x0,r.x0);u.y0=fminf(u.y0,r.y0);u.x1=fmaxf(u.x1,r.x1);u.y1=fmaxf(u.y1,r.y1);}
                ownClipVisible=pushScissor(u.x0,u.y0,u.x1,u.y1); ownScissored = true;
            }
        }
        // 节点级顶点缓存门控(不缓存清单见缓存块头注释)。recordingActive 已含 stencil/maskWrite/
        // post/stream 排除;门控对存与取一致,缓存里永远只有默认 filter/mask 上下文的
        // 顶点流。主屏 output transform/alpha 会烘进顶点,在缓存项里逐位精确比对；窗口尺寸、
        // BackingScale 或 RT 输出变化时保守 miss 并重建。
        const bool vcCacheable = vcMode() != 0 && recordingActive()
            && !fromSpineMarker && !shadowPass && !activeShadowOn_
            && nodeFilterAlpha == 1.0f && nodeBlurX == 0.0f && nodeBlurY == 0.0f
            && !nodeCMOn && !nodeOverlayOn && nodeNoise == 0.0f;
        if (ownClipVisible && !vcCacheable && (type == T_SPRITE || type == T_MULTIQUAD || type == T_NATIVE_MULTIQUAD)) vcSkip_++;
        if (ownClipVisible && type == T_SPRITE) {
            int texId = (int)s[TEXID];
            GLuint gl = texId>0 && texLookup ? texLookup(texId) : 0;
            if (gl) {
                if (!(vcCacheable && vcTryHit(idx))) {
                    if(texId>=8000000&&std::getenv("NATIVE_TEXT_NODE_TRACE")){static std::unordered_set<int> tracedTextNodes;float worldScale=(std::sqrt(s[WA]*s[WA]+s[WB]*s[WB])+std::sqrt(s[WC]*s[WC]+s[WD]*s[WD]))*.5f;if(worldScale>.5f&&tracedTextNodes.size()<2048&&tracedTextNodes.insert(idx).second)std::fprintf(stderr,"[native-text-node] node=%d parent=%d tex=%d rect=%.1f,%.1f %.1fx%.1f local=%.1f,%.1f scale=%.3f,%.3f world=%.3f,%.3f,%.3f,%.3f at=%.1f,%.1f alpha=%.2f\n",idx,sg_->nodes[idx].parent,texId,s[RX],s[RY],s[RW],s[RH],s[X],s[Y],s[SX],s[SY],s[WA],s[WB],s[WC],s[WD],s[WTX],s[WTY],s[WALPHA]);}
                    VcHarvest vh; const bool vcStoring = vcCacheable && vcStoreBegin(vh);
                    emitQuad(s, gl);
                    if (vcStoring) vcStoreEnd(idx, vh);
                }
            }
            else { reportTextureEscape(idx,texId,-1);escTex++; }   // 无纹理 Sprite:跳过自身 quad,但下方仍递归子节点(TaggedText 等"无纹理 Sprite + 有子节点"的富文本;JS __wrTex 已触发上传)
        }
        else if (ownClipVisible && type == T_MULTIQUAD) {   // M4:一节点多 quad(9-slice/BitmapText/Graphics),数据在 quad buffer
            if (!(vcCacheable && vcTryHit(idx))) {
                VcHarvest vh; bool vcStoring = vcCacheable && vcStoreBegin(vh);
                int off = (int)s[QUADOFF], cnt = (int)s[QUADCNT];
                const float* qb = sg_->buf + QUAD_BASE + 1;
                for (int q = 0; q < cnt; q++) {
                    const float* mq = qb + (size_t)(off + q) * QUAD_STRIDE_F;
                    if (mq[0] < 0.0f) {
                        vcStoring = false;   // spine drawOrder 标记:子树绘制穿插其间,节点不可整段缓存
                        int child = (int)(-mq[0]);
                        if (child>0 && child<NODE_CAP && sg_->nodes[child].alive && sg_->nodes[child].parent==idx)
                            walk(child,nodeFilterAlpha,nodeBlurX,nodeBlurY,nodeCMOn?nodeCM:nullptr,nodeCMOn,true,nodeOverlayOn?nodeOverlay:nullptr,nodeOverlayOn,shadowPass,nodeNoise,nodeNoiseSeed);
                    } else emitMeshQuad(s, mq, idx, q);
                }
                if (vcStoring) vcStoreEnd(idx, vh);
            }
        }
        else if (ownClipVisible && type == T_NATIVE_MULTIQUAD) {
            if (!(vcCacheable && vcTryHit(idx))) {
                VcHarvest vh; bool vcStoring = vcCacheable && vcStoreBegin(vh);
                const auto& qs=sg_->nodes[idx].nativeQuads;
                for(size_t q=0;q<qs.size();q++){
                    const float* mq=qs[q].data();
                    if (mq[0] < 0.0f) {
                        vcStoring = false;   // spinex slotObject drawOrder 标记:子树绘制穿插其间,节点不可整段缓存(与 T_MULTIQUAD 同约定)
                        int child = (int)(-mq[0]);
                        if (child>0 && child<NODE_CAP && sg_->nodes[child].alive && sg_->nodes[child].parent==idx)
                            walk(child,nodeFilterAlpha,nodeBlurX,nodeBlurY,nodeCMOn?nodeCM:nullptr,nodeCMOn,true,nodeOverlayOn?nodeOverlay:nullptr,nodeOverlayOn,shadowPass,nodeNoise,nodeNoiseSeed);
                    } else emitMeshQuad(s,mq,idx,(int)q);
                }
                if (vcStoring) vcStoreEnd(idx, vh);
            }
        }
        if (ownScissored) popScissor();
        Node& n = sg_->nodes[idx];
        for (size_t i=0;i<n.children.size();++i) {
            int c = n.children[i];
            if (c>0 && sg_->nodes[c].alive) {
                if (!unionMaskRects.empty()) {
                    float* cs=sg_->buf+c*STRIDE;
                    // Select a union branch from a point inside the child subtree. Container
                    // origins commonly sit exactly on a reel-column seam. Treating that origin
                    // as the witness makes both adjacent rectangles distance zero, and the first
                    // (left) rectangle wins; a nested per-cell mask in the right column then
                    // intersects it to zero width. Prefer the child's own clip center, then its
                    // render-subtree center. Both represent visible area rather than an arbitrary
                    // transform origin and work for any segmented rectangular mask.
                    float cx=cs[WTX];
                    SRect childClip={0,0,0,0};
                    int childMask=(int)cs[MASKI];
                    bool hasChildClip=childMask>0&&childMask<NODE_CAP&&sg_->nodes[childMask].alive&&maskRect(childMask,childClip);
                    const float* cb=sg_->buf+BOUNDS_BASE+(size_t)c*BOUNDS_STRIDE_F+4;
                    bool hasRenderBounds=cb[0]<=cb[2]&&cb[1]<=cb[3];
                    float boundsX0=cb[0],boundsX1=cb[2];
                    if(hasRenderBounds&&outputTransformOn_){
                        float boundsCenterX=(cb[0]+cb[2])*0.5f,boundsCenterY=(cb[1]+cb[3])*0.5f;
                        mapOutput(boundsCenterX,boundsCenterY);boundsX0=boundsX1=boundsCenterX;
                    }
                    if(!hasChildClip&&!hasRenderBounds&&(int)cs[TYPE]==T_SPRITE)cx=cs[WA]*(cs[RX]+cs[RW]*0.5f)+cs[WTX];
                    cx=union_mask_branch::witnessX(cx,hasChildClip,childClip.x0,childClip.x1,hasRenderBounds,boundsX0,boundsX1);
                    size_t best=union_mask_branch::nearestBranch(unionMaskRects,cx);
                    const SRect& r=unionMaskRects[best];bool branchVisible=pushScissor(r.x0,r.y0,r.x1,r.y1);
                    if(branchVisible)walk(c,nodeFilterAlpha,nodeBlurX,nodeBlurY,nodeCMOn?nodeCM:nullptr,nodeCMOn,false,nodeOverlayOn?nodeOverlay:nullptr,nodeOverlayOn,shadowPass,nodeNoise,nodeNoiseSeed);popScissor();
                } else walk(c,nodeFilterAlpha,nodeBlurX,nodeBlurY,nodeCMOn?nodeCM:nullptr,nodeCMOn,false,nodeOverlayOn?nodeOverlay:nullptr,nodeOverlayOn,shadowPass,nodeNoise,nodeNoiseSeed);
            }
        }
        if (scissored) popScissor();
        if (alphaMasked) popAlphaMask(maskIdx, maskGeometry);
    }

    void doEscape(int idx) {
        flush();
        escapes++;
        (void)idx;
    }

    void reportMaskEscape(int node,int mask,const char* reason){
        static const bool diag=getenv("GAME_MASK_DIAG")!=nullptr;static int left=32;
        if(!diag||left--<=0||!sg_||mask<=0||mask>=NODE_CAP)return;
        const float* m=sg_->buf+(size_t)mask*STRIDE;const Node& mn=sg_->nodes[mask];
        fprintf(stderr,"[mask-escape] node=%d mask=%d reason=%s type=%d flags=%d rect=%.1f,%.1f %.1fx%.1f q=%d nativeQ=%zu children=%zu world=%.3f,%.3f,%.3f,%.3f\n",
            node,mask,reason,(int)m[TYPE],(int)m[HITFLAGS],m[RX],m[RY],m[RW],m[RH],(int)m[QUADCNT],mn.nativeQuads.size(),mn.children.size(),m[WA],m[WB],m[WC],m[WD]);
    }

    void reportTextureEscape(int node,int texture,int quad){
        static const bool diag=getenv("GAME_TEX_ESCAPE_DIAG")!=nullptr;static int left=64;
        if(!diag||left--<=0||!sg_||node<=0||node>=NODE_CAP)return;
        const float* s=sg_->buf+(size_t)node*STRIDE;const Node& n=sg_->nodes[node];
        fprintf(stderr,"[tex-escape] node=%d parent=%d type=%d texture=%d quad=%d qCount=%d nativeQ=%zu rect=%.1f,%.1f %.1fx%.1f alpha=%.3f world=%.1f,%.1f\n",
            node,n.parent,(int)s[TYPE],texture,quad,(int)s[QUADCNT],n.nativeQuads.size(),s[RX],s[RY],s[RW],s[RH],s[WALPHA],s[WTX],s[WTY]);
    }

    std::vector<SRect> scissors_;
    float screenH_ = 0;
    int stencilDepth_ = 0;
    std::vector<uint8_t> maskGeomStack_;   // per-depth: mode pushAlphaMask actually used (1=geometry)
    bool cullEnabled_ = true;
    float cullPad_ = 64.0f;
    float cullX0_ = 0, cullY0_ = 0, cullX1_ = 0, cullY1_ = 0;

    // true = entirely outside the padded viewport (safe to skip the subtree).
    // Empty / invalid bounds always keep the node so build-time zero-size nodes still paint.
    bool outsideViewport(int idx) const {
        if (!sg_ || !sg_->buf || idx <= 0 || idx >= NODE_CAP) return false;
        // Filtered nodes paint outside their geometry AABB (drop-shadow offset, blur halo can
        // exceed the fixed 64px pad and would pop in/out at the cull edge). They are rare per
        // scene, so never cull them instead of modeling per-filter padding here.
        // HAS_FILTER 位 0 → 无 filter,免读 fdc 的 2 条远端 cache line。
        if (!filterFlagEnabled() || sg_->buf[FILTER_FLAG_BASE + idx] != 0.0f) {
            const float* fdc = sg_->buf + FILTER_BASE + (size_t)idx*FILTER_STRIDE_F;
            if (fdc[29]>0.5f||fdc[40]>0.5f||fdc[46]>0.5f||fdc[47]>0.5f||fdc[61]>0.5f||fdc[78]>0.5f||fdc[1]>0.0f||fdc[2]>0.0f) return false;
        }
        const float* d = sg_->buf + BOUNDS_BASE + (size_t)idx * BOUNDS_STRIDE_F + 4; // render-subtree AABB
        float x0 = d[0], y0 = d[1], x1 = d[2], y1 = d[3];
        if (x0 > x1 || y0 > y1) return false;                // empty → keep (may be growing)
        if (outputTransformOn_) {
            float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
            float rx0 = 1e30f, ry0 = 1e30f, rx1 = -1e30f, ry1 = -1e30f;
            for (int k = 0; k < 4; k++) {
                float ox = outputTransform_[0] * xs[k] + outputTransform_[2] * ys[k] + outputTransform_[4];
                float oy = outputTransform_[1] * xs[k] + outputTransform_[3] * ys[k] + outputTransform_[5];
                rx0 = fminf(rx0, ox); ry0 = fminf(ry0, oy); rx1 = fmaxf(rx1, ox); ry1 = fmaxf(ry1, oy);
            }
            x0 = rx0; y0 = ry0; x1 = rx1; y1 = ry1;
        }
        return (x1 < cullX0_ || x0 > cullX1_ || y1 < cullY0_ || y0 > cullY1_);
    }

    bool emitMaskShape(float* m, bool geometryMask, GLuint gl) {
        int node=(int)(m-sg_->buf)/STRIDE;
        if (geometryMask) {
            if(node>0&&node<NODE_CAP&&!sg_->nodes[node].nativeQuads.empty()){
                bool emitted=false;
                for(const auto& q:sg_->nodes[node].nativeQuads){
                    GLuint qgl=((int)q[0])>0&&texLookup?texLookup((int)q[0]):0;
                    if(!qgl && texLookup) qgl=texLookup(7999999);
                    if(!qgl)continue;
                    emitMeshQuad(m,q.data());emitted=true;
                }
                if(emitted)return true;
            }
            int off=(int)m[QUADOFF],cnt=(int)m[QUADCNT];
            if(off>=0&&cnt>0){
                const float* qb=sg_->buf+QUAD_BASE+1;
                bool emitted=false;
                for(int q=0;q<cnt;q++){
                    const float* mq=qb+(size_t)(off+q)*QUAD_STRIDE_F;
                    if(mq[0]<0.0f)continue;
                    GLuint qgl=((int)mq[0])>0&&texLookup?texLookup((int)mq[0]):0;
                    if(!qgl && texLookup) qgl=texLookup(7999999);
                    if(!qgl)continue;
                    emitMeshQuad(m,mq);emitted=true;
                }
                if(emitted)return true;
            }
        }
        // Sprite / solid-rect fallback: works for rotated rectangles once the world matrix is applied.
        if(!gl && texLookup) gl=texLookup(((int)m[TEXID])>0?(int)m[TEXID]:7999999);
        if(!gl) return false;
        emitQuad(m,gl);
        return true;
    }

    bool pushAlphaMask(int idx, bool forceGeometry=false) {
        float* m=sg_->buf+idx*STRIDE;
        int flags=(int)m[HITFLAGS];
        bool geometryMask=forceGeometry||(flags&64)!=0||hasMaskGeometry(idx,m);
        GLuint gl=0;
        if(!geometryMask){
            gl=((int)m[TEXID])>0&&texLookup?texLookup((int)m[TEXID]):0;
            // Solid rotated rectangles often keep a zero TEXID until the first sprite refresh.
            // A 1x1 white texture (cid registered by the host) is enough to write stencil coverage.
            if(!gl && m[RW]>=0.0f && m[RH]>=0.0f && texLookup) gl=texLookup(7999999);
        }
        if((geometryMask&&!hasMaskGeometry(idx,m)&&((int)m[QUADCNT]<=0))||(!geometryMask&&!gl))return false;
        // stencil clear/写形状即将改动裁剪缓冲,且 mask 区间内的内容走即时绘制:
        // 之前录制的批必须先画出,否则帧末回放会盖到 mask 内容之上(z 序错乱)。
        // 此后 stencilDepth_>0 / curMaskWrite_!=0 期间 recordingActive() 为假,全程旧即时路径。
        replayPending();
        if(stencilDepth_==0){
            glDisable(GL_SCISSOR_TEST);
            glStencilMask(0xFF);glClearStencil(0);glClear(GL_STENCIL_BUFFER_BIT);
            applyScissor();glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS,1,0xFF);glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);
        }else{
            glStencilMask(0xFF);glStencilFunc(GL_EQUAL,stencilDepth_,0xFF);glStencilOp(GL_KEEP,GL_KEEP,GL_INCR);
        }
        // mode 0 = solid coverage (geometry / opaque rect), 1 = alpha texture, 2 = ellipse.
        int mode=geometryMask?0:((flags&16)?2:1);
        float blurX=activeBlurX_,blurY=activeBlurY_;activeBlurX_=activeBlurY_=0.0f;
        maskShapeEmit_=true;   // stencil 形状写恒即时(geometry mode=0 时 curMaskWrite_/stencilDepth_ 都判不住)
        glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);setMaskWrite(mode);bool emitted=emitMaskShape(m,geometryMask,gl);flush();setMaskWrite(0);
        maskShapeEmit_=false;
        activeBlurX_=blurX;activeBlurY_=blurY;
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        if(!emitted){
            if(stencilDepth_==0){glStencilMask(0xFF);glDisable(GL_STENCIL_TEST);}
            else{glStencilMask(0x00);glStencilFunc(GL_EQUAL,stencilDepth_,0xFF);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);}
            return false;
        }
        ++stencilDepth_;maskGeomStack_.push_back(geometryMask?1:0);
        glStencilMask(0x00);glStencilFunc(GL_EQUAL,stencilDepth_,0xFF);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);
        return true;
    }

    void popAlphaMask(int idx, bool forceGeometry=false) {
        flush();
        if(stencilDepth_<=1){stencilDepth_=0;maskGeomStack_.clear();glStencilMask(0xFF);glDisable(GL_STENCIL_TEST);return;}
        float* m=sg_->buf+idx*STRIDE;
        int flags=(int)m[HITFLAGS];
        // Replay exactly the mode push used (push may promote to geometry via hasMaskGeometry,
        // which the flag test alone misses); recomputing here desyncs INCR/DECR and leaves
        // stencil residue that clips outer masked content.
        bool geometryMask=forceGeometry||(flags&64)!=0||hasMaskGeometry(idx,m);
        if(!maskGeomStack_.empty()){geometryMask=maskGeomStack_.back()!=0;maskGeomStack_.pop_back();}
        GLuint gl=geometryMask?0:(((int)m[TEXID])>0&&texLookup?texLookup((int)m[TEXID]):0);
        // Same white-texture fallback as pushAlphaMask: rotated rects with TEXID=0 still must erase.
        if(!geometryMask&&!gl&&m[RW]>=0.0f&&m[RH]>=0.0f&&texLookup)gl=texLookup(7999999);
        if(geometryMask||gl){
            glStencilMask(0xFF);glStencilFunc(GL_EQUAL,stencilDepth_,0xFF);glStencilOp(GL_KEEP,GL_KEEP,GL_DECR);
            int mode=geometryMask?0:((flags&16)?2:1);
            float blurX=activeBlurX_,blurY=activeBlurY_;activeBlurX_=activeBlurY_=0.0f;
            maskShapeEmit_=true;   // 同 pushAlphaMask:擦除写也必须即时(此处 stencilDepth_>0 本已即时,防御性一致)
            glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);setMaskWrite(mode);emitMaskShape(m,geometryMask,gl);flush();setMaskWrite(0);
            maskShapeEmit_=false;
            activeBlurX_=blurX;activeBlurY_=blurY;
            glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        }
        --stencilDepth_;glStencilMask(0x00);glStencilFunc(GL_EQUAL,stencilDepth_,0xFF);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);
    }

    bool maskRect(int idx,SRect& r) {
        float* m=sg_->buf+idx*STRIDE;
        if(fabsf(m[WB])>=0.01f||fabsf(m[WC])>=0.01f||m[RW]<0.0f||m[RH]<0.0f)return false;
        r.x0=m[WA]*m[RX]+m[WTX];r.y0=m[WD]*m[RY]+m[WTY];
        r.x1=r.x0+m[WA]*m[RW];r.y1=r.y0+m[WD]*m[RH];
        if(r.x1<r.x0)std::swap(r.x0,r.x1);if(r.y1<r.y0)std::swap(r.y0,r.y1);
        if(outputTransformOn_){float x[4]={r.x0,r.x1,r.x1,r.x0},y[4]={r.y0,r.y0,r.y1,r.y1};for(int i=0;i<4;i++)mapOutput(x[i],y[i]);r={x[0],y[0],x[0],y[0]};for(int i=1;i<4;i++){r.x0=fminf(r.x0,x[i]);r.y0=fminf(r.y0,y[i]);r.x1=fmaxf(r.x1,x[i]);r.y1=fmaxf(r.y1,y[i]);}}
        return true;
    }

    bool pushScissor(float x0, float y0, float x1, float y1) {
        flush();
        SRect r = {x0,y0,x1,y1};
        if (!scissors_.empty()) {                       // 嵌套 mask = 交集(同 pixi scissor 语义)
            SRect& p = scissors_.back();
            if (r.x0 < p.x0) r.x0 = p.x0;  if (r.y0 < p.y0) r.y0 = p.y0;
            if (r.x1 > p.x1) r.x1 = p.x1;  if (r.y1 > p.y1) r.y1 = p.y1;
        }
        const bool visible=scissor_bounds::hasArea(r.x0,r.y0,r.x1,r.y1);
        scissors_.push_back(r);
        if(visible)applyScissor();
        return visible;
    }
    void popScissor() { flush(); if (!scissors_.empty()) scissors_.pop_back(); applyScissor(); }
    void scissorPixels(const SRect& r,GLint* out) const {
        const float sx=targetLogicalW_>0?targetViewport_[2]/targetLogicalW_:1;
        const float sy=targetLogicalH_>0?targetViewport_[3]/targetLogicalH_:1;
        const auto px=scissor_bounds::enclosing(r.x0*sx,r.y0*sy,r.x1*sx,r.y1*sy);
        out[0]=targetViewport_[0]+(GLint)px.x0;
        out[1]=targetViewport_[1]+targetViewport_[3]-(GLint)px.y1;
        out[2]=(GLint)px.width();out[3]=(GLint)px.height();
    }
    void applyScissor() {
        if (scissors_.empty()) { glDisable(GL_SCISSOR_TEST); return; }
        SRect& r = scissors_.back();
        // A transformed Pixi rectangle normally lands on fractional device pixels. Truncating
        // both origin and size can drop the right/bottom edge, most visibly when progress reaches
        // 100%. Expand positive-area coverage to the enclosing integer rectangle, while preserving
        // Pixi's empty-mask contract instead of inflating a fractional zero extent to one pixel.
        GLint px[4];scissorPixels(r,px);
        glEnable(GL_SCISSOR_TEST);
        glScissor(px[0],px[1],px[2],px[3]);   // GL 原点左下 → y 翻转
    }

    void emitQuad(float* s, GLuint gl) {
        if (streamBase_ + quadCount_ + 1 > MAX_QUADS) wrapRing();   // 环形回绕:已录批先画出,从头 orphan
        int blend = (int)s[BLEND], premul = (int)s[PREMUL];
        if (gl != curTex_ || blend != curBlend_ || premul != curPremul_ || curTwoColor_ != 0 || curDistanceField_ != 0 || curTiling_ != 0) { flush(); curTex_ = gl; setBlend(blend); setPremul(premul); setTwoColor(0);setDistanceField(0,0);setTiling(nullptr); }   // 精灵永远使用通用取样
        curTexId_ = (int)s[TEXID];   // gl 同值合批时也刷新 id 影子(顶点缓存段收集用;mask 白纹理路径会短暂失真,该路径不收集)
        setColorMatrix(activeCMOn_,activeCMOn_?activeCM_:nullptr);
        setOverlay(activeOverlayOn_,activeOverlayOn_?activeOverlay_:nullptr);
        setShadow(activeShadowOn_,activeShadowOn_?activeShadow_:nullptr);
        setNoise(activeNoise_,activeNoiseSeed_);
        float x=s[RX], y=s[RY], w=s[RW], h=s[RH];
        float a=s[WA], b=s[WB], c=s[WC], d=s[WD], tx=s[WTX], ty=s[WTY];
        float wal = s[WALPHA]*outputAlphaScale_*activeFilterAlpha_; if (wal>1.0f) wal=1.0f;
        uint32_t tint = (uint32_t)s[TINT];
        // 四角:左上(x,y) 右上(x+w,y) 右下(x+w,y+h) 左下(x,y+h) —— 与 pixi uvs x0..x3 顺序一致
        float xs[4] = {x, x+w, x+w, x};
        float ys[4] = {y, y, y+h, y+h};
        const float* uv = s + U0;
        auto append=[&](float dx,float dy,float alphaScale){
            if(streamBase_+quadCount_+1>MAX_QUADS)wrapRing();
            float al=wal*alphaScale;if(al>1.0f)al=1.0f;
            uint32_t A=(uint32_t)(al*255.0f+0.5f),R=(uint32_t)(((tint>>16)&255)*al+0.5f),G=(uint32_t)(((tint>>8)&255)*al+0.5f),B=(uint32_t)((tint&255)*al+0.5f),rgba=(A<<24)|(B<<16)|(G<<8)|R;
            float pc;memcpy(&pc,&rgba,4);
            for(int i=0;i<4;i++){float px=xs[i],py=ys[i],vx=a*px+c*py+tx+dx+(activeShadowOn_?activeShadow_[0]:0),vy=b*px+d*py+ty+dy+(activeShadowOn_?activeShadow_[1]:0);mapOutput(vx,vy);verts_.push_back(vx);verts_.push_back(vy);verts_.push_back(uv[i*2]);verts_.push_back(uv[i*2+1]);verts_.push_back(pc);}
            if(++quadCount_>=MAX_QUADS)flush();
        };
        float dx=fminf(activeBlurX_*0.18f,16.0f),dy=fminf(activeBlurY_*0.18f,16.0f);
        if(dx>0.5f){append(-dx,0,0.12f);append(dx,0,0.12f);} if(dy>0.5f){append(0,-dy,0.12f);append(0,dy,0.12f);} append(0,0,1.0f);
    }

    // M4/M5:多-quad 节点的单 quad —— 4 任意角(局部/骨骼空间)+ uv/tint/texid 取自 quad buffer(qd),
    // world/blend/alpha 取自节点 slot(s)。qd 布局:[0]texid [1..8]4角(x0y0..x3y3) [9..16]4uv [17]tint。
    void emitMeshQuad(const float* s, const float* qd, int node=-1, int quad=-1) {
        GLuint gl = ((int)qd[0]) > 0 && texLookup ? texLookup((int)qd[0]) : 0;
        if (!gl) { reportTextureEscape(node,(int)qd[0],quad);escTex++; return; }
        if (streamBase_ + quadCount_ + 1 > MAX_QUADS) wrapRing();
        int qbl = (int)qd[19]; int blend = (qbl < 0) ? (int)s[BLEND] : qbl;   // qd[19]<0=用节点 blend;spine 传 per-slot blend(0-3)
        int premul = (int)qd[22];   // per-quad premul(multi-page BitmapText / multi-atlas Spine / Graphics 纹理填充 可混 alphaMode)
        float darkP = qd[20]; int twoColor = (darkP >= 0.0f) ? 1 : 0;   // qd[20]<0=非双色;>=0=twoColor dark(0xRRGGBB)
        int distanceField = darkP < -1.5f ? 1 : 0;
        int tiling = (int)qd[23];
        float worldScale=(sqrtf(s[WA]*s[WA]+s[WB]*s[WB])+sqrtf(s[WC]*s[WC]+s[WD]*s[WD]))*0.5f;
        float fwidth=distanceField?std::max(0.0001f,worldScale*qd[21]):0.0f;
        uint32_t darkRGB = twoColor ? (uint32_t)darkP : 0; int darkA = twoColor ? (int)(qd[21]*255.0f) : -1;
        bool darkChg = twoColor && (darkRGB != curDark_ || darkA != curDarkA_);
        bool distanceChg=distanceField!=curDistanceField_||(distanceField&&fabsf(fwidth-curFWidth_)>0.0001f);
        bool tilingChg=tiling!=curTiling_;if(tiling&&!tilingChg)for(int i=0;i<8;i++)if(fabsf(curTileUV_[i]-qd[24+i])>0.000001f){tilingChg=true;break;}
        if (gl != curTex_ || blend != curBlend_ || premul != curPremul_ || twoColor != curTwoColor_ || darkChg || distanceChg || tilingChg) {
            flush(); curTex_ = gl; setBlend(blend); setPremul(premul); setTwoColor(twoColor); if (twoColor) setDark(darkRGB, qd[21]);setDistanceField(distanceField,fwidth);setTiling(tiling?qd+24:nullptr,tiling);
        }
        curTexId_ = (int)qd[0];
        setColorMatrix(activeCMOn_,activeCMOn_?activeCM_:nullptr);
        setOverlay(activeOverlayOn_,activeOverlayOn_?activeOverlay_:nullptr);
        setShadow(activeShadowOn_,activeShadowOn_?activeShadow_:nullptr);
        setNoise(activeNoise_,activeNoiseSeed_);
        float a=s[WA], b=s[WB], c=s[WC], d=s[WD], tx=s[WTX], ty=s[WTY];
        float wal=s[WALPHA]*outputAlphaScale_*qd[18]*activeFilterAlpha_; if (wal>1.0f) wal=1.0f; if (wal<0.0f) wal=0.0f;   // qd[18]=per-slot alpha 乘节点 alpha
        uint32_t tint = (uint32_t)qd[17];
        const float* uv = qd + 9;
        auto append=[&](float dx,float dy,float alphaScale){
            if(streamBase_+quadCount_+1>MAX_QUADS)wrapRing();
            float al=wal*alphaScale;if(al>1.0f)al=1.0f;
            uint32_t A=(uint32_t)(al*255.0f+0.5f),R=(uint32_t)(((tint>>16)&255)*al+0.5f),G=(uint32_t)(((tint>>8)&255)*al+0.5f),Bc=(uint32_t)((tint&255)*al+0.5f),rgba=(A<<24)|(Bc<<16)|(G<<8)|R;
            float pc;memcpy(&pc,&rgba,4);
            for(int i=0;i<4;i++){float px=qd[1+i*2],py=qd[2+i*2],vx=a*px+c*py+tx+dx+(activeShadowOn_?activeShadow_[0]:0),vy=b*px+d*py+ty+dy+(activeShadowOn_?activeShadow_[1]:0);mapOutput(vx,vy);verts_.push_back(vx);verts_.push_back(vy);verts_.push_back(uv[i*2]);verts_.push_back(uv[i*2+1]);verts_.push_back(pc);}
            if(++quadCount_>=MAX_QUADS)flush();
        };
        float dx=fminf(activeBlurX_*0.18f,16.0f),dy=fminf(activeBlurY_*0.18f,16.0f);
        if(dx>0.5f){append(-dx,0,0.12f);append(dx,0,0.12f);} if(dy>0.5f){append(0,-dy,0.12f);append(0,dy,0.12f);} append(0,0,1.0f);
    }

    // ---- 批录制+单次上传(对标 Cocos Batcher2d 的 DrawBatch2D+uploadBuffers+UIPhase 回放) ----
    // flush() 在录制态只把当前段登记为 RecordedBatch(状态快照+槽位区间),顶点留在 verts_(帧级
    // 大缓冲);replayPending() 一次 map 上传整段 + 逐批经影子缓存 set 状态 + glDrawElements。
    // 保守策略:仅"纯普通 quad 流"录制;stencil mask 写/内容、renderPost 离屏捕获期间
    // recordingActive() 为假,flush 走 flushImmediate 旧路径,正确性与旧行为逐帧一致。
    // SG_BATCH_RECORD=0 整体退回旧即时路径。
    struct RecordedBatch {
        int quadStart=0, quadCount=0;    // VBO 环形槽区间(绝对 quad 槽;索引字节偏移=quadStart*6*2)
        GLuint tex=0;
        int texId=0;                     // client 纹理 id(顶点缓存段收集用;回放仍用 tex GLuint)
        int blend=0, premul=0, twoColor=0;
        uint32_t darkRGB=0; float darkA=0;
        int distanceField=0; float fwidth=0;
        int tiling=0; float tileUV[8]={0};
        bool cmOn=false; float cm[20]={0};          // 大字段仅 On 时快照,Off 时不读
        bool overlayOn=false; float overlay[4]={0};
        bool shadowOn=false; float shadow[4]={0};   // 色彩部分(curShadow_);dx/dy 已烘进顶点
        float noise=0, noiseSeed=0;
        int maskWrite=0;                 // 录制段恒 0(mask 写路径走即时);防御性保留
        bool scissorOn=false; GLint sc[4]={0,0,0,0};
    };
    std::vector<RecordedBatch> recorded_;
    int recBaseSlot_ = 0;                // 已录区起始槽 = 一次性上传 verts_ 的 VBO 偏移
    bool frameRecording_ = false;        // 仅 frame() 全屏路径置位;stream 段恒即时
    bool maskShapeEmit_ = false;         // push/popAlphaMask 的 stencil 形状写区间(恒即时,见 recordingActive)

    static bool recordFlagEnabled() {    // SG_BATCH_RECORD=0 退回旧逐批即时上传(A/B 对拍用)
        static const bool on = [](){ const char* e = getenv("SG_BATCH_RECORD"); return !(e && e[0]=='0'); }();
        return on;
    }
    // 录制仅当:frame 主路径 + 不在任何"必须立即出图"的区段内。stencil mask(形状写与被裁内容)
    // 依赖之前内容已上屏且 stencil 缓冲随后被改写;renderPost 捕获期间目标是离屏 FBO。
    // maskShapeEmit_:push/popAlphaMask 的形状写区间。geometry mask(mode=0)不改 curMaskWrite_
    // 且首层时 stencilDepth_ 仍为 0,仅靠上两者判不住——形状 quad 若被录走,帧末会以开ColorMask
    // 无stencil 的状态回放成可见矩形。该区间必须即时。
    bool recordingActive() const {
        return frameRecording_ && stencilDepth_==0 && curMaskWrite_==0 && postDepth_==0 && !maskShapeEmit_;
    }

    void flush() {
        if (!quadCount_) return;
        if (recordingActive()) { recordFlush(); return; }
        flushImmediate();
    }

    void ensureOrphan() {
        if (!orphan_) return;
        if (persistentVbo_) { orphan_ = false; return; }
        auto t=std::chrono::steady_clock::now();
        glBufferData(GL_ARRAY_BUFFER, MAX_QUADS*4*STRIDE_B, nullptr, GL_STREAM_DRAW);
        bufferDataMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
        orphan_ = false;
    }

    void uploadRange(GLintptr byteOffset, GLsizeiptr byteSize, const void* data) {
        ++uploadCalls;
        auto subT=std::chrono::steady_clock::now();
        if (persistentVbo_) {
            memcpy(static_cast<uint8_t*>(persistentVbo_) + persistentRegionBase() + byteOffset, data, (size_t)byteSize);
            persistentUsed_ = true;
            bufferSubMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-subT).count();
            return;
        }
#if TARGET_OS_IPHONE || defined(__ANDROID__)
        // On both iOS and Android, many small glBufferSubData calls can dominate a frame. The VBO
        // was orphaned at frame start and every batch writes a disjoint ring range, so an
        // unsynchronised invalidated mapping is safe and avoids the driver's extra copy/stall.
        void* mapped = glMapBufferRange(GL_ARRAY_BUFFER, byteOffset, byteSize,
            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (mapped) {
            memcpy(mapped, data, (size_t)byteSize);
            glUnmapBuffer(GL_ARRAY_BUFFER);
        } else {
            ++mapFallbacks;
            glBufferSubData(GL_ARRAY_BUFFER, byteOffset, byteSize, data);
        }
#else
        glBufferSubData(GL_ARRAY_BUFFER, byteOffset, byteSize, data);
#endif
        bufferSubMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-subT).count();
    }

    void flushImmediate() {   // 旧即时路径原样保留:SG_BATCH_RECORD=0 与 mask/post 即时区段
        ++immediateBatches;
        if (stencilDepth_ > 0) ++stencilBatches;
        if (postDepth_ > 0) ++postBatches;
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        ensureOrphan();
        uploadRange((GLintptr)((size_t)streamBase_*4*STRIDE_B), (GLsizeiptr)(verts_.size()*sizeof(float)), verts_.data());
        glBindTexture(GL_TEXTURE_2D, curTex_);
        auto drawT=std::chrono::steady_clock::now();
        glDrawElements(GL_TRIANGLES, quadCount_*6, GL_UNSIGNED_SHORT, (const void*)(intptr_t)((size_t)streamBase_*6*2));
        double oneDrawMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-drawT).count();
        if(oneDrawMs>drawMs){drawMs=oneDrawMs;slowBlend=curBlend_;slowPremul=curPremul_;slowTwoColor=curTwoColor_;slowMaskWrite=curMaskWrite_;}
        drawCalls++; quads += (unsigned)quadCount_;
        framePos_ = streamBase_ + quadCount_;
        streamBase_ = framePos_;
        recBaseSlot_ = streamBase_;   // 即时段推进后,(空的)已录区起点跟随,恢复录制时上传偏移对位
        quadCount_ = 0; verts_.clear(); vertsResetSeq_++;
    }

    void captureState(RecordedBatch& rb) const {   // 快照当前批状态(录制时 = 该段 quad 的生效状态)
        rb.tex=curTex_; rb.texId=curTexId_; rb.blend=curBlend_; rb.premul=curPremul_; rb.twoColor=curTwoColor_;
        if(rb.twoColor==1){rb.darkRGB=curDark_;rb.darkA=curDarkAF_;}
        rb.distanceField=curDistanceField_; if(rb.distanceField==1)rb.fwidth=curFWidth_;
        rb.tiling=curTiling_; if(rb.tiling>0)memcpy(rb.tileUV,curTileUV_,sizeof(rb.tileUV));
        rb.cmOn=curCMOn_; if(rb.cmOn)memcpy(rb.cm,curCM_,sizeof(rb.cm));
        rb.overlayOn=curOverlayOn_; if(rb.overlayOn)memcpy(rb.overlay,curOverlay_,sizeof(rb.overlay));
        rb.shadowOn=curShadowOn_; if(rb.shadowOn)memcpy(rb.shadow,curShadow_,sizeof(rb.shadow));
        rb.noise=curNoise_; rb.noiseSeed=curNoiseSeed_;
        rb.maskWrite=curMaskWrite_;
        if(!scissors_.empty()){   // 与 applyScissor 完全相同的取整,保证回放像素级一致
            rb.scissorOn=true;scissorPixels(scissors_.back(),rb.sc);
        }
    }

    // 经现有影子缓存 setter 应用一批的状态(与即时路径同一套去冗余;setter 内部 flush 此时
    // quadCount_==0 必为 no-op)。纹理与 scissor 由回放循环单独做局部影子过滤。
    void applyState(const RecordedBatch& rb) {
        setBlend(rb.blend); setPremul(rb.premul); setTwoColor(rb.twoColor);
        if (rb.twoColor==1 && (rb.darkRGB!=curDark_ || rb.darkA!=curDarkAF_)) setDark(rb.darkRGB, rb.darkA);
        if (rb.distanceField!=curDistanceField_ || (rb.distanceField==1 && fabsf(rb.fwidth-curFWidth_)>0.0001f))
            setDistanceField(rb.distanceField, rb.fwidth);
        bool tilingChg = rb.tiling!=curTiling_;
        if (rb.tiling>0 && !tilingChg) for (int i=0;i<8;i++) if (fabsf(curTileUV_[i]-rb.tileUV[i])>0.000001f) { tilingChg=true; break; }
        if (tilingChg) setTiling(rb.tiling>0?rb.tileUV:nullptr,rb.tiling);
        setColorMatrix(rb.cmOn, rb.cmOn?rb.cm:nullptr);
        setOverlay(rb.overlayOn, rb.overlayOn?rb.overlay:nullptr);
        if (rb.shadowOn) { float sv[6]={0,0,rb.shadow[0],rb.shadow[1],rb.shadow[2],rb.shadow[3]}; setShadow(true,sv); }
        else setShadow(false,nullptr);
        setNoise(rb.noise, rb.noiseSeed);
        setMaskWrite(rb.maskWrite);
    }

    void recordFlush() {   // 录制:当前段 → RecordedBatch;顶点留在 verts_,槽位照常前进
        ++recordedBatches;
        RecordedBatch rb;
        rb.quadStart = streamBase_; rb.quadCount = quadCount_;
        captureState(rb);
        recorded_.push_back(rb);
        quads += (unsigned)quadCount_;
        framePos_ = streamBase_ + quadCount_;
        streamBase_ = framePos_;
        quadCount_ = 0;   // verts_ 不清:帧级累积,replayPending 一次上传
    }

    // 把"已录未画"的批立即一次上传+回放,之后语义与逐批即时 flush 完全一致。任何依赖
    // "之前内容已画到当前目标"的操作前必须调用:stencil 写(pushAlphaMask)、FBO 切换/离屏
    // 捕获(renderPost)、环形槽位复用(wrapRing)、帧末 present。即时模式下等价于 flush()。
    void replayPending() {
        flush();   // 结转开放段:录制态追加为最后一批;即时态直接画出(此时列表必为空)
        if (recorded_.empty()) { recBaseSlot_ = streamBase_; return; }
        // 防御性重建核心绑定:renderPost/预热等路径退出时都已 setupState,这里兜底一次,
        // 保证 map 的目标 VBO、索引缓冲与 program 正确。attrib 指针由 setupState 维护。
        glUseProgram(prog_);
        glActiveTexture(GL_TEXTURE0);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
        ensureOrphan();
        uploadRange((GLintptr)((size_t)recBaseSlot_*4*STRIDE_B), (GLsizeiptr)(verts_.size()*sizeof(float)), verts_.data());
        verts_.clear(); vertsResetSeq_++;
        RecordedBatch resume; captureState(resume);   // 回放会改写影子状态,循环后恢复,
                                                      // 使 GL/影子仍与"当前开放流"一致(回绕中段续录依赖此)
        GLuint lastTex = 0xFFFFFFFFu;                 // 纹理影子过滤:仅与上一批不同才 bind
        int lastScOn = -1; GLint lastSc[4]={0,0,0,0};
        bool spriteActive=false;
        int spritePremul=-1,spriteTwoColor=-1;
        uint32_t spriteDark=0;float spriteDarkA=-1;
        for (const RecordedBatch& rb : recorded_) {
            const bool ordinary=specializeSprites && rb.distanceField==0 && rb.tiling==0 &&
                !rb.cmOn && !rb.overlayOn && !rb.shadowOn && rb.noise==0 && rb.maskWrite==0;
            if(ordinary){
                if(!spriteActive){glUseProgram(spriteProg_);glUniform4fv(spriteProj_,1,proj_);spriteActive=true;}
                // Blend/scissor/texture are context state. The other shadow values belong to the
                // full program and must stay untouched while its uniforms are not being written.
                setBlend(rb.blend);
                if(rb.premul!=spritePremul){glUniform1f(spritePremul_,(float)rb.premul);spritePremul=rb.premul;}
                if(rb.twoColor!=spriteTwoColor){glUniform1f(spriteTwoColor_,(float)rb.twoColor);spriteTwoColor=rb.twoColor;}
                if(rb.twoColor==1&&(rb.darkRGB!=spriteDark||rb.darkA!=spriteDarkA)){
                    glUniform4f(spriteDark_,((rb.darkRGB>>16)&255)/255.0f,((rb.darkRGB>>8)&255)/255.0f,(rb.darkRGB&255)/255.0f,rb.darkA);
                    spriteDark=rb.darkRGB;spriteDarkA=rb.darkA;
                }
            }else{
                if(spriteActive){glUseProgram(prog_);spriteActive=false;}
                applyState(rb);
            }
            int scOn = rb.scissorOn ? 1 : 0;
            if (scOn != lastScOn || (scOn && memcmp(lastSc, rb.sc, sizeof(lastSc)) != 0)) {
                if (scOn) { glEnable(GL_SCISSOR_TEST); glScissor(rb.sc[0], rb.sc[1], (GLsizei)rb.sc[2], (GLsizei)rb.sc[3]); memcpy(lastSc, rb.sc, sizeof(lastSc)); }
                else glDisable(GL_SCISSOR_TEST);
                lastScOn = scOn;
            }
            if (rb.tex != lastTex) { glBindTexture(GL_TEXTURE_2D, rb.tex); lastTex = rb.tex; }
            auto drawT=std::chrono::steady_clock::now();
            glDrawElements(GL_TRIANGLES, rb.quadCount*6, GL_UNSIGNED_SHORT, (const void*)(intptr_t)((size_t)rb.quadStart*6*2));
            double oneDrawMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-drawT).count();
            if(oneDrawMs>drawMs){drawMs=oneDrawMs;slowBlend=rb.blend;slowPremul=rb.premul;slowTwoColor=rb.twoColor;slowMaskWrite=rb.maskWrite;}
            drawCalls++;
        }
        recorded_.clear();
        recBaseSlot_ = streamBase_;
        if(spriteActive)glUseProgram(prog_);
        applyState(resume);   // 影子/GL 状态复原到回放前(开放流的状态)
        applyScissor();       // 批内 scissor 是录制时刻的历史状态,恢复为当前 scissors_ 栈真值
    }

    void wrapRing() {   // 环形回绕:槽位即将从头复用,已录批必须先上传+画出,再整体 orphan
        replayPending();
        if (persistentVbo_) {
            auto t=std::chrono::steady_clock::now();
            advancePersistentRegion(); pointAtPersistentRegion();
            syncMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
        }
        streamBase_ = 0; framePos_ = 0; orphan_ = true; recBaseSlot_ = 0;
    }

    // ---- 节点级世界顶点缓存(对标 Cocos Batcher2d.cpp:347 的 changedFlags|vertDirty 门控:
    //      干净节点每帧零顶点运算) ----
    // 每个节点缓存"它上一次现算 emit 出的最终顶点流"(与 verts_ 内格式逐字节一致:世界变换、
    // tint*alpha 打包全部烘好)+ 按批断点切分的段属性(texId/blend/premul/twoColor/df/tiling)。
    // 命中 = sg 的 vertGen_[idx]/vertEpoch_ 双相等 → 逐段做与 emitMeshQuad 相同的批切分判断后
    // 整段 insert 进 verts_;未命中 → 走原 emit 现算,完成后从"录制批增量 + 开放批影子状态"
    // 收割段列表存入缓存。失效完全依赖 SceneGraph::markBoundsDirty 收口的 per-node gen(compute
    // world/alpha 重算、JS 内容队列、结构变化、setNativeQuads)+ 全局 epoch(无成员信息的回退)
    // + 精确 output transform/alpha 键(主窗口尺寸或 backing scale 变化)。
    // 不缓存清单(现算路径,walk 内 vcCacheable 门控):继承/自带 filter 非默认(alpha/blur/CM/
    // overlay/noise/shadow —— blur 产生 ghost quad、shadow 偏移烘顶点,不可复用)、fromSpineMarker
    // 子树(slot 容器由父 drawOrder 驱动)、
    // stencil mask 区段与 mask 形状写(recordingActive 已含)、renderPost 离屏(postDepth_>0)、
    // stream 段(frameRecording_=false)、含 drawOrder 标记 quad 的 spine 根(子树绘制穿插)、
    // 本帧有 quad 因纹理未上传被跳过的节点(否则纹理到位后永不补画)。
    // SG_VERT_CACHE: 1=启用(默认) 0=完全绕过 2=对拍(恒现算,与缓存逐字节比对,[vcache-diff])。
    // SG_VERT_CACHE_STATS=1:每 300 帧打命中率。
    struct VertSeg {
        int texId=0, blend=0, premul=0, twoColor=0, distanceField=0, tiling=0, quadCount=0;
        uint32_t darkRGB=0; float darkA=0, fwidth=0; float tileUV[8]={0,0,0,0,0,0,0,0};
    };
    struct NodeVertCache {
        uint32_t gen=0, epoch=0;         // epoch=0 = 从未写入
        bool outputOn=false;
        float outputTransform[6]={1,0,0,1,0,0};
        float outputAlphaScale=1.0f;
        int totalQuads=0;
        std::vector<VertSeg> segs;
        std::vector<float> data;         // 段按序连续存放,seg.quadCount*4*STRIDE_F float/段
    };
    std::vector<NodeVertCache> vertCache_;   // 按节点 idx 索引;首次存储时才分配,未启用的 batcher 不占
    std::vector<VertSeg> vcSegScratch_;
    std::vector<GLuint> vcGlScratch_;
    uint64_t vertsResetSeq_ = 0;         // verts_ 被清空的次数(回绕/回放打断现算收割时废弃本次存储)
    unsigned long vcHit_=0, vcMiss_=0, vcSkip_=0, vcHitQuads_=0, vcVerify_=0, vcDiff_=0, vcFrames_=0;
    int vcDiffLogs_ = 0;
    static int vcMode() {
        static const int m = [](){ const char* e = getenv("SG_VERT_CACHE"); return e ? atoi(e) : 1; }();
        return m;
    }
    static bool vcStatsOn() {
        static const bool on = [](){ const char* e = getenv("SG_VERT_CACHE_STATS"); return e && e[0]=='1'; }();
        return on;
    }
    struct VcHarvest { size_t v0=0, rec0=0; int openQuads0=0; unsigned escTex0=0; uint64_t reset0=0; };
    bool vcStoreBegin(VcHarvest& h) const {
        h.v0=verts_.size(); h.rec0=recorded_.size(); h.openQuads0=quadCount_; h.escTex0=escTex; h.reset0=vertsResetSeq_;
        return true;
    }
    VertSeg vcSegFromBatch(const RecordedBatch& rb, int q) const {
        VertSeg s{}; s.texId=rb.texId; s.blend=rb.blend; s.premul=rb.premul; s.twoColor=rb.twoColor;
        s.darkRGB=rb.darkRGB; s.darkA=rb.darkA; s.distanceField=rb.distanceField; s.fwidth=rb.fwidth;
        s.tiling=rb.tiling; memcpy(s.tileUV,rb.tileUV,sizeof(s.tileUV)); s.quadCount=q; return s;
    }
    VertSeg vcSegFromCur(int q) const {   // 开放批(未 flush)的状态 = 当前影子状态
        VertSeg s{}; s.texId=curTexId_; s.blend=curBlend_; s.premul=curPremul_; s.twoColor=curTwoColor_;
        s.darkRGB=curDark_; s.darkA=curDarkAF_; s.distanceField=curDistanceField_; s.fwidth=curFWidth_;
        s.tiling=curTiling_; memcpy(s.tileUV,curTileUV_,sizeof(s.tileUV)); s.quadCount=q; return s;
    }
    // 现算完成后收割本节点的顶点流与批段并写缓存。段来源:落进已录批的部分用批快照(首批可能
    // 混有前序节点的 quad,只取本节点份额),仍在开放批的尾段用当前影子状态。
    void vcStoreEnd(int idx, const VcHarvest& h) {
        if (vertsResetSeq_ != h.reset0) return;   // 环回绕/回放清过 verts_:本次流不完整,下帧再存
        if (escTex != h.escTex0) return;          // 有 quad 因纹理未上传被跳过:缓存会永久丢它,不存
        if (!sg_ || sg_->vertGen_.empty()) return;
        vcSegScratch_.clear();
        int total = 0;
        if (recorded_.size() == h.rec0) {         // 节点内未发生 flush:全部在开放批
            int q = quadCount_ - h.openQuads0;
            if (q > 0) { vcSegScratch_.push_back(vcSegFromCur(q)); total += q; }
        } else {
            const RecordedBatch& rb0 = recorded_[h.rec0];
            int q0 = rb0.quadCount - h.openQuads0;
            if (q0 > 0) { vcSegScratch_.push_back(vcSegFromBatch(rb0, q0)); total += q0; }
            for (size_t i = h.rec0 + 1; i < recorded_.size(); i++) {
                const RecordedBatch& rb = recorded_[i];
                if (rb.quadCount > 0) { vcSegScratch_.push_back(vcSegFromBatch(rb, rb.quadCount)); total += rb.quadCount; }
            }
            if (quadCount_ > 0) { vcSegScratch_.push_back(vcSegFromCur(quadCount_)); total += quadCount_; }
        }
        size_t floats = verts_.size() - h.v0;
        if (floats != (size_t)total * 4 * STRIDE_F) return;   // 防御:段账与顶点流对不上(理论不可达)
        if (vertCache_.empty()) vertCache_.resize(NODE_CAP);
        NodeVertCache& e = vertCache_[idx];
        uint32_t gen = sg_->vertGen_[idx], epoch = sg_->vertEpoch_;
        bool sameOutput = e.outputOn == outputTransformOn_
            && memcmp(e.outputTransform, outputTransform_, sizeof(e.outputTransform)) == 0
            && memcmp(&e.outputAlphaScale, &outputAlphaScale_, sizeof(outputAlphaScale_)) == 0;
        if (vcMode() == 2 && e.epoch == epoch && e.gen == gen && sameOutput) {   // 对拍:缓存声称仍有效,与现算逐字节比对
            bool same = e.data.size() == floats
                && (floats == 0 || memcmp(e.data.data(), verts_.data() + h.v0, floats * sizeof(float)) == 0)
                && e.segs.size() == vcSegScratch_.size()
                && (vcSegScratch_.empty() || memcmp(e.segs.data(), vcSegScratch_.data(), vcSegScratch_.size() * sizeof(VertSeg)) == 0);
            if (!same) {
                vcDiff_++;
                if (vcDiffLogs_ < 32) {
                    vcDiffLogs_++;
                    fprintf(stderr, "[vcache-diff] node=%d gen=%u epoch=%u cachedQuads=%d freshQuads=%d cachedSegs=%zu freshSegs=%zu\n",
                            idx, gen, epoch, e.totalQuads, total, e.segs.size(), vcSegScratch_.size());
                }
            } else vcVerify_++;
        }
        e.gen = gen; e.epoch = epoch; e.totalQuads = total;
        e.outputOn = outputTransformOn_;
        memcpy(e.outputTransform, outputTransform_, sizeof(e.outputTransform));
        e.outputAlphaScale = outputAlphaScale_;
        e.segs.assign(vcSegScratch_.begin(), vcSegScratch_.end());
        e.data.assign(verts_.begin() + (long)h.v0, verts_.end());
    }
    // 命中路径:gen/epoch 双验 → 段纹理全部重解析(防纹理释放重建后 GLuint 悬挂)→ 逐段做与
    // emitMeshQuad 完全相同的批切分/状态判断,顶点整段 insert。任一段纹理解析失败 → 回退现算。
    bool vcTryHit(int idx) {
        if (vertCache_.empty() || !sg_ || sg_->vertGen_.empty()) { vcMiss_++; return false; }
        NodeVertCache& e = vertCache_[idx];
        if (e.epoch != sg_->vertEpoch_ || e.gen != sg_->vertGen_[idx]
            || e.outputOn != outputTransformOn_
            || memcmp(e.outputTransform, outputTransform_, sizeof(e.outputTransform)) != 0
            || memcmp(&e.outputAlphaScale, &outputAlphaScale_, sizeof(outputAlphaScale_)) != 0) {
            vcMiss_++;
            return false;
        }
        if (vcMode() == 2) return false;   // 对拍模式恒现算;比对与计数在 vcStoreEnd
        vcGlScratch_.clear();
        for (const VertSeg& sgm : e.segs) {
            GLuint gl = sgm.texId > 0 && texLookup ? texLookup(sgm.texId) : 0;
            if (!gl) { vcMiss_++; return false; }
            vcGlScratch_.push_back(gl);
        }
        vcHit_++; vcHitQuads_ += (unsigned long)e.totalQuads;
        if (e.totalQuads == 0) return true;   // 空节点(QUADCNT=0 等)也缓存:直接免循环
        // 继承 filter 全默认(门控保证):与现算路径逐 quad 的 set*(默认值) 等价,一次归位
        // (仅上一节点带 CM/overlay/noise 时真正 flush+关断)。
        setColorMatrix(false, nullptr); setOverlay(false, nullptr); setShadow(false, nullptr); setNoise(0.0f, 0.0f);
        size_t fOff = 0;
        for (size_t si = 0; si < e.segs.size(); si++) {
            const VertSeg& sgm = e.segs[si];
            if (streamBase_ + quadCount_ + sgm.quadCount > MAX_QUADS) wrapRing();
            GLuint gl = vcGlScratch_[si];
            bool darkChg = sgm.twoColor == 1 && (sgm.darkRGB != curDark_ || (int)(sgm.darkA * 255.0f) != curDarkA_);
            bool distanceChg = sgm.distanceField != curDistanceField_ || (sgm.distanceField == 1 && fabsf(sgm.fwidth - curFWidth_) > 0.0001f);
            bool tilingChg = sgm.tiling != curTiling_;
            if (sgm.tiling > 0 && !tilingChg) for (int i = 0; i < 8; i++) if (fabsf(curTileUV_[i] - sgm.tileUV[i]) > 0.000001f) { tilingChg = true; break; }
            if (gl != curTex_ || sgm.blend != curBlend_ || sgm.premul != curPremul_ || sgm.twoColor != curTwoColor_ || darkChg || distanceChg || tilingChg) {
                flush(); curTex_ = gl;
                setBlend(sgm.blend); setPremul(sgm.premul); setTwoColor(sgm.twoColor);
                if (sgm.twoColor == 1) setDark(sgm.darkRGB, sgm.darkA);
                setDistanceField(sgm.distanceField, sgm.fwidth); setTiling(sgm.tiling > 0 ? sgm.tileUV : nullptr,sgm.tiling);
            }
            curTexId_ = sgm.texId;
            size_t n = (size_t)sgm.quadCount * 4 * STRIDE_F;
            verts_.insert(verts_.end(), e.data.begin() + (long)fOff, e.data.begin() + (long)(fOff + n));
            fOff += n; quadCount_ += sgm.quadCount;
            if (quadCount_ >= MAX_QUADS) flush();
        }
        return true;
    }

    void setPremul(int p) {   // p=1 非预乘纹理(shader 内预乘);切换需在 flush 后设(uniform 影响下批)
        if (p == curPremul_) return;
        curPremul_ = p;
        if (uPremul_ >= 0) glUniform1f(uPremul_, (float)p);
    }
    void setTwoColor(int t) {   // t=1 spine 双色 tint(vColor=直值 light + uDark);flush 后设
        if (t == curTwoColor_) return;
        curTwoColor_ = t;
        if (uTwoColor_ >= 0) glUniform1f(uTwoColor_, (float)t);
    }
    void setDistanceField(int enabled,float width){
        curDistanceField_=enabled;curFWidth_=width;
        if(uDistanceField_>=0)glUniform1f(uDistanceField_,(float)enabled);
        if(uFWidth_>=0)glUniform1f(uFWidth_,width);
    }
    void setTiling(const float* corners,int kind=Q_SHADER_TILING){
        curTiling_=corners?kind:Q_SHADER_DEFAULT;if(uTiling_>=0)glUniform1f(uTiling_,(float)curTiling_);
        if(!corners)return;for(int i=0;i<8;i++)curTileUV_[i]=corners[i];for(int i=0;i<4;i++)if(uTileUV_[i]>=0)glUniform2f(uTileUV_[i],corners[i*2],corners[i*2+1]);
    }
    void setMaskWrite(int mode) {
        if(mode==curMaskWrite_)return;
        flush();curMaskWrite_=mode;
        if(uMaskWrite_>=0)glUniform1f(uMaskWrite_,(float)mode);
    }
    void setDark(uint32_t rgb, float a) {   // uDark = dark 颜色(0xRRGGBB + alpha)
        curDark_ = rgb; curDarkA_ = (int)(a*255.0f); curDarkAF_ = a;
        if (uDark_ >= 0) glUniform4f(uDark_, ((rgb>>16)&0xFF)/255.0f, ((rgb>>8)&0xFF)/255.0f, (rgb&0xFF)/255.0f, a);
    }
    void setColorMatrix(bool on,const float* m){
        bool changed=on!=curCMOn_;
        if(on&&!changed)for(int i=0;i<20;i++)if(fabsf(curCM_[i]-m[i])>0.00001f){changed=true;break;}
        if(!changed)return;flush();curCMOn_=on;if(uCMOn_>=0)glUniform1f(uCMOn_,on?1.0f:0.0f);if(!on)return;
        memcpy(curCM_,m,sizeof(curCM_));float gm[16];for(int r=0;r<4;r++)for(int c=0;c<4;c++)gm[c*4+r]=m[r*5+c];
        if(uCM_>=0)glUniformMatrix4fv(uCM_,1,GL_FALSE,gm);if(uCMOff_>=0)glUniform4f(uCMOff_,m[4],m[9],m[14],m[19]);
    }
    void setOverlay(bool on,const float* v){
        bool changed=on!=curOverlayOn_;if(on&&!changed)for(int i=0;i<4;i++)if(fabsf(curOverlay_[i]-v[i])>0.00001f){changed=true;break;}if(!changed)return;flush();curOverlayOn_=on;if(uOverlayOn_>=0)glUniform1f(uOverlayOn_,on?1.0f:0.0f);if(!on)return;memcpy(curOverlay_,v,sizeof(curOverlay_));if(uOverlayColor_>=0)glUniform3f(uOverlayColor_,v[0],v[1],v[2]);if(uOverlayAlpha_>=0)glUniform1f(uOverlayAlpha_,v[3]);
    }
    void setShadow(bool on,const float* v){
        bool changed=on!=curShadowOn_;if(on&&!changed)for(int i=0;i<4;i++)if(fabsf(curShadow_[i]-v[i+2])>0.00001f){changed=true;break;}if(!changed)return;flush();curShadowOn_=on;if(uShadowOn_>=0)glUniform1f(uShadowOn_,on?1.0f:0.0f);if(!on)return;for(int i=0;i<4;i++)curShadow_[i]=v[i+2];if(uShadowColor_>=0)glUniform3f(uShadowColor_,v[2],v[3],v[4]);if(uShadowAlpha_>=0)glUniform1f(uShadowAlpha_,v[5]);
    }
    void setNoise(float amount,float seed){
        if(fabsf(amount-curNoise_)<=0.00001f&&fabsf(seed-curNoiseSeed_)<=0.00001f)return;
        flush();curNoise_=amount;curNoiseSeed_=seed;
        if(uNoise_>=0)glUniform1f(uNoise_,amount);
        if(uNoiseSeed_>=0)glUniform1f(uNoiseSeed_,seed);
    }

    void setBlend(int mode) {
        if (mode == curBlend_) return;
        curBlend_ = mode;
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);              // SUBTRACT 后切回普通模式必须恢复
        if (mode == 20) { glDisable(GL_BLEND); return; }  // NONE:pixi state.blend=false → 关混合,源 RGBA 直接覆盖
        glEnable(GL_BLEND);                               // 从 NONE 切回来要重新开
        switch (mode) {                                   // 与 Pixi mapWebGLBlendModesToPixi 对齐
            case 1:  glBlendFunc(GL_ONE, GL_ONE); break;                          // ADD
            case 2:  glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
            case 3:  glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
            // NPM 变体:Pixi 里 SRC_ALPHA 因子服务于"未预乘的 shader 输出";本 shader 经 uPremul
            // 恒输出预乘色,再乘 SRC_ALPHA=双重预乘过暗 → 映射到对应预乘因子。
            case 17: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;                                              // NORMAL_NPM
            case 18: glBlendFunc(GL_ONE, GL_ONE); break;                                                              // ADD_NPM
            case 19: glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;      // SCREEN_NPM
            case 21: glBlendFunc(GL_DST_ALPHA, GL_ZERO); break;                    // SRC_IN
            case 22: glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ZERO); break;          // SRC_OUT
            case 23: glBlendFunc(GL_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;      // SRC_ATOP
            case 24: glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE); break;            // DST_OVER
            case 25: glBlendFunc(GL_ZERO, GL_SRC_ALPHA); break;                     // DST_IN
            case 26: glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA); break;           // DST_OUT / ERASE
            case 27: glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA); break;       // DST_ATOP
            case 28: glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE); glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD); break;
            case 29: glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break; // XOR
            default: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;            // NORMAL;4..16 在 Pixi WebGL 同样退化 normal
        }
    }

    static GLuint link(const char* vs, const char* fs, GLint pos=-1, GLint uv=-1, GLint color=-1) {
        GLuint v = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(v, 1, &vs, nullptr); glCompileShader(v);
        GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(f, 1, &fs, nullptr); glCompileShader(f);
        GLuint p = glCreateProgram();
        glAttachShader(p, v); glAttachShader(p, f);
        // The specialized program consumes the existing VAO/pointers without rebinding them.
        if(pos>=0)glBindAttribLocation(p,(GLuint)pos,"aPos");
        if(uv>=0)glBindAttribLocation(p,(GLuint)uv,"aUV");
        if(color>=0)glBindAttribLocation(p,(GLuint)color,"aColor");
        glLinkProgram(p);
        GLint ok=0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) { char log[512]; glGetProgramInfoLog(p, 512, nullptr, log); fprintf(stderr, "[batcher] link fail: %s\n", log); }
        glDeleteShader(v); glDeleteShader(f);
        return ok ? p : 0;
    }
};

} // namespace sg
