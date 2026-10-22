// ===== 全局环境垫片 =====
globalThis.self = globalThis;
globalThis.window = globalThis;
// window.open:业务外链(设置菜单 FAN PAGE、条款/隐私政策等)在浏览器开新标签页;native 转系统
// 默认浏览器(mac __nativeOpenURL→NSWorkspace,仅 http/https)。返回 null(=浏览器弹窗被拦语义,
// 业务不读返回句柄);无桥平台静默降级,点击不再 TypeError 中断 handler。
globalThis.open = function(url){
  try{ if(url && typeof __nativeOpenURL==='function') __nativeOpenURL(String(url)); }
  catch(e){ if(typeof __log==='function')__log('[open-url] 异常 '+(e&&e.message)); }
  return null;
};
// UA：iOS(native 注入 __isMobile)用 iPhone UA → PIXI.isMobile.any=true → fgui 走移动端触摸分支(滚动起点用事件真实坐标，
// 否则桌面分支取 globalMouseStatus，触摸无 hover 预热 → 起点错 → 滚动松手回弹)。macOS 无注入则保持桌面 UA。
globalThis.__NAV_UA = (typeof __isMobile!=='undefined' && __isMobile)
  ? 'Mozilla/5.0 (iPhone; CPU iPhone OS 16_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/15E148'
  : 'pixi-native';
