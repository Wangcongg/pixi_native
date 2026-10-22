// ===== XMLHttpRequest 垫片（浏览器 axios / 游戏网络走它）=====
// 异步委托 host：__xhrSend(reqId, method, absUrl, headersJson, body)  body: string|ArrayBuffer|ArrayBufferView|null
//   host 完成时回调 globalThis.__xhrDone(reqId, status, statusText, respHeadersJson, respBody)
//   respBody: ArrayBuffer（二进制/protobuf）或 string；网络失败 status=0。
//   node_runner 用 http/https 实现 __xhrSend；native(game_window.mm) 用 NSURLSessionDataTask。
function __abToStr(b){ if(b==null) return ''; if(typeof b==='string') return b;
  var u=(b instanceof ArrayBuffer)?new Uint8Array(b):(b.buffer?new Uint8Array(b.buffer,b.byteOffset,b.byteLength):new Uint8Array(b));
  return new globalThis.TextDecoder().decode(u); }
// Shell scripts only: retain the existing network/TextDecoder behaviour and source patching.
// Older hosts, explicit diagnostic opt-out and invalid UTF-8 all keep the original path.
function __scriptBytesToString(b){
  if(b==null||typeof b==='string') return __abToStr(b);
  if(globalThis.__NATIVE_SCRIPT_UTF8!==0&&typeof globalThis.__decodeScriptUTF8==='function'){
    var u=(b instanceof ArrayBuffer)?new Uint8Array(b):(b.buffer?new Uint8Array(b.buffer,b.byteOffset,b.byteLength):new Uint8Array(b));
    var text=globalThis.__decodeScriptUTF8(u.buffer,u.byteOffset,u.byteLength);
    if(typeof text==='string') return text;
  }
  return __abToStr(b);
}
function __strToAB(s){ var u=new globalThis.TextEncoder().encode(String(s)); return u.buffer.slice(u.byteOffset, u.byteOffset+u.byteLength); }
var __xhrReg = {}, __xhrSeq = 0;
globalThis.__xhrDone = function(reqId, status, statusText, respHeadersJson, respBody){
  var x = __xhrReg[reqId]; if(!x) return; delete __xhrReg[reqId];
  if (globalThis.__PROF === 1){   // 响应处理链同步耗时(__PROF=1 才计)
    var t0 = performance.now();
    x.__finish(status, statusText, respHeadersJson, respBody);
    if (performance.now() - t0 > 30) __log('[xhr-slow] 响应处理 '+(performance.now()-t0).toFixed(0)+'ms '+String(x._url||'').slice(-50));
  } else {
    x.__finish(status, statusText, respHeadersJson, respBody);
  }
};
globalThis.XMLHttpRequest = function(){
  var self=this;
  self.UNSENT=0; self.OPENED=1; self.HEADERS_RECEIVED=2; self.LOADING=3; self.DONE=4;
  self.readyState=0; self.status=0; self.statusText=''; self.response=null; self.responseText='';
  self.responseType=''; self.responseURL=''; self.timeout=0; self.withCredentials=false;
  self.onreadystatechange=null; self.onload=null; self.onerror=null; self.ontimeout=null; self.onabort=null; self.onloadend=null;
  self.upload={ addEventListener:function(){}, removeEventListener:function(){} };
  self._headers={}; self._respHeaders={}; self._method='GET'; self._url=''; self._lst={};
  self.addEventListener=function(t,f){ (self._lst[t]=self._lst[t]||[]).push(f); };
  self.removeEventListener=function(t,f){ var a=self._lst[t]; if(a){var i=a.indexOf(f); if(i>=0)a.splice(i,1);} };
  function emit(t){ var ev={type:t,target:self,currentTarget:self};
    if(typeof self['on'+t]==='function'){ try{self['on'+t](ev);}catch(e){__log('[xhr] on'+t+' 异常: '+e.message);} }
    (self._lst[t]||[]).forEach(function(f){ try{f(ev);}catch(e){__log('[xhr] '+t+' 监听异常: '+e.message);} }); }
  function setRS(s){ self.readyState=s; emit('readystatechange'); }
  self.open=function(m,u,async){ self._method=(m||'GET').toUpperCase(); self._url=__resolveURL(u, globalThis.location&&globalThis.location.href); self.responseURL=self._url; setRS(1); };
  self.setRequestHeader=function(k,v){ self._headers[k]=v; };
  self.getResponseHeader=function(k){ k=String(k).toLowerCase(); for(var h in self._respHeaders) if(h.toLowerCase()===k) return String(self._respHeaders[h]); return null; };
  self.getAllResponseHeaders=function(){ var s=''; for(var h in self._respHeaders) s+=h+': '+self._respHeaders[h]+'\r\n'; return s; };
  self.overrideMimeType=function(){};
  self.abort=function(){ if(self.__id!=null){ delete __xhrReg[self.__id]; if(typeof __xhrAbort==='function') try{__xhrAbort(self.__id);}catch(e){} } emit('abort'); emit('loadend'); };
  self.__finish=function(status, statusText, respHeadersJson, respBody){
    // [login-trace] 取证(2026-08-25 iOS deviceguest 登录 error=>null 排查):POST 一律打完成态,
    // 响应前 120 字节可判"服务器拒/协议错位/空响应"。定位后随 REQ 侧一起撤。
    if(globalThis.__LOGIN_TRACE===1&&self._method==='POST'){ var __bl=0; try{ __bl=(respBody&&(respBody.byteLength||respBody.length))||0; }catch(e){}
      var __head=''; try{ __head=String(__abToStr(respBody)||'').slice(0,120).replace(/[\r\n]+/g,'⏎'); }catch(e){}
      __log('[login-trace] RSP #'+self.__id+' status='+(status||0)+' bytes='+__bl+' head="'+__head+'"'); }
    self.status=status||0; self.statusText=statusText||'';
    try{ self._respHeaders = respHeadersJson ? (typeof respHeadersJson==='string'?JSON.parse(respHeadersJson):respHeadersJson) : {}; }catch(e){ self._respHeaders={}; }
    var rt=self.responseType||'';
    if(rt==='arraybuffer'){ self.response = (respBody instanceof ArrayBuffer)?respBody:(respBody&&respBody.buffer?respBody.buffer.slice(respBody.byteOffset,respBody.byteOffset+respBody.byteLength):__strToAB(respBody||'')); }
    else if(rt==='json'){ try{ self.response=JSON.parse(__abToStr(respBody)); }catch(e){ self.response=null; } }
    else { self.responseText=__abToStr(respBody); self.response=self.responseText; }
    setRS(2); setRS(3); setRS(4);
    if(self.status>0) emit('load'); else emit('error');
    emit('loadend');
  };
  self.send=function(body){
    var id=++__xhrSeq; self.__id=id; __xhrReg[id]=self;
    if (globalThis.__NATIVE_DAT_GUI===1 && /(?:^|\/)dat\.gui(?:\.min)?\.js(?:$|\?)/.test(self._url)) {
      __log('[native-gui] satisfy dat.gui request with native proxy');
      globalThis.setTimeout(function(){ globalThis.__xhrDone(id,200,'OK','{}','/* native dat.gui proxy */'); },0);
      return;
    }
    if (/\/libs\/v1\.0\/unity\//.test(self._url)) {
      __log('[xhr] 跳过可选脚本 '+self._url);
      globalThis.setTimeout(function(){ globalThis.__xhrDone(id,200,'OK','{}',''); },0);
      return;
    }
    // Real @pixi/filter-*.mjs packages are ESM. Satisfy the load queue with our UMD shims so
    // createjs/preloadjs marks them complete and game code can construct the same filter classes.
    if (/\/libs\/v1\.0\/pixijs\.v7\/filter-.*\.mjs(?:$|\?)/.test(self._url)) {
      try{
        if(typeof globalThis.__evalScript==='function') globalThis.__evalScript('host/native_filters.js');
        __log('[xhr] filter mjs → native shim '+self._url);
      }catch(e){ __log('[xhr] filter shim 失败 '+e.message); }
      globalThis.setTimeout(function(){ globalThis.__xhrDone(id,200,'OK','{}','/* native filter shim */'); },0);
      return;
    }
    if(globalThis.__LOGIN_TRACE===1&&self._method==='POST'){ var __bl2=0; try{ __bl2=(body&&(body.byteLength||body.length))||0; }catch(e){}
      __log('[login-trace] REQ #'+id+' POST '+String(self._url).slice(0,140)+' bodyLen='+__bl2); }
    if(typeof __xhrSend!=='function'){ __log('[xhr] __xhrSend 未注入，请求挂起: '+self._url); return; }
    try{ __xhrSend(id, self._method, self._url, JSON.stringify(self._headers), (body==null?null:body)); }
    catch(e){ __log('[xhr] send 异常: '+e.message); globalThis.__xhrDone(id,0,'',null,''); }
  };
};

