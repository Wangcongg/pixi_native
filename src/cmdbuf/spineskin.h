// spine 蒙皮桥:把 computeWorldVertices(骨骼矩阵 × 权重顶点,spine 的每帧热点)搬进 C++。
// 架构:JS 保留动画状态/事件/API(零语义风险);attachment 静态数据(权重表)注册一次;
// 每帧每 spine:JS 把骨骼矩阵写共享 buffer → skinAll 一次算出该实例全部已注册 slot 的顶点写 arena
// → JS 的 computeWorldVertices patch 变成一次 Float32Array 拷贝。arena 单实例复用(算完立即消费)。
// 精确复刻 spine-ts VertexAttachment/RegionAttachment.computeWorldVertices，包含 weighted/unweighted deform。
#pragma once
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include "scenegraph.h"

namespace spsk {

// 共享 buffer 布局(float32):
//   [0 .. BONES_F)          骨骼输入区:每骨 6f(a,b,c,d,worldX,worldY),JS 每帧写"当前实例"的
//   [BONES_F .. IDX_END)    索引区:每 slot 2f(arena 内偏移, 长度 floats;len=0 表示无效)
//   [ARENA .. TOTAL_F)      顶点输出 arena(紧凑 xy 对)
enum { MAX_BONES=1024, BONES_F=MAX_BONES*6,
       MAX_SLOTS=2048, IDX_F=MAX_SLOTS*2, IDX_END=BONES_F+IDX_F,
       ARENA=IDX_END, ARENA_F=1<<18,
       TOTAL_F=ARENA+ARENA_F };

struct SlotReg {
    int type = 0;                 // 0=region(offset[8]) 1=mesh无权重(顶点在slot.bone局部) 2=mesh加权
    int boneIndex = 0;            // type0/1:slot.bone 在 skeleton.bones 的下标
    std::vector<float> verts;     // type0:offset[8];type1:vertices;type2:[vx,vy,weight]×
    std::vector<int32_t> bones;   // type2:交错 [n, boneIdx...]
    int outFloats = 0;            // 输出长度(顶点数×2)
    // ---- native SpineRenderer(S1):渲染静态数据(一次注册)----
    int rtype = -1;               // -1未注册渲染 0=region(2三角) 1=mesh(用 tris)
    std::vector<float> uvs;       // region:4角uv[8];mesh:worldVerticesLength
    std::vector<int32_t> tris;    // mesh 三角索引
};

// The authored draw/clip order changes much less often than the pose. Keep that topology in C++
// and combine it with the slot colors produced by the Native AnimationState pass at emission time.
// JS therefore rebuilds this plan only after an attachment, sequence, draw-order or slotObject
// mutation instead of walking every slot for every presented frame.
struct RenderState {
    std::vector<float> plan;       // 12 floats per entry; draw entries contain attachment/static data
    std::vector<float> slotColors; // 7 floats per slot: rgba + dark rgb
    // (SlotObjShadow base/last 影子协议已随 Phase E EFX 通道废除:合成因子写 EFX 区段,
    //  ALPHA/VISIBLE 槽保持业务 base,不存在「猜测采纳」——死结构已清理。)
};

class SpineSkin {
public:
    float* buf = nullptr;
    // instId → (slotIdx → 注册)
    std::unordered_map<int, std::unordered_map<int, SlotReg> > insts;
    std::unordered_map<int, RenderState> renderStates;
    std::vector<int> lastWritten_;   // 上次 skinAll 写过的 slotIdx —— 只清这些,免每帧清整块 IDX_F(1024 floats/spine)

    void init(float* b) { buf = b; }

    void reg(int instId, int slotIdx, int type, int boneIndex,
             const float* fv, int fn, const int32_t* iv, int in_, int outFloats) {
        if (slotIdx < 0 || slotIdx >= MAX_SLOTS || outFloats <= 0 || outFloats > ARENA_F) return;
        SlotReg r; r.type = type; r.boneIndex = boneIndex; r.outFloats = outFloats;
        if (fv && fn > 0) r.verts.assign(fv, fv+fn);
        if (iv && in_ > 0) r.bones.assign(iv, iv+in_);
        insts[instId][slotIdx] = r;
    }