globalThis.navigator = { userAgent:globalThis.__NAV_UA, platform:'native', hardwareConcurrency:8 };
(function(){
  var perf = globalThis.performance || {};
  // performance.now() 必须返回“相对时间”，与 host 喂给 __tick 的 tMs 同基准（值都从启动起算、都很小）。
  // 否则 pixi Ticker 的 maxFPS 节流里 delta=currentTime(tMs相对)-_lastFrame(perf.now绝对) 恒为大负数 →
  // 永远命中 `if(delta<_minElapsedMS)return` 早返回 → ticker.lastTime 冻结、render listener 不再 emit → 画面永久定格在最后一帧(beach)。
  // 必须在 game_host 加载最早期锚定 __perfBase：app/ticker 可能在首帧 __tick 之前、于 XHR 回调里创建并 start，
  // 那时 perf.now() 若返回绝对值，_lastFrame 会被污染成绝对值且因 _minElapsedMS=0 永不更新 → 之后设 maxFPS 即冻结。
  if (globalThis.__perfBase === undefined && typeof __nowUs === 'function') globalThis.__perfBase = __nowUs()/1000;
  if (typeof perf.now !== 'function') perf.now = function(){ return __nowUs()/1000 - (globalThis.__perfBase||0); };
  if (typeof perf.setResourceTimingBufferSize !== 'function') perf.setResourceTimingBufferSize = function(){};
  if (typeof perf.clearResourceTimings !== 'function') perf.clearResourceTimings = function(){};
  if (typeof perf.getEntries !== 'function') perf.getEntries = function(){ return []; };
  if (typeof perf.getEntriesByType !== 'function') perf.getEntriesByType = function(){ return []; };
  if (typeof perf.getEntriesByName !== 'function') perf.getEntriesByName = function(){ return []; };
  if (typeof perf.mark !== 'function') perf.mark = function(){};
  if (typeof perf.measure !== 'function') perf.measure = function(){};
  if (typeof perf.clearMarks !== 'function') perf.clearMarks = function(){};
  if (typeof perf.clearMeasures !== 'function') perf.clearMeasures = function(){};
  if (typeof perf.timeOrigin !== 'number') { try { perf.timeOrigin = 0; } catch(e){} }
  globalThis.performance = perf;
})();
globalThis.WebGLRenderingContext = WebGLRCStub;
globalThis.WebGL2RenderingContext = WebGLRCStub;
// raf：暂存回调，由 host 每帧（native=vsync drawOneFrame / node=定时器）调 globalThis.__tick(tMs) 驱动 pixi ticker。
var _rafCbs = [], _rafSpare = [], _rafId = 0, _rafDispatchDepth = 0;
// NATIVE_RAF_CALLBACK_PROFILER_BEGIN
// 逐 RAF callback 归因只在 spin 深度探针轮开启。正式路径不创建 WeakMap、不取时、
// 不读 Function#toString；探针路径也先结束回调计时，再只给 top3 生成标签，避免标签成本
// 被算进某个业务回调。快照携带将在帧尾公布的帧号，__badFrame 只消费同帧样本。
var __rafProbeLabelCache;
function __rafProbeCallbackLabel(cb){
  var cache=__rafProbeLabelCache;
  if(cache===undefined){
    try{cache=__rafProbeLabelCache=(typeof WeakMap==='function'?new WeakMap():null);}catch(e){cache=__rafProbeLabelCache=null;}
  }
  if(cache&&cb&&(typeof cb==='function'||typeof cb==='object')){
    try{var cached=cache.get(cb);if(cached)return cached;}catch(e){}
  }
  var label='';
  try{label=String(cb&&cb.name||'');}catch(e){}
  if(!label||label==='anonymous'){
    try{label=Function.prototype.toString.call(cb).replace(/[\r\n{},|]+/g,' ').replace(/\s+/g,' ').trim().slice(0,64);}catch(e){}
  }
  label=String(label||'anonymous').replace(/[\r\n{},|]+/g,' ').replace(/\s+/g,' ').trim().slice(0,64)||'anonymous';
  if(cache&&cb&&(typeof cb==='function'||typeof cb==='object'))try{cache.set(cb,label);}catch(e){}
  return label;
}
function __rafProbeKeepTop(top,cb,ms){
  if(typeof ms!=='number'||ms<0||ms!==ms)return;
  var at=top.length;
  while(at>0&&top[at-1].ms<ms)at--;
  top.splice(at,0,{cb:cb,ms:ms});
  if(top.length>3)top.length=3;
}
function __rafProbePublish(top,frame){
  var items=[];
  for(var i=0;i<top.length;i++)items.push({label:__rafProbeCallbackLabel(top[i].cb),ms:top[i].ms});
  globalThis.__rafCallbackTop={frame:frame|0,items:items};
}
function __rafProbeFormat(frame){
  if(globalThis.__NATIVE_SPIN_PROF!==1)return '';
  var snap=globalThis.__rafCallbackTop;
  if(!snap||snap.frame!==(frame|0)||!snap.items||!snap.items.length)return '';
  var out=[];
  for(var i=0;i<snap.items.length;i++)out.push(snap.items[i].label+':'+snap.items[i].ms.toFixed(1));
  return out.join(',');
}
// NATIVE_RAF_CALLBACK_PROFILER_END
// native 的 NSTimer 对 0ms 任务偶发滞后；PreloadJS/script onload 这类“下一拍”任务用 JS 队列随 __tick 稳定推进。
var _hostTaskQ = [], _hostTaskId = 0;
function __reportHostCallbackError(type,error){
  __log('[host-error] '+type+': '+String(error&&(error.stack||error.message)||error));
}
function __postHostTask(fn){
  var id = -(++_hostTaskId);
  _hostTaskQ.push({ id:id, fn:fn, canceled:false });
  return id;
}
function __cancelHostTask(id){
  for (var i=0;i<_hostTaskQ.length;i++) if (_hostTaskQ[i].id === id) { _hostTaskQ[i].canceled = true; break; }
}
function __flushHostTasks(){
  globalThis.__slowTaskMs = 0; globalThis.__slowTaskTag = '';   // 每帧重置(badframe 读本帧最慢的那个)
  if (!_hostTaskQ.length) return;
  var q = _hostTaskQ; _hostTaskQ = [];
  for (var i=0;i<q.length;i++){
    if (!q[i].canceled && typeof q[i].fn === 'function') {
      // 始终计时:两次 performance.now 的开销远小于"尖峰无从归因"的代价。只有超过阈值才去取
      // 函数体做标签(toString 才是贵的那一步),所以正常帧上这里只多两次取时。
      var __t0 = performance.now();
      try { q[i].fn(); } catch(e){ __log('[task] 回调异常: '+e.message); __reportHostCallbackError('task_exception', e); }
      var __td = performance.now() - __t0;
      if (__td > 8 && __td > globalThis.__slowTaskMs){
        globalThis.__slowTaskMs = __td;
        var __tag = '';
        try { __tag = (q[i].fn.name||'anon') + ' ' + String(q[i].fn).replace(/\s+/g,' ').slice(0,90); } catch(e){ __tag='?'; }
        globalThis.__slowTaskTag = __td.toFixed(0)+'ms '+__tag;
      }
      if (globalThis.__PROF === 1 && __td > 30) __log('[t-slow] timer 回调 '+__td.toFixed(0)+'ms');
    }
  }
}
// Spine 数据所有权模型：skeletonCache 只是"衍生缓存"，真数据在仍被持有的源里——spinex 门面
// 路径的源是 native preparse/build 层（引用计数，native_spinex.js reviveSkeletonStub 负责按需
// 重建）；legacy 路径的源是 Assets.cache 里 bundle 仍持有的 zspine 解析产物（此处负责）。
// 业务对不属于自己的共享 spine 调 removeSpineData（加载者是别的 bundle）时不再拦截删除——
// 删的只是衍生条目；create() 在需要时从源通用重建，任何 key 一视同仁。两处源都没有 = 数据
// 真的没了，维持与浏览器一致的 miss 语义（不识别任何资源名，也不猜调用者意图）。

