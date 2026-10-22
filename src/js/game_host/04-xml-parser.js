// ===== 纯 JS XML 解析（JSC 无 DOMParser）——pixi 位图字体 .fnt / fgui XML 配置需要 =====
// 产出带 getElementsByTagName/getAttribute/childNodes 的极简文档树。
// 全局 Node 常量 + array-like NodeList/NamedNodeMap（fgui 自带 XmlParser 用 nodeType/childNodes.item()/attributes.item()）。
globalThis.Node = globalThis.Node || { ELEMENT_NODE:1, TEXT_NODE:3, COMMENT_NODE:8, DOCUMENT_NODE:9 };
function __mkList(){ var a=[]; a.item=function(i){ return (i>=0&&i<this.length)?this[i]:null; }; return a; }
function __XmlNode(tag, nodeType){
  this.tagName=tag; this.nodeName=tag;
  this.nodeType=(nodeType==null?1:nodeType);            // 1元素 / 3文本 / 9文档
  this._attrMap={};                                      // 名→值，供 getAttribute（map 语义）
  this.attributes=__mkList();                            // NamedNodeMap-like：{nodeName,nodeValue}，供 fgui getNodeAttributes
  this.childNodes=__mkList();                            // 元素子节点（fgui 按 nodeType 过滤文本，缺文本节点无碍）
  this.children=[];                                      // 仅元素
  this.textContent=''; this.nodeValue=null; this.parentNode=null;
}
Object.defineProperty(__XmlNode.prototype,'firstChild',{ get:function(){ return this.childNodes.length?this.childNodes[0]:null; }, configurable:true });
Object.defineProperty(__XmlNode.prototype,'lastChild',{ get:function(){ return this.childNodes.length?this.childNodes[this.childNodes.length-1]:null; }, configurable:true });
__XmlNode.prototype._setAttr=function(k,v){ k=String(k); v=String(v);
  if(!(k in this._attrMap)){ this.attributes.push({ nodeName:k, nodeValue:v, name:k, value:v }); }
  else { for(var i=0;i<this.attributes.length;i++){ if(this.attributes[i].nodeName===k){ this.attributes[i].nodeValue=v; this.attributes[i].value=v; } } }
  this._attrMap[k]=v; };