    // S1:注册 slot 渲染静态数据(uvs/triangles/rtype),一次。挂到已存在的 SlotReg(reg 先调)。
    void regRender(int instId, int slotIdx, int rtype, const float* uvs, int uvN, const int32_t* tris, int triN) {
        if (slotIdx < 0 || slotIdx >= MAX_SLOTS) return;
        SlotReg& r = insts[instId][slotIdx];   // 若无则默认构造(reg 通常先调)
        r.rtype = rtype;
        if (uvs && uvN > 0) r.uvs.assign(uvs, uvs+uvN); else r.uvs.clear();
        if (tris && triN > 0) r.tris.assign(tris, tris+triN); else r.tris.clear();
    }

    void setRenderPlan(int instId,const float* plan,int planFloats,const float* colors,int colorFloats) {
        if(instId<=0)return;RenderState& state=renderStates[instId];
        if(plan&&planFloats>=12)state.plan.assign(plan,plan+(planFloats/12)*12);else state.plan.clear();
        if(colors&&colorFloats>0)state.slotColors.assign(colors,colors+colorFloats);
    }

    // Attachment/Sequence timelines usually change only one slot.  Each authored slot occupies
    // exactly one stable record in the cached plan (draw, clip or end), so patch those records in
    // one typed-array bridge call instead of rebuilding and copying the whole Spine plan.
    // Patch stride: [recordIndex, 12 record floats].
    bool patchRenderPlan(int instId,const float* patches,int patchCount) {
        if(instId<=0||!patches||patchCount<=0)return false;
        auto found=renderStates.find(instId);if(found==renderStates.end())return false;
        std::vector<float>& plan=found->second.plan;const int STRIDE=13,REC=12;
        for(int p=0;p<patchCount;p++){
            const float* src=patches+(size_t)p*STRIDE;int record=(int)src[0];
            if(record<0||(size_t)(record+1)*REC>plan.size())return false;
            std::memcpy(plan.data()+(size_t)record*REC,src+1,(size_t)REC*sizeof(float));
        }
        return true;
    }

    void setRenderColors(int instId,const float* colors,int colorFloats) {
        if(instId<=0||!colors||colorFloats<=0)return;
        RenderState& state=renderStates[instId];state.slotColors.assign(colors,colors+colorFloats);
    }

    std::vector<std::vector<float> > emitClipPolys_;
    std::vector<float> emitClipPoly_;
    std::vector<int> emitTri_;
    std::vector<int> emitIdx_;
    std::vector<char> emitConcave_;

