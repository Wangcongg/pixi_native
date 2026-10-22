// ===== 最小 DOMAdapter =====
function makeCanvas(w, h){
  var glctx = null, ctx2d = null, cid = -1;
  var nativeCursor='default',canvasStyle={};
  Object.defineProperty(canvasStyle,'cursor',{configurable:true,enumerable:true,
    get:function(){return nativeCursor;},
    set:function(value){
      value=String(value||'default');if(value===nativeCursor)return;nativeCursor=value;
      // Pixi also creates render-texture/offscreen canvases. Their EventSystems may update their
      // own CSS cursor, but only the visible application's canvas owns the AppKit cursor.
      var mainCanvas=(globalThis.__PIXI_APP__&&globalThis.__PIXI_APP__.view)||
                     (globalThis.__pixiApp&&globalThis.__pixiApp.view)||null;
      if(cv===mainCanvas&&typeof globalThis.__setNativeCursor==='function'){
        globalThis.__setNativeCursor(value);
      }
    }
  });
  var cv = {
    _w: w||1, _h: h||1, style:canvasStyle, _lst:{},
    // appendChild marks the visible Pixi canvas connected so EventSystem uses the CSS rect below
    // rather than its disconnected-canvas fallback (which uses backing pixels and divides pointer
    // input by DPR a second time on Retina displays).
    isConnected:false,
    __ensureCid:function(){ if (cid<0 && typeof __c2dCreate==='function') cid = __c2dCreate(this._w, this._h); return cid; },
    addEventListener:function(t,f){ (this._lst[t] = this._lst[t] || []).push(f); },
    removeEventListener:function(t,f){ var a=this._lst[t]; if(a){ var i=a.indexOf(f); if(i>=0) a.splice(i,1); } },
    setPointerCapture:function(){}, releasePointerCapture:function(){}, hasPointerCapture:function(){ return false; },
    getBoundingClientRect:function(){
      var cssW=parseFloat(this.style.width),cssH=parseFloat(this.style.height);
      var dpr=Number(globalThis.devicePixelRatio)||1;
      var rw=cssW>0?cssW:this._w/dpr,rh=cssH>0?cssH:this._h/dpr;
      return { x:0,y:0,top:0,left:0,right:rw,bottom:rh,width:rw,height:rh };
    },
    getContext:function(id){
      if (id==='webgl2') return glctx || (glctx = makeGL());
      if (id==='webgl' || id==='experimental-webgl') return glctx || (glctx = makeGL());
      if (id==='2d') return ctx2d || (ctx2d = make2D(this));
      return null;
    },
    toDataURL:function(fmt, q){
      var id = this.__ensureCid();
      if (id>=0 && typeof __c2dToDataURL==='function') return __c2dToDataURL(id, ''+(fmt||'image/png'), (q==null?0.92:+q));
      return 'data:,';
    }
  };
  Object.defineProperty(cv, 'width',  { get:function(){ return this._w; }, set:function(v){ this._w=v|0; if(cid>=0) __c2dResize(cid,this._w,this._h); } });
  Object.defineProperty(cv, 'height', { get:function(){ return this._h; }, set:function(v){ this._h=v|0; if(cid>=0) __c2dResize(cid,this._w,this._h); } });
  // 当 2d canvas 当纹理上传时，6-arg texImage2D 读 __data（左上行优先 premultiplied RGBA）
  Object.defineProperty(cv, '__data', { get:function(){ return (cid>=0 && typeof __c2dGetData==='function') ? __c2dGetData(cid) : null; } });
  if (globalThis.HTMLCanvasElement) Object.setPrototypeOf(cv, globalThis.HTMLCanvasElement.prototype);  // pixi CanvasResource.test 用 instanceof
  (globalThis.__allCanvases||(globalThis.__allCanvases=[])).push(cv);
  return cv;
}
function css(v){ return (typeof v==='string') ? v : (typeof v==='number' ? ('#'+('000000'+(v>>>0).toString(16)).slice(-6)) : '#000000'); }
// 颜色字符串/数字 → [r,g,b,a] 0..1（供渐变 stop 用）
function __colorRGBA(c){
  if (typeof c==='number'){ return [((c>>16)&255)/255,((c>>8)&255)/255,(c&255)/255,1]; }
  var s=(''+c).trim();
  if (s.charAt(0)==='#'){ var h=s.slice(1); if(h.length===3)h=h.charAt(0)+h.charAt(0)+h.charAt(1)+h.charAt(1)+h.charAt(2)+h.charAt(2);
    var n=parseInt(h,16); if(h.length===6)return[((n>>16)&255)/255,((n>>8)&255)/255,(n&255)/255,1];
    if(h.length===8)return[((n>>>24)&255)/255,((n>>16)&255)/255,((n>>8)&255)/255,(n&255)/255]; }
  var m=s.match(/rgba?\(([^)]+)\)/i);
  if(m){ var p=m[1].split(',').map(function(t){return parseFloat(t);}); return [(p[0]||0)/255,(p[1]||0)/255,(p[2]||0)/255, p[3]==null?1:p[3]]; }
  return [0,0,0,1];
}
// 渐变对象 → 代表性纯色(取末个 stop，radial/异常兜底用，避免黑)
function __gradSolid(g){ var s=g&&g.stops; if(!s||s.length<5) return '#ffffff'; var i=s.length-4;
  function h2(v){ return ('0'+Math.round(v*255).toString(16)).slice(-2); }
  return '#'+h2(s[i])+h2(s[i+1])+h2(s[i+2]); }
