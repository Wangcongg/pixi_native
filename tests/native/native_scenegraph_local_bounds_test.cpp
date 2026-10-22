#include "cmdbuf/scenegraph.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
    std::vector<float> shared(sg::SHARED_FLOATS,0);
    sg::SceneGraph graph;
    graph.init(shared.data());
    auto slot=[&](int node){return shared.data()+node*sg::STRIDE;};
    auto node=[&](int parent,float x=0,float y=0){
        int id=graph.alloc();graph.setParent(id,parent,-1);
        slot(id)[sg::X]=x;slot(id)[sg::Y]=y;graph.markLocalInputChanged(id);
        return id;
    };
    auto sprite=[&](int parent,float x,float y,float w,float h){
        int id=node(parent);float* s=slot(id);
        s[sg::TYPE]=sg::T_SPRITE;s[sg::RX]=x;s[sg::RY]=y;s[sg::RW]=w;s[sg::RH]=h;
        graph.markBoundsDirty(id);return id;
    };
    auto expect=[&](int id,float x,float y,float w,float h){
        float actual[4];assert(graph.getLocalBounds(id,actual));
        const float expected[4]={x,y,w,h};
        for(int i=0;i<4;i++){
            if(std::abs(actual[i]-expected[i])>.001f)
                std::cerr<<"node "<<id<<" component "<<i<<": "<<actual[i]<<" != "<<expected[i]<<'\n';
            assert(std::abs(actual[i]-expected[i])<.001f);
        }
    };

    // LuckyPickerFinishRewardTip measures before reopening. Its item graphs contain the
    // icon, anchored number, and description as dynamic children; native FGUI item roots
    // and intermediate graphs need no JS facade for this query to include their contents.
    const int stage=node(0),tip=node(stage,900,200),box=node(tip,224,49),content=node(box);
    for(int i=0;i<2;i++){
        int item=node(content,i*165,0);
        sprite(node(item,0,-2),0,0,70,70);
        sprite(node(item,65,32),0,-12, i?25:70,24);
        sprite(node(item,35,62),i?-45:-20,-9.5,i?90:40,19);
    }
    expect(content,0,-2,255,73.5);
    for(float scale:{0.f,.001f,1.f,-1.f,2.f}){
        slot(tip)[sg::SY]=scale;slot(tip)[sg::VISIBLE]=0;
        slot(stage)[sg::ROT]=.4f;graph.markLocalInputChanged(tip);graph.markLocalInputChanged(stage);
        expect(content,0,-2,255,73.5);
    }
    slot(content)[sg::SX]=0;slot(content)[sg::SY]=0;
    slot(content)[sg::PVX]=120;slot(content)[sg::ROT]=.8f;graph.markLocalInputChanged(content);
    expect(content,0,-2,255,73.5); // the query's own transform is excluded too

    // Local queries must not overwrite canonical world matrices or publish a new frame seal.
    graph.compute(stage,sg::CM_CANONICAL_MAIN);
    std::vector<float> worlds;
    for(int id=1;id<=content;id++)for(int k=sg::WA;k<=sg::WALPHA;k++)worlds.push_back(slot(id)[k]);
    const auto epoch=graph.u32(sg::WEPOCH_BASE+content);
    expect(content,0,-2,255,73.5);
    size_t wi=0;
    for(int id=1;id<=content;id++)for(int k=sg::WA;k<=sg::WALPHA;k++)assert(worlds[wi++]==slot(id)[k]);
    assert(epoch==graph.u32(sg::WEPOCH_BASE+content));

    // Hidden and non-renderable descendant subtrees are excluded; empty layout graphs
    // contribute no artificial point even if they are positioned far from the content.
    int hidden=node(content,1000,1000);sprite(hidden,0,0,400,400);
    slot(hidden)[sg::VISIBLE]=0;graph.markLocalInputChanged(hidden);
    node(content,2000,2000);
    expect(content,0,-2,255,73.5);
    slot(hidden)[sg::VISIBLE]=1;slot(hidden)[sg::RENDERABLE]=0;graph.markLocalInputChanged(hidden);
    expect(content,0,-2,255,73.5);

    // Both quad storage formats use the same geometry as world bounds. Child transforms
    // still apply, including rotation and pivot; ancestor transforms never do.
    int quads=node(0),native=node(quads,7,9);
    sg::NativeQuad quad{};
    quad[0]=1;quad[1]=0;quad[2]=0;quad[3]=20;quad[4]=0;
    quad[5]=20;quad[6]=10;quad[7]=0;quad[8]=10;
    graph.setNativeQuads(native,{quad});
    slot(native)[sg::ROT]=std::acos(-1.f)/2;slot(native)[sg::PVX]=5;graph.markLocalInputChanged(native);
    expect(quads,-3,4,10,20);
    int packed=node(quads,30,0);float* s=slot(packed);
    s[sg::TYPE]=sg::T_MULTIQUAD;s[sg::QUADOFF]=0;s[sg::QUADCNT]=1;
    std::copy(quad.begin(),quad.end(),shared.data()+sg::QUAD_BASE+1);graph.markBoundsDirty(packed);
    expect(quads,-3,0,53,24);
    expect(node(0),0,0,0,0);
    float unused[4];assert(!graph.getLocalBounds(-1,unused));
    std::cout<<"native scenegraph local bounds: PASS\n";
}