globalThis.requestAnimationFrame = function(cb){ _rafCbs.push({ id:++_rafId, cb:cb }); return _rafId; };
globalThis.cancelAnimationFrame = function(id){ for (var i=0;i<_rafCbs.length;i++) if (_rafCbs[i].id===id){ _rafCbs.splice(i,1); break; } };
globalThis.__tick = function(tMs){
  // Native screen rendering runs after this JS tick has flushed its Pixi command stream. An
  // offscreen Renderer.render() can therefore be the last JS render of a tick even though native
  // rendering changes the real GL state afterwards. Advance an epoch at the tick boundary so the
  // first Pixi offscreen render of the next tick always invalidates its cached GL bindings.
  globalThis.__nativeGlEpoch = (globalThis.__nativeGlEpoch||0) + 1;
  if (globalThis.__spinePaceTick) __spinePaceTick();   // zspine 摊帧闸门:重置本帧预算+放行排队

  // 首帧锚定 performance.now() 基准：__perfBase = __nowUs()/1000 - tMs ≈ gStartUs/1000，
  // 此后 perf.now()=(nowUs-gStartUs)/1000，与 tMs 完全同基准（修复 pixi Ticker 冻结，见 perf.now 处注释）。
  if (globalThis.__perfBase === undefined && typeof __nowUs === 'function') globalThis.__perfBase = __nowUs()/1000 - tMs;

  // 一次性清理:历史误收编进 __sgOv 的文本对象(改 __ovChk 前注册的存量;新的已被拦截)
  if (!globalThis.__ovCleaned && globalThis.__sgOv && globalThis.PIXI && globalThis.PIXI.Text && globalThis.__sgOv.size > 0){
    globalThis.__ovCleaned = 1;
    var __rmN = 0;
    globalThis.__sgOv.forEach(function(o){ if (o instanceof globalThis.PIXI.Text){ globalThis.__sgOv.delete(o); __rmN++; } });
    if (__rmN) __log('[ov] 清理误收编文本 '+__rmN+' 个(剩 '+globalThis.__sgOv.size+' 个真驱动对象)');
  }
  // 性能计数器每帧清零(badframe 快照读取;filter/mask/rt 由 pixi hook 计,text 由 c2d fillText 计)
  var __pc = globalThis.__pfCnt || (globalThis.__pfCnt = {});
  __pc.filter = 0; __pc.mask = 0; __pc.rt = 0; __pc.text = 0; __pc.gfx = 0;
  // 脏剪枝对拍模式(__SG_DIRTY=2):静止帧也带上对拍标记(count 小数 0.5),漏标节点由 [sg-verify] 点名
  if (globalThis.__SG_DIRTY === 2 && globalThis.__sg){
    var __sgbV = globalThis.__SGB;
    if (__sgbV){ var __dc = Math.floor(__sgbV[32768*48]); if (__dc <= 4096) __sgbV[32768*48] = __dc + 0.5; }
  }
  // __tick 里 timer 回调与 IO 消费此前不在任何 __tk.* 分段内(计时从 raf 才开始),于是 badframe 会
  // 出现"JS 2600ms 但 raf/fgui/st 全是 0"的盲区——耗时明明在 tick 内却无从归因。这里补两段计时。
  var __tkPre = globalThis.__tkProf || (globalThis.__tkProf = {});
  __tkPre.__bodyT0 = performance.now();   // __tick 函数体起点(与 body 终点配对)
  var __tkP0 = __tkPre.__bodyT0;
  if(globalThis.__nativeFguiTransitionVisibilityFrame)try{globalThis.__nativeFguiTransitionVisibilityFrame();}catch(e){if(!globalThis.__nativeFguiTransitionVisibilityErr){globalThis.__nativeFguiTransitionVisibilityErr=1;__log('[native-fgui] transition visibility '+e.message);}}
  __flushHostTasks();
  __tkPre.task = performance.now() - __tkP0; __tkP0 = performance.now();
  if (globalThis.__ioDrain) globalThis.__ioDrain();   // IO 回调分帧消费(每帧预算 6ms,loading 期帧率保护)
  __tkPre.io = performance.now() - __tkP0;
  if(globalThis.__NATIVE_RESOURCE_DIAG===1&&typeof globalThis.__nativeResourceStats==='function'){
    globalThis.__nativeResourceDiagFrame=(globalThis.__nativeResourceDiagFrame||0)+1;
    if(globalThis.__nativeResourceDiagFrame%300===0){
      try{var __nrs=__nativeResourceStats();__nrs.adoptions=globalThis.__nativeResourceAdoptions||0;__nrs.releases=globalThis.__nativeResourceReleases||0;__nrs.imgAdopts=globalThis.__nativeImgAdopts||0;__nrs.imgCancels=globalThis.__nativeImgCancels||0;__nrs.imgDisowns=globalThis.__nativeImgDisowns||0;__nrs.imgTransfers=globalThis.__nativeImgTransfers||0;__log('[native-resource-stats] '+JSON.stringify(__nrs));}catch(e){}
    }
  }
  var __tk = globalThis.__tkProf || (globalThis.__tkProf = {});   // __tick 分段耗时(badframe 快照读,回答"渲染管线之外的 JS 在哪")
  var __tkT = performance.now();
  // fgui facade 合帧的帧边界兜底(开头这次收上帧微任务续体产生的 dirty,通常空集零成本;
  // 尾部 flush 段还有一次)。挂接缺席=合帧无兜底,native_fgui 侧靠读屏障与 fail-safe 自保。
  if (typeof globalThis.__nativeFguiFrameFlush === 'function') try{ globalThis.__nativeFguiFrameFlush(); }catch(e){}
  // Swap two persistent queues instead of allocating one Array per normal frame. Callbacks
  // scheduled during dispatch still land in the next-frame queue exactly as before. A synthetic
  // re-entrant __tick must not reuse/clear the outer queue which is still being traversed, so only
  // that exceptional path allocates the same fresh queue used by the old implementation.
  var cbs = _rafCbs;
  if(_rafDispatchDepth===0){_rafCbs = _rafSpare; _rafCbs.length = 0; _rafSpare = cbs;}
  else _rafCbs = [];
  globalThis.__sgPhase = 'raf';   // 诊断用阶段标记([sp-vis] 等打点读):raf→fgui→st→idle(微任务/timer 都在 idle)
  if (globalThis.__auTryHook) globalThis.__auTryHook();   // pixi-sound 加载晚:每帧尝试接音频 decode 钩子,成功即短路
  // NATIVE_RAF_CALLBACK_PROFILE_DISPATCH_BEGIN
  _rafDispatchDepth++;
  try{
    if(globalThis.__NATIVE_SPIN_PROF===1){
      var __rafTop=[];
      for(var i=0;i<cbs.length;i++){
        var __rafCbT0=performance.now();
        try{cbs[i].cb(tMs);}catch(e){__log('[raf] 回调异常: '+e.message+' | 栈: '+String(e.stack||'').slice(0,2000));__reportHostCallbackError('raf_exception',e);}
        __rafProbeKeepTop(__rafTop,cbs[i].cb,performance.now()-__rafCbT0);
      }
      __rafProbePublish(__rafTop,(globalThis.__frameNo|0)+1);
    }else{
      // 基线仍是原始直调循环：无逐回调取时、标签或排序开销。
      for(var i=0;i<cbs.length;i++){try{cbs[i].cb(tMs);}catch(e){__log('[raf] 回调异常: '+e.message+' | 栈: '+String(e.stack||'').slice(0,2000));__reportHostCallbackError('raf_exception',e);}}
    }
  }finally{
    cbs.length=0;
    _rafDispatchDepth--;
  }
  // NATIVE_RAF_CALLBACK_PROFILE_DISPATCH_END
  __tk.raf = performance.now() - __tkT; __tkT = performance.now();
  globalThis.__sgPhase = 'fgui';
  // 手动驱动 fgui GTimer：实测它的 advance/tickTween 没被 app.ticker 实际调用（fgui transition/timer 永不前进 →
  // 场景 fadein 的 Transition onComplete 不触发 → 卡死）。在 raf 回调（含 app.ticker.update→刷新 deltaTime）之后调，
  // 用其 $ticker(app.ticker) 已算好的 deltaTime 推进。
  try {
    var g = globalThis.fgui;
    if (g && g.GTimer && g.GTimer.inst && g.GTimer.inst.advance) {
      var gt = g.GTimer.inst;
      // 双驱动防护:fgui 原生 $ticker 挂接(setTicker→app.ticker 的 advance/tickTween)在 perf.now 基准
      // 修复后已于 native 复活——本帧已被原生驱动(Tween.tick 帧号为证)则跳过手动,否则 Transition/
      // GTimer 全部 2 倍速(实测相册 cut_card 每帧 tick×2,卡片提前 200ms 出现)。原生失效时仍兜底。
      if (globalThis.__tweenTickF !== globalThis.__frameNo && gt.$ticker && typeof gt.$ticker.deltaTime === 'number') {
        gt.$ticker.started = true;
        gt.advance();
        if (gt.tickTween) gt.tickTween();
      }
    }
  } catch(e){ if(!globalThis.__fguiDriveErr){ globalThis.__fguiDriveErr=1; __log('[fgui-drive] '+e.message); } }
  __tk.fgui = performance.now() - __tkT; __tkT = performance.now();
  globalThis.__sgPhase = 'st';
  // （SimpleWorkflowController.onTick=停轮/中奖演出工作流）挂在 Ticker.shared 上，native 环境里该
  // ticker 的 raf 回调实测未被持续驱动 → 必须每帧手动 update(tMs) 推进。
  // 注：这些驱动源（以及 async 工作流的微任务续体）都可能在 renderer.render 之后继续改状态；
  // "画 render 时刻状态"由 native 侧保证——game_window.mm 的 __nativePresent 已改为调用点内联取景，
  // 不再等 __tick 结束（否则这些 render 后中间态会提前一帧上屏，如 m90004 小狗停轮闪第一格）。
  try {
    var st = globalThis.__pixiTickerShared;
    if (st && st.update) {
      st.started = true; st.update(tMs);
    }
    var sy = globalThis.__pixiTickerSystem;
    if (sy && sy.update) { sy.started = true; sy.update(tMs); }
  } catch(e){ if(!globalThis.__stErr){ globalThis.__stErr=1; __log('[shared-ticker] '+e.message); } }
  __tk.st = performance.now() - __tkT;
  // Native Spine subclasses (OnceSpine / IntervalSpine / SpineView...) deliberately keep the
  // browser-side manual clock. Main-render deltas are flushed by native_scenegraph immediately
  // before its inline present so ordinary UI and Spine commit the same logical frame. This tail
  // pump owns lifecycle/event delivery and catches deltas produced after that render (notably by
  // shared/system tickers), ready for the next presentation.
  if (typeof globalThis.__spxFacadeTick === 'function') {
    try { globalThis.__spxFacadeTick(); }
    catch(e) { if(!globalThis.__spxTickErr){ globalThis.__spxTickErr=1; __log('[spinex-tick] '+e.message); } }
  }
  // __tick 函数体总耗时。宿主测的 gFrameJSUs 是 JSObjectCallAsFunction 的墙钟,JSC 会在它返回前
  // 排空微任务队列——业务的 async/await 续体(弹窗 init 等)因此计入宿主耗时却不在任何 __tk.* 里。
  // body 远小于 busy 即证明尖峰在微任务而非 tick 本体。
  __tk.body = performance.now() - __tk.__bodyT0;
  // [tick-load] 诊断态负载观测(每 300 帧≈5s 一行)。badframe 的 JS 墙钟 = body + 微任务排空
  // + JSC GC;__tk.* 只测 body。mtq 哨兵排到本帧微任务队尾,近似测出排空时长(低估:哨兵后新续体
  // 不计)——body 大=tick 本体、mtq 大=async 续体、两者都小而墙钟大=GC/JSC 内部。规模列跟踪
  // "越玩越卡"的结构堆积:spinex 门面 live 数/Ticker listener 数/__sgOv/blob 字节/adopt 差/显存。
  // Promise 哨兵本身每帧创建 continuation/Promise，会持续给正式路径制造 GC 压力。测试机、
  // __PROF 或显式 __NATIVE_RUNTIME_DIAG=1 时保留完整采样；native >25ms 的 __badFrame
  // 分段计时不依赖本块，始终有效。

  globalThis.__sgPhase = 'idle';   // __tick 之后跑的都是微任务/timer(打点看到 idle = render 管线之外)
  globalThis.__frameNo = (globalThis.__frameNo||0) + 1;
  // mask/filter/rt 计数 hook(renderer 实例就绪后一次性挂;系统类未导出 PIXI 命名空间,只能走实例原型)
  if (!globalThis.__pfSysHook && globalThis.__PIXI_APP__ && globalThis.__PIXI_APP__.renderer){
    globalThis.__pfSysHook = 1;
    try {
      var __r = globalThis.__PIXI_APP__.renderer;
      var __msP = __r.mask && Object.getPrototypeOf(__r.mask);
      if (__msP && __msP.push){ var _mp2 = __msP.push;
        __msP.push = function(){ if(globalThis.__pfCnt) globalThis.__pfCnt.mask++; return _mp2.apply(this, arguments); }; }
      var __flP = __r.filter && Object.getPrototypeOf(__r.filter);
      if (__flP && __flP.push){ var _fp2 = __flP.push;
        __flP.push = function(){ if(globalThis.__pfCnt) globalThis.__pfCnt.filter++; return _fp2.apply(this, arguments); }; }
      var __rtP = __r.renderTexture && Object.getPrototypeOf(__r.renderTexture);
      if (__rtP && __rtP.bind){ var _rb2 = __rtP.bind;
        __rtP.bind = function(rt){ if(rt && globalThis.__pfCnt) globalThis.__pfCnt.rt++; return _rb2.apply(this, arguments); }; }
      __log('[pf] mask/filter/rt 实例 hook 已挂');
    } catch(e){ __log('[pf] 实例 hook 失败 '+e.message); }
  }
  // 注:曾试 grayed→cacheAsBitmap 消灭灰卡 filter pass,实测列表 item 复用使 grayed 频繁切换 →
  // 缓存反复烘焙(filter:10+text:10 周期 spike),净收益为负,已回滚。灰卡 filter 治理需游戏侧配合(灰度纹理)。
  // native 解析失败(zip64/未知压缩法)或条目 miss → 回落原始 JSZip,行为不变。__ZIP_NATIVE=0 关。
  if (!globalThis.__zipHook && globalThis.JSZip && typeof globalThis.__zipParse === 'function' && globalThis.__ZIP_NATIVE !== 0){
    globalThis.__zipHook = 1;
    (function(){
      var _orig = globalThis.JSZip.loadAsync.bind(globalThis.JSZip);
      var tk = 0, pend = {};
      globalThis.__zipDone = function(t, namesJson){
        var p = pend[t]; delete pend[t]; if (!p) return;
        if (!namesJson){ __log('[zip] native 解析失败,回落 JSZip'); _orig(p.ab).then(p.res, p.rej); return; }
        var names = JSON.parse(namesJson), files = {};
        names.forEach(function(n){
          files[n] = { name: n, dir: false,
            async: function(type){
              var asStr = (type === 'string' || type === 'text');
              var v = globalThis.__zipGet(t, n, asStr ? 1 : 0);
              if (v === null || v === undefined)
                return _orig(p.ab).then(function(z){ var f = z.file(n); return f ? f.async(type) : Promise.reject(new Error('zip entry miss: '+n)); });
              if (type === 'uint8array') v = new Uint8Array(v);
              return Promise.resolve(v);
            } };
        });
        p.res({ files: files,
          // JSZip 语义:file(string)→单条目|null;file(RegExp)→匹配数组(i18n 用 zip.file(/.json/) 曾因缺此崩启动)
          file: function(sel){
            if (sel instanceof RegExp){ var out = []; names.forEach(function(n){ if (sel.test(n)) out.push(files[n]); }); return out; }
            return files[sel] || null;
          },
          forEach: function(cb){ names.forEach(function(n){ cb(n, files[n]); }); } });
      };
      globalThis.JSZip.loadAsync = function(buf){
        var ab = (buf instanceof ArrayBuffer) ? buf
               : (buf && buf.buffer instanceof ArrayBuffer) ? buf.buffer : null;
        if (!ab) return _orig.apply(null, arguments);
        return new Promise(function(res, rej){ var t = ++tk; pend[t] = {res:res, rej:rej, ab:ab}; globalThis.__zipParse(t, ab); });
      };
      __log('[pf] JSZip native 桥已挂');
    })();
  }
  // Replace only the zspine parser's package-preparation half. Native failure
  // calls the untouched parser, which keeps the existing JSZip compatibility path.



  // fgui UIPackage 解析计时(__PROF=1 才装):包二进制解析+组件构建

  // (fgui 深度 dispose __FGUI_DEEP_DISPOSE 死路线已删:深销毁 orphan 误杀游戏仍引用的共享对象
  //  (buy_btn 等)导致切场景卡死,判据 `!_destroyed && !parent` 无法区分真孤儿与游戏持有的离树
  //  对象——泄漏定性为缓存池常驻树内的 plateau(~198 稳定),无可 destroy 根治项,2026-08-25 清仓)
  // ===== 以下诊断 hook 仅 __PROF===1 才装(默认关,测温零污染)=====

  // spine 屏外剔除统计(每 5s 一行;__SPINE_CULL=0 可关剔除)
  if ((globalThis.__frameNo % 300) === 0 && (globalThis.__spCulled||globalThis.__spFull)){
    __log('[spine-cull] 5s: 剔除'+(globalThis.__spCulled||0)+' 全驱动'+(globalThis.__spFull||0));
    globalThis.__spCulled = 0; globalThis.__spFull = 0;
  }
  // spine 批量驱动统计(每 5s 一行;flush 次数≈帧数,批实例/flush=每帧原本的跨界次数,现在每帧 1 次)
  if ((globalThis.__frameNo % 300) === 0 && globalThis.__skbFlushN){
    __log('[skel-batch] 5s: flush'+globalThis.__skbFlushN+'次 批'+(globalThis.__skbBatchedN||0)+'实例(峰值'+(globalThis.__skbPeak||0)+'/帧) 同步fallback'+(globalThis.__skbSyncN||0));
    globalThis.__skbFlushN=0; globalThis.__skbBatchedN=0; globalThis.__skbSyncN=0; globalThis.__skbPeak=0;
  }
  // P1a native 骨骼变换对拍汇总(__SPSKEL=2)
  if ((globalThis.__frameNo % 300) === 0 && globalThis.__skelChkN){
    __log('[skel-check] 300帧: 对拍'+globalThis.__skelChkN+'次 全绿'+(globalThis.__skelOkN||0)+'次'+((globalThis.__skelOkN===globalThis.__skelChkN)?' ✅全部一致':' ⚠️见[skel-verify]'));
    globalThis.__skelChkN=0; globalThis.__skelOkN=0;
  }
  // Path ROI 度量(__SKEL_STAT=1):native 已接管 vs fallback(Path 是剩余大头?)
  if ((globalThis.__frameNo % 300) === 0 && globalThis.__SKEL_STAT===1 && (globalThis.__skelNatB||globalThis.__skelFbB)){
    var __nb=globalThis.__skelNatB||0, __fb=globalThis.__skelFbB||0, __pb=globalThis.__skelPathB||0, __t=__nb+__fb;
    __log('[skel-stat] 300帧骨骼·帧: native已接管'+__nb+'('+(100*__nb/__t|0)+'%) fallback'+__fb+'(其中Path'+__pb+') → Path 若做能再吃 '+(100*__pb/__t|0)+'%');
    globalThis.__skelNatB=0; globalThis.__skelFbB=0; globalThis.__skelPathB=0;
  }
  // fgui 双驱动防护配套:createjs.Tween.tick 记录本帧已推进(上方手动驱动块以此判断原生挂接是否活着)
  if (!globalThis.__trDbgPatched && globalThis.createjs && globalThis.createjs.Tween){
    globalThis.__trDbgPatched = 1;
    (function(){
      var _ttick = globalThis.createjs.Tween.tick;
      globalThis.createjs.Tween.tick = function(){
        globalThis.__tweenTickF = globalThis.__frameNo;
        return _ttick.apply(this, arguments);
      };
    })();
  }
  // gsap.to 首帧立即应用(复刻浏览器时序):浏览器里 gsap ticker 先于渲染跑,tween 创建后画面上看不到
  // "初始位置"那一帧;native 帧间(pointer 事件)创建的 tween 首 tick 在下一帧,期间对象停在初始位置
  // (如 motionPath 的食物在 (0,0))被渲染 → 左上角闪。创建时 render(0) 立即写入 0 时刻值即根治。
  // 仅限无 delay/paused 的 to(delay 期间不应提前应用;from/fromTo 的 immediateRender gsap 自带)。
  if (!globalThis.__gsapPatched && globalThis.gsap && globalThis.gsap.to){
    globalThis.__gsapPatched = 1;
    (function(){
      var _gto = globalThis.gsap.to;
      globalThis.gsap.to = function(){
        var tw = _gto.apply(this, arguments);
        var v = arguments[arguments.length-1];
        if (tw && v && typeof v==='object' && !v.paused && !v.delay){ try{ tw.render(0, true, true); }catch(e){} }
        return tw;
      };
      __log('[gsap] to() 首帧立即应用已接管(防帧间创建的 tween 首帧闪)');
    })();
  }
  // 全屏截图接管:PixiUtils.screenShot(游戏发邮件等用)→ native __snapshot 一步出 JPEG data URL,
  // 绕开 pixi extract 的 generateTexture/renderTexture 离屏链(重且曾诱发场景图回写异常);失败回退原路径。

  __tkT = performance.now();
  __flushHostTasks();
  // Flush facade semantics before the final command submission. handleSizeChanged and
  // markScrollPaneDirty may encode native commands; running this after flushPending would make
  // controller-driven size changes appear one frame late.
  if (typeof globalThis.__nativeFguiFrameFlush === 'function') try{ globalThis.__nativeFguiFrameFlush(); }catch(e){}
  flushPending();
  __tk.flush = performance.now() - __tkT;

};
// 逃逸率审计(路线图 5.1,conch 立项决策依据;__ESC_AUDIT=0 关):遍历 stage 统计可 native 合批的
// 普通 sprite 覆盖率 vs 各类逃逸节点(spine 子树/graphics/text/mask/filter)。tick 每 600 帧自动打一行。
globalThis.__escAudit = function(){
  var app = globalThis.__PIXI_APP__; if (!app || !app.stage) return;
  var PX = globalThis.PIXI; if (!PX) return;
  var c = {sprite:0, noTex:0, text:0, bmText:0, gfx:0, spineTree:0, container:0, masked:0, filtered:0, other:0, invisTree:0};
  function walk(n){
    if (n.visible === false){ c.invisTree++; return; }
    if (n.__spRoot || n.__spTree){ c.spineTree++; return; }   // spine 子树整支 = escape,不深入
    if (n._mask) c.masked++;
    if (n.filters && n.filters.length) c.filtered++;
    if (PX.Text && n instanceof PX.Text) c.text++;
    else if (PX.BitmapText && n instanceof PX.BitmapText) c.bmText++;
    else if (PX.Graphics && n instanceof PX.Graphics) c.gfx++;
    else if (n.isSprite){ c.sprite++; if (!(n.texture && n.texture.baseTexture && n.texture.baseTexture.valid)) c.noTex++; }
    else if (!n.children || !n.children.length) c.other++;
    else c.container++;
    var ch = n.children; if (ch) for (var i=0;i<ch.length;i++){ var k=ch[i]; if (k) walk(k); }
  }
  try { walk(app.stage); } catch(e){ return; }
  var leaf = c.sprite + c.text + c.bmText + c.gfx + c.spineTree + c.other;
  __log('[esc-audit] 叶子'+leaf+' sprite:'+c.sprite+'('+(leaf?(100*c.sprite/leaf).toFixed(0):'?')+'% noTex:'+c.noTex+')'
    +' text:'+c.text+' bmText:'+c.bmText+' gfx:'+c.gfx+' spine子树:'+c.spineTree+' 其它:'+c.other
    +' | 容器:'+c.container+' mask:'+c.masked+' filter:'+c.filtered+' 不可见子树:'+c.invisTree);
};
