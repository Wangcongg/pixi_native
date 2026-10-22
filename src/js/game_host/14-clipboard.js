// ================= Native clipboard / legacy DOM copy contract =================
// The game bundles still use the browser-era textarea.select() + document.execCommand('copy')
// path. JSC has neither a DOM selection nor a system clipboard, so keep just enough selection
// state to preserve that contract and hand the selected text to the platform host synchronously.
(function(){
  var selectedTarget = null;
  globalThis.__nativeClipboardSelect = function(target, start, end){
    if(!target) return;
    var value=String(target.value==null?'':target.value), length=value.length;
    start=Math.max(0,Math.min(length,Number(start)||0));
    end=end==null?length:Math.max(0,Math.min(length,Number(end)||0));
    target.selectionStart=Math.min(start,end);
    target.selectionEnd=Math.max(start,end);
    selectedTarget=target;
  };
  globalThis.getSelection = globalThis.getSelection || function(){
    return {rangeCount:0,getRangeAt:function(){return null;},removeAllRanges:function(){},addRange:function(){}};
  };
  globalThis.document.queryCommandSupported = function(command){ return String(command).toLowerCase()==='copy'; };
  globalThis.document.queryCommandEnabled = globalThis.document.queryCommandSupported;
  globalThis.document.execCommand = function(command){
    if(String(command).toLowerCase()!=='copy'||!selectedTarget) return false;
    var value=String(selectedTarget.value==null?'':selectedTarget.value), length=value.length;
    var start=Math.max(0,Math.min(length,Number(selectedTarget.selectionStart)||0));
    var end=Math.max(start,Math.min(length,Number(selectedTarget.selectionEnd)||0));
    if(typeof globalThis.__clipboardWrite!=='function') return false;
    try{return globalThis.__clipboardWrite(value.slice(start,end))!==false;}
    catch(e){if(typeof __log==='function')__log('[clipboard] write failed: '+e.message);return false;}
  };
  if(globalThis.navigator){
    globalThis.navigator.clipboard=globalThis.navigator.clipboard||{};
    if(typeof globalThis.navigator.clipboard.writeText!=='function'){
      globalThis.navigator.clipboard.writeText=function(text){
        return new Promise(function(resolve,reject){
          try{
            if(typeof globalThis.__clipboardWrite==='function'&&globalThis.__clipboardWrite(String(text))!==false)resolve();
            else reject(new Error('Native clipboard is unavailable'));
          }catch(e){reject(e);}
        });
      };
    }
  }
})();

