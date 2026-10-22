// Generic viewport, event and image compatibility.
globalThis.__badFrame = function(){ globalThis.__badFrameCount=(globalThis.__badFrameCount||0)+1; };
// S12: pixi EventSystem 在 document/window 上注册 pointermove/pointerup（捕获画布外的移动/抬起）
var _docLst = Object.create(null);
globalThis.__nativeInstallEventTarget(globalThis,_docLst);
// AppKit window resizing enters through this browser-shaped bridge.  Update viewport-backed DOM
// shims before dispatching resize so Pixi's ResizePlugin reads the new client size, then let the
// game's normal StageUtils/FairyGUI listeners perform their shared responsive layout.
globalThis.__resizeEvent = function(width, height, dpr){
  width=Math.max(1,Math.round(Number(width)||1));
  height=Math.max(1,Math.round(Number(height)||1));
  dpr=Number(dpr)||1;
  globalThis.innerWidth=width; globalThis.innerHeight=height; globalThis.devicePixelRatio=dpr;
  if(typeof _elemCache==='object') for(var key in _elemCache){
    var el=_elemCache[key]; if(!el)continue;
    el.clientWidth=el.offsetWidth=el.scrollWidth=width;
    el.clientHeight=el.offsetHeight=el.scrollHeight=height;
  }
  var viewportNodes=[globalThis.document&&document.body,globalThis.document&&document.documentElement,
    typeof _gBody!=='undefined'&&_gBody,typeof _gDocEl!=='undefined'&&_gDocEl];
  for(var vi=0;vi<viewportNodes.length;vi++){
    var node=viewportNodes[vi]; if(!node)continue;
    node.clientWidth=node.offsetWidth=node.scrollWidth=width;
    node.clientHeight=node.offsetHeight=node.scrollHeight=height;
  }
  // The page canvas stays full-size; game_screen may leave its natural surplus to the footer.
  var page=globalThis.__nativeBoot&&globalThis.__nativeBoot.get('page-footer');
  if(page)page.prepareViewport();
  // Pixi's ResizePlugin intentionally defers renderer.resize() to requestAnimationFrame. That is
  // safe in a browser because the compositor keeps presenting the previous canvas until the RAF
  // resize/render is complete. The native host has already resized its drawable before entering
  // this callback, so deferring Pixi exposes a mixed frame in the new viewport. Move the renderer
  // into the new coordinate space before any game/FairyGUI layout listener can publish transforms.
  var resizeApp=globalThis.__PIXI_APP__||globalThis.__pixiApp;
  if(resizeApp&&resizeApp.renderer&&typeof resizeApp.renderer.resize==='function'){
    try{
      if(typeof resizeApp.cancelResize==='function')resizeApp.cancelResize();
      var oldResolution=Number(resizeApp.renderer.resolution)||1;
      var resolutionChanged=Math.abs(oldResolution-dpr)>0.0001;
      if(resolutionChanged)resizeApp.renderer.resolution=dpr;
      if(globalThis.PIXI&&PIXI.settings)PIXI.settings.RESOLUTION=dpr;
      var resizeScreen=resizeApp.renderer.screen;
      if(resolutionChanged||!resizeScreen||Math.round(Number(resizeScreen.width)||0)!==width||Math.round(Number(resizeScreen.height)||0)!==height)
        resizeApp.renderer.resize(width,height);
    }catch(resizeError){__log('[game] renderer resize exception: '+resizeError.message);}
  }
  var ev={type:'resize',target:globalThis,currentTarget:globalThis,bubbles:false,cancelable:false,
    width:width,height:height,preventDefault:function(){},stopPropagation:function(){}};
  var handler=globalThis.onresize;
  if(typeof handler==='function')try{handler.call(globalThis,ev);}catch(e){__log('[game] resize handler exception: '+e.message);}
  var listeners=(_docLst.resize||[]).slice();
  for(var i=0;i<listeners.length;i++)try{listeners[i].call(globalThis,ev);}catch(e2){__log('[game] resize callback exception: '+e2.message);}
  // FairyGUI's browser UIStage deliberately debounces updateScreenSize() by 300ms while CSS
  // stretches the last canvas frame.  Native has no CSS compositor for that interim state: GRoot
  // has already adopted the new game scale above, while UIStage still exposes its old stage size.
  // Complete the same public UIStage resize synchronously before this native callback can return
  // to the renderer.  The library's later debounced call is idempotent.
  // Read the existing singleton directly; the `inst` getter would create an unattached GRoot if
  // AppKit delivered an early resize while the game was still bootstrapping.
  var fguiRoot=globalThis.fgui&&globalThis.fgui.GRoot&&globalThis.fgui.GRoot.$inst;
  var fguiStage=fguiRoot&&fguiRoot.$uiStage;
  if(fguiStage&&typeof fguiStage.updateScreenSize==='function'){
    try{fguiStage.updateScreenSize();}catch(fguiResizeError){__log('[game] FairyGUI stage resize exception: '+fguiResizeError.message);}
  }
  // The ResizePlugin listener above queued the same resize for the next RAF. Continuous native
  // rendering has already applied it, so suppress that redundant render of an intermediate tree.
  if(resizeApp&&typeof resizeApp.cancelResize==='function')resizeApp.cancelResize();
};
globalThis.PointerEvent = function(){};   // 定义后 pixi 走统一 pointer 事件路径（而非 mouse 回退）
globalThis.matchMedia = function(){ return { matches:false, addListener:function(){}, removeListener:function(){}, addEventListener:function(){}, removeEventListener:function(){} }; };
globalThis.location = { href:'file:///app/', origin:'file://', protocol:'file:', pathname:'/app/', host:'', hostname:'', search:'', hash:'' };
globalThis.document = {
  createElement:function(tag){
    if (tag==='canvas') return makeCanvas(1,1);
    if (tag==='audio' || tag==='video') return globalThis.Audio ? new globalThis.Audio() : { canPlayType:function(){ return ''; }, style:{}, addEventListener:function(){}, removeEventListener:function(){} };
    if (tag==='input' || tag==='textarea') return globalThis.__mkInput ? globalThis.__mkInput(tag==='textarea') : { style:{}, addEventListener:function(){}, removeEventListener:function(){} };
    return { style:{}, addEventListener:function(){}, removeEventListener:function(){} };
  },
  body:{ appendChild:function(){}, removeChild:function(){}, addEventListener:function(){}, removeEventListener:function(){} },
  documentElement:{ style:{} },
  activeElement:null   // 输入垫片 focus/blur 维护
};
globalThis.__nativeInstallEventTarget(globalThis.document,_docLst);


