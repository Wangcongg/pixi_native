    var __bdDirty = [];   // getLocalBounds 域内被 JS 改写过 worldTransform 的 transform(域退出时从 buffer 恢复)
    var _tut = TRp.updateTransform;
    TRp.updateTransform = function(pt){
      if(globalThis.__SG>=2 && this.__b!==undefined && !this.__spT){   // spine 子树(__spT)始终走 JS 自算
        if(!globalThis.__sgBD) return;               // 域外:C++ 权威,JS 矩阵计算跳过
        this._parentID = -1;                          // 域内:强制全量重算 world(dirty 缓存会误判)
        __bdDirty.push(this);                         // 记录:域内 wt 会被写成局部空间值(污染),域退出时恢复
      }
      return _tut.call(this, pt);
    };
    var _cut = COp.updateTransform;
    COp.updateTransform = function(){
      // Native SceneGraph owns world matrices for ordinary nodes, Spine roots and slotObject
      // descendants alike.  Bounds/RenderTexture compatibility enters __sgBD explicitly and may
      // still request Pixi's recursive transform pass; the main frame never walks a JS tree.
      if(globalThis.__SG>=2 && this.__i>0 && !globalThis.__sgBD) return;
      return _cut.call(this); };
    // JS 作用域(__sgBD):域内变换全走 pixi 原生 JS(临时父/局部空间重算都正常),域外读 C++ buffer。
    // 需要包域的入口(游戏实际依赖面,已侦察确认):getLocalBounds、cacheAsBitmap 的 _initCachedDisplayObject(含把子树
    // 按局部空间渲染进 renderTexture —— UI 放大的根因)、renderer.generateTexture。均为低频操作,域内回退无感。
    // 异常安全:Pixi 的 getLocalBounds/_initCachedDisplayObject 会把 this.transform 临时换成
    // _tempDisplayObjectParent.transform、把 parent 置空,靠函数尾部换回——**没有 try/finally**。
    // 域内任何一次抛异常(子节点纹理失效/代理节点几何异常等)都会让对象永久留着临时 transform:
    // 它的 position 是另一个恒为 (0,0) 的 Point,于是 fgui 的 $x/$y 与 displayObject 从此脱节
    // ——而 GObject.setXY 在值没变时早退,再也不会重写,表现为"复用的弹窗第二次打开跑到左上角"
    // (实证:同一 displayObject、position.set 从未被调用、G.xy 正确而 disp=(0,0))。
    // 这里在域出口把 transform/parent 复位,把 Pixi 的这个缺陷挡在宿主层,不改业务、不吞异常。
    var __inBD = function(self, fn, args){
      globalThis.__sgBD++;
      // getLocalBounds 换出的是 transform / parent / _bounds 三件套,三者都要保证换回
      var __bdT=self&&self.transform, __bdP=self&&self.parent, __bdB=self&&self._bounds, __bdE=null;
      try{ return fn.apply(self, args); }
      catch(e){ __bdE=e; throw e; }                 // 只观察不吞:异常照常上抛
      finally{
      if(self&&__bdB&&self._bounds!==__bdB)self._bounds=__bdB;
      if(self&&__bdT&&self.transform!==__bdT){
        self.transform=__bdT;
        if((globalThis.__sgBdRestores=(globalThis.__sgBdRestores||0)+1)<=3)
          __log('[sg-guard] getLocalBounds 域异常退出,已复位 transform name='+((self.name||(self.constructor&&self.constructor.name))||'?')
            +' err='+((__bdE&&__bdE.message)||'(无异常但未复位)')+' | '+String((__bdE&&__bdE.stack)||'').replace(/\n\s*/g,' <- ').slice(0,300));
      }
      if(self&&__bdP&&self.parent!==__bdP)self.parent=__bdP;
      globalThis.__sgBD--;
      if(!globalThis.__sgBD && __bdDirty.length){    // 最外层域退出:被污染的 worldTransform 立即从 buffer 恢复(否则渲染用局部空间矩阵 → 画到屏幕外)
        for(var z=0;z<__bdDirty.length;z++){ var tt=__bdDirty[z]; if(tt.__b===undefined) continue;
          var bb=tt.__b, w=tt.worldTransform;
          w.a=SGB[bb+12]; w.b=SGB[bb+13]; w.c=SGB[bb+14]; w.d=SGB[bb+15]; w.tx=SGB[bb+16]; w.ty=SGB[bb+17];
          tt._worldID=((tt._worldID||0)+1)|0;        // bump → sprite 顶点缓存失效重算
        }
        __bdDirty.length=0;
      }
    } };
    var _glbD = DOp.getLocalBounds, _glbC = COp.getLocalBounds;
    // world bounds 已随 native present 计算并缓存。稳定节点直接 O(1) 读共享区；构建期/空几何仍走 Pixi 原实现。
    var _gbD=DOp.getBounds;
    var __boundsHasPending=function(o){if(__renderDirty.has(o))return true;var owner=o&&o.__i|0;if(!(owner>0)||!__renderDirty.size)return false;var hit=false;__renderDirty.forEach(function(d){if(hit)return;var id=d&&d.__i|0;for(var g=0;id>0&&g++<32768;id=__nativeParentOf(id))if(id===owner){hit=true;break;}});return hit;};
    // Prefer the native subtree AABB when the node is stable. This avoids walking a JS
    // facade tree that intentionally omits native-only FGUI leaves. Keep the Pixi
    // compatibility domain only for empty/unstable geometry (cacheAsBitmap / generateTexture
    // still enter __inBD on their own entry points).
    var __nativeLocalBoundsOf=function(o,rect){
      if(!o||!(o.__i>0)||typeof globalThis.__sgUpdateBounds!=='function'||globalThis.__sgBD||__boundsHasPending(o))return null;
      try{
        {var rootId=o.__i|0,parentId=__nativeParentOf(rootId),g=0;while(parentId>0&&g++<32768){rootId=parentId;parentId=__nativeParentOf(rootId);}
         if(SGB[DB]>0||SGB[DBB+rootId]>0.5)__sgCanonicalBarrier(rootId);}
        __sgUpdateBounds(o.__i);if(globalThis.__sgWriteback)globalThis.__sgWriteback();
        var d=BB+o.__i*BSF+4,x0=SGB[d],y0=SGB[d+1],x1=SGB[d+2],y1=SGB[d+3];
        if(!(x0<=x1&&y0<=y1))return null;
        var b=o.__i*ST,a=SGB[b+12],bb=SGB[b+13],c=SGB[b+14],dd=SGB[b+15],tx=SGB[b+16],ty=SGB[b+17],det=a*dd-bb*c;
        if(Math.abs(det)<1e-12)return null;
        var ia=dd/det,ib=-bb/det,ic=-c/det,id=a/det,lx0=Infinity,ly0=Infinity,lx1=-Infinity,ly1=-Infinity;
        var xs=[x0,x1],ys=[y0,y1];
        for(var yi=0;yi<2;yi++)for(var xi=0;xi<2;xi++){var px=xs[xi]-tx,py=ys[yi]-ty,lx=ia*px+ic*py,ly=ib*px+id*py;if(lx<lx0)lx0=lx;if(ly<ly0)ly0=ly;if(lx>lx1)lx1=lx;if(ly>ly1)ly1=ly;}
        rect=rect||new PIXI.Rectangle();rect.x=lx0;rect.y=ly0;rect.width=lx1-lx0;rect.height=ly1-ly0;return rect;
      }catch(e){return null;}
    };
    DOp.getLocalBounds = function(rect){
      __flushMutationReads();
      if(globalThis.__SG>=2){var nb=__nativeLocalBoundsOf(this,rect);if(nb)return nb;}
      if(globalThis.__SG<2) return _glbD.apply(this,arguments);
      return __inBD(this,_glbD,arguments);
    };
    COp.getLocalBounds = function(rect){
      __flushMutationReads();
      if(globalThis.__SG>=2){var nb=__nativeLocalBoundsOf(this,rect);if(nb)return nb;}
      if(globalThis.__SG<2) return _glbC.apply(this,arguments);
      return __inBD(this,_glbC,arguments);
    };
    DOp.getBounds=function(skipUpdate,rect){
      __flushMutationReads();
      if(this.__i>0&&typeof globalThis.__sgUpdateBounds==='function'&&!globalThis.__sgBD&&!__boundsHasPending(this)){
        try{
          if(!skipUpdate){var rootId=this.__i|0,parentId=__nativeParentOf(rootId),g=0;while(parentId>0&&g++<32768){rootId=parentId;parentId=__nativeParentOf(rootId);}
           if(SGB[DB]>0||SGB[DBB+rootId]>0.5)__sgCanonicalBarrier(rootId);}
          __sgUpdateBounds(this.__i, skipUpdate?0:1);if(globalThis.__sgWriteback)globalThis.__sgWriteback();var d=BB+this.__i*BSF+4,x0=SGB[d],y0=SGB[d+1],x1=SGB[d+2],y1=SGB[d+3];
          if(x0<=x1&&y0<=y1){rect=rect||(this._boundsRect||(this._boundsRect=new PIXI.Rectangle()));rect.x=x0;rect.y=y0;rect.width=x1-x0;rect.height=y1-y0;return rect;}
        }catch(e){}
      }
      return _gbD.apply(this,arguments);
    };
    // EventBoundary 的递归 hitTest 改成一次 native DFS。target 是实际收事件对象，witness 是提供命中几何的 auto 子节点。
    var __hitPoint=new PIXI.Point();
    var __hitToLocal=function(o,x,y,out){
      if(o&&o.__i>0){
        var b=o.__i*ST,a=SGB[b+12],bb=SGB[b+13],c=SGB[b+14],d=SGB[b+15],tx=SGB[b+16],ty=SGB[b+17],det=a*d-bb*c;
        if(Math.abs(det)>1e-12){var px=x-tx,py=y-ty;out.x=(d*px-c*py)/det;out.y=(a*py-bb*px)/det;return true;}
      }
      var wt=o&&o.worldTransform;if(!wt||!wt.applyInverse)return false;wt.applyInverse(__hitPoint,out);return true;
    };
    // Sprite/Graphics.containsPoint 内部仍会读取 Pixi worldTransform。mode 4 下事件可能落在本帧
    // native compute 与 JS writeback 之间，短暂替换成权威矩阵，检查后恢复，避免 pivot/scale 弹窗误判。
    var __nativeContainsPoint=function(o,p){
      if(!o||typeof o.containsPoint!=='function')return false;
      if(o.__nativeV8LocalHit){var local=new PIXI.Point();return __hitToLocal(o,p.x,p.y,local)&&o.containsPoint(local);}
      var wt=o.worldTransform;if(!(o.__i>0)||!wt)return o.containsPoint(p);
      // Lightweight handles read their authoritative world matrix directly from the shared native
      // scene buffer, so there is nothing to patch (and their matrix accessors are intentionally read-only).
      if(wt.__nativeSharedMatrix)return o.containsPoint(p);
      var sv=[wt.a,wt.b,wt.c,wt.d,wt.tx,wt.ty],b=o.__i*ST;
      wt.a=SGB[b+12];wt.b=SGB[b+13];wt.c=SGB[b+14];wt.d=SGB[b+15];wt.tx=SGB[b+16];wt.ty=SGB[b+17];
      try{return o.containsPoint(p);}finally{wt.a=sv[0];wt.b=sv[1];wt.c=sv[2];wt.d=sv[3];wt.tx=sv[4];wt.ty=sv[5];}
    };
    var __nativeGeometryPoint=new PIXI.Point();
    var __pointInTriangle=function(px,py,ax,ay,bx,by,cx,cy){
      var area=(bx-ax)*(cy-ay)-(by-ay)*(cx-ax);
      if(Math.abs(area)<1e-6)return false;
      var ab=(px-bx)*(ay-by)-(ax-bx)*(py-by);
      var bc=(px-cx)*(by-cy)-(bx-cx)*(py-cy);
      var ca=(px-ax)*(cy-ay)-(cx-ax)*(py-ay);
      var hasNeg=ab<-1e-4||bc<-1e-4||ca<-1e-4;
      var hasPos=ab>1e-4||bc>1e-4||ca>1e-4;
      return !(hasNeg&&hasPos);
    };
    var __nativeGeometryContainsPoint=function(o,x,y){
      if(!o||!(o.__i>0)||!__hitToLocal(o,x,y,__nativeGeometryPoint))return false;
      var b=o.__i*ST;
      var _ngt=SGB[b+oTYPE]|0;if(!((SGB[b+oHITFLAGS]|0)&32))return false;
      // T_NATIVE_MULTIQUAD(帧库回放等 C++ 持有 quad):三角测试在 C++ 完成(quad 同为节点局部系)
      if(_ngt===5)return typeof globalThis.__sgNativeQuadHit==='function'?!!globalThis.__sgNativeQuadHit(o.__i,__nativeGeometryPoint.x,__nativeGeometryPoint.y):false;
      if(_ngt!==3)return false;
      var off=SGB[b+oQUADOFF]|0,cnt=SGB[b+oQUADCNT]|0;
      if(off<0||cnt<=0||off+cnt>QCAP)return false;
      var px=__nativeGeometryPoint.x,py=__nativeGeometryPoint.y;
      for(var i=0;i<cnt;i++){
        var q=QB+1+(off+i)*QSF;
        if(SGB[q]<0||SGB[q+18]<=1e-4)continue;
        var x0=SGB[q+1],y0=SGB[q+2],x1=SGB[q+3],y1=SGB[q+4];
        var x2=SGB[q+5],y2=SGB[q+6],x3=SGB[q+7],y3=SGB[q+8];
        if(__pointInTriangle(px,py,x0,y0,x1,y1,x2,y2)||
           __pointInTriangle(px,py,x0,y0,x2,y2,x3,y3))return true;
      }
      return false;
    };
    // 吞点击诊断(__HIT_DIAG=1 开):__heDiag 打开时记录候选被哪一步拒绝(move 不启用)。
    var __heDiag=0,__heWhy='';
    var __heName=function(o){
      if(!o)return 'null';
      var u=o.UIOwner,un=u&&u.name,ut=u&&u.constructor&&u.constructor.name,ru=u&&u.resourceURL;
      var nm=o.name||un||ut||(o.constructor&&o.constructor.name)||'?';
      var si=o.parent&&o.parent.children?o.parent.children.indexOf(o):-1;
      var id=o.__i|0,b=id>0?id*ST:0,event=id>0?('/e'+(SGB[b+45]|0)+'i'+(SGB[b+46]|0)+'h'+(SGB[b+oHITFLAGS]|0)):'';
      return nm+(ut&&ut!==nm?'<'+ut+'>':'')+(ru?'['+ru+']':'')+'#'+id+'@'+si+event;
    };
    // Build propagation paths from the authoritative native hierarchy. Native package leaves that
    // intentionally have no JS facade are skipped, while their semantic GObject ancestors remain
    // in the path. This removes input dispatch's dependency on DisplayObject.parent/children and is
    // a prerequisite for deleting the compatibility display tree altogether.
    var __nativeEventChain=function(from,root){
      if(!from||!root)return null;
      if(typeof __nativeParentOf==='function'&&(from.__i|0)>0&&(root.__i|0)>0){
        var out=[],id=from.__i|0,rootId=root.__i|0,g=0,reached=false;
        while(id>0&&g++<2048){var o=__sgObjs[id];if(o)out.push(o);if(id===rootId){reached=true;break;}id=__nativeParentOf(id);}
        return reached?out:null;
      }
      var out=[],p=from,g=0;while(p&&g++<2048){out.push(p);if(p===root)return out;p=p.parent;}return null;
    };
    // FederatedEvent.composedPath() normally rebuilds its route by following DisplayObject.parent.
    // That is no longer authoritative once an FGUI component contains native-only descendants,
    // and a popup replacement can leave the old JS parent chain detached for one pointer-move.
    // Use the retained C++ hierarchy for both dispatch and stale hover/down target validation.
    // Besides removing the recurring "disconnected target" exception, this lets lightweight
    // business handles receive capture/bubble events without retaining intermediate JS children.
    if(PIXI.EventBoundary&&!PIXI.EventBoundary.prototype.__nativePropagationPath){
      var _nativeEventBoundaryProto=PIXI.EventBoundary.prototype;
      var _eventPropagationPath=_nativeEventBoundaryProto.propagationPath;
      var _eventFindMountedTarget=_nativeEventBoundaryProto.findMountedTarget;
      var __sgDetachedPathLogN=0;
      _nativeEventBoundaryProto.propagationPath=function(target){
        var chain=__nativeEventChain(target,this.rootTarget);
        if(chain){chain.reverse();return chain;}
        // A target destroyed between hit-test and dispatch is no longer a valid event endpoint.
        // Returning the root is the browser-equivalent mounted remainder and avoids dispatching
        // a click/pointermove into an object whose native handle has already been reclaimed.
        if(target&&(target._destroyed||((target.__i|0)>0&&__nativeParentOf(target.__i|0)<=0)))return this.rootTarget?[this.rootTarget]:[];
        try{return _eventPropagationPath.call(this,target);}catch(e){
          // Remaining detachment shape: the target still has a live native parent id but its chain
          // no longer reaches this boundary's root (e.g. a popup replacement parks the subtree on
          // an overlay for a frame), and the JS parent chain is broken too. Never rethrow — the
          // exception aborts mapEvent, so root-level listeners (scratch card's stage pointermove)
          // and the cursor refresh behind it were silently dropped tens of thousands of times.
          // Return the minimal mounted path root→target: root listeners keep capture/bubble, and
          // the resulting composedPath immediately replaces the stale trackingData.overTargets.
          if(__sgDetachedPathLogN<8){__sgDetachedPathLogN++;try{console.warn('[sg-events] re-rooted disconnected event target:',__heName(target));}catch(_e){}}
          return this.rootTarget&&this.rootTarget!==target?[this.rootTarget,target]:[target];
        }
      };
      _nativeEventBoundaryProto.findMountedTarget=function(path){
        if(!path||!path.length)return null;
        for(var pi=path.length-1;pi>=0;pi--){var candidate=path[pi],chain=__nativeEventChain(candidate,this.rootTarget);if(chain)return candidate;}
        return _eventFindMountedTarget.call(this,path);
      };
      _nativeEventBoundaryProto.__nativePropagationPath=true;
    }
    var __hitExact=function(root,target,witness,x,y){
      if(!target||!witness||!target.isInteractive||!target.isInteractive()){if(__heDiag)__heWhy='非interactive '+__heName(target);return false;}
      var chain=__nativeEventChain(witness,root);if(!chain){if(__heDiag)__heWhy='witness脱树 '+__heName(witness);return false;}
      __hitPoint.x=x;__hitPoint.y=y;
      for(var i=chain.length-1;i>=0;i--){var o=chain[i];if(!o||o.visible===false||o.renderable===false||o.isMask||o.eventMode==='none'){if(__heDiag)__heWhy='链上不可见/none '+__heName(o);return false;}
        // chain 是 witness -> root，循环方向是 root -> witness。只有当前节点下面仍要继续深入时
        // 才检查 interactiveChildren；witness 自身为 false 仍然可以作为事件 target。
        if(i>0&&o.interactiveChildren===false){if(__heDiag)__heWhy='interactiveChildren=0 '+__heName(o);return false;}
        if(o.hitArea){if(!o.hitArea.contains||!__hitToLocal(o,x,y,__hitPoint)){if(__heDiag)__heWhy='hitArea不可用 '+__heName(o);return false;}var ok=o.hitArea.contains(__hitPoint.x,__hitPoint.y),overflowAncestor=!ok&&o!==witness&&((SGB[o.__i*ST+oHITFLAGS]|0)&128)!==0;__hitPoint.x=x;__hitPoint.y=y;if(!ok&&!overflowAncestor){if(__heDiag)__heWhy='hitArea拒绝 '+__heName(o)+'('+(o.hitArea.x|0)+','+(o.hitArea.y|0)+' '+(o.hitArea.width|0)+'x'+(o.hitArea.height|0)+')';return false;}}
        var mk=o._mask,mo=mk?(mk.isMaskData?mk.maskObject:mk):null;if(mo&&!__nativeContainsPoint(mo,__hitPoint)){if(__heDiag)__heWhy='mask拒绝 '+__heName(o);return false;}
      }
      var witnessFlags=SGB[witness.__i*ST+oHITFLAGS]|0;
      var descendantWitness=(witnessFlags&512)!==0&&witness.interactiveChildren!==false;
      // Native may have collapsed browser child meshes onto this one facade. Their geometry must
      // obey the owner's live interactiveChildren gate even though there are no JS/native child
      // nodes left for EventBoundary to prune. An explicit hitArea remains true owner geometry.
      if((witnessFlags&512)&&witness.interactiveChildren===false&&!witness.hitArea){
        if(__heDiag)__heWhy='compressed descendants disabled '+__heName(witness);return false;
      }
      if(witness.eventMode==='passive'&&!descendantWitness){if(__heDiag)__heWhy='witness passive '+__heName(witness);return false;}
      if((witnessFlags&256)&&witness.eventMode!=='static'&&witness.eventMode!=='dynamic'){
        if(__heDiag)__heWhy='self-only native geometry '+__heName(witness);return false;
      }
      if(!witness.hitArea&&(!(witnessFlags&32)?!__nativeContainsPoint(witness,__hitPoint):!__nativeGeometryContainsPoint(witness,x,y))){if(__heDiag)__heWhy='containsPoint拒绝 '+__heName(witness);return false;}
      for(var q=0;q<chain.length;q++)if(chain[q]===target)return true;
      if(__heDiag)__heWhy='target不在链上 '+__heName(target);
      return false;
    };
    if(PIXI.EventBoundary&&typeof globalThis.__sgHit==='function'){
      var _ebHit=PIXI.EventBoundary.prototype.hitTest;
      PIXI.EventBoundary.prototype.hitTest=function(x,y){
        var root=this.rootTarget;if(!root||!(root.__i>0))return _ebHit.call(this,x,y);
        // Pointer handlers can mutate transforms after the previous present. ScrollPane does this
        // on every pointermove; a pointerup may arrive before another RAF. H5 hit-testing observes
        // those live transforms, while querying Native's previous world matrix makes the release
        // appear to land on the original pressed item and Pixi incorrectly synthesises pointertap.
        // Flush ordered structure first, then run SceneGraph's incremental compute so hit bounds
        // and the release propagation path use the same positions as the browser runtime.
        if(typeof globalThis.__sgBatchReadBarrier==='function')try{__sgBatchReadBarrier();}catch(e){}
        if(typeof globalThis.__sgCompute==='function')try{__sgCanonicalBarrier(root.__i|0);}catch(e){}   // P1-A+P1-C:hit 前收敛=canonical 原子入口(compute+writeback 不可分离)
        // Explicit hitTest must observe geometry authored since the last presentation.
        // World matrices alone do not publish a newly filled/cleared Graphics mesh.
        __flushRenderData(root);
        var collectAll=!!(this._isPointerMoveEvent&&this.enableGlobalMoveEvents),n=0;
        try{n=__sgHit(root.__i,x,y,collectAll)|0;}catch(e){return _ebHit.call(this,x,y);}
        if(globalThis.__sgWriteback)globalThis.__sgWriteback();   // P1-C 保险:C++ hitTest 内部 canonical 的清单也被消费(空清单=零成本)
        if(collectAll){var an=SGB[HAC]|0;this._allInteractiveElements.length=0;for(var ai=0;ai<an&&ai<HCAP;ai++){var ao=__sgObjs[SGB[HAB+ai]|0];if(ao&&ao.__i>0&&!ao._destroyed)this._allInteractiveElements.push(ao);}}
        var diag=!this._isPointerMoveEvent&&globalThis.__HIT_DIAG===1&&(globalThis.__hitDiagN||0)<60,rej='';
        __heDiag=diag?1:0;
        for(var i=0;i<n&&i<HCAP;i++){var targetId=SGB[HR+1+i*2]|0,witnessId=SGB[HR+2+i*2]|0,target=__sgObjs[targetId],witness=__sgObjs[witnessId],lazyHit=globalThis.__nativeFguiResolveHitNode;
          // An authored Native FGUI list item intentionally has no JS facade until it is used.
          // Materialise only the item under the pointer; nested native witnesses resolve to their
          // nearest registered list-item root. Legacy/Pixi candidates keep the unchanged lookup.
          if(!target&&typeof lazyHit==='function')target=lazyHit(targetId);
          if(!witness&&witnessId===targetId)witness=target;
          if(!witness&&typeof lazyHit==='function')witness=lazyHit(witnessId);
          witness=witness||target;
          if(!__hitExact(root,target,witness,x,y)){if(diag&&i<4)rej+=' ['+i+']'+__heWhy;continue;}
          __heDiag=0;
          var path=__nativeEventChain(target,root);if(!path)continue;
          if(diag){
            var old=null;try{old=_ebHit.call(this,x,y);}catch(e){}
            var names=[];for(var pi=0;pi<Math.min(path.length,8);pi++)names.push(__heName(path[pi]));
            var oldNames=[],op=old,og=0;while(op&&og++<8){oldNames.push(__heName(op));if(op===root)break;op=op.parent;}
            var candidates=[];for(var ci=0;ci<Math.min(n,8);ci++)candidates.push(__heName(__sgObjs[SGB[HR+1+ci*2]|0])+'/'+__heName(__sgObjs[SGB[HR+2+ci*2]|0]));
            globalThis.__hitDiagN=(globalThis.__hitDiagN||0)+1;
            __log('[hit-target] '+(globalThis.__nativePointerType||'?')+' ('+(x|0)+','+(y|0)+') native='+__heName(target)+' witness='+__heName(witness)+' pixi='+__heName(old)+' path='+names.join(' <- ')+' pixiPath='+oldNames.join(' <- ')+' rejected='+rej+' candidates='+candidates.join(' | '));
          }
          // FGUI buttons set cursor explicitly. Runtime Pixi containers often bind pointertap
          // directly instead; infer the same browser cursor from activation listeners so every
          // native interactive control gets a pointing hand without view-specific patches.
          var activation=false;
          for(var ac=0;ac<path.length&&!activation;ac++){
            var ao=path[ac];if(!ao)continue;
            if(ao.onpointertap||ao.onclick||ao.onmousedown||ao.onpointerdown)activation=true;
            else if(typeof ao.listenerCount==='function')activation=ao.listenerCount('pointertap')>0||ao.listenerCount('click')>0||ao.listenerCount('tap')>0;
          }
          if(activation){if(!target.cursor||target.__nativeAutoCursor){target.cursor='pointer';target.__nativeAutoCursor=true;}}
          else if(target.__nativeAutoCursor){target.cursor=null;target.__nativeAutoCursor=false;}
          this._hitElements=path;globalThis.__nativeHitCount=(globalThis.__nativeHitCount||0)+1;return target;}
        __heDiag=0;
        if(diag){globalThis.__hitDiagN=(globalThis.__hitDiagN||0)+1;__log('[hit-miss] ('+(x|0)+','+(y|0)+') 候选'+n+(n?rej:' broad-phase 0候选(命中区/裁剪在 C++ 就被排除)'));}
        this._hitElements.length=0;return null;
      };
    }
