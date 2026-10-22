// ================= HTMLAudio 垫片(pixi-sound legacy 后端 → native AVAudioPlayer)=================
// 架构:不提供 window.AudioContext(pixi-sound 的探测是运行时 getter,给了会误走 WebAudio 全套)
//   → pixi-sound 自动 useLegacy(HTMLAudio);Audio 垫片每实例一个 AVAudioPlayer(cloneNode 共享字节)。
//   PixiSoundMgr 的 decode 预检(sound.context._ctx.decodeAudioData)由 __auTryHook 塞假实现,
//   返回 Audio 垫片元素(instanceof HTMLAudioElement → pixi-sound add 原生走 legacy source)。
(function(){
  var seq = 0, __playing = {}, __loading = {};
  var HAS_NATIVE = typeof globalThis.__auLoadUrl === 'function';
  function AudioEl(src){
    this.__id = 0; this.__lst = {};
    this._volume = 1; this._loop = false; this._rate = 1; this._paused = true; this._src = '';
    this.readyState = 0; this.duration = NaN; this.error = null;
    this.autoplay = false; this.muted = false; this.preload = 'auto';
    this.sampleRate = 44100; this.numberOfChannels = 2;   // 兼容"假 AudioBuffer"消费方读取
    this.style = {};
    if (src) this.src = src;
  }
  var P = AudioEl.prototype;
  P.HAVE_ENOUGH_DATA = 4;
  P.canPlayType = function(t){ t=String(t||'').toLowerCase();
    return (t.indexOf('mpeg')>=0||t.indexOf('mp3')>=0||t.indexOf('mp4')>=0||t.indexOf('aac')>=0||t.indexOf('wav')>=0||t.indexOf('m4a')>=0||t.indexOf('x-m4a')>=0)?'probably':''; };
  P.addEventListener = function(t, f){
    (this.__lst[t]||(this.__lst[t]=[])).push(f);
    // 已就绪后才注册的加载类监听:补发一次(库常"先设 src 后监听",不补则永远等不到)
    if ((t==='canplaythrough'||t==='load'||t==='loadedmetadata') && this.readyState===4){
      var s=this; setTimeout(function(){ try{ f.call(s,{type:t,target:s}); }catch(e){} },0); }
  };
  P.removeEventListener = function(t, f){ var a=this.__lst[t]; if(a){ var i=a.indexOf(f); if(i>=0)a.splice(i,1); } };
  P.__fire = function(t){ var self=this;
    setTimeout(function(){ var ev={type:t,target:self};
      var on=self['on'+t]; if(typeof on==='function'){ try{ on.call(self,ev); }catch(e){} }
      var a=self.__lst[t]; if(a) for(var i=0;i<a.length;i++){ try{ a[i].call(self,ev); }catch(e){} } },0); };
  P.__loadDone = function(dur){
    if (dur>=0){ this.duration=dur; this.readyState=4; this.__fire('loadedmetadata'); this.__fire('canplaythrough'); this.__fire('load'); }
    else { this.error={code:4}; this.__fire('error'); }
  };
  P.__loadData = function(buf, cb){    // 假 decodeAudioData 入口:直接喂字节(zip 内 mp3 等)
    var dur=-1;
    if (HAS_NATIVE){ try{ var id=++seq; dur=__auLoadData(id, buf); if(dur>=0) this.__id=id; }catch(e){} }
    this.__loadDone(dur); if(cb) cb(dur>=0);
  };
  // URL 音频首次从 LAN 拉取时必须走异步桥。同步 __auLoadUrl 会在 CADisplayLink/JSC
  // play() 可能早于加载完成：下面保留 _paused=false，__auDone 建好 player 后补播。
  Object.defineProperty(P, 'src', {
    get:function(){ return this._src; },
    set:function(v){ this._src=v||'';
      if(this.__loadToken){delete __loading[this.__loadToken];if(HAS_NATIVE&&this.__loadToken!==this.__id)__auCtl(this.__loadToken,6);this.__loadToken=0;}
      if(this.__id>0){delete __playing[this.__id];if(HAS_NATIVE)__auCtl(this.__id,6);this.__id=0;}
      this.readyState=0;this.duration=NaN;this.error=null;this.__ended=false;this.__pendingTime=0;
      if(!v){this._paused=true;if(this.__tu){clearInterval(this.__tu);this.__tu=0;}return;}
      var dur=-1;
      if (HAS_NATIVE){ try{
        var id=++seq; this.__loadToken=id;   // __auDone 用它辨别迟到的旧加载回调
        if (typeof v==='string' && v.indexOf('blob:pn/')===0 && __blobReg[v]) dur=__auLoadData(id, __blobReg[v]);
        else if(typeof globalThis.__auLoadUrlAsync==='function'){
          __loading[id]=this; __auLoadUrlAsync(id,String(v)); return;
        }else dur=__auLoadUrl(id, String(v));
        if (dur>=0) this.__id=id; }catch(e){} }
      this.__loadDone(dur);
    }});
  P.load = function(){};
  P._effVol = function(){ return this.muted ? 0 : this._volume; };
  P.play = function(){
    this._paused=false;this.__ended=false;
    if (this.__id>0){ __auCtl(this.__id,3,this._effVol()); if(this._rate!==1)__auCtl(this.__id,4,this._rate);
      __auCtl(this.__id,0,this._loop?1:0,0); __playing[this.__id]=this;
      var s=this; if(!this.__tu) this.__tu=setInterval(function(){ if(!s._paused) s.__fire('timeupdate'); },250); }   // sprite 依赖 timeupdate 停点
    this.__fire('play'); return Promise.resolve();
  };
  globalThis.__auDone = function(id,dur){
    var el=__loading[id]; if(!el){if(dur>=0)__auCtl(id,6);return;} delete __loading[id];
    if(el.__loadToken!==id){ if(dur>=0)try{__auCtl(id,6);}catch(e){} return; }   // 迟到回调:元素已换源,释放勿改写 __id
    el.__loadToken=0;
    if(dur>=0)el.__id=id;
    el.__loadDone(dur);
    // 加载期间写入的 currentTime(≙ 浏览器元数据就绪后才执行的挂起 seek)在开播前落到原生播放器;超出时长按 0。
    if(dur>=0&&el.__pendingTime>0){ if(el.__pendingTime<dur)__auCtl(id,5,el.__pendingTime); el.__pendingTime=0; }
    if(dur>=0&&!el._paused){
      __auCtl(id,3,el._effVol()); if(el._rate!==1)__auCtl(id,4,el._rate);
      __auCtl(id,0,el._loop?1:0,0); __playing[id]=el;
      if(!el.__tu)el.__tu=setInterval(function(){if(!el._paused)el.__fire('timeupdate');},250);
    }
  };
  P.pause = function(){ this._paused=true; if(this.__id>0){delete __playing[this.__id];__auCtl(this.__id,1);} if(this.__tu){ clearInterval(this.__tu); this.__tu=0; } this.__fire('pause'); };
  Object.defineProperty(P,'volume',{ get:function(){return this._volume;}, set:function(v){ this._volume=+v||0; if(this.__id>0)__auCtl(this.__id,3,this._effVol()); }});
  Object.defineProperty(P,'loop',{ get:function(){return this._loop;}, set:function(v){ this._loop=!!v; }});
  Object.defineProperty(P,'playbackRate',{ get:function(){return this._rate;}, set:function(v){ this._rate=+v||1; if(this.__id>0)__auCtl(this.__id,4,this._rate); }});
  // 原生播放器尚未就绪(URL 异步加载中)时 seek 先挂起,由 __auDone 在开播前执行(≙ HTMLMediaElement 的挂起 seek)。
  Object.defineProperty(P,'currentTime',{ get:function(){ return this.__id>0?__auCtl(this.__id,7):(this.__pendingTime||0); },
    set:function(v){ v=Math.max(0,+v||0); if(this.__id>0)__auCtl(this.__id,5,v); else this.__pendingTime=v; }});
  Object.defineProperty(P,'paused',{ get:function(){ return this._paused; }});
  Object.defineProperty(P,'ended',{ get:function(){ return this.__ended===true; }});
  P.cloneNode = function(){
    var c = new AudioEl();
    c._volume=this._volume; c._loop=this._loop; c._rate=this._rate; c._src=this._src;
    if (this.__id>0 && HAS_NATIVE){ try{ var id=++seq, dur=__auClone(id, this.__id);
      if (dur>=0){ c.__id=id; c.duration=dur; c.readyState=4; } }catch(e){} }
    else { c.duration=this.duration; c.readyState=this.readyState; }
    return c;
  };
  globalThis.__auEnded = function(id){ var el=__playing[id]; if(!el) return; delete __playing[id];
    el._paused=true; el.__ended=true; if(el.__tu){ clearInterval(el.__tu); el.__tu=0; } el.__fire('ended'); };
  globalThis.Audio = AudioEl;
  globalThis.HTMLAudioElement = AudioEl;
  // ===== Capacitor AppBridge 音频桥 =====
  // AppBridge.sendCommand(soundPlay/Stop/StopAll/FadeIn/FadeOut/Volume/Mute,alias 键控)。
  // 宿主实现直接复用 Audio 垫片(native AVAudioPlayer)。zip 内字节(无 url)仍走 pixi-sound legacy 兜底。
  globalThis.__NATIVE_AUDIO = 1;   // 配合 __patchLibCode 的 isAndroid 判定放宽
  (function(){
    var players = {}, lastVol = {}, fades = {}, clips = {}, muted = false;
    var ensure = function(alias, url){
      var p = players[alias];
      if (p && p.__srcUrl === url) return p;
      if (p){ p.pause(); p.src=''; }
      p = new AudioEl(); p.__srcUrl = url; p.src = url;
      players[alias] = p; return p;
    };
    var clearFade = function(a){ if(fades[a]){ clearInterval(fades[a]); delete fades[a]; } };
    var clearClip = function(a){ if(clips[a]){ clearTimeout(clips[a]); delete clips[a]; } };
    // 5.18s 文件的后半段)。Web Audio 以 source.start(0,start,end-start) 在 end 硬截止;原生播放器没有截止点,
    // 按原生实际播放位置轮询,到点与 soundStop 同样收尾。start>=end 时两者都不生效(≙ PixiSoundMgr.validatePlayOptions);
    // 循环音只取 start(≙ pixi-sound HTMLAudio 实例:end 只约束非循环播放)。
    var playClip = function(a, p, pl){
      var start = Math.max(0, +pl.start||0), end = +pl.end>0 ? +pl.end : 0, speed = +pl.speed>0 ? +pl.speed : 1;
      if (end && start>=end){ start = 0; end = 0; }
      clearClip(a);
      if (p.playbackRate !== speed) p.playbackRate = speed;
      try{ p.currentTime=start; }catch(e){}
      p.play();
      if (!end || p.loop) return;
      var check = function(){
        delete clips[a];
        if (players[a]!==p || p.paused || p.error) return;               // 已停/换源/加载失败
        if (p.readyState!==4){ clips[a] = setTimeout(check, 50); return; }  // URL 异步加载中,尚未开播
        var remain = (Math.min(end, p.duration) - p.currentTime) / (p.playbackRate||1);
        if (!(remain>0.004)){ try{ p.pause(); p.currentTime=0; }catch(e){} return; }
        clips[a] = setTimeout(check, Math.min(250, remain*1000));
      };
      clips[a] = setTimeout(check, 0);
    };
    var fadeTo = function(a, p, target, durSec, thenStop){
      clearFade(a);
      var steps = Math.max(1, Math.round((+durSec||0.5)*10)), i = 0, from = p.volume;
      fades[a] = setInterval(function(){
        i++; p.volume = muted ? 0 : (from + (target-from)*(i/steps));
        if (i>=steps){ clearFade(a); if(thenStop){ try{ p.pause(); p.currentTime=0; }catch(e){} } }
      }, 100);
    };
    var au = {
      soundPlay: function(pl){ var a=pl.alias, u=pl.url; if(!a||!u) return;
        var p = ensure(a,u); lastVol[a] = pl.volume!=null ? +pl.volume : 1;
        p.loop = !!pl.loop; p.volume = muted ? 0 : lastVol[a];
        clearFade(a); playClip(a, p, pl); },
      soundStop: function(pl){ var p=players[pl.alias]; clearFade(pl.alias); clearClip(pl.alias); if(p){ try{ p.pause(); p.currentTime=0; }catch(e){} } },
      soundStopAll: function(){ for(var a in players){ clearFade(a); clearClip(a); try{ players[a].pause(); players[a].currentTime=0; }catch(e){} } },
      soundFadeIn: function(pl){ var a=pl.alias, u=pl.url; if(!a||!u) return;
        var p = ensure(a,u); var vol = pl.volume!=null ? +pl.volume : 1; lastVol[a]=vol;
        p.loop = (pl.loop!==false); p.volume = 0;
        playClip(a, p, pl); fadeTo(a, p, vol, pl.duration, false); },
      soundFadeOut: function(pl){ var p=players[pl.alias]; if(p) fadeTo(pl.alias, p, 0, pl.duration, true); },
      soundVolume: function(pl){ var a=pl.alias, p=players[a]; if(p){ lastVol[a]=+pl.volume||0; if(!muted) p.volume=lastVol[a]; } },
      soundMute: function(pl){ muted=!!pl.mute; for(var a in players){ players[a].volume = muted?0:(lastVol[a]!=null?lastVol[a]:1); } }
    };
    globalThis.window = globalThis.window || globalThis;
    var W = globalThis.window;
    W.Capacitor = W.Capacitor || {};
    W.Capacitor.Plugins = W.Capacitor.Plugins || {};
    W.Capacitor.Plugins.AppBridge = {
      sendCommand: function(o){
        if (!o || typeof o.command !== 'string') return Promise.reject(new Error('command is required'));
        if (au[o.command]) { au[o.command](o.payload||{}); return Promise.resolve(); }
        if (typeof globalThis.__appBridgeSendCommand !== 'function') return Promise.reject(new Error('Native bridge unavailable'));
        globalThis.__appBridgeSendCommand(JSON.stringify(o));
        return Promise.resolve();
      }
    };
    globalThis.__appBridgeReceive = function(json){
      var detail=typeof json==='string'?JSON.parse(json):json;
      W.dispatchEvent(new W.CustomEvent('appBridgeMessage',{detail:detail}));
    };

  })();
  // pixi-sound 在 Shell.JS_LIB 静态链内(game/_app.js),loadJsLib 期间加载;脚本锚点驱动为主,
  // render loop 每帧探测保留为兜底,成功即停。
  var __auHooked = false;
  globalThis.__auTryHook = function(){
    if (__auHooked) return true;
    var ps = globalThis.PIXI && globalThis.PIXI.sound;
    if (!ps || !ps.context) return false;
    if (!ps.context._ctx) ps.context._ctx = { decodeAudioData: function(buf){
      return new Promise(function(res, rej){ var el=new AudioEl(); el.__loadData(buf, function(ok){ ok?res(el):rej(new Error('decode failed')); }); }); } };
    __auHooked = true;
    // seal 后才由每帧兜底装上=编排断裂(契约状态已定格 legacy 而音频对象此刻被修改):与
    // fgui 兜底同栏计数,使 installAfterSealCount=0 能完整证明"没有 seal 后安装"。
    if (globalThis.__nativeBoot && globalThis.__nativeBoot.isSealed()) {
      var auSealStats = globalThis.__nativeBootStats || (globalThis.__nativeBootStats = {});
      auSealStats.installAfterSealCount = (auSealStats.installAfterSealCount || 0) + 1;
      __log('[native-boot] 警告: audio-decode-shim 在 seal 后才由每帧兜底装上(编排断裂)');
    }
    __log('[audio] HTMLAudio 垫片已接入(pixi-sound legacy='+(ps.useLegacy!==false)+',后端=AVAudioPlayer,native='+HAS_NATIVE+')');
    return true;
  };
  // lib 域注册:pixi-sound 在 Shell.JS_LIB(game/_app.js)静态链内,seal 前必然加载,脚本 eval
  // 锚点同步驱动;render loop 的每帧短路探测保留为兜底(成功后每帧仅一次布尔判断)。契约模块
  // 尚未加载,入队列。
  (globalThis.__nativeBootQueue=globalThis.__nativeBootQueue||[]).push({id:'audio-decode-shim',install:function(){return globalThis.__auTryHook()?'installed':'pending';}});
})();