// console 重定向到宿主 __log（JSC 原生 console 输出到系统日志，看不到；统一到 stderr 便于调试）
(function(){
  // Quiet routine boundaries by default; an explicit debug query or numeric override restores
  // the developer trace. The source command uses the same flag before copying its payload.
  if(globalThis.__NATIVE_ROUTINE_LOGS===undefined)
    globalThis.__NATIVE_ROUTINE_LOGS=/(?:^|[?&])debug=(?:1|true)(?:&|$)/.test(String(globalThis.__HOST_QUERY||''))?1:0;
  function j(v){ if (typeof v==='string') return v; if (v instanceof Error) return (v.message||'')+' | stack: '+(v.stack||''); try{ return JSON.stringify(v); }catch(e){ return String(v); } }
  function cat(a){ var s=''; for (var i=0;i<a.length;i++) s += (i?' ':'')+j(a[i]); return s; }
  // Keep slow-step/error lines and non-workflow logs. This bridge also handles older chunks
  // which have not yet adopted the source-side routine trace guard.
  function routineNoise(s){return globalThis.__NATIVE_ROUTINE_LOGS===0&&/^\[LOG\]:\s+#\d+\.\[/.test(s)&&s.indexOf('.elapsed =')<0;}
  function logArgs(a){var s=cat(a);if(!routineNoise(s))__log(s);}
  var c = { log:function(){logArgs(arguments);}, info:function(){logArgs(arguments);},
    warn:function(){__log('[warn] '+cat(arguments));}, error:function(){__log('[error] '+cat(arguments));},
    debug:function(){}, trace:function(){}, group:function(){}, groupCollapsed:function(){}, groupEnd:function(){},
    table:function(){}, dir:function(){logArgs(arguments);}, assert:function(){}, count:function(){}, time:function(){}, timeEnd:function(){} };
  globalThis.console = c;
})();

// 通用容错 DOM 垫片（按需：大部分 DOM 是分析/HTML-GUI，no-op 即可）
function makeEl(tag){
  var el = {
    tagName:(tag||'div').toUpperCase(), nodeName:(tag||'div').toUpperCase(), tag:tag, id:'', className:'', parentNode:null, parentElement:null, ownerDocument:globalThis.document,
    style:{ setProperty:function(){}, removeProperty:function(){}, getPropertyValue:function(){return '';}, cssText:'' },
    children:[], childNodes:[], dataset:{}, _attrs:{},
    classList:{ add:function(){}, remove:function(){}, toggle:function(){}, contains:function(){return false;} },
    appendChild:function(c){ this.children.push(c); if(c){ c.parentNode=this; c.parentElement=this; c.isConnected=true; } return c; },
    removeChild:function(c){ if(c){ c.parentNode=null; c.parentElement=null; c.isConnected=false; } return c; },
    insertBefore:function(c){ this.children.push(c); if(c){ c.parentNode=this; c.parentElement=this; c.isConnected=true; } return c; }, append:function(){}, prepend:function(){}, after:function(){}, before:function(){},
    setAttribute:function(k,v){ this._attrs[k]=v; }, getAttribute:function(k){ return (k in this._attrs)?this._attrs[k]:null; }, removeAttribute:function(){}, hasAttribute:function(k){return k in this._attrs;},
    addEventListener:function(){}, removeEventListener:function(){}, dispatchEvent:function(){return true;},
    getElementsByTagName:function(){ return []; }, getElementsByClassName:function(){ return []; }, querySelector:function(){ return null; }, querySelectorAll:function(){ return []; },
    getBoundingClientRect:function(){ var w=globalThis.innerWidth||1590, h=globalThis.innerHeight||960; return {x:0,y:0,top:0,left:0,right:w,bottom:h,width:w,height:h}; },
    clientWidth:(globalThis.innerWidth||1590), clientHeight:(globalThis.innerHeight||960), clientTop:0, clientLeft:0,
    offsetWidth:(globalThis.innerWidth||1590), offsetHeight:(globalThis.innerHeight||960), offsetTop:0, offsetLeft:0,
    scrollTop:0, scrollLeft:0, scrollWidth:(globalThis.innerWidth||1590), scrollHeight:(globalThis.innerHeight||960),
    getContext:function(){ return null; }, focus:function(){}, blur:function(){}, click:function(){}, remove:function(){}, scrollIntoView:function(){},
    contains:function(){ return false; }, cloneNode:function(){ return makeEl(tag); }, replaceChild:function(){}, normalize:function(){},
    set innerHTML(v){}, get innerHTML(){ return ''; }, set textContent(v){}, get textContent(){ return ''; }, set innerText(v){}, get innerText(){ return ''; },
    set onload(v){}, set onerror(v){}, set src(v){this._attrs.src=v;}, get src(){return this._attrs.src||'';}
  };
  return el;
}
var _elemCache = {};
function getEl(id){ return _elemCache[id] || (_elemCache[id] = (function(){ var e=makeEl('div'); e.id=id; return e; })()); }
var _gHead = makeEl('head'), _gBody = makeEl('body'), _gDocEl = makeEl('html');
Object.assign(globalThis.document, {
  getElementById:function(id){ return getEl(id); },
  getElementsByTagName:function(t){ t=(t||'').toLowerCase(); if(t==='head')return [_gHead]; if(t==='body')return [_gBody]; if(t==='html')return [_gDocEl]; var e=makeEl(t); e.parentNode=_gHead; return [e]; },
  getElementsByClassName:function(){return [];}, querySelector:function(sel){ return getEl('sel:'+sel); }, querySelectorAll:function(){return [];},
  createElement:function(tag){ tag=(tag||'').toLowerCase();
    if(tag==='canvas')return makeCanvas(300,150);
    if(tag==='img')return new globalThis.Image();
    if(tag==='a')return makeAnchor();    // axios/URL 解析依赖 anchor 的 protocol/host/pathname
    if(tag==='script')return _makeScriptEl();
    if(tag==='audio'||tag==='video')return globalThis.Audio ? new globalThis.Audio() : Object.assign(makeEl(tag),{canPlayType:function(){return '';},play:function(){return Promise.resolve();},pause:function(){},load:function(){}});
    if(tag==='input'||tag==='textarea')return globalThis.__mkInput ? globalThis.__mkInput(tag==='textarea') : makeEl(tag);
    return makeEl(tag); },
  createElementNS:function(ns,tag){ return makeEl(tag); },
  createTextNode:function(data){ var t=makeEl('#text'); t.nodeValue=(data==null?'':String(data)); t.data=t.nodeValue; return t; },  // 保留文本（createjs 把 JS 代码作文本节点塞进 <script>）
  createDocumentFragment:function(){ return makeEl('#fragment'); },
  head:_gHead, body:_gBody, documentElement:_gDocEl,
  cookie:'', readyState:'complete', visibilityState:'visible', hidden:false, title:'Pixi Native'
});
// document.currentScript：webpack publicPath 探测用（内联 eval 期间由 _makeScriptEl.tryFire 设 globalThis.__curScript）。
// 用 defineProperty 而非放进上面 Object.assign（assign 会把 getter 求值成静态值）。
Object.defineProperty(globalThis.document, 'currentScript', { get:function(){ return globalThis.__curScript || null; }, configurable:true });
