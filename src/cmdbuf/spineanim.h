// spineanim.h - batched Spine 4.2 numeric timeline evaluation for the native screen path.
//
// JavaScript keeps TrackEntry/listener semantics, attachments and events. A complete
// AnimationState.apply pass records its timelines and sends them here once. Numeric interpolation
// and discrete draw-order selection run over compact contiguous arrays in C++ instead of executing
// thousands of tiny JavaScript timeline methods every frame.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace spanim {

enum Type {
    ROTATE=1, TRANSLATE=2, TRANSLATE_X=3, TRANSLATE_Y=4,
    SCALE=5, SCALE_X=6, SCALE_Y=7, SHEAR=8, SHEAR_X=9, SHEAR_Y=10, INHERIT=11,
    SLOT_RGBA=20, SLOT_RGB=21, SLOT_ALPHA=22, SLOT_RGBA2=23, SLOT_RGB2=24,
    IK=30, TRANSFORM=31, PATH_POSITION=32, PATH_SPACING=33, PATH_MIX=34,
    SEQUENCE=40, EVENT=41, DRAW_ORDER=42, ATTACHMENT=43,
    PHYSICS_INERTIA=50, PHYSICS_STRENGTH=51, PHYSICS_DAMPING=52, PHYSICS_MASS=53,
    PHYSICS_WIND=54,PHYSICS_GRAVITY=55,PHYSICS_MIX=56,PHYSICS_RESET=57
};

struct Timeline {
    int type=0, target=0, entries=0;
    std::vector<float> frames;
    std::vector<float> curves;
};

struct DeformTimelineData {
    int vertexCount=0;
    std::vector<float> frames;
    std::vector<float> curves;
    std::vector<float> vertices; // frame-major, frameCount * vertexCount
};

class TimelineSet {
    std::unordered_map<int, Timeline> timelines_;
    std::unordered_map<int, DeformTimelineData> deformTimelines_;
    std::unordered_map<int, std::vector<int> > programs_;
    std::vector<float> programCommands_;

