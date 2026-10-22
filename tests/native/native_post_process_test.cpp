#include "cmdbuf/batcher.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
constexpr int width=640,height=480;
void require(bool ok,const char* message){
    if(!ok){std::fprintf(stderr,"%s (GL=%x EGL=%x)\n",message,glGetError(),eglGetError());std::exit(1);}
}

struct Fixture {
    std::vector<float> shared=std::vector<float>(sg::SHARED_FLOATS,0);
    sg::SceneGraph graph;
    int root;
    Fixture(){
        graph.init(shared.data());root=graph.alloc();
        int previous=0;
        for(int i=0;i<12;i++){
            int node=graph.alloc();graph.setParent(node,root,-1);
            float* s=shared.data()+node*sg::STRIDE;
            s[sg::X]=(i%4)*155+18;s[sg::Y]=(i/4)*155+14;
            sg::NativeQuad q{};
            q[0]=1;q[1]=0;q[2]=0;q[3]=75;q[4]=0;q[5]=75;q[6]=65;q[7]=0;q[8]=65;
            q[9]=0;q[10]=0;q[11]=1;q[12]=0;q[13]=1;q[14]=1;q[15]=0;q[16]=1;
            q[17]=float(0xffffff);q[18]=.85f;q[20]=-1;
            if(i==1){q[3]=q[5]=128;q[6]=q[8]=128;}
            if(i==11){q[3]=q[5]=520;s[sg::X]=0;s[sg::Y]=390;}
            if(i==8){graph.setParent(node,previous,-1);s[sg::X]=24;s[sg::Y]=18;}
            previous=node;
            graph.setNativeQuads(node,{q});
            float* fd=shared.data()+sg::FILTER_BASE+node*sg::FILTER_STRIDE_F;
            fd[45]=i==1?0:36;
            if(i%3==0){fd[78]=1;fd[79]=18;fd[80]=23;fd[81]=2;fd[82]=9;fd[83]=i%2;}
            else {fd[61]=1;fd[62]=8;fd[63]=i%2;fd[64]=1;fd[65]=1;for(int k=0;k<8;k++)fd[66+k]=8-k;}
            if(i==6){fd[29]=1;fd[30]=4;fd[31]=6;fd[33]=.2f;fd[34]=.1f;fd[35]=.8f;fd[36]=.6f;fd[84]=2;fd[85]=fd[86]=1;fd[87]=3;fd[88]=1;}
            graph.refreshFilterFlag(node);
        }
        graph.compute(root,sg::CM_CANONICAL_MAIN);
    }
};

void draw(sg::Batcher& batcher,Fixture& fixture,bool optimized){
    batcher.compactPostTargets=optimized;batcher.postTargetBanks=optimized?3:1;
    glViewport(0,0,width,height);glDisable(GL_SCISSOR_TEST);glStencilMask(0xff);
    glClearColor(0.1f,0.2f,0.3f,1);glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    batcher.frameReset();batcher.frame(&fixture.graph,fixture.root,width,height);
    require(!batcher.escapes,"all fixture nodes rendered");
    require(glGetError()==GL_NO_ERROR,"program/state switching GL status");
}

