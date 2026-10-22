    var NativeSceneRuntimeLocalWorld=new PIXI.Rectangle();
    // ===== native-only 主屏提交 =====
    // Main screen and RenderTexture content both consume the same native scene tree. Pixi still
    // owns RenderTexture allocation/FBO lifetime, but no longer traverses display objects to fill it.
    var __nativePresent = globalThis.__nativePresent;
    var __nativeBackground = new Float32Array(4);
    if (typeof __nativePresent !== 'function') throw new Error('缺少宿主能力 __nativePresent');
    if (PIXI.Renderer && !globalThis.__nativeScreenHook){
      globalThis.__nativeScreenHook = 1;
      globalThis.__NATIVE_SCREEN = 1;
      globalThis.__SG_DIAG    = (globalThis.__SG_DIAG!==undefined)    ? globalThis.__SG_DIAG    : 0;    // 1:打开 native scenegraph 诊断日志
      globalThis.__FLTIGNORE  = 0;    // 未登记的 Filter 必须进入 T_ESCAPE；严格 coverage 不能把缺失实现误报为通过
      // FPS 是宿主诊断 UI，不属于游戏渲染树。mac/iOS 各自由 native overlay 展示，
      // 避免动态 Text 纹理参与业务层级、遮罩和 RenderTexture 状态切换。
      // slotObject 的骨骼矩阵由 C++ compute 写入 Native SceneGraph；可见性、alpha 和
      // drawOrder 已合并进上面的唯一 record pass。这里仅确保新增绑定完成。
      // (金币文本/topbar-y 调查的 __coin3/__tbw/[mon3] 取证探针已随定案拆除,2026-08-25)
      PIXI.Renderer.prototype.render = function(displayObject, options){
        // M6 flip:短路 pixi,C++ native 直接上屏(复刻 mode3:驱动 spine + 事件)
        {
          var rtF = options && options.renderTexture;
          var _cl2=0; try{ var _mainApp=globalThis.__PIXI_APP__, _mainRenderer=_mainApp&&_mainApp.renderer;
            _cl2=(!rtF && displayObject && _mainRenderer && this===_mainRenderer)?1:0; }catch(e){}   // 只接管主屏 renderer;工具/离屏 Renderer 不能覆盖 flip root
          if(globalThis.__SG_DIAG===1 && (globalThis.__flipProbeN||0)<8){ globalThis.__flipProbeN=(globalThis.__flipProbeN||0)+1; __log('[flip-probe] enter='+(_cl2?1:0)+' rt='+(rtF?1:0)+' i='+((displayObject&&displayObject.__i)|0)+' name='+((displayObject&&displayObject.name)||((displayObject&&displayObject.constructor&&displayObject.constructor.name)||'?'))); }
          if (_cl2){
            var __preF=0, __flipProf=globalThis.__PROF===1, __flipT0=__flipProf?performance.now():0, __flipPhaseT=__flipT0;
            var __flipPreMs=0, __flipSpMs=0, __flipSgMs=0, __flipDataMs=0;
            try {
              globalThis.__nativeRenderer=this; globalThis.__sgRenderer=this;
              // 主屏纹理由 native tree 持有并采样，不会经过 Pixi TextureSystem.bind 更新 touched。
              // Pixi AUTO GC 会把仍在 native tree 使用的静态纹理误判为闲置（默认约 3600 帧后回收）。
              // 改为显式资源生命周期：场景的 Assets.unload/BaseTexture.destroy 仍正常释放 GPU 对象。
              if(!this.__nativeTextureGC && this.textureGC){
                this.__nativeTextureGC=1;
                this.textureGC.mode=PIXI.GC_MODES.MANUAL;
                __log('[native-screen] Pixi TextureGC=AUTO -> MANUAL (native tree owns visible textures)');
              }
              if (displayObject.__i === undefined) __bind(displayObject);
              if(globalThis.__SG_DIAG===1 && (globalThis.__flipBindN||0)<8){ globalThis.__flipBindN=(globalThis.__flipBindN||0)+1; __log('[flip-bind] i='+((displayObject&&displayObject.__i)|0)+' children='+(displayObject&&displayObject.children?displayObject.children.length:0)); }
              if (displayObject.__i > 0){
                if(globalThis.__nativePrepareMeshRender)globalThis.__nativePrepareMeshRender(displayObject);
                // 上一帧 native batcher 改过真实 GL 纹理绑定；先废弃 Pixi 的绑定缓存，避免动态文本/canvas
                // 更新时省略 bindTexture，把 texSubImage2D 写进 native 最后使用的图片纹理。
                try{ if(this.texture&&this.texture.reset)this.texture.reset(); }catch(e){}
                try{ this.runners.prerender.emit(); this.emit('prerender'); __preF=1; }catch(e){}
                if(__flipProf){var __flipNow=performance.now();__flipPreMs=__flipNow-__flipPhaseT;__flipPhaseT=__flipNow;}
              __sgPh(3);   // B0-1:阶段3=native tween/transition hook
              if(globalThis.__nativeTweenFrame)try{globalThis.__nativeTweenFrame(performance.now());}catch(e){if(!globalThis.__nativeTweenErr){globalThis.__nativeTweenErr=1;__log('[native-tween] frame '+e.message);}}
              if(globalThis.__nativeFguiTransitionFrame)try{globalThis.__nativeFguiTransitionFrame(performance.now());}catch(e){if(!globalThis.__nativeFguiTransitionErr){globalThis.__nativeFguiTransitionErr=1;__log('[native-fgui] transition frame '+e.message);}}
              __sgPh(4);   // B0-1:阶段4=spine 驱动段
              // Manual Spine deltas belong to this render attempt only if C++ receives them before
              // its single Runtime::updateAll. Flushing merely before present leaves pose N-1 next
              // to ordinary UI N even though both bridges have individually completed.
              if(typeof globalThis.__spxFacadeBeforeAdvance==='function'){
                try{globalThis.__spxFacadeBeforeAdvance();}
                catch(e){if(!globalThis.__spxPreAdvanceErr){globalThis.__spxPreAdvanceErr=1;__log('[spinex-pre-advance] '+e.message);}}
              }
              // Phase D-1:native 动画(spine 帧库/spinex updateAll)统一在 ADVANCE_NATIVE 推进,
              // 写入发生在 FINAL_COMPUTE 之前(帧内闭环);draw 期从此零写入零 compute。
              if(globalThis.__sgAdvanceAnimations)try{globalThis.__sgAdvanceAnimations(displayObject.__i|0);}catch(e){if(!globalThis.__sgAdvErr){globalThis.__sgAdvErr=1;__log('[sg-adv] '+e.message);}}
              // P1-C 保险:ADVANCE_NATIVE 的 canonical 清单立即消费(空清单=零成本)。不消费则
              // phase5 的 compute 在 scenegraph.h:454 重置清单 → phase4 收敛的节点 facade 永久停旧值。
              // 回写失败不能静默:一次性报警 + 硬计数(正确性回写缺帧=facade 停旧值的直接前兆)。
              if(globalThis.__sgWriteback)try{globalThis.__sgWriteback();}catch(e){
                globalThis.__sgAdvWbErrN=(globalThis.__sgAdvWbErrN||0)+1;
                if(!globalThis.__sgAdvWbErr){globalThis.__sgAdvWbErr=1;__log('[sg-adv-wb] ADVANCE_NATIVE 回写异常 '+e.message);}
              }
              var __now=Date.now(), __dt=globalThis.__spDrvT?(__now-globalThis.__spDrvT)/1000:0.016; globalThis.__spDrvT=__now; if(__dt>0.1)__dt=0.1; if(__dt<0)__dt=0;   // flip 帧间隔(秒)
              // Dispatch the events produced by that one advance before final canonical compute.
              // Event callbacks may hide/destroy nodes in frame N. Any callback-created manual
              // delta remains queued for N+1; there is deliberately no second same-dt advance.
              if(typeof globalThis.__spxFacadeAfterAdvance==='function'){
                try{globalThis.__spxFacadeAfterAdvance();}
                catch(e){if(!globalThis.__spxPostAdvanceErr){globalThis.__spxPostAdvanceErr=1;__log('[spinex-post-advance] '+e.message);}}
              }
              // This bounded two-domain barrier throws on an incomplete/failed generation. The
              // outer native-screen guard then aborts this presentation instead of sampling a
              // mixed facade/pane state; fail() has already switched future writes to immediate.
              __convergeFguiBeforePresent(displayObject);
                if(globalThis.__SG_AUDIT===1&&((globalThis.__sgHoleFrame=(globalThis.__sgHoleFrame||0)+1)%30)===1)__repairLinkedTreeHoles(displayObject);
                // Spine 动画时间只由业务层推进；slotObject 的矩阵由 C++ Skeleton.compute
                // 直接写 Native SceneGraph，可见性/alpha/drawOrder 在 AnimationState.apply 完成时
                // 按需同步。主屏 present 不再遍历任何 Spine root 或 JS slotObject Map。
                if(__flipProf){var __flipNow=performance.now();__flipSpMs=__flipNow-__flipPhaseT;__flipPhaseT=__flipNow;}
                if(globalThis.__SG_DIAG===1 && (globalThis.__adT=(globalThis.__adT||0)+1)%60===0 && (globalThis.__adN||0)<8){ globalThis.__adN=(globalThis.__adN||0)+1; __ov.forEach(function(o){ if(o.state&&o.state.tracks){ var tr=o.state.tracks[0]; if(tr&&tr.animation) __log('[anim] '+((o.skeleton&&o.skeleton.data&&o.skeleton.data.name)||'?')+' anim='+(tr.animation.name)+' t='+(tr.trackTime||0).toFixed(2)+' loop='+tr.loop+' auto='+o.autoUpdate); } }); }   // 诊断:trackTime 每秒推进没
                // One presentation is a transaction: compute world matrices, write dynamic geometry,
                // then check whether those steps synchronously added/reparented a node or changed a
                // local transform. Retry inside this same present instead of hiding the node for a
                // later frame. The normal path is one pass; only a real late mutation pays for another.
                var __ru=0,__atomicPass=0,__atomicRetry=0;
                // The quota belongs to this presentation transaction, not to each convergence pass.
                // Otherwise a 3-pass frame can rasterise 3x the advertised amount and still hitch.
                var __nativeBudget=(globalThis.__nativeRenderWorkBudget|0);
                globalThis.__nativeRenderBudgetRemaining=__nativeBudget>0?__nativeBudget:0x7fffffff;
                // Phase A-5:verify 静止帧盲区补偿。__SG_DIRTY=2(全量对拍)只在"本帧有 mark"时触发
                // (0.5 小数标记),纯静止帧凝固态漏检。每 300 帧对 stage 根补一次幂等 mark:同值重算
                // 零视觉扰动,但让 verifyNode 周期性全树对拍,凝固病灶最迟 5s 内现形。
                if(globalThis.__SG_DIRTY===2&&((globalThis.__frameNo|0)%300===0)&&displayObject.__i>0)__markD(displayObject.__i*ST);
                __sgPh(5);   // Phase D-1:阶段5=固定两段式收敛(替换三次猜测重试)
                globalThis.__sgInFlush=1;
                {
                  // 第一段:排序收敛→主 compute→回写→渲染描述 flush(布局回写等写入允许发生)
                  __flushSortableOrder(displayObject);
                  var __atomicSgT=__flipProf?performance.now():0;
                  try{if(globalThis.__SG_AUDIT===1)__syncLocals();__sgCanonicalBarrier(displayObject.__i);}catch(e){}
                  if(__flipProf)__flipSgMs+=performance.now()-__atomicSgT;
                  var __atomicDataT=__flipProf?performance.now():0;
                  __ru+=__flushRenderData(displayObject);
                  if(__flipProf)__flipDataMs+=performance.now()-__atomicDataT;
                  __atomicPass=1;
                  // 第二段(条件):flush 产生了新写(结构/队列/债务任一)→只 compute 不再 flush。
                  // compute 自身不写 local,故第二段后必然收敛——结构性保证,非猜测式重试。
                  if((SGB[DB]|0)!==0||(SGB[DBB+(displayObject.__i|0)]>0.5)){
                    __flushSortableOrder(displayObject);
                    var __sg2T=__flipProf?performance.now():0;
                    try{__sgCanonicalBarrier(displayObject.__i);}catch(e){}
                    if(__flipProf)__flipSgMs+=performance.now()-__sg2T;
                    __atomicPass=2;
                  }
                  // seal 检测:两段后仍有本根债务=真 late mutation(理论恒 0;draw 后写入归下帧不计)
                  __atomicRetry=(SGB[DBB+(displayObject.__i|0)]>0.5);
                  // P2-4:present seal 显式发布(债务归零才推进 sealedEpoch;未收敛调用只计数)
                  if(!__atomicRetry&&typeof globalThis.__sgPublishSeal==='function')globalThis.__sgPublishSeal(displayObject.__i|0);
                }
                globalThis.__sgInFlush=0;
                __sgPh(6);   // B0-1:阶段6=compute 后(present 前)
                __refreshDisplacementOwners();   // maskSprite 每帧运动 → 位移场描述符跟随(本帧世界矩阵已回写,写在 __nativePresent 读取 SGB 之前)
                __refreshMutableFilterOwners();  // TypedArray 下标原地写无 setter:draw 前按值刷新 native filter payload
                if(typeof __dogRootDiagFlush==='function')__dogRootDiagFlush();   // 诊断段已被移除,防悬空调用每帧抛异常
                if(typeof __swapDump==='function')__swapDump();   // [swap-tree] 诊断段已被移除,防悬空调用每帧抛异常
                if(__flipProf)__flipPhaseT=performance.now();
                if(__atomicRetry){var __awin=((globalThis.__frameNo|0)/300)|0;if(globalThis.__sgAtomicWarnWin!==__awin){globalThis.__sgAtomicWarnWin=__awin;globalThis.__sgAtomicRetryWarn=0;}}
                if(__atomicRetry&&(globalThis.__sgAtomicRetryWarn||0)<4){globalThis.__sgAtomicRetryWarn=(globalThis.__sgAtomicRetryWarn||0)+1;__log('[sg-atomic] present 后仍有连续变更 pass='+__atomicPass+' dirty='+(SGB[DB]|0)+' debt='+SGB[DBB+(displayObject.__i|0)].toFixed(0)+' f='+(globalThis.__frameNo|0));
                  var __dn=0,__dl='';for(var __di2=1;__di2<32768&&__dn<6;__di2++){if((SGB[PDB+__di2]|0)&3){__dn++;__dl+=' i'+__di2+'(pd'+(SGB[PDB+__di2]|0)+' mk'+(SGB[MPB+__di2]|0)+' ty'+(SGB[__di2*ST+11]|0)+' p'+(__sgNativeParent?(__sgNativeParent(__di2)|0):-9)+' al'+((typeof __sgNativeAlive==='function')?(__sgNativeAlive(__di2)|0):-9)+')';}}
                  __log('[sg-atomic] 欠债样本:'+__dl);
                  if(globalThis.__SG_TRACE_DIRTY===1&&typeof globalThis.__sgDirtyTraceDump==='function')globalThis.__sgDirtyTraceDump(6);}   // Phase A-3:警告帧自动给出 dirty 生产者构成
                if(globalThis.__SG_DIAG===1 && (globalThis.__incT=(globalThis.__incT||0)+1)%120===0){ var __qs=globalThis.__sgQuadStats(); __log('[render-inc] updated='+__ru+' dynamic='+__qs.dynamic+' filters='+__qs.filterDynamic+' q='+__qs.used+'/'+__qs.capacity+' owners='+__qs.owners+' free='+__qs.freeBlocks+' reclaimed='+__qs.reclaimed); }
                if(globalThis.__SG_DIAG===1 && (globalThis.__spSkT=(globalThis.__spSkT||0)+1)%120===0){ var _s=globalThis; if((_s.__spNoTex||0)+(_s.__spNoLen||0)+(_s.__spEsc||0)>0) __log('[sp-skip] 无纹理'+(_s.__spNoTex||0)+' arena空'+(_s.__spNoLen||0)+' 整spine逃逸'+(_s.__spEsc||0)+'(每120帧;缺数据不静默)'); _s.__spNoTex=0;_s.__spNoLen=0;_s.__spEsc=0; }   // #4:spine 缺数据可见
                if (this.objectRenderer) this.objectRenderer.lastObjectRendered=displayObject;   // interaction 依赖
                // native draw 会在当前 JS 调用栈内立即采样纹理。先执行尚在编码缓冲里的
                // texImage/generateMipmap/texParameteri，保持与 WebGL 提交顺序一致；否则大图 level0
                // 被节流后，batcher 会赶在 mipmap 命令解码前采样不完整纹理，首帧得到整块黑色。
                if(globalThis.__encFlush)globalThis.__encFlush();
                // Cross-domain epochs are unrelated clocks. The audit captures one vector only
                // after every local domain has closed, then checks exact per-domain stability at
                // the draw boundary. It is test/grey instrumentation; a failed check is reported
                // but cannot pretend that skipping this JS call would preserve the host backbuffer.
                var __frameSeal=typeof globalThis.__nativeFrameSealCapture==='function'?globalThis.__nativeFrameSealCapture(displayObject.__i|0):null;
                if(__frameSeal&&typeof globalThis.__nativeFrameSealVerify==='function')globalThis.__nativeFrameSealVerify(__frameSeal);
                globalThis.__sgStage=displayObject;   // [sg-watch] 渲染根引用(排查巡检/手动 __sgCompare 用)
                // unsealed 口径=本根债务(DEBT 是 per-root 可达性契约);DB 队列计数含合法跨帧
                // detached 保留脏(不属于本 present 树),不算未封存——首版口径每帧误报 ~57/帧。
                if(SGB[DBB+(displayObject.__i|0)]>0.5)globalThis.__sgUnsealedPresent=(globalThis.__sgUnsealedPresent|0)+1;
                // The native draw replaces Pixi's main-screen clear. Carry its current color
                // and alpha into the host instead of inheriting the host's black default.
                this.background.backgroundColor.toArray(__nativeBackground);
                __nativePresent(displayObject.__i, __nativeBackground[0], __nativeBackground[1],
                  __nativeBackground[2], __nativeBackground[3]);
                globalThis.__sgPresentedF = globalThis.__frameNo;   // [sp-vis] 用:标记本帧已 present,之后的可见性变更只能下帧上屏
                if(typeof globalThis.__sgProbeFlush==='function')globalThis.__sgProbeFlush();   // 临时诊断:PiggyBank 探针(present 后采样=本帧终态)
                if(globalThis.__SG_DIAG===1 && (globalThis.__flipFlagN||0)<8){ globalThis.__flipFlagN=(globalThis.__flipFlagN||0)+1; __log('[flip-flag] root='+displayObject.__i+' q='+(SGB[QB]|0)); }
                try{ this.runners.postrender.emit(); this.emit('postrender'); __preF=0; }catch(e){}
                if(__flipProf)globalThis.__renderMs=performance.now()-__flipT0;
                if(__flipProf){var __fa=globalThis.__flipAcc||(globalThis.__flipAcc={n:0,pre:0,sp:0,sg:0,data:0,post:0});
                  __fa.n++;__fa.pre+=__flipPreMs;__fa.sp+=__flipSpMs;__fa.sg+=__flipSgMs;__fa.data+=__flipDataMs;__fa.post+=performance.now()-__flipPhaseT;
                  if(__fa.n>=120){var __fi=1/__fa.n;__log('[flip-prof] pre '+(__fa.pre*__fi).toFixed(2)+'ms spine '+(__fa.sp*__fi).toFixed(2)+' sg '+(__fa.sg*__fi).toFixed(2)+' data '+(__fa.data*__fi).toFixed(2)+' post '+(__fa.post*__fi).toFixed(2));__fa.n=0;__fa.pre=__fa.sp=__fa.sg=__fa.data=__fa.post=0;}}
                globalThis.__wasFlip = 1;   // 本帧 flip 用了 native GL 状态 → 下次 pixi 渲染前需重建其状态缓存
                return;   // 短路 pixi
              }
            } catch(e){ if(__flipProf)globalThis.__renderMs=performance.now()-__flipT0; if(!globalThis.__flipErr){ globalThis.__flipErr=1; __log('[native-screen] 异常 '+e.message); }
              if(__preF){ try{ this.runners.postrender.emit(); this.emit('postrender'); }catch(e2){} } }
            return;   // 主屏只允许 native 提交；异常也不回落到 Pixi 真屏路径
          }
        }
        var __nativeEpoch=globalThis.__nativeGlEpoch||0;
        if (globalThis.__wasFlip && this.__nativeResetEpoch!==__nativeEpoch){
          this.__nativeResetEpoch=__nativeEpoch;   // native 在上一宿主帧末尾改过真实 GL 状态；本帧首次 Pixi 离屏绘制前重建缓存
          try{
            this.shader.reset(); this.geometry.reset();
            if(this.buffer&&this.buffer.reset)this.buffer.reset();
            this.state.reset(); this.texture.reset();
            // native 主屏提交会真实绑定默认 FBO，但不会经过 Pixi RenderTextureSystem。
            // Pixi 要求先失效底层 FBO 缓存，再由 RenderTextureSystem.reset() 绑定主屏并恢复
            // sourceFrame/projection/maskStack。顺序反过来会让两层缓存再次失配。
            if(this.framebuffer&&this.framebuffer.reset)this.framebuffer.reset();
            if(this.renderTexture&&this.renderTexture.reset)this.renderTexture.reset();
          }catch(e){}
        }
        if(rtF&&displayObject&&typeof globalThis.__nativeRenderTexture==='function'){
          globalThis.__nativeRenderer=this;globalThis.__sgRenderer=this;
          if(displayObject.__i===undefined)__bind(displayObject);
          if(displayObject.__i>0){
            if(globalThis.__nativePrepareMeshRender)globalThis.__nativePrepareMeshRender(displayObject);
            var __rtTopId=__nativeRootId(displayObject.__i),__rtTop=__sgObjs[__rtTopId]||{__i:__rtTopId};
            var __rtOldFlush=globalThis.__sgInFlush,__rtPass=0,__rtRetry=0;
            globalThis.__sgInFlush=1;
            do{
              __flushSortableOrder(__rtTop);var __rtStamp=__sgStructureVersion;
              try{__sgCanonicalBarrier(__rtTop.__i);}catch(e){}   // 四审:RT 烘焙前主树收敛=canonicalizing barrier(隐式 QueryNoAck 废止)
              __flushRenderData(__rtTop,displayObject);__rtPass++;__rtRetry=(__sgStructureVersion!==__rtStamp)||((SGB[DB]|0)!==0);
            }while(__rtRetry&&__rtPass<3);
            globalThis.__sgInFlush=__rtOldFlush;
            // Refuse before touching the target if any visible node is not native-complete.  The
            // authoritative hierarchy and render metadata live in C++; do not recursively scan
            // the JS compatibility DisplayObject tree on popup/filter/cache creation.
            // New texture commands must reach the decoder before its native texture lookup.
            if(globalThis.__encFlush)globalThis.__encFlush();
            var __rtReady=typeof globalThis.__nativeRenderTexturePreflight==='function'&&
              !!globalThis.__nativeRenderTexturePreflight(displayObject.__i);
            if(__rtReady){
              var __rbt=rtF.baseTexture,__rw=Number(__rbt&&__rbt.width)||Number(rtF.width)||1,__rh=Number(__rbt&&__rbt.height)||Number(rtF.height)||1,__rbb=(displayObject.__i|0)*ST;
              var ca=SGB[__rbb+12],cb=SGB[__rbb+13],cc=SGB[__rbb+14],cd=SGB[__rbb+15],ctx=SGB[__rbb+16],cty=SGB[__rbb+17],det=ca*cd-cb*cc;
              if(Math.abs(det)>1e-10){
                var ia=cd/det,ib=-cb/det,ic=-cc/det,id=ca/det,itx=(cc*cty-cd*ctx)/det,ity=(cb*ctx-ca*cty)/det;
                var la=SGB[__rbb+20],lb=SGB[__rbb+21],lc=SGB[__rbb+22],ld=SGB[__rbb+23],ltx=SGB[__rbb+24],lty=SGB[__rbb+25],tm=options&&options.transform;
                if(tm){var na=tm.a*la+tm.c*lb,nb=tm.b*la+tm.d*lb,nc=tm.a*lc+tm.c*ld,nd=tm.b*lc+tm.d*ld,ntx=tm.a*ltx+tm.c*lty+tm.tx,nty=tm.b*ltx+tm.d*lty+tm.ty;la=na;lb=nb;lc=nc;ld=nd;ltx=ntx;lty=nty;}
                var ma=la*ia+lc*ib,mb=lb*ia+ld*ib,mc=la*ic+lc*id,md=lb*ic+ld*id,mtx=la*itx+lc*ity+ltx,mty=lb*itx+ld*ity+lty;
                var worldAlpha=SGB[__rbb+oWALPHA],alphaScale=Math.abs(worldAlpha)>1e-8?SGB[__rbb+oALPHA]/worldAlpha:1,clearRT=options&&options.clear!==undefined?!!options.clear:!!(this.background&&this.background.clearBeforeRender);
                var __rtPre=0,__rtOk=false;
                try{
                  this.runners.prerender.emit();this.emit('prerender');__rtPre=1;
                  // Match Pixi's ObjectRendererSystem contract: only a screen render may replace
                  // lastObjectRendered. EventSystem reads that field before every pointer event to
                  // choose its rootTarget. Publishing this detached/offscreen brush here makes the
                  // next drag move miss the stage and synthesize a false stage pointerleave.
                  this.projection.transform=tm||null;this.renderTexture.bind(rtF);
                  // Native masks use stencil directly; Pixi MaskSystem is never traversed here.
                  // Allocate the current RT attachment before native stencil writes.
                  this.framebuffer.forceStencil();
                  if(globalThis.__encFlush)globalThis.__encFlush();
                  // Pixi uses opposite projection signs for the screen and framebuffer-backed
                  // RenderTextures (ProjectionSystem: root ? y-down : y-up).  Batcher::frame uses
                  // the screen projection for both targets, so pre-compose F(x,y)=(x,h-y) here.
                  // Without this conversion an interactive brush is written at h-y and a sampled
                  // RenderTexture (scratch masks in particular) reveals the mirrored location.
                  __rtOk=!!globalThis.__nativeRenderTexture(displayObject.__i,__rw,__rh,clearRT,alphaScale,
                    ma,-mb,mc,-md,mtx,__rh-mty);
                  if(__rtOk){
                    // Native drawing changed real GL bindings. Invalidate Pixi's
                    // caches immediately: a nested cache bake can bind/sample its
                    // texture before another renderer.render() reaches the entry
                    // reset above. Keeping the old sampler binding makes Android
                    // apply texture parameters to the last native source instead
                    // of the new render target (an incomplete/transparent sample).
                    this.shader.reset();this.geometry.reset();
                    if(this.buffer&&this.buffer.reset)this.buffer.reset();
                    this.state.reset();this.texture.reset();
                    if(this.framebuffer&&this.framebuffer.reset)this.framebuffer.reset();
                    if(options&&options.blit&&this.framebuffer&&this.framebuffer.blit)this.framebuffer.blit();
                    if(__rbt&&__rbt.update)__rbt.update();
                    var __rtgw=__rbt&&__rbt._glTextures&&__rbt._glTextures[this.CONTEXT_UID];if(__rtgw)__rtgw.dirtyId=__rbt.dirtyId;
                    this.runners.postrender.emit();this.projection.transform=null;this.emit('postrender');__rtPre=0;
                    globalThis.__nativeRenderTextureCount=(globalThis.__nativeRenderTextureCount||0)+1;globalThis.__wasFlip=1;this.__nativeResetEpoch=-1;return;
                  }
                }catch(__rte){if((globalThis.__nativeRtErrN||0)<8){globalThis.__nativeRtErrN=(globalThis.__nativeRtErrN||0)+1;__log('[native-rt] '+__rte.message);}}
                finally{if(__rtPre)try{this.runners.postrender.emit();this.projection.transform=null;this.emit('postrender');}catch(e){}}
              }
            }
          }
        }
        // Pixi Application performs one render of its empty lexical bootstrap Container before
        // NativeApplication can replace it with the numeric Native stage. Suppress that empty
        // constructor pass; it contains no game nodes and must not become a Pixi presentation.
        if(!rtF&&(globalThis.__nativeApplicationBootstrapDepth|0)>0){var __bootstrapStats=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});__bootstrapStats.suppressedApplicationBootstrapRenders=(__bootstrapStats.suppressedApplicationBootstrapRenders||0)+1;return;}
        // Native-only contract: unsupported offscreen/tool rendering is an explicit coverage
        // failure. Never rebuild or traverse a Pixi display tree as a silent fallback.
        if(typeof globalThis.__nativeRejectPixiRender==='function')globalThis.__nativeRejectPixiRender(rtF?'render-texture':'tool-renderer',displayObject);
        else{var __rejectStats=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});__rejectStats.rejectedPixiRenders=(__rejectStats.rejectedPixiRenders||0)+1;}
        return;
      };
      // native surface 从后台恢复后真实 GL 状态已变化，废弃 Pixi 的状态缓存；场景树和纹理资源本身不重建。
      globalThis.__nativeResume=function(){
        var r=globalThis.__nativeRenderer||globalThis.__sgRenderer;
        if(r){
          try{r.shader.reset();}catch(e){}
          try{r.geometry.reset();}catch(e){}
          try{if(r.buffer&&r.buffer.reset)r.buffer.reset();}catch(e){}
          try{r.state.reset();}catch(e){}
          try{r.texture.reset();}catch(e){}
          try{if(r.framebuffer&&r.framebuffer.reset)r.framebuffer.reset();}catch(e){}
          try{if(r.renderTexture&&r.renderTexture.reset)r.renderTexture.reset();}catch(e){}
        }
        // surface 恢复会重置真实 GL 状态；同时重写一次当前 JS 视觉真值，清掉休眠窗口内可能积累的
        // 旧 UV/texture id/quad 数据。该 O(nodes) 成本只发生在前台恢复，不进入逐帧热路径。
        for(var i=1;i<__sgObjs.length;i++){var o=__sgObjs[i];if(o&&o.__i===i&&!o._destroyed)__markRender(o);}
        globalThis.__spDrvT=Date.now();
        r.__nativeResetEpoch=-1;
        globalThis.__wasFlip=1;
        globalThis.__sgDirty=1;
      };
      __log('[native-screen] native-only 主屏提交已挂载');
    }
    // 并行校验:DFS 对比 C++ buffer 的 world 矩阵 vs pixi JS 的 transform.worldTransform,返回最大绝对误差。
    globalThis.__sgCmpN = 0;
    globalThis.__sgCompare = function(root){
      var maxd=0, n=0, worst=null, worstP=null;
      var walk=function(o, par){
        if(!o || o.visible===false) return;
        if(o.__i>0 && o.transform){ var b=o.__i*ST, wt=o.transform.worldTransform;
          var d=Math.max(Math.abs(wt.a-SGB[b+12]),Math.abs(wt.b-SGB[b+13]),Math.abs(wt.c-SGB[b+14]),Math.abs(wt.d-SGB[b+15]),Math.abs(wt.tx-SGB[b+16]),Math.abs(wt.ty-SGB[b+17]));
          if(d>maxd){ maxd=d; worst=o; worstP=par; } n++; }
        var ch=o.children; if(ch) for(var i=0;i<ch.length;i++) walk(ch[i], o);
      };
      walk(root, null); globalThis.__sgCmpN=n;
      if(worst && maxd>0.01){
        // 从最差节点往上走,打印每层祖先:名字 | 是否绑定 | buf visible | world tx(buf vs JS),定位断链点
        var chain=[], p=worst, guard=0;
        while(p && guard++<20){ var nm=(p.constructor&&p.constructor.name)||'?';
          if(p.__i>0){ var pb=p.__i*ST; chain.push(nm+'[i'+p.__i+' vis'+SGB[pb+10]+' wtxB'+SGB[pb+16].toFixed(0)+'/J'+(p.transform?p.transform.worldTransform.tx.toFixed(0):'?')+']'); }
          else chain.push(nm+'[未绑定__i='+p.__i+']');
          p=p.parent; }
        __log('[sg-worst] 链(下→上): '+chain.join('  ←  '));
      }
      return maxd;
    };