    static float sign(float v) { return v < 0 ? -1.0f : (v > 0 ? 1.0f : 0.0f); }
    static float clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }

    static int search(const Timeline& t, float time) {
        int n=(int)t.frames.size(), step=t.entries;
        for(int i=step;i<n;i+=step) if(t.frames[i]>time) return i-step;
        return std::max(0,n-step);
    }

    static float bezier(const Timeline& t,float time,int frame,int value,int ci) {
        const auto& c=t.curves; const auto& f=t.frames;
        if(ci<0||ci+1>=(int)c.size()) return f[frame+value];
        if(c[ci]>time){float x=f[frame],y=f[frame+value],d=c[ci]-x;return d==0?y:y+(time-x)/d*(c[ci+1]-y);}
        int end=std::min((int)c.size(),ci+18);
        for(int i=ci+2;i+1<end;i+=2)if(c[i]>=time){float x=c[i-2],y=c[i-1],d=c[i]-x;return d==0?y:y+(time-x)/d*(c[i+1]-y);}
        int nf=std::min((int)f.size()-t.entries,frame+t.entries),i=end-2;
        if(i<ci)return f[frame+value];float d=f[nf]-c[i];return d==0?c[i+1]:c[i+1]+(time-c[i])/d*(f[nf+value]-c[i+1]);
    }

    static bool values(const Timeline& t,float time,float* out,int count,int* frameOut=nullptr) {
        if(t.frames.empty()||t.entries<2||time<t.frames[0])return false;
        int i=search(t,time),fi=i/t.entries; if(frameOut)*frameOut=i;
        int curve=(fi<(int)t.curves.size())?(int)t.curves[fi]:1;
        for(int v=1;v<=count;v++){
            float x=t.frames[i+v];
            if(curve==0&&i+t.entries<(int)t.frames.size()){
                float d=t.frames[i+t.entries]-t.frames[i];float p=d==0?0:(time-t.frames[i])/d;
                x+= (t.frames[i+t.entries+v]-x)*p;
            }else if(curve>1)x=bezier(t,time,i,v,curve-2+18*(v-1));
            out[v-1]=x;
        }
        return true;
    }

    static float relative1(const Timeline& t,float time,float alpha,int blend,float current,float setup) {
        if(time<t.frames[0]){if(blend==0)return setup;if(blend==1)return current+(setup-current)*alpha;return current;}
        float v=0;values(t,time,&v,1);if(blend==0)return setup+v*alpha;if(blend==1||blend==2)v+=setup-current;return current+v*alpha;
    }
    static float absolute1(const Timeline& t,float time,float alpha,int blend,float current,float setup) {
        if(time<t.frames[0]){if(blend==0)return setup;if(blend==1)return current+(setup-current)*alpha;return current;}
        float v=0;values(t,time,&v,1);return blend==0?setup+(v-setup)*alpha:current+(v-current)*alpha;
    }
    static float scale1(const Timeline& t,float time,float alpha,int blend,int direction,float current,float setup) {
        if(time<t.frames[0]){if(blend==0)return setup;if(blend==1)return current+(setup-current)*alpha;return current;}
        float v=0;values(t,time,&v,1);v*=setup;
        if(alpha==1)return blend==3?current+v-setup:v;
        if(direction==1){if(blend==0)return setup+(std::fabs(v)*sign(setup)-setup)*alpha;if(blend==1||blend==2)return current+(std::fabs(v)*sign(current)-current)*alpha;}
        else {if(blend==0){float s=std::fabs(setup)*sign(v);return s+(v-s)*alpha;}if(blend==1||blend==2){float s=std::fabs(current)*sign(v);return s+(v-s)*alpha;}}
        return current+(v-setup)*alpha;
    }

    static void colorN(float* cur,const float* setup,const float* v,int count,float alpha,int blend) {
        if(alpha==1){for(int i=0;i<count;i++)cur[i]=clamp01(v[i]);return;}
        if(blend==0)for(int i=0;i<count;i++)cur[i]=setup[i];
        for(int i=0;i<count;i++)cur[i]=clamp01(cur[i]+(v[i]-cur[i])*alpha);
    }

    static void resetColor(float* cur,const float* setup,int type,float alpha,int blend) {
        int map[7], n=0;
        if(type==SLOT_RGBA||type==SLOT_RGBA2){map[n++]=0;map[n++]=1;map[n++]=2;map[n++]=3;}
        else if(type==SLOT_RGB||type==SLOT_RGB2){map[n++]=0;map[n++]=1;map[n++]=2;}
        else map[n++]=3;
        if(type==SLOT_RGBA2||type==SLOT_RGB2){map[n++]=4;map[n++]=5;map[n++]=6;}
        if(blend==0)for(int i=0;i<n;i++)cur[map[i]]=setup[map[i]];
        else if(blend==1)for(int i=0;i<n;i++){int q=map[i];cur[q]+=(setup[q]-cur[q])*alpha;}
    }

    static void colorMapped(float* cur,const float* setup,const float* values,int type,float alpha,int blend) {
        int map[7], n=0;
        if(type==SLOT_RGBA||type==SLOT_RGBA2){map[n++]=0;map[n++]=1;map[n++]=2;map[n++]=3;}
        else if(type==SLOT_RGB||type==SLOT_RGB2){map[n++]=0;map[n++]=1;map[n++]=2;}
        else map[n++]=3;
        if(type==SLOT_RGBA2||type==SLOT_RGB2){map[n++]=4;map[n++]=5;map[n++]=6;}
        if(blend==0)for(int i=0;i<n;i++)cur[map[i]]=setup[map[i]];
        if(alpha==1)for(int i=0;i<n;i++)cur[map[i]]=clamp01(values[i]);
        else for(int i=0;i<n;i++){int q=map[i];cur[q]=clamp01(cur[q]+(values[i]-cur[q])*alpha);}
    }

    static float deformPercent(const DeformTimelineData& t,float time,int frame) {
        if(frame<0||frame+1>=(int)t.frames.size())return 0.0f;
        int code=frame<(int)t.curves.size()?(int)t.curves[frame]:0;
        float t0=t.frames[frame],t1=t.frames[frame+1];
        if(code==0)return t1==t0?0.0f:(time-t0)/(t1-t0);
        if(code==1)return 0.0f;
        int i=code-2,n=i+18;if(i<0||n>(int)t.curves.size())return t1==t0?0.0f:(time-t0)/(t1-t0);
        if(t.curves[i]>time){float d=t.curves[i]-t0;return d==0?0:t.curves[i+1]*(time-t0)/d;}
        for(i+=2;i<n;i+=2)if(t.curves[i]>=time){float x=t.curves[i-2],y=t.curves[i-1],d=t.curves[i]-x;return d==0?y:y+(time-x)/d*(t.curves[i+1]-y);}
        float x=t.curves[n-2],y=t.curves[n-1],d=t1-x;return d==0?y:y+(1-y)*(time-x)/d;
    }

