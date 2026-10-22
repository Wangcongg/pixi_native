// ===== native 纹理所有权协议(publish → adopt/cancel → release)=====
// Native 在把 cid 交给 __imgDone 之前已为它上账一个软引用(refs+1/adoptRefs+1)。JS 收到后
// 二选一:adopt(引用转归 Image 持有)或 cancel(token 失配,立即退还)。adopt 后的对称收尾
// 两条显式路,谁先到谁执行(owned 标志保证恰好一次):
//   1) 首次 glTexture 绑定 __nativeImgTransfer(软转硬);2) Image 换 src / BaseImageResource
//      .dispose(BaseTexture 销毁,见 native_scenegraph.js)→ __nativeImgDisown 退还。
// 显式路都没走到(Image 被 GC)的软引用不退——无兜底也无害:软引用不参与驱逐决策
// (refs==adoptRefs 恒可驱逐),条目照常被预算回收,计数随条目消失;曾试过
// FinalizationRegistry 兜底,JSC 嵌入环境实测从不触发(imgFinalizes 恒 0),已删。
// Pixi TextureSystem 的 retain/release(native_scenegraph.js)是叠加在这层之上的独立硬引用,
// 二者互不认账:adopt 引用管"Image 活着就能随时重挂纹理",Pixi 引用管"glTexture 正在用"。
function __imgAdopt(img, cid){
  img.__nativeTextureId = cid; img.__nativeTextureOwned = 1;
  globalThis.__nativeImgAdopts = (globalThis.__nativeImgAdopts||0)+1;
}
function __imgCancel(cid){   // 发布出的引用没人要(token 失配/加载失败):立即退还(软引用口)
  if (!(cid > 0)) return;
  var rel = globalThis.__nativeResourceReleaseAdopt || globalThis.__nativeResourceRelease;   // 旧宿主回退普通口
  if (typeof rel !== 'function') return;
  try{ rel(cid|0); globalThis.__nativeImgCancels=(globalThis.__nativeImgCancels||0)+1; }catch(e){}
}
globalThis.__nativeImgTransfer = function(img){   // adopt 引用移交给 Pixi glTexture(不 release):
  // 首次绑定时调用。此后 GPU 生命周期完全由 glTexture 的 retain/release + 预算回收决定——
  // TextureGC destroyTexture 之后 refs 归零,闲置图能被 384MB 预算回收(浏览器式闭环);
  // Image 只保留软 cid,复绑时 cid 若已被回收走 stale reload 重新加载。
  // 若不转交而是叠加 retain,Image(常驻 Assets 缓存)会把 refs 钉在 ≥1,预算回收永远失效。
  // native 侧同步记账(软→硬,__nativeResourceTransferred):"从未绑定"的预载图 refs 全软,
  // 过保护期允许预算驱逐;native 返回 false = 条目已被驱逐 → 本次转交失败,调用方走
  // retain(也会 miss)→ stale reload 重建。旧宿主无此桥时按成功处理(行为=硬引用钉住)。
  if(!img||!img.__nativeTextureOwned) return false;
  var cid = img.__nativeTextureId|0;
  if (cid > 0 && typeof globalThis.__nativeResourceTransferred === 'function'){
    var ok = false;
    try{ ok = !!__nativeResourceTransferred(cid); }catch(e){}
    if (!ok){   // 条目已被预算驱逐:owned 标志作废,引用无从移交
      img.__nativeTextureOwned = 0;
      return false;
    }
  }
  img.__nativeTextureOwned = 0;
  globalThis.__nativeImgTransfers=(globalThis.__nativeImgTransfers||0)+1;
  return true;
};
globalThis.__nativeImgDisown = function(img){   // adopt 过的 Image 主动放弃(换 src/dispose)——软引用口
  if (!img || !img.__nativeTextureOwned) return;
  var cid = img.__nativeTextureId|0;
  img.__nativeTextureOwned = 0;
  if (!(cid > 0)) return;
  var rel = globalThis.__nativeResourceReleaseAdopt || globalThis.__nativeResourceRelease;   // 旧宿主回退普通口
  if (typeof rel !== 'function') return;
  try{ rel(cid); globalThis.__nativeImgDisowns=(globalThis.__nativeImgDisowns||0)+1; }catch(e){}
};
function __imgFire(img, ok, srcForErr){
  var __texFireStart=(img&&img.__nativeTextureParserUrl&&typeof globalThis.__nativeTextureFinalizeImageFired==='function')?performance.now():0;
  if (ok) {
    if (img.onload) img.onload();
    (img._lst['load'] || []).forEach(function(f){ try{ f({type:'load',target:img}); }catch(e){} });
  } else {
    var err = new Error('load fail: ' + srcForErr);
    if(globalThis.__NATIVE_IMAGE_ERROR_ORIGINS&&img.__nativeImageOrigin&&typeof globalThis.__log==='function')
      globalThis.__log('[native-image-error-origin] '+img.__nativeImageOrigin);
    if (img.onerror) img.onerror(err);
    (img._lst['error'] || []).forEach(function(f){ try{ f(err); }catch(e){} });
  }
  if(__texFireStart)try{globalThis.__nativeTextureFinalizeImageFired(img,ok,__texFireStart,performance.now());}catch(e){}
}
globalThis.__imgDone = function(tk, w, h, data, nativeCid, nativeNeedsPremultiply){   // native 后台加载完成回调
  var img = __imgPending[tk]; delete __imgPending[tk];
  if (!img || img.__tk !== tk) { __imgCancel(nativeCid); return; }   // 实例已换 src / 上下文已 reload:退还发布引用
  if(img.__nativeTextureParserUrl)img.__nativeTextureReadyAt=performance.now();
  var fin = function(){
    if (img.__tk !== tk) { __imgCancel(nativeCid); return; }   // 分帧等待期间换过 src:退还
    var ok = !!((data || nativeCid > 0) && w > 0);
    if (ok) {
      img.width = w; img.height = h; img.__data = data || null;
      // Requested URL may still say .webp while native format fallback actually supplied KTX2.
      // Preserve the KTX DFD straight-alpha flag on the Image so both Pixi and SceneGraph choose
      // the non-premultiplied texture path instead of exposing transparent-edge RGB as a matte.
      img.__nativeNeedsPremultiply = !!nativeNeedsPremultiply;
      if (nativeCid > 0) __imgAdopt(img, nativeCid|0);   // 发布引用转归 Image 持有(换src/dispose/GC 时对称释放)
      else img.__nativeTextureId = 0;
      img.complete = true;
    } else __imgCancel(nativeCid);   // cid 有效但尺寸非法等:不 adopt 就必须退还
    __imgFire(img, ok, img.src);
  };
  if (globalThis.__IO_PACE === 0){ fin(); return; }
  __ioQ.push(fin);   // 分帧派发(见 __ioDrain,loading 期帧率保护)
};
globalThis.Image = function(){
  var self = this;
  // Captured synchronously by the Pixi texture-loader bridge, before Image.src
  // starts native decoding. Keep it on the Image for eviction/reload as well.
  self.__nativeSourcePremultiplied = globalThis.__nativeImageSourcePremultiplied === true;
  self.onload = null; self.onerror = null; self.complete = false; self.width = 0; self.height = 0;
  self.crossOrigin = null; self.__data = null; self.__nativeTextureId = 0; self.__nativeTextureOwned = 0;
  self.__nativeNeedsPremultiply = false; self._lst = {};
  // loadTextures.load() is async but constructs and assigns Image.src before its first await.
  // The parser wrapper publishes this tag only for that synchronous window, keeping unrelated
  // Spine/DOM image traffic out of the finalize profile.
  if(globalThis.__nativeTextureParserUrl)self.__nativeTextureParserUrl=globalThis.__nativeTextureParserUrl;
  self.addEventListener = function(t,f){ (self._lst[t] = self._lst[t] || []).push(f); };
  self.removeEventListener = function(t,f){
    var listeners=self._lst[t],index=listeners?listeners.indexOf(f):-1;
    if(index>=0)listeners.splice(index,1);
  };
  self.decode = function(){ return Promise.resolve(); };
  var _src = '';
  Object.defineProperty(self, 'src', { get:function(){ return _src; }, set:function(v){
    _src = v;
    if(globalThis.__NATIVE_IMAGE_ERROR_ORIGINS)
      self.__nativeImageOrigin=/^https?:\/\//i.test(String(v))?String(new Error('external image assigned').stack||''):'';
    self.complete = false;
    if (self.__nativeTextureOwned) globalThis.__nativeImgDisown(self);   // 旧图作废:释放 adopt 引用
    self.__data = null; self.__nativeTextureId = 0; self.__nativeNeedsPremultiply = false;
    if(self.__nativeTextureParserUrl)self.__nativeTextureLoadStarted=performance.now();
    // 每次赋 src 都换 token，包括同步 fallback；旧的 native/定时回调因此无法覆盖新图。
    if(self.__tk)delete __imgPending[self.__tk];
    var tk = ++__imgTk; self.__tk = tk;
    // Pixi disposes images by clearing src. Invalidate the old request and ownership,
    // but never send an empty URL to native I/O/decode. Keep the asynchronous error
    // event for callers which explicitly listen for invalid image sources.
    if(v===''){
      self.complete=true;self.width=0;self.height=0;
      globalThis.setTimeout(function(){if(self.__tk===tk)__imgFire(self,false,'');},0);
      return;
    }
    var blobBytes = v && typeof v==='string' && v.indexOf('blob:pn/')===0 ? __blobReg[v] : null;
    // Native-prepared zspine image: keep the compressed PNG entirely outside
    // JSC. ResourceManager reads the prepared entry and returns only a GPU cid.
    if(blobBytes&&blobBytes.__nativeSpineHandle>0&&blobBytes.__nativeSpineEntry&&typeof __loadSpinePreparedImage==='function'){
      __imgPending[tk]=self;
      try{if(__loadSpinePreparedImage(blobBytes.__nativeSpineHandle,String(blobBytes.__nativeSpineEntry),tk)!==false)return;}catch(e){if(globalThis.__BLOBDBG)__log('[blobdbg] prepared spine image fallback: '+e.message);}
      delete __imgPending[tk];
    }
    // ZIP/.zspine 里的 PNG/JPEG：压缩字节立即复制到 Native，ImageIO 解码在 assetQueue。
    if (blobBytes && typeof __decodeImageAsync === 'function') {
      __imgPending[tk] = self;
      if(globalThis.__BLOBDBG)__log('[blobdbg] Image.src '+v+' → __decodeImageAsync');
      try {
        if (__decodeImageAsync(tk, blobBytes, self.__nativeSourcePremultiplied) !== false) return;
      } catch(e) {
        if(globalThis.__BLOBDBG)__log('[blobdbg] __decodeImageAsync fallback: '+e.message);
      }
      delete __imgPending[tk];
    }
    // URL 异步路径:后台拉取+解码,__imgDone 回调设置属性并派发事件。
    if (!blobBytes && typeof __loadImageAsync === 'function') {
      __imgPending[tk] = self;
      __loadImageAsync(String(v), tk, self.__nativeSourcePremultiplied);
      return;
    }
    var r = null;
    if (blobBytes && typeof __decodeImage === 'function') {
      if(globalThis.__BLOBDBG)__log('[blobdbg] Image.src '+v+' → __decodeImage');
      r = __decodeImage(blobBytes, self.__nativeSourcePremultiplied);
    } else {
      r = (typeof __loadImage === 'function') ? __loadImage(v, self.__nativeSourcePremultiplied) : null;
    }
    var ok = !!(r && r.width);
    if (ok) { self.width = r.width; self.height = r.height; self.__data = r.data; self.complete = true; }
    if(self.__nativeTextureParserUrl)self.__nativeTextureReadyAt=performance.now();
    // 事件异步派发：浏览器中图片加载是异步的，调用方常先 img.src=URL 再 addEventListener('load')。
    // 若同步派发，src= 时监听器尚未注册 → 永久挂起（fgui/spine 的 ZipUtils.getHTMLImage 即此模式）。
    globalThis.setTimeout(function(){ if(self.__tk===tk)__imgFire(self, ok, v); }, 0);
  }});
};
globalThis.HTMLImageElement = globalThis.Image;   // pixi 用 instanceof 探测资源类型
globalThis.HTMLCanvasElement = function(){};
