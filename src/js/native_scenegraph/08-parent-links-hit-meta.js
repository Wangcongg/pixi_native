    // ---- SG_PARENT_SIDECAR_RECEIPT_END ----
    var __nativeParentOf=function(id){id=id|0;if(id<=0||id>=32768)return -1;var p=__nativeParents[id];if(p===-2&&typeof globalThis.__sgNativeParent==='function'){p=__sgNativeParent(id)|0;__nativeParents[id]=p;}return p;};
    // Native FGUI keeps authored list items facade-free until they are actually queried or hit.
    // Export the read-only numeric parent lookup so that the lazy facade layer can resolve a
    // nested hit back to its item root without recreating or reparenting the whole list.
    globalThis.__nativeParentOf=__nativeParentOf;
    var __nativeRootId=function(id){id=id|0;var root=id,p=__nativeParentOf(id),g=0;while(p>0&&g++<32768){root=p;p=__nativeParentOf(p);}return root;};
    var __linkedCompat=__linkedToRoot,__visibleCompat=__treeVisible;
    __linkedToRoot=function(o,root){
      if(!root)return true;var id=o&&o.__i|0,rootId=root&&root.__i|0;if(!(id>0&&rootId>0))return __linkedCompat(o,root);
      for(var g=0;id>0&&g++<32768;id=__nativeParentOf(id))if(id===rootId)return true;return false;
    };
    __treeVisible=function(o){
      var id=o&&o.__i|0;if(!(id>0))return __visibleCompat(o);for(var g=0;id>0&&g++<32768;id=__nativeParentOf(id)){var b=id*ST;if(SGB[b+oVIS]===0||SGB[b+28]===0)return false;}return g<32768;
    };
    // 一次父链遍历同时判"挂在 root 树上"与"可见链":0=不挂树 1=挂且可见 2=挂但不可见。
    // 每帧对 __ov 全员各爬两次链(linked+visible)的跨桥成本会随 spine 数线性放大,合并成一次。
    var __linkedVisState=function(o,root){
      var id=o&&o.__i|0;if(!(id>0))return 0;
      var rootId=root&&root.__i|0;if(!(rootId>0))return __linkedCompat(o,root)?(__treeVisible(o)?1:2):0;
      var vis=1;
      for(var g=0;id>0&&g++<32768;id=__nativeParentOf(id)){
        var b=id*ST;if(vis===1&&(SGB[b+oVIS]===0||SGB[b+28]===0))vis=2;
        if(id===rootId)return vis;
      }
      return 0;
    };
    globalThis.__sgNativeParentHandles=__nativeParents;
    // Native is authoritative for sibling order. Keep an event-driven set instead of walking the
    // complete JS display tree every present just to find the handful of sortDirty containers.
    var __orderDirty=new Set();
    var __markOrderDirty=function(o){if(o&&!o._destroyed&&o.children)__orderDirty.add(o);};
    globalThis.__sgMarkOrderDirty=__markOrderDirty;
    var __instrumentSortDirty=function(o){
      if(!o||!o.children||o.__sgSortDirtyHook)return;o.__sgSortDirtyHook=1;
      var d=Object.getOwnPropertyDescriptor(o,'sortDirty');if(d&&(d.get||d.set||!d.configurable))return;
      var value=!!o.sortDirty;o.__sgSortDirtyValue=value;
      try{Object.defineProperty(o,'sortDirty',{configurable:true,enumerable:true,get:function(){return this.__sgSortDirtyValue;},set:function(v){v=!!v;this.__sgSortDirtyValue=v;if(v)__markOrderDirty(this);}});}catch(e){}
      if(value)__markOrderDirty(o);
    };
    var __eventModeCode=function(v){return v==='none'?1:(v==='passive'?2:(v==='static'?3:(v==='dynamic'?4:0)));};
    var __hitAreaAABB=function(a){
      if(!a)return null;var x=Number(a.x)||0,y=Number(a.y)||0,w=Number(a.width),h=Number(a.height);
      if(isFinite(w)&&isFinite(h))return [x,y,x+w,y+h];
      if(isFinite(a.radius)){var r=Math.abs(Number(a.radius));return [x-r,y-r,x+r,y+r];}
      var p=a.points;if(p&&p.length>=2){var x0=Number(p[0]),y0=Number(p[1]),x1=x0,y1=y0;for(var i=2;i+1<p.length;i+=2){var px=Number(p[i]),py=Number(p[i+1]);if(px<x0)x0=px;if(py<y0)y0=py;if(px>x1)x1=px;if(py>y1)y1=py;}return [x0,y0,x1,y1];}
      // 自定义 IHitArea 没有可读尺寸时不允许 broad-phase 漏判；精确 contains 仍在 JS 执行。
      return [-1000000000,-1000000000,1000000000,1000000000];
    };
    var __writeHitMeta=function(o){
      if(!o||!(o.__i>0))return;var i=o.__i,b=i*ST,ha=o.hitArea,flags=globalThis.__nativeMaskContract.hitFlags(o,!!ha,typeof o.containsPoint==='function');
      SGB[b+45]=__eventModeCode(o.eventMode);SGB[b+46]=(o.interactiveChildren===false?0:1);SGB[b+47]=flags;
      var d=BB+i*BSF+16,r=__hitAreaAABB(ha);if(r){SGB[d]=Math.min(r[0],r[2]);SGB[d+1]=Math.min(r[1],r[3]);SGB[d+2]=Math.max(r[0],r[2]);SGB[d+3]=Math.max(r[1],r[3]);}
      else{SGB[d]=SGB[d+1]=1;SGB[d+2]=SGB[d+3]=-1;}__markBoundsNode(i);
    };
    // fgui 惯用"赋一次 Rectangle,之后就地改 x/width/height"(GComponent.updateOpaque/GList.handleSizeChanged),
    // 不触发 hitArea setter → 共享区 hit AABB 停在赋值时刻(常为 0×0),C++ broad-phase 按旧区域裁掉整棵子树
    // (挂树后再 relayout 的组件点击全灭/间歇失灵)。给赋进来的 shape 装字段访问器,就地修改时回写 hit meta。
    var __instrumentHitArea=function(o,a){
      if(!a||typeof a!=='object')return;
      if(!a.__sgHAOwners)a.__sgHAOwners=new Set();
      a.__sgHAOwners.add(o);
      if(a.__sgHAHook)return; a.__sgHAHook=1;
      ['x','y','width','height','radius'].forEach(function(k){
        if(!(k in a))return;                                        // Polygon 等没有该字段
        var d=Object.getOwnPropertyDescriptor(a,k);
        if(d&&(d.get||d.set||!d.configurable))return;               // 已是访问器/不可改 → 不重复包
        var tag='__sgHA_'+k; a[tag]=a[k];
        try{Object.defineProperty(a,k,{configurable:true,enumerable:true,
          get:function(){return this[tag];},
          set:function(v){var ch=this[tag]!==v;this[tag]=v;if(ch&&this.__sgHAOwners)this.__sgHAOwners.forEach(__writeHitMeta);}});}catch(e){}
      });
    };
    // Pixi v7 的事件字段是 prototype mixin。改成共享槽写钩子，赋值时无需等待下一帧扫描。
    var __emd=Object.getOwnPropertyDescriptor(DOp,'eventMode');
    if(__emd&&__emd.get&&__emd.set&&__emd.configurable)Object.defineProperty(DOp,'eventMode',{configurable:true,get:__emd.get,set:function(v){__emd.set.call(this,v);__writeHitMeta(this);}});
    var __icd=Object.getOwnPropertyDescriptor(DOp,'interactiveChildren');
    if(__icd&&__icd.configurable)Object.defineProperty(DOp,'interactiveChildren',{configurable:true,get:function(){return this.__sgInteractiveChildren===undefined?true:this.__sgInteractiveChildren;},set:function(v){
      // Honor the live Pixi/FGUI gate exactly. ScrollPane business code deliberately disables a
      // list's descendants while dragging so pointerup cannot become pointertap on a reward item.
      // Compound list children that must stay interactive are handled explicitly by configureList.
      this.__sgInteractiveChildren=!!v;__writeHitMeta(this);
    }});
    var __had=Object.getOwnPropertyDescriptor(DOp,'hitArea');
    if(__had&&__had.configurable)Object.defineProperty(DOp,'hitArea',{configurable:true,get:function(){return this.__sgHitArea||null;},set:function(v){this.__sgHitArea=v||null;if(v)__instrumentHitArea(this,v);__writeHitMeta(this);}});