    static bool triPositiveArea(float p1x,float p1y,float p2x,float p2y,float p3x,float p3y) {
        return p1x * (p3y - p2y) + p2x * (p1y - p3y) + p3x * (p2y - p1y) >= 0;
    }
    static int triWinding(float p1x,float p1y,float p2x,float p2y,float p3x,float p3y) {
        float px = p2x - p1x, py = p2y - p1y;
        return p3x * py - p3y * px + px * p1y - p1x * py >= 0 ? 1 : -1;
    }
    static bool triIsConcave(int index, int vertexCount, const std::vector<float>& vertices, const std::vector<int>& indices) {
        int previous = indices[(vertexCount + index - 1) % vertexCount] << 1;
        int current = indices[index] << 1;
        int next = indices[(index + 1) % vertexCount] << 1;
        return !triPositiveArea(vertices[previous], vertices[previous + 1], vertices[current], vertices[current + 1], vertices[next], vertices[next + 1]);
    }
    static void makeClockwise(std::vector<float>& polygon) {
        int verticesLength = (int)polygon.size();
        if (verticesLength < 6) return;
        float area = polygon[verticesLength - 2] * polygon[1] - polygon[0] * polygon[verticesLength - 1];
        for (int i = 0, n = verticesLength - 3; i < n; i += 2)
            area += polygon[i] * polygon[i + 3] - polygon[i + 2] * polygon[i + 1];
        if (area < 0) return;
        for (int i = 0, lastX = verticesLength - 2, n = verticesLength >> 1; i < n; i += 2) {
            int other = lastX - i;
            std::swap(polygon[i], polygon[other]);
            std::swap(polygon[i + 1], polygon[other + 1]);
        }
    }
    void triangulateClipPolygon(const std::vector<float>& vertices, std::vector<int>& triangles) {
        int vertexCount = (int)vertices.size() >> 1;
        triangles.clear();
        if (vertexCount < 3) return;
        emitIdx_.resize(vertexCount);
        for (int i = 0; i < vertexCount; i++) emitIdx_[i] = i;
        emitConcave_.resize(vertexCount);
        for (int i = 0; i < vertexCount; i++) emitConcave_[i] = triIsConcave(i, vertexCount, vertices, emitIdx_) ? 1 : 0;
        while (vertexCount > 3) {
            int previous = vertexCount - 1, i = 0, next = 1;
            while (true) {
                bool ear = false;
                if (!emitConcave_[i]) {
                    int p1 = emitIdx_[previous] << 1, p2 = emitIdx_[i] << 1, p3 = emitIdx_[next] << 1;
                    float p1x = vertices[p1], p1y = vertices[p1 + 1];
                    float p2x = vertices[p2], p2y = vertices[p2 + 1];
                    float p3x = vertices[p3], p3y = vertices[p3 + 1];
                    ear = true;
                    for (int ii = (next + 1) % vertexCount; ii != previous; ii = (ii + 1) % vertexCount) {
                        if (!emitConcave_[ii]) continue;
                        int v = emitIdx_[ii] << 1;
                        float vx = vertices[v], vy = vertices[v + 1];
                        if (triPositiveArea(p3x, p3y, p1x, p1y, vx, vy) &&
                            triPositiveArea(p1x, p1y, p2x, p2y, vx, vy) &&
                            triPositiveArea(p2x, p2y, p3x, p3y, vx, vy)) { ear = false; break; }
                    }
                    if (ear) break;
                }
                if (next == 0) {
                    do {
                        if (!emitConcave_[i]) break;
                        i--;
                    } while (i > 0);
                    break;
                }
                previous = i;
                i = next;
                next = (next + 1) % vertexCount;
            }
            triangles.push_back(emitIdx_[(vertexCount + i - 1) % vertexCount]);
            triangles.push_back(emitIdx_[i]);
            triangles.push_back(emitIdx_[(i + 1) % vertexCount]);
            emitIdx_.erase(emitIdx_.begin() + i);
            emitConcave_.erase(emitConcave_.begin() + i);
            vertexCount--;
            int previousIndex = (vertexCount + i - 1) % vertexCount;
            int nextIndex = i == vertexCount ? 0 : i;
            emitConcave_[previousIndex] = triIsConcave(previousIndex, vertexCount, vertices, emitIdx_) ? 1 : 0;
            emitConcave_[nextIndex] = triIsConcave(nextIndex, vertexCount, vertices, emitIdx_) ? 1 : 0;
        }
        if (vertexCount == 3) {
            triangles.push_back(emitIdx_[2]);
            triangles.push_back(emitIdx_[0]);
            triangles.push_back(emitIdx_[1]);
        }
    }
    bool buildClipPolygons(const float* v, int len) {
        emitClipPolys_.clear();
        if (!v || len < 6 || (len & 1)) return false;
        emitClipPoly_.assign(v, v + len);
        makeClockwise(emitClipPoly_);
        triangulateClipPolygon(emitClipPoly_, emitTri_);
        for (size_t i = 0; i + 2 < emitTri_.size(); i += 3) {
            int i0 = emitTri_[i] << 1, i1 = emitTri_[i + 1] << 1, i2 = emitTri_[i + 2] << 1;
            if (i0 + 1 >= len || i1 + 1 >= len || i2 + 1 >= len) continue;
            std::vector<float> poly;
            poly.reserve(8);
            poly.push_back(emitClipPoly_[i0]);     poly.push_back(emitClipPoly_[i0 + 1]);
            poly.push_back(emitClipPoly_[i1]);     poly.push_back(emitClipPoly_[i1 + 1]);
            poly.push_back(emitClipPoly_[i2]);     poly.push_back(emitClipPoly_[i2 + 1]);
            makeClockwise(poly);
            poly.push_back(poly[0]); poly.push_back(poly[1]);
            emitClipPolys_.push_back(poly);
        }
        return !emitClipPolys_.empty();
    }
    static bool writeTriQuad(float* qbase, int QSF, int quadCursor, int quadCap, int& n,
                             float x0,float y0,float u0,float v0,float x1,float y1,float u1,float v1,float x2,float y2,float u2,float v2,
                             float tid,float tint,float al,float bl,float dk,float dka,float pm) {
        if (quadCursor + n + 1 > quadCap) return false;
        float* q = qbase + (size_t)(quadCursor + n) * QSF;
        q[0]=tid; q[1]=x0;q[2]=y0; q[3]=x1;q[4]=y1; q[5]=x2;q[6]=y2; q[7]=x2;q[8]=y2;
        q[9]=u0;q[10]=v0; q[11]=u1;q[12]=v1; q[13]=u2;q[14]=v2; q[15]=u2;q[16]=v2;
        q[17]=tint; q[18]=al; q[19]=bl; q[20]=dk; q[21]=dka; q[22]=pm;
        q[23]=0;   // shader kind: persistent quad blocks are reused across owners; a stale
                   // tiling flag (written by a freed TilingSprite) would reroute this quad
                   // to the tiling sampler and corrupt sampling.
        n++;
        return true;
    }