// 动态 <script> 加载：桥到 host __evalScript（顶层 var 挂全局，同浏览器 <script>）。
// 浏览器语义：脚本插入文档 + 有 src，两者齐了才开始加载——createjs/preloadjs 先 append 后设 src，
// 故不能在 appendChild 当下就 fire（那时 src 还空）；改为 append 与 src 任一后触发，两条件齐了才真加载。
// 库源码补丁：某些内联库的错误清理路径会掩盖真实错误，eval 前打补丁。
function __patchLibCode(code){ return code; }
function _makeScriptEl(){
  var s = makeEl('script'); s._loaded=false; s._appended=false; s._lst={}; s._code='';
  var _onload=null, _onerror=null;   // makeEl 把 onload/onerror setter 吞成 no-op，这里在实例上恢复真实存取
  Object.defineProperty(s,'onload', { get:function(){return _onload;},  set:function(v){_onload=v;},  configurable:true });
  Object.defineProperty(s,'onerror',{ get:function(){return _onerror;}, set:function(v){_onerror=v;}, configurable:true });
  // preloadjs 在 preferXHR 模式下：XHR 拉到 JS 文本 → document.createTextNode(code) 作子节点塞进 <script> → append 执行。
  // 收集内联代码：子文本节点 + 直接设的 .text/.textContent/.innerHTML。
  function inlineCode(){ if(s._code) return s._code; var t=''; for(var i=0;i<s.children.length;i++){ var c=s.children[i]; if(c&&c.nodeValue!=null) t+=c.nodeValue; } return t; }
  Object.defineProperty(s,'text',        { get:function(){return s._code;}, set:function(v){ s._code=(v==null?'':String(v)); tryFire(); }, configurable:true });
  Object.defineProperty(s,'textContent', { get:function(){return s._code;}, set:function(v){ s._code=(v==null?'':String(v)); tryFire(); }, configurable:true });
  Object.defineProperty(s,'innerHTML',   { get:function(){return s._code;}, set:function(v){ s._code=(v==null?'':String(v)); tryFire(); }, configurable:true });
  var _origAppend = s.appendChild;
  s.appendChild = function(c){ _origAppend.call(s,c); tryFire(); return c; };   // 文本节点塞进来后也试触发
  function tryFire(){
    if (s._loaded || !s._appended) return;       // 必须已插入文档
    var url = s._attrs.src || '', code = url ? '' : inlineCode();
    if (!url && !code) return;                    // src / 内联代码 二者皆无 → 还没准备好
    s._loaded = true;
    var ok = true, err = null, label = url || ('inline['+code.length+'B]:'+code.replace(/^[\s;]+/,'').slice(0,70).replace(/\s+/g,' '));
    // webpack 运行时靠 document.currentScript.src 推 publicPath；内联执行时给它一个带 http src 的当前脚本，避免
    // "Automatic publicPath is not supported"。url 已知则用真实地址，内联未知则退到 dev1 根（足够算出有效 publicPath）。
    var prevCur = globalThis.__curScript;
    var curSrc = url ? __resolveURL(url, globalThis.location && globalThis.location.href) : ((globalThis.location && globalThis.location.href) || '');
    globalThis.__curScript = { tagName:'SCRIPT', nodeName:'SCRIPT', src:curSrc, getAttribute:function(k){ return String(k).toLowerCase()==='src' ? this.src : null; } };
    try {
      if (url) __loadScriptByUrl(url);            // 远程/本地文件：__evalScript 全局 eval
      else if (typeof __evalCode==='function') __evalCode(__patchLibCode(code), label);   // 内联代码：__evalCode 全局 eval（顶层 var 挂全局）
      else throw new Error('__evalCode 未注入，无法执行内联脚本');
    } catch(e){ ok=false; err=e; __log('[game] script 执行失败 '+label+': '+e.message); }
    finally { globalThis.__curScript = prevCur; }
    // evalShellScript 锚点覆盖。将来若有 seal 后动态脚本装依赖,须实现带 src 匹配的
    // afterScript 锚点,不得用通用 installNow 猜——见 docs/native-boot-contract.md。)
    // PreloadJS 的 JS 顺序队列会等 script load/error 信号；native 下异步 timer 偶发拖慢，直接派发更稳定。
    if (ok){ if (typeof s.onload==='function') s.onload({type:'load',target:s}); (s._lst.load||[]).forEach(function(f){ try{f({type:'load',target:s});}catch(e){} }); }
    else  { if (typeof s.onerror==='function') s.onerror(err); (s._lst.error||[]).forEach(function(f){ try{f(err);}catch(e){} }); }
  }
  s._fire = function(){ s._appended = true; tryFire(); };            // 由 head/body.appendChild 拦截调用
  s.addEventListener = function(t,f){ (s._lst[t]=s._lst[t]||[]).push(f); };
  s.setAttribute = function(k,v){ s._attrs[k]=v; if(String(k).toLowerCase()==='src') tryFire(); };
  Object.defineProperty(s,'src', { get:function(){ return s._attrs.src||''; }, set:function(v){ s._attrs.src=v; tryFire(); }, configurable:true });
  return s;
}
// _gHead/_gBody.appendChild 拦截 script
[_gHead,_gBody].forEach(function(p){ var orig=p.appendChild; p.appendChild=function(c){ orig.call(p,c); if(c&&c.tag==='script'&&c._fire) c._fire(); return c; }; });