public:
    // Advance independent TrackEntry clocks in one native call.  The common Spine case in this
    // game is one current entry per track with no queued or mixing entry.  JavaScript keeps the
    // business-facing TrackEntry object, but no longer performs its per-field clock arithmetic.
    //
    // Record layout (9 floats): delay, trackTime, trackLast, nextTrackLast, animationLast,
    // nextAnimationLast, trackEnd, timeScale, status.  status=0 active, 1 still delayed,
    // 2 reached trackEnd and should be ended by AnimationState's listener/event facade.
    void advanceSimpleTracks(float* records, int recordFloats, float delta) const {
        if (!records || recordFloats < 9 || !std::isfinite(delta)) return;
        for (int o = 0; o + 8 < recordFloats; o += 9) {
            float& delay = records[o];
            float& trackTime = records[o + 1];
            float& trackLast = records[o + 2];
            float& nextTrackLast = records[o + 3];
            float& animationLast = records[o + 4];
            float& nextAnimationLast = records[o + 5];
            const float trackEnd = records[o + 6];
            const float timeScale = records[o + 7];
            float& status = records[o + 8];
            status = 0;
            animationLast = nextAnimationLast;
            trackLast = nextTrackLast;
            float currentDelta = delta * timeScale;
            if (delay > 0) {
                delay -= currentDelta;
                if (delay > 0) { status = 1; continue; }
                currentDelta = -delay;
                delay = 0;
            }
            // The caller only submits entries without next/mixingFrom, matching the corresponding
            // branch in spine-ts AnimationState.update exactly.
            if (trackLast >= trackEnd) { status = 2; continue; }
            trackTime += currentDelta;
        }
    }

    // Full spine-ts AnimationState.update graph pass.  JavaScript supplies stable TrackEntry
    // facades and maps the returned lifecycle actions to listeners, while all queued-entry,
    // mixing-chain and clock decisions execute here in one bounded call.
    //
    // Record layout (16 floats): next, mixingFrom, mixingTo, delay, trackTime, trackLast,
    // nextTrackLast, animationLast, nextAnimationLast, trackEnd, timeScale, mixTime,
    // mixDuration, interruptAlpha, totalAlpha, reserved.
    // Action pairs use spine EventQueue order: 0=start, 1=interrupt, 2=end, 3=reset rotations.
    int advanceState(float* r, int floats, int32_t* roots, int rootCount, float delta,
                     int32_t* actions, int actionInts) const {
        constexpr int S=16,NEXT=0,FROM=1,TO=2,DELAY=3,TRACK_TIME=4,TRACK_LAST=5,
                      NEXT_TRACK_LAST=6,ANIM_LAST=7,NEXT_ANIM_LAST=8,TRACK_END=9,
                      TIME_SCALE=10,MIX_TIME=11,MIX_DURATION=12,INTERRUPT_ALPHA=13,TOTAL_ALPHA=14;
        if(!r||floats<0||floats%S||!roots||rootCount<0||!actions||actionInts<0||!std::isfinite(delta))return -1;
        const int count=floats/S;int actionCount=0;
        auto valid=[&](int index){return index>=0&&index<count;};
        auto getIndex=[&](int index,int field){if(!valid(index))return -1;int value=(int)r[index*S+field];return valid(value)?value:-1;};
        auto emit=[&](int type,int index){if(actionCount+2>actionInts)return false;actions[actionCount++]=type;actions[actionCount++]=index;return true;};
        std::function<int(int,int)> updateMixingFrom=[&](int to,int depth)->int{
            if(!valid(to)||depth>64)return -1;int from=getIndex(to,FROM);if(from<0)return 1;
            int finished=updateMixingFrom(from,depth+1);if(finished<0)return -1;
            float* f=r+from*S,* t=r+to*S;f[ANIM_LAST]=f[NEXT_ANIM_LAST];f[TRACK_LAST]=f[NEXT_TRACK_LAST];
            if(t[NEXT_TRACK_LAST]!=-1){bool discard=t[MIX_TIME]==0&&f[MIX_TIME]==0;
                if(t[MIX_TIME]>=t[MIX_DURATION]||discard){
                    if(f[TOTAL_ALPHA]==0||t[MIX_DURATION]==0||discard){int prior=getIndex(from,FROM);t[FROM]=(float)prior;if(prior>=0)r[prior*S+TO]=(float)to;t[INTERRUPT_ALPHA]=f[INTERRUPT_ALPHA];if(!emit(2,from))return -1;}
                    return finished;
                }
            }
            f[TRACK_TIME]+=delta*f[TIME_SCALE];t[MIX_TIME]+=delta;return 0;
        };
        for(int track=0;track<rootCount;track++){
            int current=roots[track];if(!valid(current)){roots[track]=-1;continue;}float* c=r+current*S;
            c[ANIM_LAST]=c[NEXT_ANIM_LAST];c[TRACK_LAST]=c[NEXT_TRACK_LAST];float currentDelta=delta*c[TIME_SCALE];
            if(c[DELAY]>0){c[DELAY]-=currentDelta;if(c[DELAY]>0)continue;currentDelta=-c[DELAY];c[DELAY]=0;}
            int next=getIndex(current,NEXT);
            if(next>=0){float* ne=r+next*S;float nextTime=c[TRACK_LAST]-ne[DELAY];if(nextTime>=0){
                ne[DELAY]=0;ne[TRACK_TIME]+=c[TIME_SCALE]==0?0:(nextTime/c[TIME_SCALE]+delta)*ne[TIME_SCALE];c[TRACK_TIME]+=currentDelta;
                roots[track]=next;if(!emit(1,current))return -1;ne[FROM]=(float)current;c[TO]=(float)next;ne[MIX_TIME]=0;
                int prior=getIndex(current,FROM);if(prior>=0&&c[MIX_DURATION]>0)ne[INTERRUPT_ALPHA]*=std::min(1.0f,c[MIX_TIME]/c[MIX_DURATION]);
                if(!emit(3,current)||!emit(0,next))return -1;
                for(int entry=next,guard=0;guard++<64;){int from=getIndex(entry,FROM);if(from<0)break;r[entry*S+MIX_TIME]+=delta;entry=from;}
                continue;
            }}
            int from=getIndex(current,FROM);
            if(next<0&&c[TRACK_LAST]>=c[TRACK_END]&&from<0){roots[track]=-1;if(!emit(2,current))return -1;continue;}
            if(from>=0){int finished=updateMixingFrom(current,0);if(finished<0)return -1;if(finished){
                int ending=getIndex(current,FROM);c[FROM]=-1;if(ending>=0)r[ending*S+TO]=-1;
                for(int entry=ending,guard=0;entry>=0&&guard++<64;entry=getIndex(entry,FROM))if(!emit(2,entry))return -1;
            }}
            c[TRACK_TIME]+=currentDelta;
        }
        return actionCount/2;
    }

    void reg(int id,int type,int target,int entries,const float* frames,int fn,const float* curves,int cn) {
        if(id<=0||entries<1||!frames||fn<entries)return;Timeline t;t.type=type;t.target=target;t.entries=entries;
        t.frames.assign(frames,frames+fn);if(curves&&cn>0)t.curves.assign(curves,curves+cn);else t.curves.assign(fn/entries,1.0f);timelines_[id]=std::move(t);
    }
    void regDeform(int id,const float* frames,int frameCount,const float* curves,int curveCount,const float* vertices,int vertexFloats,int vertexCount){
        if(id<=0||!frames||frameCount<=0||!vertices||vertexCount<=0||vertexFloats<frameCount*vertexCount)return;
        DeformTimelineData t;t.vertexCount=vertexCount;t.frames.assign(frames,frames+frameCount);t.vertices.assign(vertices,vertices+(size_t)frameCount*vertexCount);
        if(curves&&curveCount>0)t.curves.assign(curves,curves+curveCount);else t.curves.assign(frameCount,0.0f);
        deformTimelines_[id]=std::move(t);
    }
    void regProgram(int id,const float* timelineIds,int count){if(id<=0||!timelineIds||count<=0)return;std::vector<int> ids;ids.reserve(count);for(int i=0;i<count;i++){int timeline=(int)timelineIds[i];if(timelines_.find(timeline)!=timelines_.end())ids.push_back(timeline);}if(!ids.empty())programs_[id]=std::move(ids);}
    void clear(){timelines_.clear();deformTimelines_.clear();programs_.clear();programCommands_.clear();}

    // In-place implementation of spine-ts DeformTimeline.apply. Return -1 for an unknown
    // registration, -2 for "keep current", 0 for "clear deform", or vertexCount for output.
    int applyDeform(int id,float time,float alpha,int blend,bool weighted,bool wasEmpty,float* deform,int deformN,const float* setup,int setupN) const {
        auto it=deformTimelines_.find(id);if(it==deformTimelines_.end())return -1;const DeformTimelineData& t=it->second;
        const int vc=t.vertexCount,fc=(int)t.frames.size();if(!deform||deformN<vc||fc<=0||(!weighted&&(!setup||setupN<vc)))return -1;
        if(wasEmpty)blend=0;
        if(time<t.frames[0]){
            if(blend==0)return 0;
            if(blend!=1)return -2;
            if(alpha==1)return 0;
            if(weighted){float a=1-alpha;for(int i=0;i<vc;i++)deform[i]*=a;}
            else for(int i=0;i<vc;i++)deform[i]+=(setup[i]-deform[i])*alpha;
            return vc;
        }
        int frame=fc-1;float percent=0;
        if(time<t.frames.back()){
            auto ub=std::upper_bound(t.frames.begin(),t.frames.end(),time);frame=std::max(0,(int)(ub-t.frames.begin())-1);percent=deformPercent(t,time,frame);
        }
        const float* prev=t.vertices.data()+(size_t)frame*vc;
        const float* next=(frame+1<fc)?prev+vc:prev;
        for(int i=0;i<vc;i++){
            float value=prev[i]+(next[i]-prev[i])*percent;
            if(alpha==1){if(blend==3)deform[i]+=weighted?value:value-setup[i];else deform[i]=value;continue;}
            switch(blend){
                case 0:deform[i]=weighted?value*alpha:setup[i]+(value-setup[i])*alpha;break;
                case 1:case 2:deform[i]+=(value-deform[i])*alpha;break;
                case 3:deform[i]+=(weighted?value:value-setup[i])*alpha;break;
            }
        }
        return vc;
    }

    // Select a discrete key frame synchronously. Attachment changes must be visible to later
    // Sequence/Deform timelines in the same AnimationState.apply call, so this tiny native lookup
    // is intentionally immediate instead of being deferred with numeric timeline writes.
    // -3: invalid timeline, -2: keep current value, -1: restore setup value, >=0: frame index.
    int selectDiscrete(int id,float time,int blend,int direction) const {
        auto it=timelines_.find(id);if(it==timelines_.end())return -3;const Timeline& t=it->second;
        if(t.type!=ATTACHMENT&&t.type!=DRAW_ORDER&&t.type!=INHERIT)return -3;
        if(direction==1){return blend==0?-1:-2;}
        if(t.frames.empty())return -2;
        if(time<t.frames[0])return (blend==0||blend==1)?-1:-2;
        return search(t,time);
    }

    // commands: [timelineId,time,alpha,blend,direction,mode,stateTotal,stateDiff] repeated.
    // mode bit0 is AnimationState's stateful shortest/remembered-direction rotate path; bit1 marks
    // its first frame. The final rotation state is written back into stateTotal/stateDiff so the
    // TrackEntry keeps exactly the same direction across subsequent mixed frames.
    void apply(float* commands,int commandFloats,
               float* bones,int boneFloats,const float* boneSetup,int boneSetupFloats,
               float* slots,int slotFloats,const float* slotSetup,int slotSetupFloats,
               float* ik,int ikFloats,const float* ikSetup,int ikSetupFloats,
               float* tc,int tcFloats,const float* tcSetup,int tcSetupFloats,
               float* pc,int pcFloats,const float* pcSetup,int pcSetupFloats,
               float* sequences,int sequenceFloats,
               float* physics,int physicsFloats,const float* physicsSetup,int physicsSetupFloats,
               const float* physicsGlobal,int physicsGlobalFloats) const {
        if(!commands)return;
        for(int co=0;co+7<commandFloats;co+=8){
            auto it=timelines_.find((int)commands[co]);if(it==timelines_.end())continue;const Timeline& t=it->second;
            float time=commands[co+1],alpha=commands[co+2];int blend=(int)commands[co+3],direction=(int)commands[co+4],mode=(int)commands[co+5],z=t.target;
            if(t.type>=ROTATE&&t.type<=SHEAR_Y){
                int o=z*7;if(o<0||o+6>=boneFloats||o+6>=boneSetupFloats)continue;float* b=bones+o;const float* s=boneSetup+o;float v[2]={0,0};
                if(t.type==ROTATE&&(mode&1)){
                    bool firstFrame=(mode&2)!=0;if(firstFrame)commands[co+6]=0;
                    if(alpha==1){b[2]=relative1(t,time,1,blend,b[2],s[2]);continue;}
                    float r1=0,r2=0;
                    if(time<t.frames[0]){
                        if(blend==0){b[2]=s[2];continue;}
                        if(blend!=1)continue;
                        r1=b[2];r2=s[2];
                    }else{
                        r1=blend==0?s[2]:b[2];float rv=0;values(t,time,&rv,1);r2=s[2]+rv;
                    }
                    float total=0,diff=r2-r1;diff-=std::ceil(diff/360.0f-0.5f)*360.0f;
                    if(diff==0)total=commands[co+6];
                    else{
                        float lastTotal=firstFrame?0:commands[co+6],lastDiff=firstFrame?diff:commands[co+7];
                        float loops=lastTotal-std::fmod(lastTotal,360.0f);total=diff+loops;
                        bool current=diff>=0,dir=lastTotal>=0;
                        if(std::fabs(lastDiff)<=90&&sign(lastDiff)!=sign(diff)){
                            if(std::fabs(lastTotal-loops)>180){total+=360*sign(lastTotal);dir=current;}
                            else if(loops!=0)total-=360*sign(lastTotal);
                            else dir=current;
                        }
                        if(dir!=current)total+=360*sign(lastTotal);
                        commands[co+6]=total;
                    }
                    commands[co+7]=diff;b[2]=r1+total*alpha;continue;
                }
                switch(t.type){
                    case ROTATE:b[2]=relative1(t,time,alpha,blend,b[2],s[2]);break;
                    case TRANSLATE:if(!values(t,time,v,2)){if(blend==0){b[0]=s[0];b[1]=s[1];}else if(blend==1){b[0]+=(s[0]-b[0])*alpha;b[1]+=(s[1]-b[1])*alpha;}}else if(blend==0){b[0]=s[0]+v[0]*alpha;b[1]=s[1]+v[1]*alpha;}else if(blend==3){b[0]+=v[0]*alpha;b[1]+=v[1]*alpha;}else{b[0]+=(s[0]+v[0]-b[0])*alpha;b[1]+=(s[1]+v[1]-b[1])*alpha;}break;
                    case TRANSLATE_X:b[0]=relative1(t,time,alpha,blend,b[0],s[0]);break;
                    case TRANSLATE_Y:b[1]=relative1(t,time,alpha,blend,b[1],s[1]);break;
                    case SCALE:if(!values(t,time,v,2)){if(blend==0){b[3]=s[3];b[4]=s[4];}else if(blend==1){b[3]+=(s[3]-b[3])*alpha;b[4]+=(s[4]-b[4])*alpha;}}else{v[0]*=s[3];v[1]*=s[4];if(alpha==1){if(blend==3){b[3]+=v[0]-s[3];b[4]+=v[1]-s[4];}else{b[3]=v[0];b[4]=v[1];}}else if(direction==1){if(blend==0){b[3]=s[3]+(std::fabs(v[0])*sign(s[3])-s[3])*alpha;b[4]=s[4]+(std::fabs(v[1])*sign(s[4])-s[4])*alpha;}else if(blend==3){b[3]+=(v[0]-s[3])*alpha;b[4]+=(v[1]-s[4])*alpha;}else{b[3]+=(std::fabs(v[0])*sign(b[3])-b[3])*alpha;b[4]+=(std::fabs(v[1])*sign(b[4])-b[4])*alpha;}}else if(blend==0){float x=std::fabs(s[3])*sign(v[0]),y=std::fabs(s[4])*sign(v[1]);b[3]=x+(v[0]-x)*alpha;b[4]=y+(v[1]-y)*alpha;}else if(blend==3){b[3]+=(v[0]-s[3])*alpha;b[4]+=(v[1]-s[4])*alpha;}else{float x=std::fabs(b[3])*sign(v[0]),y=std::fabs(b[4])*sign(v[1]);b[3]=x+(v[0]-x)*alpha;b[4]=y+(v[1]-y)*alpha;}}break;
                    case SCALE_X:b[3]=scale1(t,time,alpha,blend,direction,b[3],s[3]);break;
                    case SCALE_Y:b[4]=scale1(t,time,alpha,blend,direction,b[4],s[4]);break;
                    case SHEAR:if(!values(t,time,v,2)){if(blend==0){b[5]=s[5];b[6]=s[6];}else if(blend==1){b[5]+=(s[5]-b[5])*alpha;b[6]+=(s[6]-b[6])*alpha;}}else if(blend==0){b[5]=s[5]+v[0]*alpha;b[6]=s[6]+v[1]*alpha;}else if(blend==3){b[5]+=v[0]*alpha;b[6]+=v[1]*alpha;}else{b[5]+=(s[5]+v[0]-b[5])*alpha;b[6]+=(s[6]+v[1]-b[6])*alpha;}break;
                    case SHEAR_X:b[5]=relative1(t,time,alpha,blend,b[5],s[5]);break;
                    case SHEAR_Y:b[6]=relative1(t,time,alpha,blend,b[6],s[6]);break;
                }continue;
            }
            if(t.type>=SLOT_RGBA&&t.type<=SLOT_RGB2){
                int o=z*7;if(o<0||o+6>=slotFloats||o+6>=slotSetupFloats)continue;float* c=slots+o;const float* s=slotSetup+o;float v[7]={0};
                int n=t.type==SLOT_RGBA?4:(t.type==SLOT_RGB?3:(t.type==SLOT_ALPHA?1:(t.type==SLOT_RGBA2?7:6)));
                if(!values(t,time,v,n)){resetColor(c,s,t.type,alpha,blend);continue;}
                colorMapped(c,s,v,t.type,alpha,blend);continue;
            }
            if(t.type==IK){int o=z*5;if(o<0||o+4>=ikFloats||o+4>=ikSetupFloats)continue;float* c=ik+o;const float* s=ikSetup+o;float v[2]={0};int fr=0;
                if(!values(t,time,v,2,&fr)){if(blend==0)for(int i=0;i<5;i++)c[i]=s[i];else if(blend==1){c[0]+=(s[0]-c[0])*alpha;c[1]+=(s[1]-c[1])*alpha;c[2]=s[2];c[3]=s[3];c[4]=s[4];}}else{if(blend==0){c[0]=s[0]+(v[0]-s[0])*alpha;c[1]=s[1]+(v[1]-s[1])*alpha;}else{c[0]+=(v[0]-c[0])*alpha;c[1]+=(v[1]-c[1])*alpha;}if(direction==0){c[2]=t.frames[fr+3];c[3]=t.frames[fr+4];c[4]=t.frames[fr+5];}else if(blend==0){c[2]=s[2];c[3]=s[3];c[4]=s[4];}}continue;
            }
            if(t.type==TRANSFORM){int o=z*6;if(o<0||o+5>=tcFloats||o+5>=tcSetupFloats)continue;float* c=tc+o;const float* s=tcSetup+o;float v[6]={0};if(!values(t,time,v,6)){if(blend==0)for(int i=0;i<6;i++)c[i]=s[i];else if(blend==1)for(int i=0;i<6;i++)c[i]+=(s[i]-c[i])*alpha;}else for(int i=0;i<6;i++)c[i]=(blend==0?s[i]:c[i])+((blend==0?v[i]-s[i]:v[i]-c[i])*alpha);continue;}
            if(t.type==PATH_POSITION||t.type==PATH_SPACING){int o=z*5,q=t.type==PATH_POSITION?0:1;if(o<0||o+4>=pcFloats||o+4>=pcSetupFloats)continue;pc[o+q]=absolute1(t,time,alpha,blend,pc[o+q],pcSetup[o+q]);continue;}
            if(t.type==PATH_MIX){int o=z*5;if(o<0||o+4>=pcFloats||o+4>=pcSetupFloats)continue;float* c=pc+o;const float* s=pcSetup+o;float v[3]={0};if(!values(t,time,v,3)){if(blend==0){c[2]=s[2];c[3]=s[3];c[4]=s[4];}else if(blend==1)for(int i=0;i<3;i++)c[2+i]+=(s[2+i]-c[2+i])*alpha;}else for(int i=0;i<3;i++)c[2+i]=(blend==0?s[2+i]:c[2+i])+((blend==0?v[i]-s[2+i]:v[i]-c[2+i])*alpha);}
            if(t.type>=PHYSICS_INERTIA&&t.type<=PHYSICS_MIX){
                int q=t.type-PHYSICS_INERTIA,physCount=physicsFloats/7;
                auto applyOne=[&](int index){int o=index*7;if(index<0||o+6>=physicsFloats||o+6>=physicsSetupFloats)return;float current=physics[o+q],setup=physicsSetup[o+q];
                    if(t.type==PHYSICS_MASS){current=current!=0?1/current:0;setup=setup!=0?1/setup:0;float value=absolute1(t,time,alpha,blend,current,setup);physics[o+q]=value!=0?1/value:0;}
                    else physics[o+q]=absolute1(t,time,alpha,blend,current,setup);
                };
                if(z>=0)applyOne(z);else for(int index=0;index<physCount;index++){int o=index*7+q;if(o>=0&&o<physicsGlobalFloats&&physicsGlobal[o]!=0)applyOne(index);}continue;
            }
            if(t.type==PHYSICS_RESET){
                float lastTime=commands[co+5];auto a=std::upper_bound(t.frames.begin(),t.frames.end(),lastTime),b=std::upper_bound(t.frames.begin(),t.frames.end(),time);
                commands[co+6]=(lastTime>time?(a!=t.frames.end()||b!=t.frames.begin()):(a<b))?1.0f:0.0f;continue;
            }
            if(t.type==SEQUENCE){
                if(z<0||z>=sequenceFloats||t.frames.size()<3)continue;
                if(direction==1){if(blend==0)sequences[z]=-1;continue;}
                if(time<t.frames[0]){if(blend==0||blend==1)sequences[z]=-1;continue;}
                int frame=search(t,time),modeAndIndex=(int)t.frames[frame+1];float delay=t.frames[frame+2];
                int index=modeAndIndex>>4,count=t.curves.empty()?0:std::max(0,(int)t.curves[0]),mode=modeAndIndex&15;
                if(mode!=0&&count>0&&delay>0){index+=(int)((time-t.frames[frame])/delay+0.00001f);
                    switch(mode){
                        case 1:index=std::min(count-1,index);break;
                        case 2:index%=count;break;
                        case 3:{int n=(count<<1)-2;index=n==0?0:index%n;if(index>=count)index=n-index;break;}
                        case 4:index=std::max(count-1-index,0);break;
                        case 5:index=count-1-index%count;break;
                        case 6:{int n=(count<<1)-2;index=n==0?0:(index+count-1)%n;if(index>=count)index=n-index;break;}
                    }
                }
                sequences[z]=(float)index;continue;
            }
            if(t.type==EVENT){
                // co+5 carries lastTime. Return one circular [start,count] range in co+6/co+7;
                // JavaScript only maps those indices to business event objects and dispatches them.
                commands[co+6]=-1;commands[co+7]=0;
                const int n=(int)t.frames.size();if(n==0)continue;
                const float lastTime=commands[co+5];
                auto after=[&](float v){return (int)(std::upper_bound(t.frames.begin(),t.frames.end(),v)-t.frames.begin());};
                if(lastTime>time){
                    const int tailStart=after(lastTime),tailCount=n-tailStart,headCount=after(time),count=tailCount+headCount;
                    if(count>0){commands[co+6]=(float)(tailCount>0?tailStart:0);commands[co+7]=(float)count;}
                }else{
                    const int start=after(lastTime),end=after(time);
                    if(end>start){commands[co+6]=(float)start;commands[co+7]=(float)(end-start);}
                }
                continue;
            }
            if(t.type==DRAW_ORDER){
                // co+6 is an output slot consumed after the batch. -2 means keep the current
                // order, -1 means setup order, and a non-negative value selects drawOrders[idx].
                commands[co+6]=-2;
                if(direction==1){if(blend==0)commands[co+6]=-1;continue;}
                if(t.frames.empty())continue;
                if(time<t.frames[0]){if(blend==0||blend==1)commands[co+6]=-1;continue;}
                commands[co+6]=(float)search(t,time);continue;
            }
            if(t.type==ATTACHMENT){
                commands[co+6]=(float)selectDiscrete((int)commands[co],time,blend,direction);
                continue;
            }
        }
    }

    bool applyProgram(int programId,float time,float alpha,int blend,int direction,
                      float* bones,int boneFloats,const float* boneSetup,int boneSetupFloats,
                      float* slots,int slotFloats,const float* slotSetup,int slotSetupFloats,
                      float* ik,int ikFloats,const float* ikSetup,int ikSetupFloats,
                      float* tc,int tcFloats,const float* tcSetup,int tcSetupFloats,
                      float* pc,int pcFloats,const float* pcSetup,int pcSetupFloats,
                      float* sequences,int sequenceFloats,
                      float* physics,int physicsFloats,const float* physicsSetup,int physicsSetupFloats,
                      const float* physicsGlobal,int physicsGlobalFloats) {
        auto found=programs_.find(programId);if(found==programs_.end()||found->second.empty())return false;
        programCommands_.assign(found->second.size()*8,0.0f);for(size_t i=0;i<found->second.size();i++){float* c=programCommands_.data()+i*8;c[0]=(float)found->second[i];c[1]=time;c[2]=alpha;c[3]=(float)blend;c[4]=(float)direction;}
        apply(programCommands_.data(),(int)programCommands_.size(),bones,boneFloats,boneSetup,boneSetupFloats,slots,slotFloats,slotSetup,slotSetupFloats,ik,ikFloats,ikSetup,ikSetupFloats,tc,tcFloats,tcSetup,tcSetupFloats,pc,pcFloats,pcSetup,pcSetupFloats,sequences,sequenceFloats,physics,physicsFloats,physicsSetup,physicsSetupFloats,physicsGlobal,physicsGlobalFloats);return true;
    }
};

} // namespace spanim