    // S2:按 draw order 产 quad(格式同 JS emitTri:退化 quad,写 sgbuf 的 quad 区)。
    // recs 步长 12:
    //   kind=0 draw [0, slotIdx, texid, tint, alpha, blend, dark, darkAlpha, premul, 0, 0, 0]
    //   kind=1 clip [1, slotIdx, endSlotIdx, ...]
    //   kind=2 slot-end marker [2, slotIdx, ...]
    //   kind=3 slot child marker [3, slotIdx, nativeChildNode, followAttachment, attached, ...]
    // 返回写入 quad 数;溢出返 -1。region/mesh/clipping 都在 C++ 发 native quad。
    int emitSpine(int instId, const float* recs, int recN, float* sgbuf, int quadBase, int quadCursor, int quadCap, int QSF) {
        if (!buf) return 0;
        std::unordered_map<int, std::unordered_map<int, SlotReg> >::iterator it = insts.find(instId);
        if (it == insts.end()) return 0;
        const float* arena = buf + ARENA;
        const float* idx = buf + BONES_F;
        float* qbase = sgbuf + quadBase + 1;
        static const int RT[6] = {0,1,2,0,2,3};
        int n = 0;
        bool clipping = false;
        int clipEndSlot = -1;
        const int REC = 12;
        auto endClipForSlot = [&](int slotIdx) {
            if (clipping && slotIdx == clipEndSlot) {
                clipping = false;
                clipEndSlot = -1;
                emitClipPolys_.clear();
            }
        };
        for (int r = 0; r < recN; r++) {
            const float* rec = recs + r*REC;
            int kind = (int)rec[0];
            int slotIdx = (int)rec[1];
            if (kind == 2) { endClipForSlot(slotIdx); continue; }
            if (kind == 3) {
                if (quadCursor + n + 1 > quadCap) return -1;
                float* q = qbase + (size_t)(quadCursor + n) * QSF;
                for (int k = 0; k < QSF; k++) q[k] = 0.0f;
                q[0] = -(float)((int)rec[2]);
                n++;
                continue;
            }
            if (slotIdx < 0 || slotIdx >= MAX_SLOTS) continue;
            std::unordered_map<int, SlotReg>::iterator sk = it->second.find(slotIdx);
            if (kind == 1) {
                if (!clipping && sk != it->second.end()) {
                    int off = (int)idx[slotIdx*2], len = (int)idx[slotIdx*2+1];
                    if (len >= 6) {
                        clipping = buildClipPolygons(arena + off, len);
                        clipEndSlot = (int)rec[2];
                    }
                }
                continue;
            }
            if (sk == it->second.end()) { endClipForSlot(slotIdx); continue; }
            SlotReg& sr = sk->second;
            int off = (int)idx[slotIdx*2], len = (int)idx[slotIdx*2+1];
            if (len < 6 || sr.uvs.empty()) { endClipForSlot(slotIdx); continue; }
            const float* v = arena + off;
            const float* uv = sr.uvs.data(); int uvN = (int)sr.uvs.size();
            float tid=rec[2], tint=rec[3], al=rec[4], bl=rec[5], dk=rec[6], dka=rec[7], pm=rec[8];
            const int* tr; int trCnt;
            if (sr.rtype == 0) { tr = RT; trCnt = 6; }                                  // region:2三角
            else if (sr.rtype == 1) { if (sr.tris.empty()) { endClipForSlot(slotIdx); continue; } trCnt = (int)sr.tris.size(); } // mesh
            else { endClipForSlot(slotIdx); continue; }
            for (int t = 0; t+2 < trCnt; t += 3) {
                int i0, i1, i2;
                if (sr.rtype == 0) { i0=RT[t]; i1=RT[t+1]; i2=RT[t+2]; }
                else { i0=sr.tris[t]; i1=sr.tris[t+1]; i2=sr.tris[t+2]; }
                int a0=i0*2, a1=i1*2, a2=i2*2;
                if (a0+1>=len||a1+1>=len||a2+1>=len) continue;                          // 越界(arena 没算到)
                if (a0+1>=uvN||a1+1>=uvN||a2+1>=uvN) continue;
                float x1=v[a0], y1=v[a0+1], u1=uv[a0], vv1=uv[a0+1];
                float x2=v[a1], y2=v[a1+1], u2=uv[a1], vv2=uv[a1+1];
                float x3=v[a2], y3=v[a2+1], u3=uv[a2], vv3=uv[a2+1];
                if (clipping && !emitClipPolys_.empty()) {
                    for (size_t p = 0; p < emitClipPolys_.size(); p++) {
                        const std::vector<float>& poly = emitClipPolys_[p];
                        if (clipTri(x1,y1,x2,y2,x3,y3, poly.data(), (int)poly.size(), clipOutput_)) {
                            int cnt = (int)clipOutput_.size() >> 1;
                            if (cnt < 3) continue;
                            float d0 = y2-y3, d1 = x3-x2, d2 = x1-x3, d4 = y3-y1;
                            float d = 1.0f / (d0*d2 + d1*(y1-y3));
                            for (int ii = 1; ii + 1 < cnt; ii++) {
                                float px0=clipOutput_[0], py0=clipOutput_[1];
                                float px1=clipOutput_[ii*2], py1=clipOutput_[ii*2+1];
                                float px2=clipOutput_[(ii+1)*2], py2=clipOutput_[(ii+1)*2+1];
                                float c0=px0-x3, c1=py0-y3, ba=(d0*c0+d1*c1)*d, bb=(d4*c0+d2*c1)*d, bc=1-ba-bb;
                                float pu0=u1*ba+u2*bb+u3*bc, pv0=vv1*ba+vv2*bb+vv3*bc;
                                c0=px1-x3; c1=py1-y3; ba=(d0*c0+d1*c1)*d; bb=(d4*c0+d2*c1)*d; bc=1-ba-bb;
                                float pu1=u1*ba+u2*bb+u3*bc, pv1=vv1*ba+vv2*bb+vv3*bc;
                                c0=px2-x3; c1=py2-y3; ba=(d0*c0+d1*c1)*d; bb=(d4*c0+d2*c1)*d; bc=1-ba-bb;
                                float pu2=u1*ba+u2*bb+u3*bc, pv2=vv1*ba+vv2*bb+vv3*bc;
                                if (!writeTriQuad(qbase,QSF,quadCursor,quadCap,n, px0,py0,pu0,pv0, px1,py1,pu1,pv1, px2,py2,pu2,pv2, tid,tint,al,bl,dk,dka,pm)) return -1;
                            }
                        } else {
                            if (!writeTriQuad(qbase,QSF,quadCursor,quadCap,n, x1,y1,u1,vv1, x2,y2,u2,vv2, x3,y3,u3,vv3, tid,tint,al,bl,dk,dka,pm)) return -1;
                            break;
                        }
                    }
                } else {
                    if (!writeTriQuad(qbase,QSF,quadCursor,quadCap,n, x1,y1,u1,vv1, x2,y2,u2,vv2, x3,y3,u3,vv3, tid,tint,al,bl,dk,dka,pm)) return -1;
                }
            }
            endClipForSlot(slotIdx);
        }
        return n;
    }

