    // ===== spine 蒙皮桥(C++ computeWorldVertices)=====
    // JS 保留动画状态/事件/API;每帧每 spine:__spPrep 写骨骼矩阵 → native skinAll 算全部已注册 slot 顶点
    // → computeWorldVertices patch 变成一次拷贝。deform/sequence/部分顶点(start≠0)/未注册 → JS 原始 fallback。
    // spine 库晚于本模块加载。game_host 在库就位后立即调 __nativeSpineRuntimeReady，
    // update/prime 时已经走 Native Skeleton/AnimationState。__ovChk 里仍保留懒安装兜底。
    // Called immediately after spine-pixi-v7.js is evaluated.  Besides moving the one-time patch
    // work out of popup construction, the opt-in profiler separates official Spine construction,
    // Skeleton.updateCache and the explicit pool-prime update without changing production calls.
    globalThis.__nativeSpineRuntimeReady=function(){
      // 旧的 __spSkinPatch(给 spine-ts 原型打蒙皮/形变/物理/cache 补丁)已随 legacy 路径退役:
      // 全部 spine 走 NativeSpine 门面,其 skeleton 是 SpxSkeleton 而非 spine-ts Skeleton,补丁永不触发。
      // 下面的尺寸访问器与 opt-in 构造 profiler 仍然需要,故保留本函数。
      var SPN=globalThis.spine;
      // spine-pixi captures Pixi's original DisplayObject prototype before the native container
      // replacement is installed.  Give every Spine the same bounds-based size contract as a
      // native container so responsive `width`/`height` assignments update the shared transform.
      // This is renderer-wide and applies to any responsive Spine, not to a named asset/view.
      if(SPN&&SPN.Spine&&SPN.Spine.prototype&&!Object.prototype.hasOwnProperty.call(SPN.Spine.prototype,'__nativeSizeAccessors')){
        var spineProto=SPN.Spine.prototype,spineBounds=new PIXI.Rectangle();
        try{Object.defineProperties(spineProto,{
          width:{configurable:true,get:function(){var r=__spineLayoutBounds(this,spineBounds);return Math.abs(this.scale.x)*(r&&r.width||0);},set:function(value){var r=__spineLayoutBounds(this,spineBounds),w=r&&r.width||0,sign=this.scale.x<0?-1:1;if(w)this.scale.x=Math.max(0,Number(value)||0)/w*sign;else this.__nativePendingWidth=Number(value)||0;}},
          height:{configurable:true,get:function(){var r=__spineLayoutBounds(this,spineBounds);return Math.abs(this.scale.y)*(r&&r.height||0);},set:function(value){var r=__spineLayoutBounds(this,spineBounds),h=r&&r.height||0,sign=this.scale.y<0?-1:1;if(h)this.scale.y=Math.max(0,Number(value)||0)/h*sign;else this.__nativePendingHeight=Number(value)||0;}}
        });spineProto.__nativeSizeAccessors=1;}catch(_spineSizeAccessorError){}
      }
      var PixiSpine=PIXI.spine&&PIXI.spine.Spine;
      if(PixiSpine&&PixiSpine.prototype&&!Object.prototype.hasOwnProperty.call(PixiSpine.prototype,'__nativeSizeAccessors')){
        var pixiSpineProto=PixiSpine.prototype,pixiSpineBounds=new PIXI.Rectangle();
        try{Object.defineProperties(pixiSpineProto,{
          width:{configurable:true,get:function(){var r=__spineLayoutBounds(this,pixiSpineBounds);return Math.abs(this.scale.x)*(r&&r.width||0);},set:function(value){var r=__spineLayoutBounds(this,pixiSpineBounds),w=r&&r.width||0,sign=this.scale.x<0?-1:1;if(w)this.scale.x=Math.max(0,Number(value)||0)/w*sign;else this.__nativePendingWidth=Number(value)||0;}},
          height:{configurable:true,get:function(){var r=__spineLayoutBounds(this,pixiSpineBounds);return Math.abs(this.scale.y)*(r&&r.height||0);},set:function(value){var r=__spineLayoutBounds(this,pixiSpineBounds),h=r&&r.height||0,sign=this.scale.y<0?-1:1;if(h)this.scale.y=Math.max(0,Number(value)||0)/h*sign;else this.__nativePendingHeight=Number(value)||0;}}
        });pixiSpineProto.__nativeSizeAccessors=1;}catch(_pixiSpineSizeAccessorError){}
      }
      return true;
    };
    // spine 自测开关原本在此补装 __spSkinPatch(已随 legacy 路径退役)。
    // 脏剪枝通道:local/alpha 变化的节点号 push 进共享 buffer 追加区(C++ compute 只重算脏支)。
    // 溢出(>4096)或 __SG_DIRTY=0(回退开关)→ 写溢出值 = C++ 视为全脏帧(等价旧全量行为)。
    var NCAP=32768, DB=NCAP*48, DCAP=4096, CCAP=4096;
    var QB=DB+1+DCAP+1+CCAP, QSF=32, QCAP=65536;
    // Keep this aligned with scenegraph.h::FILTER_STRIDE_F. Direct and offscreen filter payloads
    // extend the old 24-float block; the following bounds/hit regions must start after all 104.
    var FSF=224, FB=QB+1+QCAP*QSF;
    var BSF=20, BB=FB+NCAP*FSF, BM=BB+NCAP*BSF;
    var HCAP=2048, HR=BM+2, HAC=HR+1+HCAP*2, HAB=HAC+1;
    // HAS_FILTER 位(1 float/节点,同步 scenegraph.h FILTER_FLAG_BASE):__wrFilters 写非默认
    // filter 状态时置 1、恢复默认时清 0;C++ batcher 位 0 时跳过整个 FILTER 区读取。
    var FFB=HAB+HCAP;
    // bounds 增量化事件队列(同步 scenegraph.h BOUNDS_QUEUE_BASE):内容改写(quad/纹理 rect/
    // hitArea/hitflags/renderable)把节点 idx 追加进来,C++ updateBounds 汇入 per-node 脏位后
    // 只沿脏链重算;溢出写 cap+1 = C++ 全脏回退。transform 变化仍走 __markD→compute 自动脏化。
    var BQB=FFB+NCAP, BQCAP=4096;
    // 去重印章:同一 drain 窗口内每节点只入队一次(x/y/scale/rotation 逐 setter 调用会对同一
    // 节点重复标记,重帧不去重会把 4096 容量顶溢出 → C++ 全脏回退,增量收益清零)。
    // C++ 汇走后 count 归 0,下一次 push 观察到 0 即翻代,全部节点重新可入队。
    var __bqStamp=new Int32Array(NCAP), __bqEpoch=1;
    // B0-1 时序诊断区(同步 scenegraph.h FRAME_PHASE_BASE/MARK_PHASE_BASE/CONSUME_TAG_BASE):
    // PHB=当前帧阶段(1=C++前段 2=JS tick 3=tween/transition hook 4=spine 驱动 5=主 compute 循环
    // 6=compute 后 7=present);MPB[idx]=该节点 local 脏最后写入阶段。宿主设 1/2/7,JS 设 3-6。
    var PHB=BQB+1+BQCAP, MPB=PHB+1;
    // B0-2 持久脏位区(同步 scenegraph.h PDIRTY/DEBT/WSEQ/EPOCH;诊断区 CONSUME/VISIT/PWTY 各占
    // NCAP,依布局顺序推进):位为正确性、队列为加速。JS 写入口与 C++ writer 同一套置位协议。
    // 布局链与 scenegraph.h 严格同步:CONSUME→VISIT→PWTY→LTYPE→PDIRTY→DEBT→WSEQ→EPOCH。
    // 任何一侧插区段必须双侧同改(2026-08-13 曾因 C++ 插 LTYPE 未同步 JS,置位整体错位一格)。
    // Phase E:EFX 合成因子区段(EFA=alpha 因子/EFV=vis 门,纯 C++ 通道,JS 只对齐布局)。
    var CTB=MPB+NCAP, VGB=CTB+NCAP, PWB=VGB+NCAP, LTB=PWB+NCAP, PDB=LTB+NCAP, DBB=PDB+NCAP, WQB=DBB+NCAP, EPB=WQB+NCAP, EFA=EPB+2, EFV=EFA+NCAP, WEB=EFV+NCAP, LSC=WEB+NCAP, NGB=LSC+8,
        // fgui facade 版本化缓存协议(docs/fgui-facade-commit-protocol.md §1):FSQ=facadeSeq FDB=facadeDirtyBits FCS=childSeq FCM=instanceCommitSeq[root],全部 u32 视图
        FSQ=NGB+NCAP, FDB=FSQ+NCAP, FCS=FDB+NCAP, FCM=FCS+NCAP, LMB=FCM+NCAP;
    // B0-3 必修点4:WSEQ/EPOCH 用 Uint32 视图(Float32 2^24 后 +1 失效);布局握手:C++ init 写入
    // MAGIC/SHARED_FLOATS/关键 offset,JS 公式不一致=启动即抛错,杜绝区段插入漂移。
    var U32=new Uint32Array(SGB.buffer, SGB.byteOffset, SGB.length);
    (function(){
      var magic=SGB[LMB]|0, sf=SGB[LMB+1]|0, pdb=SGB[LMB+2]|0, wqb=SGB[LMB+3]|0;
      if(magic!==8130006||sf!==SGB.length||pdb!==PDB||wqb!==WQB)
        throw new Error('[sg-layout] 共享布局握手失败 magic='+magic+' C++PDIRTY='+pdb+' JS='+PDB+' C++WSEQ='+wqb+' JS='+WQB+' — 两侧区段定义不同步');
      if(globalThis.__SG_DIAG===1)__log('[sg-layout] 握手通过 floats='+sf+' PDIRTY@'+pdb+' WSEQ@'+wqb);
    })();
    // clean→dirty 债务上卷:沿 __nativeParentOf 缓存父链(懒失效,-2 时桥查)。仅位翻转时走链(低频)。
    var __persistMarkDirty=function(idx){
      U32[WQB+idx]=(U32[WQB+idx]+1)>>>0; U32[EPB]=(U32[EPB]+1)>>>0;   // uint32 计数,不受 2^24 限制
      var pd=SGB[PDB+idx]|0;
      if(!(pd&1)){
        // 债务上卷统一经 C++ 真父链(__sgDebtRollup 内含置位+加债)——JS 缓存父链与真链不一致
        // 时,加债走缓存/减债走真链=每循环净泄漏(实锤:DEBT[stage] 恒 115 死债)。桥缺席
        // (node 测试环境)退回缓存链。
        if(typeof globalThis.__sgDebtRollup==='function'){globalThis.__sgDebtRollup(idx);}
        else{
          SGB[PDB+idx]=pd|1; SGB[DBB+idx]+=1;
          var p=__nativeParentOf(idx),g=0;
          while(p>=0&&g++<64){SGB[DBB+p]+=1;if(p===0)break;p=__nativeParentOf(p);}
        }
      }
    };
    var __markBoundsNode=function(i){
      SGB[BM]=1; if(!(i>0)||i>=NCAP)return;
      var n=SGB[BQB]|0;
      if(n===0)__bqEpoch++;
      else if(n>BQCAP)return;   // 已饱和(本窗口 C++ 将全脏回退)
      if(__bqStamp[i]===__bqEpoch)return;
      __bqStamp[i]=__bqEpoch;
      if(n<BQCAP){SGB[BQB+1+n]=i;SGB[BQB]=n+1;}
      else SGB[BQB]=BQCAP+1;   // 溢出饱和(幂等):C++ 视为全脏帧
    };
    var __markBoundsAll=function(){ SGB[BM]=1; SGB[BQB]=BQCAP+1; };
    // 主屏渲染数据只处理内容发生变化的节点。Pixi 对象继续作为游戏/FGUI 的 API 外壳，
    // 但 native present 不再按 Pixi children 做渲染遍历，也不再每帧扫描全部对象。
    var __renderDirty = new Set(), __renderDetachedDirty = new Set();
    // Pixi tint 是 ColorSource，getter 会保留传入形式（如 '#7272E6'）。共享树只接受 0xRRGGBB，
    // 不能把字符串直接写入 Float32Array，否则会变 NaN 并在 C++ 端落成黑色。
    var __tintOf = function(o){
      if(!o) return 0xFFFFFF;
      try{ var tv=o.tintValue; if(typeof tv==='number'&&isFinite(tv)) return tv&0xFFFFFF; }catch(e){}
      var v; try{v=o.tint;}catch(e){v=0xFFFFFF;}
      if(v===undefined||v===null)return 0xFFFFFF;
      if(typeof v==='number'&&isFinite(v))return v&0xFFFFFF;
      try{return PIXI.Color.shared.setValue(v).toNumber()&0xFFFFFF;}catch(e){return 0xFFFFFF;}
    };
    var __markRender = function(o){
      if(!o || o._destroyed) return;
      // Particle children are packed into their container's one native multiquad node. Redirect
      // every child transform/texture/tint invalidation to that owner so no per-particle native
      // node is rebuilt or traversed.
      var pp=o.parent;
      if(pp&&PIXI.ParticleContainer&&pp instanceof PIXI.ParticleContainer)o=pp;
      __renderDirty.add(o); globalThis.__sgDirty=1;
      // 带 idx 的 bounds 提示(去重后近零成本):否则"仅 BM=1 无 per-node 信息"会让 C++ 保守整树
      // 重算——detached 列表行/延迟预算对象每次 markRender 都会造出这种帧。提示永远保守安全:
      // 内容尚未写入时多算一次该链,flush 真正写入后(队列翻代)会再次入队。
      if(o.__i>0)__markBoundsNode(o.__i); else SGB[BM]=1;
    };
    // Spine can start while its own node or an ancestor is hidden. When that subtree becomes
    // visible again, force the current AnimationState pose into Skeleton before native emits it.
    // Otherwise native can submit setup pose/no attachments for one frame.
    // Pixi 真屏路径会在每次 Sprite.calculateVertices() 时比较 Texture._updateID，UV 变更可自动自愈。
    // native 主屏短路了 calculateVertices，因此显式维护 Texture -> Sprite 和 BaseTexture -> native 图元
    // 的消费者关系。既用于 UV 定向回写，也用于跨场景缓存对象的资源保活，避免退化成逐帧全树扫描。
    var __invalidateBaseTextureOwners = function(bt){
      var owners=bt&&bt.__sgTextureOwners;if(!owners||!owners.size)return;
      owners.forEach(function(o){
        if(!o||o._destroyed)return;
        // A Spine plan stores native texture handles.  BaseTexture can become valid after the
        // first plan was submitted (async atlas/KTX decode); wake that plan immediately instead
        // of waiting for its bounded missing-resource retry.
        if(o.skeleton){o.__spRenderPlanKey=null;o.__spRenderRetryFrame=0;o.__spRenderPendingAttempts=0;}
        // Lightweight render proxies may have committed layout while this page texture was still
        // awaiting decode/upload. Give them a generic readiness callback before queueing the next
        // visual write; BitmapText uses it to reset its bounded retry counter without knowing
        // anything about the machine or asset that supplied the font page.
        if(typeof o.__nativeTextureReady==='function')o.__nativeTextureReady(bt);
        __markRender(o);
      });
    };
    var __addBaseTextureOwner = function(bt,o){
      if(!bt||!o)return;
      if(!bt.__sgTextureOwners)bt.__sgTextureOwners=new Set();
      if(!o.__sgOwnedBaseTextures)o.__sgOwnedBaseTextures=new Set();
      if(!bt.__sgNativeOwnerUpdateHook&&typeof bt.on==='function'){
        bt.__sgNativeOwnerUpdateHook=function(){__invalidateBaseTextureOwners(bt);};
        bt.on('update',bt.__sgNativeOwnerUpdateHook);
      }
      if(o.__sgOwnedBaseTextures.has(bt))return;
      bt.__sgTextureOwners.add(o);o.__sgOwnedBaseTextures.add(bt);
    };
    var __removeBaseTextureOwner = function(bt,o){
      if(!bt||!o)return;
      if(bt.__sgTextureOwners)bt.__sgTextureOwners.delete(o);
      if(o.__sgOwnedBaseTextures)o.__sgOwnedBaseTextures.delete(bt);
      if(globalThis.__sgTryFinalizeTextureResource)globalThis.__sgTryFinalizeTextureResource(bt);
    };
    var __addTextureOwner = function(tex,o){
      if(!tex||!o)return;
      if(!tex.__sgSpriteOwners)tex.__sgSpriteOwners=new Set();tex.__sgSpriteOwners.add(o);
      __addBaseTextureOwner(tex.baseTexture,o);
    };
    var __removeTextureOwner = function(tex,o){
      if(!tex||!o)return;
      if(tex.__sgSpriteOwners)tex.__sgSpriteOwners.delete(o);
      __removeBaseTextureOwner(tex.baseTexture,o);
      if(globalThis.__sgTryFinalizeTextureResource)globalThis.__sgTryFinalizeTextureResource(tex);
    };
    var __replaceTextureOwner = function(oldTex,newTex,o){
      if(oldTex===newTex)return;
      if(oldTex&&oldTex.__sgSpriteOwners)oldTex.__sgSpriteOwners.delete(o);
      if(newTex){if(!newTex.__sgSpriteOwners)newTex.__sgSpriteOwners=new Set();newTex.__sgSpriteOwners.add(o);}
      var oldBT=oldTex&&oldTex.baseTexture,newBT=newTex&&newTex.baseTexture;
      if(oldBT!==newBT){__removeBaseTextureOwner(oldBT,o);__addBaseTextureOwner(newBT,o);}
      if(oldTex&&globalThis.__sgTryFinalizeTextureResource)globalThis.__sgTryFinalizeTextureResource(oldTex);
    };
    var __releaseOwnedBaseTextures = function(o){
      var owned=o&&o.__sgOwnedBaseTextures;if(!owned||!owned.size)return;
      var list=Array.from(owned);owned.clear();
      for(var i=0;i<list.length;i++){var bt=list[i];if(bt&&bt.__sgTextureOwners)bt.__sgTextureOwners.delete(o);if(globalThis.__sgTryFinalizeTextureResource)globalThis.__sgTryFinalizeTextureResource(bt);}
    };
    var __treeVisible=function(o){ for(var p=o;p;p=p.parent)if(p.visible===false||p.renderable===false)return false; return true; };
    // TYPE 写闸(E-2 收口):跨 T_SPINE_SLOT 边界=local 矩阵来源切换,必须置 bit2+mark,
    // 否则 LA 骨骼缓存成为无主第二真值(E-1 可见族凝固实锤)。非跨界写=纯渲染标注,裸写零成本。
    // CI 扫描(native_type_gateway_scan_test)禁止本文件再出现裸 oTYPE]= 写(/*type-gateway*/ 行豁免)。
    var __sgTypeW = function(b,v){
      var old=SGB[b+oTYPE],id=(b/ST)|0;
      // A live spinex slot lease owns both TYPE and LA..LTY. addChild/visual invalidation can put
      // the same container in the ordinary render-data queue after C++ has already published its
      // slot matrix. Letting that later writer cross 4 -> 0 exposes local (0,0) at the Spine root
      // for one frame. Unbind releases the generation-checked lease first, after which this same
      // gateway performs the normal source transition and bit2 rebuild.
      if(old===4&&v!==4&&typeof globalThis.__spxOwnsSlotType==='function'&&
         globalThis.__spxOwnsSlotType(__sgObjs[id])){
        globalThis.__sgSpineTypeWriteBlocks=(globalThis.__sgSpineTypeWriteBlocks|0)+1;
        return;
      }
      SGB[b+oTYPE]=v; /*type-gateway*/
      if((old===4)!==(v===4)){ SGB[PDB+id]=(SGB[PDB+id]|0)|2; __markD(b); }
    };
    globalThis.__sgTypeW=function(b,v){ __sgTypeW(b,v); };   // node_proxy 等外闭包统一入口
    // Phase D 硬指标计数器(终局验收口径,心跳行输出):
    // __sgLateMut   = seal(阶段6)后、present 前仍发生的权威 mark(理论恒 0;阶段7 写归下帧合法不计)
    // __sgUnsealedPresent = present 时队列非空或根债务>0.5(未封存就画,理论恒 0)
    // __sgPhaseRegress    = JS 帧阶段回退(3→4→5→6 必须单调,理论恒 0)
    var __sgPh = function(v){ var old=SGB[PHB]|0; if(v<old&&old<7)globalThis.__sgPhaseRegress=(globalThis.__sgPhaseRegress|0)+1; SGB[PHB]=v; };
    var __markD = function(b){   // b = 节点 buffer 偏移(idx*ST)或 transform.__b
      if((SGB[PHB]|0)===6)globalThis.__sgLateMut=(globalThis.__sgLateMut|0)+1;
      // bounds 队列同步入队:world 将变 → AABB 将变。若不入队,present 前的 updateBounds
      // (spine 驱动闸门/hitTest)只看到"BM=1 且无 per-node 信息",会保守整树重算。
      __markBoundsNode((b/ST)|0);
      // Phase A-2 溯源(migration-plan §11 Phase A):__SG_TRACE_DIRTY=1 时按调用栈签名聚合计数,
      // 并统计同帧同节点重复 mark(区分真实变化 vs 幂等重复,定位 [sg-atomic] dirty=154 生产者)。
      // 采样式:不改几十处调用点签名;默认关,零开销。dump 用 __sgDirtyTraceDump()。
      if (globalThis.__SG_TRACE_DIRTY === 1){
        var __tk;try{__tk=String((new Error()).stack||'').split('\n').slice(2,5).join(' | ').replace(/@.*?native_scenegraph[^:]*:/g,'@');}catch(e){__tk='?';}
        var __tm=globalThis.__sgDirtyTrace||(globalThis.__sgDirtyTrace={});__tm[__tk]=(__tm[__tk]||0)+1;
        var __fr=globalThis.__frameNo|0,__nid=(b/ST)|0;
        var __nt=globalThis.__sgDirtyNodeTrace||(globalThis.__sgDirtyNodeTrace={f:-1,m:null,rep:0});
        if(__nt.f!==__fr){__nt.f=__fr;__nt.m={};__nt.rep=0;}
        if(__nt.m[__nid])__nt.rep++;__nt.m[__nid]=(__nt.m[__nid]||0)+1;
      }
      var __mi=(b/ST)|0;
      SGB[MPB+__mi]=SGB[PHB];   // B0-1:记录 local 脏的写入阶段(JS 写入方)
      var __wasClean=!((SGB[PDB+__mi]|0)&1);
      __persistMarkDirty(__mi);   // B0-2:持久位+债务+epoch(位为正确性,队列为加速)
      if(!__wasClean){ if (globalThis.__SG_DIRTY === 0){ SGB[DB] = DCAP + 1; } return; }   // B0-3 ⑤:位已置=队列已有候选,重复写不重复入队(canonical 清位后再写自动重新入队)
      if (globalThis.__SG_DIRTY === 0){ SGB[DB] = DCAP + 1; return; }
      var c = Math.floor(SGB[DB]);
      if (c <= DCAP - 1) { SGB[DB + 1 + c] = b / ST; SGB[DB] = c + 1 + (globalThis.__SG_DIRTY === 2 ? 0.5 : 0); }
      else SGB[DB] = DCAP + 1;   // 溢出=全脏(对拍标记随之丢弃,全脏帧无需对拍)
    };
    // 溯源 dump:top 栈签名(总量)+当前帧重复节点。配合 [sg-atomic] 警告帧使用:
    // 警告出现时 __SG_TRACE_DIRTY=1 再复现,dump 直接给出 dirty=N 的生产者构成。
    globalThis.__sgDirtyTraceDump=function(topN){
      topN=topN|0||10;var tm=globalThis.__sgDirtyTrace||{},ks=Object.keys(tm);
      ks.sort(function(a,b2){return tm[b2]-tm[a];});
      __log('[sg-trace] 栈签名 '+ks.length+' 种,top'+Math.min(topN,ks.length)+':');
      for(var i=0;i<Math.min(topN,ks.length);i++)__log('[sg-trace] ×'+tm[ks[i]]+' '+ks[i].slice(0,200));
      var nt=globalThis.__sgDirtyNodeTrace;if(nt&&nt.m){var ns=Object.keys(nt.m);ns.sort(function(a,b2){return nt.m[b2]-nt.m[a];});
        var line='';for(var j=0;j<Math.min(8,ns.length);j++)line+=' i'+ns[j]+'×'+nt.m[ns[j]];
        __log('[sg-trace] 帧'+nt.f+' 节点数'+ns.length+' 帧内重复'+nt.rep+' top:'+line);}
      return ks.length;
    };
    // A numeric slot offset is not a write capability: destroyed Pixi shells and delayed animation
    // callbacks can retain it after SceneGraph has reused the slot.  Every shared-slot publisher
    // must prove that the semantic owner is still the authoritative entry in __sgObjs.
    var __sgOwnsSlot=function(owner,off){var id=off===undefined?-1:((off/ST)|0);return !!owner&&owner.__i===id&&__sgObjs[id]===owner;};
    var __sgDropStaleSlotWrite=function(owner,off,kind){
      globalThis.__sgStaleSlotWriteDrops=(globalThis.__sgStaleSlotWriteDrops||0)+1;
      if(globalThis.__sgStaleSlotWriteDrops<=4){var id=off===undefined?-1:((off/ST)|0);
        __log('[sg-guard] 已丢弃非当前所有者的延迟'+kind+'写入 id='+id+' owner='+((owner&&(owner.name||(owner.constructor&&owner.constructor.name)))||'?'));}
    };
