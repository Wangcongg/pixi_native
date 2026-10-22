    // ===== conch P1:渲染数据同步(JS 只写不读;native 合批器消费) =====
    // slot 26+:TEXID/BLEND/RENDERABLE/局部rect/8UV/TINT;slot11=TYPE(0容器 1sprite 2逃生舱)
    var oTYPE=11, oTEXID=26, oBLEND=27, oREND=28, oRX=29, oUV=33, oTINT=41;
    var oQUADOFF=42, oQUADCNT=43, oPREMUL=44, oHITFLAGS=47; // M4:多-quad 起始号+个数;M6:PREMUL(1=纹理非预乘)
    var __nativeTextStyleContract=globalThis.__nativeTextStyleContract;
    var __nativeTextColor=function(v){if(__nativeTextStyleContract&&typeof __nativeTextStyleContract.colorCss==='function')return __nativeTextStyleContract.colorCss(v,'transparent');if(Array.isArray(v))v=v[0];if(typeof v==='number')return '#'+('000000'+(v>>>0).toString(16)).slice(-6);return String(v==null?'transparent':v);};
    var __nativeTextGradient=function(style){return __nativeTextStyleContract&&typeof __nativeTextStyleContract.gradient==='function'?__nativeTextStyleContract.gradient(style||{}):null;};
    var __nativeTextFontMetrics=function(font){try{var tm=PIXI.TextMetrics,fm=tm&&tm.measureFont&&tm.measureFont(font),a=Number(fm&&fm.ascent),d=Number(fm&&fm.descent);if(isFinite(a)&&isFinite(d)&&a>0&&d>=0)return {ascent:a,descent:d};}catch(_fontMetrics){}return {ascent:0,descent:0};};
    var __nativeTextGeometryMatches=function(o,txt,anchorX,anchorY){
      if(!txt)return true;
      if(!o||!(o.__i>0))return false;
      var b=o.__i*ST,w=Number(SGB[b+oRX+2]),h=Number(SGB[b+oRX+3]);
      if((SGB[b+oTYPE]|0)!==1||!(SGB[b+oTEXID]>0)||!isFinite(w)||!isFinite(h)||w<=0||h<=0)return false;
      var eps=1/256;
      return Math.abs(Number(SGB[b+oRX])-(-anchorX*w))<=eps&&Math.abs(Number(SGB[b+oRX+1])-(-anchorY*h))<=eps;
    };
    var __syncNativeText=function(o){
      var st=o.style||o._style,txt=String(o.text||''),sid=st&&st.styleID,ax=o._anchor?Number(o._anchor._x)||0:0,ay=o._anchor?Number(o._anchor._y)||0:0,key=txt+'|'+sid+'|'+(o._resolution||1)+'|anchor:'+ax+','+ay;
      if(o.__nativeMetricsKey===key&&o.__nativeMetrics){
        if(__nativeTextGeometryMatches(o,txt,ax,ay))return o.__nativeMetrics;
        var repairStats=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});repairStats.nativeTextGeometryRepairs=(repairStats.nativeTextGeometryRepairs||0)+1;
      }
      if(!(o.__i>0)&&typeof __bind==='function')__bind(o);if(!(o.__i>0))return null;
      var font=typeof st.toFontString==='function'?st.toFontString():(((st.fontSize||26)+'px ')+(st.fontFamily||'Arial'));
      var sd=Number(st.dropShadowDistance)||0,sa=Number(st.dropShadowAngle)||0,pad=Number(st.padding)||0,fillGradient=__nativeTextGradient(st),fontMetrics=__nativeTextFontMetrics(font);
      // 三宽度协议:scenegraph 层的语义文本是动态 PIXI.Text,契约恒为 natural(V1 调用 box=0 即
      // natural 的显式定义,见 text_layout_contract.js);不经契约模块决议,无推断分支。
      var m=__nativeTextUpdate(o.__i,txt,font,__nativeTextColor(st.fill),__nativeTextColor(st.stroke),Number(st.strokeThickness)||0,
        st.dropShadow?__nativeTextColor(st.dropShadowColor):'transparent',Number(st.dropShadowBlur)||0,
        st.dropShadow?sd*Math.cos(sa):0,st.dropShadow?sd*Math.sin(sa):0,st.align||'left',pad,Number(o._resolution||1)||1,ax,ay,
        st.wordWrap?(Number(st.wordWrapWidth)||0):0,Number(st.lineHeight)||0,Number(st.letterSpacing)||0,Number(st.leading)||0,'top',
        fillGradient?fillGradient.colors:'',fillGradient?fillGradient.stops:'',fillGradient?fillGradient.type:0,fontMetrics.ascent,fontMetrics.descent);
      if(!m)return null;m.nativeWidth=m.width;m.nativeHeight=m.height;
      // Preserve Pixi layout contracts (width/height/getBounds) without resizing or painting canvas.
      var tx=o._texture;
      if(tx&&tx.orig&&tx.trim&&tx._frame){tx.trim.width=tx._frame.width=m.nativeWidth;tx.trim.height=tx._frame.height=m.nativeHeight;
        tx.trim.x=-pad;tx.trim.y=-pad;tx.orig.width=Math.max(1,m.nativeWidth-pad*2);tx.orig.height=Math.max(1,m.nativeHeight-pad*2);}
      o._font=font;
      o.localStyleID=sid;o.dirty=false;o.__nativeMetricsKey=key;o.__nativeMetrics=m;return m;
    };
    if(typeof globalThis.__nativeTextUpdate==='function'&&PIXI.Text&&PIXI.Text.prototype&&!PIXI.Text.prototype.__nativeSemanticText){
      PIXI.Text.prototype.__nativeSemanticText=1;
      PIXI.Text.prototype.updateText=function(respectDirty){
        var st=this.style||this._style;if(this.localStyleID!==(st&&st.styleID)){this.dirty=true;this.__nativeMetricsKey='';}
        if(!this.dirty&&respectDirty&&this.__nativeMetrics)return;
        __syncNativeText(this);
      };
      __log('[native-text] Pixi Text canvas raster path disabled; semantic CoreText path active');
    }
    var __setNativeGeometryHit=function(o,enabled){
      if(!o||!(o.__i>0))return;
      enabled=!!enabled;
      if(!!o.__nativeHitGeometry===enabled)return;
      o.__nativeHitGeometry=enabled;
      var b=o.__i*ST,flags=SGB[b+oHITFLAGS]|0;
      SGB[b+oHITFLAGS]=enabled?(flags|32):(flags&~32);
      __markBoundsNode(o.__i);
    };
    var __escNote = function(o, reason){
      if(!o || o.visible===false || o.renderable===false || (o.worldAlpha!==undefined&&o.worldAlpha<=0.02)) return;
      var cn=(o.constructor&&o.constructor.name)||'?', key=cn+'+'+reason;
      var ET=(globalThis.__escTally=globalThis.__escTally||{}); ET[key]=(ET[key]||0)+1;
      if(globalThis.__SG_DIAG===1){
        var seen=(globalThis.__escSeen=globalThis.__escSeen||{}), sig=key+'#'+(o.name||'')+'@'+(o.__i|0);
        if(!seen[sig] && (globalThis.__escDetailC||0)<120){
          seen[sig]=1; globalThis.__escDetailC=(globalThis.__escDetailC||0)+1;
          var wt=o.worldTransform, tex=o._texture||o.texture, bt=tex&&tex.baseTexture, tt=o.tileTransform;
          __log('[esc-detail] '+key+' i='+(o.__i|0)+' name='+(o.name||'')+' ch='+(o.children?o.children.length:0)+' pos='+(wt?((wt.tx|0)+','+(wt.ty|0)):'?')+' size='+((o._width||o.width||0)|0)+'x'+((o._height||o.height||0)|0)+' tex='+(bt?((bt.valid?1:0)+'/'+(bt.realWidth||bt.width)+'x'+(bt.realHeight||bt.height)):'-')+(tt?(' tile='+((tt.position&&tt.position.x)||0)+','+((tt.position&&tt.position.y)||0)+' sc='+((tt.scale&&tt.scale.x)||1)+','+((tt.scale&&tt.scale.y)||1)):'')+(o.__sgEscExtra?(' '+o.__sgEscExtra):''));
        }
      }
      if((globalThis.__escLogT=(globalThis.__escLogT||0)+1)%300===0){ var es=''; for(var kk in ET) es+=kk+':'+ET[kk]+' '; __log('[esc-vis] '+es); globalThis.__escTally={}; }
    };
    var __setEsc = function(o,b,reason){ o.__sgEscapeReason=reason||'render'; __sgTypeW(b,2); __escNote(o, o.__sgEscapeReason); };
    // Pixi's PMA metadata describes encoded image bytes, not a request to
    // multiply them again. Its HTML-image loader assigns src before the first
    // await, so scope the decode option to that synchronous Image construction.
    if(globalThis.__nativeSupportsImageSourcePMA===true && PIXI.loadTextures && !PIXI.loadTextures.__nativeSourceAlpha){
      (function(parser){var load=parser.load;parser.__nativeSourceAlpha=true;
        parser.load=function(url,asset,loader){
          var previous=globalThis.__nativeImageSourcePremultiplied;
          globalThis.__nativeImageSourcePremultiplied=!!(asset&&asset.data&&asset.data.alphaMode===PIXI.ALPHA_MODES.PMA);
          try{return load.apply(this,arguments);}
          finally{globalThis.__nativeImageSourcePremultiplied=previous;}
        };
      })(PIXI.loadTextures);
    }
    var __nativeNeedsPremultiplyOf = function(bt){
      var resource=bt&&bt.resource,source=resource&&(resource.source||resource.bitmap);
      return !!(source&&source.__nativeNeedsPremultiply);
    };
    var __premulOf = function(bt){ return (__nativeNeedsPremultiplyOf(bt)||(bt&&bt.alphaMode===0))?1:0; };   // KTX DFD straight / Pixi NPM → shader 内预乘
    // 返回 1=上传成功(或非 native 主屏无需上传)、0=失败(bt 未就绪/bind 异常)。调用方据此决定是否重试,
    // 不能把失败当成功消费掉——否则文字纹理停在旧字形(商店按钮残留编辑器默认 "PLAY" 即此症)。
    var __forceUploadBT = function(bt, reason){
      if(!(globalThis.__NATIVE_SCREEN===1 && globalThis.__nativeRenderer)) return 1;
      if(!(bt && bt.valid)) return 0;
      try{ if(bt.update) bt.update(); }catch(e){}
      try{
        var glt=bt._glTextures;
        if(glt) for(var k in glt){ var gt=glt[k]; if(gt){ gt.dirtyId=-1; gt.dirtyStyleId=-1; } }
      }catch(e){}
      try{
        globalThis.__nativeRenderer.texture.bind(bt,0);
        if(globalThis.__encFlush) globalThis.__encFlush();   // flip 短路 Pixi: native batcher 采样前必须先执行文本/canvas 上传命令
      }catch(e){ if((globalThis.__upErrN||0)<6){ globalThis.__upErrN=(globalThis.__upErrN||0)+1; __log('[upload] '+(reason||'?')+' '+e.message); } return 0; }
      return 1;
    };
    var __texIdFromWrap=function(glt,k){
      var wrap=glt&&glt[k],gt=wrap&&wrap.texture;
      // A cached external wrapper whose ResourceManager entry disappeared is only a stale numeric
      // capability. Keep it installed so the normal ImageResource update can promote it in place,
      // but never publish that dead id to SceneGraph while rehydration is in flight.
      if(wrap&&wrap.__nativeExternalInvalid)return 0;
      // WebGLTexture 已 delete 后，decoder 对应槽为 0。移除 Pixi 的旧缓存，下一次 bind 重建。
      if(gt&&gt.__deleted){ try{delete glt[k];}catch(e){glt[k]=null;} return 0; }
      return gt&&gt.__id ? gt.__id : 0;
    };
    var __readTexId = function(bt){
      var glt=bt&&bt._glTextures;if(!glt)return 0;
      var r=globalThis.__nativeRenderer,uid=r&&r.CONTEXT_UID;
      // native scene graph 与 __nativeRenderer 共用一个 GL context。这里不能返回离屏 renderer
      // 或已销毁 renderer 留下的第一个历史句柄，否则 Spine 几何正常但会采样错误纹理而变黑。
      if(uid!==undefined&&uid!==null)return __texIdFromWrap(glt,uid);
      for(var k in glt){var id=__texIdFromWrap(glt,k);if(id)return id;}
      return 0;
    };
    // Native ResourceManager images are already decoded and uploaded by the macOS host.
    // Adopt the external texture id instead of copying a multi-megabyte RGBA ArrayBuffer
    // through JSC and the WebGL command stream. Pixi keeps its BaseTexture/layout API, but
    // the actual GPU lifetime is reference-counted by Native.
    var __nativeExternalCid=function(texture){
      var resource=texture&&texture.resource,source=resource&&(resource.source||resource.bitmap);
      return source&&source.__nativeTextureId>0?(source.__nativeTextureId|0):0;
    };
    var __nativeExternalWrapNeedsPromotion=function(texture,glTexture){
      return !!(__nativeExternalCid(texture)&&glTexture&&!glTexture.__nativeExternalCid);
    };
    // Native GPU eviction and Pixi's JS asset cache have deliberately different lifetimes.
    // A cached Image/BaseTexture can therefore outlive the ResourceManager entry named by its
    // __nativeTextureId.  Never publish that stale number merely because it is still present on
    // the Image: retain() is the authoritative liveness check.  On a miss, reassign the original
    // source URL so the normal ImageResource pipeline obtains a fresh native cid and emits the
    // BaseTexture update that wakes all scene-graph owners.
    var __reloadStaleNativeExternal=function(texture,staleCid){
      var resource=texture&&texture.resource,source=resource&&(resource.source||resource.bitmap);
      if(!source||source.__nativeTextureReloading)return !!(source&&source.__nativeTextureReloading);
      var url=source.src||resource.url;if(!url||typeof source.src==='undefined')return false;
      source.__nativeTextureReloading=staleCid||1;
      globalThis.__nativeResourceStaleHandles=(globalThis.__nativeResourceStaleHandles||0)+1;
      var settled=false;
      // ImageResource.load() caches its first promise. Reassigning source.src after native eviction
      // can therefore return an already-resolved promise before the replacement cid exists. Wake
      // the BaseTexture only from the actual Image load event (or a promise that observes the fresh
      // cid), otherwise the ordinary placeholder installed below remains permanently black.
      var finish=function(){
        if(settled||!(source.__nativeTextureId>0))return;
        settled=true;source.__nativeTextureReloading=0;
        try{if(texture&&typeof texture.update==='function')texture.update();
          else if(resource&&typeof resource.update==='function')resource.update();}catch(e){}
      };
      var fail=function(){if(settled)return;settled=true;source.__nativeTextureReloading=0;};
      try{
        if(typeof source.addEventListener==='function'){
          source.addEventListener('load',finish);source.addEventListener('error',fail);
        }
        // The host Image setter clears __nativeTextureId before starting the asynchronous native
        // request. ImageResource.load() then calls update(), preserving Pixi's ordinary contract.
        source.src=url;
        var pending=resource.load&&resource.load();
        if(pending&&typeof pending.then==='function')pending.then(finish,fail);
        else finish();
        __log('[native-resource] stale external cid='+staleCid+'; reload '+String(url).slice(0,160));
        return true;
      }catch(e){fail();__log('[native-resource] stale cid reload failed: '+(e&&e.message||e));return false;}
    };
    // initTexture validates a cid when the GL wrapper is first created, but long-lived Pixi asset
    // caches can also reuse a wrapper created in an earlier residency generation. Validate that
    // existing capability at the common texture-id boundary. One check per wrapper per frame keeps
    // shared atlas consumers cheap; a live hard reference cannot be evicted between such checks.
    var __validateNativeExternalWrap=function(texture,glTexture){
      if(!glTexture||!glTexture.__nativeExternalCid)return true;
      if(glTexture.__nativeExternalInvalid)return false;
      if(typeof globalThis.__nativeResourceAlive!=='function')return true;
      var frame=globalThis.__frameNo|0;
      if(glTexture.__nativeExternalAliveFrame===frame)return true;
      glTexture.__nativeExternalAliveFrame=frame;
      var cid=glTexture.__nativeExternalCid|0,alive=false;
      try{alive=!!globalThis.__nativeResourceAlive(cid);}catch(e){alive=true;}
      if(alive)return true;
      glTexture.__nativeExternalInvalid=1;
      // The native entry is already gone, so this wrapper no longer owns a releasable reference.
      // Clearing the flag prevents a later replacement/destroy from generating a bogus release.
      glTexture.__nativeExternalRetained=0;
      __reloadStaleNativeExternal(texture,cid);
      return false;
    };
    var __retainNativeExternal=function(texture,cid){
      var retained=false;try{retained=!!__nativeResourceRetain(cid);}catch(e){}
      if(!retained)__reloadStaleNativeExternal(texture,cid);
      return retained;
    };
    // 取得 cid 的 glTexture 引用:优先"转交"——Image adopt 的 publish 引用直接移交给 glTexture
    // (refs 不变),Image 从此只持软 cid。否则(复绑/共享 BaseTexture)走普通 retain。
    // 关键取舍:若在 adopt 之上叠加 retain,常驻 Assets 缓存的 Image 会把 refs 钉在 ≥1,
    // TextureGC destroyTexture 后闲置图也无法被 384MB 预算回收(29 台马拉松 gpu 顶到 701MB
    // 的机制);转交后 destroyTexture → refs=0 → 预算可回收,复绑时 stale reload 兜底,
    // 与浏览器"TextureGC 释放 GL、再次使用重新上传"同构。
    var __acquireNativeExternal=function(texture,cid){
      var resource=texture&&texture.resource,source=resource&&(resource.source||resource.bitmap);
      if(source&&source.__nativeTextureOwned&&(source.__nativeTextureId|0)===cid
        &&typeof globalThis.__nativeImgTransfer==='function'&&globalThis.__nativeImgTransfer(source))return true;
      return __retainNativeExternal(texture,cid);
    };
    if(PIXI.TextureSystem&&PIXI.TextureSystem.prototype&&PIXI.GLTexture&&typeof globalThis.__nativeResourceRetain==='function'&&!PIXI.TextureSystem.prototype.__nativeExternalImages){
      var __tsp=PIXI.TextureSystem.prototype,__oldInitTexture=__tsp.initTexture,__oldUpdateTexture=__tsp.updateTexture,__oldDestroyTexture=__tsp.destroyTexture;
      __tsp.__nativeExternalImages=1;
      __tsp.initTexture=function(texture){
        var cid=__nativeExternalCid(texture);if(!cid)return __oldInitTexture.apply(this,arguments);
        // acquire() returning false means ResourceManager already evicted this cid while the JS
        // Image stayed cached. Fall back to a normal (temporarily empty) Pixi texture until the
        // reload above publishes a fresh cid; do not manufacture a live-looking dead handle.
        if(!__acquireNativeExternal(texture,cid))return __oldInitTexture.apply(this,arguments);
        var handle={__id:cid,__kind:'texture',__nativeExternal:1},glTexture=new PIXI.GLTexture(handle);
        glTexture.width=texture.realWidth;glTexture.height=texture.realHeight;glTexture.dirtyId=texture.dirtyId;glTexture.__nativeExternalCid=cid;glTexture.__nativeExternalRetained=1;glTexture.__nativeExternalInvalid=0;
        texture.alphaMode=__nativeNeedsPremultiplyOf(texture)?PIXI.ALPHA_MODES.NPM:PIXI.ALPHA_MODES.PMA;texture.__texRealUp=1;texture.__texDataPending=0;
        texture._glTextures[this.CONTEXT_UID]=glTexture;this.managedTextures.push(texture);texture.on('dispose',this.destroyTexture,this);
        globalThis.__nativeResourceAdoptions=(globalThis.__nativeResourceAdoptions||0)+1;
        return glTexture;
      };
      __tsp.updateTexture=function(texture){
        var cid=__nativeExternalCid(texture),glTexture=texture&&texture._glTextures&&texture._glTextures[this.CONTEXT_UID];
        if(!cid||!glTexture)return __oldUpdateTexture.apply(this,arguments);
        // A retain miss in initTexture intentionally installed Pixi's ordinary placeholder.
        // Once ImageResource finishes reloading, promote that same GLTexture wrapper to the fresh
        // external cid. This keeps managedTextures/dispose listeners singular and deterministic.
        if(!glTexture.__nativeExternalCid){
          if(!__acquireNativeExternal(texture,cid))return __oldUpdateTexture.apply(this,arguments);
          var oldHandle=glTexture.texture;
          try{this.unbind(texture);}catch(e){}
          try{if(oldHandle)this.gl.deleteTexture(oldHandle);}catch(e){}
          glTexture.texture={__id:cid,__kind:'texture',__nativeExternal:1};glTexture.__nativeExternalCid=cid;glTexture.__nativeExternalRetained=1;glTexture.__nativeExternalInvalid=0;
          globalThis.__nativeResourceAdoptions=(globalThis.__nativeResourceAdoptions||0)+1;
        }
        if(glTexture.__nativeExternalCid!==cid){
          // Acquire the replacement first. If it is stale too, keep the old live handle until the
          // source reload completes rather than turning a transient cache miss into blank reels.
          if(!__acquireNativeExternal(texture,cid))return;
          try{if(glTexture.__nativeExternalRetained)__nativeResourceRelease(glTexture.__nativeExternalCid);}catch(e){}
          glTexture.__nativeExternalCid=cid;glTexture.texture={__id:cid,__kind:'texture',__nativeExternal:1};
          glTexture.__nativeExternalRetained=1;glTexture.__nativeExternalInvalid=0;
          this.gl.bindTexture(texture.target,glTexture.texture);
        }
        this.initTextureType(texture,glTexture);glTexture.width=texture.realWidth;glTexture.height=texture.realHeight;
        texture.alphaMode=__nativeNeedsPremultiplyOf(texture)?PIXI.ALPHA_MODES.NPM:PIXI.ALPHA_MODES.PMA;texture.__texRealUp=1;texture.__texDataPending=0;
        if(texture.dirtyStyleId!==glTexture.dirtyStyleId)this.updateTextureStyle(texture);glTexture.dirtyId=texture.dirtyId;return;
      };
      __tsp.destroyTexture=function(texture,skipRemove){
        var bt=texture&&texture.castToBaseTexture?texture.castToBaseTexture():texture;
        var glTexture=bt&&bt._glTextures&&bt._glTextures[this.CONTEXT_UID],cid=glTexture&&glTexture.__nativeExternalCid,retained=glTexture&&glTexture.__nativeExternalRetained;
        var result=__oldDestroyTexture.apply(this,arguments);
        if(cid&&retained){glTexture.__nativeExternalRetained=0;try{__nativeResourceRelease(cid);}catch(e){}globalThis.__nativeResourceReleases=(globalThis.__nativeResourceReleases||0)+1;}
        return result;
      };
      __log('[native-resource] Pixi ImageResource 已切换为 Native GPU texture handle');
    }
    // 所有权协议的显式释放路之一:BaseTexture.destroy → resource.destroy → dispose 把
    // source 置 null,从此没人再持有这个 Image → 它 adopt 的 native 引用在这里对称释放。
    // (另一条是 Image 换 src,见 game_host.js;被 GC 而两条都没走到的软引用不退也无害——
    // 软引用不挡驱逐,条目照常被预算回收。)
    // 注意这与 destroyTexture 里的 __nativeResourceRelease 互不相干:那是 Pixi glTexture 的
    // retain 对应项,这是 publish→adopt 引用的对应项,两层各自平衡。
    if(PIXI.BaseImageResource&&PIXI.BaseImageResource.prototype&&typeof globalThis.__nativeImgDisown==='function'&&!PIXI.BaseImageResource.prototype.__nativeImgDisposeHook){
      PIXI.BaseImageResource.prototype.__nativeImgDisposeHook=1;
      var __oldBaseImgDispose=PIXI.BaseImageResource.prototype.dispose;
      PIXI.BaseImageResource.prototype.dispose=function(){
        var src=this.source;
        if(src&&src.__nativeTextureOwned)try{globalThis.__nativeImgDisown(src);}catch(e){}
        return __oldBaseImgDispose.apply(this,arguments);
      };
    }
    // 【上传验证器·安装】挂在 TextureSystem.updateTexture 原型上:无论 bind 来自 flip 的 __nativeTexId
    // 仅在 __nativeTexId 内验证会全部漏网),每次 updateTexture 都用 __texUpDataN 差值验证
    // "是否真的发出了带像素的上传";没发出且资源存在 → 挂 __texDataPending,由 __nativeTexId 的
    // 重试路径补传。RenderTexture(resource 空,内容来自 FBO)豁免。
    var __installUpdateTexHook=function(){
      if(globalThis.__utHookDone)return; var __r=globalThis.__nativeRenderer; if(!__r||!__r.texture)return;
      var __proto=Object.getPrototypeOf(__r.texture); if(!__proto||typeof __proto.updateTexture!=='function')return;
      globalThis.__utHookDone=1;
      var _ut=__proto.updateTexture;
      __proto.updateTexture=function(texture){
        var __u0=globalThis.__texUpDataN|0;
        var __rr=_ut.apply(this,arguments);
        if(texture&&texture.resource&&__nativeExternalCid(texture)){
          texture.__texRealUp=1;texture.__texDataPending=0;
        }else if(texture&&texture.resource){
          if((globalThis.__texUpDataN|0)>__u0){ texture.__texRealUp=1; texture.__texDataPending=0; }
          else if(!texture.__texRealUp&&texture.valid&&texture.__texDataPending!==2){
            texture.__texDataPending=1;
            if(texture.__texPendF===undefined)texture.__texPendF=globalThis.__frameNo|0;
          }
        }
        return __rr;
      };
      __log('[tex-verify] TextureSystem.updateTexture 上传验证器已装(覆盖离屏/cacheAsBitmap 首绑路径)');
    };
    // 所有 native 图元统一从这里取得 decoder texture id。首次遇到的新纹理会同步触发 Pixi 上传并立即重读，
    // 避免各图元各自实现时出现“本帧上传、下帧才可见”的空白窗口。
    var __nativeTexId = function(bt, upload){
      if(!globalThis.__utHookDone)__installUpdateTexHook();
      var id=__readTexId(bt),__externalPromote=false;
      if(id&&bt&&bt._glTextures&&globalThis.__nativeRenderer){
        var __externalWrap=bt._glTextures[globalThis.__nativeRenderer.CONTEXT_UID];
        if(!__validateNativeExternalWrap(bt,__externalWrap))id=0;
        else __externalPromote=__nativeExternalWrapNeedsPromotion(bt,__externalWrap);
      }
      // KTX2 可能在上传验证器安装前已被 Pixi 首绑。CompressedTextureResource.upload() 当时若
      // levelBuffers 尚未就绪会 return false，但 Pixi 仍把 dirtyId 标成已处理；句柄存在、纹理内容为空，
      // 后续永远不会再进 updateTexture。每个压缩 BaseTexture 在主 context 首次被 native 使用时
      // 强制验证上传一次，成功后 __texRealUp 收敛，不产生逐帧重传。
      var __rn=bt&&bt.resource&&bt.resource.constructor&&bt.resource.constructor.name;
      if(id&&__rn==='CompressedTextureResource'&&!bt.__texRealUp&&!bt.__texCompressedVerifyStarted){
        bt.__texCompressedVerifyStarted=1;
        bt.__texDataPending=1;
        bt.__texPendF=(globalThis.__frameNo|0)-15;
      }
      // dirtyId 失配 = 资源在首绑之后才更新(自定义 KTX2 的 zstd 异步解压、后到的像素、canvas 重绘等):
      // 浏览器 Pixi 每帧渲染都会 TextureSystem.bind → 自动补传;flip 路径原先只在"取不到 id"时 bind 一次,
      // 首绑时 CompressedTextureResource._levelBuffers 未就绪 → upload() return false → 纹理停留在
      // "已分配未上传"= 黑色剪影,且永不重试(m96380 图标 spine 外链 ktx2 图集黑块的根因;
      // black2.log 佐证:几十张 CompressedTextureResource 绑定却 0 条 [astc-up] 真实上传)。
      // 这里对齐浏览器语义:dirtyId 失配就重新 bind,数据到位那一帧即补传。
      // 【上传验证器】pixi updateTexture 结尾**无条件** glTexture.dirtyId=texture.dirtyId——异步资源
      // (自定义 KTX2 zstd 解压、晚到的像素)首绑时 upload() return false 也被当成"已消费",永不重试
      // → 黑色剪影。不猜资源内部结构:以 game_host 编码器的 __texUpDataN(真实带像素上传命令计数,
      // texImage2D/texSubImage2D/compressedTexImage2D 都记)为准——bind 前后计数没涨 = 这纹理没传成,
      // 挂 __texDataPending,之后每 15 帧把主上下文句柄 dirtyId 置 -1 强制重试一次,传成即收敛。
      // RenderTexture(bt.resource 空,内容来自 FBO)不适用;约 40 次(~10s)仍失败放弃并告警。
      // ⚠️ 失配判定只看主渲染器 CONTEXT_UID 那份句柄:离屏/工具渲染器(cacheAsBitmap/generateTexture)
      var stale=false;
      if(id && bt && bt._glTextures && globalThis.__nativeRenderer){
        var __gw=bt._glTextures[globalThis.__nativeRenderer.CONTEXT_UID];
        if(__gw && __gw.dirtyId!==undefined && __gw.dirtyId!==bt.dirtyId) stale=true;
      }
      // A retain miss deliberately falls back to Pixi's ordinary GLTexture while native reloads
      // the Image. That placeholder has a valid non-zero GL name, so id/stale alone can mistake it
      // for final content. Once the source publishes a fresh cid, force TextureSystem.updateTexture
      // to promote the existing wrapper in place.
      if(__externalPromote)stale=true;
      var __pendRetry = !!(bt && bt.__texDataPending===1 && bt.resource && !bt.__texRealUp
        && ((globalThis.__frameNo|0)-(bt.__texPendF|0))>=15);
      if((!id||stale||__pendRetry) && upload && globalThis.__NATIVE_SCREEN===1 && globalThis.__nativeRenderer && bt && bt.valid){
        var __wasFirst=!id;
        if(__externalPromote && bt._glTextures){
          var __gwe=bt._glTextures[globalThis.__nativeRenderer.CONTEXT_UID];
          if(__gwe)__gwe.dirtyId=-1;
        }
        if(__pendRetry && id && !stale && bt._glTextures){
          var __gwp=bt._glTextures[globalThis.__nativeRenderer.CONTEXT_UID];
          if(__gwp) __gwp.dirtyId=-1;   // 强制 updateTexture 重跑(pixi 已把失败标成已消费,只能硬掰)
        }
        var __up0=globalThis.__texUpDataN|0;
        try{ globalThis.__nativeRenderer.texture.bind(bt,0); id=__readTexId(bt); }
        catch(e){ if((globalThis.__texBindErrN||0)<8){ globalThis.__texBindErrN=(globalThis.__texBindErrN||0)+1;
          __log('[tex-bind-err] '+e.message+' res='+(bt.resource&&bt.resource.constructor&&bt.resource.constructor.name)+' '+bt.realWidth+'x'+bt.realHeight); } }
        if((globalThis.__texUpDataN|0)>__up0){ bt.__texRealUp=1; bt.__texDataPending=0;
          if(!__wasFirst&&globalThis.__SP_FIRST_DIAG===1&&(globalThis.__texHealLogs||0)<40){ globalThis.__texHealLogs=(globalThis.__texHealLogs||0)+1;
            __log('[tex-heal] f='+(globalThis.__frameNo|0)+' id='+id+' '+bt.realWidth+'x'+bt.realHeight+' 重试第'+(bt.__texPendN|0)+'次后真实上传成功'); } }
        else if(bt.resource && !bt.__texRealUp){
          bt.__texDataPending=(bt.__texPendN|0)>=40?2:1;   // 2=放弃(资源确实无数据,如加载失败)
          bt.__texPendF=globalThis.__frameNo|0;
          bt.__texPendN=(bt.__texPendN|0)+1;
          if(bt.__texDataPending===2&&(globalThis.__texGiveupLogs||0)<8){ globalThis.__texGiveupLogs=(globalThis.__texGiveupLogs||0)+1;
            __log('[tex-pending-giveup] '+bt.realWidth+'x'+bt.realHeight+' res='+(bt.resource.constructor&&bt.resource.constructor.name)+' url='+String(bt.resource.url||'').slice(-80)); }
        }
        if(globalThis.__SP_FIRST_DIAG===1){
          if(__wasFirst&&(globalThis.__texBindLogs||0)<60){
            globalThis.__texBindLogs=(globalThis.__texBindLogs||0)+1;
            var __tbRes=bt.resource,__tbSrc=__tbRes&&(__tbRes.source||__tbRes.data);
            __log('[tex-bind] f='+(globalThis.__frameNo||0)+' id='+id+' valid='+(bt.valid?1:0)+' '+bt.realWidth+'x'+bt.realHeight
              +' alphaMode='+bt.alphaMode+' res='+(__tbRes&&__tbRes.constructor&&__tbRes.constructor.name)
              +' src='+(__tbSrc&&__tbSrc.constructor&&__tbSrc.constructor.name)
              +' hasData='+!!(__tbSrc&&__tbSrc.__data)+' complete='+(__tbSrc&&__tbSrc.complete)
              +' url='+String((__tbSrc&&(__tbSrc.src||__tbSrc.__srcURL))||(__tbRes&&__tbRes.url)||'').slice(-90));
          }
          if(!__wasFirst&&(globalThis.__texRebindLogs||0)<40){
            globalThis.__texRebindLogs=(globalThis.__texRebindLogs||0)+1;
            __log('[tex-rebind] f='+(globalThis.__frameNo||0)+' id='+id+' '+bt.realWidth+'x'+bt.realHeight
              +' res='+(bt.resource&&bt.resource.constructor&&bt.resource.constructor.name)+' dirtyId='+bt.dirtyId);
          }
        }
      }
      if(id&&bt&&globalThis.__nativeApplyTextureStyle)globalThis.__nativeApplyTextureStyle(bt,globalThis.__nativeRenderer);
      return id;
    };
    var __clearNativeFilters=function(i){var off=FB+i*FSF,id=__cmIdentity();for(var z=0;z<FSF;z++)SGB[off+z]=0;SGB[off]=1;for(var k=0;k<20;k++)SGB[off+4+k]=id[k];SGB[FFB+i]=0;};
    // QB/QSF/QCAP 与 scenegraph.h 的持久 quad 区严格对齐。
    var __qTop=0, __qFree=[], __qOwners=new Set(), __qActiveRoot=null, __qReclaimed=0, __qSweepTick=0;
    var __linkedToRoot=function(o,root){
      if(!root)return true;
      for(var p=o,g=0;p&&g++<2048;p=p.parent)if(p===root)return true;
      return false;
    };
    var __qMerge = function(){
      if(!__qFree.length) return;
      __qFree.sort(function(a,b){ return a.off-b.off; });
      var w=0;
      for(var i=0;i<__qFree.length;i++){
        var cur=__qFree[i], prev=w?__qFree[w-1]:null;
        if(prev && prev.off+prev.cap===cur.off) prev.cap+=cur.cap;
        else __qFree[w++]=cur;
      }
      __qFree.length=w;
      while(__qFree.length){ var last=__qFree[__qFree.length-1]; if(last.off+last.cap!==__qTop) break; __qTop=last.off; __qFree.pop(); }
      SGB[QB]=__qTop;
    };
    var __qRelease = function(o, detached){
      if(!o || !(o.__qCap>0)) return;
      __qFree.push({off:o.__qOff|0,cap:o.__qCap|0}); o.__qOff=undefined; o.__qCap=0; __qOwners.delete(o); __qMerge();
      if(detached){
        o.__qReleased=1;
        var top=o;while(top.parent)top=top.parent;
        top.__qReleasedTree=1;
      }
      if(o.__i>0){ var b=o.__i*ST; SGB[b+oQUADOFF]=0; SGB[b+oQUADCNT]=0; }
    };
    // Pixi removeChild 只把旧场景根从 stage 摘掉，内部子树仍互相保留 parent。若只判断
    // o.parent，旧场景的 Spine 会继续每帧改写并永久占住 quad 池。池压较高/分配失败时，
    // 释放所有不再从当前 stage 可达的块；缓存树以后重挂时由 addChild 递归重建。
    var __qReclaimDetached=function(root){
      if(!root||!__qOwners.size)return 0;
      var old=Array.from(__qOwners),n=0;
      for(var i=0;i<old.length;i++){var o=old[i];if(o&&o.__qCap>0&&!__linkedToRoot(o,root)){__qRelease(o,true);n++;}}
      if(n){__qReclaimed+=n;if(globalThis.__SG_DIAG===1)__log('[quad-gc] 回收树外几何 '+n+' 块, q='+__qTop+'/'+QCAP+' free='+__qFree.length);}
      return n;
    };
    var __qTake=function(cap){
      var best=-1;
      for(var i=0;i<__qFree.length;i++) if(__qFree[i].cap>=cap && (best<0 || __qFree[i].cap<__qFree[best].cap)) best=i;
      if(best>=0){var block=__qFree[best],off=block.off;if(block.cap===cap)__qFree.splice(best,1);else{block.off+=cap;block.cap-=cap;}return off;}
      if(__qTop+cap>QCAP)return -1;
      var off2=__qTop;__qTop+=cap;SGB[QB]=__qTop;return off2;
    };
    var __qEnsure = function(o, need){
      need=Math.ceil(need||0); if(!(need>0) || need>QCAP) return -1;
      if(o.__qCap>=need) return o.__qOff|0;
      var cap=4; while(cap<need && cap<QCAP) cap*=2; if(cap>QCAP) return -1;
      __qRelease(o);
      var off=__qTake(cap);
      if(off<0&&__qActiveRoot){__qReclaimDetached(__qActiveRoot);off=__qTake(cap);}
      if(off<0)return -1;
      o.__qOff=off; o.__qCap=cap;o.__qReleased=0;__qOwners.add(o);return off;
    };
    globalThis.__sgQuadStats=function(){ return {used:__qTop,capacity:QCAP,freeBlocks:__qFree.length,owners:__qOwners.size,reclaimed:__qReclaimed,dynamic:0,spineSlotDynamic:0,filterDynamic:0,dirty:__renderDirty.size,detachedDirty:__renderDetachedDirty.size}; };
    var FB=QB+1+QCAP*QSF, FSF=224;   // direct filters + native offscreen post-process descriptors
    var __filterNativeKind=function(f){ var n=(f&&f.constructor&&f.constructor.name)||'',u=f&&f.uniforms; if(n.indexOf('DropShadowFilter')>=0||(f&&f._tintFilter&&f._blurFilter&&f.shadowOnly!==undefined))return 5; if(n.indexOf('KawaseBlurFilter')>=0||f&&f.__nativeKawase===true||(f&&Array.isArray(f._kernels)&&f._pixelSize))return 10; if(n.indexOf('BlurFilter')>=0)return 2; if(n.indexOf('AlphaFilter')>=0)return 1; if(n.indexOf('ColorMatrixFilter')>=0)return 3; if(n.indexOf('NoiseFilter')>=0||(u&&u.uNoise!==undefined&&u.uSeed!==undefined))return 6; if(n.indexOf('TwistFilter')>=0||(u&&u.radius!==undefined&&u.angle!==undefined&&u.offset!==undefined))return 7; if(n.indexOf('FXAAFilter')>=0||f&&f.__nativeFXAA===true)return 8; if(n.indexOf('DisplacementFilter')>=0||f&&f.__nativeDisplacement===true||(f&&f.maskSprite&&u&&u.mapSampler&&u.rotation))return 9; if(n.indexOf('ColorOverlayFilter')>=0||(u&&u.color&&u.color.length>=3&&u.alpha!==undefined&&!u.shift))return 4; return 0; };
    var __filtersAllNative=function(fs){if(!fs||!fs.length)return true;for(var i=0;i<fs.length;i++)if(fs[i]&&fs[i].enabled!==false&&!__filterNativeKind(fs[i]))return false;return true;};
    var __filterPayloadContract=globalThis.__nativeFilterPayloadContract;
    if(!__filterPayloadContract||typeof __filterPayloadContract.snapshot!=='function'||typeof __filterPayloadContract.equal!=='function')throw new Error('native_filter_payload_contract missing');
    // Property accessors cannot observe `typedArray[index] = value`. GSAP PixiPlugin and other
    // animation systems use exactly that path for ColorMatrixFilter, so keep the small set of
    // owners with mutable encoded sequences and compare value snapshots immediately before draw.
    var __mutableFilterOwners=globalThis.__sgMutableFilterOwners||(globalThis.__sgMutableFilterOwners=new Set());
    var __filterPayloadSnapshot=function(fs){return __filterPayloadContract.snapshot(fs,__filterNativeKind);};
    var __cmIdentity=function(){return [1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,1,0];};
    var __cmMul=function(outer,inner){var out=new Array(20);for(var r=0;r<4;r++){var ro=r*5;for(var c=0;c<4;c++){var v=0;for(var k=0;k<4;k++)v+=outer[ro+k]*inner[k*5+c];out[ro+c]=v;}var q=outer[ro+4];for(var k=0;k<4;k++)q+=outer[ro+k]*inner[k*5+4];out[ro+4]=q;}return out;};
    var __filterMarkOwners=function(target){var owners=target&&target.__sgFilterOwners;if(owners)owners.forEach(__markRender);};
    var __watchFilterField=function(target,name,o){
      if(!target)return;var fields=target.__sgFilterFields||(target.__sgFilterFields=Object.create(null));if(!target.__sgFilterOwners)target.__sgFilterOwners=new Set();target.__sgFilterOwners.add(o);if(fields[name])return;
      var own=Object.getOwnPropertyDescriptor(target,name),p=Object.getPrototypeOf(target),d=own;while(!d&&p){d=Object.getOwnPropertyDescriptor(p,name);p=Object.getPrototypeOf(p);}if(d&&!d.configurable&&own)return;
      try{
        if(d&&d.get&&d.set){Object.defineProperty(target,name,{configurable:true,enumerable:d.enumerable,get:function(){return d.get.call(this);},set:function(v){var before=d.get.call(this);d.set.call(this,v);if(before!==d.get.call(this))__filterMarkOwners(this);}});}
        else{var value=target[name],tag='__sgFilterValue_'+name;target[tag]=value;Object.defineProperty(target,name,{configurable:true,enumerable:true,get:function(){return this[tag];},set:function(v){var changed=this[tag]!==v;this[tag]=v;if(changed)__filterMarkOwners(this);}});}
        fields[name]=1;
      }catch(e){}
    };
    var __watchFilterValue=function(f,value,o){
      if(!value||typeof value!=='object')return;if(!f.__sgFilterTargets)f.__sgFilterTargets=new Set();f.__sgFilterTargets.add(value);
      if(!value.__sgFilterOwners)value.__sgFilterOwners=new Set();value.__sgFilterOwners.add(o);
      ['x','y','width','height'].forEach(function(name){if(name in value)__watchFilterField(value,name,o);});
    };
    var __watchFilter=function(f,o){
      if(!f||!o)return;if(!f.__sgOwners)f.__sgOwners=new Set();f.__sgOwners.add(o);if(!f.__sgFilterOwners)f.__sgFilterOwners=f.__sgOwners;else f.__sgFilterOwners.add(o);
      var fields=['enabled','padding','quality','repeatEdgePixels','shadowOnly','alpha','blur','blurX','blurY','color','noise','seed','radius','angle','offset','scale','map','maskSprite','kernels','pixelSize','clamp','_kernels','_pixelSize','_clamp','matrix'];
      for(var fi=0;fi<fields.length;fi++){var name=fields[fi];if(name in f)__watchFilterField(f,name,o);try{__watchFilterValue(f,f[name],o);}catch(e){}}
      var uniforms=f.uniforms;if(uniforms){if(!f.__sgFilterTargets)f.__sgFilterTargets=new Set();f.__sgFilterTargets.add(uniforms);['uNoise','uSeed','radius','angle','offset','color','alpha','scale','mapSampler','rotation'].forEach(function(name){if(name in uniforms){__watchFilterField(uniforms,name,o);try{__watchFilterValue(f,uniforms[name],o);}catch(e){}}});}
      // ColorMatrixFilter mutates its Float32Array in place. Wrap its public mutation methods once;
      // ordinary scalar/vector filters are covered by the field hooks above.
      if(!f.__sgFilterMethods){f.__sgFilterMethods=1;['reset','brightness','greyscale','blackAndWhite','hue','contrast','saturate','desaturate','negative','sepia','technicolor','polaroid','toBGR','kodachrome','browni','vintage','colorTone','night','predator','lsd'].forEach(function(name){var fn=f[name];if(typeof fn!=='function')return;try{f[name]=function(){var result=fn.apply(this,arguments);__filterMarkOwners(this);return result;};}catch(e){}});}
    };
    // DisplacementFilter 的位移场描述符烘的是 maskSprite.worldTransform 快照;水波/热浪类效果
    // 每帧移动 maskSprite,而 __watchFilter 只监听 filter 自身字段 → native 位移场静止。
    // 记录带 Displacement 的 owner,每帧 present 前(世界矩阵已回写后)比对 maskSprite 世界矩阵
    // 签名,变化才重写描述符。owner 集合极小(Displacement 用得少),签名比对为每帧 O(集合大小)。
    var __dispOwners=globalThis.__sgDisplacementOwners||(globalThis.__sgDisplacementOwners=new Set());
    var __dispSigOf=function(fs){
      var sig='';
      if(fs)for(var i=0;i<fs.length;i++){var f=fs[i];if(!f||f.enabled===false||__filterNativeKind(f)!==9)continue;
        var ms=f.maskSprite||f.__nativeMaskSprite,mwt=ms&&ms.worldTransform;
        sig+=mwt?(mwt.a+','+mwt.b+','+mwt.c+','+mwt.d+','+mwt.tx+','+mwt.ty+';'):'x;';}
      return sig;
    };
    var __refreshDisplacementOwners=function(){
      if(!__dispOwners.size)return;
      __dispOwners.forEach(function(o){
        if(!o||o._destroyed||!(o.__i>0)||__sgObjs[o.__i]!==o){__dispOwners.delete(o);if(o)o.__sgDispSig=undefined;return;}
        var sig=__dispSigOf(o.filters);
        if(sig!==o.__sgDispSig){__wrFilters(o);globalThis.__sgDirty=1;}
      });
    };
    var __refreshMutableFilterOwners=function(){
      if(!__mutableFilterOwners.size)return;
      __mutableFilterOwners.forEach(function(o){
        if(!o||o._destroyed||!(o.__i>0)||__sgObjs[o.__i]!==o){__mutableFilterOwners.delete(o);if(o)o.__sgMutableFilterSnapshot=null;return;}
        var snapshot=__filterPayloadSnapshot(o.filters);
        if(!snapshot){__mutableFilterOwners.delete(o);o.__sgMutableFilterSnapshot=null;return;}
        if(!__filterPayloadContract.equal(snapshot,o.__sgMutableFilterSnapshot)){
          __wrFilters(o);globalThis.__sgDirty=1;
        }
      });
    };
    var __wrFilters=function(o){
      if(!o||!(o.__i>0))return; var off=FB+o.__i*FSF, alpha=1,bx=0,by=0,cm=__cmIdentity(),hasCM=0,overlay=null,shadow=null,shadowK=null,noise=0,noiseSeed=0,twist=null,fxaa=0,disp=null,kawase=null,gaussian=null,padding=0,unavailable=0,postOrder=[],postDescriptors=[],active=0,fs=o.filters,sawDisp=0;
      var addPost=function(kind,data){var p=new Array(20);for(var pi=0;pi<20;pi++)p[pi]=0;p[0]=kind;for(var pj=0;data&&pj<data.length&&pj<19;pj++)p[pj+1]=Number(data[pj])||0;postOrder.push(kind);postDescriptors.push(p);};
      if(o.__sgFilters)for(var z=0;z<o.__sgFilters.length;z++){var old=o.__sgFilters[z];if(old&&old.__sgOwners)old.__sgOwners.delete(o);if(old&&old.__sgFilterOwners)old.__sgFilterOwners.delete(o);if(old&&old.__sgFilterTargets)old.__sgFilterTargets.forEach(function(target){if(target&&target.__sgFilterOwners)target.__sgFilterOwners.delete(o);});} o.__sgFilters=[];
      if(fs)for(var i=0;i<fs.length;i++){var f=fs[i];if(!f)continue;__watchFilter(f,o);o.__sgFilters.push(f);if(f.enabled===false)continue;active++;padding=Math.max(padding,Math.max(0,Number(f.padding)||0));var kind=__filterNativeKind(f);
        if(kind===1)alpha*=Math.max(0,Math.min(1,f.alpha===undefined?1:f.alpha));
        else if(kind===2){var gx=Math.abs(Number(f.blurX===undefined?(f.blur||0):f.blurX)||0),gy=Math.abs(Number(f.blurY===undefined?(f.blur||0):f.blurY)||0),gq=Math.max(1,Math.round(Number(f.quality)||1)),gpass=f.blurXFilter||f,src=String(gpass&&((gpass.program&&(gpass.program.fragmentSrc||gpass.program.fragment))||gpass.fragmentSrc)||''),gm=src.match(/vBlurTexCoords\[(\d+)\]/),gk=gm?Number(gm[1]):5;if(!gx&&!gy){}else if(gq<=8&&[5,7,9,11,13,15].indexOf(gk)>=0){gaussian=[gx,gy,gq,gk,f.repeatEdgePixels?1:0];addPost(kind,gaussian);}else unavailable=1;}
        else if(kind===3&&f.matrix&&f.matrix.length>=20){var fm=Array.prototype.slice.call(f.matrix,0,20),mix=(f.alpha===undefined?1:f.alpha);if(mix<1){for(var mr=0;mr<3;mr++)for(var mc=0;mc<5;mc++){var mi=mr*5+mc,id=(mc===mr?1:0);fm[mi]=id+(fm[mi]-id)*mix;}}cm=__cmMul(fm,cm);hasCM=1;}
        else if(kind===4){var oc=f.uniforms&&f.uniforms.color,oa=f.alpha===undefined?(f.uniforms&&f.uniforms.alpha===undefined?1:f.uniforms.alpha):f.alpha;if(oc&&oc.length>=3)overlay=[Number(oc[0])||0,Number(oc[1])||0,Number(oc[2])||0,Math.max(0,Math.min(1,Number(oa)||0))];}
        else if(kind===5){var so=f.offset||f._offset||{x:0,y:0},sc=f._tintFilter&&f._tintFilter.uniforms&&f._tintFilter.uniforms.color,sa=f.alpha===undefined?.5:f.alpha;if(!sc){var sn=Number(f.color)||0;sc=[((sn>>16)&255)/255,((sn>>8)&255)/255,(sn&255)/255];}shadow=[Number(so.x)||0,Number(so.y)||0,Math.max(0,Number(f.blur)||0),Number(sc[0])||0,Number(sc[1])||0,Number(sc[2])||0,Math.max(0,Math.min(1,Number(sa)||0)),f.shadowOnly?1:0];var sb=f._blurFilter,skn=sb&&(sb.kernels||sb._kernels),sps=sb&&(sb.pixelSize||sb._pixelSize)||{x:1,y:1};if(skn&&skn.length>0&&skn.length<=8){shadowK=[skn.length,Number(sps.x===undefined?sps[0]:sps.x)||1,Number(sps.y===undefined?sps[1]:sps.y)||1,sb.clamp||sb._clamp?1:0];for(var si=0;si<skn.length;si++)shadowK.push(Number(skn[si])||0);var sd=[shadow[0],shadow[1],shadow[3],shadow[4],shadow[5],shadow[6],shadow[7],shadowK[0],shadowK[1],shadowK[2],shadowK[3]];for(var sj=0;sj<8;sj++)sd.push(sj<skn.length?Number(skn[sj])||0:0);addPost(kind,sd);}else unavailable=1;}
        else if(kind===6){var nu=f.uniforms||{},nn=f.noise===undefined?nu.uNoise:f.noise,ns=f.seed===undefined?nu.uSeed:f.seed;noise=Math.min(1,noise+Math.max(0,Number(nn)||0));noiseSeed=Number(ns)||0;}
        else if(kind===7){var tu=f.uniforms||{},to=f.offset===undefined?tu.offset:f.offset;twist=[Math.max(0,Number(f.radius===undefined?tu.radius:f.radius)||0),Number(f.angle===undefined?tu.angle:f.angle)||0,Number(to&&to.x!==undefined?to.x:to&&to[0])||0,Number(to&&to.y!==undefined?to.y:to&&to[1])||0,Math.max(0,Number(f.padding)||0)];addPost(kind,twist);}
        else if(kind===8){fxaa=1;addPost(kind,null);}
        else if(kind===9){
          sawDisp=1;
          var du=f.uniforms||{},ms=f.maskSprite||f.__nativeMaskSprite,mt=f.map||du.mapSampler||(ms&&ms._texture),mbt=mt&&mt.baseTexture,dtid=__nativeTexId(mbt,true),mwt=ms&&ms.worldTransform,orig=mt&&mt.orig;
          if(dtid&&mwt&&orig&&orig.width>0&&orig.height>0){
            var uv=mt._uvs;if(!uv&&mt.updateUvs)try{mt.updateUvs();uv=mt._uvs;}catch(e){}
            var u0=uv?Number(uv.x0)||0:0,v0=uv?Number(uv.y0)||0:0,u1=uv?Number(uv.x1):1,v1=uv?Number(uv.y1)||0:0,u3=uv?Number(uv.x3)||0:0,v3=uv?Number(uv.y3):1;
            if(!isFinite(u1))u1=1;if(!isFinite(v3))v3=1;
            var wa=Number(mwt.a)||0,wb=Number(mwt.b)||0,wc=Number(mwt.c)||0,wd=Number(mwt.d)||0,wtx=Number(mwt.tx)||0,wty=Number(mwt.ty)||0,det=wa*wd-wb*wc;
            if(Math.abs(det)>1e-8){
              var ia=wd/det,ib=-wb/det,ic=-wc/det,id=wa/det,itx=-(ia*wtx+ic*wty),ity=-(ib*wtx+id*wty),iw=1/orig.width,ih=1/orig.height,ax=ms.anchor?Number(ms.anchor.x)||0:0,ay=ms.anchor?Number(ms.anchor.y)||0:0;
              var dux=u1-u0,dvx=v1-v0,duy=u3-u0,dvy=v3-v0,A=dux*ia*iw+duy*ib*ih,B=dvx*ia*iw+dvy*ib*ih,C=dux*ic*iw+duy*id*ih,D=dvx*ic*iw+dvy*id*ih,TX=u0+dux*(itx*iw+ax)+duy*(ity*ih+ay),TY=v0+dvx*(itx*iw+ax)+dvy*(ity*ih+ay);
              var lenX=Math.sqrt(wa*wa+wb*wb),lenY=Math.sqrt(wc*wc+wd*wd),sc=f.scale||du.scale||{x:20,y:20};
              if(lenX>1e-8&&lenY>1e-8){disp=[dtid,Number(sc.x===undefined?sc[0]:sc.x)||0,Number(sc.y===undefined?sc[1]:sc.y)||0,A,B,C,D,TX,TY,wa/lenX,wb/lenX,wc/lenY,wd/lenY];__addBaseTextureOwner(mbt,o);}
            }
          }
          // map 纹理迟到(异步图集/KTX 解压时 dtid=0)→ 注册纹理 owner,上传完成的 update 事件会
          // __markRender(o) 重跑本函数;否则 filter-resource 逃逸永不自愈。
          if(!disp){unavailable=1;if(mbt)__addBaseTextureOwner(mbt,o);}else addPost(kind,disp);
        }
        else if(kind===10){var kernels=f.kernels||f._kernels,ps=f.pixelSize||f._pixelSize||{x:1,y:1};if(kernels&&kernels.length>0&&kernels.length<=12){kawase=[kernels.length,f.clamp||f._clamp?1:0,Number(ps.x===undefined?ps[0]:ps.x)||1,Number(ps.y===undefined?ps[1]:ps.y)||1];for(var ki=0;ki<kernels.length;ki++)kawase.push(Number(kernels[ki])||0);addPost(kind,kawase);}else unavailable=1;}
      }
      SGB[off]=alpha;SGB[off+1]=Math.min(64,bx);SGB[off+2]=Math.min(64,by);SGB[off+3]=hasCM;
      for(var j=0;j<20;j++)SGB[off+4+j]=cm[j];
      SGB[off+24]=overlay?1:0;SGB[off+25]=overlay?overlay[0]:0;SGB[off+26]=overlay?overlay[1]:0;SGB[off+27]=overlay?overlay[2]:0;SGB[off+28]=overlay?overlay[3]:0;
      SGB[off+29]=shadow?1:0;for(var sk=0;sk<8;sk++)SGB[off+30+sk]=shadow?shadow[sk]:0;
      SGB[off+38]=noise;SGB[off+39]=noiseSeed;
      SGB[off+40]=twist?1:0;for(var tw=0;tw<4;tw++)SGB[off+41+tw]=twist?twist[tw]:0;SGB[off+45]=Math.max(padding,fxaa?1:0);SGB[off+46]=fxaa;
      SGB[off+47]=disp?1:0;for(var dp=0;dp<13;dp++)SGB[off+48+dp]=disp?disp[dp]:0;
      SGB[off+61]=kawase?1:0;SGB[off+62]=kawase?kawase[0]:0;SGB[off+63]=kawase?kawase[1]:0;SGB[off+64]=kawase?kawase[2]:0;SGB[off+65]=kawase?kawase[3]:0;for(var kk=0;kk<12;kk++)SGB[off+66+kk]=kawase&&kk<kawase[0]?kawase[4+kk]:0;SGB[off+78]=SGB[off+79]=0;
      SGB[off+78]=gaussian?1:0;SGB[off+79]=gaussian?gaussian[0]:0;SGB[off+80]=gaussian?gaussian[1]:0;SGB[off+81]=gaussian?gaussian[2]:0;SGB[off+82]=gaussian?gaussian[3]:0;SGB[off+83]=gaussian?gaussian[4]:0;
      SGB[off+84]=shadowK?shadowK[0]:0;SGB[off+85]=shadowK?shadowK[1]:0;SGB[off+86]=shadowK?shadowK[2]:0;for(var sr=0;sr<8;sr++)SGB[off+87+sr]=shadowK&&sr<shadowK[0]?shadowK[4+sr]:0;SGB[off+95]=shadowK?shadowK[3]:0;
      SGB[off+96]=postOrder.length;for(var po=0;po<6;po++)SGB[off+97+po]=po<postOrder.length?postOrder[po]:0;SGB[off+103]=2;
      for(var pd=0;pd<6;pd++)for(var ps=0;ps<20;ps++)SGB[off+104+pd*20+ps]=pd<postDescriptors.length?postDescriptors[pd][ps]:0;
      // HAS_FILTER 位:任一非默认 filter 状态置 1(判定集合与 C++ refreshFilterFlag 一致);
      // 全默认清 0 → batcher 热路径跳过该节点的整个 FILTER 区读取。
      SGB[FFB+o.__i]=(alpha!==1||bx>0||by>0||hasCM||overlay||shadow||noise>0||twist||fxaa||disp||kawase||gaussian||postOrder.length>0)?1:0;
      var conflict=postOrder.length>6?1:0;if((o.__sgFilterUnavailable|0)!==unavailable||(o.__sgPostFilterConflict|0)!==conflict){o.__sgFilterUnavailable=unavailable;o.__sgPostFilterConflict=conflict;if(o.__sgFilterUnavailableReady!==undefined)__markRender(o);o.__sgFilterUnavailableReady=1;}
      // Displacement owner 集合成员资格由本函数唯一维护:filters 清空/替换时自动退出。
      if(sawDisp){__dispOwners.add(o);o.__sgDispSig=__dispSigOf(fs);}
      else if(o.__sgDispSig!==undefined){__dispOwners.delete(o);o.__sgDispSig=undefined;}
      var mutableSnapshot=__filterPayloadSnapshot(fs);
      if(mutableSnapshot){__mutableFilterOwners.add(o);o.__sgMutableFilterSnapshot=mutableSnapshot;}
      else{__mutableFilterOwners.delete(o);o.__sgMutableFilterSnapshot=null;}
    };
    var __wrTex = function(o){          // sprite 纹理/rect/uv/tint → slot(纹理、anchor、tint 变化时调)
      if(!o || o.__i===undefined || o.__i<=0 || !o.isSprite) return;
      // NativeNodeProxy 用 _nativeWidth/_nativeHeight 保存 FGUI 设定后的显示尺寸。
      // 通用 Pixi Sprite 路径只看 texture.orig，会把例如 1075x783 的弹框宣传图
      // 重新覆盖为图集原始的 756x545。交还代理自己的刷新逻辑，同时加重入保护，
      // 因为首次 texture bind 可能同步触发纹理 owner 更新回调。
      if(o.__nativeNodeProxy&&typeof o._refreshSprite==='function'){
        if(o.__sgRefreshingProxyTexture)return;
        o.__sgRefreshingProxyTexture=1;
        try{o._refreshSprite();}finally{o.__sgRefreshingProxyTexture=0;}
        return;
      }
      var b=o.__i*ST, tex=o._texture;
      if(!tex || !tex.valid || !tex.orig){ SGB[b+oTEXID]=0; return; }   // 销毁中的 texture:valid 可能残留 true 但 orig=null
      var orig=tex.orig, trim=tex.trim, ax=o._anchor?o._anchor._x:0, ay=o._anchor?o._anchor._y:0;
      var w=orig.width, h=orig.height, x0,y0,rw,rh;
      if(trim){ x0=trim.x-ax*w; y0=trim.y-ay*h; rw=trim.width; rh=trim.height; }
      else { x0=-ax*w; y0=-ay*h; rw=w; rh=h; }
      SGB[b+oRX]=x0; SGB[b+oRX+1]=y0; SGB[b+oRX+2]=rw; SGB[b+oRX+3]=rh;
      // Pixi normally creates lazy atlas UVs from calculateVertices(). Native-only mode bypasses that
      // render path, so controller-created/initially hidden FGUI images can otherwise keep all-zero UVs.
      var uvs=tex._uvs;
      if(!uvs && tex.updateUvs){try{tex.updateUvs();uvs=tex._uvs;}catch(e){}}
      if(!uvs){
        SGB[b+oTEXID]=0;
        if((o.__sgUvRetry=(o.__sgUvRetry||0)+1)<=120)__markRender(o);
        return;
      }
      o.__sgUvRetry=0;
      SGB[b+oUV]=uvs.x0;SGB[b+oUV+1]=uvs.y0;SGB[b+oUV+2]=uvs.x1;SGB[b+oUV+3]=uvs.y1;SGB[b+oUV+4]=uvs.x2;SGB[b+oUV+5]=uvs.y2;SGB[b+oUV+6]=uvs.x3;SGB[b+oUV+7]=uvs.y3;
      var id=__nativeTexId(tex.baseTexture, true);
      SGB[b+oTEXID]=id;
      SGB[b+oBLEND]=o.__nativeNodeProxy?(o._blendMode||0):(o.blendMode||0);
      SGB[b+oTINT]=__tintOf(o);   // filter tint 在 native DFS 按容器层级继承
      SGB[b+oPREMUL]=__premulOf(tex.baseTexture);   // M6:纹理非预乘(ASTC)→ shader 内预乘修白框
      globalThis.__sgDirty=1;   // 纹理/anchor/tint 变化 → 置脏(静止帧跳过的判定源)
    };
    var oMASKI=19;
    var __rectMaskOf = function(m){     // m 是"单矩形无孔洞 Graphics"→ 返回 shape(Rectangle),否则 null
      try {
        if(!m || !m.geometry || !m.geometry.graphicsData || m.geometry.graphicsData.length!==1) return null;
        var gd=m.geometry.graphicsData[0];
        var st=gd.shape&&gd.shape.type, isRect=(st===PIXI.SHAPES.RECT) || (PIXI.SHAPES.RREC!==undefined && st===PIXI.SHAPES.RREC);
        if(!gd.shape || !isRect || (gd.holes&&gd.holes.length)) return null;
        if(gd.matrix){
          // Pixi records some rectangular masks with a per-shape matrix.  Preserve the exact
          // stencil mesh, but provide its transformed local AABB for native mask bookkeeping.
          var sh=gd.shape,mx=gd.matrix,x=Number(sh.x)||0,y=Number(sh.y)||0,w=Number(sh.width)||0,h=Number(sh.height)||0;
          if(!(w>0&&h>0))return null;var aa=Number(mx.a);if(!isFinite(aa))aa=1;var bb=Number(mx.b)||0,cc=Number(mx.c)||0,dd=Number(mx.d);if(!isFinite(dd))dd=1;var tx=Number(mx.tx)||0,ty=Number(mx.ty)||0;
          var xs=[aa*x+cc*y+tx,aa*(x+w)+cc*y+tx,aa*(x+w)+cc*(y+h)+tx,aa*x+cc*(y+h)+tx],ys=[bb*x+dd*y+ty,bb*(x+w)+dd*y+ty,bb*(x+w)+dd*(y+h)+ty,bb*x+dd*(y+h)+ty],minX=Math.min.apply(Math,xs),maxX=Math.max.apply(Math,xs),minY=Math.min.apply(Math,ys),maxY=Math.max.apply(Math,ys);
          return{x:minX,y:minY,width:maxX-minX,height:maxY-minY,__geometry:true};
        }
        return gd.shape;
      } catch(e){ return null; }
    };
    var __spriteMaskRectOf = function(m){
      try{
        if(!m || !m.isSprite) return null;
        var tex=m._texture||m.texture, orig=tex&&tex.orig; if(!orig) return null;
        var ax=m._anchor?m._anchor._x:0, ay=m._anchor?m._anchor._y:0, w=orig.width, h=orig.height;
        return {x:-ax*w, y:-ay*h, width:w, height:h};
      }catch(e){ return null; }
    };
    var __ellipseMaskRectOf = function(m){
      try{
        if(!m) return null;
        // FGUI 的 GGraph 在 drawGraph 后会被 Pixi 清掉 graphicsData；UIOwner.$type
        // 才是稳定的形状来源。$type=2 对应 drawEllipse/drawCircle。
        var u=m.UIOwner;
        if(u && u.$type===2){
          var uw=Number(u._width), uh=Number(u._height);
          if(!(uw>0))uw=Number(u.width); if(!(uh>0))uh=Number(u.height);
          if(uw>0&&uh>0)return {x:0,y:0,width:uw,height:uh};
        }
        var gd=m.geometry&&m.geometry.graphicsData;
        if(!gd||gd.length!==1)return null;
        var d=gd[0],sh=d&&d.shape,st=sh&&sh.type;
        var circ=PIXI.SHAPES&&PIXI.SHAPES.CIRC, elip=PIXI.SHAPES&&PIXI.SHAPES.ELIP;
        if(!sh || (st!==circ&&st!==elip) || (d.holes&&d.holes.length) || d.matrix)return null;
        if(isFinite(sh.radius))return {x:sh.x-sh.radius,y:sh.y-sh.radius,width:sh.radius*2,height:sh.radius*2};
        if(isFinite(sh.width)&&isFinite(sh.height))return {x:sh.x-sh.width,y:sh.y-sh.height,width:sh.width*2,height:sh.height*2};
        return __boundsMaskRectOf(m);
      }catch(e){return null;}
    };
    var __boundsMaskRectOf = function(m){
      try{
        if(!m || !m.getLocalBounds) return null;
        var r=m.getLocalBounds(); if(!r || !(r.width>0) || !(r.height>0)) return null;
        return {x:r.x||0, y:r.y||0, width:r.width, height:r.height};
      }catch(e){ return null; }
    };
    var __graphicsMaskRectOf = function(m){
      try{
        // Do not rely on `instanceof PIXI.Graphics`: late-loaded modules can retain the original
        // Pixi constructor while the native host replaces Container/Sprite facades. Geometry is
        // the stable capability contract and is exactly what __wrGfx consumes.
        if(!m||!m.geometry)return null;
        var geom=m.geometry,gd=geom&&geom.graphicsData;
        if((!gd||!gd.length)&&!m.__nativeV8Graphics)return null;
        var bounds=__boundsMaskRectOf(m);if(bounds)return bounds;
        // A Graphics mask carrying a per-shape matrix can make Pixi getLocalBounds throw while
        // its triangulated points are already valid.  Native stencil rendering consumes those
        // exact points in __wrGfx; derive only the bookkeeping AABB here so the owner never falls
        // back to T_ESCAPE.  This remains exact for arbitrary polygon/rotation/holes because the
        // AABB does not replace the stencil geometry.
        try{if(geom.updateBatches)geom.updateBatches();}catch(_gmu){}
        var pts=geom.points,minX=Infinity,minY=Infinity,maxX=-Infinity,maxY=-Infinity;
        if(pts&&pts.length>=2){for(var pi=0;pi+1<pts.length;pi+=2){var px=Number(pts[pi]),py=Number(pts[pi+1]);if(!isFinite(px)||!isFinite(py))continue;if(px<minX)minX=px;if(px>maxX)maxX=px;if(py<minY)minY=py;if(py>maxY)maxY=py;}if(maxX>minX&&maxY>minY)return{x:minX,y:minY,width:maxX-minX,height:maxY-minY};}
        var sh=gd[0]&&gd[0].shape,sw=Number(sh&&sh.width),shh=Number(sh&&sh.height);if(sh&&sw>0&&shh>0)return{x:Number(sh.x)||0,y:Number(sh.y)||0,width:sw,height:shh};
        return null;
      }catch(e){return null;}
    };
    var __ownerMaskRectOf = function(o){
      try{
        var w=Number(o&&o._width),h=Number(o&&o._height);
        if(!(w>0))w=Number(o&&o.width);if(!(h>0))h=Number(o&&o.height);
        if(w>0&&h>0)return {x:0,y:0,width:w,height:h};
        return __boundsMaskRectOf(o);
      }catch(e){return null;}
    };
    var __rectMaskTargetOf = function(m){ // fgui/reel 常用容器包一层或多层矩形 Graphics
      var r=__rectMaskOf(m); if(r) return {obj:m, rect:r,geometry:!!r.__geometry};
      r=__spriteMaskRectOf(m); if(r) return {obj:m, rect:r, alpha:true};
      r=__ellipseMaskRectOf(m); if(r) return {obj:m, rect:r, ellipse:true};
      // Native-FGUI authored GGraphs retain their real triangles in C++; their lightweight facade
      // deliberately has no JS `_commands`.  Classifying every NativeGraphicsProxy as dynamic
      // geometry made mask assignment call _syncNativeGraphics(true), which submitted an empty
      // mesh and erased the authoritative native quads.  Preserve that payload and only use the
      // facade bounds as mask bookkeeping.  A genuinely empty authored graph stays an empty mask.
      var retainedGraphics=globalThis.__nativeMaskContract.usesRetainedGraphics(m);
      if(retainedGraphics){
        // Lazy facades can be classified before Pixi has a usable local-bounds cache.  Their
        // descriptor dimensions are still authoritative bookkeeping bounds; the stencil keeps
        // consuming the exact retained quads, so this fallback never rectangularizes the mask.
        r=__boundsMaskRectOf(m)||__ownerMaskRectOf(m);
        var retainedQuads=!!(m&&m.__i>0&&SGB[m.__i*ST+oQUADCNT]>0);
        return {obj:m,rect:retainedQuads?r:null,retainedGeometry:retainedQuads};
      }
      if(m&&m.__nativeGraphicsProxy){r=__boundsMaskRectOf(m);if(r)return {obj:m,rect:r,proxyGeometry:globalThis.__nativeMaskContract.hasRuntimeProxyGeometry(m)};}
      r=__graphicsMaskRectOf(m); if(r) return {obj:m, rect:r, geometry:true};
      // Pixi's legacy Graphics can have a valid single-shape geometry while both
      // getLocalBounds() and updateBatches() are temporarily unavailable during FGUI
      // construction.  Read the authoritative shape directly at this final dispatch
      // boundary.  The rectangle is only mask bookkeeping; __wrGfx still emits the
      // original Graphics triangles as the stencil, so matrices/rounded corners stay exact.
      try{
        var mgd=m&&m.geometry&&m.geometry.graphicsData,md=mgd&&mgd[0],msh=md&&md.shape;
        if(mgd&&mgd.length===1&&msh&&!(md.holes&&md.holes.length)){
          var mw=Number(msh.width),mh=Number(msh.height);
          if(mw>0&&mh>0){
            var mr={x:Number(msh.x)||0,y:Number(msh.y)||0,width:mw,height:mh};
            if(md.matrix){
              var mm=md.matrix,ma=Number(mm.a);if(!isFinite(ma))ma=1;
              var mb=Number(mm.b)||0,mc=Number(mm.c)||0,mdv=Number(mm.d);if(!isFinite(mdv))mdv=1;
              var mtx=Number(mm.tx)||0,mty=Number(mm.ty)||0,mx0=mr.x,my0=mr.y,mx1=mx0+mw,my1=my0+mh;
              var mxx=[ma*mx0+mc*my0+mtx,ma*mx1+mc*my0+mtx,ma*mx1+mc*my1+mtx,ma*mx0+mc*my1+mtx];
              var myy=[mb*mx0+mdv*my0+mty,mb*mx1+mdv*my0+mty,mb*mx1+mdv*my1+mty,mb*mx0+mdv*my1+mty];
              var mnx=Math.min.apply(Math,mxx),mxxv=Math.max.apply(Math,mxx),mny=Math.min.apply(Math,myy),mxy=Math.max.apply(Math,myy);
              mr={x:mnx,y:mny,width:mxxv-mnx,height:mxy-mny};
            }
            return {obj:m,rect:mr,geometry:true};
          }
        }
      }catch(_directMaskShape){}
      var found=null, rects=[], allPlain=true;
      var ch=m&&m.children;
      if(ch&&ch.length){
        for(var i=0;i<ch.length;i++){
          var c=ch[i]; if(!c || c.visible===false || c.renderable===false) continue;
          var rr=__rectMaskTargetOf(c);
          if(!rr) continue;
          rects.push(rr); if(rr.alpha||rr.ellipse)allPlain=false; if(!found) found=rr;
        }
        if(rects.length>1&&allPlain){
          // MReelsMask 是逐列矩形的并集，不能合成一个大 scissor，否则滚动扩展 symbol 会穿过
          // 相邻列/异形边界。rects 保留给 native batcher 按直接子树选择对应列。
          var all=__boundsMaskRectOf(m);
          return all?{obj:m,rect:all,rects:rects}:null;
        }
        if(found) return found;
      }
      r=__boundsMaskRectOf(m); if(r) return {obj:m, rect:r};
      return null;
    };
    var __maskDesc = function(m){
      var gd0=m&&m.geometry&&m.geometry.graphicsData, g00=gd0&&gd0[0], sh0=g00&&g00.shape, tex0=m&&m.isSprite&&(m._texture||m.texture);
      var s='mask='+((m&&m.constructor&&m.constructor.name)||'?')+' ch='+(m&&m.children?m.children.length:0)+' selfg='+(gd0?gd0.length:0)+' shape='+(sh0?(sh0.type+':'+((sh0.width||0)|0)+'x'+((sh0.height||0)|0)):'-')+(tex0?(' spriteTex='+(tex0.orig?((tex0.orig.width||0)+'x'+(tex0.orig.height||0)):'?')):'');
      var ch=m&&m.children; if(ch) for(var i=0;i<ch.length&&i<6;i++){ var c=ch[i], gd=c&&c.geometry&&c.geometry.graphicsData, g0=gd&&gd[0], sh=g0&&g0.shape;
        s+=' ['+i+']'+((c&&c.constructor&&c.constructor.name)||'?')+' ch='+(c&&c.children?c.children.length:0)+' g='+(gd?gd.length:0)+' shape='+(sh?(sh.type+':'+((sh.width||0)|0)+'x'+((sh.height||0)|0)):'-')+' pos='+((c&&c.x)|0)+','+((c&&c.y)|0);
      }
      return s;
    };
    // M5:Graphics 矩形填充 → 多-quad(白纹理 + 填充色当 tint)。仅处理"矩形填充"(fgui 背景/分割块最常见);
    // 非矩形/带洞/带矩阵/纯线 → 逃生舱(TYPE=2 黑洞,后续再补)。作遮罩用的 Graphics 不画自身。
    var __whiteTexId = function(){
      var wt = PIXI.Texture && PIXI.Texture.WHITE;
      return __nativeTexId(wt&&wt.baseTexture, true);
    };
    var __setMaskKind = function(m, mt){
      if(!m)return;
      m.__nativeAlphaMask=!!(mt&&mt.alpha);
      m.__nativeEllipseMask=!!(mt&&mt.ellipse);
      m.__nativeGeometryMask=!!(mt&&mt.geometry);
      m.__nativeProxyGeometryMask=!!(mt&&mt.proxyGeometry);
      m.__nativeRetainedGeometryMask=!!(mt&&mt.retainedGeometry);
      if(m.__nativeEllipseMask && m.__i>0){
        var b=m.__i*ST,wid=__whiteTexId();
        SGB[b+oTEXID]=wid;SGB[b+oUV]=0;SGB[b+oUV+1]=0;SGB[b+oUV+2]=1;SGB[b+oUV+3]=0;
        SGB[b+oUV+4]=1;SGB[b+oUV+5]=1;SGB[b+oUV+6]=0;SGB[b+oUV+7]=1;
        SGB[b+oTINT]=0xFFFFFF;SGB[b+oPREMUL]=0;
      }
      __writeHitMeta(m);
    };
    // A Pixi mask is still part of the display tree, but it must not paint as an ordinary node.
    // Keep every claimed mask (including the per-column children of a union mask), because FGUI
    // reuses these objects after a scene switch. Leaving __maskOf on an old child makes it disappear;
    // failing to set TYPE=0 immediately makes a dynamic RenderTexture brush flash on the main screen.
    var __releaseOwnedMasks = function(owner, keep){
      var old=owner&&owner.__sgMaskObjs;if(!old||!old.length)return;
      for(var i=0;i<old.length;i++){
        var m=old[i];if(!m || (keep&&keep.indexOf(m)>=0) || m.__maskOf!==owner)continue;
        m.__maskOf=undefined;__setMaskKind(m,null);__markRender(m);
      }
    };
    var __claimOwnedMasks = function(owner, entries){
      var next=[];
      for(var i=0;i<entries.length;i++)if(entries[i]&&entries[i].obj&&next.indexOf(entries[i].obj)<0)next.push(entries[i].obj);
      __releaseOwnedMasks(owner,next);
      owner.__sgMaskObjs=next;owner.__sgMaskObj=next[0]||null;
      for(var j=0;j<entries.length;j++){
        var e=entries[j],m=e&&e.obj;if(!m)continue;
        m.__maskOf=owner;__setMaskKind(m,e.kind||null);
        if(m.__i>0){
          // A mask can be bound before addChild attaches it (the mask setter and async loading
          // paths both do this). In that case its native node kept the old/root parent even though
          // Pixi's JS parent was later correct. Its world scale then differed from the owner; the
          // splash progress mask was 0.96/1.20 = 80% wide at a reported 100%. Reattach it to the
          // current JS parent whenever it is claimed so native and Pixi world transforms agree.
          var mp=m.parent;
          if(mp&&mp.__i>0&&typeof __sgSetParent==='function')__sgSetParent(m.__i,mp.__i,-1);
          var mb=m.__i*ST;SGB[mb+oMASKI]=0;
          if(globalThis.__nativeMaskContract.usesRetainedGraphics(m)){__sgTypeW(mb,0);}
          else if(m.__nativeProxyGeometryMask&&typeof m._syncNativeGraphics==='function')m._syncNativeGraphics(true);
          else {__sgTypeW(mb,0);SGB[mb+oQUADCNT]=0;}
        }
      }
    };
    var __emptyGfxPoints=new Float32Array([0,0]),__emptyGfxIndices=new Uint32Array([0]),
        __emptyGfxBatches=new Float32Array([0,0,0,0,0,0,0]);
    var __clearGfx = function(o,b,asMask){
      // TYPE alone cannot clear retained geometry: SceneGraph restores drawable NativeQuads.
      // Submit an empty mesh to release the authoritative payload (including stencil geometry).
      if(o.__sgNativeGraphicsPayload){
        if(!__nativeGraphicsMesh(o.__i,__emptyGfxPoints,__emptyGfxIndices,__emptyGfxPoints,__emptyGfxBatches,o.__nativeNodeProxy?(o._blendMode||0):(o.blendMode||0),!!asMask))
          throw new Error('Native Graphics clear failed for node '+o.__i);
        o.__sgNativeGraphicsPayload=false;
      }
      __qRelease(o);__sgTypeW(b,0);SGB[b+oQUADCNT]=0;
      __setNativeGeometryHit(o,false);__markBoundsNode(o.__i);
    };
    var __wrGfx = function(o,asMask){
      var b=o.__i*ST;
      SGB[b+oQUADCNT]=0;
      if(o.__maskOf&&!asMask){ __sgTypeW(b,0); return; }                         // 作遮罩的 Graphics 只写 stencil 几何,不画颜色
      if(typeof o.finishPoly==='function')o.finishPoly();
      var geom=o.geometry; if(!geom){ __clearGfx(o,b,asMask); return; }
      try{ geom.updateBatches(); }catch(e){ __setEsc(o,b,'gfx-update'); return; }            // 三角化(圆/圆角/多边形/线/洞/矩阵全在内,已缓存)
      var batches=geom.batches, points=geom.points, indices=geom.indices, guv=geom.uvs||[];
      if(!batches || !batches.length || !points || !points.length || !indices || !indices.length){ __clearGfx(o,b,asMask); return; }
      var wid=__whiteTexId();
      if(!wid){ __setEsc(o,b,'gfx-white-tex'); return; }
      var WBT=PIXI.Texture&&PIXI.Texture.WHITE&&PIXI.Texture.WHITE.baseTexture;
      var need=0; for(var nb=0;nb<batches.length;nb++) if(batches[nb]&&batches[nb].style) need+=Math.floor((batches[nb].size||0)/3);
      var gt2=__tintOf(o), gtR=((gt2>>16)&0xFF)/255, gtG=((gt2>>8)&0xFF)/255, gtB=(gt2&0xFF)/255;   // Graphics.tint 乘进填充色
      var premul=__premulOf(WBT);
      if(typeof globalThis.__nativeGraphicsMesh==='function'){
        // Pixi 只保留 Graphics API 与语义三角化结果；最终 triangle->quad 构造、存储、
        // bounds 与销毁都由 C++ SceneGraph 持有，不再占用 JS 持久 Quad 池。
        var gd=geom.dirty|0;
        if(geom.__sgNativeTypedDirty!==gd||!geom.__sgNativePoints||geom.__sgNativePoints.length!==points.length||geom.__sgNativeIndices.length!==indices.length||geom.__sgNativeUVs.length!==guv.length){
          geom.__sgNativeTypedDirty=gd;geom.__sgNativePoints=new Float32Array(points);geom.__sgNativeIndices=new Uint32Array(indices);geom.__sgNativeUVs=new Float32Array(guv);
        }
        var gm=new Float32Array(batches.length*7),gmn=0;
        for(var gbi=0;gbi<batches.length;gbi++){var gbp=batches[gbi],gst=gbp&&gbp.style;if(!gst)continue;
          var gsc=(gst.color===undefined?0xFFFFFF:gst.color),gal=(gst.alpha===undefined?1:gst.alpha);
          var gcol=((((((gsc>>16)&0xFF)*gtR)|0)<<16)|(((((gsc>>8)&0xFF)*gtG)|0)<<8)|(((gsc&0xFF)*gtB)|0));
          var gtid=wid,gu=0,gpm=premul;
          if(gst.texture&&gst.texture.baseTexture&&gst.texture.baseTexture!==WBT){var gbt=gst.texture.baseTexture,grt=__nativeTexId(gbt,true);if(!grt)continue;__addBaseTextureOwner(gbt,o);gtid=grt;gu=1;gpm=__premulOf(gbt);}
          var go=gmn*7;gm[go]=gtid;gm[go+1]=gbp.start||0;gm[go+2]=gbp.size||0;gm[go+3]=gcol;gm[go+4]=gal;gm[go+5]=gpm;gm[go+6]=gu;gmn++;
        }
        if(__nativeGraphicsMesh(o.__i,geom.__sgNativePoints,geom.__sgNativeIndices,geom.__sgNativeUVs,gmn===batches.length?gm:gm.subarray(0,gmn*7),o.__nativeNodeProxy?(o._blendMode||0):(o.blendMode||0),!!asMask)){
          o.__sgNativeGraphicsPayload=true;
          __qRelease(o);
          // NativeGraphicsMesh stores the exact Pixi triangles in C++.  Keep normal Graphics on
          // the native-multiquad path and expose those same triangles to hit testing.  Early FGUI
          // GGraphs (GuideMgr creates its modal mask before the deferred lean-leaf installer runs)
          // could otherwise retain T_CONTAINER with an empty bounds tree: the black overlay was
          // invisible and pointer events fell through to the machine cards behind it.
          if(!asMask&&!o.__maskOf&&!o.isMask)__sgTypeW(b,5);
          __setNativeGeometryHit(o,!asMask&&!o.__maskOf&&!o.isMask);
          __markBoundsNode(o.__i);return;
        }
      }
      var cur=__qEnsure(o,need), base=QB+1, n=0, ovf=0, limit=cur+(o.__qCap||0);
      if(cur<0){ __setEsc(o,b,'gfx-quad-pool'); return; }
      for(var bi=0;bi<batches.length;bi++){ var bp=batches[bi], st=bp.style; if(!st) continue;
        var sc=(st.color===undefined?0xFFFFFF:st.color), al=(st.alpha===undefined?1:st.alpha);
        var col=((((((sc>>16)&0xFF)*gtR)|0)<<16)|(((((sc>>8)&0xFF)*gtG)|0)<<8)|(((sc&0xFF)*gtB)|0));   // 填充色 × Graphics.tint
        var tid=wid, useUV=false, qpm=premul;
        if(st.texture && st.texture.baseTexture && st.texture.baseTexture!==WBT){   // 纹理填充:用真纹理+uv
          var tbt=st.texture.baseTexture, tt=__nativeTexId(tbt, true);
          if(tt){ __addBaseTextureOwner(tbt,o);tid=tt; useUV=true; qpm=__premulOf(tbt); } else continue;
        }
        var start=bp.start, size=bp.size;
        for(var t=0;t+2<size;t+=3){ if(cur+n+1>limit){ ovf=1; break; }
          var i0=indices[start+t]*2, i1=indices[start+t+1]*2, i2=indices[start+t+2]*2, q=base+(cur+n)*QSF;
          SGB[q]=tid;
          SGB[q+1]=points[i0];SGB[q+2]=points[i0+1]; SGB[q+3]=points[i1];SGB[q+4]=points[i1+1]; SGB[q+5]=points[i2];SGB[q+6]=points[i2+1]; SGB[q+7]=points[i2];SGB[q+8]=points[i2+1];   // 退化第4角
          if(useUV){ SGB[q+9]=guv[i0];SGB[q+10]=guv[i0+1];SGB[q+11]=guv[i1];SGB[q+12]=guv[i1+1];SGB[q+13]=guv[i2];SGB[q+14]=guv[i2+1];SGB[q+15]=guv[i2];SGB[q+16]=guv[i2+1]; }
          else { SGB[q+9]=0;SGB[q+10]=0;SGB[q+11]=1;SGB[q+12]=0;SGB[q+13]=1;SGB[q+14]=1;SGB[q+15]=1;SGB[q+16]=1; }   // 白纹理:全白
          SGB[q+17]=col; SGB[q+18]=al; SGB[q+19]=-1; SGB[q+20]=-1; SGB[q+21]=0; SGB[q+22]=qpm; SGB[q+23]=0; n++;   // 填充色 + 填充 alpha + per-quad premul;[23]清 shader kind(池块复用防陈旧 tiling 标志)
        }
        if(ovf) break;
      }
      if(ovf){ __setEsc(o,b,'gfx-quad-overflow'); return; }
      if(n===0){ __sgTypeW(b,0); return; }
      SGB[b+oQUADOFF]=cur; SGB[b+oQUADCNT]=n;
      __sgTypeW(b,asMask?0:3); SGB[b+oBLEND]=o.__nativeNodeProxy?(o._blendMode||0):(o.blendMode||0); SGB[b+oPREMUL]=premul;
    };
    // M5-S1:spine region attachment → 自由quad。populate pass 里对该 spine 重新 skin(填 arena),
    // 按 drawOrder 读每个 region slot 的世界4顶点(arena)+ 附件 uv + 纹理 + tint → 写 quad。
    // mesh/clip attachment 暂跳过(S2/S4);无 region 可画 → 逃生舱。顶点是骨骼空间,batcher 乘 spine 节点 world 矩阵。
    var __firstPendingAttachmentTime = function(entries){
      var best=Infinity;
      for(var i=0;i<entries.length;i++){
        var timelines=entries[i].animation&&entries[i].animation.timelines;
        if(!timelines)continue;
        for(var j=0;j<timelines.length;j++){
          var timeline=timelines[j], names=timeline&&timeline.attachmentNames, frames=timeline&&timeline.frames;
          if(!names||!frames)continue;
          for(var k=0;k<names.length&&k<frames.length;k++){
            var t=Number(frames[k]);
            if(names[k]!=null&&isFinite(t)&&t>=0&&t<best)best=t;
          }
        }
      }
      return best;
    };
    // Native render emission lives outside the runtime-patching closure above. Keep its deform
    // conversion helper in this scope as well; otherwise a deformed MeshAttachment throws during
    // dirty flushing and the whole Spine node retains stale/empty quads for that presentation.
    var __is9Slice = function(o){
      return !!(o && ((PIXI.NineSlicePlane && o instanceof PIXI.NineSlicePlane) ||
        (o.leftWidth!==undefined && o.rightWidth!==undefined && o.topHeight!==undefined && o.bottomHeight!==undefined && o.geometry && o.geometry.buffers)));
    };
    var __clear9Slice = function(o){
      if(!o||!(o.__i>0))return;
      // The host retains NativeNineSlice quads outside the shared JS quad pool. Merely changing
      // TYPE to container leaves the previous non-zero geometry alive, which makes a reused 0%
      // loading bar flash at 100% until its next positive-width update. Route empty/invalid
      // surfaces through the same host callback so it executes clearNativeQuads(node).
      if(typeof globalThis.__nativeNineSlice==='function')try{
        __nativeNineSlice(o.__i,0,0,0,0,0,0,0,1,1,false,false,0,0,1,0,1,1,0,1,0xFFFFFF,0,0);
      }catch(e){}
      __qRelease(o);var b=o.__i*ST;__sgTypeW(b,0);SGB[b+oQUADCNT]=0;
    };
    // NineSlicePlane/FguiNineSlicePlane: Mac native host 直接生成并持有 9 个 quad；
    // 其他 host 仍保留共享 quad buffer 回退。
    var __wr9Slice = function(o){
      var b=o.__i*ST, tex=o._texture||o.texture;
      if(!tex || !tex.valid){ __clear9Slice(o); return; }                     // 无纹理 → 当容器不画
      __addBaseTextureOwner(tex.baseTexture,o);
      // NineSlicePlane uses an explicit zero width for empty progress bars. Do not use truthy
      // fallback here: turning 0 into the texture's intrinsic width renders a large fill at 0%.
      var W=(o._width!==undefined&&o._width!==null)?Number(o._width):Number(o.width);
      var H=(o._height!==undefined&&o._height!==null)?Number(o._height):Number(o.height);
      if(!isFinite(W))W=(tex.orig&&tex.orig.width)||(tex.frame&&tex.frame.width)||1;
      if(!isFinite(H))H=(tex.orig&&tex.orig.height)||(tex.frame&&tex.frame.height)||1;
      if(!(W>0&&H>0)){__clear9Slice(o);return;}
      var id=__nativeTexId(tex.baseTexture, true);
      if(!id){ __setEsc(o,b,'9slice-no-tex'); return; }   // 纹理未上传 → 逃逸等下帧/原生路径兜底
      var lw=o.leftWidth||0, rw=o.rightWidth||0, th=o.topHeight||0, bh=o.bottomHeight||0;
      var orig=tex.orig||tex._frame||tex.frame||tex.orig, tw=orig&&orig.width||tex.width||1, thh=orig&&orig.height||tex.height||1, uvs=tex._uvs;
      if(!uvs && tex.updateUvs){ try{ tex.updateUvs(); uvs=tex._uvs; }catch(e){} }
      if(!uvs){ __setEsc(o,b,'9slice-no-uv'); return; }
      if(typeof globalThis.__nativeNineSlice==='function'){
        var _n9t=__tintOf(o),_n9p=__premulOf(tex.baseTexture);
        if(__nativeNineSlice(o.__i,id,W,H,lw,rw,th,bh,tw,thh,!!o.$flipX,!!o.$flipY,
             uvs.x0,uvs.y0,uvs.x1,uvs.y1,uvs.x2,uvs.y2,uvs.x3,uvs.y3,_n9t,o.__nativeNodeProxy?(o._blendMode||0):(o.blendMode||0),_n9p)){
          // NineSlice 的 Pixi geometry 仍保留完整 containsPoint 语义。不打 native-geometry
          // 标记，否则精确命中会去读已释放的 JS quad 池。
          __qRelease(o);__setNativeGeometryHit(o,false);return;
        }
      }
      var swW=lw+rw, swH=th+bh, scW=(W>swW||swW<=0)?1:W/swW, scH=(H>swH||swH<=0)?1:H/swH, msc=Math.min(scW,scH);   // pixi _getMinScale:面板小于边框时等比压缩边框
      var xs=[0, (o.$flipX?rw:lw)*msc, W-(o.$flipX?lw:rw)*msc, W];       // FGUI flip 会交换顶点边距
      var ys=[0, (o.$flipY?bh:th)*msc, H-(o.$flipY?th:bh)*msc, H];
      var xr=o.$flipX?[1,1-rw/tw,lw/tw,0]:[0,lw/tw,1-rw/tw,1];            // uv 用原始边框比例;FGUI flip 镜像 4 列/行
      var yr=o.$flipY?[1,1-bh/thh,th/thh,0]:[0,th/thh,1-bh/thh,1];
      var uvAt=function(px,py){                                           // 支持 atlas rotate:用 pixi 4 角 UV 做双线性插值
        var ax=1-px, ay=1-py;
        return [
          uvs.x0*ax*ay + uvs.x1*px*ay + uvs.x2*px*py + uvs.x3*ax*py,
          uvs.y0*ax*ay + uvs.y1*px*ay + uvs.y2*px*py + uvs.y3*ax*py
        ];
      };
      var cur=__qEnsure(o,9);
      if(cur<0){ __setEsc(o,b,'9slice-quad-pool'); return; }
      var tint=__tintOf(o), base=QB+1, n=0, s9pm=__premulOf(tex.baseTexture);
      for(var r=0;r<3;r++) for(var c=0;c<3;c++){
        var qx=xs[c], qy=ys[r], qw=xs[c+1]-xs[c], qh=ys[r+1]-ys[r];
        var uv00=uvAt(xr[c],yr[r]), uv10=uvAt(xr[c+1],yr[r]), uv11=uvAt(xr[c+1],yr[r+1]), uv01=uvAt(xr[c],yr[r+1]);
        var q=base+(cur+n)*QSF;
        SGB[q]=id;
        SGB[q+1]=qx;    SGB[q+2]=qy;       SGB[q+3]=qx+qw; SGB[q+4]=qy;        // 角0左上 角1右上
        SGB[q+5]=qx+qw; SGB[q+6]=qy+qh;    SGB[q+7]=qx;    SGB[q+8]=qy+qh;     // 角2右下 角3左下
        SGB[q+9]=uv00[0]; SGB[q+10]=uv00[1]; SGB[q+11]=uv10[0]; SGB[q+12]=uv10[1];            // uv 角0 角1
        SGB[q+13]=uv11[0]; SGB[q+14]=uv11[1]; SGB[q+15]=uv01[0]; SGB[q+16]=uv01[1];           // uv 角2 角3
        SGB[q+17]=tint; SGB[q+18]=1; SGB[q+19]=-1; SGB[q+20]=-1; SGB[q+21]=0; SGB[q+22]=s9pm; SGB[q+23]=0; n++;   // 用节点 alpha/blend,非双色,per-quad premul;[23]清 shader kind
      }
      SGB[b+oQUADOFF]=cur; SGB[b+oQUADCNT]=9;
      __sgTypeW(b,3); SGB[b+oBLEND]=o.__nativeNodeProxy?(o._blendMode||0):(o.blendMode||0); SGB[b+oPREMUL]=__premulOf(tex.baseTexture);   // M6
    };
    // BitmapText:每字符一个 quad,几何取自 _activePagesMeshData(局部空间 4角+uv,已含 anchor/scale)。page mesh 子节点在树里会逃逸跳过,不重复。
    var __wrBmp = function(o){
      var b=o.__i*ST;
      var fd=PIXI.BitmapFont && PIXI.BitmapFont.available && PIXI.BitmapFont.available[o._fontName];
      var distanceFieldWidth=fd&&fd.distanceFieldType&&fd.distanceFieldType!=='none'?(Number(fd.distanceFieldRange)||0)*(Number(o.fontSize)||Number(fd.size)||1)/Math.max(1,Number(fd.size)||1)*(Number(o.resolution)||1):0;
      try{ if(o.validate) o.validate(); else if(o.dirty) o.updateText(); }catch(e){ __setEsc(o,b,'bitmap-validate'); return; }   // 文字/字体变了要重建几何(短路后 _render 不跑)
      var txt=String(o.text===undefined?(o._text||''):o.text), pages=o._activePagesMeshData;
      if(!pages || !pages.length){ if(!txt.trim()){ __sgTypeW(b,0); return; } __setEsc(o,b,'bitmap-no-pages'); return; }
      var need=0; for(var pn=0;pn<pages.length;pn++){ var pg0=pages[pn], tm0=pg0&&pg0.mesh, tn0=pg0&&(pg0.total|0); if(!tn0&&tm0&&tm0.size)tn0=(tm0.size/6)|0; need+=tn0||0; }
      var cur=__qEnsure(o,need), base=QB+1, n=0, tint=__tintOf(o), premul=0, ovf=0, limit=cur+(o.__qCap||0);
      if(cur<0){ __setEsc(o,b,'bitmap-quad-pool'); return; }
      for(var p=0;p<pages.length;p++){ var pg=pages[p], mesh=pg.mesh; if(!mesh) continue;
        var tex=mesh.texture||mesh._texture, bt=tex&&tex.baseTexture; if(!bt) continue;
        __addBaseTextureOwner(bt,o);
        var tid=__nativeTexId(bt, true); if(!tid) continue;
        premul=__premulOf(bt);
        var vs=pg.vertices, uvs=pg.uvs, total=pg.total|0; if(!total && mesh.size) total=(mesh.size/6)|0; if(!vs||!uvs) continue;
        for(var c=0;c<total;c++){ if(cur+n+1>limit){ ovf=1; break; } var vo=c*8, q=base+(cur+n)*QSF;
          SGB[q]=tid;
          SGB[q+1]=vs[vo];SGB[q+2]=vs[vo+1];SGB[q+3]=vs[vo+2];SGB[q+4]=vs[vo+3];SGB[q+5]=vs[vo+4];SGB[q+6]=vs[vo+5];SGB[q+7]=vs[vo+6];SGB[q+8]=vs[vo+7];
          SGB[q+9]=uvs[vo];SGB[q+10]=uvs[vo+1];SGB[q+11]=uvs[vo+2];SGB[q+12]=uvs[vo+3];SGB[q+13]=uvs[vo+4];SGB[q+14]=uvs[vo+5];SGB[q+15]=uvs[vo+6];SGB[q+16]=uvs[vo+7];
          SGB[q+17]=tint; SGB[q+18]=1; SGB[q+19]=-1; SGB[q+20]=distanceFieldWidth>0?-2:-1; SGB[q+21]=distanceFieldWidth; SGB[q+22]=premul; SGB[q+23]=0; n++;
        }
        if(ovf) break;
      }
      if(ovf){ __setEsc(o,b,'bitmap-quad-overflow'); return; }   // 溢出 → 整节点逃逸(不缺字)
      if(n>0){ SGB[b+oQUADOFF]=cur; SGB[b+oQUADCNT]=n; __sgTypeW(b,3); SGB[b+oBLEND]=o.__nativeNodeProxy?(o._blendMode||0):(o.blendMode||0); SGB[b+oPREMUL]=premul; }
      else { if(!txt.trim()) __sgTypeW(b,0); else __setEsc(o,b,'bitmap-empty'); }
    };
    // ParticleContainer: one native node owns all visible particle quads. Particle emitters still
    // mutate their lightweight Sprite handles, but property hooks invalidate only this packed node;
    // native never traverses one scenegraph node per particle.
    var __wrParticle = function(o){
      var b=o.__i*ST,ch=o.__nativeV8ParticleViews||o.children||[],need=0;
      // NativeNodeProxy 的 children 是 Proxy 视图:逐下标访问=每次跨桥;先一次性取快照数组
      if(o.__childrenView&&ch===o.__childrenView&&typeof ch.slice==='function')ch=ch.slice();
      // 父子关系校验(第二道防线):只打包"当前确实还挂在本容器下"的子节点。快照缓存一旦因为
      // 任何原因过期(历史上就是 R.remove 不推进结构版本),被摘除的粒子会被继续提交,发射结束
      // 后不再有结构变化触发刷新,残影就永久留在屏上。这里按对象的实时 parent 兜住。
      var __packOk=function(c){return !!(c&&c.isSprite&&c.parent===o&&c.visible!==false&&c._renderable!==false&&c.alpha!==0);};
      for(var ci=0;ci<ch.length;ci++)if(__packOk(ch[ci]))need++;
      if(!need){__qRelease(o);__sgTypeW(b,0);SGB[b+oQUADCNT]=0;SGB[b+oREND]=(o._renderable===false?0:1);return;}
      var cur=__qEnsure(o,need);if(cur<0){__setEsc(o,b,'particle-quad-pool');return;}
      var base=QB+1,n=0,blend=o.__nativeNodeProxy?(o._blendMode||0):(o.blendMode||0);
      for(var i=0;i<ch.length;i++){
        var c=ch[i];if(!c||!c.isSprite)continue;
        // Its semantic handle remains available to the emitter, but rendering is owned by parent.
        if(c.__i>0){var cb=c.__i*ST;__qRelease(c);__sgTypeW(cb,0);SGB[cb+oQUADCNT]=0;SGB[cb+oREND]=0;}
        if(!__packOk(c))continue;   // 与计数同一判据(含 parent 校验),否则 need 与实际写入数不一致
        var tex=c._texture||c.texture,bt=tex&&tex.baseTexture,orig=tex&&tex.orig;if(!tex||!tex.valid||!bt||!orig)continue;
        var uvs=tex._uvs;if(!uvs&&tex.updateUvs)try{tex.updateUvs();uvs=tex._uvs;}catch(e){}if(!uvs)continue;
        var tid=__nativeTexId(bt,true);if(!tid)continue;__addBaseTextureOwner(bt,o);
        var anchor=c._anchor||c.anchor,ax=anchor?(Number(anchor._x===undefined?anchor.x:anchor._x)||0):0,ay=anchor?(Number(anchor._y===undefined?anchor.y:anchor._y)||0):0;
        var trim=tex.trim,w=orig.width,h=orig.height,x0=trim?trim.x-ax*w:-ax*w,y0=trim?trim.y-ay*h:-ay*h,rw=trim?trim.width:w,rh=trim?trim.height:h;
        var t=c.transform||{},pos=t.position||c.position||{x:c.x||0,y:c.y||0},sc=t.scale||c.scale||{x:1,y:1},pv=t.pivot||c.pivot||{x:0,y:0},sk=t.skew||c.skew||{x:0,y:0};
        var rot=Number(t._rotation===undefined?c.rotation:t._rotation)||0,sx=Number(sc._x===undefined?sc.x:sc._x);if(!isFinite(sx))sx=1;var sy=Number(sc._y===undefined?sc.y:sc._y);if(!isFinite(sy))sy=1;
        var skx=Number(sk._x===undefined?sk.x:sk._x)||0,sky=Number(sk._y===undefined?sk.y:sk._y)||0,a=Math.cos(rot+sky)*sx,bb=Math.sin(rot+sky)*sx,cc=-Math.sin(rot-skx)*sy,d=Math.cos(rot-skx)*sy;
        var px=Number(pv._x===undefined?pv.x:pv._x)||0,py=Number(pv._y===undefined?pv.y:pv._y)||0,tx=(Number(pos._x===undefined?pos.x:pos._x)||0)-px*a-py*cc,ty=(Number(pos._y===undefined?pos.y:pos._y)||0)-px*bb-py*d;
        var x1=x0+rw,y1=y0+rh,q=base+(cur+n)*QSF,writePoint=function(off,x,y){SGB[q+off]=a*x+cc*y+tx;SGB[q+off+1]=bb*x+d*y+ty;};
        SGB[q]=tid;writePoint(1,x0,y0);writePoint(3,x1,y0);writePoint(5,x1,y1);writePoint(7,x0,y1);
        SGB[q+9]=uvs.x0;SGB[q+10]=uvs.y0;SGB[q+11]=uvs.x1;SGB[q+12]=uvs.y1;SGB[q+13]=uvs.x2;SGB[q+14]=uvs.y2;SGB[q+15]=uvs.x3;SGB[q+16]=uvs.y3;
        SGB[q+17]=__tintOf(c);SGB[q+18]=Math.max(0,Math.min(1,Number(c.alpha===undefined?1:c.alpha)||0));SGB[q+19]=-1;SGB[q+20]=-1;SGB[q+21]=0;SGB[q+22]=__premulOf(bt);SGB[q+23]=0;n++;
      }
      if(n){SGB[b+oQUADOFF]=cur;SGB[b+oQUADCNT]=n;__sgTypeW(b,3);SGB[b+oBLEND]=blend;SGB[b+oPREMUL]=0;SGB[b+oREND]=(o._renderable===false?0:1);}
      else {__qRelease(o);__sgTypeW(b,0);SGB[b+oQUADCNT]=0;SGB[b+oREND]=(o._renderable===false?0:1);}
    };
    // Standard MeshMaterial: encode every triangle as a degenerate native quad. Pixi SimpleRope
    // uses TRIANGLE_STRIP and planes may use TRIANGLE_FAN, so all three standard modes are native.
    // Truly custom Mesh shaders remain explicit until their program/uniforms are registered.
    // Bake a DisplayObject subtree into a RenderTexture using the same native RT path that
    // filters/popups already use. The result is a normal Sprite + texture id that __wrTex can emit.
    // This is intentionally generic: spine freezes, fullscreen popups, and list snapshots all
    // share the same code path. Failures leave the node live (no T_ESCAPE) so content still shows.
    var __ensureNativeCacheAsBitmap=function(o){
      if(!o||!(o.__i>0)||!o._cacheAsBitmap||!PIXI.RenderTexture||!PIXI.Sprite)return false;
      var r=globalThis.__nativeRenderer||globalThis.__sgRenderer;
      if(!r||typeof globalThis.__nativeRenderTexture!=='function')return false;
      var data=o._cacheData||(o._cacheData={});
      // Re-bake when the node was marked dirty after the last successful bake.
      if(data.__nativeBaked&&(data.__nativeEmpty||data.sprite&&data.sprite.texture&&data.sprite.texture.valid)&&
         (o.__nativeV8Cache||data.__nativeBakeFrame===(globalThis.__frameNo|0))&&!data.__nativeBakeDirty)return true;
      if(data.__nativeBakeBusy)return !!(data.sprite&&data.sprite.texture&&data.sprite.texture.valid);
      if(o.__nativeV8Cache){
        // Rebuild live bounds before refresh. The previous cached sprite must
        // not enlarge the next bake after content moves, shrinks or is cleared.
        data.__nativeBakeBusy=1;o.__nativeCachedTextureActive=false;__writeHitMeta(o);
        try{
          // Publish pending geometry before measuring. This runs only on an
          // explicit cache build/refresh, never on steady cached frames.
          var publishCacheSubtree=function(node){
            if(node!==o&&node.__nativeCachedTextureActive){__wrType(node);return;}
            var children=node.children||[];
            for(var ci=0;ci<children.length;ci++)if(children[ci]&&!children[ci]._destroyed)publishCacheSubtree(children[ci]);
            __wrType(node);
          };
          publishCacheSubtree(o);
        }finally{data.__nativeBakeBusy=0;}
      }
      // Bounds in world space. Prefer native getBounds so FGUI leaves without JS facades count.
      var bounds=null;
      try{bounds=o.getBounds(true);}catch(e){if(o.__nativeV8Cache)throw e;bounds=null;}
      if(o.__nativeV8Cache&&bounds&&(!(bounds.width>0)||!(bounds.height>0))){
        if(data.sprite)data.sprite.destroy();if(data.__nativeRT)data.__nativeRT.destroy(true);
        data.sprite=null;data.__nativeRT=null;data.__nativeEmpty=true;
        data.__nativeBaked=1;data.__nativeBakeDirty=0;return true;
      }
      if(!bounds||!(bounds.width>0.5)||!(bounds.height>0.5))return !!(data.sprite&&data.sprite.texture&&data.sprite.texture.valid);
      var res=Number(o._cacheAsBitmapResolution||r.resolution||1)||1;
      // ── 烘焙坐标链(与消费端逐环对齐) ──────────────────────────────────────────────
      // C++ cbNativeRenderTexture 把 outputTransform M 叠在子树每个节点的世界矩阵上
      // (batcher.h mapOutput: v = M∘W_node∘local);present 钩子按 Pixi render(o,{transform:tm})
      // 语义组合 M = tm∘L∘W⁻¹(L/W = o 在 SGB 的 local/world 矩阵,LA..=20..25/WA..=12..17)。
      // 缓存显示端(__wrCachedBitmap)把 RT 画成节点 o 自己的 local rect(原点 bakeX/Y),由 W 变换。
      // 视觉恒等 ⇔ M = res·T(-a)∘W⁻¹,a = 子树 AABB 在 o 局部空间的原点;由 M=tm∘L∘W⁻¹ 解得
      //   tm = res·T(-a)∘L⁻¹  (∘L 与消费端组合链精确相消)。
      // 旧公式 m=W⁻¹∘T(-b_world) 保留了 W⁻¹ 自身的平移分量,组合后 M=W⁻¹∘T(-b)∘L∘W⁻¹,仅当
      // W≈I 且 L≈I(全屏弹窗挂在 0,0)时与正确解一致;o 带世界平移 t 时 RT 内容整体偏移
      // -W⁻¹_lin·t → 内容裁角/偏移/空白。新公式在 W=L=I 特例下与旧代码逐位一致(不回归判据),
      // 且局部空间烘焙允许对象烘焙后平移/缩放而无需重烘。
      var cbB=o.__i*ST;
      var cwA=SGB[cbB+12],cwBv=SGB[cbB+13],cwC=SGB[cbB+14],cwD=SGB[cbB+15],cwTX=SGB[cbB+16],cwTY=SGB[cbB+17];
      var clA=SGB[cbB+20],clBv=SGB[cbB+21],clC=SGB[cbB+22],clD=SGB[cbB+23],clTX=SGB[cbB+24],clTY=SGB[cbB+25];
      var cDetW=cwA*cwD-cwBv*cwC,cDetL=clA*clD-clBv*clC;
      if(!(Math.abs(cDetW)>1e-10)||!(Math.abs(cDetL)>1e-10))return !!(data.sprite&&data.sprite.texture&&data.sprite.texture.valid);
      // 世界 AABB 4 角经 W⁻¹ → o 局部空间 AABB(平移/轴对齐缩放时精确;旋转时为保守包络,
      // 与 __nativeLocalBoundsOf 同一映射)。
      var ciwA=cwD/cDetW,ciwB=-cwBv/cDetW,ciwC=-cwC/cDetW,ciwD=cwA/cDetW;
      var cbX0=Infinity,cbY0=Infinity,cbX1=-Infinity,cbY1=-Infinity;
      for(var cbc=0;cbc<4;cbc++){
        var cpx=bounds.x+((cbc&1)?bounds.width:0)-cwTX,cpy=bounds.y+((cbc&2)?bounds.height:0)-cwTY;
        var clx=ciwA*cpx+ciwC*cpy,cly=ciwB*cpx+ciwD*cpy;
        if(clx<cbX0)cbX0=clx;if(clx>cbX1)cbX1=clx;if(cly<cbY0)cbY0=cly;if(cly>cbY1)cbY1=cly;
      }
      var cbW=cbX1-cbX0,cbH=cbY1-cbY0;
      if(!(cbW>0.5)||!(cbH>0.5)||!isFinite(cbW)||!isFinite(cbH))return !!(data.sprite&&data.sprite.texture&&data.sprite.texture.valid);
      var w=Math.max(1,Math.ceil(cbW*res)),h=Math.max(1,Math.ceil(cbH*res));
      // Cap extreme bake sizes (full-stage accidents) without feature-specific names.
      if(w*h>4096*4096){w=Math.min(w,4096);h=Math.min(h,4096);}
      var rt=data.__nativeRT;
      if(!rt||!rt.baseTexture||Math.abs((rt.baseTexture.width||0)-w)>0.5||Math.abs((rt.baseTexture.height||0)-h)>0.5){
        try{if(rt)rt.destroy(true);}catch(e){}
        try{rt=PIXI.RenderTexture.create({width:w,height:h,resolution:1,scaleMode:PIXI.SCALE_MODES?PIXI.SCALE_MODES.LINEAR:0});}
        catch(e){return false;}
        data.__nativeRT=rt;
      }
      // tm = res·T(-a)∘L⁻¹:消费端右乘 ∘L∘W⁻¹ 后成为 res·T(-a)∘W⁻¹ —— RT pass 里子树按世界
      // 矩阵绘制,该 M 把世界坐标拉回 o 局部空间、再把局部 AABB 原点平移到 RT (0,0)。
      // L 取自 SGB(与消费端同一数据源),保证 ∘L 精确相消。
      var m=new PIXI.Matrix();
      var cilA=clD/cDetL,cilB=-clBv/cDetL,cilC=-clC/cDetL,cilD=clA/cDetL;
      m.a=res*cilA;m.b=res*cilB;m.c=res*cilC;m.d=res*cilD;
      m.tx=res*(-(cilA*clTX+cilC*clTY)-cbX0);m.ty=res*(-(cilB*clTX+cilD*clTY)-cbY0);
      data.__nativeBakeBusy=1;
      var cacheAlpha=o.alpha,cacheTint=o.tint,cacheFilters=o.filters,cacheMask=o.mask;
      if(o.__nativeV8Cache){
        o.__nativeCachedTextureActive=false;__writeHitMeta(o);
        // Group-level color, opacity, filters and mask are applied once when the
        // cached image is painted, rather than being baked and then applied twice.
        o.alpha=1;o.tint=0xFFFFFF;o.filters=null;o.mask=null;
      }
      // Walk the live subtree while baking. Do NOT toggle o._cacheAsBitmap — that setter
      // destroys the RT on false. Instead __wrType checks __nativeBakeBusy and skips cache paint.
      var ok=false;
      try{
        // Force a full native render-data flush of this subtree before baking.
        __markRender(o);
        r.render(o,{renderTexture:rt,clear:true,transform:m,skipUpdateTransform:false});
        ok=!!(rt.baseTexture&&rt.baseTexture.valid!==false);
      }catch(e){
        if(o.__nativeV8Cache)throw e;
        if((globalThis.__nativeCabErrN||0)<8){globalThis.__nativeCabErrN=(globalThis.__nativeCabErrN||0)+1;__log('[native-cab] bake fail '+e.message);}
        ok=false;
      }finally{
        if(o.__nativeV8Cache){o.alpha=cacheAlpha;o.tint=cacheTint;o.filters=cacheFilters;o.mask=cacheMask;}
        data.__nativeBakeBusy=0;
      }
      if(!ok)return !!(data.sprite&&data.sprite.texture&&data.sprite.texture.valid);
      var sprite=data.sprite;
      if(!sprite){sprite=new PIXI.Sprite(rt);data.sprite=sprite;}
      else if(sprite.texture!==rt)sprite.texture=rt;
      // Anchor so the cached rect reproduces the subtree's local-space AABB origin; the node's
      // own world matrix W then places it exactly where the live subtree rendered (any W, not
      // only W≈I as the old world-space bake required).
      if(sprite.anchor){sprite.anchor.x=-(cbX0/cbW);sprite.anchor.y=-(cbY0/cbH);}
      sprite._bounds=o._bounds;
      data.__nativeBaked=1;data.__nativeBakeDirty=0;data.__nativeBakeFrame=globalThis.__frameNo|0;
      data.__nativeEmpty=false;
      data.__nativeBakeW=cbW;data.__nativeBakeH=cbH;data.__nativeBakeX=cbX0;data.__nativeBakeY=cbY0;
      if((globalThis.__nativeCabOkN||0)<4){globalThis.__nativeCabOkN=(globalThis.__nativeCabOkN||0)+1;__log('[native-cab] baked '+((o.constructor&&o.constructor.name)||'?')+' '+(w|0)+'x'+(h|0));}
      return true;
    };
    var __wrCachedBitmap=function(o){
      var data=o&&o._cacheData,sprite=data&&data.sprite,tex=sprite&&sprite.texture;
      if(o&&o.__nativeV8Cache&&data&&data.__nativeEmpty){
        var emptyB=o.__i*ST;__sgTypeW(emptyB,0);SGB[emptyB+oTEXID]=0;SGB[emptyB+oQUADCNT]=0;
        o.__nativeCachedTextureActive=true;__writeHitMeta(o);return true;
      }
      if(!tex||!tex.valid||!tex.baseTexture)return false;
      var b=o.__i*ST,bt=tex.baseTexture,tid=__nativeTexId(bt,true);
      if(!tid)return false;
      __addBaseTextureOwner(bt,o);
      // Local rect matches the original subtree's local bounds origin (anchor-adjusted).
      var w=Number(data.__nativeBakeW)||tex.width||bt.width||0;
      var h=Number(data.__nativeBakeH)||tex.height||bt.height||0;
      if(!(w>0&&h>0))return false;
      var ax=sprite.anchor?Number(sprite.anchor.x)||0:0,ay=sprite.anchor?Number(sprite.anchor.y)||0:0;
      var x0=-ax*w,y0=-ay*h;
      SGB[b+oRX]=x0;SGB[b+oRX+1]=y0;SGB[b+oRX+2]=w;SGB[b+oRX+3]=h;
      var uvs=tex._uvs;if(!uvs&&tex.updateUvs)try{tex.updateUvs();uvs=tex._uvs;}catch(e){}
      if(uvs){SGB[b+oUV]=uvs.x0;SGB[b+oUV+1]=uvs.y0;SGB[b+oUV+2]=uvs.x1;SGB[b+oUV+3]=uvs.y1;SGB[b+oUV+4]=uvs.x2;SGB[b+oUV+5]=uvs.y2;SGB[b+oUV+6]=uvs.x3;SGB[b+oUV+7]=uvs.y3;}
      else{SGB[b+oUV]=0;SGB[b+oUV+1]=0;SGB[b+oUV+2]=1;SGB[b+oUV+3]=0;SGB[b+oUV+4]=1;SGB[b+oUV+5]=1;SGB[b+oUV+6]=0;SGB[b+oUV+7]=1;}
      SGB[b+oTEXID]=tid;__sgTypeW(b,1);SGB[b+oQUADCNT]=0;SGB[b+oBLEND]=o.__nativeNodeProxy?(o._blendMode||0):(o.blendMode||0);SGB[b+oTINT]=__tintOf(o);SGB[b+oPREMUL]=__premulOf(bt);
      if(o.__nativeV8Cache){o.__nativeCachedTextureActive=true;__writeHitMeta(o);}
      // Hide live children for paint order: the cache replaces the whole subtree visually.
      // Children stay in the tree for event/lifecycle; RENDERABLE=0 only suppresses drawing.
      // We do not mutate child flags permanently — next non-cached frame rewrites them via __wrType.
      return true;
    };
    var __tryNativeCachedBitmap=function(o,b){
      if(!o._cacheAsBitmap||(o._cacheData&&o._cacheData.__nativeBakeBusy))return false;
      // Cache construction is opportunistic. Empty pre-attachment bounds, an RT preflight miss or
      // a texture that is not publishable yet must leave the node to its authoritative live type.
      // Returning false is what lets __wrType continue into NativeSpine/Graphics/Sprite below.
      if(!__ensureNativeCacheAsBitmap(o)||!__wrCachedBitmap(o))return false;
      SGB[b+oREND]=(o._renderable===false?0:1);
      return true;
    };
    var __wrMesh = function(o){
      var b=o.__i*ST, geom=o.__nativeMeshGeometry||o.geometry, mat=o.shader||o.material;
      if(!geom||!mat||!(PIXI.MeshMaterial&&mat instanceof PIXI.MeshMaterial)){__setEsc(o,b,'mesh-material');return;}
      var dm=PIXI.DRAW_MODES||{}, mode=o.drawMode;
      if(mode===undefined||mode===null)mode=dm.TRIANGLES;
      // Accept both enum members and raw GL constants (some game code passes 4/5/6).
      var isTri=mode===dm.TRIANGLES||mode===4;
      var isStrip=mode===dm.TRIANGLE_STRIP||mode===5;
      var isFan=mode===dm.TRIANGLE_FAN||mode===6;
      // Line / point meshes are rare but used by debug overlays and some particle trails.
      // Expand thin primitives into degenerate triangle quads so they stay on the native path
      // instead of becoming T_ESCAPE. Width is 1 logical pixel in local space.
      var isLines=mode===dm.LINES||mode===1;
      var isLineStrip=mode===dm.LINE_STRIP||mode===3;
      var isLineLoop=mode===dm.LINE_LOOP||mode===2;
      var isPoints=mode===dm.POINTS||mode===0;
      if(!isTri&&!isStrip&&!isFan&&!isLines&&!isLineStrip&&!isLineLoop&&!isPoints){__setEsc(o,b,'mesh-draw-mode');return;}
      var tex=mat.texture||o.texture, bt=tex&&tex.baseTexture, tid=__nativeTexId(bt,true);
      if(!tex||!tex.valid||!tid){ __sgTypeW(b,0);SGB[b+oQUADCNT]=0;return; }
      __addBaseTextureOwner(bt,o);
      try{ if(mat.uvMatrix){ mat.uvMatrix.update(); o.calculateUvs(); } }catch(e){}
      var vb=geom.getBuffer?geom.getBuffer('aVertexPosition'):geom.buffers&&geom.buffers[0], ib=geom.getIndex?geom.getIndex():geom.indexBuffer;
      var vertices=vb&&vb.data, indices=ib&&ib.data, uvs=o.uvs||((geom.getBuffer&&geom.getBuffer('aTextureCoord'))||geom.buffers&&geom.buffers[1]); if(uvs&&uvs.data)uvs=uvs.data;
      if(!vertices||!uvs){ __setEsc(o,b,'mesh-data'); return; }
      var elemCount=indices?indices.length:Math.floor(vertices.length/2),start=Math.max(0,o.start|0),avail=Math.max(0,elemCount-start),size=(o.size|0)>0?Math.min(o.size|0,avail):avail;
      var need=0;
      if(isTri)need=Math.floor(size/3);
      else if(isStrip||isFan)need=Math.max(0,size-2);
      else if(isLines)need=Math.floor(size/2);
      else if(isLineStrip)need=Math.max(0,size-1);
      else if(isLineLoop)need=size>1?size:0;
      else if(isPoints)need=size;
      var cur=__qEnsure(o,need);
      if(cur<0){ __setEsc(o,b,'mesh-quad-pool'); return; }
      var base=QB+1,n=0,tint=__tintOf(o),alpha=(mat.alpha===undefined?1:mat.alpha),pm=__premulOf(bt),at=function(pos){return indices?indices[pos]:pos;};
      var half=0.5; // local-space half-width for expanded line/point primitives
      var emitTri=function(ai,ci,di){
        if(ai===ci||ci===di||ai===di)return;
        var a=ai*2,c=ci*2,d=di*2;if(a<0||c<0||d<0||a+1>=vertices.length||c+1>=vertices.length||d+1>=vertices.length||a+1>=uvs.length||c+1>=uvs.length||d+1>=uvs.length)return;
        var q=base+(cur+n)*QSF; SGB[q]=tid;
        SGB[q+1]=vertices[a];SGB[q+2]=vertices[a+1]; SGB[q+3]=vertices[c];SGB[q+4]=vertices[c+1]; SGB[q+5]=vertices[d];SGB[q+6]=vertices[d+1]; SGB[q+7]=vertices[d];SGB[q+8]=vertices[d+1];
        SGB[q+9]=uvs[a];SGB[q+10]=uvs[a+1]; SGB[q+11]=uvs[c];SGB[q+12]=uvs[c+1]; SGB[q+13]=uvs[d];SGB[q+14]=uvs[d+1]; SGB[q+15]=uvs[d];SGB[q+16]=uvs[d+1];
        SGB[q+17]=tint;SGB[q+18]=alpha;SGB[q+19]=-1;SGB[q+20]=-1;SGB[q+21]=0;SGB[q+22]=pm;SGB[q+23]=0;n++;
      };
      var emitLine=function(i0,i1){
        var a=i0*2,c=i1*2;if(a<0||c<0||a+1>=vertices.length||c+1>=vertices.length)return;
        var x0=vertices[a],y0=vertices[a+1],x1=vertices[c],y1=vertices[c+1],dx=x1-x0,dy=y1-y0,len=Math.sqrt(dx*dx+dy*dy)||1,nx=-dy/len*half,ny=dx/len*half;
        var u0=uvs[a],v0=uvs[a+1],u1=uvs[c],v1=uvs[c+1];
        var q=base+(cur+n)*QSF;SGB[q]=tid;
        SGB[q+1]=x0+nx;SGB[q+2]=y0+ny;SGB[q+3]=x1+nx;SGB[q+4]=y1+ny;SGB[q+5]=x1-nx;SGB[q+6]=y1-ny;SGB[q+7]=x0-nx;SGB[q+8]=y0-ny;
        SGB[q+9]=u0;SGB[q+10]=v0;SGB[q+11]=u1;SGB[q+12]=v1;SGB[q+13]=u1;SGB[q+14]=v1;SGB[q+15]=u0;SGB[q+16]=v0;
        SGB[q+17]=tint;SGB[q+18]=alpha;SGB[q+19]=-1;SGB[q+20]=-1;SGB[q+21]=0;SGB[q+22]=pm;SGB[q+23]=0;n++;
      };
      var emitPoint=function(i0){
        var a=i0*2;if(a<0||a+1>=vertices.length)return;
        var x=vertices[a],y=vertices[a+1],u=uvs[a],v=uvs[a+1];
        var q=base+(cur+n)*QSF;SGB[q]=tid;
        SGB[q+1]=x-half;SGB[q+2]=y-half;SGB[q+3]=x+half;SGB[q+4]=y-half;SGB[q+5]=x+half;SGB[q+6]=y+half;SGB[q+7]=x-half;SGB[q+8]=y+half;
        SGB[q+9]=u;SGB[q+10]=v;SGB[q+11]=u;SGB[q+12]=v;SGB[q+13]=u;SGB[q+14]=v;SGB[q+15]=u;SGB[q+16]=v;
        SGB[q+17]=tint;SGB[q+18]=alpha;SGB[q+19]=-1;SGB[q+20]=-1;SGB[q+21]=0;SGB[q+22]=pm;SGB[q+23]=0;n++;
      };
      if(isTri||isStrip||isFan){
        for(var tri=0;tri<need;tri++){
          var pa,pb,pc;
          if(isTri){pa=start+tri*3;pb=pa+1;pc=pa+2;}
          else if(isStrip){pa=start+tri;pb=pa+1;pc=pa+2;if(tri&1){var ps=pa;pa=pb;pb=ps;}}
          else {pa=start;pb=start+tri+1;pc=pb+1;}
          emitTri(at(pa),at(pb),at(pc));
        }
      }else if(isLines){
        for(var li=0;li+1<size;li+=2)emitLine(at(start+li),at(start+li+1));
      }else if(isLineStrip){
        for(var ls=0;ls+1<size;ls++)emitLine(at(start+ls),at(start+ls+1));
      }else if(isLineLoop){
        for(var ll=0;ll<size;ll++)emitLine(at(start+ll),at(start+((ll+1)%size)));
      }else if(isPoints){
        for(var pt=0;pt<size;pt++)emitPoint(at(start+pt));
      }
      if(n>0){SGB[b+oQUADOFF]=cur;SGB[b+oQUADCNT]=n;__sgTypeW(b,3);SGB[b+oBLEND]=o.__nativeNodeProxy?(o._blendMode||0):(o.blendMode||0);SGB[b+oPREMUL]=pm;}
      else __sgTypeW(b,0);
    };
    // TilingSprite: one native quad carries Pixi's inverse tile transform. The fragment shader
    // repeats those affine coordinates and remaps them through the atlas UV corners, so rotation,
    // skew, pivot, uvRespectAnchor and arbitrarily dense tiling never need a snapshot or N quads.
    var __wrTiling = function(o){
      var b=o.__i*ST, tex=o._texture||o.texture; if(!tex || !tex.valid || !tex.orig){ __setEsc(o,b,'tiling-no-tex'); return; }
      var tt=o.tileTransform||{},bt=tex.baseTexture, tid=__nativeTexId(bt, true);
      if(!tid){ __setEsc(o,b,'tiling-no-gltex'); return; }
      __addBaseTextureOwner(bt,o);
      var w=(o._width!==undefined&&o._width!==null)?o._width:o.width, h=(o._height!==undefined&&o._height!==null)?o._height:o.height, ax=o._anchor?o._anchor._x:0, ay=o._anchor?o._anchor._y:0, ox=-ax*w, oy=-ay*h;
      if(!(w>0&&h>0)){__sgTypeW(b,0);SGB[b+oQUADCNT]=0;return;}
      var uvs=tex._uvs; if(!uvs && tex.updateUvs){ try{ tex.updateUvs(); uvs=tex._uvs; }catch(e){} }
      if(!uvs){ __setEsc(o,b,'tiling-no-uv'); return; }
      var lt=tt.localTransform;if(tt.updateLocalTransform)try{tt.updateLocalTransform();lt=tt.localTransform;}catch(e){}
      if(!lt){var sc=tt.scale||{x:1,y:1},sk=tt.skew||{x:0,y:0},ps=tt.position||{x:0,y:0},pv=tt.pivot||{x:0,y:0},rot=Number(tt.rotation)||0;
        var a0=Math.cos(rot+(Number(sk.y)||0))*(Number(sc.x)||0),b0=Math.sin(rot+(Number(sk.y)||0))*(Number(sc.x)||0),c0=-Math.sin(rot-(Number(sk.x)||0))*(Number(sc.y)||0),d0=Math.cos(rot-(Number(sk.x)||0))*(Number(sc.y)||0);
        lt={a:a0,b:b0,c:c0,d:d0,tx:(Number(ps.x)||0)-(Number(pv.x)||0)*a0-(Number(pv.y)||0)*c0,ty:(Number(ps.y)||0)-(Number(pv.x)||0)*b0-(Number(pv.y)||0)*d0};
      }
      var texW=Number(tex.width||tex.orig.width)||1,texH=Number(tex.height||tex.orig.height)||1,ma=lt.a*texW/w,mb=lt.b*texW/h,mc=lt.c*texH/w,md=lt.d*texH/h,mtx=lt.tx/w,mty=lt.ty/h,det=ma*md-mb*mc;
      if(Math.abs(det)<1e-8){__setEsc(o,b,'tiling-zero-transform');return;}
      var ia=md/det,ib=-mb/det,ic=-mc/det,id=ma/det,itx=(mc*mty-md*mtx)/det,ity=(mb*mtx-ma*mty)/det,anchorX=o.uvRespectAnchor?ax:0,anchorY=o.uvRespectAnchor?ay:0;
      var tileAt=function(x,y){return [ia*x+ic*y+itx,ib*x+id*y+ity];},t0=tileAt(-anchorX,-anchorY),t1=tileAt(1-anchorX,-anchorY),t2=tileAt(1-anchorX,1-anchorY),t3=tileAt(-anchorX,1-anchorY);
      var cur=__qEnsure(o,1), base=QB+1, tint=__tintOf(o), tpm=__premulOf(bt);
      if(cur<0){ __setEsc(o,b,'tiling-quad-pool'); return; }
      var q=base+cur*QSF;SGB[q]=tid;SGB[q+1]=ox;SGB[q+2]=oy;SGB[q+3]=ox+w;SGB[q+4]=oy;SGB[q+5]=ox+w;SGB[q+6]=oy+h;SGB[q+7]=ox;SGB[q+8]=oy+h;
      SGB[q+9]=t0[0];SGB[q+10]=t0[1];SGB[q+11]=t1[0];SGB[q+12]=t1[1];SGB[q+13]=t2[0];SGB[q+14]=t2[1];SGB[q+15]=t3[0];SGB[q+16]=t3[1];
      SGB[q+17]=tint;SGB[q+18]=1;SGB[q+19]=-1;SGB[q+20]=-1;SGB[q+21]=0;SGB[q+22]=tpm;SGB[q+23]=1;
      var au=[uvs.x0,uvs.y0,uvs.x1,uvs.y1,uvs.x2,uvs.y2,uvs.x3,uvs.y3];if(o._flipX)au=[au[2],au[3],au[0],au[1],au[6],au[7],au[4],au[5]];if(o._flipY)au=[au[6],au[7],au[4],au[5],au[2],au[3],au[0],au[1]];
      // 图集子区平铺:shader 在四角间按 fract 插值,边界正好落在子区边缘,GL_LINEAR 会混入
      // 邻块半个 texel(每个平铺周期一条 1px 渗色缝)。四角向形心内缩半 texel(rotate 子区同样成立)。
      {var tbw=Number(bt.realWidth||bt.width)||1,tbh=Number(bt.realHeight||bt.height)||1,thu=0.5/tbw,thv=0.5/tbh;
       var tcx=(au[0]+au[2]+au[4]+au[6])*0.25,tcy=(au[1]+au[3]+au[5]+au[7])*0.25;
       var tsh=function(v,c,hstep){var d=c-v;if(d>hstep)return v+hstep;if(d<-hstep)return v-hstep;return c;};
       for(var aus=0;aus<8;aus+=2){au[aus]=tsh(au[aus],tcx,thu);au[aus+1]=tsh(au[aus+1],tcy,thv);}}
      for(var aui=0;aui<8;aui++)SGB[q+24+aui]=au[aui];
      SGB[b+oQUADOFF]=cur;SGB[b+oQUADCNT]=1;__sgTypeW(b,3);SGB[b+oBLEND]=o.__nativeNodeProxy?(o._blendMode||0):(o.blendMode||0);SGB[b+oPREMUL]=tpm;
    };
    var __wrType = function(o){         // TYPE 判定:sprite=1;未知 filter/重写渲染/未知 mask=2 逃生舱;纯容器=0
      if(!o || o.__i===undefined || o.__i<=0) return;
      __markBoundsNode(o.__i);          // 本次改写的一切内容(类型/纹理 rect/quad/命中元数据)→ 该节点 bounds 脏
      // B0-3 修订:曾在此对「当前 TYPE==4」预置来源切换位,但 C++ spine 每帧写回 TYPE=4 与
      // 预置形成置位互搏(canonical 清→C++ 写 4→JS 又置,pass=3 dirty=0 仍 DEBT>0 的 atomic
      // 空转)。回退预置——正确性由两层覆盖:①computeNode 的 LTYPE 自持检测(重建来源≠当前
      // TYPE 即强制重建,任何触达即纠正);②每 300 帧 verify 强制 markD(stage)=全树 parentDirty
      // 触达,切换后纠正上限 5s。C++ 归还路径(restoreSlotNode)仍显式置 bit2。TYPE 写入口的
      // 统一置位归 Phase B Router(写前读旧值,真转换才置)。
      var b=o.__i*ST, ty=0;o.__sgEscapeReason=null;__writeHitMeta(o);__wrFilters(o);
      // Authored FGUI masks keep their mesh/texture in the native descriptor. A facade refresh
      // must neither paint that payload nor clear it through the generic Pixi-mask branch.
      if(o.__nativeFguiMaskFlags&4){SGB[b+oREND]=0;return;}
      if(o.__maskOf||o.isMask){ SGB[b+oMASKI]=0; if(globalThis.__nativeMaskContract.usesRetainedGraphics(o)){__sgTypeW(b,0);}else if(o.__nativeProxyGeometryMask&&typeof o._syncNativeGraphics==='function')o._syncNativeGraphics(true);else if(o.__nativeGeometryMask)__wrGfx(o,true);else{__sgTypeW(b,0);SGB[b+oQUADCNT]=0;} return; }   // Pixi mask object 只参与 stencil,不能再按普通图元画到屏幕
      // mask:矩形 Graphics 走 scissor，纹理/圆椭圆走 stencil；未知形状才按兜底策略处理。
      var mk=o._mask, mo=mk?(mk.isMaskData?mk.maskObject:mk):null, mt=null, mrect=null, mobj=null;
      if(mo){
        // FGUI commonly assigns an empty Graphics first and calls drawRect/drawShape later.
        // Establish the reverse dependency before inspecting geometry so those later drawing
        // hooks always invalidate this owner instead of leaving its first T_ESCAPE result cached.
        if(!(mo.__i>0))__bind(mo);
        __claimOwnedMasks(o,[{obj:mo,kind:null}]);
        mt=__rectMaskTargetOf(mo); mrect=mt&&mt.rect; mobj=(mt&&mt.obj)||mo;
        // SplashProgressBar uses a stretched Sprite only as a rectangular reveal mask for a
        // TilingSprite (the visible rounded border is a separate NineSlicePlane).  Treating that
        // Sprite as an alpha stencil makes the native path sample/repeat its atlas frame and can
        // leave the final part of the tiled bar unrevealed. Pixi's effective result here is a
        // changing axis-aligned rectangle, so use the native scissor path. Keep ordinary Sprite
        // masks on the alpha-stencil path; they may contain real silhouettes.
        if(mt&&mt.alpha&&PIXI.TilingSprite&&o instanceof PIXI.TilingSprite&&
           (o.name==='arrow'||mo.name==='progress_mask'||mo.__nativeRectMask===true)){
          mt.alpha=false;
        }
        if(mrect){
          if(!(mobj.__i>0)) __bind(mobj);   // fgui 常先设 mask 再 addChild:先绑上(树挂接由后续 addChild 完成)
          var maskEntries=[{obj:mobj,kind:mt}];
          if(mt.rects&&mt.rects.length>1)for(var mei=0;mei<mt.rects.length;mei++)if(mt.rects[mei]&&mt.rects[mei].obj)maskEntries.push({obj:mt.rects[mei].obj,kind:null});
          __claimOwnedMasks(o,maskEntries);
        }
        if(mrect && mobj.__i>0){ SGB[b+oMASKI]=mobj.__i;
          if(mt.proxyGeometry&&typeof mobj._syncNativeGraphics==='function')mobj._syncNativeGraphics(true);
          else if(mt.geometry)__wrGfx(mobj,true);
          var mb=mobj.__i*ST; SGB[mb+oRX]=mrect.x; SGB[mb+oRX+1]=mrect.y; SGB[mb+oRX+2]=mrect.width; SGB[mb+oRX+3]=mrect.height;
          if(mt.rects&&mt.rects.length>1){
            for(var mri=0;mri<mt.rects.length;mri++){
              var mrt=mt.rects[mri], mro=mrt&&mrt.obj, mrr=mrt&&mrt.rect; if(!mro||!mrr)continue;
              if(!(mro.__i>0))__bind(mro); if(!(mro.__i>0))continue;
              var mrb=mro.__i*ST;
              SGB[mrb+oRX]=mrr.x;SGB[mrb+oRX+1]=mrr.y;SGB[mrb+oRX+2]=mrr.width;SGB[mrb+oRX+3]=mrr.height;
              __sgTypeW(mrb,0);SGB[mrb+oQUADCNT]=0;
            }
          }
        }
        else {
          // Pixi 的空 mask 表示完全裁掉。FGUI 的 0% ProgressBar 正是 0x0 UISprite；过去拿 owner
          // 满尺寸兜底会把 0% 画成 100%。保留独立 mask 节点并写零宽/零高 rect，后续纹理或 transform
          // 更新仍能直接驱动同一个 MASKI。
          var ownRect=__ownerMaskRectOf(o);
          if(ownRect&&mobj){
            if(!(mobj.__i>0))__bind(mobj);
            if(mobj.__i>0){var __zmb=mobj.__i*ST,__zh=ownRect.width>=ownRect.height;__claimOwnedMasks(o,[{obj:mobj,kind:null}]);SGB[b+oMASKI]=mobj.__i;SGB[__zmb+oRX]=ownRect.x;SGB[__zmb+oRX+1]=ownRect.y;SGB[__zmb+oRX+2]=__zh?0:ownRect.width;SGB[__zmb+oRX+3]=__zh?ownRect.height:0;__sgTypeW(__zmb,0);SGB[__zmb+oQUADCNT]=0;}
            else {SGB[b+oMASKI]=0;ty=2;}
          }
          else if(mobj&&mobj.geometry&&mobj.__i>0){
            // A genuinely empty Graphics mask means "clip everything". Keep a zero-sized native
            // mask now; drawShape/drawRect will mark the owner and replace it with exact geometry.
            var __emb=mobj.__i*ST;SGB[b+oMASKI]=mobj.__i;SGB[__emb+oRX]=0;SGB[__emb+oRX+1]=0;SGB[__emb+oRX+2]=0;SGB[__emb+oRX+3]=0;__sgTypeW(__emb,0);SGB[__emb+oQUADCNT]=0;
          }
          else { SGB[b+oMASKI]=0; o.__sgEscExtra=__maskDesc(mo); o.__sgEscapeReason='mask'; ty=2; }
        }
      } else {
        // Native FGUI may own a package-authored mask that has no retained Pixi `_mask` object.
        // Generic facade refreshes (notably addChild on a reel_view) must preserve that mask id;
        // `_mask == null` only means that no runtime Pixi mask overrides it. Removing a temporary
        // runtime mask naturally restores the authored clip instead of disabling clipping.
        var authoredMask=o.__nativeFguiClipMaskId|0;
        SGB[b+oMASKI]=authoredMask>0?authoredMask:0;
        __releaseOwnedMasks(o,null); o.__sgMaskObjs=[];o.__sgMaskObj=null;
      }
      if(ty===0){
        if(o.filters && o.filters.length && (globalThis.__fltN||0)<24){ var unsupported=0,fn='';for(var fi=0;fi<o.filters.length;fi++){var ff=o.filters[fi];fn+=((ff.constructor&&ff.constructor.name)||'?')+' ';if(!__filterNativeKind(ff))unsupported++;}if(unsupported){globalThis.__fltN=(globalThis.__fltN||0)+1;__log('[filter] '+((o.constructor&&o.constructor.name)||'?')+(o.visible!==false&&(o.worldAlpha===undefined||o.worldAlpha>0.02)?'(可见)':'')+' → '+fn+'(逃逸)');}}   // 已登记的 Filter 走 native；未知 Filter 必须显式记 T_ESCAPE
        if(!__filtersAllNative(o.filters)||o.__sgFilterUnavailable||o.__sgPostFilterConflict){o.__sgEscapeReason=o.__sgPostFilterConflict?'filter-stack':(o.__sgFilterUnavailable?'filter-resource':'filter');ty=2;}
        // Generic cacheAsBitmap: main-screen never calls DisplayObject.render, so Pixi's
        // _renderCached never runs. Bake once through the existing native RenderTexture path
        // (renderer.render → __nativeRenderTexture) and thereafter paint the cached sprite.
        // No per-feature special cases — any DisplayObject that sets cacheAsBitmap uses this.
        else if(__tryNativeCachedBitmap(o,b))return;
        // NativeSpine 门面(spinex P1b):quads/TYPE 由 C++ spinex setNativeQuads 直挂(T_NATIVE_MULTIQUAD)。
        // 既不走旧 __wrSpine,也不落容器尾部(那会把 TYPE 抹回 0,show/重挂过渡帧丢 quads);同 __nativeFguiFacade 语义保留 native 类型。
        else if(o.__spxFacade){ SGB[b+oREND]=(o._renderable===false?0:1); return; }
        // 旧 __wrSpine(JS 侧蒙皮/形变→逐 quad 回写)已退役:全库骨架 4.2+.skel,spine 全部走门面,
        // 且包内已无 .json 可回退,非门面的 spine 不可能再出现(出现即在 NativeSpine 构造处点名抛错)。
        else if(o.__nativeTilingProxy&&typeof o._syncNativeTiling==='function'){SGB[b+oREND]=(o._renderable===false?0:1);o._syncNativeTiling();return;}
        else if(o.__nativeNineProxy||__is9Slice(o)){ SGB[b+oREND]=(o._renderable===false?0:1); __wr9Slice(o); return; }   // M4:自己写 TYPE=3 + quad
        else if(o.__nativeGraphicsProxy&&typeof o._syncNativeGraphics==='function'){
          SGB[b+oREND]=(o._renderable===false?0:1);
          if(globalThis.__nativeMaskContract.usesRetainedGraphics(o)){
            // Releasing a runtime mask schedules a normal visual refresh. Restore the retained
            // authored payload's drawable type without rebuilding it from the intentionally empty
            // JS command list, so repeated mask1→mask2→mask1 switches remain lossless.
            __sgTypeW(b,SGB[b+oQUADCNT]>0?5:0);return;
          }
          o._syncNativeGraphics();return;
        }
        else if(PIXI.Graphics && o instanceof PIXI.Graphics){ SGB[b+oREND]=(o._renderable===false?0:1); __wrGfx(o); return; }                 // M5:矩形填充→多-quad
        else if(o.__nativeBitmapText&&typeof o.validate==='function'){SGB[b+oREND]=(o._renderable===false?0:1);o.validate();return;}
        else if(PIXI.BitmapText && o instanceof PIXI.BitmapText){ SGB[b+oREND]=(o._renderable===false?0:1); __wrBmp(o); return; }            // 每字符→quad(page mesh 子节点逃逸跳过)
        else if(PIXI.ParticleContainer && o instanceof PIXI.ParticleContainer){SGB[b+oREND]=(o._renderable===false?0:1);__wrParticle(o);return;}
        else if(PIXI.TilingSprite && o instanceof PIXI.TilingSprite){ SGB[b+oREND]=(o._renderable===false?0:1); __wrTiling(o); return; }   // 平铺:多 quad(继承 Sprite,须在 isSprite 前)
        else if(PIXI.Mesh && o instanceof PIXI.Mesh){ var _mcn=(o.constructor&&o.constructor.name)||''; SGB[b+oREND]=(o._renderable===false?0:1); if(_mcn.indexOf('SlotMesh')>=0||(o.parent&&PIXI.BitmapText&&o.parent instanceof PIXI.BitmapText)){__sgTypeW(b,0);SGB[b+oQUADCNT]=0;}else __wrMesh(o); return; }
        else if(o.__nativeTextProxy&&typeof o._syncNativeText==='function'){SGB[b+oREND]=(o._renderable===false?0:1);o._syncNativeText(false);return;}
        // Native FGUI construction has already populated the immutable visual payload directly
        // in C++. A lazily materialised business facade intentionally does not keep the Pixi
        // texture/graphics object that produced it. Preserve that native type instead of
        // classifying the facade's compatibility render method as T_ESCAPE. GLoader also uses a
        // NativeContainerProxy, so preserve its package image before the generic container case.
        else if(o.__nativeFguiFacade){SGB[b+oREND]=(o._renderable===false?0:1);return;}
        // A lightweight component never paints through its own `render` method; it is only a
        // parent for native children. It can be attached while empty and populated afterwards.
        // Keep ordinary container proxies traversable even before their children arrive.
        else if(o.__nativeContainerProxy){ty=0;}
        else if(PIXI.Text && o instanceof PIXI.Text){ var _txt0=String(o.text||''), _tk0=_txt0+'|'+(o._styleID||0)+'|'+(o.localStyleID||0)+'|'+(o._resolution||0), _needTxUp=(o.dirty||o.__sgTextKey!==_tk0);
          // Mac semantic text path: keep Pixi TextMetrics temporarily for FGUI-compatible wrapping,
          // but send the whole laid-out block to CoreText once. No Canvas fillText/updateText or
          // Pixi texture upload participates in the native main-screen path.
          if(typeof globalThis.__nativeTextUpdate==='function'){
            try{
              var _tm=__syncNativeText(o);
              if(_tm){
                SGB[b+oBLEND]=o.__nativeNodeProxy?(o._blendMode||0):(o.blendMode||0);SGB[b+oTINT]=__tintOf(o);SGB[b+oREND]=(o._renderable===false?0:1);
                o.__sgTextKey=_tk0;o.__sgNativeText=1;o.dirty=false;return;
              }
            }catch(_nte){if(globalThis.__SG_DIAG===1&&(globalThis.__nativeTextErrN||0)<8){globalThis.__nativeTextErrN=(globalThis.__nativeTextErrN||0)+1;__log('[native-text] '+_nte.message);}}
          }
          try{ o.updateText(true); }catch(e){}
          try{ var _txTex=o._texture||o.texture, _txBT=_txTex&&_txTex.baseTexture;   // flip 短路 Text._render 后,手动把重绘的 canvas 上传到 GL
            if(_needTxUp){
              if(__forceUploadBT(_txBT,'text')){ o.__sgTextKey=_tk0; o.__sgTxRetry=0; }
              else if((o.__sgTxRetry=(o.__sgTxRetry||0)+1)<240){ __markRender(o);   // 上传失败:key 不前进,下帧重试(约 4s 上限防死循环)
                if((globalThis.__txRetryN=(globalThis.__txRetryN||0)+1)<=8) __log('[text-upload-retry] "'+_txt0.slice(0,16)+'" valid='+(_txBT&&_txBT.valid?1:0)+' retry='+o.__sgTxRetry);
              } else o.__sgTextKey=_tk0;   // 重试超限:放弃并消费,避免每帧空转
            } else o.__sgTextKey=_tk0;
          }catch(e){}
          SGB[b+oREND]=(o._renderable===false?0:1); __wrTex(o); __sgTypeW(b,(SGB[b+oTEXID]>0)?1:2);
          var _txt=String(o.text||''), _txSeen=globalThis.__txSeen||(globalThis.__txSeen={});   // text-dbg:按内容去重(不被重复帧刷爆),抓金币数字 + 位置/alpha(看画屏外/被盖)
          if(globalThis.__SG_DIAG===1 && _txt.replace(/\s/g,'').length>0 && !_txSeen[_txt] && (globalThis.__txDbgC||0)<120){ _txSeen[_txt]=1; globalThis.__txDbgC=(globalThis.__txDbgC||0)+1; var _tt=o._texture, _bt=_tt&&_tt.baseTexture, _wt=o.worldTransform;
            __log('[text-dbg] "'+_txt.slice(0,16)+'" TEXID='+(SGB[b+oTEXID]|0)+' texValid='+(_tt&&_tt.valid?1:0)+' bt='+(_bt?(_bt.realWidth||_bt.width)+'x'+(_bt.realHeight||_bt.height):'null')+' rend='+(o._renderable===false?0:1)+' wa='+(o.worldAlpha!==undefined?o.worldAlpha.toFixed(2):'?')+' pos='+(_wt?((_wt.tx|0)+','+(_wt.ty|0)):'?')+' sc='+(_wt?(_wt.a.toFixed(2)):'?')); }
          if(globalThis.__SG_DIAG===1 && /^\d+%$/.test(_txt) && (globalThis.__pctDbgC||0)<40){ var _pk=(o.__i|0)+'/'+_txt, _ps=globalThis.__pctSeen||(globalThis.__pctSeen={});
            if(!_ps[_pk]){ _ps[_pk]=1; globalThis.__pctDbgC=(globalThis.__pctDbgC||0)+1; var _pc='', _pn=o, _pd=0, _pwt=o.worldTransform;
              while(_pn&&_pd<7){ _pc+=' ['+_pd+']'+(_pn.name||((_pn.constructor&&_pn.constructor.name)||'?'))+' i'+((_pn.__i)|0)+' ch='+(_pn.children?_pn.children.length:0); _pn=_pn.parent; _pd++; }
              __log('[pct-dbg] "'+_txt+'" i='+(o.__i|0)+' pos='+(_pwt?((_pwt.tx|0)+','+(_pwt.ty|0)):'?')+' scale='+(_pwt?_pwt.a.toFixed(2):'?')+_pc); } }
          if(globalThis.__SG_DIAG===1 && /[\d][\d,]{5,}/.test(_txt) && (globalThis.__txChainC||0)<2){ globalThis.__txChainC=(globalThis.__txChainC||0)+1;   // 金币/奖池长数字 → dump transform 链定位 -67 来源(哪一层 wty 冒负 + 是否有 pivot/skew/angle)
            var chain='', node=o, depth=0;   // 对比 o.y(JS新鲜) vs SGB local y(C++用的)+ caching node[2/3/4] 供每帧监控弹框开关时 y 变化
            while(node && depth<14){ var wt2=node.worldTransform, ii=node.__i|0, sgy=(ii>0)?(SGB[ii*ST+oY]|0):'NB', sgx=(ii>0)?(SGB[ii*ST+oX]|0):'NB', jy=node.y|0;
              if(depth===3) globalThis.__mon3=node; if(depth===2) globalThis.__mon2=node; if(depth===4) globalThis.__mon4=node;
              chain += ' ['+depth+']'+(node.name||(node.constructor&&node.constructor.name)||'?')+' i'+ii+' jy'+jy+' sgY'+sgy+((sgy!=='NB'&&sgy!==jy)?'★差':'')+' jx'+(node.x|0)+' sgX'+sgx+' wty'+(wt2?wt2.ty.toFixed(0):'?');
              node=node.parent; depth++; }
            __log('[text-chain] "'+_txt.slice(0,14)+'"'+chain); }
          return; }   // full flip 短路了 Text._render → 主动 updateText(true) 刷新画布纹理再画(否则 text 改了读旧纹理)
        else if(o.isSprite){var _stx=o._texture||o.texture;ty=(_stx&&_stx.valid&&_stx.orig)?1:0;}   // 无有效纹理时只作容器；loaded/update owner hook 会再切回 sprite
        else if(globalThis.__OVREND!==0 && o.children && o.children.length){ ty=0;   // 有子节点 → 当容器渲染(fgui/自定义容器:子节点各自处理,mask 走 MASKI)。__OVREND=0 关此行为
          if(o.render!==COp.render && o.render!==DOp.render){ var ot=(o.constructor&&o.constructor.name)||'?'; var OT=(globalThis.__ovrTally=globalThis.__ovrTally||{}); if(!OT[ot]){ OT[ot]=1; __log('[ov-render] 自定义render容器→当纯容器: '+ot+'(若它自身画背景/特效会丢,需审计)'); } }   // 审计:哪些自定义 render 容器被当纯容器
        }
        else if(o.render!==COp.render && o.render!==DOp.render){ o.__sgEscapeReason='custom-render'; ty=2; }   // 无子节点 + 自定义绘制(Mesh/custom draw)→ 逃逸
        else if(__ov.has(o)){ o.__sgEscapeReason='override-transform'; ty=2; }   // 剩余 __ov 成员(无子节点)→ 逃逸
      }
      if(ty===2){   // 只数可见逃逸;细分原因留给 __escNote 统一处理
        var __er=(mo&&!mrect)?'mask':((o.filters&&o.filters.length)?'filter':(__ov.has(o)?'ov':'render'));
        if(!o.__sgEscapeReason)o.__sgEscapeReason=__er;
        __escNote(o, __er);
      }
      __sgTypeW(b,ty); SGB[b+oREND]=(o._renderable===false?0:1);
      if(ty===1) __wrTex(o);
    };
    globalThis.__sgWrTex=__wrTex; globalThis.__sgWrType=__wrType;globalThis.__sgWriteTiling=__wrTiling;