    int emitCached(int instId,float skeletonR,float skeletonG,float skeletonB,float skeletonA,
                   float* sgbuf,int quadBase,int quadCursor,int quadCap,int QSF,sg::SceneGraph* graph=nullptr) {
        auto found=renderStates.find(instId);if(found==renderStates.end()||found->second.plan.empty())return 0;
        RenderState& state=found->second;emitDynamic_=state.plan;
        auto channel=[](float value){return std::max(0,std::min(255,(int)(value*255.0f)));};
        const int REC=12,recN=(int)emitDynamic_.size()/REC;
        for(int r=0;r<recN;r++){
            float* rec=emitDynamic_.data()+(size_t)r*REC;int kind=(int)rec[0];
            if(kind==3){
                int slot=(int)rec[1],node=(int)rec[2],offset=slot*7;if(!sgbuf||node<=0||node>=sg::NODE_CAP)continue;
                float slotAlpha=1;if(offset>=0&&offset+3<(int)state.slotColors.size())slotAlpha=state.slotColors[offset+3];
                float* child=sgbuf+(size_t)node*sg::STRIDE;
                // Phase E:base/effective 拆分——ALPHA/VISIBLE 槽保持业务 base 不再覆写;合成因子
                // (skeletonA×slotAlpha 与 spineHidden 门)写独立 EFX 通道,由 computeNode 的
                // WALPHA 派生链应用。「槽值≠lastAlpha 猜测采纳为 base」协议废除:effective 是
                // 派生结果,不是第二真值(base 衰减螺旋的结构性根治)。
                bool spineHidden=(rec[3]>0.5f&&rec[4]<0.5f);
                float efxA=skeletonA*slotAlpha, efxV=spineHidden?0.0f:1.0f;
                bool changed=(int)child[sg::TYPE]!=sg::T_SPINE_SLOT
                    ||sgbuf[sg::EFX_ALPHA_BASE+node]!=efxA||sgbuf[sg::EFX_VIS_BASE+node]!=efxV;
                child[sg::TYPE]=(float)sg::T_SPINE_SLOT;
                sgbuf[sg::EFX_ALPHA_BASE+node]=efxA;sgbuf[sg::EFX_VIS_BASE+node]=efxV;
                if(changed&&graph)graph->markLocalInputChanged(node);
                continue;
            }
            if(kind!=0)continue;
            int slot=(int)rec[1],offset=slot*7;float sr=1,sg=1,sb=1,sa=1,dr=0,dg=0,db=0;
            if(offset>=0&&offset+6<(int)state.slotColors.size()){
                const float* c=state.slotColors.data()+offset;sr=c[0];sg=c[1];sb=c[2];sa=c[3];dr=c[4];dg=c[5];db=c[6];
            }
            uint32_t authored=(uint32_t)rec[3];float ar=((authored>>16)&255)/255.0f,ag=((authored>>8)&255)/255.0f,ab=(authored&255)/255.0f;
            rec[3]=(float)((channel(sr*skeletonR*ar)<<16)|(channel(sg*skeletonG*ag)<<8)|channel(sb*skeletonB*ab));
            rec[4]=sa*skeletonA*rec[4];
            rec[6]=(float)((channel(dr)<<16)|(channel(dg)<<8)|channel(db));
            rec[7]=rec[7]<0?0:rec[7];
        }
        return emitSpine(instId,emitDynamic_.data(),recN,sgbuf,quadBase,quadCursor,quadCap,QSF);
    }

