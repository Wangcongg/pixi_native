    var _icdo = DOp._initCachedDisplayObject;
    if (_icdo) DOp._initCachedDisplayObject = function(){ if(globalThis.__SG<2) return _icdo.apply(this,arguments); return __inBD(this,_icdo,arguments); };
    // _renderCached:缓存 sprite 与宿主共享同一个 _wt Matrix 引用,靠"读宿主 worldTransform getter"从 buffer 刷新;
    // mode 2 下渲染不再读宿主的 getter,这里每帧主动触发一次,否则缓存 sprite 停在旧位置。
    var _rcd = DOp._renderCached;
    if (_rcd) DOp._renderCached = function(renderer){ if(globalThis.__SG>=2 && this.__b!==undefined) void this.transform.worldTransform; return _rcd.call(this, renderer); };
    if (PIXI.Renderer && PIXI.Renderer.prototype.generateTexture){ var _gt = PIXI.Renderer.prototype.generateTexture;
      PIXI.Renderer.prototype.generateTexture = function(){ if(globalThis.__SG<2) return _gt.apply(this,arguments); return __inBD(this,_gt,arguments); }; }
    // 主屏整体交 native 合批器。JS 每帧只驱动 Spine 状态、同步 dirty render data 并提交 root。
    // TYPE=2 是 coverage 失败标记：native 只计数并跳过，不存在回调 Pixi 真屏的通道。
    // (7b) mode 4 静止帧跳过(精修版):只有整棵场景树本帧无变化时才允许跳过"画"。
    //   关键原则:只跳"画",不跳"算" —— compute+回写照常(游戏逻辑读 worldTransform/toGlobal 永远新鲜),
    //   回写变化数兼作第二道脏检测(兜住绕过置脏 hook 的矩阵变化);新 spine(未初始化)照常驱动,驱动即不跳。
    //   净省:渲染树遍历+batch+命令编码/解码+draw+present。__SKIPF=0 关。
    globalThis.__SKIPF = 0;   // 实测大厅每帧 9-28 节点变化(UI 动画常驻),静止帧不存在 → 跳帧无收益,关闭
    // 全局关 mipmap(__MIPMAP=1 恢复):pixi v7 对 POT 纹理(4096 图集)默认生成 mipmap 链,
    if (globalThis.__MIPMAP !== 1 && PIXI.BaseTexture && PIXI.BaseTexture.defaultOptions){
      try { PIXI.BaseTexture.defaultOptions.mipmap = PIXI.MIPMAP_MODES.OFF; } catch(e){}
    }
    if (__SG_MODE===4 && PIXI.Renderer){
      var _rr4 = PIXI.Renderer.prototype.render;
      PIXI.Renderer.prototype.render = function(dO, opt){
        var rt = opt && opt.renderTexture;
        if (globalThis.__SG===4 && !rt && !globalThis.__sgBD){
          var fc = (globalThis.__sgFc = ((globalThis.__sgFc||0)+1)|0);
          if ((fc&1)===1 && !globalThis.__sgDirty && globalThis.__SKIPF===1 && dO && dO.__i>0){
            __sgCanonicalBarrier(dO.__i);                         // "算"照常(canonical 原子入口,D-1 同族)
            var wb = 1;
            var drove = 0;
            __ov.forEach(function(o){ if(!o.__ovInit && o.__i>0&&__linkedToRoot(o,dO)){
              if(!__treeVisible(o))return;
              try{ o.updateTransform(); o.__ovInit=1; drove++; }catch(e){} } });
            if (wb===0 && !drove && !globalThis.__sgDirty){       // 三重确认:回写无变化+无新 spine+期间无置脏
              globalThis.__skipN=(globalThis.__skipN||0)+1;
              try{ this.runners.prerender.emit(); this.emit('prerender'); this.runners.postrender.emit(); this.emit('postrender'); }catch(e){}   // 游戏 renderNum 依赖
              return;
            }
          }
          globalThis.__sgDirty = 0;   // 本次渲染将反映当前全部状态
        }
        if (!rt){   // 主屏渲染当帧耗时(badframe 用 raf-render 拆出游戏逻辑占比)
          var __rt0 = performance.now();
          try { return _rr4.call(this, dO, opt); }
          finally { globalThis.__renderMs = performance.now() - __rt0; }
        }
        return _rr4.call(this, dO, opt);
      };
    }
    // (mode3 render-flag 补丁块已删除——Phase F-8,同上死代码。)
    // 主屏没有 JS escape 回调。T_ESCAPE 只作为 native coverage 失败标记，必须补 native 能力后归零。
    globalThis.__sgBind = __bind; globalThis.__SGB = SGB; globalThis.__SG_ST = ST;
    globalThis.__sgDumpVisibleEscapes=function(root){
      var out=[];for(var i=1;i<__sgObjs.length&&out.length<40;i++){var o=__sgObjs[i];if(!o||o.__i!==i||o._destroyed||SGB[i*ST+oTYPE]!==2||!__treeVisible(o))continue;
        var p=o,g=0,linked=!root;while(p&&g++<256){if(p===root){linked=true;break;}p=p.parent;}if(!linked)continue;
        out.push('#'+i+' '+((o.constructor&&o.constructor.name)||'?')+' name="'+(o.name||'')+'" reason='+(o.__sgEscapeReason||'?')+' kids='+(o.children?o.children.length:0)+' mask='+__maskDesc(o._mask&&o._mask.maskObject?o._mask.maskObject:o._mask));}
      __log('[native-escapes] count='+out.length+(out.length?'\n'+out.join('\n'):''));return out.length;
    };
    // Deterministic native-coverage gate. Unlike the old diagnostic dump this returns every
    // counter needed by an automated cold-start run and never truncates the failure count.
    globalThis.__nativeCoverageAudit=function(name,root,strict){
      root=root||((globalThis.__PIXI_APP__||globalThis.__pixiApp||{}).stage);name=String(name||'checkpoint');
      var leanStats=globalThis.__nativeLeanStats||{},OriginalPixiText=globalThis.__NativeOriginalPixiText,__spJsApplyTotal=globalThis.__spAnimJSApplyFrames||0,__spJsApplyBaseline=globalThis.__nativeCoverageSpineJSApplyBaseline||0,__spJsStateTotal=globalThis.__spStateJSUpdateFrames||0,__spJsStateBaseline=globalThis.__nativeCoverageSpineJSStateBaseline||0;
      var result={name:name,visible:0,native:0,containers:0,escapes:0,snapshots:0,missingTextures:0,textVisible:0,bitmapTextVisible:0,textSyncFailures:0,
        nativeDynamicTexts:Number(leanStats.dynamicTextsLive)||0,nativeDynamicTextVisible:0,nativeDynamicContainers:Number(leanStats.dynamicContainersLive)||0,nativeDynamicContainerVisible:0,nativeApplicationStages:Number(leanStats.nativeApplicationStages)||0,nativeDynamicSprites:Number(leanStats.dynamicSpritesLive)||0,nativeDynamicSpriteVisible:0,nativeDynamicAnimatedSprites:Number(leanStats.dynamicAnimatedSpritesLive)||0,nativeDynamicAnimatedSpriteVisible:0,nativeDynamicTilings:Number(leanStats.dynamicTilingsLive)||0,nativeDynamicTilingVisible:0,nativeDynamicNineSlices:Number(leanStats.dynamicNineSlicesLive)||0,nativeDynamicNineSliceVisible:0,nativeDynamicGraphics:Number(leanStats.dynamicGraphicsLive)||0,nativeDynamicGraphicsVisible:0,nativeDynamicMeshes:Number(leanStats.dynamicMeshesLive)||0,nativeDynamicMeshVisible:0,nativeParticleContainers:Number(leanStats.particleContainersLive)||0,nativeParticleContainerVisible:0,retainedPixiTexts:0,retainedTextCanvases:0,retainedTextTextures:0,
        retainedPixiGraphics:0,retainedPixiSprites:0,retainedPixiAnimatedSprites:0,retainedPixiNineSlices:0,retainedPixiMeshes:0,retainedPixiContainers:0,retainedRichTextRoots:0,retainedSpineRuntimeObjects:0,retainedSpineRendererFacades:0,spineRendererFacadesPruned:(globalThis.__spineFacadesPruned||0),retainedTypes:{},retainedSamples:[],
        fguiFallbacks:((globalThis.__nativeFguiFactoryStats||{}).fallbacks|0),fguiFallbackSamples:((globalThis.__nativeFguiFactoryStats||{}).fallbackSamples||[]).slice(-16),rejectedPixiRenders:Number(leanStats.rejectedPixiRenders)||0,rejectedPixiRenderSamples:(leanStats.rejectedPixiRenderSamples||[]).slice(-16),fguiNativeNodes:0,fguiNativeTextNodes:0,fguiNativeTextFailures:0,fguiManagedFreeDeferrals:0,fguiSemanticChildRemaps:Number(leanStats.fguiSemanticChildRemaps)||0,nativeBatchSerial:0,nativeBatchEscapes:0,
        nativeBatchMaskEscapes:0,nativeBatchTypeEscapes:0,nativeBatchTextureEscapes:0,nativeDraws:0,nativeQuads:0,
        hierarchyMismatches:0,parentlessFacades:0,spinesVisible:0,spinesNative:0,spineComputeFallbacks:0,spineJSAnimationStates:0,spineJSActiveTracks:0,
        spineNativeAnimationFrames:(globalThis.__spAnimNativeFrames||0),spineNativeFastTrackFrames:(globalThis.__spAnimNativeFastFrames||0),spineNativeProgramFrames:(globalThis.__spAnimNativeProgramFrames||0),spineNativeMixFrames:(globalThis.__spAnimNativeMixFrames||0),spineNativeStateFrames:(globalThis.__spStateNativeFrames||0),spineNativeStateGraphFrames:(globalThis.__spStateNativeGraphFrames||0),spineJSStateFrames:__spJsStateTotal,spineJSStateSinceCheckpoint:Math.max(0,__spJsStateTotal-__spJsStateBaseline),spineJSStateReasons:globalThis.__spStateJSUpdateReasons||{},spineRenderPlanBuilds:(globalThis.__spRenderPlanBuilds||0),spineRenderPlanTopologyBuilds:(globalThis.__spRenderPlanTopologyBuilds||0),spineRenderPlanInitialBuilds:(globalThis.__spRenderPlanInitialBuilds||0),spineRenderPlanInstanceBuilds:(globalThis.__spRenderPlanInstanceBuilds||0),spineRenderPlanVersionBuilds:(globalThis.__spRenderPlanVersionBuilds||0),spineRenderPlanSlotBuilds:(globalThis.__spRenderPlanSlotBuilds||0),spineRenderPlanPatchBatches:(globalThis.__spRenderPlanPatchBatches||0),spineRenderPlanPatches:(globalThis.__spRenderPlanPatches||0),spineRenderPlanPatchPending:(globalThis.__spRenderPlanPatchPending||0),spineRenderPlanPatchFailures:(globalThis.__spRenderPlanPatchFailures||0),spineRenderPlanPendingBuilds:(globalThis.__spRenderPlanPendingBuilds||0),spineRenderPlanRetries:(globalThis.__spRenderPlanRetries||0),spineRenderPlanGeometryPending:(globalThis.__spRenderPlanGeometryPending||0),spineRenderPlanTexturePending:(globalThis.__spRenderPlanTexturePending||0),spineRenderTopologyReasons:globalThis.__spRenderTopologyReasons||{},spineRenderTopologyOwners:globalThis.__spRenderTopologyOwners||{},spineDeepMixFallback:(globalThis.__spAnimDeepMixFallback||0),spineJSApplyFrames:__spJsApplyTotal,spineJSApplySinceCheckpoint:Math.max(0,__spJsApplyTotal-__spJsApplyBaseline),spineJSApplyReasons:globalThis.__spAnimJSApplyReasons||{},
        spineNativeTreeMarks:(globalThis.__spNativeTreeMarks||0),spineNativeTreeScans:(globalThis.__spNativeTreeScans||0),fguiNativePaneScans:(globalThis.__fguiNativePaneScans||0),fguiPaneDirty:(globalThis.__fguiPaneDirtyCount||0),reasons:{},samples:[]};
      var linked=function(o){return !root||__linkedToRoot(o,root);};
      for(var i=1;i<__sgObjs.length;i++){
        var o=__sgObjs[i];if(!o||o.__i!==i||o._destroyed||!linked(o)||!__treeVisible(o))continue;
        var b=i*ST,type=SGB[b+oTYPE]|0;if(o.renderable===false&&type!==0)continue;result.visible++;
        var isText=!!o.__nativeTextProxy||!!(PIXI.Text&&o instanceof PIXI.Text),ownerText=o.UIOwner&&o.UIOwner.text,objectText=o._text!==undefined?o._text:o.text;
        if(o.__nativeDynamicContainer)result.nativeDynamicContainerVisible++;
        if(o.__nativeDynamicSprite)result.nativeDynamicSpriteVisible++;
        if(o.__nativeDynamicAnimatedSprite)result.nativeDynamicAnimatedSpriteVisible++;
        if(o.__nativeDynamicTiling)result.nativeDynamicTilingVisible++;
        if(o.__nativeDynamicNineSlice)result.nativeDynamicNineSliceVisible++;
        if(o.__nativeDynamicGraphics)result.nativeDynamicGraphicsVisible++;
        if(o.__nativeDynamicMesh)result.nativeDynamicMeshVisible++;
        if(o.__nativeParticleContainer)result.nativeParticleContainerVisible++;
        if(o.__nativeDynamicText){result.nativeDynamicTextVisible++;if(o.canvas||o._canvas)result.retainedTextCanvases++;if(o._texture||o.texture)result.retainedTextTextures++;}
        else if(OriginalPixiText&&o instanceof OriginalPixiText){result.retainedPixiTexts++;if(o.canvas||o._canvas)result.retainedTextCanvases++;if(o._texture||o.texture)result.retainedTextTextures++;}
        if(!o.__nativeNodeProxy){var retainedName=(o.constructor&&o.constructor.name)||'?',spineRendererFacade=/_SlotMesh/i.test(retainedName),spineRuntime=!!o.skeleton||spineRendererFacade||/Spine/i.test(retainedName)||!!o.__spTree||!!o.__nativeSpineOwner;result.retainedTypes[retainedName]=(result.retainedTypes[retainedName]||0)+1;if(spineRuntime)result.retainedSpineRuntimeObjects++;if(spineRendererFacade)result.retainedSpineRendererFacades++;
          if(result.retainedSamples.length<32)result.retainedSamples.push({id:i,type:retainedName,name:o.name||'',children:o.children?o.children.length:0,sprite:!!(PIXI.Sprite&&o instanceof PIXI.Sprite),graphics:!!(PIXI.Graphics&&o instanceof PIXI.Graphics),spine:!!o.skeleton});
          if(PIXI.NineSlicePlane&&o instanceof PIXI.NineSlicePlane)result.retainedPixiNineSlices++;
          if(PIXI.Mesh&&o instanceof PIXI.Mesh)result.retainedPixiMeshes++;
          if(PIXI.Graphics&&o instanceof PIXI.Graphics)result.retainedPixiGraphics++;
          else if(PIXI.Sprite&&o instanceof PIXI.Sprite){result.retainedPixiSprites++;if(PIXI.AnimatedSprite&&o instanceof PIXI.AnimatedSprite)result.retainedPixiAnimatedSprites++;}
          else if(PIXI.Container&&o instanceof PIXI.Container&&!spineRuntime)result.retainedPixiContainers++;
          if(/TaggedText|MultiStyleText|RichText/i.test(retainedName)||(o._tagStyles&&o._tokens&&o._textContainer&&o._spriteContainer))result.retainedRichTextRoots++;
        }
        if(isText){
          var expectedText=String(ownerText==null?objectText:ownerText),actualText=String(objectText==null?'':objectText);
          if(expectedText.replace(/\s/g,'').length){
            result.textVisible++;
            if(o.__nativeTextProxy&&typeof o._syncNativeText==='function')try{o._syncNativeText(false);type=SGB[b+oTYPE]|0;}catch(_txe){}
            if(!actualText.replace(/\s/g,'').length||type!==1||!(SGB[b+oTEXID]>0)){
              result.textSyncFailures++;var tr='text-sync';result.reasons[tr]=(result.reasons[tr]||0)+1;
              if(result.samples.length<24)result.samples.push({id:i,type:(o.constructor&&o.constructor.name)||'Text',name:o.name||'',reason:tr,expected:expectedText.slice(0,80),actual:actualText.slice(0,80),nativeType:type,texture:SGB[b+oTEXID]|0});
            }
          }
        }
        if(o.__nativeBitmapText){
          var bitmapExpected=String(o.UIOwner&&o.UIOwner.text!=null?o.UIOwner.text:(o._text==null?'':o._text));
          if(bitmapExpected.replace(/\s/g,'').length){result.bitmapTextVisible++;if(type!==5||!(SGB[b+oQUADCNT]>0)||String(o.__nativeBitmapSourceText||'')!==bitmapExpected){result.textSyncFailures++;var br='bitmap-text-sync';result.reasons[br]=(result.reasons[br]||0)+1;if(result.samples.length<24)result.samples.push({id:i,type:'NativeBitmapText',name:o.name||'',reason:br,expected:bitmapExpected.slice(0,80),actual:String(o.__nativeBitmapSourceText||'').slice(0,80),nativeType:type,quads:SGB[b+oQUADCNT]|0});}}
        }
        if(type===0)result.containers++;else result.native++;
        if(type===2){var reason=String(o.__sgEscapeReason||'unknown'),sampleMask=o._mask&&o._mask.maskObject?o._mask.maskObject:o._mask;result.escapes++;result.reasons[reason]=(result.reasons[reason]||0)+1;
          if(result.samples.length<24)result.samples.push({id:i,type:(o.constructor&&o.constructor.name)||'?',name:o.name||'',reason:reason,children:o.children?o.children.length:0,mask:__maskDesc(sampleMask),maskId:sampleMask&&sampleMask.__i|0,maskType:(sampleMask&&sampleMask.constructor&&sampleMask.constructor.name)||'',maskNative:!!(sampleMask&&sampleMask.__nativeNodeProxy)});}
        if(type===1&&!(SGB[b+oTEXID]>0)){result.missingTextures++;var mr='missing-texture';result.reasons[mr]=(result.reasons[mr]||0)+1;
          if(result.samples.length<24)result.samples.push({id:i,type:(o.constructor&&o.constructor.name)||'?',name:o.name||'',reason:mr});}
        if(typeof globalThis.__sgNativeParent==='function'){var actualParent=__sgNativeParent(i)|0;if(o.__nativeParentlessFacade){result.parentlessFacades++;if(__nativeParentOf(i)!==actualParent){result.hierarchyMismatches++;var cr='native-parent-cache';result.reasons[cr]=(result.reasons[cr]||0)+1;if(result.samples.length<24)result.samples.push({id:i,type:(o.constructor&&o.constructor.name)||'?',name:o.name||'',reason:cr,cachedParent:__nativeParentOf(i),nativeParent:actualParent});}}
          else{var jp=o.parent,jg=0;while(jp&&!(jp.__i>0)&&jg++<256)jp=jp.parent;var expectedParent=jp?(jp.__i|0):-1;if(actualParent!==expectedParent){result.hierarchyMismatches++;var hr='native-parent';result.reasons[hr]=(result.reasons[hr]||0)+1;if(result.samples.length<24)result.samples.push({id:i,type:(o.constructor&&o.constructor.name)||'?',name:o.name||'',reason:hr,jsParent:expectedParent,nativeParent:actualParent});}}}
      }
      // A native leaf-only audit is insufficient for Spine: the final pixels may be submitted by
      // C++ while Skeleton.updateWorldTransform silently falls back to the official JS runtime.
      // Audit visible linked roots separately and make strict checkpoints reject that hidden work.
      __ov.forEach(function(o){if(!o||o._destroyed||!o.skeleton||!linked(o)||!__treeVisible(o))return;result.spinesVisible++;
        var state=o.state,tracks=state&&state.tracks;if(state){result.spineJSAnimationStates++;if(tracks)for(var ti=0;ti<tracks.length;ti++)if(tracks[ti])result.spineJSActiveTracks++;}
        var sk=o.skeleton,sid=sk.__skelId|0;if(sid>0){result.spinesNative++;return;}
        result.spineComputeFallbacks++;var sr='spine-'+String(sk.__skelFallbackReason||((globalThis.__SPSKEL|0)===0?'disabled':'unregistered'));
        result.reasons[sr]=(result.reasons[sr]||0)+1;if(result.samples.length<24)result.samples.push({id:o.__i|0,type:(o.constructor&&o.constructor.name)||'Spine',name:o.name||((sk.data&&sk.data.name)||''),reason:sr});
      });
      if(typeof globalThis.__nativeFguiRuntimeStats==='function')try{var fs=__nativeFguiRuntimeStats();if(fs){result.fguiNativeNodes=Number(fs.nodes)||0;result.fguiNativeTextNodes=Number(fs.nonEmptyTextNodes)||0;result.fguiNativeTextFailures=Number(fs.textFailures)||0;result.fguiManagedFreeDeferrals=Number(fs.managedFreeDeferrals)||0;if(result.fguiNativeTextFailures){result.reasons['fgui-native-text']=(result.reasons['fgui-native-text']||0)+result.fguiNativeTextFailures;if(result.samples.length<24)result.samples.push({type:'NativeFGUI',reason:'fgui-native-text',failures:result.fguiNativeTextFailures,bitmapFailures:Number(fs.bitmapTextFailures)||0,details:fs.failureSamples||[]});}}}catch(_fse){}
      if(typeof globalThis.__nativeFguiCompatibilityIssues==='function')try{result.fguiCompatibilityIssues=__nativeFguiCompatibilityIssues()||[];}catch(_fgci){result.fguiCompatibilityIssues=[{reason:'census-error:'+String(_fgci&&_fgci.message||_fgci)}];}
      if(typeof globalThis.__nativeFguiPreparseStats==='function')try{result.fguiPreparse=__nativeFguiPreparseStats();}catch(_fgp){}
      if(typeof globalThis.__nativeResourceStats==='function')try{result.nativeResources=__nativeResourceStats();result.nativeResources.adoptions=globalThis.__nativeResourceAdoptions||0;result.nativeResources.releases=globalThis.__nativeResourceReleases||0;result.nativeResources.staleHandles=globalThis.__nativeResourceStaleHandles||0;result.nativeResources.imgAdopts=globalThis.__nativeImgAdopts||0;result.nativeResources.imgCancels=globalThis.__nativeImgCancels||0;result.nativeResources.imgDisowns=globalThis.__nativeImgDisowns||0;result.nativeResources.imgTransfers=globalThis.__nativeImgTransfers||0;}catch(_nre){}
      if(typeof globalThis.__nativeSpinePreparseStats==='function')try{result.spinePreparse=__nativeSpinePreparseStats();}catch(_spp){}
      result.spineLoader=globalThis.__nativeSpineLoaderStats||{};
      result.fguiListAuthoredHandlesAvoided=Number((globalThis.__nativeFguiFactoryStats||{}).listAuthoredHandlesAvoided)||0;
      result.fguiListFirstHitMaterialisations=Number((globalThis.__nativeFguiFactoryStats||{}).listHitMaterialisations)||0;
      if(typeof globalThis.__nativeBatchCoverage==='function'){try{var bc=globalThis.__nativeBatchCoverage();if(bc&&bc.length>=7){result.nativeBatchSerial=Number(bc[0])||0;result.nativeBatchEscapes=Number(bc[1])||0;result.nativeBatchMaskEscapes=Number(bc[2])||0;result.nativeBatchTypeEscapes=Number(bc[3])||0;result.nativeBatchTextureEscapes=Number(bc[4])||0;result.nativeDraws=Number(bc[5])||0;result.nativeQuads=Number(bc[6])||0;}}catch(e){}}
      result.pass=result.escapes===0&&result.missingTextures===0&&result.textSyncFailures===0&&result.fguiNativeTextFailures===0&&result.fguiFallbacks===0&&result.rejectedPixiRenders===0&&result.nativeBatchEscapes===0&&result.nativeBatchTextureEscapes===0&&(!strict||(result.snapshots===0&&result.hierarchyMismatches===0&&result.spineComputeFallbacks===0&&result.spineJSApplySinceCheckpoint===0&&result.spineJSStateSinceCheckpoint===0&&result.retainedPixiTexts===0&&result.retainedTextCanvases===0&&result.retainedTextTextures===0&&result.retainedPixiGraphics===0&&result.retainedPixiSprites===0&&result.retainedPixiAnimatedSprites===0&&result.retainedPixiNineSlices===0&&result.retainedPixiMeshes===0&&result.retainedPixiContainers===0&&result.retainedRichTextRoots===0&&result.retainedSpineRendererFacades===0));
      globalThis.__nativeCoverageSpineJSApplyBaseline=__spJsApplyTotal;
      globalThis.__nativeCoverageSpineJSStateBaseline=__spJsStateTotal;
      globalThis.__nativeCoverageLast=result;var history=globalThis.__nativeCoverageHistory||(globalThis.__nativeCoverageHistory=[]);history.push(result);
      __log('[native-coverage] '+JSON.stringify(result));
      if(!result.pass&&globalThis.__NATIVE_COVERAGE_THROW===1)throw new Error('native coverage failed at '+name+': escapes='+result.escapes+' batchEscapes='+result.nativeBatchEscapes+' batchTextures='+result.nativeBatchTextureEscapes+' snapshots='+result.snapshots+' hierarchy='+result.hierarchyMismatches+' missing='+result.missingTextures+' textSync='+result.textSyncFailures+' rejectedPixiRenders='+result.rejectedPixiRenders+' retainedPixiText='+result.retainedPixiTexts+' retainedTextCanvas='+result.retainedTextCanvases+' retainedTextTexture='+result.retainedTextTextures+' retainedGraphics='+result.retainedPixiGraphics+' retainedSprites='+result.retainedPixiSprites+' retainedAnimatedSprites='+result.retainedPixiAnimatedSprites+' retainedNineSlices='+result.retainedPixiNineSlices+' retainedMeshes='+result.retainedPixiMeshes+' retainedContainers='+result.retainedPixiContainers+' retainedRichText='+result.retainedRichTextRoots+' retainedSlotMesh='+result.retainedSpineRendererFacades+' fguiNativeText='+result.fguiNativeTextFailures+' fguiFallbacks='+result.fguiFallbacks+' spineFallbacks='+result.spineComputeFallbacks+' spineJSApply='+result.spineJSApplySinceCheckpoint+' spineJSState='+result.spineJSStateSinceCheckpoint);
      return result;
    };
    globalThis.__nativeDumpRegion=function(x0,y0,x1,y1,minW,minH){
      x0=Number(x0)||0;y0=Number(y0)||0;x1=Number(x1)||1590;y1=Number(y1)||960;minW=Number(minW)||1;minH=Number(minH)||1;
      var rootId=__qActiveRoot&&__qActiveRoot.__i|0,ids=rootId>0&&typeof globalThis.__sgNativeSubtree==='function'?__sgNativeSubtree(rootId):[],out=[];
      for(var n=0;n<ids.length;n++){var id=ids[n]|0,b=id*ST,d=BB+id*BSF+4,ax=SGB[d],ay=SGB[d+1],bx=SGB[d+2],by=SGB[d+3],w=bx-ax,h=by-ay;if(!(ax<x1&&bx>x0&&ay<y1&&by>y0&&w>=minW&&h>=minH))continue;var o=__sgObjs[id],actual=typeof globalThis.__sgNativeParent==='function'?__sgNativeParent(id)|0:__nativeParentOf(id),meta=null;
        if(!o&&typeof globalThis.__nativeFguiNode==='function'){for(var ap=actual,g=0;ap>0&&g++<64;ap=typeof globalThis.__sgNativeParent==='function'?__sgNativeParent(ap)|0:__nativeParentOf(ap)){var ao=__sgObjs[ap],owner=ao&&ao.UIOwner,state=owner&&owner.__nativeFguiState;if(state){try{meta=__nativeFguiNode(state.id,id);}catch(e){}break;}}}
        var textValue=o&&(o._text!==undefined?o._text:o.text),ownerValue=o&&o.UIOwner&&o.UIOwner.text;
        out.push('#'+id+' p='+__nativeParentOf(id)+' actual='+actual+' t='+(SGB[b+oTYPE]|0)+' tex='+(SGB[b+oTEXID]|0)+' q='+(SGB[b+oQUADCNT]|0)+' mask='+(SGB[b+oMASKI]|0)+' hf='+(SGB[b+oHITFLAGS]|0)+' local='+[SGB[b+oX],SGB[b+oY],SGB[b+oSX],SGB[b+oSY],SGB[b+oROT]].map(function(v){return Number(v).toFixed(2);}).join(',')+' world='+[SGB[b+12],SGB[b+13],SGB[b+14],SGB[b+15],SGB[b+16],SGB[b+17]].map(function(v){return Number(v).toFixed(2);}).join(',')+' rect='+[SGB[b+oRX],SGB[b+oRX+1],SGB[b+oRX+2],SGB[b+oRX+3]].map(function(v){return Number(v).toFixed(0);}).join(',')+' bounds='+[ax,ay,bx,by].map(function(v){return Number(v).toFixed(0);}).join(',')+' obj='+((o&&o.constructor&&o.constructor.name)||'-')+' name='+((o&&o.name)||'')+(textValue!==undefined?(' text='+JSON.stringify(String(textValue).slice(0,100))):'')+(ownerValue!==undefined?(' ownerText='+JSON.stringify(String(ownerValue).slice(0,100))):'')+(meta?(' meta='+(meta.kind||'?')+'/'+(meta.name||meta.id||'?')+' attrs='+JSON.stringify(meta.attrs||{})):''));}
      __log('[native-region] '+[x0,y0,x1,y1].join(',')+' count='+out.length+(out.length?'\n'+out.join('\n'):''));return out;
    };
    // Cold-start smoke helper: pick a visible native leaf by native type/bounds and exercise the
    // offscreen Twist pass without creating a PIXI.Sprite or retaining a test DisplayObject.
    globalThis.__nativeInstallTwistExercise=function(nested){
      var pick=0,bounds=null;
      for(var i=1;i<__sgObjs.length;i++){var o=__sgObjs[i];if(!o||o.__i!==i||o._destroyed||o.__nativeNodeProxy||!__treeVisible(o))continue;var b=i*ST,t=SGB[b+oTYPE]|0;if(t!==1&&t!==3&&t!==5)continue;
        try{__sgUpdateBounds(i);}catch(e){}if(globalThis.__sgWriteback)__sgWriteback();var d=BB+i*BSF+4,x0=SGB[d],y0=SGB[d+1],x1=SGB[d+2],y1=SGB[d+3],w=x1-x0,h=y1-y0;
        if(w>80&&h>80&&w<900&&h<900&&x1>4&&y1>4&&x0<1586&&y0<956){pick=i;bounds=[x0,y0,x1,y1];break;}}
      if(!pick)return null;
      var install=function(id,bb){var o=__sgObjs[id],r=Math.max(bb[2]-bb[0],bb[3]-bb[1])*.7,f={enabled:true,padding:20,uniforms:{radius:r,angle:0,offset:{x:(bb[0]+bb[2])*.5,y:(bb[1]+bb[3])*.5}}},forceOnly=!!(o&&o.__nativeNodeProxy&&(SGB[id*ST+oTYPE]|0)!==0);if(!forceOnly){var fs=o&&o.filters?o.filters.slice():[];fs.push(f);if(o)o.filters=fs;__wrFilters(o);}return {id:id,object:o,filter:f,forceOnly:forceOnly};};
      var forceDescriptor=function(t){if(!t||!(t.id>0))return;var f=t.filter,u=f.uniforms,off=FB+t.id*FSF;SGB[off+40]=1;SGB[off+41]=Number(u.radius)||0;SGB[off+42]=Number(u.angle)||0;SGB[off+43]=Number(u.offset.x)||0;SGB[off+44]=Number(u.offset.y)||0;SGB[off+45]=Number(f.padding)||0;SGB[FFB+t.id]=1;};
      var innerTest=install(pick,bounds),outer=0,outerTest=null,outerBounds=null;
      if(nested){
        var stage=(globalThis.__PIXI_APP__||globalThis.__pixiApp||{}).stage;
        if(stage&&(stage.__i|0)>0&&!stage.__nativeNodeProxy){
          outer=stage.__i|0;outerBounds=[0,0,1590,960];
          // Follow the authoritative C++ tree, not compatibility DisplayObject.children. This
          // also makes the smoke test detect hierarchy divergence instead of silently selecting
          // an object that JS still retains but Native no longer paints.
          if(typeof globalThis.__sgNativeChildCount==='function'&&typeof globalThis.__sgNativeChildAt==='function'){
            var nativeQueue=[outer],scan=0;
            while(nativeQueue.length&&scan++<1024){var parentId=nativeQueue.shift(),childN=__sgNativeChildCount(parentId)|0;
              for(var ni=0;ni<childN;ni++){var qi=__sgNativeChildAt(parentId,ni)|0;if(!(qi>0))continue;nativeQueue.push(qi);var q=__sgObjs[qi];
                if(!q||q===stage||!__treeVisible(q))continue;var qt=SGB[qi*ST+oTYPE]|0;if(qt!==0)continue;
                try{__sgUpdateBounds(qi);}catch(e){}if(globalThis.__sgWriteback)__sgWriteback();var qd=BB+qi*BSF+4,qb=[SGB[qd],SGB[qd+1],SGB[qd+2],SGB[qd+3]],qw=qb[2]-qb[0],qh=qb[3]-qb[1];
                if(qw>120&&qh>120&&qb[2]>4&&qb[3]>4&&qb[0]<1586&&qb[1]<956){innerTest=install(qi,qb);pick=qi;bounds=qb;nativeQueue.length=0;break;}
              }
            }
          }
        }else{
          var p=__sgObjs[pick]&&__sgObjs[pick].parent,guard=0;
          while(p&&guard++<64){var pi=p.__i|0;if(pi>0&&!p.__nativeNodeProxy){try{__sgUpdateBounds(pi);}catch(e){}if(globalThis.__sgWriteback)__sgWriteback();var pd=BB+pi*BSF+4,pb=[SGB[pd],SGB[pd+1],SGB[pd+2],SGB[pd+3]],pw=pb[2]-pb[0],ph=pb[3]-pb[1];if(pw>bounds[2]-bounds[0]+20&&ph>bounds[3]-bounds[1]+20&&pw<1200&&ph<850&&pb[2]>4&&pb[3]>4&&pb[0]<1586&&pb[1]<956){outer=pi;outerBounds=pb;break;}}p=p.parent;}
        }
        if(outer)outerTest=install(outer,outerBounds);
      }
      var step=0;function animate(){if(step++>=24)return;var a=Math.sin(step/24*Math.PI)*3;innerTest.filter.uniforms.angle=a;if(!innerTest.forceOnly)__wrFilters(innerTest.object);forceDescriptor(innerTest);if(outerTest){outerTest.filter.uniforms.angle=-a*.35;if(!outerTest.forceOnly)__wrFilters(outerTest.object);forceDescriptor(outerTest);}globalThis.__sgDirty=1;setTimeout(animate,50);}animate();
      __log('[twist-exercise] native #'+pick+' '+((bounds[2]-bounds[0])|0)+'x'+((bounds[3]-bounds[1])|0)+(outer?' nested-in #'+outer:'')+(nested&&!outer?' nested-parent unavailable':''));return pick;
    };
    // Cold-start smoke helper for Pixi's FXAAFilter.  Use a real visible native leaf so the
    // test covers the same bounded-FBO path as production content without loading/retaining a
    // JS filter plugin.  The synthetic descriptor is deliberately tagged instead of relying on
    // a constructor name because minified filter constructors are not stable.
    globalThis.__nativeInstallFXAAExercise=function(){
      var rootId=__qActiveRoot&&__qActiveRoot.__i|0,ids=rootId>0&&typeof globalThis.__sgNativeSubtree==='function'?__sgNativeSubtree(rootId):null;
      var pick=0,bounds=null,total=ids&&ids.length||__sgObjs.length;
      for(var n=0;n<total;n++){
        var i=ids?(ids[n]|0):n,o=__sgObjs[i];if(!(i>0)||o&&o._destroyed)continue;
        var b=i*ST,t=SGB[b+oTYPE]|0;if((t!==1&&t!==3&&t!==5)||SGB[b+oVIS]===0||SGB[b+28]===0||SGB[b+18]<=0)continue;
        try{__sgUpdateBounds(i);}catch(e){}if(globalThis.__sgWriteback)__sgWriteback();
        var d=BB+i*BSF+4,x0=SGB[d],y0=SGB[d+1],x1=SGB[d+2],y1=SGB[d+3],w=x1-x0,h=y1-y0;
        if(w>80&&h>80&&w<900&&h<900&&x1>4&&y1>4&&x0<1586&&y0<956){pick=i;bounds=[x0,y0,x1,y1];break;}
      }
      if(!pick)return null;
      var target=__sgObjs[pick],filter={enabled:true,__nativeFXAA:true,padding:1},off=FB+pick*FSF;
      if(target&&!target.__nativeNodeProxy){var fs=target.filters?target.filters.slice():[];fs.push(filter);target.filters=fs;__wrFilters(target);}
      else{SGB[off+45]=Math.max(SGB[off+45],1);SGB[off+46]=1;}
      // Keep several host frames dirty.  A cold-start popup may install the descriptor between
      // ticker/render callbacks; pulsing makes the smoke test deterministic even when the scene
      // itself has reached an otherwise static frame.
      var pulse=0;(function requestPresent(){globalThis.__sgDirty=1;if(++pulse<24)setTimeout(requestPresent,50);})();
      __log('[fxaa-exercise] native #'+pick+' '+((bounds[2]-bounds[0])|0)+'x'+((bounds[3]-bounds[1])|0)+' root=#'+rootId+' descriptor='+SGB[off+46]);
      return pick;
    };
    // Exercise the real JS DisplacementFilter descriptor bridge with an already resident sprite
    // as both target and map.  Production constructors still supply their own maskSprite/map;
    // this hook is only a deterministic cold-start validation and never creates a display node.
    globalThis.__nativeInstallDisplacementExercise=function(){
      var rootId=__qActiveRoot&&__qActiveRoot.__i|0,ids=rootId>0&&typeof globalThis.__sgNativeSubtree==='function'?__sgNativeSubtree(rootId):null;
      if(!ids)return null;var target=null,bounds=null;
      for(var n=0;n<ids.length;n++){var id=ids[n]|0,o=__sgObjs[id];if(!o||o.__i!==id||o._destroyed||o.__nativeNodeProxy||!o.isSprite||!o._texture||!o._texture.valid||!__treeVisible(o))continue;
        var b=id*ST;if((SGB[b+oTYPE]|0)!==1||SGB[b+oREND]===0)continue;try{__sgUpdateBounds(id);}catch(e){}if(globalThis.__sgWriteback)__sgWriteback();var d=BB+id*BSF+4,x0=SGB[d],y0=SGB[d+1],x1=SGB[d+2],y1=SGB[d+3],w=x1-x0,h=y1-y0;
        if(w>100&&h>100&&w<800&&h<800&&x1>4&&y1>4&&x0<1586&&y0<956){target=o;bounds=[x0,y0,x1,y1];break;}}
      if(!target)return null;
      var filter={enabled:true,__nativeDisplacement:true,maskSprite:target,map:target._texture,scale:{x:28,y:18},padding:0},fs=target.filters?target.filters.slice():[];fs.push(filter);target.filters=fs;__wrFilters(target);
      var off=FB+target.__i*FSF,pulse=0;(function requestPresent(){__wrFilters(target);globalThis.__sgDirty=1;if(++pulse<24)setTimeout(requestPresent,50);})();
      __log('[displacement-exercise] native #'+target.__i+' '+((bounds[2]-bounds[0])|0)+'x'+((bounds[3]-bounds[1])|0)+' root=#'+rootId+' map='+SGB[off+48]+' descriptor='+SGB[off+47]);
      return target.__i;
    };
    globalThis.__nativeInstallKawaseExercise=function(){
      var rootId=__qActiveRoot&&__qActiveRoot.__i|0,ids=rootId>0&&typeof globalThis.__sgNativeSubtree==='function'?__sgNativeSubtree(rootId):null;
      if(!ids)return null;var target=null,bounds=null;
      for(var n=0;n<ids.length;n++){var id=ids[n]|0,o=__sgObjs[id];if(!o||o.__i!==id||o._destroyed||o.__nativeNodeProxy||!o.isSprite||!o._texture||!o._texture.valid||!__treeVisible(o))continue;
        var b=id*ST;if((SGB[b+oTYPE]|0)!==1||SGB[b+oREND]===0)continue;try{__sgUpdateBounds(id);}catch(e){}if(globalThis.__sgWriteback)__sgWriteback();var d=BB+id*BSF+4,x0=SGB[d],y0=SGB[d+1],x1=SGB[d+2],y1=SGB[d+3],w=x1-x0,h=y1-y0;
        if(w>100&&h>100&&w<800&&h<800&&x1>4&&y1>4&&x0<1586&&y0<956){target=o;bounds=[x0,y0,x1,y1];break;}}
      if(!target)return null;
      var filter={enabled:true,__nativeKawase:true,_kernels:[4,2.6666667,1.3333333],_pixelSize:{x:1,y:1},_clamp:false,padding:10},fs=target.filters?target.filters.slice():[];fs.push(filter);target.filters=fs;__wrFilters(target);
      var off=FB+target.__i*FSF,pulse=0;(function requestPresent(){__wrFilters(target);globalThis.__sgDirty=1;if(++pulse<24)setTimeout(requestPresent,50);})();
      __log('[kawase-exercise] native #'+target.__i+' '+((bounds[2]-bounds[0])|0)+'x'+((bounds[3]-bounds[1])|0)+' root=#'+rootId+' passes='+SGB[off+62]+' descriptor='+SGB[off+61]);
      return target.__i;
    };
    globalThis.__nativeInstallGaussianExercise=function(){
      if(!PIXI.BlurFilter)return null;var rootId=__qActiveRoot&&__qActiveRoot.__i|0,ids=rootId>0&&typeof globalThis.__sgNativeSubtree==='function'?__sgNativeSubtree(rootId):null;
      if(!ids)return null;var target=null,bounds=null;
      for(var n=0;n<ids.length;n++){var id=ids[n]|0,o=__sgObjs[id];if(!o||o.__i!==id||o._destroyed||o.__nativeNodeProxy||!o.isSprite||!o._texture||!o._texture.valid||!__treeVisible(o))continue;
        var b=id*ST;if((SGB[b+oTYPE]|0)!==1||SGB[b+oREND]===0)continue;try{__sgUpdateBounds(id);}catch(e){}if(globalThis.__sgWriteback)__sgWriteback();var d=BB+id*BSF+4,x0=SGB[d],y0=SGB[d+1],x1=SGB[d+2],y1=SGB[d+3],w=x1-x0,h=y1-y0;
        if(w>100&&h>100&&w<800&&h<800&&x1>4&&y1>4&&x0<1586&&y0<956){target=o;bounds=[x0,y0,x1,y1];break;}}
      if(!target)return null;
      var filter=new PIXI.BlurFilter(8,2,1,5),fs=target.filters?target.filters.slice():[];fs.push(filter);target.filters=fs;__wrFilters(target);
      // Exercise the production invalidation path: after the initial descriptor registration no
      // test code calls __wrFilters or forces the screen dirty.  The BlurFilter setter must mark
      // its owner, and the normal dirty flush must publish each new strength.
      var off=FB+target.__i*FSF,pulse=0;(function mutateFilter(){filter.blur=2+(pulse%6)*2;if(++pulse<24)setTimeout(mutateFilter,50);else setTimeout(function(){__log('[filter-dirty-exercise] native #'+target.__i+' blur='+filter.blur+' descriptorX='+Number(SGB[off+79]).toFixed(1)+' dirty='+(globalThis.__sgQuadStats().dirty|0));},80);})();
      __log('[gaussian-exercise] native #'+target.__i+' '+((bounds[2]-bounds[0])|0)+'x'+((bounds[3]-bounds[1])|0)+' root=#'+rootId+' quality='+SGB[off+81]+' kernel='+SGB[off+82]+' descriptor='+SGB[off+78]);
      return target.__i;
    };
    // Ordered post-filter smoke test.  Keep the target's list deterministic so this validates
    // descriptor ordering and native FBO chaining independently of filters already installed by
    // game content.  Two Blur instances prove that same-family uniforms are not collapsed; the
    // trailing FXAA also catches accidental family-priority dispatch.
    globalThis.__nativeInstallFilterStackExercise=function(){
      if(!PIXI.BlurFilter)return null;var rootId=__qActiveRoot&&__qActiveRoot.__i|0,ids=rootId>0&&typeof globalThis.__sgNativeSubtree==='function'?__sgNativeSubtree(rootId):null;
      if(!ids)return null;var target=null,bounds=null;
      for(var n=0;n<ids.length;n++){var id=ids[n]|0,o=__sgObjs[id];if(!o||o.__i!==id||o._destroyed||o.__nativeNodeProxy||!o.isSprite||!o._texture||!o._texture.valid||!__treeVisible(o))continue;
        var b=id*ST;if((SGB[b+oTYPE]|0)!==1||SGB[b+oREND]===0)continue;try{__sgUpdateBounds(id);}catch(e){}if(globalThis.__sgWriteback)__sgWriteback();var d=BB+id*BSF+4,x0=SGB[d],y0=SGB[d+1],x1=SGB[d+2],y1=SGB[d+3],w=x1-x0,h=y1-y0;
        if(w>100&&h>100&&w<800&&h<800&&x1>4&&y1>4&&x0<1586&&y0<956){target=o;bounds=[x0,y0,x1,y1];break;}}
      if(!target)return null;
      var blurA=new PIXI.BlurFilter(3,1,1,5),blurB=new PIXI.BlurFilter(7,1,1,9),fxaa={enabled:true,__nativeFXAA:true,padding:1};target.filters=[blurA,blurB,fxaa];__wrFilters(target);
      var off=FB+target.__i*FSF,pulse=0;(function requestPresent(){__wrFilters(target);globalThis.__sgDirty=1;if(++pulse<24)setTimeout(requestPresent,50);})();
      var count=SGB[off+96]|0,order=[];for(var i=0;i<count&&i<6;i++)order.push(SGB[off+97+i]|0);
      __log('[filter-stack-exercise] native #'+target.__i+' '+((bounds[2]-bounds[0])|0)+'x'+((bounds[3]-bounds[1])|0)+' root=#'+rootId+' count='+count+' order='+order.join(',')+' conflict='+(target.__sgPostFilterConflict|0));
      return target.__i;
    };
    globalThis.__nativeInstallDropShadowExercise=function(){
      var rootId=__qActiveRoot&&__qActiveRoot.__i|0,ids=rootId>0&&typeof globalThis.__sgNativeSubtree==='function'?__sgNativeSubtree(rootId):null;
      if(!ids)return null;var target=null,bounds=null;
      for(var n=0;n<ids.length;n++){var id=ids[n]|0,o=__sgObjs[id];if(!o||o.__i!==id||o._destroyed||o.__nativeNodeProxy||!o.isSprite||!o._texture||!o._texture.valid||!__treeVisible(o))continue;
        var b=id*ST;if((SGB[b+oTYPE]|0)!==1||SGB[b+oREND]===0)continue;try{__sgUpdateBounds(id);}catch(e){}if(globalThis.__sgWriteback)__sgWriteback();var d=BB+id*BSF+4,x0=SGB[d],y0=SGB[d+1],x1=SGB[d+2],y1=SGB[d+3],w=x1-x0,h=y1-y0;
        if(w>100&&h>100&&w<800&&h<800&&x1>4&&y1>4&&x0<1586&&y0<956){target=o;bounds=[x0,y0,x1,y1];break;}}
      if(!target)return null;
      var filter={enabled:true,offset:{x:14,y:12},alpha:.75,shadowOnly:false,padding:28,_tintFilter:{uniforms:{color:new Float32Array([0.08,0.01,0.18])}},_blurFilter:{kernels:[4,2.6666667,1.3333333],pixelSize:{x:1,y:1},clamp:false}},fs=target.filters?target.filters.slice():[];fs.push(filter);target.filters=fs;__wrFilters(target);
      var off=FB+target.__i*FSF,pulse=0;(function requestPresent(){__wrFilters(target);globalThis.__sgDirty=1;if(++pulse<24)setTimeout(requestPresent,50);})();
      __log('[drop-shadow-exercise] native #'+target.__i+' '+((bounds[2]-bounds[0])|0)+'x'+((bounds[3]-bounds[1])|0)+' root=#'+rootId+' passes='+SGB[off+84]+' descriptor='+SGB[off+29]);
      return target.__i;
    };
    globalThis.__sgDumpSpines=function(root){
      var out=[];
      __ov.forEach(function(o){
        if(!o||!(o.__i>0)||o._destroyed||!__treeVisible(o))return;
        var p=o,g=0,linked=!root;while(p&&g++<256){if(p===root){linked=true;break;}p=p.parent;}if(!linked)return;
        var sk=o.skeleton,b=o.__i*ST,tracks=[],ats={region:0,mesh:0,clip:0,other:0,none:0};
        if(o.state&&o.state.tracks)for(var ti=0;ti<o.state.tracks.length;ti++){var tr=o.state.tracks[ti];if(tr&&tr.animation)tracks.push(ti+':'+tr.animation.name+'@'+(+tr.trackTime||0).toFixed(2));}
        var dro=sk&&sk.drawOrder||[];
        for(var di=0;di<dro.length;di++){var sl=dro[di],a=sl&&(sl.getAttachment?sl.getAttachment():sl.attachment);if(!a){ats.none++;continue;}var k=a.__spKind;if(k===undefined){var cn=(a.constructor&&a.constructor.name)||'';k=cn.indexOf('Clipping')>=0?2:(cn.indexOf('Mesh')>=0?1:(cn.indexOf('Region')>=0?0:3));}if(k===0)ats.region++;else if(k===1)ats.mesh++;else if(k===2)ats.clip++;else ats.other++;}
        out.push('#'+o.__i+' '+((sk&&sk.data&&sk.data.name)||o.name||'?')+' anim='+(tracks.join(',')||'-')+' bones='+(sk&&sk.bones?sk.bones.length:0)+' slots='+dro.length+' att='+ats.region+'r/'+ats.mesh+'m/'+ats.clip+'c/'+ats.other+'o/'+ats.none+'n quad='+(SGB[b+oQUADCNT]|0)+' type='+(SGB[b+oTYPE]|0)+' slotObj='+(o.slotsObject&&o.slotsObject.size||0)+' diag='+(o.__spDiag||'ok'));
      });
      out.sort();__log('[native-spines] count='+out.length+(out.length?'\n'+out.join('\n'):''));return out.length;
    };

    globalThis.__sgDumpNamedTree=function(name){
      var root=null;for(var i=1;i<__sgObjs.length;i++){var x=__sgObjs[i];if(x&&x.__i===i&&x.name===name){root=x;break;}}
      if(!root){__log('[native-tree] not found '+name);return 0;}
      var out=[];function walk(o,d){if(!o||out.length>=160)return;var id=o.__i|0,b=id*ST,bd=BB+id*BSF+4,tx=o._texture||o.texture,bt=tx&&tx.baseTexture,sd='';
        if(o.isSprite&&tx){var fr=tx.frame,or=tx.orig;sd=' tid='+(SGB[b+oTEXID]|0)+' rect='+Number(SGB[b+oRX]).toFixed(0)+','+Number(SGB[b+oRX+1]).toFixed(0)+','+Number(SGB[b+oRX+2]).toFixed(0)+'x'+Number(SGB[b+oRX+3]).toFixed(0)+' uv='+[0,1,2,3,4,5,6,7].map(function(z){return Number(SGB[b+oUV+z]).toFixed(3);}).join(',')+' frame='+(fr?(fr.x+','+fr.y+','+fr.width+'x'+fr.height):'-')+' orig='+(or?(or.width+'x'+or.height):'-')+' rot='+(tx.rotate||0);}
        out.push(Array(d+1).join(' ')+'#'+id+' '+((o.constructor&&o.constructor.name)||'?')+' name="'+(o.name||'')+'" vis='+(o.visible===false?0:1)+' rend='+(o.renderable===false?0:1)+' alpha='+Number(o.alpha===undefined?1:o.alpha).toFixed(2)+' wa='+Number(SGB[b+oWALPHA]).toFixed(2)+' event='+(o.eventMode||'-')+'/'+(o.interactiveChildren===false?0:1)+' nativeEvent='+(SGB[b+45]|0)+'/'+(SGB[b+46]|0)+' hf='+(SGB[b+oHITFLAGS]|0)+' local='+Number(o.x||0).toFixed(0)+','+Number(o.y||0).toFixed(0)+' size='+Number(o._width||o.width||0).toFixed(0)+'x'+Number(o._height||o.height||0).toFixed(0)+' sg=t'+(SGB[b+oTYPE]|0)+' q'+(SGB[b+oQUADCNT]|0)+' off'+(SGB[b+oQUADOFF]|0)+' mask'+(SGB[b+oMASKI]|0)+' world='+Number(SGB[b+16]).toFixed(0)+','+Number(SGB[b+17]).toFixed(0)+' bounds='+Number(SGB[bd]).toFixed(0)+','+Number(SGB[bd+1]).toFixed(0)+'..'+Number(SGB[bd+2]).toFixed(0)+','+Number(SGB[bd+3]).toFixed(0)+' hit='+Number(SGB[bd+8]).toFixed(0)+','+Number(SGB[bd+9]).toFixed(0)+'..'+Number(SGB[bd+10]).toFixed(0)+','+Number(SGB[bd+11]).toFixed(0)+' treeHit='+Number(SGB[bd+12]).toFixed(0)+','+Number(SGB[bd+13]).toFixed(0)+'..'+Number(SGB[bd+14]).toFixed(0)+','+Number(SGB[bd+15]).toFixed(0)+' tex='+(bt?(bt.valid?1:0):'-')+sd);
        var ch=o.children;if(ch)for(var k=0;k<ch.length;k++)walk(ch[k],d+1);
      }walk(root,0);__log('[native-tree] '+name+' count='+out.length+'\n'+out.join('\n'));return out.length;
    };

    // 给轻代理层的窄接口：代理不继承 Pixi DisplayObject，只持有 native slot 和必要事件/API。
    // 普通 FGUI 图片代理仍可作为 duck-typed child 留在 Pixi children 中，以保持框架的排序、显隐和
    // 生命周期语义；主屏不遍历 Pixi 绘制它，最终图元始终由 native scene graph 产生。
    globalThis.__nativeSceneRuntime={
      buffer:SGB,objects:__sgObjs,stride:ST,boundsBase:BB,boundsStride:BSF,boundsMeta:BM,
      // fgui facade 版本槽(u32 视图与四个区段基址;S1 覆盖审计 / S2 读时判新鲜度用)
      u32:U32,fguiSeqBase:FSQ,fguiDirtyBase:FDB,fguiChildSeqBase:FCS,fguiCommitBase:FCM,nodeGenBase:NGB,
      // 审计用:读节点记录里的 mask 槽(oMASKI=19,与 03-render-sync 的槽位常量一致;fgui 派生状态 oracle 对拍 JS 镜像 __nativeFguiClipMaskId)
      nodeMaskId:function(i){i=i|0;return i>0?(SGB[i*ST+19]|0):0;},
      claimNativeSlots:__claimNativeSlots,
      alloc:function(o){var i=__sgAlloc();if(i>0){__sgClaimSlot(o,i,'runtime.alloc');SGB[BM]=1;}return i;},
      // Rebind a freshly-created semantic facade to a slot that was already allocated and
      // populated by the native FGUI factory. NativePoint reads owner.__i dynamically, so proxy
      // facades need no transform rebuild. Pixi ObservablePoints instead hold ABSOLUTE offsets
      // (__o) — instrumented Pixi shells are outside this API's design (adopted slots keep the
      // authored native values; JS transform fields are not republished). Defend below.
      adopt:function(o,i){i=i|0;if(!o||i<=0)return false;var old=o.__i|0,oldOwned=old>0&&__sgObjs[old]===o;
        // The slot was created and may have been reparented wholly inside the native FGUI
        // factory before its semantic JS facade was requested.  Never inherit a parent value
        // cached by an earlier temporary facade.
        if(i<__nativeParents.length)__nativeParents[i]=-2;
        if(old===i&&oldOwned)return true;
        if(oldOwned&&old!==i){__sgReleaseSlotLease(o,true);__sgObjs[old]=undefined;__sgFree(old);}
        else if(old>0&&old!==i){__sgReleaseSlotLease(o,false);__sgDropStaleSlotWrite(o,old*ST,'adopt/free');}
        // v2 门槛5(影子消灭阶段0):adopt 语义=槽值以 native 工厂为准,不回灌 JS transform。
        // 已 instrument 的 Pixi transform 被 adopt 属设计外路径:撤销其旧租约(防悬挂偏移,
        // 读写落暂存,槽保持 authored 值)并计数供审计(release 幂等,重复 detach 无害)。
        if(o.transform&&!o.__nativeNodeProxy&&o.transform.position&&o.transform.position.__sgRawHook){
          globalThis.__sgAdoptInstrumentedPixi=(globalThis.__sgAdoptInstrumentedPixi|0)+1;
          __sgReleaseSlotLease(o,false);
        }
        __sgClaimSlot(o,i,'runtime.adopt');SGB[BM]=1;globalThis.__sgDirty=1;return true;},
      free:function(o){if(o&&o.__i>0){var i=o.__i;if(__sgObjs[i]===o){__sgReleaseSlotLease(o,true);__sgObjs[i]=undefined;__sgFree(i);SGB[BM]=1;}else{__sgReleaseSlotLease(o,false);__sgDropStaleSlotWrite(o,i*ST,'runtime.free');}o.__i=undefined;}},
      bindPixi:__bind,setParent:function(c,p,before){__sgSetParent(c,p,before===undefined?-1:before);SGB[BM]=1;},
      remove:function(i){__sgRemove(i);SGB[BM]=1;},markLocal:function(i){__markD(i*ST);globalThis.__sgDirty=1;},
      setKnownParent:function(child,parent){child=child|0;if(child>0&&child<__nativeParents.length)__nativeParents[child]=parent|0;},
      invalidateParent:function(child){child=child|0;if(child>0&&child<__nativeParents.length)__nativeParents[child]=-2;},
      // 复挂复活的统一出口：quad 存储恢复(__qReleasedTree)/子树洞重绑/renderDetachedDirty 转正。
      // COp.addChild(PIXI 父)一直走它;NativeNodeProxy._adopt(proxy 父收 pixi 子,如 FGUI
      // setNativeObject)此前漏接——渲染缓存被释放过的对象经该路径挂回时 quad 树不复活,
      // 表现为"切走一段时间再切回,spine/动态内容消失"。两条挂载路径
      // 必须共享同一套复活语义,不识别任何业务对象。
      activateAttached:function(child,parent){return __activateDetachedRenderTree(child,parent,false);},
      registerScrollPane:function(display){__registerFguiPaneOne(display);},
      markScrollPaneDirty:function(display){__registerFguiPaneOne(display);},
      markBounds:function(){__markBoundsAll();globalThis.__sgDirty=1;},writeHitMeta:__writeHitMeta,instrumentHitArea:__instrumentHitArea,
      markBoundsNode:function(i){__markBoundsNode(i|0);globalThis.__sgDirty=1;},
      // Queue semantic/native-only nodes for the next render-data flush.  Unlike Pixi objects,
      // these proxies are never visited by renderer.render(), so merely setting `dirty` is not
      // enough to turn an empty container slot into its final sprite/text representation.
      markVisual:__markRender,writeVisual:__wrType,
      // Semantic filter changes must reach this frame even when text/geometry work is budgeted.
      writeFilters:function(o){if(!o||!(o.__i>0))return;__wrFilters(o);__markD(o.__i*ST);globalThis.__sgDirty=1;},
      // FairyGUI's legacy bitmap-font layout still supplies positioned glyph sprites, but they
      // are transient measurements only. Pack them into one native multiquad node so a 20-character
      // label does not retain 20 Pixi children and 20 native scene nodes.
      writeBitmapGlyphs:function(o,glyphs){
        if(!o||!(o.__i>0))return false;glyphs=glyphs||[];var b=o.__i*ST,need=0;
        for(var i=0;i<glyphs.length;i++){var g=glyphs[i],tx=g&&(g._texture||g.texture);if(g&&tx&&tx.valid&&tx.baseTexture&&tx.orig){if(!tx._uvs&&typeof tx.updateUvs==='function')tx.updateUvs();if(tx._uvs)need++;}}
        o.__nativeBitmapCandidateCount=glyphs.length;o.__nativeBitmapUsableCount=need;o.__nativeBitmapSubmittedCount=0;
        __releaseOwnedBaseTextures(o);
        if(!need){__qRelease(o);__sgTypeW(b,0);SGB[b+oQUADCNT]=0;__setNativeGeometryHit(o,false);__markBoundsNode(o.__i);globalThis.__sgDirty=1;return true;}
        var cur=__qEnsure(o,need);if(cur<0){__setEsc(o,b,'fgui-bitmap-quad-pool');return false;}
        var base=QB+1,n=0,premul=0;
        for(var gi=0;gi<glyphs.length;gi++){
          var glyph=glyphs[gi],tex=glyph&&(glyph._texture||glyph.texture),bt=tex&&tex.baseTexture,orig=tex&&tex.orig;if(tex&&!tex._uvs&&typeof tex.updateUvs==='function')tex.updateUvs();var uv=tex&&tex._uvs;if(!glyph||!tex||!tex.valid||!bt||!orig||!uv)continue;
          var tid=__nativeTexId(bt,true);if(!tid)continue;__addBaseTextureOwner(bt,o);premul=__premulOf(bt);
          var trim=tex.trim,anchor=glyph._anchor||glyph.anchor,ax=anchor?(anchor._x===undefined?(anchor.x||0):anchor._x):0,ay=anchor?(anchor._y===undefined?(anchor.y||0):anchor._y):0;
          var x0,y0,x1,y1;if(trim){x0=trim.x-ax*orig.width;y0=trim.y-ay*orig.height;x1=x0+trim.width;y1=y0+trim.height;}else{x0=-ax*orig.width;y0=-ay*orig.height;x1=x0+orig.width;y1=y0+orig.height;}
          var tr=glyph.transform,m=tr&&tr.localTransform;if(tr&&tr.updateLocalTransform)tr.updateLocalTransform();
          var a=m?m.a:(glyph.scale?glyph.scale.x:1),bb=m?m.b:0,c=m?m.c:0,d=m?m.d:(glyph.scale?glyph.scale.y:1),tx=m?m.tx:(glyph.x||0),ty=m?m.ty:(glyph.y||0),q=base+(cur+n)*QSF;
          SGB[q]=tid;SGB[q+1]=a*x0+c*y0+tx;SGB[q+2]=bb*x0+d*y0+ty;SGB[q+3]=a*x1+c*y0+tx;SGB[q+4]=bb*x1+d*y0+ty;SGB[q+5]=a*x1+c*y1+tx;SGB[q+6]=bb*x1+d*y1+ty;SGB[q+7]=a*x0+c*y1+tx;SGB[q+8]=bb*x0+d*y1+ty;
          SGB[q+9]=uv.x0;SGB[q+10]=uv.y0;SGB[q+11]=uv.x1;SGB[q+12]=uv.y1;SGB[q+13]=uv.x2;SGB[q+14]=uv.y2;SGB[q+15]=uv.x3;SGB[q+16]=uv.y3;
          SGB[q+17]=__tintOf(glyph);SGB[q+18]=glyph.alpha===undefined?1:glyph.alpha;SGB[q+19]=-1;SGB[q+20]=-1;SGB[q+21]=0;SGB[q+22]=premul;SGB[q+23]=0;n++;
        }
        o.__nativeBitmapSubmittedCount=n;
        if(!n){__qRelease(o);__sgTypeW(b,0);SGB[b+oQUADCNT]=0;__markBoundsNode(o.__i);return true;}
        SGB[b+oQUADOFF]=cur;SGB[b+oQUADCNT]=n;__sgTypeW(b,3);SGB[b+oBLEND]=o.blendMode||0;SGB[b+oPREMUL]=premul;__setNativeGeometryHit(o,false);__markBoundsNode(o.__i);globalThis.__sgDirty=1;return true;
      },
      retainTexture:function(tex,o){if(tex)__addTextureOwner(tex,o);},
      releaseTexture:function(tex,o){if(tex)__removeTextureOwner(tex,o);},
      rebindTextureSource:function(tex,oldBase,newBase){if(tex&&tex.__sgSpriteOwners)tex.__sgSpriteOwners.forEach(function(o){__removeBaseTextureOwner(oldBase,o);__addBaseTextureOwner(newBase,o);__markRender(o);});},
      releaseVisual:function(o){
        if(!o)return;__releaseOwnedMasks(o,null);o.__sgMaskObjs=[];o.__sgMaskObj=null;
        __renderDirty.delete(o);__renderDetachedDirty.delete(o);
        if(o.__sgFilters)for(var i=0;i<o.__sgFilters.length;i++){var f=o.__sgFilters[i];if(f&&f.__sgOwners)f.__sgOwners.delete(o);}o.__sgFilters=[];
      },
      syncWorld:function(o){if(!o||!(o.__i>0))return;var id=o.__i|0,p=__nativeParentOf(id),guard=0;while(p>0&&guard++<32768){id=p;p=__nativeParentOf(id);}__sgCanonicalBarrier(id);},
      textureId:function(tex){var bt=tex&&tex.baseTexture?tex.baseTexture:tex;return __nativeTexId(bt,true);},
      getBounds:function(o,out){if(!o||!(o.__i>0))return null;__sgUpdateBounds(o.__i);if(globalThis.__sgWriteback)globalThis.__sgWriteback();var d=BB+o.__i*BSF+4,x0=SGB[d],y0=SGB[d+1],x1=SGB[d+2],y1=SGB[d+3];if(x0>x1||y0>y1)return null;out=out||new PIXI.Rectangle();out.x=x0;out.y=y0;out.width=x1-x0;out.height=y1-y0;return out;},
      // Query local geometry directly. World-space inversion cannot measure a popup while
      // its show/hide animation has scale=0; a facade walk also omits native-only FGUI leaves.
      getLocalBounds:function(o,out){
        if(!o||!(o.__i>0))return null;
        __flushMutationReads();this.syncWorld(o);
        var bounds=__sgGetLocalBounds(o.__i);if(!bounds)return null;
        out=out||new PIXI.Rectangle();out.x=bounds[0];out.y=bounds[1];out.width=bounds[2];out.height=bounds[3];return out;
      }
    };