// S6: Image 垫片——src 设置时加载解码(预乘+翻正 RGBA),load/error 事件异步派发(浏览器语义)。
// 加载路径:native 有 __loadImageAsync 时 URL 拉取+解码在后台线程；有
// __decodeImageAsync 时 ZIP/blob 内存图片也在后台解码。旧宿主保留同步桥回退。
var __imgTk = 0, __imgPending = {};   // token → Image 实例
var __fileTk = 0, __filePending = {}; // token → fetch resolve 回调
// 后续解析(zip/spine/json)挤满每帧(实测 loading 期 JS 26ms/帧、10-21fps)。完成回调先入队,
// tick 每帧按预算(6ms)消费——loading 稍长一点,换 loading 动画流畅。
var __ioQ = [];
globalThis.__ioDrain = function(){
  if (!__ioQ.length) return;
  var t0 = performance.now(), budget = 6;
  while (__ioQ.length && (performance.now() - t0) < budget){
    var job = __ioQ.shift();
    if (globalThis.__PROF === 1){   // 单 job 计时(__PROF=1 才计)
      var j0 = performance.now();
      try { job(); } catch(e){ __log('[io-pace] 回调异常 '+e.message); }
      if (performance.now() - j0 > 30) __log('[io-slow] 单个 IO 回调 '+(performance.now()-j0).toFixed(0)+'ms');
    } else {
      try { job(); } catch(e){ __log('[io-pace] 回调异常 '+e.message); }
    }
  }
};
globalThis.__fileDone = function(tk, data){   // native 后台读取完成回调(data: ArrayBuffer|null)
  var cb = __filePending[tk]; delete __filePending[tk];
  if (!cb) return;
  // 先归还网络并发槽，再按帧派发 Response。两套背压不能串联，否则回调队列会让 CDN 空转。
  if(cb.__transportDone)try{cb.__transportDone();}catch(_transportDoneError){}
  if (globalThis.__IO_PACE === 0){ cb(data); return; }
  __ioQ.push(function(){ cb(data); });
};
globalThis.__nativeFguiPreparseDone = function(tk, ok, inflateMs, xmlMs, entries){
  var cb=__fguiPrePending[tk];delete __fguiPrePending[tk];if(!cb)return;
  var finish=function(){cb(!!ok,inflateMs,xmlMs,entries);};
  if(globalThis.__IO_PACE===0){finish();return;}
  __ioQ.push(finish);
};
globalThis.__nativeSpinePreparseDone = function(tk, ok, handle, zipMs, jsonMs, entries, images, leaseHeld){
  // 在途 lease:native 已在 notify 排队**之前**的同步块为 wantsLease 的 waiter 获取引用(新
  // holder 以 lease-waiter 数出生,插入/排队期的逐出全程免疫——JS 侧任何时点 pin 都有窗口,
  // 故不在此获取)。leaseHeld=native 回传的本 waiter 是否携带引用(legacy 链 wantsLease=0 不
  // 携带,记账给它必然泄漏)。本回调只做移交:记账进 __spinePinnedHandles 计数表(同 handle
  // 多条链各取各的),__nativeSpineLoadPrepared 取用并 finally 释放;token 失配立即退还。
  // 旧宿主(7 参调用/无 unpin 桥)= native 未获取,全程 no-op。
  var leased=!!leaseHeld&&(handle|0)>0&&typeof globalThis.__nativeSpineUnpin==='function';
  var cb=__spinePrePending[tk];delete __spinePrePending[tk];
  if(!cb){if(leased)try{globalThis.__nativeSpineUnpin(handle|0);}catch(e){}return;}
  if(leased){var pt=globalThis.__spinePinnedHandles||(globalThis.__spinePinnedHandles={});pt[handle|0]=(pt[handle|0]||0)+1;}
  var finish=function(){cb(!!ok,handle,zipMs,jsonMs,entries,images);};
  if(globalThis.__IO_PACE===0){finish();return;}
  __ioQ.push(finish);
};
