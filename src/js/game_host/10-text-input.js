// TextInput 机制:平时用 pixi Text(surrogate)在画面内渲染文本,聚焦时才用 DOM input 接键盘。
// native 替身:focus → __kbShow 弹 iOS 键盘(隐形 UITextField 承接)→ 每次变化 __kbInput(text) 回传
// → 设 value + 派发 input 事件 → surrogate 实时更新;回车/收起 → __kbDone → keydown(13)/blur。
// 文本同步铁律:JS 侧任何 `input.value = x` 写入(游戏 inputHandler 截断/过滤后写回)必须同步到 native
// UITextField(__kbSetText),否则两边文本分叉 → 画面不更新/Done 提交旧值/re-focus 覆盖丢字。
// 反向:native 键盘输入经 __kbInput 回传时写内部槽(不回写 native,防回环)。
(function(){
  var __kbActive = null;
  var __kbHidePending = null;   // blur 后延迟一拍才真正收键盘;同 tick 内 re-focus 同一元素则取消(库的 blur→focus 流程)
  function InputEl(multiline){
    this.__lst = {}; this.__multiline = !!multiline;
    this.__v = ''; this.type = 'text'; this.style = {};
    this.maxLength = 0; this.disabled = false; this.readOnly = false;
    this.selectionStart = 0; this.selectionEnd = 0;
    this.parentNode = null; this.parentElement = null;
  }
  var P = InputEl.prototype;
  // value 桥接:游戏写 value(超长截断/非法字符过滤后写回)→ 同步 native UITextField,键盘缓冲与 JS 保持一致
  Object.defineProperty(P, 'value', {
    get: function(){ return this.__v; },
    set: function(v){ v = String(v==null?'':v);
      if (v === this.__v) return;
      this.__v = v;
      if (__kbActive===this && typeof __kbSetText==='function'){ try{ __kbSetText(v); }catch(e){} }
    }
  });
  P.addEventListener = function(t,f){ (this.__lst[t]||(this.__lst[t]=[])).push(f); };
  P.removeEventListener = function(t,f){ var a=this.__lst[t]; if(a){ var i=a.indexOf(f); if(i>=0)a.splice(i,1); } };
  P.__fire = function(t, ev){ ev=ev||{}; ev.type=t; ev.target=this; ev.keyCode=ev.keyCode||0;
    ev.preventDefault=ev.preventDefault||function(){}; ev.stopPropagation=ev.stopPropagation||function(){};
    var on=this['on'+t]; if(typeof on==='function'){ try{ on.call(this,ev); }catch(e){} }
    var a=this.__lst[t]; if(a){ a=a.slice(); for(var i=0;i<a.length;i++){ try{ a[i].call(this,ev); }catch(e){} } } };
  function __inputPadding(style){
    var raw=String(style.padding||'0').trim().split(/\s+/), n=[];
    for(var i=0;i<raw.length;i++) n.push(parseFloat(raw[i])||0);
    if(n.length===1) return [n[0],n[0],n[0],n[0]];
    if(n.length===2) return [n[0],n[1],n[0],n[1]];
    if(n.length===3) return [n[0],n[1],n[2],n[1]];
    return [n[0]||0,n[1]||0,n[2]||0,n[3]||0];
  }
  function __inputColor(value){
    if(typeof value==='number' && isFinite(value)) return value>>>0;
    var s=String(value||'').trim(), m;
    if(/^#[0-9a-f]{3}$/i.test(s)) return parseInt(s[1]+s[1]+s[2]+s[2]+s[3]+s[3],16);
    if(/^#[0-9a-f]{6}$/i.test(s)) return parseInt(s.slice(1),16);
    m=/rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)/i.exec(s);
    return m ? ((+m[1]&255)<<16)|((+m[2]&255)<<8)|(+m[3]&255) : 0xffffff;
  }
  function __inputNativeStyle(el){
    var style=el.style, sx=1, sy=1, owner=el.__textInputOwner, wt=owner&&owner.worldTransform;
    if(wt){ sx=Math.sqrt(wt.a*wt.a+wt.b*wt.b)||1; sy=Math.sqrt(wt.c*wt.c+wt.d*wt.d)||1; }
    else { var matrix=/matrix\(([^)]+)\)/.exec(String(style.transform||''));
      if(matrix){ var p=matrix[1].split(','); sx=Math.abs(parseFloat(p[0])||1); sy=Math.abs(parseFloat(p[3])||1); }
    }
    var pad=__inputPadding(style), align=String(style.textAlign||'left').toLowerCase();
    return {
      fontSize:(parseFloat(style.fontSize)||20)*sy,
      fontFamily:String(style.fontFamily||''),
      color:__inputColor(style.caretColor||style.color),
      align:align==='center'?1:(align==='right'?2:0),
      top:pad[0]*sy, right:pad[1]*sx, bottom:pad[2]*sy, left:pad[3]*sx
    };
  }
  function __inputLocalSize(style){
    var width=parseFloat(style.width)||0, height=parseFloat(style.height)||0;
    if(!height){ var fontSize=parseFloat(style.fontSize)||20, padding=(parseFloat(style.padding)||0)*2;
      height=Math.ceil(fontSize*1.3)+padding; }
    return {width:width,height:height};
  }
  function __inputWorldBounds(owner,width,height){
    if(!owner || typeof owner.toGlobal!=='function') return null;
    try {
      // toGlobal refreshes the native scenegraph world matrix before returning coordinates.
      var p0=owner.toGlobal({x:0,y:0}), p1=owner.toGlobal({x:width,y:0});
      var p2=owner.toGlobal({x:0,y:height}), p3=owner.toGlobal({x:width,y:height});
      var x=Math.min(p0.x,p1.x,p2.x,p3.x), y=Math.min(p0.y,p1.y,p2.y,p3.y);
      return {x:x,y:y,width:Math.max(p0.x,p1.x,p2.x,p3.x)-x,
        height:Math.max(p0.y,p1.y,p2.y,p3.y)-y};
    } catch(e) { return null; }
  }
  P.focus = function(){
    globalThis.__PROF===1 && __log('[input]focus(value="'+this.value+'" native='+(typeof __kbShow==='function')+') active='+(__kbActive===this?'self':(__kbActive?'other':'none'))+' pending='+(__kbHidePending===this));
    if (__kbActive===this){ if(typeof __kbKeep==='function'){ try{ __kbKeep(); }catch(e){} } return; }   // 会话未变:仅取消 native 延迟收起,不动文本
    if (__kbHidePending===this){   // 刚 blur 又 focus 同一元素(库重聚焦):恢复会话,native 文本保持不重置
      __kbHidePending=null; __kbActive=this;
      if(typeof __kbKeep==='function'){ try{ __kbKeep(); }catch(e){} }
      this.__fire('focus'); return;
    }
    __kbHidePending = null;
    if (__kbActive) __kbActive.__fire('blur');   // 切换输入框:旧的补 blur 事件
    __kbActive = this;
    document.activeElement = this;   // 库 _hasFocus() 用 activeElement 判断,不模拟会导致 _ensureFocus 反复 re-focus
    if (typeof __kbShow==='function'){ try{
      // The DOM substitute can be focused before Pixi has written its CSS transform.
      // Keyboard placement always needs the owner's screen-space bounds; the local
      // bounds returned during TextInput layout measurement must stay unscaled.
      var size=__inputLocalSize(this.style);
      var r=__inputWorldBounds(this.__textInputOwner,size.width,size.height) || this.getBoundingClientRect();
      var ns=__inputNativeStyle(this);
      this.selectionStart=this.selectionEnd=this.value.length;
      globalThis.__PROF===1 && __log('[input]__kbShow rect=('+r.x+','+r.y+','+r.width+','+r.height+')');
      __kbShow(String(this.value||''), this.maxLength>0?this.maxLength:0, this.type==='password'?1:0, this.__multiline?1:0,
        r.x||0, r.y||0, r.width||0, r.height||0, ns.fontSize, ns.fontFamily, ns.color, ns.align,
        ns.top, ns.right, ns.bottom, ns.left);
    }catch(e){} }
    this.__fire('focus');
  };
  P.blur = function(){
    globalThis.__PROF===1 && __log('[input]blur(active='+(__kbActive===this)+')');
    if (__kbActive===this){
      __kbActive=null; __kbHidePending=this;
      if (document.activeElement===this) document.activeElement=null;
      var self=this;
      setTimeout(function(){ if(__kbHidePending===self){ __kbHidePending=null; if(typeof __kbHide==='function'){ try{ __kbHide(); }catch(e){} } } },0);
    }
    this.__fire('blur');
  };
  P.select = function(){ globalThis.__nativeClipboardSelect(this, 0, this.value.length); };
  P.setSelectionRange = function(s,e){ globalThis.__nativeClipboardSelect(this, s, e); };
  P.setAttribute = function(k,v){ if(k==='maxlength')this.maxLength=+v||0; else if(k==='type')this.type=v; else this[k]=v; };
  P.removeAttribute = function(k){ if(k==='maxlength')this.maxLength=0; };
  P.getBoundingClientRect = function(){
    // TextInput 的 surrogate hitbox 尺寸取自这里(浏览器里反映 CSS style)→ 必须解析 style 的 width/height,
    // 返回 0 会导致 hitbox 零面积、输入框永远点不到。
    var size=__inputLocalSize(this.style), w=size.width, h=size.height;
    var x = parseFloat(this.style.left)||0, y = parseFloat(this.style.top)||0;
    // left/top 只是 canvas 原点(0):真实位置/缩放从 matrix 取,否则 hitbox/键盘避让拿到的全是 0。
    var owner=this.__textInputOwner, wt=owner&&owner.worldTransform, usedGlobal=false;
    // TextInput._getDOMInputBounds temporarily clears transform to measure the
    // unscaled CSS box. Only publish screen-space bounds while the transform is
    // present (the focus/keyboard path); otherwise keep width/height local.
    var hasTransform=!!String(this.style.transform||'').trim();
    var world=hasTransform && __inputWorldBounds(owner,w,h);
    if(world){ x=world.x; y=world.y; w=world.width; h=world.height; usedGlobal=true; }
    if (!usedGlobal && hasTransform && wt){
      x += wt.tx||0; y += wt.ty||0;
      w *= Math.sqrt(wt.a*wt.a+wt.b*wt.b)||1;
      h *= Math.sqrt(wt.c*wt.c+wt.d*wt.d)||1;
    } else if (!usedGlobal && hasTransform){
      var m = /matrix\(([^)]+)\)/.exec(this.style.transform);
      if (m){ var p = m[1].split(',');
        x += parseFloat(p[4])||0; y += parseFloat(p[5])||0;
        w *= Math.abs(parseFloat(p[0])||1); h *= Math.abs(parseFloat(p[3])||1);
      } else {
        var t = /translate\(\s*(-?[\d.]+)px?\s*,\s*(-?[\d.]+)px?/.exec(this.style.transform);
        if (t){ x += parseFloat(t[1])||0; y += parseFloat(t[2])||0; }
      }
    }
    return {x:x, y:y, left:x, top:y, right:x+w, bottom:y+h, width:w, height:h};
  };
  globalThis.HTMLInputElement = InputEl;
  globalThis.__mkInput = function(multiline){ return new InputEl(multiline); };
  // native callbacks
  globalThis.__kbInput = function(t, composing){ var el=__kbActive;
    globalThis.__PROF===1 && __log('[input]__kbInput length='+String(t==null?'':t).length+' composing='+!!composing+' active='+!!el);
    if(!el) return;
    if(composing && !el.__composing){ el.__composing=true; el.__fire('compositionstart'); }
    el.__v = String(t==null?'':t);   // 写内部槽:native 是来源,不经 setter 回写(防回环)
    el.selectionStart = el.selectionEnd = el.__v.length;
    el.__fire('input');
    if(!composing && el.__composing){ el.__composing=false; el.__fire('compositionend'); }
  };   // 游戏 handler 若截断写回 value → setter 桥接 __kbSetText 同步 native
  globalThis.__kbDone = function(isReturn){ var el=__kbActive; __kbHidePending=null; if(!el) return; __kbActive=null;
    globalThis.__PROF===1 && __log('[input]__kbDone(isReturn='+!!isReturn+', length='+el.__v.length+')');
    if (document.activeElement===el) document.activeElement=null;
    if(el.__composing){ el.__composing=false; el.__fire('compositionend'); }
    if (isReturn){ el.__fire('keydown',{keyCode:13}); el.__fire('keyup',{keyCode:13}); }
    el.__fire('blur'); };
  globalThis.__kbIsEditing = function(){ return !!__kbActive; };
})();