// 输入派发到所有 canvas + document/window 监听（native 鼠标 → __pointerEvent）
var __activeNativePointers=Object.create(null);
function __nativeDispatchPointerCancel(ev){
  var app=globalThis.__PIXI_APP__||globalThis.__pixiApp,system=app&&app.renderer&&app.renderer.events,boundary=system&&system.rootBoundary;
  if(!boundary)return;
  var tracking=boundary.mappingState.trackingData[ev.pointerId];if(!tracking)return;
  var targets=[];Object.keys(tracking.pressTargetsByButton).forEach(function(button){var target=boundary.findMountedTarget(tracking.pressTargetsByButton[button]);if(target&&targets.indexOf(target)<0)targets.push(target);});
  // Clear before notifying: a cancellation callback may synchronously release the pointer.
  delete boundary.mappingState.trackingData[ev.pointerId];
  var from=system.bootstrapEvent(system.rootPointerEvent,ev);
  for(var i=0;i<targets.length;i++){
    var event=boundary.createPointerEvent(from,'pointercancel',targets[i]);
    try{boundary.dispatchEvent(event,'pointercancel');if(event.pointerType==='touch')boundary.dispatchEvent(event,'touchcancel');}
    finally{boundary.freeEvent(event);}
  }
}
globalThis.__nativeCancelPointers=function(){
  var active=__activeNativePointers;__activeNativePointers=Object.create(null);
  Object.keys(active).forEach(function(id){var e=active[id];globalThis.__pointerEvent('pointercancel',e.clientX,e.clientY,e.button,0,e.pointerId,e.pointerType,e.isPrimary,0);});
};
globalThis.__pointerEvent = function(type, x, y, button, buttons, pointerId, pointerType, isPrimary, pressure){
  // AppKit can stop delivering a drag after the pointer crosses into another window.  A plain
  // pointerout only updates Pixi's hover path; FairyGUI keeps its stage-level touchDown and
  // ScrollPane pointerup listeners armed.  The native boundary-cancel event therefore has two
  // parts: a real pointerupoutside through Pixi (clears the pressed target), followed by a
  // non-clicking stage pointerup (releases GRoot/ScrollPane/GObject drag state).
  var boundaryCancel=type==='pointerboundarycancel';
  var dispatchType=boundaryCancel?'pointerup':type;
  // target 必须是 pixi EventSystem 的 domElement(=app.view 画布)：onPointerUp 注册在 globalThis 上，
  // 判 `target!==domElement ? 'outside'` → 若 target 为 null/非画布，pointerup 会被当成 pointerupoutside →
  // 不触发 click/tap。把 target 指向主 pixi 画布即可让画布内抬起正常派发 click。
  var cv = (globalThis.__PIXI_APP__ && globalThis.__PIXI_APP__.view) ||
           (globalThis.__pixiApp && globalThis.__pixiApp.view) ||
           (globalThis.__allCanvases && globalThis.__allCanvases[0]) || null;
  var nativeTarget=boundaryCancel?globalThis:cv;
  var ev = { type:dispatchType, clientX:x, clientY:y, pageX:x, pageY:y, offsetX:x, offsetY:y, screenX:x, screenY:y, movementX:0, movementY:0,
    pointerId:pointerId===undefined?1:Number(pointerId), pointerType:pointerType||'mouse', isPrimary:isPrimary===undefined?true:!!isPrimary, button:(button|0), buttons:(buttons|0), pressure:pressure===undefined?(buttons?0.5:0):Number(pressure), tangentialPressure:0,
    width:1, height:1, tiltX:0, tiltY:0, twist:0, altKey:false, ctrlKey:false, shiftKey:false, metaKey:false,
    isTrusted:true, cancelable:true, bubbles:true, composed:true, target:nativeTarget, srcElement:nativeTarget, currentTarget:null, relatedTarget:null,
    preventDefault:function(){}, stopPropagation:function(){}, stopImmediatePropagation:function(){}, composedPath:function(){ return nativeTarget?[nativeTarget]:[]; } };
  if(dispatchType==='pointerdown'||dispatchType==='pointermove'&&ev.buttons)__activeNativePointers[ev.pointerId]=ev;
  else if(dispatchType==='pointerup'||dispatchType==='pointercancel')delete __activeNativePointers[ev.pointerId];
  if(dispatchType==='pointercancel')__nativeDispatchPointerCancel(ev);
  var fns = (_docLst[dispatchType]||[]).slice();
  // A browser dispatches the pointer event only to the canvas under the pointer. Broadcasting it
  // to render-texture/offscreen canvases runs several Pixi EventSystems for one native event:
  // they can duplicate clicks and race the main canvas's pointer cursor back to `inherit`.
  var cs = boundaryCancel?[]:(cv?[cv]:(globalThis.__allCanvases||[])); for (var i=0;i<cs.length;i++){ var a=cs[i]._lst&&cs[i]._lst[dispatchType]; if(a) fns=fns.concat(a); }
  globalThis.__nativePointerType=boundaryCancel?'pointerboundarycancel':dispatchType;
  for (var j=0;j<fns.length;j++){ try{ fns[j](ev); }catch(e){ __log('[game] pointer 回调异常: '+e.message+' '+(e.stack||'')); } }
  if(boundaryCancel){
    var app=globalThis.__PIXI_APP__||globalThis.__pixiApp,stage=app&&app.stage;
    if(stage&&typeof stage.emit==='function'){
      var point={x:Number(x)||0,y:Number(y)||0};
      var release={type:'pointerup',pointerId:1,pointerType:'mouse',isPrimary:true,button:(button|0),buttons:(buttons|0),
        data:{global:point,button:(button|0),buttons:(buttons|0),pointerId:1,pointerType:'mouse'},global:point,
        target:stage,currentTarget:stage,nativeEvent:ev,originalEvent:ev,
        preventDefault:function(){},stopPropagation:function(){},stopImmediatePropagation:function(){}};
      try{stage.emit('pointerup',release);}catch(e){__log('[game] boundary pointerup 异常: '+e.message+' '+(e.stack||''));}
    }
    if(globalThis.__NATIVE_SCROLL_TRACE===1){
      var root=globalThis.fgui&&globalThis.fgui.GRoot,status=root&&root.globalMouseStatus;
      __log('[native-scrolltrace] boundary-release stage='+(stage?1:0)+' down='+(status&&status.touchDown?1:0));
    }
  }
  globalThis.__nativePointerType='';
};
// AppKit delivers trackpad and mouse-wheel input separately from pointer events. Feed the
// browser-compatible `wheel` listeners used by FairyGUI's DOMEventManager, and first refresh the
// pointer location so GRoot dispatches the wheel to the ScrollPane currently under the cursor.
globalThis.__wheelEvent = function(x, y, deltaX, deltaY, deltaMode){
  globalThis.__pointerEvent('pointermove',x,y,-1,0);
  var cv = (globalThis.__PIXI_APP__ && globalThis.__PIXI_APP__.view) ||
           (globalThis.__pixiApp && globalThis.__pixiApp.view) ||
           (globalThis.__allCanvases && globalThis.__allCanvases[0]) || null;
  var ev = {type:'wheel',clientX:x,clientY:y,pageX:x,pageY:y,offsetX:x,offsetY:y,screenX:x,screenY:y,
    deltaX:Number(deltaX)||0,deltaY:Number(deltaY)||0,deltaZ:0,deltaMode:deltaMode|0,
    wheelDelta:-(Number(deltaY)||0),wheelDeltaX:-(Number(deltaX)||0),wheelDeltaY:-(Number(deltaY)||0),
    altKey:false,ctrlKey:false,shiftKey:false,metaKey:false,isTrusted:true,cancelable:true,bubbles:true,
    target:cv,srcElement:cv,currentTarget:globalThis,defaultPrevented:false,
    preventDefault:function(){this.defaultPrevented=true;},stopPropagation:function(){},stopImmediatePropagation:function(){}};
  // FGUI chooses `wheel` only when `document.onwheel` exists; the lightweight native document
  // intentionally has no DOM event properties, so older bundles register the mousewheel aliases.
  var types=['wheel','mousewheel','DomMouseScroll','MozMousePixelScroll'];
  for(var ti=0;ti<types.length;ti++){
    var fns=(_docLst[types[ti]]||[]).slice();if(!fns.length)continue;ev.type=types[ti];
    for(var i=0;i<fns.length;i++){try{fns[i](ev);}catch(e){__log('[game] wheel 回调异常: '+e.message);}}
  }
};
// macOS native key events use the same window listener queue as browser key events.
// pixi-keyboard consumes which/keyCode and emits its own pressed/released signals.
globalThis.__keyboardEvent = function(type, key, code, keyCode, repeat, alt, ctrl, meta, shift){
  if (typeof globalThis.__kbIsEditing==='function' && globalThis.__kbIsEditing()) return;
  keyCode=keyCode|0;
  var ev={type:type, key:String(key==null?'':key), code:String(code==null?'':code),
    keyCode:keyCode, which:keyCode, charCode:0, repeat:!!repeat,
    altKey:!!alt, ctrlKey:!!ctrl, metaKey:!!meta, shiftKey:!!shift,
    isTrusted:true, cancelable:true, bubbles:true, target:document.activeElement||document.body,
    currentTarget:globalThis, defaultPrevented:false, cancelBubble:false,
    preventDefault:function(){this.defaultPrevented=true;},
    stopPropagation:function(){this.cancelBubble=true;},
    stopImmediatePropagation:function(){this.cancelBubble=true;this.__stopImmediate=true;}
  };
  var handler=globalThis['on'+type];
  if(typeof handler==='function'){
    try{handler.call(globalThis,ev);}catch(e){__log('[game] keyboard handler exception: '+e.message);}
  }
  var fns=(_docLst[type]||[]).slice();
  for(var i=0;i<fns.length;i++){
    try{fns[i].call(globalThis,ev);}catch(e){__log('[game] keyboard callback exception: '+e.message);}
    if(ev.__stopImmediate) break;
  }
};