    void clearInst(int instId) { insts.erase(instId); renderStates.erase(instId); }

    // Recompute one registered mesh using the current deform array. skinAll has already allocated
    // the slot's arena span and copied the current bone matrices into buf. For an unweighted mesh
    // Spine treats deform as replacement local vertices; for a weighted mesh it contains one xy
    // offset per influence. The result overwrites that same arena span for emitSpine.
    bool skinDeform(int instId,int slotIdx,int boneCount,const float* deform,int deformN,const float* boneMatrices=nullptr) {
        if(!buf||!deform||deformN<=0||slotIdx<0||slotIdx>=MAX_SLOTS)return false;
        auto ii=insts.find(instId);if(ii==insts.end())return false;auto si=ii->second.find(slotIdx);if(si==ii->second.end())return false;
        SlotReg& r=si->second;if(r.type!=1&&r.type!=2)return false;
        float* idx=buf+BONES_F;int off=(int)idx[slotIdx*2],len=(int)idx[slotIdx*2+1];if(len<r.outFloats||off<0||off+len>ARENA_F)return false;
        float* out=buf+ARENA+off;const float* B=boneMatrices?boneMatrices:buf;
        if(r.type==1){
            if(r.boneIndex<0||r.boneIndex>=boneCount||deformN<r.outFloats)return false;const float* bm=B+r.boneIndex*6;
            for(int w=0;w<r.outFloats;w+=2){float vx=deform[w],vy=deform[w+1];out[w]=vx*bm[0]+vy*bm[1]+bm[4];out[w+1]=vx*bm[2]+vy*bm[3]+bm[5];}
            return true;
        }
        const int32_t* bn=r.bones.data();int bnLen=(int)r.bones.size();const float* vv=r.verts.data();int vvLen=(int)r.verts.size();int v=0,b3=0,f=0;
        for(int w=0;w<r.outFloats;w+=2){
            if(v>=bnLen)return false;int n=bn[v++],end=v+n;if(end>bnLen||b3+n*3>vvLen||f+n*2>deformN)return false;float wx=0,wy=0;
            for(;v<end;v++,b3+=3,f+=2){int bi=bn[v];if(bi<0||bi>=boneCount)bi=0;const float* bm=B+bi*6;float vx=vv[b3]+deform[f],vy=vv[b3+1]+deform[f+1],wt=vv[b3+2];wx+=(vx*bm[0]+vy*bm[1]+bm[4])*wt;wy+=(vx*bm[2]+vy*bm[3]+bm[5])*wt;}
            out[w]=wx;out[w+1]=wy;
        }
        return true;
    }