// Blob：JSZip 解压走 .async('blob')（需 Blob 支持探测通过 + new Blob([u8],{type}) 收字节）；
// .zspine/.zip 内的 png 经 Blob → createObjectURL(blob:pn/N，存 __bytes) → Image.src → __decodeImage 解码。
globalThis.Blob = globalThis.Blob || function(parts, opts){
  var chunks=[], total=0;
  (parts||[]).forEach(function(p){
    var u8;
    if (p instanceof ArrayBuffer) u8=new Uint8Array(p);
    else if (p && p.buffer instanceof ArrayBuffer) u8=new Uint8Array(p.buffer, p.byteOffset||0, p.byteLength);  // TypedArray/DataView
    else if (p && p.__bytes) u8=new Uint8Array(p.__bytes);                                                       // 我们的 Blob
    else if (typeof p==='string'){ var s=p; u8=new Uint8Array(s.length); for(var i=0;i<s.length;i++) u8[i]=s.charCodeAt(i)&0xff; }
    else u8=new Uint8Array(0);
    chunks.push(u8); total+=u8.length;
  });
  var all=new Uint8Array(total), off=0;
  chunks.forEach(function(c){ all.set(c, off); off+=c.length; });
  this.__bytes = all.buffer;                                  // ArrayBuffer
  this.size = total;
  this.type = (opts && opts.type) || '';
  var self=this;
  this.arrayBuffer = function(){ return Promise.resolve(self.__bytes); };
  this.text = function(){ return Promise.resolve(new TextDecoder().decode(new Uint8Array(self.__bytes))); };
  this.slice = function(s,e,t){ var b=new globalThis.Blob([self.__bytes.slice(s||0, e==null?self.size:e)], {type:t||''}); return b; };
};
