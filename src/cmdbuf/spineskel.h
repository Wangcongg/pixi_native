// spineskel.h — Spine 4.2 Skeleton.updateWorldTransform 的 C++ 复刻(Bone + IK + Transform + Path + Physics)。
// 目标:接管 profiler 实测占 ov 95% 的逐骨骼 world matrix 更新(JS 无 JIT 的 Math.cos/sin + 矩阵乘)。
// 严格逐位匹配 spine-pixi-v7.js 的 Bone.updateWorldTransformWith(4.2),否则 __SPSKEL 对拍会挂。
// 所有约束及 Path deform 都进入紧凑描述流；Physics 积分状态直接常驻 Native。
#pragma once
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace sk {

// inherit 模式(与 spine 4.2 枚举一致):
enum { INH_NORMAL=0, INH_ONLY_TRANSLATION=1, INH_NO_ROT_REFL=2, INH_NO_SCALE=3, INH_NO_SCALE_REFL=4 };

// ===== 批量驱动共享缓冲(__skelBatchBuf / __skelComputeAll)布局 =====
// N 个活跃骨架每帧 N 次 __skelComputeEx 跨界 → JS 把全部实例参数打包进本缓冲,一次
// __skelComputeAll(count)。[0..BATCH_HDR_TOTAL_F) 定长记录头×BATCH_MAX;之后是 payload 区
// (JS 按帧 bump 分配 loc/ik/mix/pdyn/inherit/phys/w 段,flush 后整体复位)。宿主 JS 与所有
// 平台宿主必须使用同一组常量。
enum { BATCH_MAX = 512, BATCH_HDR_F = 24,
       BATCH_HDR_TOTAL_F = BATCH_MAX * BATCH_HDR_F,
       BATCH_TOTAL_F = 1 << 20 };
// 记录头字段(全 float;偏移是相对缓冲起点的 float 下标,<2^24 float32 精确):
enum { BH_ID=0, BH_LOC_OFF=1, BH_LOC_LEN=2, BH_IK_OFF=3, BH_IK_LEN=4,
       BH_MIX_OFF=5, BH_MIX_LEN=6, BH_PDYN_OFF=7, BH_PDYN_LEN=8,
       BH_INH_OFF=9, BH_INH_LEN=10, BH_PHYS_OFF=11, BH_PHYS_LEN=12,
       BH_W_OFF=13, BH_W_LEN=14, BH_SX=15, BH_SY=16, BH_X=17, BH_Y=18,
       BH_PHYS_MODE=19, BH_TIME=20, BH_YDOWN=21, BH_DONE=22 };

static const float DEG_RAD = 0.017453292519943295f;   // Math.PI/180
static const float RAD_DEG = 57.29577951308232f;      // 180/Math.PI
static const float HALF_PI = 1.5707963267948966f;

// 骨骼 world:6 float [a,b,c,d,worldX,worldY]。local:7 float [x,y,rotation,scaleX,scaleY,shearX,shearY]。
// parentW=nullptr → root。逐位复刻 Bone.updateWorldTransformWith(4.2)。
static inline void boneWorld(const float* L, int inherit, const float* parentW,
                             float skScaleX, float skScaleY, float skX, float skY, float* out) {
    float x=L[0], y=L[1], rotation=L[2], scaleX=L[3], scaleY=L[4], shearX=L[5], shearY=L[6];
    if (!parentW) {   // root
        float rx = (rotation + shearX) * DEG_RAD;
        float ry = (rotation + 90 + shearY) * DEG_RAD;
        out[0] = cosf(rx) * scaleX * skScaleX;   // a
        out[1] = cosf(ry) * scaleY * skScaleX;   // b
        out[2] = sinf(rx) * scaleX * skScaleY;   // c
        out[3] = sinf(ry) * scaleY * skScaleY;   // d
        out[4] = x * skScaleX + skX;             // worldX
        out[5] = y * skScaleY + skY;             // worldY
        return;
    }
    float pa=parentW[0], pb=parentW[1], pc=parentW[2], pd=parentW[3];
    out[4] = pa * x + pb * y + parentW[4];       // worldX
    out[5] = pc * x + pd * y + parentW[5];       // worldY
    switch (inherit) {
        case INH_NORMAL: {
            float rx = (rotation + shearX) * DEG_RAD;
            float ry = (rotation + 90 + shearY) * DEG_RAD;
            float la = cosf(rx) * scaleX, lb = cosf(ry) * scaleY;
            float lc = sinf(rx) * scaleX, ld = sinf(ry) * scaleY;
            out[0] = pa*la + pb*lc;  out[1] = pa*lb + pb*ld;
            out[2] = pc*la + pd*lc;  out[3] = pc*lb + pd*ld;
            return;   // Normal 直接返回,不乘尾部 skeleton scale(父已含)
        }
        case INH_ONLY_TRANSLATION: {
            float rx = (rotation + shearX) * DEG_RAD;
            float ry = (rotation + 90 + shearY) * DEG_RAD;
            out[0] = cosf(rx) * scaleX;  out[1] = cosf(ry) * scaleY;
            out[2] = sinf(rx) * scaleX;  out[3] = sinf(ry) * scaleY;
            break;
        }
        case INH_NO_ROT_REFL: {
            float sx = 1.0f / skScaleX, sy = 1.0f / skScaleY;
            pa *= sx; pc *= sy;
            float s = pa*pa + pc*pc, prx;
            if (s > 1e-4f) {
                s = fabsf(pa*pd*sy - pb*sx*pc) / s;
                pb = pc * s; pd = pa * s;
                prx = atan2f(pc, pa) * RAD_DEG;
            } else {
                pa = 0; pc = 0;
                prx = 90 - atan2f(pd, pb) * RAD_DEG;
            }
            float rx = (rotation + shearX - prx) * DEG_RAD;
            float ry = (rotation + shearY - prx + 90) * DEG_RAD;
            float la = cosf(rx) * scaleX, lb = cosf(ry) * scaleY;
            float lc = sinf(rx) * scaleX, ld = sinf(ry) * scaleY;
            out[0] = pa*la - pb*lc;  out[1] = pa*lb - pb*ld;
            out[2] = pc*la + pd*lc;  out[3] = pc*lb + pd*ld;
            break;
        }
        case INH_NO_SCALE:
        case INH_NO_SCALE_REFL: {
            float rot = rotation * DEG_RAD;
            float cs = cosf(rot), sn = sinf(rot);
            float za = (pa*cs + pb*sn) / skScaleX;
            float zc = (pc*cs + pd*sn) / skScaleY;
            float s = sqrtf(za*za + zc*zc);
            if (s > 1e-5f) s = 1.0f / s;
            za *= s; zc *= s;
            s = sqrtf(za*za + zc*zc);
            if (inherit == INH_NO_SCALE && (pa*pd - pb*pc < 0) != ((skScaleX < 0) != (skScaleY < 0))) s = -s;
            float rot2 = HALF_PI + atan2f(zc, za);
            float zb = cosf(rot2) * s, zd = sinf(rot2) * s;
            float shx = shearX * DEG_RAD;
            float shy = (90 + shearY) * DEG_RAD;
            float la = cosf(shx) * scaleX, lb = cosf(shy) * scaleY;
            float lc = sinf(shx) * scaleX, ld = sinf(shy) * scaleY;
            out[0] = za*la + zb*lc;  out[1] = za*lb + zb*ld;
            out[2] = zc*la + zd*lc;  out[3] = zc*lb + zd*ld;
            break;
        }
    }
    // 非 Normal 分支(1/2/3/4)尾部统一乘 skeleton scale
    out[0] *= skScaleX; out[1] *= skScaleX;
    out[2] *= skScaleY; out[3] *= skScaleY;
}

static const float PI = 3.14159265358979323846f;
static const float PI2 = 6.28318530717958647692f;

// cache 项类型:
enum { CE_BONE=0, CE_TRANSFORM=1, CE_PATH=2, CE_IK=3, CE_PHYSICS=4 };
// rotateMode / spacingMode / positionMode(与 spine 4.2 枚举一致)
enum { RM_TANGENT=0, RM_CHAIN=1, RM_CHAIN_SCALE=2 };
enum { SM_LENGTH=0, SM_FIXED=1, SM_PERCENT=2, SM_PROPORTIONAL=3 };
enum { PM_FIXED=0, PM_PERCENT=1 };

// Path 约束静态数据（constantSpeed 两种弧长算法 + weighted/unweighted deform）。
struct PathC {
    int slotBone = -1;                 // 目标 slot 的骨骼下标(unweighted 顶点用)
    std::vector<float> verts;          // unweighted:[x,y...];weighted:[vx,vy,w...] 交错
    std::vector<int32_t> vbones;       // weighted:[n,b...,n,b...];unweighted 空
    int worldVerticesLength = 0;
    bool closed = false;
    bool constantSpeed = false;
    int dynOffset = 0;                    // pathDyn 内 [5 个动态值,deformN,deform...]
    int deformCapacity = 0;
    std::vector<float> lengths;        // 静态 setup 弧长(curveCount 个)
    int positionMode=0, spacingMode=0, rotateMode=0;
    float offsetRotation = 0;          // 静态(度)
    int targetBone = -1;               // this.target.bone(reflect + tip 用)
    std::vector<int> bones;            // 受约束骨骼下标
    std::vector<float> boneLen;        // 每受约束骨骼 setup length(spacing 用)
};

// Transform 约束静态数据：absolute/relative × world/local 四种 4.2 模式都在 Native 计算。
struct TransformC {
    int target = -1;                // 目标骨骼下标
    std::vector<int> bones;         // 受约束骨骼下标
    float offRot=0, offX=0, offY=0, offSX=0, offSY=0, offShY=0;   // 静态 offset(rot/shear 为度)
    bool local=false, relative=false;
};

struct IkC {
    int target = -1;
    bool uniform = false;
    std::vector<int> bones;
    std::vector<float> lengths;
};

struct PhysicsC {
    int bone = -1;
    float x=0,y=0,rotate=0,scaleX=0,shearX=0,limit=0,step=0,length=0,referenceScale=100;
    bool resetState=true;
    float remaining=0,lastTime=0,ux=0,uy=0,cx=0,cy=0,tx=0,ty=0;
    float xOffset=0,xVelocity=0,yOffset=0,yVelocity=0,rotateOffset=0,rotateVelocity=0,scaleOffset=0,scaleVelocity=0;
    void reset(float time){remaining=0;lastTime=time;resetState=true;xOffset=xVelocity=yOffset=yVelocity=rotateOffset=rotateVelocity=scaleOffset=scaleVelocity=0;}
    void translate(float dx,float dy){ux-=dx;uy-=dy;cx-=dx;cy-=dy;}
    void rotateAround(float x0,float y0,float degrees){float r=degrees*DEG_RAD,cs=cosf(r),sn=sinf(r),dx=cx-x0,dy=cy-y0;translate(dx*cs-dy*sn-dx,dx*sn+dy*cs-dy);}
};

// 每骨架静态数据(注册一次):骨骼 parent/inherit + updateCache 混排(骨骼+约束,拓扑序)。
struct SkelReg {
    int boneCount = 0, cacheN = 0;
    std::vector<int> parent;        // 每骨骼父下标(-1=root)
    std::vector<uint8_t> inherit;   // 每骨骼 inherit 模式 0-4
    std::vector<uint8_t> cacheType; // 每 cache 项类型 CE_*
    std::vector<int> cacheRef;      // 骨骼下标 或 约束下标
    std::vector<TransformC> tcs;    // Transform 约束
    std::vector<PathC> pcs;         // Path 约束
    std::vector<IkC> iks;           // 1/2-bone IK 约束
    std::vector<PhysicsC> physics;  // 有状态 Physics 约束
    int pathDynSize = 0;
};

// Pixi Spine slotObject 不是骨架网格的一部分，而是挂在某个 slot/bone 上的普通
// display node（按钮、数字、红点等）。只保留数字句柄和骨骼下标，世界矩阵由
// Skeleton.compute 产出后直接写入 Native SceneGraph，不再回到 JS 遍历 bone 对象。
struct SlotBinding {
    int nodeId = 0;
    int boneIndex = -1;
};

class SkelSet {
public:
    std::unordered_map<int, SkelReg> regs;
    std::unordered_map<int, std::vector<SlotBinding>> slotBindings;

    // desc:自描述 float 缓冲 [boneCount, cacheN, tcN, pcN, ikN, physicsN, parent×n, inherit×n, cacheType×cacheN, cacheRef×cacheN,
    //   (Transform: target, offRot,offX,offY,offSX,offSY,offShY, local,relative, bc, boneIdx...)...,
    //   (Path: slotBone, worldVerticesLength, closed, constantSpeed, positionMode, spacingMode, rotateMode, offsetRotation, targetBone, deformCapacity,
    //          lengthsN, lengths..., vertsN, verts..., vbonesN, vbones..., bc, (boneIdx, boneLen)...)...,
    //   (Physics: bone,x,y,rotate,scaleX,shearX,limit,step,length,referenceScale)...]
    void reg(int instId, const float* d, int len) {
        if (len < 6) return;
        int i = 0;
        SkelReg r;
        r.boneCount = (int)d[i++]; r.cacheN = (int)d[i++]; int tcN = (int)d[i++]; int pcN = (int)d[i++]; int ikN = (int)d[i++];int physicsN=(int)d[i++];
        int n = r.boneCount;
        if (n <= 0 || n > 4096 || r.cacheN < 0 || r.cacheN > 8192) return;
        r.parent.resize(n); for (int k=0;k<n;k++) r.parent[k] = (int)d[i++];
        r.inherit.resize(n); for (int k=0;k<n;k++) r.inherit[k] = (uint8_t)d[i++];
        r.cacheType.resize(r.cacheN); for (int k=0;k<r.cacheN;k++) r.cacheType[k] = (uint8_t)d[i++];
        r.cacheRef.resize(r.cacheN);  for (int k=0;k<r.cacheN;k++) r.cacheRef[k] = (int)d[i++];
        r.tcs.resize(tcN);
        for (int t=0;t<tcN;t++) {
            TransformC& c = r.tcs[t];
            c.target=(int)d[i++]; c.offRot=d[i++]; c.offX=d[i++]; c.offY=d[i++];
            c.offSX=d[i++]; c.offSY=d[i++]; c.offShY=d[i++];
            c.local=d[i++]!=0;c.relative=d[i++]!=0;
            int bc=(int)d[i++]; c.bones.resize(bc);
            for (int k=0;k<bc;k++) c.bones[k]=(int)d[i++];
        }
        r.pcs.resize(pcN);int pathDynCursor=0;
        for (int t=0;t<pcN;t++) {
            PathC& c = r.pcs[t];
            c.slotBone=(int)d[i++]; c.worldVerticesLength=(int)d[i++]; c.closed=d[i++]!=0; c.constantSpeed=d[i++]!=0;
            c.positionMode=(int)d[i++]; c.spacingMode=(int)d[i++]; c.rotateMode=(int)d[i++];
            c.offsetRotation=d[i++]; c.targetBone=(int)d[i++];c.deformCapacity=std::max(0,(int)d[i++]);c.dynOffset=pathDynCursor;pathDynCursor+=6+c.deformCapacity;
            int ln=(int)d[i++]; c.lengths.resize(ln); for(int k=0;k<ln;k++) c.lengths[k]=d[i++];
            int vn=(int)d[i++]; c.verts.resize(vn); for(int k=0;k<vn;k++) c.verts[k]=d[i++];
            int vbn=(int)d[i++]; c.vbones.resize(vbn); for(int k=0;k<vbn;k++) c.vbones[k]=(int32_t)d[i++];
            int bc=(int)d[i++]; c.bones.resize(bc); c.boneLen.resize(bc);
            for (int k=0;k<bc;k++){ c.bones[k]=(int)d[i++]; c.boneLen[k]=d[i++]; }
        }
        r.pathDynSize=pathDynCursor;
        r.iks.resize(ikN);
        for (int t=0;t<ikN;t++) {
            IkC& c=r.iks[t];c.target=(int)d[i++];c.uniform=d[i++]!=0;int bc=(int)d[i++];
            c.bones.resize(bc);c.lengths.resize(bc);
            for(int k=0;k<bc;k++){c.bones[k]=(int)d[i++];c.lengths[k]=d[i++];}
        }
        r.physics.resize(physicsN);
        for(int t=0;t<physicsN;t++){PhysicsC& c=r.physics[t];c.bone=(int)d[i++];c.x=d[i++];c.y=d[i++];c.rotate=d[i++];c.scaleX=d[i++];c.shearX=d[i++];c.limit=d[i++];c.step=d[i++];c.length=d[i++];c.referenceScale=d[i++];}
        regs[instId] = std::move(r);
    }
    void clear(int instId) { regs.erase(instId); slotBindings.erase(instId); }

    bool bindSlot(int instId, int nodeId, int boneIndex) {
        auto ri=regs.find(instId);
        if(ri==regs.end() || nodeId<=0 || boneIndex<0 || boneIndex>=ri->second.boneCount)return false;
        std::vector<SlotBinding>& list=slotBindings[instId];
        for(size_t i=0;i<list.size();++i){
            if(list[i].nodeId==nodeId){list[i].boneIndex=boneIndex;return true;}
        }
        list.push_back({nodeId,boneIndex});return true;
    }
    void clearSlots(int instId) { slotBindings.erase(instId); }
    const std::vector<SlotBinding>* slots(int instId) const {
        auto it=slotBindings.find(instId);return it==slotBindings.end()?nullptr:&it->second;
    }
    bool physicsReset(int instId,int index,float time){auto it=regs.find(instId);if(it==regs.end()||index<0||index>=(int)it->second.physics.size())return false;it->second.physics[index].reset(time);return true;}
    bool physicsTranslate(int instId,int index,float x,float y){auto it=regs.find(instId);if(it==regs.end()||index<0||index>=(int)it->second.physics.size())return false;it->second.physics[index].translate(x,y);return true;}
    bool physicsRotate(int instId,int index,float x,float y,float degrees){auto it=regs.find(instId);if(it==regs.end()||index<0||index>=(int)it->second.physics.size())return false;it->second.physics[index].rotateAround(x,y,degrees);return true;}

    // World constraint 修改矩阵后，后续 local constraint/IK 可能会继续读 applied pose。
    // 逐位复刻 Bone.updateAppliedTransform，避免把后续约束留在 JS。
    void updateApplied(const SkelReg& r,int bi,const float* out,float* applied,float skSX,float skSY,float skX,float skY,const float* original){
        if(bi<0||bi>=r.boneCount)return;const float* B=out+bi*6;float* L=applied+bi*7;int pi=r.parent[bi];
        if(pi<0){L[0]=B[4]-skX;L[1]=B[5]-skY;L[2]=atan2f(B[2],B[0])*RAD_DEG;L[3]=sqrtf(B[0]*B[0]+B[2]*B[2]);L[4]=sqrtf(B[1]*B[1]+B[3]*B[3]);L[5]=0;L[6]=atan2f(B[0]*B[1]+B[2]*B[3],B[0]*B[3]-B[1]*B[2])*RAD_DEG;return;}
        const float* P=out+pi*6;float pa=P[0],pb=P[1],pc=P[2],pd=P[3],det=pa*pd-pb*pc,pid=fabsf(det)>1e-12f?1.0f/det:0,ia=pd*pid,ib=pb*pid,ic=pc*pid,id=pa*pid;
        float dx=B[4]-P[4],dy=B[5]-P[5];L[0]=dx*ia-dy*ib;L[1]=dy*id-dx*ic;float ra,rb,rc,rd;
        if(r.inherit[bi]==INH_ONLY_TRANSLATION){ra=B[0];rb=B[1];rc=B[2];rd=B[3];}
        else{
            if(r.inherit[bi]==INH_NO_ROT_REFL){float den=pa*pa+pc*pc,s2=den>1e-12f?fabsf(det)/den:0;pb=-pc*skSX*s2/skSY;pd=pa*skSY*s2/skSX;det=pa*pd-pb*pc;pid=fabsf(det)>1e-12f?1.0f/det:0;ia=pd*pid;ib=pb*pid;}
            else if(r.inherit[bi]==INH_NO_SCALE||r.inherit[bi]==INH_NO_SCALE_REFL){float rot=original[bi*7+2]*DEG_RAD,cs=cosf(rot),sn=sinf(rot);pa=(pa*cs+pb*sn)/skSX;pc=(pc*cs+pd*sn)/skSY;float s=sqrtf(pa*pa+pc*pc);if(s>1e-5f)s=1/s;pa*=s;pc*=s;s=sqrtf(pa*pa+pc*pc);if(r.inherit[bi]==INH_NO_SCALE&&((pid<0)!=((skSX<0)!=(skSY<0))))s=-s;float rr=HALF_PI+atan2f(pc,pa);pb=cosf(rr)*s;pd=sinf(rr)*s;det=pa*pd-pb*pc;pid=fabsf(det)>1e-12f?1.0f/det:0;ia=pd*pid;ib=pb*pid;ic=pc*pid;id=pa*pid;}
            ra=ia*B[0]-ib*B[2];rb=ia*B[1]-ib*B[3];rc=id*B[2]-ic*B[0];rd=id*B[3]-ic*B[1];
        }
        L[5]=0;L[3]=sqrtf(ra*ra+rc*rc);if(L[3]>1e-4f){float d2=ra*rd-rb*rc;L[4]=d2/L[3];L[6]=-atan2f(ra*rb+rc*rd,d2)*RAD_DEG;L[2]=atan2f(rc,ra)*RAD_DEG;}else{L[3]=0;L[4]=sqrtf(rb*rb+rd*rd);L[6]=0;L[2]=90-atan2f(rd,rb)*RAD_DEG;}
    }

    // TransformConstraint 4.2: absolute/relative × world/local 四种模式都在 Native 完成。
    void applyTransform(const SkelReg& r,const TransformC& c,const float* mix,float* applied,float* out,float skSX,float skSY,float skX,float skY,const float* original) {
        float mixRotate=mix[0], mixX=mix[1], mixY=mix[2], mixSX=mix[3], mixSY=mix[4], mixShY=mix[5];
        if (mixRotate==0 && mixX==0 && mixY==0 && mixSX==0 && mixSY==0 && mixShY==0) return;
        if(c.target<0||c.target>=r.boneCount)return;
        if(c.local){const float* T=applied+c.target*7;
            for(size_t k=0;k<c.bones.size();++k){int bi=c.bones[k];if(bi<0||bi>=r.boneCount)continue;float* L=applied+bi*7;
                if(c.relative){L[2]+=(T[2]+c.offRot)*mixRotate;L[0]+=(T[0]+c.offX)*mixX;L[1]+=(T[1]+c.offY)*mixY;L[3]*=(T[3]-1+c.offSX)*mixSX+1;L[4]*=(T[4]-1+c.offSY)*mixSY+1;L[6]+=(T[6]+c.offShY)*mixShY;}
                else{float rotation=L[2];rotation+=(T[2]-rotation+c.offRot)*mixRotate;float x=L[0]+(T[0]-L[0]+c.offX)*mixX,y=L[1]+(T[1]-L[1]+c.offY)*mixY,sx=L[3],sy=L[4];if(mixSX!=0&&sx!=0)sx=(sx+(T[3]-sx+c.offSX)*mixSX)/sx;if(mixSY!=0&&sy!=0)sy=(sy+(T[4]-sy+c.offSY)*mixSY)/sy;float shy=L[6]+(T[6]-L[6]+c.offShY)*mixShY;L[0]=x;L[1]=y;L[2]=rotation;L[3]=sx;L[4]=sy;L[6]=shy;}
                int pi=r.parent[bi];const float* P=pi>=0?out+pi*6:nullptr;boneWorld(L,r.inherit[bi],P,skSX,skSY,skX,skY,out+bi*6);
            }return;
        }
        bool translate = mixX!=0 || mixY!=0;
        const float* T = out + c.target*6;
        float ta=T[0], tb=T[1], tc_=T[2], td=T[3], twx=T[4], twy=T[5];
        float degRadReflect = (ta*td - tb*tc_ > 0) ? DEG_RAD : -DEG_RAD;
        float offRot = c.offRot * degRadReflect;
        float offShY = c.offShY * degRadReflect;
        for (size_t bi=0; bi<c.bones.size(); bi++) {
            float* B = out + c.bones[bi]*6;
            if (mixRotate != 0) {
                float a=B[0], b=B[1], cc=B[2], dd=B[3];
                float rr = atan2f(tc_, ta) - (c.relative?0:atan2f(cc, a)) + offRot;
                if (rr > PI) rr -= PI2; else if (rr < -PI) rr += PI2;
                rr *= mixRotate;
                float cs=cosf(rr), sn=sinf(rr);
                B[0]=cs*a - sn*cc; B[1]=cs*b - sn*dd; B[2]=sn*a + cs*cc; B[3]=sn*b + cs*dd;
            }
            if (translate) {   // target.localToWorld(offX, offY)
                float wx = c.offX*ta + c.offY*tb + twx;
                float wy = c.offX*tc_ + c.offY*td + twy;
                B[4] += (c.relative?wx:(wx-B[4])) * mixX;
                B[5] += (c.relative?wy:(wy-B[5])) * mixY;
            }
            if (mixSX != 0) {
                float s;if(c.relative)s=(sqrtf(ta*ta+tc_*tc_)-1+c.offSX)*mixSX+1;else{s=sqrtf(B[0]*B[0]+B[2]*B[2]);if(s!=0)s=(s+(sqrtf(ta*ta+tc_*tc_)-s+c.offSX)*mixSX)/s;}
                B[0] *= s; B[2] *= s;
            }
            if (mixSY != 0) {
                float s;if(c.relative)s=(sqrtf(tb*tb+td*td)-1+c.offSY)*mixSY+1;else{s=sqrtf(B[1]*B[1]+B[3]*B[3]);if(s!=0)s=(s+(sqrtf(tb*tb+td*td)-s+c.offSY)*mixSY)/s;}
                B[1] *= s; B[3] *= s;
            }
            if (mixShY > 0) {
                float b=B[1], dd=B[3];
                float by = atan2f(dd, b);
                float rr=atan2f(td,tb)-atan2f(tc_,ta);if(rr>PI)rr-=PI2;else if(rr<-PI)rr+=PI2;if(c.relative)rr=by+(rr-HALF_PI+offShY)*mixShY;else{rr-=(by-atan2f(B[2],B[0]));if(rr>PI)rr-=PI2;else if(rr<-PI)rr+=PI2;rr=by+(rr+offShY)*mixShY;}
                float s = sqrtf(b*b + dd*dd);
                B[1] = cosf(rr) * s; B[3] = sinf(rr) * s;
            }
            updateApplied(r,c.bones[bi],out,applied,skSX,skSY,skX,skY,original);
        }
    }

    // ===== Path 约束（constantSpeed 两分支，逐位复刻 4.2 PathConstraint）=====
    std::vector<float> spaces_, positions_, world_, plengths_, curves_, segments_;   // scratch(复用免每帧 alloc)

    // 路径世界控制点(VertexAttachment.computeWorldVertices;从 out 骨骼矩阵算，含 deform)
    void pathVerts(const PathC& p, int start, int count, float* wv, int offset, int stride, const float* bout, const float* deform, int deformN) {
        count = offset + (count >> 1) * stride;
        int vn = (int)p.verts.size(), wcap=(int)world_.size();
        if (count > wcap) count = wcap;   // 防写越界(world 缓冲固定 8)
        if (p.vbones.empty()) {   // unweighted
            if (p.slotBone < 0) return;
            const float* B = bout + p.slotBone*6;
            float a=B[0],b=B[1],c=B[2],d=B[3],x=B[4],y=B[5];
            for (int v2=start, w=offset; w<count; v2+=2, w+=stride) {
                if (v2 < 0 || v2+1 >= vn) break;   // 防读越界
                float vx=(deform&&v2+1<deformN)?deform[v2]:p.verts[v2], vy=(deform&&v2+1<deformN)?deform[v2+1]:p.verts[v2+1];
                wv[w]=vx*a+vy*b+x; wv[w+1]=vx*c+vy*d+y;
            }
            return;
        }
        int v=0, skip=0, vbn=(int)p.vbones.size();
        for (int i=0;i<start && v<vbn;i+=2){ int n=p.vbones[v]; v+=n+1; skip+=n; }
        for (int w=offset, b=skip*3; w<count; w+=stride) {
            if (v >= vbn) break;
            float wx=0, wy=0; int n=p.vbones[v++]; n+=v;
            for (; v<n && v<vbn; v++, b+=3) {
                if (b+2 >= vn) break;
                int bi=p.vbones[v]; if (bi<0) continue;
                const float* BB = bout + bi*6;
                int f=(b/3)*2;float vx=p.verts[b]+((deform&&f+1<deformN)?deform[f]:0), vy=p.verts[b+1]+((deform&&f+1<deformN)?deform[f+1]:0), weight=p.verts[b+2];
                wx += (vx*BB[0] + vy*BB[1] + BB[4]) * weight;
                wy += (vx*BB[2] + vy*BB[3] + BB[5]) * weight;
            }
            wv[w]=wx; wv[w+1]=wy;
        }
    }
    static void addBeforePos(float pp, const float* t, int i, float* out, int o) {
        float x1=t[i], y1=t[i+1], dx=t[i+2]-x1, dy=t[i+3]-y1, r=atan2f(dy,dx);
        out[o]=x1+pp*cosf(r); out[o+1]=y1+pp*sinf(r); out[o+2]=r;
    }
    static void addAfterPos(float pp, const float* t, int i, float* out, int o) {
        float x1=t[i+2], y1=t[i+3], dx=x1-t[i], dy=y1-t[i+1], r=atan2f(dy,dx);
        out[o]=x1+pp*cosf(r); out[o+1]=y1+pp*sinf(r); out[o+2]=r;
    }
    static void addCurvePos(float pp, float x1,float y1,float cx1,float cy1,float cx2,float cy2,float x2,float y2, float* out, int o, bool tangents) {
        if (pp==0 || pp!=pp) { out[o]=x1; out[o+1]=y1; out[o+2]=atan2f(cy1-y1, cx1-x1); return; }
        float tt=pp*pp, ttt=tt*pp, u=1-pp, uu=u*u, uuu=uu*u;
        float ut=u*pp, ut3=ut*3, uut3=u*ut3, utt3=ut3*pp;
        float x=x1*uuu+cx1*uut3+cx2*utt3+x2*ttt, y=y1*uuu+cy1*uut3+cy2*utt3+y2*ttt;
        out[o]=x; out[o+1]=y;
        if (tangents) {
            if (pp<1e-3f) out[o+2]=atan2f(cy1-y1, cx1-x1);
            else out[o+2]=atan2f(y-(y1*uu+cy1*ut*2+cy2*tt), x-(x1*uu+cx1*ut*2+cx2*tt));
        }
    }
    // computeWorldPositions:写 positions_(spacesCount*3+2)
    void pathPositions(const PathC& p, int spacesCount, bool tangents, float position, const float* bout, const float* deform, int deformN) {
        int verticesLength = p.worldVerticesLength, curveCount = verticesLength/6, prevCurve = -1;   // NONE
        if (spacesCount <= 0) return;
        positions_.assign(spacesCount*3+2, 0.0f);
        float* out = positions_.data(); bool closed = p.closed; const float* spaces = spaces_.data();
        if (!p.constantSpeed) {
            if ((int)world_.size() < 8) world_.assign(8, 0.0f);
            float* world = world_.data(); const float* lengths = p.lengths.data(); int ln = (int)p.lengths.size();
            curveCount -= closed ? 1 : 2;
            if (curveCount < 0 || curveCount >= ln) return;
            float pathLength = lengths[curveCount]; if (!(pathLength > 0)) return;
            if (p.positionMode == PM_PERCENT) position *= pathLength;
            float multiplier;
            switch (p.spacingMode) { case SM_PERCENT: multiplier=pathLength;break; case SM_PROPORTIONAL: multiplier=pathLength/spacesCount;break; default: multiplier=1; }
            int curve=0;
            for (int i=0,o=0;i<spacesCount;i++,o+=3) {
                float space=spaces[i]*multiplier;position+=space;float pp=position;
                if(closed){pp=fmodf(pp,pathLength);if(pp<0)pp+=pathLength;curve=0;}
                else if(pp<0){if(prevCurve!=-2){prevCurve=-2;pathVerts(p,2,4,world,0,2,bout,deform,deformN);}addBeforePos(pp,world,0,out,o);continue;}
                else if(pp>pathLength){if(prevCurve!=-3){prevCurve=-3;pathVerts(p,verticesLength-6,4,world,0,2,bout,deform,deformN);}addAfterPos(pp-pathLength,world,0,out,o);continue;}
                for(;;curve++){if(curve>=curveCount){curve=curveCount;if(curve==0){float d=lengths[0];pp=d!=0?pp/d:0;}else{float prev=lengths[curve-1],d=lengths[curve]-prev;pp=d!=0?(pp-prev)/d:0;}break;}float length=lengths[curve];if(pp>length)continue;if(curve==0)pp=length!=0?pp/length:0;else{float prev=lengths[curve-1],d=length-prev;pp=d!=0?(pp-prev)/d:0;}break;}
                if(curve!=prevCurve){prevCurve=curve;if(closed&&curve==curveCount){pathVerts(p,verticesLength-4,4,world,0,2,bout,deform,deformN);pathVerts(p,0,4,world,4,2,bout,deform,deformN);}else pathVerts(p,curve*6+2,8,world,0,2,bout,deform,deformN);}
                addCurvePos(pp,world[0],world[1],world[2],world[3],world[4],world[5],world[6],world[7],out,o,tangents||(i>0&&space==0));
            }
            return;
        }

        // constantSpeed=true: 先用 4 段对每条 Bezier 求粗弧长，再用 10 段将当前曲线弧长反解为 t。
        if (closed) {
            verticesLength += 2; world_.assign(verticesLength,0.0f);float* world=world_.data();
            pathVerts(p,2,verticesLength-4,world,0,2,bout,deform,deformN);pathVerts(p,0,2,world,verticesLength-4,2,bout,deform,deformN);
            world[verticesLength-2]=world[0];world[verticesLength-1]=world[1];
        } else {
            curveCount--;verticesLength-=4;if(curveCount<=0||verticesLength<8)return;
            world_.assign(verticesLength,0.0f);pathVerts(p,2,verticesLength,world_.data(),0,2,bout,deform,deformN);
        }
        float* world=world_.data();if(curveCount<=0)return;curves_.assign(curveCount,0.0f);
        float pathLength=0,x1=world[0],y1=world[1],cx1=0,cy1=0,cx2=0,cy2=0,x2=0,y2=0;
        for(int ci=0,w=2;ci<curveCount;ci++,w+=6){cx1=world[w];cy1=world[w+1];cx2=world[w+2];cy2=world[w+3];x2=world[w+4];y2=world[w+5];
            float tx=(x1-cx1*2+cx2)*0.1875f,ty=(y1-cy1*2+cy2)*0.1875f,dddx=((cx1-cx2)*3-x1+x2)*0.09375f,dddy=((cy1-cy2)*3-y1+y2)*0.09375f;
            float ddx=tx*2+dddx,ddy=ty*2+dddy,dx=(cx1-x1)*0.75f+tx+dddx*0.16666667f,dy=(cy1-y1)*0.75f+ty+dddy*0.16666667f;
            pathLength+=sqrtf(dx*dx+dy*dy);dx+=ddx;dy+=ddy;ddx+=dddx;ddy+=dddy;pathLength+=sqrtf(dx*dx+dy*dy);dx+=ddx;dy+=ddy;pathLength+=sqrtf(dx*dx+dy*dy);dx+=ddx+dddx;dy+=ddy+dddy;pathLength+=sqrtf(dx*dx+dy*dy);
            curves_[ci]=pathLength;x1=x2;y1=y2;
        }
        if(!(pathLength>0))return;if(p.positionMode==PM_PERCENT)position*=pathLength;
        float multiplier=p.spacingMode==SM_PERCENT?pathLength:(p.spacingMode==SM_PROPORTIONAL?pathLength/spacesCount:1.0f);
        segments_.assign(10,0.0f);float curveLength=0;int curve=0,segment=0;
        for(int si=0,o=0;si<spacesCount;si++,o+=3){float space=spaces[si]*multiplier;position+=space;float pp=position;
            if(closed){pp=fmodf(pp,pathLength);if(pp<0)pp+=pathLength;curve=0;}else if(pp<0){addBeforePos(pp,world,0,out,o);continue;}else if(pp>pathLength){addAfterPos(pp-pathLength,world,verticesLength-4,out,o);continue;}
            for(;;curve++){if(curve>=curveCount){curve=curveCount-1;pp=1;break;}float length=curves_[curve];if(pp>length)continue;if(curve==0)pp=length!=0?pp/length:0;else{float prev=curves_[curve-1],d=length-prev;pp=d!=0?(pp-prev)/d:0;}break;}
            if(curve!=prevCurve){prevCurve=curve;int wi=curve*6;x1=world[wi];y1=world[wi+1];cx1=world[wi+2];cy1=world[wi+3];cx2=world[wi+4];cy2=world[wi+5];x2=world[wi+6];y2=world[wi+7];
                float tx=(x1-cx1*2+cx2)*0.03f,ty=(y1-cy1*2+cy2)*0.03f,dddx=((cx1-cx2)*3-x1+x2)*0.006f,dddy=((cy1-cy2)*3-y1+y2)*0.006f;
                float ddx=tx*2+dddx,ddy=ty*2+dddy,dx=(cx1-x1)*0.3f+tx+dddx*0.16666667f,dy=(cy1-y1)*0.3f+ty+dddy*0.16666667f;
                curveLength=sqrtf(dx*dx+dy*dy);segments_[0]=curveLength;for(int k=1;k<8;k++){dx+=ddx;dy+=ddy;ddx+=dddx;ddy+=dddy;curveLength+=sqrtf(dx*dx+dy*dy);segments_[k]=curveLength;}dx+=ddx;dy+=ddy;curveLength+=sqrtf(dx*dx+dy*dy);segments_[8]=curveLength;dx+=ddx+dddx;dy+=ddy+dddy;curveLength+=sqrtf(dx*dx+dy*dy);segments_[9]=curveLength;segment=0;
            }
            pp*=curveLength;for(;;segment++){if(segment>=10){segment=9;pp=10;break;}float length=segments_[segment];if(pp>length)continue;if(segment==0)pp=length!=0?pp/length:0;else{float prev=segments_[segment-1],d=length-prev;pp=segment+(d!=0?(pp-prev)/d:0);}break;}
            addCurvePos(pp*0.1f,x1,y1,cx1,cy1,cx2,cy2,x2,y2,out,o,tangents||(si>0&&space==0));
        }
    }
    // PathConstraint.update
    void applyPath(PathC& p, const float* dyn, float* out) {
        float position=dyn[0], spacing=dyn[1], mixRotate=dyn[2], mixX=dyn[3], mixY=dyn[4];
        int deformN=std::max(0,std::min(p.deformCapacity,(int)dyn[5]));const float* deform=deformN>0?dyn+6:nullptr;
        if (mixRotate==0 && mixX==0 && mixY==0) return;
        bool tangents = p.rotateMode==RM_TANGENT, scale = p.rotateMode==RM_CHAIN_SCALE;
        int boneCount = (int)p.bones.size();
        if (boneCount <= 0 || (int)p.boneLen.size() < boneCount) return;
        for (int i=0;i<boneCount;i++) if (p.bones[i] < 0) return;   // 受约束骨骼下标非法(-1)→负下标写内存,跳过整约束
        int spacesCount = tangents ? boneCount : boneCount+1;
        spaces_.assign(spacesCount, 0.0f);
        if (scale) plengths_.assign(boneCount, 0.0f);
        float* spaces = spaces_.data();
        // spacing 模式
        if (p.spacingMode == SM_PERCENT) {
            if (scale) for (int i=0,n=spacesCount-1;i<n;i++){ const float* B=out+p.bones[i]*6; float sl=p.boneLen[i]; float x=sl*B[0], y=sl*B[2]; plengths_[i]=sqrtf(x*x+y*y); }
            for (int i=1;i<spacesCount;i++) spaces[i]=spacing;
        } else if (p.spacingMode == SM_PROPORTIONAL) {
            float sum=0;
            for (int i=0,n=spacesCount-1;i<n;) {
                float sl=p.boneLen[i];
                if (sl < 0.0001f) { if(scale) plengths_[i]=0; spaces[++i]=spacing; }
                else { const float* B=out+p.bones[i]*6; float x=sl*B[0], y=sl*B[2]; float length=sqrtf(x*x+y*y); if(scale) plengths_[i]=length; spaces[++i]=length; sum+=length; }
            }
            if (sum>0) { sum = spacesCount/sum*spacing; for(int i=1;i<spacesCount;i++) spaces[i]*=sum; }
        } else {   // Length(default) / Fixed
            bool lengthSpacing = p.spacingMode == SM_LENGTH;
            for (int i=0,n=spacesCount-1;i<n;) {
                float sl=p.boneLen[i];
                if (sl < 0.0001f) { if(scale) plengths_[i]=0; spaces[++i]=spacing; }
                else { const float* B=out+p.bones[i]*6; float x=sl*B[0], y=sl*B[2]; float length=sqrtf(x*x+y*y); if(scale) plengths_[i]=length; spaces[++i]=(lengthSpacing ? sl+spacing : spacing) * length/sl; }
            }
        }
        pathPositions(p, spacesCount, tangents, position, out, deform, deformN);
        const float* positions = positions_.data();
        float boneX=positions[0], boneY=positions[1], offsetRotation=p.offsetRotation;
        bool tip;
        if (offsetRotation == 0) tip = p.rotateMode == RM_CHAIN;
        else if (p.targetBone < 0) { tip = false; offsetRotation = 0; }
        else { tip = false; const float* pb = out + p.targetBone*6; offsetRotation *= (pb[0]*pb[3] - pb[1]*pb[2] > 0) ? DEG_RAD : -DEG_RAD; }
        for (int i=0, pi=3; i<boneCount; i++, pi+=3) {
            float* B = out + p.bones[i]*6;
            B[4] += (boneX - B[4]) * mixX;
            B[5] += (boneY - B[5]) * mixY;
            float x=positions[pi], y=positions[pi+1], dx=x-boneX, dy=y-boneY;
            if (scale) { float length=plengths_[i]; if(length!=0){ float s=(sqrtf(dx*dx+dy*dy)/length - 1)*mixRotate + 1; B[0]*=s; B[2]*=s; } }
            boneX=x; boneY=y;
            if (mixRotate > 0) {
                float a=B[0], b=B[1], cc=B[2], dd=B[3], r, cs, sn;
                if (tangents) r=positions[pi-1];
                else if (spaces[i+1]==0) r=positions[pi+2];
                else r=atan2f(dy, dx);
                r -= atan2f(cc, a);
                if (tip) {
                    cs=cosf(r); sn=sinf(r); float length=p.boneLen[i];
                    boneX += (length*(cs*a - sn*cc) - dx)*mixRotate;
                    boneY += (length*(sn*a + cs*cc) - dy)*mixRotate;
                } else r += offsetRotation;
                if (r > PI) r -= PI2; else if (r < -PI) r += PI2;
                r *= mixRotate;
                cs=cosf(r); sn=sinf(r);
                B[0]=cs*a - sn*cc; B[1]=cs*b - sn*dd; B[2]=sn*a + cs*cc; B[3]=sn*b + cs*dd;
            }
        }
    }

    // Spine 4.2 PhysicsConstraint.update. Simulation offsets/velocities live in PhysicsC, so JS
    // only supplies animated parameters and the requested Physics mode/time for this tick.
    void applyPhysics(const SkelReg& r, PhysicsC& p, const float* dyn, int mode, float skeletonTime,
                      float* applied, float* out, float skSX, float skSY, float skX, float skY,
                      const float* original, bool yDown) {
        if(!dyn||p.bone<0||p.bone>=r.boneCount)return;
        float inertia=dyn[0],strength=dyn[1],damping=dyn[2],massInverse=dyn[3],wind=dyn[4],gravity=dyn[5],mix=dyn[6];
        if(mix==0||mode==0)return;
        bool useX=p.x>0,useY=p.y>0,rotateOrShear=p.rotate>0||p.shearX>0,scale=p.scaleX>0;
        float* B=out+p.bone*6;float length=p.length;
        if(mode==1){p.reset(skeletonTime);mode=2;}
        if(mode==2){
            float delta=std::max(skeletonTime-p.lastTime,0.0f);p.remaining+=delta;p.lastTime=skeletonTime;
            float bx=B[4],by=B[5];
            if(p.resetState){p.resetState=false;p.ux=bx;p.uy=by;}
            else{
                float remain=p.remaining,ii=inertia,t=p.step,f=p.referenceScale,damp=-1;
                float qx=p.limit*delta,qy=qx*fabsf(skSY);qx*=fabsf(skSX);
                if(useX||useY){
                    if(useX){float u=(p.ux-bx)*ii;p.xOffset+=u>qx?qx:(u<-qx?-qx:u);p.ux=bx;}
                    if(useY){float u=(p.uy-by)*ii;p.yOffset+=u>qy?qy:(u<-qy?-qy:u);p.uy=by;}
                    if(t>0&&remain>=t){damp=powf(damping,60*t);float m=massInverse*t,e=strength,w=wind*f*skSX,g=gravity*f*skSY;do{if(useX){p.xVelocity+=(w-p.xOffset*e)*m;p.xOffset+=p.xVelocity*t;p.xVelocity*=damp;}if(useY){p.yVelocity-=(g+p.yOffset*e)*m;p.yOffset+=p.yVelocity*t;p.yVelocity*=damp;}remain-=t;}while(remain>=t);}
                    if(useX)B[4]+=p.xOffset*mix*p.x;if(useY)B[5]+=p.yOffset*mix*p.y;
                }
                if(rotateOrShear||scale){
                    float ca=atan2f(B[2],B[0]),cs=0,sn=0,mr=0,dx=p.cx-B[4],dy=p.cy-B[5];
                    dx=std::max(-qx,std::min(qx,dx));dy=std::max(-qy,std::min(qy,dy));
                    if(rotateOrShear){mr=(p.rotate+p.shearX)*mix;float rr=atan2f(dy+p.ty,dx+p.tx)-ca-p.rotateOffset*mr;p.rotateOffset+=(rr-ceilf(rr/PI2-0.5f)*PI2)*ii;rr=p.rotateOffset*mr+ca;cs=cosf(rr);sn=sinf(rr);if(scale){rr=length*sqrtf(B[0]*B[0]+B[2]*B[2]);if(rr>0)p.scaleOffset+=(dx*cs+dy*sn)*ii/rr;}}
                    else{cs=cosf(ca);sn=sinf(ca);float rr=length*sqrtf(B[0]*B[0]+B[2]*B[2]);if(rr>0)p.scaleOffset+=(dx*cs+dy*sn)*ii/rr;}
                    remain=p.remaining;
                    if(t>0&&remain>=t){if(damp<0)damp=powf(damping,60*t);float m=massInverse*t,e=strength,w=wind,g=yDown?-gravity:gravity,h=f!=0?length/f:0;for(;;){remain-=t;if(scale){p.scaleVelocity+=(w*cs-g*sn-p.scaleOffset*e)*m;p.scaleOffset+=p.scaleVelocity*t;p.scaleVelocity*=damp;}if(rotateOrShear){p.rotateVelocity-=((w*sn+g*cs)*h+p.rotateOffset*e)*m;p.rotateOffset+=p.rotateVelocity*t;p.rotateVelocity*=damp;if(remain<t)break;float rr=p.rotateOffset*mr+ca;cs=cosf(rr);sn=sinf(rr);}else if(remain<t)break;}}
                }
                p.remaining=remain;
            }
            p.cx=B[4];p.cy=B[5];
        }else if(mode==3){if(useX)B[4]+=p.xOffset*mix*p.x;if(useY)B[5]+=p.yOffset*mix*p.y;}
        if(rotateOrShear){float oo=p.rotateOffset*mix,ss=0,cc=0,aa=0;if(p.shearX>0){float rr=0;if(p.rotate>0){rr=oo*p.rotate;ss=sinf(rr);cc=cosf(rr);aa=B[1];B[1]=cc*aa-ss*B[3];B[3]=ss*aa+cc*B[3];}rr+=oo*p.shearX;ss=sinf(rr);cc=cosf(rr);aa=B[0];B[0]=cc*aa-ss*B[2];B[2]=ss*aa+cc*B[2];}else{oo*=p.rotate;ss=sinf(oo);cc=cosf(oo);aa=B[0];B[0]=cc*aa-ss*B[2];B[2]=ss*aa+cc*B[2];aa=B[1];B[1]=cc*aa-ss*B[3];B[3]=ss*aa+cc*B[3];}}
        if(scale){float ss=1+p.scaleOffset*mix*p.scaleX;B[0]*=ss;B[2]*=ss;}
        if(mode!=3){p.tx=length*B[0];p.ty=length*B[2];}
        updateApplied(r,p.bone,out,applied,skSX,skSY,skX,skY,original);
    }

    void applyIk1(const SkelReg& r, int bi, float txw, float tyw, bool compress, bool stretch, bool uniform,
                  float alpha, float* applied, float* out, float skSX, float skSY, float skX, float skY) {
        if(bi<0||bi>=r.boneCount)return;int pi=r.parent[bi];if(pi<0)return;
        float* L=applied+bi*7;float* B=out+bi*6;const float* P=out+pi*6;
        float pa=P[0],pb=P[1],pc=P[2],pd=P[3],rik=-L[5]-L[2],tx=0,ty=0;
        if(r.inherit[bi]==INH_ONLY_TRANSLATION){tx=(txw-B[4])*(skSX<0?-1.0f:1.0f);ty=(tyw-B[5])*(skSY<0?-1.0f:1.0f);}
        else {
            if(r.inherit[bi]==INH_NO_ROT_REFL){float ss=fabsf(pa*pd-pb*pc)/std::max(1e-4f,pa*pa+pc*pc),sa=pa/skSX,sc=pc/skSY;pb=-sc*ss*skSX;pd=sa*ss*skSY;rik+=atan2f(sc,sa)*RAD_DEG;}
            float x=txw-P[4],y=tyw-P[5],det=pa*pd-pb*pc;
            if(fabsf(det)>1e-4f){tx=(x*pd-y*pb)/det-L[0];ty=(y*pa-x*pc)/det-L[1];}
        }
        rik+=atan2f(ty,tx)*RAD_DEG;if(L[3]<0)rik+=180;if(rik>180)rik-=360;else if(rik<-180)rik+=360;
        float sx=L[3],sy=L[4];
        if(compress||stretch){if(r.inherit[bi]==INH_NO_SCALE||r.inherit[bi]==INH_NO_SCALE_REFL){tx=txw-B[4];ty=tyw-B[5];}
            float len=0;for(const IkC& q:r.iks)for(size_t k=0;k<q.bones.size();k++)if(q.bones[k]==bi){len=q.lengths[k];goto got_len;}got_len:;
            float bl=len*sx;if(bl>1e-4f){float dd=tx*tx+ty*ty;if((compress&&dd<bl*bl)||(stretch&&dd>bl*bl)){float ss=(sqrtf(dd)/bl-1)*alpha+1;sx*=ss;if(uniform)sy*=ss;}}}
        L[2]+=rik*alpha;L[3]=sx;L[4]=sy;
        boneWorld(L,r.inherit[bi],P,skSX,skSY,skX,skY,B);
    }

    void applyIk(const SkelReg& r,const IkC& ik,const float* dyn,float* applied,float* out,float skSX,float skSY,float skX,float skY){
        if(ik.target<0||ik.target>=r.boneCount||ik.bones.empty()||!dyn||dyn[0]==0)return;const float* T=out+ik.target*6;
        float alpha=dyn[0],soft=dyn[1];int bend=dyn[2]<0?-1:1;bool compress=dyn[3]!=0,stretch=dyn[4]!=0;
        if(ik.bones.size()==1){int bi=ik.bones[0];if(bi<0||bi>=r.boneCount)return;float* L=applied+bi*7;const float* B=out+bi*6;int pi=r.parent[bi];if(pi<0)return;const float* P=out+pi*6;
            float pa=P[0],pb=P[1],pc=P[2],pd=P[3],rik=-L[5]-L[2],tx=0,ty=0;
            if(r.inherit[bi]==INH_ONLY_TRANSLATION){tx=(T[4]-B[4])*(skSX<0?-1.0f:1.0f);ty=(T[5]-B[5])*(skSY<0?-1.0f:1.0f);}
            else{if(r.inherit[bi]==INH_NO_ROT_REFL){float ss=fabsf(pa*pd-pb*pc)/std::max(1e-4f,pa*pa+pc*pc),sa=pa/skSX,sc=pc/skSY;pb=-sc*ss*skSX;pd=sa*ss*skSY;rik+=atan2f(sc,sa)*RAD_DEG;}float x=T[4]-P[4],y=T[5]-P[5],det=pa*pd-pb*pc;if(fabsf(det)>1e-4f){tx=(x*pd-y*pb)/det-L[0];ty=(y*pa-x*pc)/det-L[1];}}
            rik+=atan2f(ty,tx)*RAD_DEG;if(L[3]<0)rik+=180;if(rik>180)rik-=360;else if(rik<-180)rik+=360;float sx=L[3],sy=L[4];
            if(compress||stretch){if(r.inherit[bi]==INH_NO_SCALE||r.inherit[bi]==INH_NO_SCALE_REFL){tx=T[4]-B[4];ty=T[5]-B[5];}float bl=(ik.lengths.empty()?0:ik.lengths[0])*sx;if(bl>1e-4f){float dd=tx*tx+ty*ty;if((compress&&dd<bl*bl)||(stretch&&dd>bl*bl)){float ss=(sqrtf(dd)/bl-1)*alpha+1;sx*=ss;if(ik.uniform)sy*=ss;}}}
            L[2]+=rik*alpha;L[3]=sx;L[4]=sy;boneWorld(L,r.inherit[bi],P,skSX,skSY,skX,skY,out+bi*6);return;
        }
        int pbi=ik.bones[0],cbi=ik.bones[1];if(pbi<0||cbi<0||pbi>=r.boneCount||cbi>=r.boneCount||r.inherit[pbi]!=INH_NORMAL||r.inherit[cbi]!=INH_NORMAL)return;
        float* PL=applied+pbi*7,*CL=applied+cbi*7;float* PW=out+pbi*6;int ppi=r.parent[pbi];if(ppi<0)return;const float* PP=out+ppi*6;
        float px=PL[0],py=PL[1],psx=PL[3],psy=PL[4],sx=psx,sy=psy,csx=CL[3],os1=0,os2=0,s2=0;
        if(psx<0){psx=-psx;os1=180;s2=-1;}else{s2=1;}if(psy<0){psy=-psy;s2=-s2;}if(csx<0){csx=-csx;os2=180;}
        float cx=CL[0],cy=0,cwx=0,cwy=0,a=PW[0],bb=PW[1],cc=PW[2],ddm=PW[3];bool uniformScale=fabsf(psx-psy)<=1e-4f;
        if(!uniformScale||stretch){cwx=a*cx+PW[4];cwy=cc*cx+PW[5];}else{cy=CL[1];cwx=a*cx+bb*cy+PW[4];cwy=cc*cx+ddm*cy+PW[5];}
        a=PP[0];bb=PP[1];cc=PP[2];ddm=PP[3];float det=a*ddm-bb*cc,x=cwx-PP[4],y=cwy-PP[5],inv=fabsf(det)<=1e-4f?0:1/det;
        float dx=(x*ddm-y*bb)*inv-px,dy=(y*a-x*cc)*inv-py,l1=sqrtf(dx*dx+dy*dy),l2=(ik.lengths.size()>1?ik.lengths[1]:0)*csx,a1=0,a2=0;
        if(l1<1e-4f){applyIk1(r,pbi,T[4],T[5],false,stretch,false,alpha,applied,out,skSX,skSY,skX,skY);CL[1]=cy;CL[2]=0;boneWorld(CL,r.inherit[cbi],out+pbi*6,skSX,skSY,skX,skY,out+cbi*6);return;}
        x=T[4]-PP[4];y=T[5]-PP[5];float tx=(x*ddm-y*bb)*inv-px,ty=(y*a-x*cc)*inv-py,dist2=tx*tx+ty*ty;
        if(soft!=0){soft*=psx*(csx+1)*.5f;float td=sqrtf(dist2),sd=td-l1-l2*psx+soft;if(sd>0&&td!=0){float q=std::min(1.0f,sd/(soft*2))-1,q2=(sd-soft*(1-q*q))/td;tx-=q2*tx;ty-=q2*ty;dist2=tx*tx+ty*ty;}}
        if(uniformScale){l2*=psx;float cosine=(dist2-l1*l1-l2*l2)/(2*l1*l2);if(cosine<-1){cosine=-1;a2=PI*bend;}else if(cosine>1){cosine=1;a2=0;if(stretch){float q=(sqrtf(dist2)/(l1+l2)-1)*alpha+1;sx*=q;if(ik.uniform)sy*=q;}}else a2=acosf(cosine)*bend;float aa=l1+l2*cosine,b2=l2*sinf(a2);a1=atan2f(ty*aa-tx*b2,tx*aa+ty*b2);}
        else{a=psx*l2;bb=psy*l2;float aa=a*a,b2=bb*bb,ta=atan2f(ty,tx),cv=b2*l1*l1+aa*dist2-aa*b2,c1=-2*b2*l1,c2=b2-aa,disc=c1*c1-4*c2*cv;bool solved=false;if(disc>=0&&c2!=0){float q=sqrtf(disc);if(c1<0)q=-q;q=-(c1+q)*.5f;if(q!=0){float r0=q/c2,r1=cv/q,rr=fabsf(r0)<fabsf(r1)?r0:r1,r02=dist2-rr*rr;if(r02>=0){y=sqrtf(r02)*bend;a1=ta-atan2f(y,rr);a2=atan2f(y/psy,(rr-l1)/psx);solved=true;}}}if(!solved){float minA=PI,minX=l1-a,minD=minX*minX,minY=0,maxA=0,maxX=l1+a,maxD=maxX*maxX,maxY=0;cv=-a*l1/(aa-b2);if(cv>=-1&&cv<=1){cv=acosf(cv);x=a*cosf(cv)+l1;y=bb*sinf(cv);float q=x*x+y*y;if(q<minD){minA=cv;minD=q;minX=x;minY=y;}if(q>maxD){maxA=cv;maxD=q;maxX=x;maxY=y;}}if(dist2<=(minD+maxD)*.5f){a1=ta-atan2f(minY*bend,minX);a2=minA*bend;}else{a1=ta-atan2f(maxY*bend,maxX);a2=maxA*bend;}}}
        float os=atan2f(cy,cx)*s2,rotation=PL[2];a1=(a1-os)*RAD_DEG+os1-rotation;if(a1>180)a1-=360;else if(a1<-180)a1+=360;PL[2]=rotation+a1*alpha;PL[3]=sx;PL[4]=sy;PL[5]=PL[6]=0;boneWorld(PL,r.inherit[pbi],PP,skSX,skSY,skX,skY,PW);
        rotation=CL[2];a2=((a2+os)*RAD_DEG-CL[5])*s2+os2-rotation;if(a2>180)a2-=360;else if(a2<-180)a2+=360;CL[0]=cx;CL[1]=cy;CL[2]=rotation+a2*alpha;boneWorld(CL,r.inherit[cbi],PW,skSX,skSY,skX,skY,out+cbi*6);
    }

    std::vector<float> original_, applied_;

    // locals: boneCount×7(in/out applied pose);ikDyn: ikN×5;mixes: tcN×6;pathDyn: PathC::dynOffset 变长记录;physicsDyn: physicsN×7。
    // out: boneCount×6。按 cacheOrder 混排算。
    void compute(int instId, float* locals, const float* ikDyn, const float* mixes, const float* pathDyn, const float* physicsDyn,
                 float* out, float skSX, float skSY, float skX, float skY, const float* inheritDyn=nullptr,
                 int physicsMode=2, float skeletonTime=0, bool yDown=false) {
        std::unordered_map<int, SkelReg>::iterator it = regs.find(instId);
        if (it == regs.end()) return;
        SkelReg& r = it->second;
        if(inheritDyn)for(int i=0;i<r.boneCount;i++)r.inherit[i]=(uint8_t)std::max(0,std::min(4,(int)inheritDyn[i]));
        original_.assign(locals,locals+r.boneCount*7);applied_=original_;
        for (int ci = 0; ci < r.cacheN; ci++) {
            int ref = r.cacheRef[ci];
            if (r.cacheType[ci] == CE_BONE) {
                if (ref < 0 || ref >= r.boneCount) continue;
                for(int k=0;k<7;k++)applied_[ref*7+k]=original_[ref*7+k];
                int p = r.parent[ref];
                const float* pw = (p >= 0 && p < r.boneCount) ? (out + p*6) : nullptr;
                boneWorld(applied_.data() + ref*7, r.inherit[ref], pw, skSX, skSY, skX, skY, out + ref*6);
            } else if (r.cacheType[ci] == CE_TRANSFORM) {
                if (ref >= 0 && ref < (int)r.tcs.size()) applyTransform(r,r.tcs[ref],mixes+ref*6,applied_.data(),out,skSX,skSY,skX,skY,original_.data());
            } else if (r.cacheType[ci] == CE_PATH) {
                if (pathDyn && ref >= 0 && ref < (int)r.pcs.size()) applyPath(r.pcs[ref], pathDyn + r.pcs[ref].dynOffset, out);
            } else if (r.cacheType[ci] == CE_IK) {
                if(ikDyn&&ref>=0&&ref<(int)r.iks.size())applyIk(r,r.iks[ref],ikDyn+ref*5,applied_.data(),out,skSX,skSY,skX,skY);
            } else if(r.cacheType[ci]==CE_PHYSICS){
                if(physicsDyn&&ref>=0&&ref<(int)r.physics.size())applyPhysics(r,r.physics[ref],physicsDyn+ref*7,physicsMode,skeletonTime,applied_.data(),out,skSX,skSY,skX,skY,original_.data(),yDown);
            }
        }
        std::copy(applied_.begin(),applied_.end(),locals);
    }

    // 批量记录逐条执行(__skelComputeAll 循环体)。h 指向 BATCH_HDR_F 定长记录头;所有段
    // off/len 先按注册需求与缓冲边界校验,不合法整条拒绝(返回 nullptr,BH_DONE 保持 0,
    // JS 端据此跳过回写,下帧重驱动)。成功置 BH_DONE=1 并返回 out(骨骼世界矩阵)指针,
    // 宿主用它做 slot 矩阵后处理(applySkelSlotMatrices)。
    float* computeBatchOne(float* buf, float* h) {
        int instId = (int)h[BH_ID];
        std::unordered_map<int, SkelReg>::iterator it = regs.find(instId);
        if (it == regs.end()) return nullptr;
        SkelReg& r = it->second;
        struct Seg { int off, len, need; };
        Seg seg[7] = {
            {(int)h[BH_LOC_OFF],  (int)h[BH_LOC_LEN],  r.boneCount * 7},
            {(int)h[BH_IK_OFF],   (int)h[BH_IK_LEN],   (int)r.iks.size() * 5},
            {(int)h[BH_MIX_OFF],  (int)h[BH_MIX_LEN],  (int)r.tcs.size() * 6},
            {(int)h[BH_PDYN_OFF], (int)h[BH_PDYN_LEN], r.pathDynSize},
            {(int)h[BH_INH_OFF],  (int)h[BH_INH_LEN],  r.boneCount},
            {(int)h[BH_PHYS_OFF], (int)h[BH_PHYS_LEN], (int)r.physics.size() * 7},
            {(int)h[BH_W_OFF],    (int)h[BH_W_LEN],    r.boneCount * 6}};
        for (int k = 0; k < 7; k++) {
            if (seg[k].len < seg[k].need) return nullptr;
            if (seg[k].len > 0 && (seg[k].off < BATCH_HDR_TOTAL_F || seg[k].off + seg[k].len > BATCH_TOTAL_F)) return nullptr;
        }
        float* out = buf + seg[6].off;
        compute(instId, buf + seg[0].off,
                seg[1].len ? buf + seg[1].off : nullptr,
                seg[2].len ? buf + seg[2].off : nullptr,
                seg[3].len ? buf + seg[3].off : nullptr,
                seg[5].len ? buf + seg[5].off : nullptr,
                out, h[BH_SX], h[BH_SY], h[BH_X], h[BH_Y],
                buf + seg[4].off, (int)h[BH_PHYS_MODE], h[BH_TIME], h[BH_YDOWN] != 0);
        h[BH_DONE] = 1;
        return out;
    }
};

} // namespace sk