// Logical geometry stays fixed while the real viewport shrinks. Check clipping against
// explicit rectangles, and blur size in logical coordinates against the full-size image.
void verifyRenderScale(sg::Batcher& batcher){
    GLuint white;glGenTextures(1,&white);glBindTexture(GL_TEXTURE_2D,white);
    const unsigned char pixel[]={255,255,255,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    auto oldLookup=batcher.texLookup;batcher.texLookup=[white](int){return white;};
    std::vector<float> shared(sg::SHARED_FLOATS,0);sg::SceneGraph graph;graph.init(shared.data());
    int root=graph.alloc(),group=graph.alloc();graph.setParent(group,root,-1);
    auto quad=[&](int parent,float x,float y,float w,float h,int tint){
        int n=graph.alloc();graph.setParent(n,parent,-1);
        float* s=shared.data()+n*sg::STRIDE;s[sg::X]=x;s[sg::Y]=y;
        sg::NativeQuad q{};q[0]=1;q[3]=q[5]=w;q[6]=q[8]=h;
        q[11]=q[13]=q[14]=q[16]=1;q[17]=float(tint);q[18]=1;q[20]=-1;
        graph.setNativeQuads(n,{q});return n;
    };
    auto mask=[&](int owner,float x,float y,float w,float h){
        int n=graph.alloc();graph.setParent(n,root,-1);float* s=shared.data()+n*sg::STRIDE;
        s[sg::X]=x;s[sg::Y]=y;s[sg::RW]=w;s[sg::RH]=h;s[sg::HITFLAGS]=4;s[sg::TEXID]=1;
        shared[owner*sg::STRIDE+sg::MASKI]=float(n);
    };
    int content=quad(group,0,0,640,480,0xffffff);
    mask(group,80,60,400,280);mask(content,120,100,280,160);
    int sibling=quad(root,0,0,640,480,0xff0000);mask(sibling,480,360,80,60);
    auto read=[&](float scale){
        int rw=std::lround(width*scale),rh=std::lround(height*scale);
        // Also exercise nonzero viewport origins without exceeding the pbuffer.
        int ox=scale<1?7:0,oy=scale<1?9:0;
        glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(ox,oy,rw,rh);
        glDisable(GL_SCISSOR_TEST);glStencilMask(0xff);glClearColor(0,0,0,1);
        glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
        graph.compute(root,sg::CM_CANONICAL_MAIN);batcher.frameReset();batcher.frame(&graph,root,width,height);
        require(!batcher.escapes,"scaled fixture has no unsupported nodes");
        std::vector<unsigned char> image(rw*rh*4);glReadPixels(ox,oy,rw,rh,GL_RGBA,GL_UNSIGNED_BYTE,image.data());
        require(glGetError()==GL_NO_ERROR,"scaled render GL status");return image;
    };
    for(bool filtered:{false,true}){
        for(int n:{group,content}){
            // A zero-strength blur exercises capture/composition and nested FBOs without
            // introducing an antialiased edge into the exact clipping oracle below.
            shared[sg::FILTER_BASE+n*sg::FILTER_STRIDE_F+78]=filtered?1:0;
            graph.refreshFilterFlag(n);
        }
        for(float scale:{1.f,.75f,.5f}){
            auto image=read(scale);int rw=std::lround(width*scale),rh=std::lround(height*scale);
            for(int y=0;y<rh;y++)for(int x=0;x<rw;x++){
                float lx=(x+.5f)/scale,ly=height-(y+.5f)/scale;
                bool box=lx>120&&lx<400&&ly>100&&ly<260;
                bool red=lx>480&&lx<560&&ly>360&&ly<420;
                const unsigned char* p=&image[(y*rw+x)*4];
                // Mali's mediump UV interpolation can blend the adjacent texel at an edge
                // even at 1x. Keep a two-buffer-pixel sampling margin only for composites.
                float edge=2/scale;
                if(filtered && lx>120-edge&&lx<400+edge&&ly>100-edge&&ly<260+edge &&
                   (std::abs(lx-120)<=edge||std::abs(lx-400)<=edge||std::abs(ly-100)<=edge||std::abs(ly-260)<=edge))continue;
                if(!(p[0]==(box||red?255:0)&&p[1]==(box?255:0)&&p[2]==(box?255:0)))
                    std::fprintf(stderr,"mask mismatch scale=%.2f filtered=%d xy=%.1f,%.1f rgb=%d,%d,%d\n",scale,filtered,lx,ly,p[0],p[1],p[2]);
                require(p[0]==(box||red?255:0)&&p[1]==(box?255:0)&&p[2]==(box?255:0),
                        "scaled nested masks and restored parent scissor match layout rectangles");
            }
            std::printf("renderScale=%.2f nestedFilters=%d clipping=ok\n",scale,filtered);
        }
    }
    shared[group*sg::STRIDE+sg::VISIBLE]=0;shared[sibling*sg::STRIDE+sg::VISIBLE]=0;
    int blur=quad(root,280,208,80,64,0xffffff);
    float* fd=shared.data()+sg::FILTER_BASE+blur*sg::FILTER_STRIDE_F;
    fd[45]=96;fd[78]=1;fd[79]=fd[80]=10;fd[81]=2;fd[82]=9;fd[83]=1;
    graph.refreshFilterFlag(blur);batcher.profilePost=true;
    double referenceSpread=0;
    for(float scale:{1.f,.75f,.5f}){
        auto image=read(scale);int rw=std::lround(width*scale),rh=std::lround(height*scale);
        double mass=0,mx=0,my=0,mxx=0,myy=0;
        for(int y=0;y<rh;y++)for(int x=0;x<rw;x++){
            double weight=image[(y*rw+x)*4],lx=(x+.5)/scale,ly=height-(y+.5)/scale;
            mass+=weight;mx+=weight*lx;my+=weight*ly;mxx+=weight*lx*lx;myy+=weight*ly*ly;
        }
        require(mass>0,"blur stays visible at reduced resolution");mx/=mass;my/=mass;
        double spread=std::sqrt(mxx/mass-mx*mx+myy/mass-my*my);
        if(scale==1)referenceSpread=spread;
        require(std::abs(mx-320)<1&&std::abs(my-240)<1,"blur center stays in logical coordinates");
        require(std::abs(spread/referenceSpread-1)<.05,"blur apparent radius stays within resampling tolerance");
        require(batcher.postProfile.maxW==std::lround(272*scale)&&batcher.postProfile.maxH==std::lround(256*scale),
                "filter raster dimensions shrink with the real viewport");
        std::printf("renderScale=%.2f blurCenter=%.2f,%.2f spread=%.3f filter=%ux%u\n",scale,mx,my,spread,
                    batcher.postProfile.maxW,batcher.postProfile.maxH);
    }
    batcher.profilePost=false;batcher.texLookup=oldLookup;glDeleteTextures(1,&white);
}
}

