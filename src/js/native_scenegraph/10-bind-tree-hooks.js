    // (4) 绑定:分配节点 + 打 __b/__o + 写初值
    var __bind = function(o){
      // Lightweight NativeContainerProxy owns its own addChild implementation, so children that
      // override updateTransform (notably Spine) do not pass through the patched PIXI.Container
      // addChild hook. Register them at the common bind boundary; otherwise the Spine itself keeps
      // rendering but slot objects are never updated and remain stacked at the origin.
      __ovChk(o);
      __instrumentSortDirty(o);
      // __i===-1 是上一次 __sgAlloc 失败留下的瞬时标记,不是有效绑定——必须允许重试,否则
      // 一次容量抖动就把对象永久毒化(之后任何 addChild 都 throw 'not native-bound')。
      //
      // 槽位所有权校验:__i 有效的前提是"这个槽现在确实归我"。R.adopt(o,id) 会把 __sgObjs[id]
      // 改指新主人却不清旧主人的 __i(FGUI facade 领养 C++ 既有节点时发生),旧对象就此持有
      // 一个不属于它的 id——之后它读写的是别人的变换(表现为跑到别处/(0,0),或被别人的写覆盖),
      // 且因为早退永远得不到修复。所有权不符即视为未绑定,重新分配一个干净槽位并回灌 JS 值。
      if (o.__i!==undefined && o.__i!==-1){
        if(__sgObjs[o.__i]===o) return o.__i;
        var __stale=(globalThis.__sgStaleRebinds=(globalThis.__sgStaleRebinds||0)+1);
        if(__stale<=3)__log('[sg-guard] 槽位所有权失配,重新绑定 id='+o.__i+' name='+((o.name||(o.constructor&&o.constructor.name))||'?'));
        o.__i=undefined;
        // 失配=槽已属别人:只撤销租约(读写落暂存),绝不从槽 capture(v2 §3.2,经 runRelease)。
        __sgReleaseSlotLease(o,false);
      }
      // 槽位失配后 freeList 可能把同一个 native id 重新分给原 Spine。native 的 __spClear(id) 已清掉
      // 注册，但若只比较数值 id，JS 会误判“仍已注册”而跳过 slot/UV 提交。每次真正重绑都开启
      // 新注册代次；__wrSpine 会据此清 slot 标记并完整重提。
      if(o.skeleton)o.__spRegisteredInst=undefined;
      // capture-first(lease contract runBind,影子消灭阶段0):在触碰任何 __o/__b 之前从当前
      // 真值源快照 9 分量(此刻租约未激活/已过期,accessor 读走暂存);populate 用快照灌槽,
      // 不再回读 accessor——旧顺序(claim→instrument→读 _x)在双态 getter 下会读到新槽默认值
      // 丢掉暂存值(docs/shadow-elimination-plan.md §3.1,评审实锤)。开关关闭时 capture 读到的
      // 就是影子值,与旧回灌语义逐位一致。
      var t=o.transform, i=-1, b=0;
      // 初始拷贝与 setter 同守卫:对象在【未绑定期】被写入非有限值时,setter 因无槽位短路
      // (不落槽也不报警),毒值留在 JS 字段里;绑定时若原样拷入,就绕过了整条 NaN 防线
      // (实证:未绑定期间被写 rotation=NaN 的门面复挂后世界矩阵永久退化,内容"干净消失")。
      // 非有限→保留 alloc 语义默认(缩放 1,其余 0)并报警点名。
      var __f=function(v,d,off){ if(isFinite(v))return v; __sgReportNaN(off); return d; };
      // 混合门面的 transform 可能持有 NativePoint(共享槽视图,_x 读 owner.__i 定位的槽)。
      // capture 发生在 alloc 之前,此时读它得 undefined(旧顺序在 claim 后读到新槽默认值再幂等
      // 灌回)。这类分量标记 __SG_SKIP:populate 跳过,槽保持 claim 后的 alloc/authored 值——
      // 与旧行为等值且不误报 NaN。rotation 是普通字段,保持原 __f 语义(undefined 照旧报警)。
      var __SG_SKIP={};
      var __capPt=function(p){return (p&&p.owner&&p.owner.__nativeNodeProxy)?__SG_SKIP:p._x;};
      var __capPtY=function(p){return (p&&p.owner&&p.owner.__nativeNodeProxy)?__SG_SKIP:p._y;};
      var __bindIO={
        capture:function(){ return t?{px:__capPt(t.position),py:__capPtY(t.position),sx:__capPt(t.scale),sy:__capPtY(t.scale),pvx:__capPt(t.pivot),pvy:__capPtY(t.pivot),skx:__capPt(t.skew),sky:__capPtY(t.skew),rot:t._rotation}:null; },
        claim:function(){
          i=__sgAlloc(); if(i<0){ o.__i=-1; return false; }
          __sgClaimSlot(o,i,'bind'); if(o.skeleton)o.skeleton.__spInst=i; b=i*ST; if(t)t.__b=b;
          if(t){ __instrumentPoint(t.position,b+oX,null,o); __instrumentPoint(t.scale,b+oSX,null,o); __instrumentPoint(t.pivot,b+oPVX,null,o); __instrumentPoint(t.skew,b+oSKX,null,o); __instrumentRotation(t,b,o); }
          return true;
        },
        populate:function(vals){ if(!t||!vals)return;
          if(vals.px!==__SG_SKIP){SGB[b+oX]=__f(vals.px,0,b+oX); SGB[b+oY]=__f(vals.py,0,b+oY);}
          if(vals.sx!==__SG_SKIP){SGB[b+oSX]=__f(vals.sx,1,b+oSX); SGB[b+oSY]=__f(vals.sy,1,b+oSY);}
          if(vals.pvx!==__SG_SKIP){SGB[b+oPVX]=__f(vals.pvx,0,b+oPVX); SGB[b+6]=__f(vals.pvy,0,b+6);}
          if(vals.skx!==__SG_SKIP){SGB[b+oSKX]=__f(vals.skx,0,b+oSKX); SGB[b+8]=__f(vals.sky,0,b+8);}
          SGB[b+oROT]=__f(vals.rot,0,b+oROT);
          // 写共享槽必须显式标脏(与 __refreshTransformBinding 的 populate 对称)。此前这里
          // 隐式依赖 C++ alloc 的 pending_ 首帧兜底——但该兜底可被"帧中 compute(toGlobal/
          // generateTexture 触发)消费队列时节点尚未挂树(detached)"丢弃,丢失后无人再补,
          // 节点带着 alloc 时的 identity 世界矩阵渲染在左上角(轮盘左上角病灶链之一)。
          globalThis.__sgDirty=1; __markD(b); },
        activate:function(){ __sgActivateTransformLease(o); }
      };
      var __bindOk=__sgLease?__sgLease.runBind(__bindIO)
        :(function(io){var vv=io.capture();if(io.claim()===false)return false;io.populate(vv);io.activate();return true;})(__bindIO);
      if(!__bindOk)return -1;
      __instrumentDisplayScalar(o,'_alpha',oALPHA,false); __instrumentDisplayScalar(o,'_visible',oVIS,true); __instrumentDisplayScalar(o,'_renderable',oREND,true);
      __instrumentRenderField(o,'filters',null); __instrumentRenderField(o,'blendMode',0); __instrumentRenderField(o,'_cacheAsBitmap',false);
      if(o.isSprite&&o._texture)__addTextureOwner(o._texture,o);   // 兜住 hook 安装前已构造、之后才挂树的 Sprite
      if(o._anchor && ((PIXI.BitmapText&&o instanceof PIXI.BitmapText)||(PIXI.TilingSprite&&o instanceof PIXI.TilingSprite))) __instrumentPoint(o._anchor,undefined,o);
      if(PIXI.TilingSprite&&o instanceof PIXI.TilingSprite){ var tt=o.tileTransform; if(tt){ __instrumentPoint(tt.position,undefined,o);__instrumentPoint(tt.scale,undefined,o);__instrumentPoint(tt.pivot,undefined,o);__instrumentPoint(tt.skew,undefined,o); } }
      if(PIXI.Mesh&&o instanceof PIXI.Mesh)__watchMeshBuffers(o);
      if(PIXI.Text&&o instanceof PIXI.Text)__watchTextStyle(o);
      var __a0=(o._alpha===undefined?o.alpha:o._alpha); SGB[b+oALPHA]=isFinite(__a0)?__a0:1; SGB[b+oVIS]=((o._visible===undefined?o.visible:o._visible)?1:0);
      // Spine geometry is write-driven by AnimationState.apply and explicit Skeleton mutation
      // methods.  The former dynamic-render set rebuilt dozens of unchanged/hidden skeletons on
      // each present; render work is now driven only by explicit pose/visual invalidation.
      __writeHitMeta(o); __markRender(o);
      // FGUI package construction owns an atomic native batch. Keep the node as a container until
      // present-time dirty flushing, instead of crossing into Text/Graphics/NineSlice per child.
      if(!(globalThis.__sgBatchActive&&globalThis.__sgBatchActive()))__wrType(o);
      return i;
    };
    // 显式诊断审计(__SG_AUDIT=1):全量比对 JS local 与共享槽。生产路径的实例字段访问器已经覆盖
    // ObservablePoint._x/_y、Transform._rotation 等直接写,因此 present 不再调用这次全对象扫描。
    var __syncLocals = function(){
      var n=0, objs=__sgObjs;
      for(var i=1;i<objs.length;i++){
        var o=objs[i]; if(!o || o.__i!==i || o._destroyed) continue;
        var t=o.transform, b=i*ST, ch=0;
        if(t){
          var p=t.position, s=t.scale, pv=t.pivot, sk=t.skew;
          if(p && (SGB[b+oX]!==p._x || SGB[b+oY]!==p._y)){ SGB[b+oX]=p._x; SGB[b+oY]=p._y; ch=1; }
          if(s && (SGB[b+oSX]!==s._x || SGB[b+oSY]!==s._y)){ SGB[b+oSX]=s._x; SGB[b+oSY]=s._y; ch=1; }
          if(pv && (SGB[b+oPVX]!==pv._x || SGB[b+6]!==pv._y)){ SGB[b+oPVX]=pv._x; SGB[b+6]=pv._y; ch=1; }
          if(sk && (SGB[b+oSKX]!==sk._x || SGB[b+8]!==sk._y)){ SGB[b+oSKX]=sk._x; SGB[b+8]=sk._y; ch=1; }
          if(SGB[b+oROT]!==t._rotation){ SGB[b+oROT]=t._rotation; ch=1; }
        }
        var al=(o._alpha===undefined?1:o._alpha), vi=((o._visible===undefined?true:o._visible)?1:0);
        if(SGB[b+oALPHA]!==al){ SGB[b+oALPHA]=al; ch=1; }
        if(SGB[b+oVIS]!==vi){ SGB[b+oVIS]=vi; ch=1; }
        if(ch){ __markD(b); globalThis.__sgDirty=1; n++; }
      }
      if(globalThis.__SG_DIAG===1 && n && (globalThis.__syncLC||0)<12){ globalThis.__syncLC=(globalThis.__syncLC||0)+1; __log('[sg-sync] local补写 '+n+' 节点'); }
      return n;
    };
    globalThis.__sgSyncLocals = __syncLocals;
    var __renderBootstrapped=0;
    var __flushRenderData=function(root,synchronousRoot){
      __qActiveRoot=root||__qActiveRoot;
      if(__qActiveRoot&&__qTop>QCAP*0.75&&((++__qSweepTick)%60===0))__qReclaimDetached(__qActiveRoot);
      if(!__renderBootstrapped){
        __renderBootstrapped=1;
        for(var bi=1;bi<__sgObjs.length;bi++){ var bo=__sgObjs[bi]; if(bo&&bo.__i===bi&&!bo._destroyed)__renderDirty.add(bo); }
      }
      var work=__renderDirty; __renderDirty=new Set();
      // iOS scene/popup transitions can dirty hundreds of freshly-created Text/Graphics nodes in
      // one tick. Rasterising all of them synchronously causes 160-700ms main-thread stalls even
      // though steady-state rendering is 60fps. The host supplies a per-frame creation budget;
      // unfinished nodes remain dirty and are completed over the following frames.
      var n=0, budgeted=0, configured=(globalThis.__nativeRenderWorkBudget|0);
      var budget=configured>0?(globalThis.__nativeRenderBudgetRemaining|0):0x7fffffff;
      if(configured>0&&budget<0)budget=0;
      work.forEach(function(o){
        if(!o||!(o.__i>0)||o._destroyed)return;
        // A requested RenderTexture must contain its complete subtree now.
        // Presentation quotas may defer unrelated screen work, never snapshot
        // contents; otherwise caching a newly created node can freeze it blank.
        var required=synchronousRoot&&__linkedToRoot(o,synchronousRoot);
        if(!required&&budgeted>=budget){__renderDirty.add(o);return;}
        // FGUI updates reusable list rows while they are detached from the stage. Multi-quad nodes
        // that already own storage can be rewritten immediately. Keep every other dirty node pending
        // until its subtree is attached again; dropping it leaves stale Text/Sprite textures such as
        // a package-authored "PLAY" title until a later hover happens to invalidate the node.
        if(!__linkedToRoot(o,__qActiveRoot)&&!(o.__qCap>0)){__renderDetachedDirty.add(o);return;}
        __renderDetachedDirty.delete(o);
        try{o.__sgWriting=1;__wrType(o);n++;if(!required)budgeted++;}catch(e){if(o.__nativePixi8)throw e;if(globalThis.__SG_DIAG===1&&(globalThis.__wrErrN||0)<8){globalThis.__wrErrN=(globalThis.__wrErrN||0)+1;__log('[render-sync] '+e.message);}}
        finally{o.__sgWriting=0;}
      });
      if(configured>0)globalThis.__nativeRenderBudgetRemaining=Math.max(0,budget-budgeted);
      globalThis.__sgRenderUpdated=n;
      return n;
    };
    globalThis.__sgFlushRenderData=__flushRenderData;
    // (5) 树同步:addChild / addChildAt / removeChild / removeChildren / destroy
    //   removeChild 只 __sgRemove(脱离父,对象可能被重新挂到别处,节点保留);destroy 才 __sgFree(释放 slot,防泄漏)。
    var _ac=COp.addChild, _aca=COp.addChildAt, _rc=COp.removeChild, _rca=COp.removeChildAt, _rcn=COp.removeChildren, _dstr=DOp.destroy;
    var __rebindTree = function(o){
      __ovChk(o); __bind(o); __wrType(o);
      var ch=o.children; if(!ch) return;
      for (var i=0;i<ch.length;i++){ var c=ch[i]; if(!c) continue;
        if (c.__i===undefined) __rebindTree(c);
        if (c.__i>0 && o.__i>0) __sgSetParent(c.__i, o.__i, -1);
      }
    };
    // A stale facade or an allocation retry can leave a linked object temporarily unbound. Some
    // FGUI restore paths only toggle visibility and do not call Pixi addChild again, so repair a
    // live object from the highest unbound ancestor below the stage and keep the subtree coherent.
    var __repairLinkedNode = function(o, root){
      if(!o || o.__i>0 || !o.parent || !__linkedToRoot(o,root)) return false;
      var top=o, parent=top.parent;
      while(parent && parent!==root && !(parent.__i>0)){ top=parent; parent=top.parent; }
      __activateDetachedRenderTree(top,parent,false);
      if(top.__i>0){
        globalThis.__sgLinkedRepairCount=(globalThis.__sgLinkedRepairCount||0)+1;
        __log('[sg-heal] rebound linked subtree '+((o.name||o.constructor&&o.constructor.name)||'?')+' root='+top.__i+' node='+(o.__i||0));
      }
      return o.__i>0;
    };
    // Container.addChild can be cached by framework code before our wrapper is installed. Pixi still
    // emits childAdded(child,parent,index) for those calls. Unlike onChildrenChange, this event does
    // not fire for remove/reorder, so syncing here cannot accidentally move an existing branch in z.
    var _cemit=COp.emit;
    if(_cemit&&!COp.__sgChildAddedHook){
      COp.__sgChildAddedHook=1;
      COp.emit=function(event){
        if(event==='childAdded')try{
          var child=arguments[1],idx=arguments[3]|0,ch=this.children;
          if(child&&child.parent===this&&!child._destroyed){
            var top=this,up=top.parent;
            while(up&&!(up.__i>0)){top=up;up=top.parent;}
            if(top!==this&&up&&up.__i>0){
              if(top.__i===undefined)__rebindTree(top);
              if(top.__i>0){var ts=up.children||[],ti=ts.indexOf(top),tb=ti>=0&&ts[ti+1];__sgSetParent(top.__i,up.__i,(tb&&tb.__i>0)?tb.__i:-1);}
            }else{
              var parentWasMissing=this.__i===undefined;
              if(parentWasMissing)__bind(this);
              if(parentWasMissing&&this.__i>0&&this.parent&&this.parent.__i>0){var ps=this.parent.children||[],pi=ps.indexOf(this),pb=pi>=0&&ps[pi+1];__sgSetParent(this.__i,this.parent.__i,(pb&&pb.__i>0)?pb.__i:-1);}
            }
            if(child.__i===undefined)__rebindTree(child);
            if(child.__i>0&&this.__i>0){var before=ch&&ch[idx+1];__sgSetParent(child.__i,this.__i,(before&&before.__i>0)?before.__i:-1);}
            if(__linkedToRoot(child,__qActiveRoot))__activateDetachedRenderTree(child);
          }
        }catch(e){}
        return _cemit.apply(this,arguments);
      };
    }
    var __reactivateQuadTree=function(o){
      if(!o)return;
      if(o.__qReleased){o.__qReleased=0;__markRender(o);}
      var ch=o.children;if(ch)for(var i=0;i<ch.length;i++)__reactivateQuadTree(ch[i]);
      o.__qReleasedTree=0;
    };
    var __activateDetachedRenderTree=function(o,parent,forceParent){
      if(!o||o._destroyed)return 0;
      // Detached scene caches may keep their Pixi objects/native ids while the quad allocator has
      // reclaimed their geometry. Every reattach path (including framework-cached addChild) reaches
      // this function, so restore released blocks and Spine dynamic registration here once for the
      // whole subtree instead of relying on a particular Container wrapper. FGUI can also reuse a
      // bound component root while some descendants were reclaimed. Rebind those holes during this
      // attach event and insert them before the next live JS sibling so native z-order stays exact.
      var missing=!(o.__i>0), repaired=0;
      if(missing){__ovChk(o);__bind(o);__wrType(o);repaired=o.__i>0?1:0;}
      if(o.__i>0&&parent&&parent.__i>0&&(missing||forceParent))
        __sgSetParent(o.__i,parent.__i,forceParent?-1:__nativeBeforeSibling(o));
      if(o.__qReleasedTree)__reactivateQuadTree(o);
      // 逐节点复活几何,不依赖"树顶标记"。__qRelease(o,true) 把 __qReleasedTree 记在**当时的
      // 最顶层祖先**上,而对象池里的复用单元往往不是那个祖先(池容器自己从不重新入树),标记
      // 于是永远等不到消费:节点还在、变换也对,就是没有 quad——表现为"复用后内容凭空消失"。
      // 又因为业务重放时常带"值没变就跳过"语义(同一分数的 BitmapText.text= 直接早退,不触发
      // 重新发布),这一帧丢了就永久丢。这里在每个被复活的节点上自己判,漏不掉。
      if(o.__qReleased){o.__qReleased=0;__markRender(o);}
      __ovChk(o);
      if(__renderDetachedDirty.delete(o))__renderDirty.add(o);
      var ch=o.children;if(ch)for(var i=0;i<ch.length;i++)repaired+=__activateDetachedRenderTree(ch[i],o,missing);
      return repaired;
    };
    // Diagnostic fallback for checking a live stage after scene transitions. Production recovery is
    // event-driven in __activateDetachedRenderTree; this helper is only called explicitly by tests or
    // diagnostics and never performs a periodic stage scan.
    var __repairLinkedTreeHoles=function(root){
      if(!root || !(root.__i>0))return 0;
      var stack=[root],repaired=0,visited=0;
      while(stack.length){
        var parent=stack.pop(),ch=parent&&parent.children;
        if(!ch)continue;
        for(var i=0;i<ch.length;i++){
          var child=ch[i];if(!child||child._destroyed)continue;
          visited++;
          if(!(child.__i>0)){
            if(__activateDetachedRenderTree(child,parent,false)>0)repaired++;
          }
          if(child.children&&child.children.length)stack.push(child);
        }
      }
      if(repaired){
        globalThis.__sgTreeHoleRepairCount=(globalThis.__sgTreeHoleRepairCount||0)+repaired;
        __log('[sg-heal] repaired '+repaired+' linked tree holes, visited='+visited);
      }
      return repaired;
    };
    globalThis.__sgRepairLinkedTreeHoles=__repairLinkedTreeHoles;
