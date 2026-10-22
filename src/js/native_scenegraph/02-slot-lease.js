    // ===== slot lease(影子消灭阶段0,docs/shadow-elimination-plan.md §3/§7) =====
    // 双态单源开关:1=绑定期 getter 读槽(阶段1行为);0=现状(读影子)。阶段0默认关。
    // Phase C 放行(2026-08-13):transform 单源化默认开——绑定期局部值以共享槽为唯一真值,
    // 影子只作失槽暂存。四门槛 Mac 侧全绿(atomic=0/债务=0/mismatch=0/bypass=0);回滚=置 0
    // (双写期影子仍每写镜像,回滚瞬间即一致,无需迁移)。fallback 计数=legacy shadow read/judge
    // 硬指标(心跳行 leaseFb 输出,终局恒 0=无失效租约读写回落)。
    if(globalThis.__SG_SINGLE_SOURCE!==0)globalThis.__SG_SINGLE_SOURCE=1;
    var __sgLease=globalThis.__nativeSlotLeaseContract||null;
    // capture(失槽方向):把槽的 transform 9 分量拷回暂存(影子)字段。仅开关开启时调用——
    // 关闭态读值来自影子,拷槽值(可能被 C++ tween 改过)会改变现状读值(放行门槛4)。
    // 只拷 transform:alpha/visible 槽可能是 spine 合成 effective 值(R8),影子才是业务真值。
    var __sgCaptureTransformToStash=function(o){
      if(!o||!(o.__i>0)||__sgObjs[o.__i]!==o)return;               // 只允许从"仍归我"的槽 capture
      var t=o.transform;if(!t)return;var b=o.__i*ST;
      var p=t.position,s=t.scale,pv=t.pivot,sk=t.skew;
      if(p&&p.__sgRawHook){p.__sgRawX=SGB[b+oX];p.__sgRawY=SGB[b+oY];}
      if(s&&s.__sgRawHook){s.__sgRawX=SGB[b+oSX];s.__sgRawY=SGB[b+oSY];}
      if(pv&&pv.__sgRawHook){pv.__sgRawX=SGB[b+oPVX];pv.__sgRawY=SGB[b+6];}
      if(sk&&sk.__sgRawHook){sk.__sgRawX=SGB[b+oSKX];sk.__sgRawY=SGB[b+8];}
      if(t.__sgRotHook)t.__sgRot=SGB[b+oROT];
    };
    // detach:撤销 transform 全部租约字段。开关关闭时也执行——现状 stale 写靠 __sgOwnsSlot
    // 拦截(不落槽),detach 后靠 __o=undefined 短路,读写行为等价(仅少了 stale 计数),
    // 且顺手消灭"旧壳悬挂 __o/__b"的脏状态(评审 B.2)。
    var __sgDetachTransformLease=function(o){
      if(!o)return;var t=o.transform;if(!t)return;
      if(t.__b!==undefined)t.__b=undefined;
      if(t.__sgOwner)t.__sgOwner=null;
      if(t.__sgRotHook)t.__sgRotLease=undefined;
      var pts=[t.position,t.scale,t.pivot,t.skew];
      // 字段语义=lease contract 的 detachFields(内联,不依赖模块加载)
      for(var i=0;i<4;i++){var p=pts[i];if(p&&p.__sgRawHook){p.__o=undefined;p.__sgOwner=null;p.__sgLease=undefined;}}
    };
    // 统一 release 入口(评审 P1:生产失槽必须与测试走同一 runRelease 编排):claim 顶替/destroy/
    // 所有权失配/runtime.free/adopt 全部经此。ownsSlot=false(失配/非登记所有者)绝不触碰槽,只
    // detach。调用时机约束:capture 需要 registry 仍登记该 owner,必须在 __sgObjs[id] 清空之前调。
    var __sgReleaseSlotLease=function(o,ownsSlot){
      if(!o)return;
      var io={captureFromSlot:function(){__sgCaptureTransformToStash(o);},detach:function(){__sgDetachTransformLease(o);}};
      if(__sgLease)__sgLease.runRelease(io,!!ownsSlot,globalThis.__SG_SINGLE_SOURCE);
      else{if(ownsSlot&&globalThis.__SG_SINGLE_SOURCE===1)io.captureFromSlot();io.detach();}
    };
    // activate:populate 完成后最后一步才把租约暴露给 getter(lease=owner 当前绑定代数)。
    // populate 之前 __sgLease 保持 undefined → 双态 getter 读暂存,不存在"读到未灌槽默认值"窗口。
    // 同时记录本次租约涉及的 transform/points,供 refresh 换绑时 revoke 被替换的旧对象(v2 §3.2)。
    var __sgActivateTransformLease=function(o){
      if(!o)return;var t=o.transform;if(!t)return;var g=o.__sgBindGeneration;
      var pts=[t.position,t.scale,t.pivot,t.skew];
      for(var i=0;i<4;i++){var p=pts[i];if(p&&p.__sgRawHook){p.__sgLease=g;}}
      if(t.__sgRotHook)t.__sgRotLease=g;
      o.__sgLeasedT=t;o.__sgLeasedPts=pts;
    };
    // revoke:FGUI 虚拟列表等会在 id 存续期间替换 Transform/point 对象。被替换的旧对象若不撤销
    // 租约,将永远持有旧偏移(现状悬挂问题)。对比上次 activate 记录,撤销一切不再是当前对象的租约。
    var __sgRevokeReplacedTransform=function(o){
      var t=o.transform,oldT=o.__sgLeasedT,oldPts=o.__sgLeasedPts;
      if(!oldPts)return;
      var cur=t?[t.position,t.scale,t.pivot,t.skew]:[null,null,null,null];
      for(var i=0;i<4;i++){var op=oldPts[i];if(op&&op!==cur[i]){op.__o=undefined;op.__sgOwner=null;op.__sgLease=undefined;}}
      if(oldT&&oldT!==t){oldT.__b=undefined;oldT.__sgRotLease=undefined;if(oldT.__sgOwner===o)oldT.__sgOwner=null;}
    };
    // Slot takeover is a semantic ownership transfer, not an array assignment. Invalidate the
    // displaced shell before publishing the new owner so its destroy path and retained callbacks
    // can neither mutate nor free the replacement node that now uses the same numeric id.
    var __sgClaimSlot=function(owner,id,where){
      id|=0;var displaced=__sgObjs[id];
      if(displaced&&displaced!==owner){
        // lease release(v2 §3.2,经 runRelease 编排):失去槽前先快照(仅单源开启且确为登记
        // 所有者),再撤销租约字段——被顶替的旧壳从此读写都落暂存,不再悬挂 __o/__b。
        __sgReleaseSlotLease(displaced,displaced.__i===id);
        if(displaced.__i===id)displaced.__i=undefined;
        if(typeof __sgKillTweens==='function')__sgKillTweens(displaced);
        globalThis.__sgSlotDisplacements=(globalThis.__sgSlotDisplacements||0)+1;
        if(globalThis.__sgSlotDisplacements<=4)__log('[sg-guard] 槽位所有权转移 id='+id+' from='+((displaced.name||(displaced.constructor&&displaced.constructor.name))||'?')+' to='+((owner&&(owner.name||(owner.constructor&&owner.constructor.name)))||'?')+(where?' @'+where:''));
      }
      // A numeric node id can be freed and handed back to the same semantic object in the very
      // next allocation.  Consumers which keep native-side registrations (notably NativeSpine
      // and its slot objects) must be able to distinguish that new lifetime from the old one;
      // comparing only `__i` makes the reuse look like an unchanged binding even though freeNode
      // already removed the C++ registration.  Advance a per-object lifetime token on every real
      // ownership claim.  Fast paths above do not call __sgClaimSlot for an already-owned node, so
      // ordinary addChild/order refreshes do not create false generations.
      if(owner){owner.__i=id;owner.__sgBindGeneration=((owner.__sgBindGeneration|0)+1)|0;}
      __sgObjs[id]=owner;return displaced;
    };
    // Native factories allocate and populate complete SceneGraph trees without passing through
    // the JavaScript allocator. A numeric id can therefore begin a new native lifetime while an
    // old, detached facade still occupies the JS registry entry for that id. Merely checking
    // `objects[id] === facade` cannot detect that generation change: the stale facade still looks
    // like the owner and a later render/hit refresh can overwrite the replacement descriptor.
    // Claim every id at the native publication boundary. The old shell keeps ordinary JS disposal
    // semantics, but loses its numeric capability immediately, so delayed callbacks cannot touch
    // the new node. Geometry is deliberately left untouched because C++ already published it.
    var __claimNativeSlots=function(ids,where){
      if(!ids)return 0;var claimed=0;
      for(var i=0;i<ids.length;i++){
        var id=ids[i]|0;if(!(id>0))continue;
        var displaced=__sgObjs[id];if(!displaced)continue;
        __sgClaimSlot(null,id,where||'native-tree');
        if(displaced.__parentId!==undefined)displaced.__parentId=-1;
        if(displaced.__parentFallback!==undefined)displaced.__parentFallback=null;
        claimed++;
      }
      return claimed;
    };
    // (1) ObservablePoint 四个改值入口 → 同步到 buffer(this.__o = 该点 x 在 buffer 的偏移;y=+1)
    //   __sgDirty:静止帧跳过的脏标志 —— 任何"值确实变化"的视觉输入都要置脏,漏一个=该变化在静止奇帧不刷新。
    var _wr = function(pt){ if (pt.__o!==undefined){
      if(__sgOwnsSlot(pt.__sgOwner,pt.__o)){if(SGB[pt.__o]!==pt._x || SGB[pt.__o+1]!==pt._y){ globalThis.__sgDirty=1; __markD(pt.__o - (pt.__o % ST)); }
        SGB[pt.__o]=pt._x; SGB[pt.__o+1]=pt._y;}else if(!pt.__sgRawHook)__sgDropStaleSlotWrite(pt.__sgOwner,pt.__o,'point'); }
      if(pt.__sgRO) __markRender(pt.__sgRO); };
    var _sx=Object.getOwnPropertyDescriptor(OPp,'x').set, _sy=Object.getOwnPropertyDescriptor(OPp,'y').set;
    Object.defineProperty(OPp,'x',{configurable:true,get:Object.getOwnPropertyDescriptor(OPp,'x').get,set:function(v){ var chg=this._x!==v; _sx.call(this,v); if(this.__o!==undefined&&__sgOwnsSlot(this.__sgOwner,this.__o)){ SGB[this.__o]=this._x; if(chg){ globalThis.__sgDirty=1; __markD(this.__o - (this.__o % ST)); } } if(chg&&this.__sgRO)__markRender(this.__sgRO); }});
    Object.defineProperty(OPp,'y',{configurable:true,get:Object.getOwnPropertyDescriptor(OPp,'y').get,set:function(v){ var chg=this._y!==v; _sy.call(this,v); if(this.__o!==undefined&&__sgOwnsSlot(this.__sgOwner,this.__o)){ SGB[this.__o+1]=this._y; if(chg){ globalThis.__sgDirty=1; __markD(this.__o - (this.__o % ST)); } } if(chg&&this.__sgRO)__markRender(this.__sgRO); }});
    var _set=OPp.set, _cf=OPp.copyFrom;
    OPp.set=function(x,y){ var r=_set.call(this,x,y); _wr(this); return r; };
    OPp.copyFrom=function(p){ var r=_cf.call(this,p); _wr(this); return r; };
    // (2) Transform.rotation → buffer
    var _rd=Object.getOwnPropertyDescriptor(TRp,'rotation');
    Object.defineProperty(TRp,'rotation',{configurable:true,get:_rd.get,set:function(v){ var chg=this._rotation!==v; _rd.set.call(this,v); if(this.__b!==undefined&&__sgOwnsSlot(this.__sgOwner,this.__b)){ SGB[this.__b+oROT]=this._rotation; if(chg){ globalThis.__sgDirty=1; __markD(this.__b); } } }});
    // (3) DisplayObject.alpha / visible → buffer(转成访问器)
    //   注意:DisplayObject 上是 __i(节点号),__b 在 transform 上!曾误用 __b 导致绑定后的
    //   alpha/visible 变化全部丢失(slot 停留在绑定快照)→ 弹窗 mask 淡入 WALPHA 恒 0 等一系列问题。
    var _defAV = function(name, off, dflt){
      // alpha 影响 worldAlpha 链必须标脏;visible 虽然 compute 不读,但作为安全网也标——
      Object.defineProperty(DOp, name, { configurable:true,
        get:function(){ return this['_'+name]===undefined?dflt:this['_'+name]; },
        set:function(v){
          var old=this['_'+name], chg=old!==v; this['_'+name]=v;
          if(this.__i>0&&__sgOwnsSlot(this,this.__i*ST)){ SGB[this.__i*ST+off]=(name==='visible'?(v?1:0):v); if(chg){ globalThis.__sgDirty=1; __markD(this.__i*ST); } }
        } });
    };
    _defAV('alpha', oALPHA, 1); _defAV('visible', oVIS, 1);
    // renderable 同步(fgui 用它藏 mask 矩形等;bool → 1/0 写 REND slot 28)
    Object.defineProperty(DOp, 'renderable', { configurable:true,
      get:function(){ return this._renderable===undefined?true:this._renderable; },
      set:function(v){
        var chg=this._renderable!==v; this._renderable=v; if(this.__i>0&&__sgOwnsSlot(this,this.__i*ST)){ SGB[this.__i*48+28]=(v===false?0:1); if(chg){globalThis.__sgDirty=1;__markBoundsNode(this.__i);} }
      } });
