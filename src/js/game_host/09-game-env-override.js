// Generic timing, storage and blob URL helpers.
var __hostQuery="";
if (typeof globalThis.URLSearchParams === 'undefined') {
  globalThis.URLSearchParams = function(init){
    this._m = [];
    var s = (typeof init==='string') ? init : (init && init.toString ? init.toString() : '');
    if (s.charAt(0)==='?') s = s.slice(1);
    var self = this;
    if (s) s.split('&').forEach(function(pair){ if(!pair) return; var i=pair.indexOf('='); var k=i<0?pair:pair.slice(0,i); var v=i<0?'':pair.slice(i+1);
      try{ k=decodeURIComponent(k.replace(/\+/g,' ')); v=decodeURIComponent(v.replace(/\+/g,' ')); }catch(e){} self._m.push([k,v]); });
  };
  var USP = globalThis.URLSearchParams.prototype;
  USP.get     = function(k){ for(var i=0;i<this._m.length;i++) if(this._m[i][0]===k) return this._m[i][1]; return null; };
  USP.getAll  = function(k){ var r=[]; for(var i=0;i<this._m.length;i++) if(this._m[i][0]===k) r.push(this._m[i][1]); return r; };
  USP.has     = function(k){ for(var i=0;i<this._m.length;i++) if(this._m[i][0]===k) return true; return false; };
  USP.set     = function(k,v){ var d=false; for(var i=0;i<this._m.length;i++){ if(this._m[i][0]===k){ if(d){this._m.splice(i,1);i--;} else {this._m[i][1]=String(v);d=true;} } } if(!d) this._m.push([k,String(v)]); };
  USP.append  = function(k,v){ this._m.push([String(k),String(v)]); };
  USP['delete']=function(k){ for(var i=0;i<this._m.length;i++) if(this._m[i][0]===k){ this._m.splice(i,1); i--; } };
  USP.forEach = function(cb,th){ for(var i=0;i<this._m.length;i++) cb.call(th,this._m[i][1],this._m[i][0],this); };
  USP.toString= function(){ return this._m.map(function(p){ return encodeURIComponent(p[0])+'='+encodeURIComponent(p[1]); }).join('&'); };
}

// 尺寸/dpr：native(iOS)可注入 __screenW/__screenH/__screenDPR 让游戏按真实屏幕比例响应式铺满；macOS 无注入则用设计值。
globalThis.innerWidth = (typeof __screenW==='number' && __screenW>0) ? __screenW : 1590;
globalThis.innerHeight = (typeof __screenH==='number' && __screenH>0) ? __screenH : 960;
globalThis.devicePixelRatio = (typeof __screenDPR==='number' && __screenDPR>0) ? __screenDPR : 1;
// timer 桥（JSC 无原生 setTimeout/setInterval；必须在加载任何 bundle 之前定义——main.dialog.js 顶层即用）。
// node 有原生 timer → 用原生；native 走 host __setTimer/__clearTimer。
globalThis.setTimeout = globalThis.setTimeout || function(fn,ms){
  try{
    ms = ms || 0;
    if (ms <= 0) return __postHostTask(fn);
    return (typeof __setTimer==='function') ? __setTimer(fn, ms, false) : (fn(),0);
  }catch(e){ return 0; }
};
globalThis.clearTimeout = globalThis.clearTimeout || function(id){ if (id < 0) __cancelHostTask(id); else if (typeof __clearTimer==='function') __clearTimer(id); };
globalThis.setInterval = globalThis.setInterval || function(fn,ms){ return (typeof __setTimer==='function') ? __setTimer(fn, ms||0, true) : 0; };
globalThis.clearInterval = globalThis.clearInterval || function(id){ if (typeof __clearTimer==='function') __clearTimer(id); };
globalThis.queueMicrotask = globalThis.queueMicrotask || function(fn){ Promise.resolve().then(fn); };
(function(){
  var memory = {};
  var persistent = typeof globalThis.__persistentStorageGet === 'function'
    && typeof globalThis.__persistentStorageSet === 'function';
  globalThis.localStorage = {
    _d: memory,
    getItem: function(k){
      k = String(k);
      if (persistent) {
        var value = globalThis.__persistentStorageGet(k);
        return value === undefined || value === null ? null : String(value);
      }
      return (k in memory) ? memory[k] : null;
    },
    setItem: function(k,v){
      k = String(k); v = String(v);
      if (persistent) globalThis.__persistentStorageSet(k, v);
      else memory[k] = v;
    },
    removeItem: function(k){
      k = String(k);
      if (persistent && typeof globalThis.__persistentStorageRemove === 'function') globalThis.__persistentStorageRemove(k);
      else delete memory[k];
    },
    clear: function(){
      if (persistent && typeof globalThis.__persistentStorageClear === 'function') globalThis.__persistentStorageClear();
      else { memory = {}; this._d = memory; }
    },
    key: function(){ return null; },
    length: 0
  };
})();
globalThis.sessionStorage = { _d:{}, getItem:function(k){return (k in this._d)?this._d[k]:null;}, setItem:function(k,v){this._d[k]=''+v;}, removeItem:function(k){delete this._d[k];}, clear:function(){this._d={};}, key:function(){return null;}, length:0 };


var __blobReg={},__blobSeq=0;
globalThis.__SG_DIRTY=1;
globalThis.__PROF=0;
if (typeof globalThis.URL === 'undefined') globalThis.URL = function(){};
globalThis.URL.createObjectURL = function(blob){
  if (blob && blob.__srcURL) { if(globalThis.__BLOBDBG)__log('[blobdbg] createObjectURL→srcURL '+blob.__srcURL); return blob.__srcURL; } // 复用原始 URL
  if (blob && blob.__bytes){ var id='blob:pn/'+(++__blobSeq); __blobReg[id]=blob.__bytes; if(globalThis.__BLOBDBG)__log('[blobdbg] createObjectURL→'+id+' bytes='+(blob.__bytes.byteLength||0)); return id; }
  var id2='blob:pn/'+(++__blobSeq); __blobReg[id2]=blob; if(globalThis.__BLOBDBG)__log('[blobdbg] createObjectURL→'+id2+' (无__bytes，存对象 type='+(blob&&blob.type)+')'); return id2;
};
globalThis.URL.revokeObjectURL = function(u){ if(u && __blobReg[u]) delete __blobReg[u]; };
globalThis.XMLSerializer = function(){ this.serializeToString=function(){ return ''; }; };
