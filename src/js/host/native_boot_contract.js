'use strict';
// Native 启动契约:所有改变构造函数/工厂/原型/所有权的安装点统一注册,由既有脚本加载锚点
// (evalShellScript 逐脚本、loadJsLib 完成边界)同步驱动;loadJsLib 结束时 seal 一次,
// required 缺失即启动失败事实(默认观察模式记录,__NATIVE_BOOT_SEAL_ENFORCE=1 拦截业务启动)。取代"定时器轮询等依赖":曾有固定 600×16ms 预算在交互登录超时后静默放弃
// (native_fgui.js boot 注释所载教训),同款预算残留于 popup/purchase/reel/scrollpane/纹理释放
// 各 hook——本契约使安装结果只由依赖事件决定,与库加载耗时无关。
//
// installer 契约:install() 同步、幂等、无副作用重试;返回
//   'installed'   Native 实现已完整安装(true 等价)
//   'pending'     依赖还没出现,等下一个锚点(false/undefined 等价)
//   'legacy'      明确选择旧实现(如显式开关关闭)
//   'unsupported' 当前运行环境不支持
//   'failed'      安装过程出错(抛异常等价)
// 状态单向:pending → 其余四态,定格后不再变化。
//
// required 语义:默认只有 'installed' 通过 seal;确有调试逃生门的能力用逐项
// allowedFinalStates:['installed','legacy'] 显式声明('failed' 永远算缺失,不可列入)。
//
// 全在 Shell.JS_LIB(game/_app.js),seal 裁决:required 仍 pending 记 failed,可选项固定
// legacy)。domain:'late' 机制保留给真正 seal 之后才到的依赖,当前无注册项;将来接入时须
// 配"目标脚本结束"锚点(afterScript({src,ok})),不得用通用锚点猜。seal 后注册 lib 域直接
// 抛错(编排断裂显性化);late 域仍可注册与推进。
//
// expect(ids):中央 manifest——声明必须注册的能力 id;seal/报告对"从未注册"(注册代码被删/
// 被前置异常跳过)记 unregistered 并计缺失,能力不会从报告中无声消失。
//
// api:register({..., api}) 可交出模块对象,消费方 __nativeBoot.get(id) 按能力 id 取,取代读 globalThis.__x
// 的隐式约定(结构方案第 3 步;旧模块不强制迁移,新模块走 registry)。get 对未注册/无 api 的 id 返回 null。
//
// 契约模块加载前的注册(game_host.js 早段)推入 globalThis.__nativeBootQueue,本模块加载时清空。
(function(root, factory){
  var api = factory();
  var commonjs = typeof module === 'object' && module.exports;
  if (commonjs) module.exports = api;
  if (root && !commonjs) api.install(root);
})(typeof globalThis !== 'undefined' ? globalThis : this, function(){

  var FINAL = { installed: 1, legacy: 1, unsupported: 1, failed: 1 };

  function create(log){
    if (typeof log !== 'function') log = function(){};
    var order = [], byId = Object.create(null), expectedIds = [];
    var seq = 0, sealedFlag = false, installing = false;

    function register(spec){
      if (!spec || typeof spec.id !== 'string' || !spec.id) throw new Error('[native-boot] register: id required');
      var existing = byId[spec.id];
      if (existing) {
        // 幂等仅限同一形态的重复 eval(如 reel-proxy 双加载);required/domain 不一致=两处
        // 注册代码已漂移,直接显性化而不是静默沿用旧 entry。
        if (existing.required !== (spec.required === true) || existing.late !== (spec.domain === 'late'))
          throw new Error('[native-boot] ' + spec.id + ': conflicting re-register (required/domain mismatch)');
        if (spec.api !== undefined) existing.api = spec.api;   // 二次加载可刷新对象(reel-proxy 重绑)
        return existing;
      }
      if (typeof spec.install !== 'function') throw new Error('[native-boot] ' + spec.id + ': install() required');
      var late = spec.domain === 'late';
      if (sealedFlag && !late) throw new Error('[native-boot] ' + spec.id + ': lib-domain register after seal (orchestration break)');
      var allowed = ['installed'];
      if (spec.allowedFinalStates !== undefined) {
        if (!Array.isArray(spec.allowedFinalStates) || !spec.allowedFinalStates.length) throw new Error('[native-boot] ' + spec.id + ': allowedFinalStates must be a non-empty array');
        allowed = [];
        for (var ai = 0; ai < spec.allowedFinalStates.length; ai++) {
          var st = String(spec.allowedFinalStates[ai]);
          if (!FINAL[st] || st === 'failed') throw new Error('[native-boot] ' + spec.id + ': allowedFinalStates entry "' + st + '" is not an allowed final state');
          allowed.push(st);
        }
      }
      var entry = {
        id: spec.id,
        required: spec.required === true,
        late: late,
        allowed: allowed,
        install: spec.install,
        api: spec.api === undefined ? null : spec.api,
        state: 'pending', reason: '', error: null,
        registerSeq: ++seq, installSeq: 0, attempts: 0
      };
      byId[entry.id] = entry;
      order.push(entry);                                  // installNow 遍历读 length:同轮内级联注册的项同轮生效
      return entry;
    }

    function expect(ids){
      if (!Array.isArray(ids)) throw new Error('[native-boot] expect: array of capability ids required');
      for (var i = 0; i < ids.length; i++) {
        var id = String(ids[i]);
        if (id && expectedIds.indexOf(id) < 0) expectedIds.push(id);
      }
    }

    function settle(entry, state, reason, error){
      entry.state = state;
      if (reason) entry.reason = reason;
      if (error) entry.error = error;
      entry.installSeq = ++seq;
    }

    function runOne(entry){
      if (entry.state !== 'pending') return;
      if (sealedFlag && !entry.late) return;              // seal 后 lib 域冻结;late 域仍由其加载锚点推进
      entry.attempts++;
      var result;
      try { result = entry.install(); }
      catch (e) {
        settle(entry, 'failed', 'installer threw', (e && e.message) || String(e));
        log('[native-boot] ' + entry.id + ' → failed: ' + entry.error + ' ' + ((e && e.stack) || ''));
        return;
      }
      if (result === true) result = 'installed';
      if (result === false || result === null || result === undefined) result = 'pending';
      if (result === 'pending') return;
      if (!FINAL[result]) {
        settle(entry, 'failed', 'bad installer result', 'installer returned "' + result + '"');
        log('[native-boot] ' + entry.id + ' → failed: ' + entry.error);
        return;
      }
      settle(entry, result, result === 'installed' ? '' : (entry.reason || 'installer decision'));
      log('[native-boot] ' + entry.id + ' → ' + result + ' (seq=' + entry.installSeq + ' attempts=' + entry.attempts + ')');
    }

    function installNow(){
      if (installing) return;                             // installer 体内触发锚点(如级联 WhenReady)防递归
      installing = true;
      try { for (var i = 0; i < order.length; i++) runOne(order[i]); }
      finally { installing = false; }
    }

    function snapshot(){
      var entries = [], missing = [], ok = true;
      for (var i = 0; i < order.length; i++) {
        var e = order[i];
        entries.push({ id: e.id, state: e.state, required: e.required, late: e.late,
          reason: e.reason, error: e.error, registerSeq: e.registerSeq, installSeq: e.installSeq, attempts: e.attempts });
        // required 默认必须 'installed';逃生门经逐项 allowedFinalStates 显式声明(failed 不可入列)。
        if (!e.late && e.required && e.allowed.indexOf(e.state) < 0) {
          ok = false;
          missing.push(e.id + '(' + e.state + (e.error ? ': ' + e.error : '') + ')');
        }
      }
      // 中央 manifest:expect 声明的是 required 能力——从未注册(注册代码被删/被前置异常跳过)
      // 或注册成了 optional(误删 required:true)都算缺失,不许无声消失/降级。
      for (var x = 0; x < expectedIds.length; x++) {
        var wanted = expectedIds[x];
        var got = byId[wanted];
        if (got) {
          if (!got.required) { ok = false; missing.push(wanted + '(registered-but-not-required)'); }
          continue;
        }
        ok = false;
        entries.push({ id: wanted, state: 'unregistered', required: true, late: false,
          reason: 'expected-but-never-registered', error: null, registerSeq: 0, installSeq: 0, attempts: 0 });
        missing.push(wanted + '(unregistered)');
      }
      return { ok: ok, sealed: sealedFlag, entries: entries, missing: missing };
    }

    function seal(){
      if (sealedFlag) return snapshot();                  // 裁决只做一次;重复调用返回实时快照,不缓存旧报告
      installNow();                                       // 封口前最后一次同步机会
      sealedFlag = true;
      for (var i = 0; i < order.length; i++) {
        var e = order[i];
        if (e.late || e.state !== 'pending') continue;
        if (e.required) settle(e, 'failed', 'sealed-while-pending', e.error || 'dependency never appeared before seal');
        else settle(e, 'legacy', 'sealed-missing-dependency');
        log('[native-boot] seal: ' + e.id + ' → ' + e.state + ' (' + e.reason + ')');
      }
      var report = snapshot();
      var lines = [];
      for (var j = 0; j < report.entries.length; j++) {
        var en = report.entries[j];
        lines.push(en.id + '=' + en.state + (en.required ? '!' : '') + (en.late ? '~' : ''));
      }
      log('[native-boot] seal ' + (report.ok ? 'OK' : 'FAILED') + ' [' + lines.join(' ') + '] (!=required ~=late)');
      if (!report.ok) log('[native-boot] missing required: ' + report.missing.join(', '));
      return report;
    }

    return {
      register: register,
      expect: expect,
      installNow: installNow,
      seal: seal,
      isSealed: function(){ return sealedFlag; },
      state: function(id){ var e = byId[id]; return e ? e.state : ''; },
      get: function(id){ var e = byId[id]; return e && e.api !== undefined ? e.api : null; },
      report: snapshot
    };
  }

  function install(root){
    if (root.__nativeBoot) return root.__nativeBoot;
    var boot = create(function(m){ if (typeof root.__log === 'function') root.__log(m); });
    root.__nativeBoot = boot;
    root.__installNativeHooksNow = boot.installNow;
    root.__sealNativeHooks = boot.seal;
    var queue = root.__nativeBootQueue;
    if (queue && queue.length) {
      for (var i = 0; i < queue.length; i++) {
        try { boot.register(queue[i]); }
        catch (e) { if (typeof root.__log === 'function') root.__log('[native-boot] queued register failed: ' + ((e && e.message) || e)); }
      }
      queue.length = 0;
    }
    if (typeof root.__log === 'function') root.__log('[native-boot] 启动契约已就位(统一安装入口+seal)');
    return boot;
  }

  return { create: create, install: install };
});
