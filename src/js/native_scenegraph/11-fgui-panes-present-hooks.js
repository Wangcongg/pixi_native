    // FGUI finalises GList/ScrollPane bounds lazily after async list items are inserted. The normal
    // Pixi render path reaches that refresh before painting; native-only presentation bypasses it,
    // so the first frame can retain provisional item positions until the user drags the pane.
    // Stabilise only panes whose content/child signature changed. This is event-like (normally once
    // per population), not a per-frame forced layout, and keeps the H5/game bundle untouched.
    var __fguiPaneOwners=new Set(),__fguiPaneDirty=new Set(),__fguiPaneBootRoot=null;
    var __registerFguiPaneOne=function(d){var u=d&&d.UIOwner;if(u&&u.scrollPane){__fguiPaneOwners.add(d);__fguiPaneDirty.add(d);u.scrollPane.__nativeLayoutSettle=Math.max(4,u.scrollPane.__nativeLayoutSettle|0);}};
    var __registerFguiPaneSubtree=function(root){
      if(!root)return;if(root.__i>0&&typeof globalThis.__sgNativeSubtree==='function'){
        var ids=__sgNativeSubtree(root.__i);for(var i=0;i<ids.length;i++)__registerFguiPaneOne(__sgObjs[ids[i]|0]);globalThis.__fguiNativePaneScans=(globalThis.__fguiNativePaneScans||0)+1;return;
      }
      // Compatibility only for hosts that have not exposed native hierarchy yet.
      var stack=[root];while(stack.length){var d=stack.pop(),ch=d&&d.children;if(ch)for(var ci=0;ci<ch.length;ci++)if(ch[ci])stack.push(ch[ci]);__registerFguiPaneOne(d);}
    };
    // Every child has its own add/bind event while a subtree is assembled, and native FGUI lists
    // explicitly register after setupScroll. Structural hot paths therefore only test that one
    // object; they never issue one hierarchy crossing per attached leaf.
    var __registerFguiPanes=function(root){__registerFguiPaneOne(root);};
    var __registerFguiPaneChain=function(d){
      if(d&&d.__i>0){for(var id=d.__i|0,g=0;id>0&&g++<32768;id=__nativeParentOf(id))__registerFguiPaneOne(__sgObjs[id]);return;}
      for(var p=d,cg=0;p&&cg++<128;p=p.parent)__registerFguiPaneOne(p);
    };
    var __stabilizeFguiScrollPanes=function(root){
      if(!root)return 0;if(__fguiPaneBootRoot!==root){__fguiPaneBootRoot=root;__registerFguiPaneSubtree(root);}
      if(!__fguiPaneDirty.size){globalThis.__fguiPaneDirtyCount=0;return 0;}var work=Array.from(__fguiPaneDirty),fixed=0,stale=[];__fguiPaneDirty.clear();
      work.forEach(function(d){
        var u=d&&d.UIOwner,sp=u&&u.scrollPane;if(!sp||!u.ensureBoundsCorrect){stale.push(d);return;}
        var content=u.container||u.$container,cc=content&&content.__i>0&&typeof globalThis.__sgNativeChildCount==='function'?(__sgNativeChildCount(content.__i)|0):((content&&content.children&&content.children.length)||0);
        var cs=sp.$contentSize,vs=sp.$viewSize;
        var sig=cc+'|'+(cs?cs.x:0)+'|'+(cs?cs.y:0)+'|'+(vs?vs.x:0)+'|'+(vs?vs.y:0);
        var sigChanged=sp.__nativeLayoutSig!==sig;
        if(sigChanged){sp.__nativeLayoutSig=sig;sp.__nativeLayoutSettle=4;}
        else if((sp.__nativeLayoutSettle|0)<=0)return;
        try{
          if(sigChanged){
            u.ensureBoundsCorrect();
            // refresh() 的几何重算与虚拟 GList 的可见 cell 摆放必须发生(零初始位不会自己
            // posChanged),但它同时无条件 emit ScrollEvent.SCROLL——这条稳定链是 native 私有,
            // H5 初始化没有对应 refresh,业务监听者(如 PopupChallenge 的 listScroll)收到
            // posX=0 假滚动会在其数据就位前驱动状态机(topBtnIndex=list_index+1 恒≥1,配合
            // text 缓存门把 CURRENT 牌可见性永久卡死:H5 隐藏/native 显示)。因此抑制对外
            // emit(guard 装于 native_fgui 的 ScrollPane.emit 包装),再手动喂 GList 内部重排
            // ($scrolled→handleScroll,≙ H5 setVirtual 链的内部直调,不经业务事件面)。
            if(typeof sp.refresh==='function'){
              sp.__suppressScrollEmit=true;
              try{sp.refresh();}finally{sp.__suppressScrollEmit=false;}
              if(typeof u.$scrolled==='function')try{u.$scrolled();}catch(_se){}
            }
          }
          var paneBox=sp.$container,paneId=paneBox&&paneBox.__i|0,pn=paneId>0&&typeof globalThis.__sgNativeChildCount==='function'?(__sgNativeChildCount(paneId)|0):((paneBox&&paneBox.children&&paneBox.children.length)||0);
          for(var pi=0;pi<pn;pi++){
            var item=paneId>0&&typeof globalThis.__sgNativeChildAt==='function'?__sgObjs[__sgNativeChildAt(paneId,pi)|0]:paneBox.children[pi];
            if(!item)continue;
            __refreshTransformBinding(item);
            // Native hierarchy already owns the authoritative parent/order; no JS child-list
            // reparent loop is needed during the settling frames.
          }
          sp.__nativeLayoutSettle=Math.max(0,(sp.__nativeLayoutSettle|0)-1);
          if(sp.__nativeLayoutSettle>0)__fguiPaneDirty.add(d);
          fixed++;
        }catch(e){}
      });
      for(var si=0;si<stale.length;si++){__fguiPaneOwners.delete(stale[si]);__fguiPaneDirty.delete(stale[si]);}
      globalThis.__fguiPaneDirtyCount=__fguiPaneDirty.size;
      return fixed;
    };
    // Facade generations and lazy ScrollPane layout form two mutually-producing domains:
    // controller sync can change pane sizes, while stabilisation/itemRenderer can write another
    // controller.  Prove a fixed point before native samples the tree.  A positive post-stabilise
    // flush therefore requires another stabilise+flush round even when that flush reports itself
    // fully drained: its final size write has not yet been consumed by the pane domain.
    var __convergeFguiBeforePresent=function(root){
      var fail=function(stage,error){
        var reason='present-'+stage;
        try{if(typeof globalThis.__nativeFguiDisableFrameSyncCoalesce==='function')globalThis.__nativeFguiDisableFrameSyncCoalesce(reason);}catch(_disableError){}
        var message='FGUI present convergence failed at '+stage;
        if(!globalThis.__fguiPresentConvergeErr){globalThis.__fguiPresentConvergeErr=1;try{__log('[fgui-present-barrier] '+message+(error&&error.message?' '+error.message:''));}catch(_logError){}}
        throw error&&typeof error.message==='string'?error:new Error(message);
      };
      var flush=function(stage){
        if(typeof globalThis.__nativeFguiFrameFlush!=='function')return 0;
        var result;
        try{result=globalThis.__nativeFguiFrameFlush();}catch(error){return fail(stage,error);}
        // undefined is the only legacy success shape: old hosts flushed synchronously and did not
        // expose a generation count. Every other non-count leaves convergence unknowable.
        if(result===undefined)return 0;
        if(result===false||typeof result!=='number'||!isFinite(result)||result<0)return fail(stage,new Error('invalid flush result '+String(result)));
        return result;
      };

      // render() presents synchronously below. Close the FGUI facade transaction before
      // scroll-pane stabilisation and canonical compute so controller-driven sizes,
      // masks and hit areas belong to this presentation. The tick-tail flush remains a
      // backstop for writes produced by postrender/late host work.
      // SceneGraph/SPX animation advancement runs after the earlier transition-frame drain and
      // may publish one more controller/property generation. Drain that tail before the facade
      // fixed point so FrameSeal cannot observe journal N with facade snapshot N-1.
      if(typeof globalThis.__nativeFguiTransitionVisibilityFrame==='function'){
        try{globalThis.__nativeFguiTransitionVisibilityFrame();}catch(error){return fail('shadow-drain',error);}
      }
      flush('pre-stabilize');
      for(var round=1;round<=4;round++){
        try{__stabilizeFguiScrollPanes(root);}catch(error){return fail('stabilize-'+round,error);}
        // Stabilisation is not read-only: a virtual GList may run itemRenderer/$scrolled,
        // and that business callback can change one or more FGUI controllers. Close that
        // second generation before canonical scenegraph compute/present as well. Without
        // this barrier the late controller write remains dirty until frame N+1, producing
        // a one-frame mixed page even though the earlier facade flush succeeded.
        var processed=flush('post-stabilize-'+round);
        if(processed===0)return round;
      }
      return fail('not-converged',new Error('facade/pane feedback exceeded 4 rounds'));
    };
    globalThis.__nativeFguiConvergeBeforePresent=__convergeFguiBeforePresent;
    // addChild/reparent 只做同帧激活准备。渲染提交会在 flush 后检查结构代次和 local 脏队列，
    // 若 compute 之后又发生了结构/坐标变化，就在当前 present 前立即补算。
    // (孤儿自愈 __SGHEAL/__sgHealCheck/__chkList 死路线已删:两次误伤定性——fgui 虚拟列表高频
    var __prepareAttachedNode = function(c){
      if(!c)return;
      __refreshTransformBinding(c);
      c.__ovInit=0;
      if(c.transform)c.transform._parentID=-1;
      if(c.__i>0){__markD(c.__i*ST);__markRender(c);}
    };
    COp.addChild=function(){ var __oldParents=[];for(var j=0;j<arguments.length;j++)__oldParents[j]=arguments[j]&&arguments[j].parent; var r=_ac.apply(this,arguments); __bind(this);__markOrderDirty(this);__registerFguiPaneChain(this); for(var k=0;k<arguments.length;k++){ var c=arguments[k]; if(c){ __dogDiagAdded(c,this); if(__oldParents[k]!==this)__dogRootDiagArm(c,'reparent '+((__oldParents[k]&&__oldParents[k].name)||'-')+'->'+(this.name||'-')); if(c.__qReleasedTree)__reactivateQuadTree(c);
      if(c.__i===undefined && c.children && c.children.length) __rebindTree(c);   // GC 失绑体重挂:递归重绑子树
      else { __ovChk(c); __bind(c); if(!(globalThis.__sgBatchActive&&globalThis.__sgBatchActive()))__wrType(c); }
      if(c.__i>0&&this.__i>0)__sgSetParent(c.__i,this.__i,-1); __registerFguiPanes(c);if(__linkedToRoot(c,__qActiveRoot))__activateDetachedRenderTree(c); __prepareAttachedNode(c); } } return r; };
    COp.addChildAt=function(child,index){ var __oldParent=child&&child.parent; var r=_aca.call(this,child,index); __bind(this);__markOrderDirty(this);__registerFguiPaneChain(this); if(child){ __dogDiagAdded(child,this); if(__oldParent!==this)__dogRootDiagArm(child,'reparent '+((__oldParent&&__oldParent.name)||'-')+'->'+(this.name||'-')); if(child.__qReleasedTree)__reactivateQuadTree(child);
      if(child.__i===undefined && child.children && child.children.length) __rebindTree(child);
      else { __ovChk(child); __bind(child); if(!(globalThis.__sgBatchActive&&globalThis.__sgBatchActive()))__wrType(child); }
      var before=this.children[index+1]; if(child.__i>0&&this.__i>0)__sgSetParent(child.__i,this.__i,(before&&before.__i>0)?before.__i:-1); __registerFguiPanes(child);if(__linkedToRoot(child,__qActiveRoot))__activateDetachedRenderTree(child); __prepareAttachedNode(child); } return r; };
    COp.removeChild=function(){ __markOrderDirty(this);__registerFguiPaneChain(this);for(var k=0;k<arguments.length;k++){ var c=arguments[k]; if(c){
      // FGUI sometimes removes its GObject from the old UI parent *after* its displayObject has
      // already been reparented to a guide/highlight layer. Pixi treats that stale remove as a
      // no-op, so native must do the same. Unconditionally calling __sgRemove here detached the
      // highlighted object from the native tree even though its live JS parent was guide_layer.
      var owned=(c.parent===this)||(this.children&&this.children.indexOf(c)>=0);
      if(owned){ if(c.__i>0)__sgRemove(c.__i); c.__ovInit=0; __ov.delete(c); }
    } } return _rc.apply(this,arguments); };
    COp.removeChildAt=function(index){ __markOrderDirty(this);__registerFguiPaneChain(this);var c=this.children[index]; if(c){ if(c.__i>0)__sgRemove(c.__i); c.__ovInit=0; __ov.delete(c); } return _rca.call(this,index); };
    if (_rcn) COp.removeChildren=function(beginIndex, endIndex){ __markOrderDirty(this);__registerFguiPaneChain(this);var arr=this.children.slice(beginIndex||0, (endIndex===undefined?this.children.length:endIndex)); for(var k=0;k<arr.length;k++){ var c=arr[k]; if(c){ if(c.__i>0)__sgRemove(c.__i); c.__ovInit=0; __ov.delete(c); } } return _rcn.apply(this,arguments); };
    // 通用生命周期契约：节点销毁时终止仍以它为目标的 gsap tween。业务 destroy 不彻底（长
    // tween 的特效活过宿主销毁）在浏览器上只是"看不见地空跑到自然结束"；native 下 tween 若
    // 继续驱动一个已把槽位还给 freeList 的对象，后续写入可能落进复用该槽位的新节点（残渣/
    // 错位）。在两条销毁路径的汇合点统一收口，不识别任何业务对象。_gsap 是 gsap3 在目标上
    // 贴的缓存标记——没被 tween 过的对象零成本跳过。
    var __sgKillTweens=function(o){
      if(!o||!o._gsap)return;
      var g=globalThis.gsap;
      if(g&&typeof g.killTweensOf==='function'){try{g.killTweensOf(o);}catch(e){}}
    };
    // destroy:释放 native 节点(防 NODE_CAP 泄漏)。子节点由 pixi 递归 destroy 各自触发。
    DOp.destroy=function(){ __sgKillTweens(this); __releaseOwnedMasks(this,null);this.__sgMaskObjs=[];this.__sgMaskObj=null; if(this.isSprite&&this._texture)__removeTextureOwner(this._texture,this); __releaseOwnedBaseTextures(this); if(this.__nativeProxyChildren&&this.__nativeProxyChildren.length){var _pc=this.__nativeProxyChildren.slice();this.__nativeProxyChildren.length=0;for(var _pi=0;_pi<_pc.length;_pi++)if(_pc[_pi]&&_pc[_pi].destroy)try{_pc[_pi].destroy({children:true});}catch(e){}}
      __orderDirty.delete(this);__fguiPaneOwners.delete(this);__fguiPaneDirty.delete(this);if(this.__i>0){ var __di=this.__i|0,__owns=__sgObjs[__di]===this; __renderDirty.delete(this); __renderDetachedDirty.delete(this); if(this.__sgMeshBuffers)for(var _mb=0;_mb<this.__sgMeshBuffers.length;_mb++){var _bf=this.__sgMeshBuffers[_mb];if(_bf&&_bf.__sgOwners)_bf.__sgOwners.delete(this);} if(this.__sgTextStyle&&this.__sgTextStyle.__sgOwners)this.__sgTextStyle.__sgOwners.delete(this); if(this.__sgFilters)for(var _fi=0;_fi<this.__sgFilters.length;_fi++){var _ff=this.__sgFilters[_fi];if(_ff&&_ff.__sgOwners)_ff.__sgOwners.delete(this);if(_ff&&_ff.__sgFilterOwners)_ff.__sgFilterOwners.delete(this);if(_ff&&_ff.__sgFilterTargets)_ff.__sgFilterTargets.forEach(function(_ft){if(_ft&&_ft.__sgFilterOwners)_ft.__sgFilterOwners.delete(this);},this);} if(__owns){__sgReleaseSlotLease(this,true);__qRelease(this);__sgObjs[__di]=undefined;try{__sgFree(__di);}catch(e){}}else{__sgReleaseSlotLease(this,false);__sgDropStaleSlotWrite(this,__di*ST,'destroy/free');} this.__i=undefined; }
      // P1a/b:清 native 骨骼变换注册(gSkel.regs + 每骨架数组),防切场景累积泄漏
      __ov.delete(this); return _dstr.apply(this,arguments); };
    // NativeNodeProxy does not inherit DisplayObject, so its destroy path cannot pass through the
    // hook above.  Release the same Spine registries/ticker-side native state before the numeric
    // node id is returned to SceneGraph's free list; otherwise a destroyed Spine root remains
    // strongly retained by __ov and a reused id can inherit stale skin/skeleton registrations.
    globalThis.__nativeSceneDispose=function(o){
      if(!o)return;var id=o.__i|0;
      __sgKillTweens(o);
      __orderDirty.delete(o);__fguiPaneOwners.delete(o);__fguiPaneDirty.delete(o);__renderDirty.delete(o);__renderDetachedDirty.delete(o);
      if(id>0&&__sgObjs[id]===o){try{__qRelease(o);}catch(e){} }
      __ov.delete(o);
    };
    // conch P1 变化钩子:纹理/anchor/tint/blendMode(Sprite)、mask/cacheAsBitmap(DisplayObject)→ 重写 slot
    var __wrTexSafe = function(o){ try{
      __wrTex(o);
      // RenderTexture 每次离屏更新都会走 Sprite._onTextureUpdate。mask 的纹理/UV 要更新给
      // stencil 使用，但它本身绝不能因此重新成为普通 Sprite 参与主树绘制。
      if(o&&(o.__maskOf||o.isMask)&&o.__i>0){var mb=o.__i*ST;__sgTypeW(mb,0);SGB[mb+oQUADCNT]=0;}
      __markRender(o);
    }catch(e){ if(o&&o.__i>0)SGB[o.__i*ST+oTEXID]=0; } };   // hook 绝不允许打断 pixi 内部流程(raf 链会整体死)
    var SPp = PIXI.Sprite && PIXI.Sprite.prototype;
    if (SPp){
      var TXp=PIXI.Texture&&PIXI.Texture.prototype;
      if(TXp&&TXp.updateUvs&&!TXp.__sgUvOwnerHook){
        TXp.__sgUvOwnerHook=1; var _tuv=TXp.updateUvs;
        TXp.updateUvs=function(){ var r=_tuv.apply(this,arguments), owners=this.__sgSpriteOwners;
          if(owners&&owners.size)owners.forEach(__wrTexSafe); return r; };
      }
      var _otu = SPp._onTextureUpdate; SPp._onTextureUpdate = function(){ var r=_otu.apply(this,arguments); __wrTexSafe(this); return r; };
      var _oau = SPp._onAnchorUpdate; if(_oau) SPp._onAnchorUpdate = function(){ var r=_oau.apply(this,arguments); __wrTexSafe(this); return r; };
      var _txd = Object.getOwnPropertyDescriptor(SPp,'texture');
      if (_txd&&_txd.set&&_txd.configurable) Object.defineProperty(SPp,'texture',{configurable:true,get:_txd.get,set:function(v){ var old=this._texture; _txd.set.call(this,v); var cur=this._texture; if(old!==cur)__replaceTextureOwner(old,cur,this); __wrTexSafe(this); }});
      var _tnd = Object.getOwnPropertyDescriptor(SPp,'tint');
      // tint 烘进 C++ 顶点段缓存的顶点色:直写 slot 后必须走 __markBoundsNode(即顶点 gen 失效通道),
      // 否则缓存命中会继续用旧 tint 的顶点流(bounds 本身不受 tint 影响,多标一次代价近零)。
      if (_tnd&&_tnd.set&&_tnd.configurable) Object.defineProperty(SPp,'tint',{configurable:true,get:_tnd.get,set:function(v){ _tnd.set.call(this,v); if(this.__i>0){SGB[this.__i*ST+oTINT]=__tintOf(this);__markBoundsNode(this.__i);} }});
    }
    // Pixi defines cacheAsBitmap with configurable:false on some builds. Prefer a real setter hook so
    // enabling/disabling the flag invalidates our native bake; fall back to dirtying via _cacheAsBitmap.
    var __safeHook = function(proto, name){
      var d = Object.getOwnPropertyDescriptor(proto, name);
      if (!d || !d.set || !d.configurable) return false;
      Object.defineProperty(proto, name, {configurable:true, get:d.get, set:function(v){
        var prev=d.get?d.get.call(this):this['_'+name];
        d.set.call(this,v);
        if(name==='cacheAsBitmap'){
          var data=this._cacheData;
          if(data&&!data.__nativeBakeBusy){
            data.__nativeBakeDirty=1;data.__nativeBaked=0;
            if(!v&&data.__nativeRT){try{data.__nativeRT.destroy(true);}catch(e){}data.__nativeRT=null;data.sprite=null;}
          }
        }
        __markRender(this); __wrType(this);
      }});
      return true;
    };
    __safeHook(DOp, 'mask');
    if(!__safeHook(DOp, 'cacheAsBitmap')){
      // Configurable:false — instrument the internal flag used by Pixi's own defineProperty.
      try{
        var _cabTag='__sgCacheAsBitmapFlag';
        Object.defineProperty(DOp,'_cacheAsBitmap',{configurable:true,enumerable:false,
          get:function(){return this[_cabTag]===true;},
          set:function(v){
            var next=!!v,prev=this[_cabTag]===true;this[_cabTag]=next;
            if(prev!==next){
              var data=this._cacheData;
              if(data&&!data.__nativeBakeBusy){
                data.__nativeBakeDirty=1;data.__nativeBaked=0;
                if(!next&&data.__nativeRT){try{data.__nativeRT.destroy(true);}catch(e){}data.__nativeRT=null;data.sprite=null;}
              }
              __markRender(this);__wrType(this);
            }
          }});
      }catch(e){}
    }
    if (true) {   // native screen:mask rect 同步 + z 序重排
    // fgui 滚动时 rectMask 用 clear()+drawRect() 更新裁剪区 → 同步到 mask 节点 slot
    var GRp = PIXI.Graphics && PIXI.Graphics.prototype;
    if (GRp && GRp.drawRect){ var _dwr = GRp.drawRect;
      GRp.drawRect = function(x,y,w,h){ var r=_dwr.apply(this,arguments);
        if(this.__maskOf && this.__i>0){ var mb=this.__i*ST; SGB[mb+oRX]=x; SGB[mb+oRX+1]=y; SGB[mb+oRX+2]=w; SGB[mb+oRX+3]=h; __markRender(this.__maskOf); }
        return r; };
    }
    // z 序变化(fgui 常用 setChildIndex):重排后按 JS children 顺序重建 C++ 树的兄弟顺序
    var __resyncOrder = function(p){ if(!(globalThis.__NATIVE_SCREEN===1 || __SG_MODE===3)) return;
      if(!p || !(p.__i>0) || !p.children) return;
      for(var i=0;i<p.children.length;i++){ var c=p.children[i]; if(c && c.__i>0) __sgSetParent(c.__i, p.__i, -1); } };
    // Native 主屏跳过 Container.updateTransform()，所以用 bind/结构/zIndex 写钩子收集真正
    // 变脏的容器。首次 root 只做一次兼容扫描，稳定帧不再遍历 JS parent/children 树。
    var __flushSortableOrder=function(root){
      if(!root)return 0;
      // Every node is instrumented at its O(1) bind boundary.  The former first-present DFS only
      // rediscovered the same containers and rebuilt a JS traversal spike during cold startup.
      __instrumentSortDirty(root);if(root.sortDirty||root.__nativeOrderDirty)__orderDirty.add(root);
      var work=Array.from(__orderDirty);__orderDirty.clear();var n=0;
      for(var wi=0;wi<work.length;wi++){
        var p=work[wi],ch=p&&p.children;if(!p||p._destroyed||!ch||!__linkedToRoot(p,root))continue;
        if(p.sortableChildren&&p.sortDirty&&typeof p.sortChildren==='function'){p.sortChildren();ch=p.children;n++;}
        // Lightweight FGUI containers can receive several add/reparent operations while a package
        // is still being assembled. Reassert their final JS sibling order once, immediately before
        // the first native present, then stay completely idle until another structural mutation.
        if(p.__nativeContainerProxy&&p.__nativeOrderDirty&&typeof p._syncChildOrder==='function'){p._syncChildOrder();n++;}
      }
      return n;
    };
    globalThis.__sgFlushSortableOrder=__flushSortableOrder;
    ['setChildIndex','swapChildren','sortChildren'].forEach(function(mn){ var f=COp[mn];
      if(f) COp[mn]=function(){ var r=f.apply(this,arguments); __resyncOrder(this);if(mn!=='sortChildren')__markOrderDirty(this); return r; }; });
    // 性能计数 hook(阶段一 profiler):Graphics rebuild 在此挂(GraphicsGeometry 由 PIXI 导出);
    // mask/filter/rt 的系统类未导出到 PIXI 命名空间,改在 tick 里从 renderer 实例惰性挂(__pfSysHook)。
    try {
      var GGp = PIXI.GraphicsGeometry && PIXI.GraphicsGeometry.prototype;
      if (GGp && GGp.updateBatches){ var _gub = GGp.updateBatches;
        GGp.updateBatches = function(){ if(this.batchDirty!==this.dirty && globalThis.__pfCnt) globalThis.__pfCnt.gfx++; return _gub.apply(this, arguments); }; }
    } catch(e){ __log('[pf] 计数 hook 失败 '+e.message); }
    }