function make2D(cv){
  var stateStack=[];
  var stateKeys=['font','fillStyle','strokeStyle','lineWidth','textBaseline','textAlign','lineJoin','miterLimit','globalAlpha',
    'shadowColor','shadowBlur','shadowOffsetX','shadowOffsetY','letterSpacing','textLetterSpacing'];
  var ctx = {
    canvas: cv,
    font:'10px sans-serif', fillStyle:'#000', strokeStyle:'#000', lineWidth:1,
    textBaseline:'alphabetic', textAlign:'left', lineJoin:'miter', miterLimit:10, globalAlpha:1,
    shadowColor:'rgba(0,0,0,0)', shadowBlur:0, shadowOffsetX:0, shadowOffsetY:0,
    measureText:function(s){ return { width: __c2dMeasure(cv.__ensureCid(), ''+s, this.font) }; },
    fillText:function(s,x,y){
      if(globalThis.__pfCnt) globalThis.__pfCnt.text++;   // 文本真实重绘计数(badframe 快照)
      var fs=this.fillStyle;
      if (fs && fs.__grad===1 && fs.stops.length>=10) {   // 线性渐变 fill(pixi Text fill 数组)→ 原生裁字形+CGGradient
        __c2dFillTextGrad(cv.__ensureCid(), ''+s, x, y, this.font, fs.x0,fs.y0,fs.x1,fs.y1, JSON.stringify(fs.stops),
          this.textBaseline, this.globalAlpha, this.textAlign, css(this.shadowColor), this.shadowBlur, this.shadowOffsetX, this.shadowOffsetY);
      } else {
        __c2dFillText(cv.__ensureCid(), ''+s, x, y, this.font, (fs&&fs.__grad)?__gradSolid(fs):css(fs),
          this.textBaseline, this.globalAlpha, this.textAlign, css(this.shadowColor), this.shadowBlur, this.shadowOffsetX, this.shadowOffsetY);
      }
    },
    strokeText:function(s,x,y){ __c2dStrokeText(cv.__ensureCid(), ''+s, x, y, this.font, css(this.strokeStyle), this.lineWidth,
      this.textBaseline, this.globalAlpha, this.textAlign, this.lineJoin, this.miterLimit,
      css(this.shadowColor), this.shadowBlur, this.shadowOffsetX, this.shadowOffsetY); },
    fillRect:function(x,y,w,h){ __c2dFillRect(cv.__ensureCid(), x,y,w,h, css(this.fillStyle), this.globalAlpha); },
    clearRect:function(x,y,w,h){ __c2dClearRect(cv.__ensureCid(), x,y,w,h); },
    getImageData:function(x,y,w,h){ var d=__c2dGetData(cv.__ensureCid()); return { data:new Uint8ClampedArray(d), width:cv._w, height:cv._h }; },
    save:function(){
      var state={}; for(var i=0;i<stateKeys.length;i++) state[stateKeys[i]]=this[stateKeys[i]];
      stateStack.push(state); __c2dSave(cv.__ensureCid());
    },
    restore:function(){
      if(!stateStack.length) return;
      var state=stateStack.pop(); for(var i=0;i<stateKeys.length;i++) this[stateKeys[i]]=state[stateKeys[i]];
      __c2dRestore(cv.__ensureCid());
    },
    scale:function(x,y){ __c2dScale(cv.__ensureCid(), +x, +y); },
    translate:function(x,y){ __c2dTranslate(cv.__ensureCid(), +x, +y); },
    rotate:function(a){ __c2dRotate(cv.__ensureCid(), +a); },
    transform:function(a,b,c,d,e,f){ __c2dTransform(cv.__ensureCid(), +a,+b,+c,+d,+e,+f); },
    setTransform:function(a,b,c,d,e,f){
      if(arguments.length===1 && a && typeof a==='object') __c2dSetTransform(cv.__ensureCid(), +a.a,+a.b,+a.c,+a.d,+a.e,+a.f);
      else __c2dSetTransform(cv.__ensureCid(), +a,+b,+c,+d,+e,+f);
    },
    resetTransform:function(){ __c2dResetTransform(cv.__ensureCid()); },
    beginPath:function(){}, closePath:function(){}, moveTo:function(){}, lineTo:function(){}, rect:function(){}, fill:function(){}, stroke:function(){}, clip:function(){}, arc:function(){}, arcTo:function(){}, quadraticCurveTo:function(){}, bezierCurveTo:function(){},
    drawImage:function(){},
    // pixi Extract.canvas 把 readPixels 像素(straight alpha)写进 canvas → toDataURL 编码;native 端预乘入 CG buf
    putImageData:function(img,dx,dy){
      if (!img || !img.data || typeof __c2dPutData!=='function') return;
      var d = img.data, ab = (!d.byteOffset && d.byteLength===d.buffer.byteLength) ? d.buffer : d.buffer.slice(d.byteOffset, d.byteOffset+d.byteLength);
      __c2dPutData(cv.__ensureCid(), ab, img.width|0, img.height|0, dx|0, dy|0);
    },
    createImageData:function(w,h){ return new ImageData(w|0||1, h|0||1); },
    createLinearGradient:function(x0,y0,x1,y1){ return { __grad:1, x0:x0,y0:y0,x1:x1,y1:y1, stops:[], addColorStop:function(o,c){ var r=__colorRGBA(c); this.stops.push(o,r[0],r[1],r[2],r[3]); } }; },
    createRadialGradient:function(x0,y0,r0,x1,y1,r1){ return { __grad:2, stops:[], addColorStop:function(o,c){ var r=__colorRGBA(c); this.stops.push(o,r[0],r[1],r[2],r[3]); } }; },
    createPattern:function(){ return null; }
  };
  return ctx;
}
var WebGLRCStub = function(){}; for (var _k in K) WebGLRCStub[_k] = K[_k]; WebGLRCStub.prototype = K;