    // 该实例全部已注册 slot 的蒙皮:骨骼区已由 JS 写好。索引区清零后按 slot 填(off,len)。
    void skinAllFromMatrices(int instId, int boneCount,const float* matrices) {
        if (!buf||!matrices) return;
        float* idx = buf + BONES_F;
        // 只清上次写过的 slot(stale 只可能来自紧邻上次 skinAll:idx 每 spine 写→读顺序执行,transient)。
        // 原来每帧清整块 IDX_F=1024 floats × 每 spine × 60fps = 数千万无谓写,是 skinAll 的固定浪费。
        for (size_t i = 0; i < lastWritten_.size(); i++) { int s = lastWritten_[i]; idx[s*2] = 0; idx[s*2+1] = 0; }
        lastWritten_.clear();
        std::unordered_map<int, std::unordered_map<int, SlotReg> >::iterator it = insts.find(instId);
        if (it == insts.end()) return;
        float* arena = buf + ARENA;
        const float* B = matrices;
        int cur = 0;
        for (std::unordered_map<int, SlotReg>::iterator kv = it->second.begin(); kv != it->second.end(); ++kv) {
            int slotIdx = kv->first; SlotReg& r = kv->second;
            if (cur + r.outFloats > ARENA_F) break;
            float* out = arena + cur;
            if (r.type == 0 || r.type == 1) {
                if (r.boneIndex < 0 || r.boneIndex >= boneCount) continue;
                const float* bm = B + r.boneIndex*6;
                float a=bm[0], b=bm[1], c=bm[2], d=bm[3], x=bm[4], y=bm[5];
                const float* v = r.verts.data();
                int n = r.outFloats;
                if ((int)r.verts.size() < n) continue;
                for (int w = 0; w < n; w += 2) {
                    float vx = v[w], vy = v[w+1];
                    out[w]   = vx*a + vy*b + x;      // 同 spine-ts:wx = vx*a + vy*b + worldX
                    out[w+1] = vx*c + vy*d + y;
                }
            } else {
                const int32_t* bn = r.bones.data(); int bnLen = (int)r.bones.size();
                const float* vv = r.verts.data();   int vvLen = (int)r.verts.size();
                int v = 0, b3 = 0; bool bad = false;
                for (int w = 0; w < r.outFloats; w += 2) {
                    float wx = 0, wy = 0;
                    if (v >= bnLen) { bad = true; break; }
                    int n = bn[v++]; int end = v + n;
                    if (end > bnLen || b3 + n*3 > vvLen) { bad = true; break; }
                    for (; v < end; v++, b3 += 3) {
                        int bi = bn[v]; if (bi < 0 || bi >= boneCount) { bi = 0; }
                        const float* bm = B + bi*6;
                        float vx = vv[b3], vy = vv[b3+1], wt = vv[b3+2];
                        wx += (vx*bm[0] + vy*bm[1] + bm[4]) * wt;
                        wy += (vx*bm[2] + vy*bm[3] + bm[5]) * wt;
                    }
                    out[w] = wx; out[w+1] = wy;
                }
                if (bad) continue;   // 数据异常:不写索引 → JS fallback
            }
            idx[slotIdx*2]   = (float)cur;
            idx[slotIdx*2+1] = (float)r.outFloats;
            lastWritten_.push_back(slotIdx);   // 记录本次写的 slot,下次只清这些
            cur += r.outFloats;
        }
    }
    void skinAll(int instId, int boneCount) { skinAllFromMatrices(instId,boneCount,buf); }
    std::vector<float> clipOutput_, scratch_, emitDynamic_;
    // Sutherland-Hodgman:三角形 vs 凸多边形(末点重复首点)。返回 true=发生裁剪。
    bool clipTri(float x1,float y1,float x2,float y2,float x3,float y3, const float* area, int areaLen, std::vector<float>& originalOutput) {
        std::vector<float>* output = &originalOutput;
        std::vector<float>* input;
        bool clipped = false;
        if (areaLen % 4 >= 2) { input = output; output = &scratch_; }
        else input = &scratch_;
        input->clear();
        input->push_back(x1); input->push_back(y1);
        input->push_back(x2); input->push_back(y2);
        input->push_back(x3); input->push_back(y3);
        input->push_back(x1); input->push_back(y1);
        output->clear();
        int last = areaLen - 4;
        for (int i = 0; ; i += 2) {
            float edgeX = area[i], edgeY = area[i+1];
            float ex = edgeX - area[i+2], ey = edgeY - area[i+3];
            size_t outputStart = output->size();
            std::vector<float>& iv = *input;
            for (size_t ii = 0, nn = iv.size()-2; ii < nn; ) {
                float inputX = iv[ii], inputY = iv[ii+1];
                ii += 2;
                float inputX2 = iv[ii], inputY2 = iv[ii+1];
                bool s2 = ey*(edgeX-inputX2) > ex*(edgeY-inputY2);
                float s1 = ey*(edgeX-inputX) - ex*(edgeY-inputY);
                if (s1 > 0) {
                    if (s2) { output->push_back(inputX2); output->push_back(inputY2); continue; }
                    float ix = inputX2-inputX, iy = inputY2-inputY, t = s1/(ix*ey - iy*ex);
                    if (t >= 0 && t <= 1) { output->push_back(inputX+ix*t); output->push_back(inputY+iy*t); }
                    else { output->push_back(inputX2); output->push_back(inputY2); continue; }
                } else if (s2) {
                    float ix = inputX2-inputX, iy = inputY2-inputY, t = s1/(ix*ey - iy*ex);
                    if (t >= 0 && t <= 1) {
                        output->push_back(inputX+ix*t); output->push_back(inputY+iy*t);
                        output->push_back(inputX2); output->push_back(inputY2);
                    } else { output->push_back(inputX2); output->push_back(inputY2); continue; }
                }
                // 注意:s1<=0 且 !s2(边完全在外)= 不输出任何点,仅标记 clipped(原版如此,勿加 else!)
                clipped = true;
            }
            if (outputStart == output->size()) { originalOutput.clear(); return true; }
            output->push_back((*output)[0]); output->push_back((*output)[1]);
            if (i == last) break;
            std::vector<float>* temp = output; output = input; output->clear(); input = temp;
        }
        if (&originalOutput != output) {
            originalOutput.clear();
            size_t n = output->size()-2;
            for (size_t i = 0; i < n; i++) originalOutput.push_back((*output)[i]);
        } else originalOutput.resize(originalOutput.size()-2);
        return clipped;
    }
};

} // namespace spsk