__XmlNode.prototype.getAttribute=function(k){ return (k in this._attrMap)?this._attrMap[k]:null; };
__XmlNode.prototype.hasAttribute=function(k){ return k in this._attrMap; };
__XmlNode.prototype.setAttribute=function(k,v){ this._setAttr(k,v); };
__XmlNode.prototype.getElementsByTagName=function(tag){ var out=[]; tag=String(tag); (function rec(n){ for(var i=0;i<n.children.length;i++){ var c=n.children[i]; if(tag==='*'||c.tagName===tag) out.push(c); rec(c); } })(this); return out; };
__XmlNode.prototype.querySelector=function(sel){ var r=this.getElementsByTagName(sel); return r[0]||null; };
__XmlNode.prototype.querySelectorAll=function(sel){ return this.getElementsByTagName(sel); };
function __xmlUnescape(s){
  return String(s)
    .replace(/&#x([0-9a-fA-F]+);/g,function(_,h){return String.fromCodePoint(parseInt(h,16));})
    .replace(/&#([0-9]+);/g,function(_,d){return String.fromCodePoint(parseInt(d,10));})
    .replace(/&lt;/g,'<').replace(/&gt;/g,'>').replace(/&quot;/g,'"')
    .replace(/&apos;/g,"'").replace(/&amp;/g,'&');
}
function __parseXMLString(str){
  str = String(str==null?'':str).replace(/<\?[\s\S]*?\?>/g,'').replace(/<!--[\s\S]*?-->/g,'').replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g,'$1');
  var doc = new __XmlNode('#document', 9); var stack=[doc];
  var tagRe = /<(\/?)([\w:.\-]+)((?:\s+[\w:.\-]+\s*=\s*(?:"[^"]*"|'[^']*'))*)\s*(\/?)>/g, m, last=0;
  while((m=tagRe.exec(str))){
    var text = str.slice(last, m.index);
    if(text.trim() && stack.length) stack[stack.length-1].textContent += __xmlUnescape(text);
    last = tagRe.lastIndex;
    var closing=m[1], tag=m[2], attrs=m[3], selfClose=m[4];
    if(closing){ if(stack.length>1) stack.pop(); continue; }
    var node=new __XmlNode(tag);
    var aRe=/([\w:.\-]+)\s*=\s*(?:"([^"]*)"|'([^']*)')/g, a;
    while((a=aRe.exec(attrs))) node._setAttr(a[1], __xmlUnescape(a[2]!==undefined?a[2]:a[3]));
    var parent=stack[stack.length-1]; parent.childNodes.push(node); parent.children.push(node); node.parentNode=parent;
    if(!selfClose) stack.push(node);
  }
  doc.documentElement = doc.children[0]||null;
  return doc;
}
globalThis.DOMParser = function(){ this.parseFromString=function(s){ return __parseXMLString(s); }; };

var __fguiPreTk = 0, __fguiPrePending = {};
var __spinePreTk = 0, __spinePrePending = {};
function __fguiResKey(url){
  var raw=String(url||''),clean=raw.split('#')[0].split('?')[0],lower=clean.toLowerCase();
  // Production still disguises the same raw-deflate package as .jpg?as=buffer.
  // A plain .jpg is an image and must never enter this path.
  var legacy=lower.slice(-4)==='.jpg'&&/(?:\?|&)as=buffer(?:&|$)/i.test(raw);
  if(lower.slice(-6)!=='.fgui'&&!legacy)return '';
  var file=clean.slice(clean.lastIndexOf('/')+1);return file.split('.')[0]||'';
}
function __spineResKey(url){
  var clean=String(url||'').split('#')[0].split('?')[0],lower=clean.toLowerCase();
  if(lower.slice(-7)!=='.zspine'&&lower.slice(-6)!=='.spine')return '';
  var file=clean.slice(clean.lastIndexOf('/')+1),dot=file.lastIndexOf('.');
  return dot>0?file.slice(0,dot):file;
}

var adapter = {
  createCanvas:function(w,h){ return makeCanvas(w,h); },
  getCanvasRenderingContext2D:function(){ return function CanvasRenderingContext2D(){}; },
  getWebGLRenderingContext:function(){ return WebGLRCStub; },
  getNavigator:function(){ return { userAgent:(globalThis.__NAV_UA||'pixi-native'), gpu:null }; },
  getBaseUrl:function(){ return ''; },
  getFontFaceSet:function(){ return null; },
  // S7: fetch 走 host 文件读取。返回最小 Response(json/text/arrayBuffer)。
  // native 有 __readFileAsync 时 LAN 拉取在后台线程。.fgui 的 arrayBuffer 在 resolve 给
  // Pixi loader 前等待 Native raw-deflate/条目/XML 预解析；.zspine/.spine 同样先在
  // Native 后台准备 ZIP manifest、atlas 文本和 skeleton JSON。
  fetch:function(url, opts){
    url = String(url);
    function mkResp(data){
      function txt(){ return new TextDecoder().decode(new Uint8Array(data)); }
      var fguiKey=__fguiResKey(url),fguiReady=null;
      var spineKey=__spineResKey(url),spineReady=null;
      // zspine 逐帧放行闸门:弹窗一次加载 10-18 个 spine,arrayBuffer 几乎同帧 resolve,下游
      // readSkeletonData+Spine 实例创建的微任务链全挤进同一帧(实测 279ms 冻结帧)。每帧最多
      // 放行 budget 个,冻结摊成数个 30ms 级帧;总时长不变(本就在 ready2show 等待期内)。
      var SP=globalThis.__spinePace||(globalThis.__spinePace={queue:[],used:0,budget:2,queuedTotal:0});
      if(!globalThis.__spinePaceTick)globalThis.__spinePaceTick=function(){
        var sp=globalThis.__spinePace;sp.used=0;
        var n=Math.min(sp.queue.length,sp.budget);
        for(var i=0;i<n;i++){var f=sp.queue.shift();sp.used++;try{f();}catch(e){}}
      };
      var spinePaceRelease=function(fn){
        if(SP.used<SP.budget){SP.used++;try{fn();}catch(e){}}
        else{SP.queue.push(fn);SP.queuedTotal++;if(SP.queuedTotal%8===1&&typeof __log==='function'&&globalThis.__NATIVE_RUNTIME_DIAG===1)__log('[spine-pace] 摊帧排队中 queue='+SP.queue.length);}
      };
      function arrayBuffer(){
        if(fguiKey&&typeof globalThis.__nativeFguiPreparse==='function'){
          if(!fguiReady)fguiReady=new Promise(function(resolve){
            var tk=++__fguiPreTk,done=false;
            __fguiPrePending[tk]=function(ok,inflateMs,xmlMs,entries){if(done)return;done=true;var stat=globalThis.__nativeFguiPreparseClientStats||(globalThis.__nativeFguiPreparseClientStats={requests:0,ready:0,failed:0,entries:0,inflateMs:0,xmlMs:0});stat.ready++;if(!ok)stat.failed++;stat.entries+=entries|0;stat.inflateMs+=Number(inflateMs)||0;stat.xmlMs+=Number(xmlMs)||0;resolve(data);};
            var client=globalThis.__nativeFguiPreparseClientStats||(globalThis.__nativeFguiPreparseClientStats={requests:0,ready:0,failed:0,entries:0,inflateMs:0,xmlMs:0});client.requests++;
            try{if(__nativeFguiPreparse(tk,fguiKey,data)!==false){
              // watchdog:native 异常路径漏回调时 arrayBuffer Promise 会永挂(loading 卡死无日志),超时按未加速回退
              setTimeout(function(){var lost=__fguiPrePending[tk];if(lost){delete __fguiPrePending[tk];if(typeof __log==='function')__log('[fgui-preparse] watchdog: '+fguiKey+' native 未回调,回退');lost(false,0,0,0);}},10000);
              return;
            }}catch(e){if(globalThis.__PROF===1&&typeof __log==='function')__log('[fgui-preparse] '+fguiKey+' fallback: '+e.message);}
            var cb=__fguiPrePending[tk];delete __fguiPrePending[tk];if(cb)cb(false,0,0,0);
          });
          return fguiReady;
        }
        if(spineKey&&typeof globalThis.__nativeSpinePreparse==='function'){
          if(!spineReady)spineReady=new Promise(function(resolve0){
            var resolve=function(d){spinePaceRelease(function(){resolve0(d);});};
            var tk=++__spinePreTk,done=false;
            __spinePrePending[tk]=function(ok,handle,zipMs,jsonMs,entries,images){
              if(done)return;done=true;
              var stat=globalThis.__nativeSpinePreparseClientStats||(globalThis.__nativeSpinePreparseClientStats={requests:0,ready:0,failed:0,entries:0,images:0,zipMs:0,jsonMs:0});
              stat.ready++;if(!ok)stat.failed++;stat.entries+=entries|0;stat.images+=images|0;stat.zipMs+=Number(zipMs)||0;stat.jsonMs+=Number(jsonMs)||0;
              if(ok&&handle>0){try{data.__nativeSpineHandle=handle|0;data.__nativeSpineUrl=url;data.__nativeSpineKey=spineKey;}catch(e){}}
              resolve(data);
            };
            var client=globalThis.__nativeSpinePreparseClientStats||(globalThis.__nativeSpinePreparseClientStats={requests:0,ready:0,failed:0,entries:0,images:0,zipMs:0,jsonMs:0});client.requests++;
            // wantsLease:只有声明将读 prepared holder 的消费者(__nativeSpineLoadPrepared 在
            // arrayBuffer 前标记 response)才让 native 预获取在途引用。legacy/fallback 链只用
            // 原始压缩字节不读 holder,发 lease 给它必然泄漏(记账无人消费,实测 pinned=1 滞留)。
            try{if(__nativeSpinePreparse(tk,url,spineKey,data,resp&&resp.__spineWantsLease?1:0)!==false){
              setTimeout(function(){var lost=__spinePrePending[tk];if(lost){delete __spinePrePending[tk];if(typeof __log==='function')__log('[spine-preparse] watchdog: '+spineKey+' native 未回调,回退');lost(false,0,0,0,0,0);}},10000);
              return;
            }}catch(e){if(globalThis.__PROF===1&&typeof __log==='function')__log('[spine-preparse] '+spineKey+' fallback: '+e.message);}
            var cb=__spinePrePending[tk];delete __spinePrePending[tk];if(cb)cb(false,0,0,0,0,0);
          });
          return spineReady;
        }
        return Promise.resolve(data);
      }
      var resp={
        ok:true, status:200, url:url,
        arrayBuffer:arrayBuffer,
        json:function(){ return Promise.resolve(JSON.parse(txt())); },
        text:function(){ return Promise.resolve(txt()); },
        // blob 携带原始 URL + 字节：pixi 图片加载走 fetch→blob→createObjectURL→Image，我们的 createObjectURL 直接复用原始 URL，避免 blob 往返。
        blob:function(){ return Promise.resolve({ size:data.byteLength, type:'', __srcURL:url, __bytes:data, arrayBuffer:function(){ return Promise.resolve(data); }, text:function(){ return Promise.resolve(txt()); } }); }
      };
      return resp;
    }
    function mk404(){ return { ok:false, status:404, url:url, json:function(){return Promise.reject(new Error('404'));}, text:function(){return Promise.reject(new Error('404'));}, arrayBuffer:function(){return Promise.reject(new Error('404'));} }; }
    if (typeof __readFileAsync === 'function'){
      // Transport completion and JS delivery are deliberately separate. __fileDone may keep the
      // response in __ioQ for a later frame, but the native URLSession slot is already free and the
      // fetch scheduler should immediately start the next request instead of leaving the CDN idle.
      var transportResolve;
      var transportDone=new Promise(function(resolve){transportResolve=resolve;});
      var request=new Promise(function(resolve,reject){
        var tk = ++__fileTk;
        var pending=function(data){resolve(data ? mkResp(data) : mk404());};
        pending.__transportDone=transportResolve;
        __filePending[tk]=pending;
        try{__readFileAsync(url, tk);}
        catch(error){delete __filePending[tk];transportResolve();reject(error);}
      });
      request.__nativeTransportDone=transportDone;
      return request;
    }
    var r = (typeof __readFile === 'function') ? __readFile(url) : null;  // {ok,data:ArrayBuffer} or null
    return Promise.resolve((r && r.ok) ? mkResp(r.data) : mk404());
  },
  parseXML:function(xml){ return __parseXMLString(xml); }
};