int main(){
#ifdef __ANDROID__
    EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
    auto getDisplay=reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    require(getDisplay!=nullptr,"ANGLE display entry point");
    const EGLint attrs[]={EGL_PLATFORM_ANGLE_TYPE_ANGLE,EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,EGL_NONE};
    EGLDisplay display=getDisplay(EGL_PLATFORM_ANGLE_ANGLE,nullptr,attrs);
#endif
    require(eglInitialize(display,nullptr,nullptr),"initialize GLES display");
    const EGLint configAttrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_STENCIL_SIZE,8,EGL_NONE};
    EGLConfig config;EGLint count=0;
    require(eglChooseConfig(display,configAttrs,&config,1,&count)&&count==1,"pbuffer config");
    const EGLint surfaceAttrs[]={EGL_WIDTH,width,EGL_HEIGHT,height,EGL_NONE};
    EGLSurface surface=eglCreatePbufferSurface(display,config,surfaceAttrs);
    const EGLint contextAttrs[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,contextAttrs);
    require(eglMakeCurrent(display,surface,surface,context),"offscreen GLES context");
    std::printf("renderer=%s\n",glGetString(GL_RENDERER));
    GLuint texture;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    std::array<unsigned char,64> pixels{};
    for(int i=0;i<16;i++){pixels[i*4]=i*15;pixels[i*4+1]=240-i*10;pixels[i*4+2]=40+i*8;pixels[i*4+3]=i*17;}
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    Fixture fixture;sg::Batcher batcher;batcher.init();batcher.texLookup=[texture](int){return texture;};
    std::vector<unsigned char> reference(width*height*4),actual(reference.size());
    for(int frame=0;frame<8;frame++){
        float* root=fixture.shared.data()+fixture.root*sg::STRIDE;
        root[sg::SX]=frame%2?0.45f:1.f;root[sg::SY]=frame%3?0.6f:1.f;
        fixture.graph.markLocalInputChanged(fixture.root);fixture.graph.compute(fixture.root,sg::CM_CANONICAL_MAIN);
        draw(batcher,fixture,false);glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,reference.data());
        draw(batcher,fixture,true);glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,actual.data());
        int maxError=0,changed=0;
        for(size_t i=0;i<actual.size();i++){int error=std::abs(int(actual[i])-int(reference[i]));maxError=std::max(maxError,error);changed+=error!=0;}
        std::printf("frame=%d changedChannels=%d maxError=%d draws=%u\n",frame,changed,maxError,batcher.drawCalls);
        unsigned long long hash=1469598103934665603ULL;for(auto v:actual){hash^=v;hash*=1099511628211ULL;}
        std::printf("pixelHash=%016llx\n",hash);
        require(maxError==0,"independent targets must match the original pixels exactly");
        bool visible=false;for(size_t i=4;i<actual.size();i+=4)if(std::memcmp(actual.data(),actual.data()+i,4)!=0){visible=true;break;}
        require(visible,"visible fixture pixels");
    }
    fixture.shared[fixture.root*sg::STRIDE+sg::SX]=1;fixture.shared[fixture.root*sg::STRIDE+sg::SY]=1;
    fixture.graph.markLocalInputChanged(fixture.root);fixture.graph.compute(fixture.root,sg::CM_CANONICAL_MAIN);
    verifyRenderScale(batcher);
    // Blocking only in this stand-alone benchmark, never in the game frame loop. Alternating
    // short windows reduces clock/temperature bias; these are fixture times, not game FPS.
    for(int round=0;round<4;round++)for(bool enabled:{false,true}){
        for(int i=0;i<5;i++)draw(batcher,fixture,enabled);glFinish();
        const auto begin=std::chrono::steady_clock::now();
        for(int i=0;i<60;i++)draw(batcher,fixture,enabled);
        glFinish();
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/60;
        std::printf("round=%d variant=%d fixtureMs=%.3f\n",round,enabled,ms);
    }
    glDeleteTextures(1,&texture);eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,context);eglDestroySurface(display,surface);eglTerminate(display);
}
