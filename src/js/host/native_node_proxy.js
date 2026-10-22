// Lightweight JS facade for nodes rendered and transformed by the native scene graph.
// It deliberately does not inherit PIXI.DisplayObject.  It is nevertheless duck-typed closely enough
// to sit in PIXI.Container.children, so FGUI can preserve child order/lifecycle without retaining a
// heavyweight Pixi Sprite/Container for every native-only leaf.
(function(){
'use strict';
var R=globalThis.__nativeSceneRuntime,PIXI=globalThis.PIXI;
if(!R||!PIXI)return;
var OriginalText=PIXI.Text,OriginalContainer=PIXI.Container,OriginalSprite=PIXI.Sprite,
    OriginalGraphics=PIXI.Graphics,OriginalParticleContainer=PIXI.ParticleContainer,OriginalApplication=PIXI.Application,
    OriginalTilingSprite=PIXI.TilingSprite,OriginalAnimatedSprite=PIXI.AnimatedSprite,OriginalNineSlicePlane=PIXI.NineSlicePlane,
    OriginalMesh=PIXI.Mesh,OriginalSimpleMesh=PIXI.SimpleMesh,OriginalSimplePlane=PIXI.SimplePlane,OriginalSimpleRope=PIXI.SimpleRope;
// Keep the original constructor private to compatibility/audit code.  Business code sees the
// lightweight replacement below; the coverage gate uses this reference to reject any old Pixi
// Text instance that was constructed before the replacement was installed.
globalThis.__NativeOriginalPixiText=OriginalText;
var B=R.buffer,ST=R.stride;
var X=0,Y=1,SX=2,SY=3,ROT=4,PVX=5,PVY=6,SKX=7,SKY=8,ALPHA=9,VISIBLE=10,TYPE=11;
var WA=12,WB=13,WC=14,WD=15,WTX=16,WTY=17,WALPHA=18,TEXID=26,BLEND=27,RENDERABLE=28;
var RX=29,RY=30,RW=31,RH=32,U0=33,TINT=41,PREMUL=44;

// The native FGUI factory has already allocated and fully populated every authored node.  A
// semantic JS facade used to allocate a second scene slot in its constructor, initialise that
// temporary descriptor, then free it again in R.adopt().  Keep a single synchronous one-shot
// construction context instead: the first NativeNodeProxy binds directly to the existing slot and
// all constructor-default descriptor writes are suppressed until the owning GObject constructor
// returns.  Native FGUI exposes this only for a conservative whitelist; arbitrary Pixi/business
// constructors never enter the context.
var pendingFacadeAdopt=null;
function consumePendingFacadeAdopt(o){
  // blank 槽(批量 __sgAllocBatch 预留的空槽)必须放行构造器的 descriptor 写入,否则批量
  // sprite 的 texture/TYPE 全被抑制且事后无人回放 → TYPE 停在容器,整批不可见。
  // 只有 FGUI 领养(C++ 已填权威 descriptor)才需要抑制。
  var p=pendingFacadeAdopt;if(!p||p.consumed||!o)return 0;p.consumed=true;p.proxy=o;o.__i=p.id;o.__nativePreserveDescriptor=!p.blank;R.objects[p.id]=o;proxyStructRev++;if(R.invalidateParent)R.invalidateParent(p.id);var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.fguiDirectSlotFacades=(st.fguiDirectSlotFacades||0)+1;return p.id;
}
R.beginFacadeAdopt=function(id,blank){
  id=id|0;if(id<=0||pendingFacadeAdopt||!R.objects||R.objects[id])return false;var snapshot=null;if(!blank&&globalThis.__NATIVE_FGUI_ADOPT_VERIFY===1){snapshot=new Float32Array(ST);snapshot.set(B.subarray(id*ST,id*ST+ST));}pendingFacadeAdopt={id:id,previous:R.objects[id],consumed:false,proxy:null,snapshot:snapshot,blank:!!blank};return true;
};
R.finishFacadeAdopt=function(display,commit){
  var p=pendingFacadeAdopt;if(!p)return false;pendingFacadeAdopt=null;var proxy=p.proxy,ok=!!(commit&&p.consumed&&proxy&&proxy===display&&(proxy.__i|0)===p.id);
  if(p.snapshot){var base=p.id*ST,changed=0;for(var si=0;si<ST;si++)if(!Object.is(B[base+si],p.snapshot[si]))changed++;var verifyStats=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});verifyStats.fguiDirectSlotDescriptorsVerified=(verifyStats.fguiDirectSlotDescriptorsVerified||0)+1;if(changed){verifyStats.fguiDirectSlotDescriptorMismatches=(verifyStats.fguiDirectSlotDescriptorMismatches||0)+changed;B.set(p.snapshot,base);R.markLocal(p.id);R.markBounds();if((verifyStats.fguiDirectSlotDescriptorWarnings||0)<4&&typeof __log==='function'){verifyStats.fguiDirectSlotDescriptorWarnings=(verifyStats.fguiDirectSlotDescriptorWarnings||0)+1;__log('[native-fgui] direct facade constructor changed '+changed+' descriptor fields for #'+p.id+'; restored authoritative native data');}}}
  if(proxy){proxy.__nativePreserveDescriptor=false;if(!ok){if(R.objects[p.id]===proxy){R.objects[p.id]=p.previous;proxyStructRev++;}proxy.__i=undefined;}}
  return ok;
};
function preservingNativeDescriptor(o){return !!(o&&o.__nativePreserveDescriptor);}

function nativePointChanged(){R.markLocal(this.__i);invalidatePackedOwner(this);}
function NativePoint(owner,ox,oy,dx,dy){this.owner=owner;this.ox=ox;this.oy=oy;this.dx=dx;this.dy=dy;this.cb=nativePointChanged;this.scope=owner;}
Object.defineProperties(NativePoint.prototype,{
  x:{get:function(){return B[this.owner.__i*ST+this.ox]-(this.__offsetX||0);},set:function(v){this._write(this.ox,Number(v)||0);}},
  y:{get:function(){return B[this.owner.__i*ST+this.oy]-(this.__offsetY||0);},set:function(v){this._write(this.oy,Number(v)||0);}},
  _x:{get:function(){return this.x;},set:function(v){this.x=v;}},
  _y:{get:function(){return this.y;},set:function(v){this.y=v;}}
});
function notifyNativeFguiDisplayPosition(owner,ox,oy){
  // Native FGUI keeps an authoritative semantic layout model in C++. A direct Pixi mutation
  // (`gObject.displayObject.x/y` or `displayListContainer.position.set`) used to update only the
  // shared SceneGraph slot. The next relation/controller/resize write then republished the stale
  // semantic coordinates and made complete components jump back to their authored position.
  // Keep the generic proxy independent from FGUI itself: native_fgui installs this optional hook
  // and filters out ordinary GObject.handleXYChanged writes.
  if((ox!==X&&oy!==Y)||!owner||!owner.__nativeFguiFacade||typeof globalThis.__nativeFguiDisplayPositionDidChange!=='function')return;
  var b=owner.__i*ST;
  return globalThis.__nativeFguiDisplayPositionDidChange(owner,B[b+X],B[b+Y]);
}
function notifyNativeFguiDisplayScale(owner,ox,oy){
  // A raw displayObject scale is a Pixi transform override, not GObject.setScale. Preserve it in
  // Native FGUI's authoritative node model so a later x/y/visible publication in the same GSAP
  // frame cannot overwrite the scale with the authored semantic value.
  if((ox!==SX&&oy!==SY)||!owner||!owner.__nativeFguiFacade||typeof globalThis.__nativeFguiDisplayScaleDidChange!=='function')return;
  var b=owner.__i*ST;
  globalThis.__nativeFguiDisplayScaleDidChange(owner,B[b+SX],B[b+SY]);
}
function notifyNativeFguiDisplayRotation(owner,rotation){
  // Raw Pixi rotation/angle is a display-layer transform, just like raw position and scale.
  // Native FGUI republishes complete semantic transforms after controller/relation/facade work,
  // so the raw value must be retained by its authoritative node instead of living only in B.
  if(!owner||!owner.__nativeFguiFacade||typeof globalThis.__nativeFguiDisplayRotationDidChange!=='function')return false;
  try{return globalThis.__nativeFguiDisplayRotationDidChange(owner,rotation)===true;}catch(e){return false;}
}
function notifyNativeFguiDisplayAlpha(owner,alpha){
  // Raw `gObject.displayObject.alpha` is a Pixi-level override. Native FGUI owns a parallel C++
  // descriptor and may republish it on any later relation/controller write; updating only the
  // shared SceneGraph slot therefore lasts less than one frame. Publish the same value into that
  // authoritative model, just as direct position/scale/visibility already do.
  if(!owner||!owner.__nativeFguiFacade||typeof globalThis.__nativeFguiDisplayAlphaDidChange!=='function')return false;
  try{return globalThis.__nativeFguiDisplayAlphaDidChange(owner,alpha)===true;}catch(e){return false;}
}
function notifyNativeFguiDisplayVisibility(owner,visible){
  if(!owner||!owner.__nativeFguiFacade||typeof globalThis.__nativeFguiDisplayVisibilityDidChange!=='function')return false;
  try{return globalThis.__nativeFguiDisplayVisibilityDidChange(owner,!!visible)===true;}catch(e){return false;}
}
NativePoint.prototype._write=function(off,v){if(preservingNativeDescriptor(this.owner))return;var i=this.owner.__i,b=i*ST;v=Math.fround(v+(off===this.ox?(this.__offsetX||0):(this.__offsetY||0)));if(B[b+off]===v)return;B[b+off]=v;R.markLocal(i);invalidatePackedOwner(this.owner);notifyNativeFguiDisplayPosition(this.owner,off,off);notifyNativeFguiDisplayScale(this.owner,off,off);};
NativePoint.prototype.set=function(x,y){if(y===undefined)y=x;if(preservingNativeDescriptor(this.owner))return this;var i=this.owner.__i,b=i*ST,xv=Math.fround(Number(x)),yv=Math.fround(Number(y));if(!isFinite(xv))xv=this.dx;if(!isFinite(yv))yv=this.dy;xv=Math.fround(xv+(this.__offsetX||0));yv=Math.fround(yv+(this.__offsetY||0));if(B[b+this.ox]!==xv||B[b+this.oy]!==yv){B[b+this.ox]=xv;B[b+this.oy]=yv;R.markLocal(i);invalidatePackedOwner(this.owner);notifyNativeFguiDisplayPosition(this.owner,this.ox,this.oy);notifyNativeFguiDisplayScale(this.owner,this.ox,this.oy);}return this;};
NativePoint.prototype.copyFrom=function(p){return this.set(p.x,p.y);};
NativePoint.prototype.copyTo=function(p){p.x=this.x;p.y=this.y;return p;};
// A clone owns its coordinates; it must never alias the native transform buffer.
// Retain ObservablePoint's callback/scope override contract for existing Pixi callers.
NativePoint.prototype.clone=function(cb,scope){return new PIXI.ObservablePoint(cb===undefined?this.cb:cb,scope===undefined?this.scope:scope,this.x,this.y);};
NativePoint.prototype.equals=function(p){return p.x===this.x&&p.y===this.y;};

function NativeAnchor(owner,x,y){this.owner=owner;this._x=Number(x)||0;this._y=Number(y)||0;}
Object.defineProperties(NativeAnchor.prototype,{
  x:{get:function(){return this._x;},set:function(v){this.set(v,this._y);}},
  y:{get:function(){return this._y;},set:function(v){this.set(this._x,v);}}
});
NativeAnchor.prototype.set=function(x,y){if(y===undefined)y=x;x=Number(x)||0;y=Number(y)||0;if(this._x===x&&this._y===y)return this;this._x=x;this._y=y;var o=this.owner;if(preservingNativeDescriptor(o))return this;if(o&&o.__nativeTextProxy&&o._syncNativeText)o._syncNativeText(true);else if(o&&o._refreshSprite)o._refreshSprite();if(o&&o.__i>0)R.markBounds();invalidatePackedOwner(o);return this;};
NativeAnchor.prototype.copyFrom=function(p){return this.set(p&&p.x,p&&p.y);};
NativeAnchor.prototype.copyTo=function(p){p.x=this._x;p.y=this._y;return p;};
NativeAnchor.prototype.equals=function(p){return !!p&&p.x===this._x&&p.y===this._y;};
NativeAnchor.prototype.clone=function(cb,scope){return new PIXI.ObservablePoint(cb===undefined?function(){var o=this.owner;if(o&&o._syncNativeText)o._syncNativeText(true);else if(o&&o._refreshSprite)o._refreshSprite();if(o&&o.__i>0)R.markBounds();invalidatePackedOwner(o);}:cb,scope===undefined?this:scope,this._x,this._y);};

function NativeWorldMatrix(owner){this.owner=owner;this.__nativeSharedMatrix=true;}
['a','b','c','d','tx','ty'].forEach(function(name,k){Object.defineProperty(NativeWorldMatrix.prototype,name,{get:function(){return B[this.owner.__i*ST+WA+k];}});});
NativeWorldMatrix.prototype.apply=function(pos,out){out=out||new PIXI.Point();var b=this.owner.__i*ST,x=pos.x,y=pos.y;out.x=B[b+WA]*x+B[b+WC]*y+B[b+WTX];out.y=B[b+WB]*x+B[b+WD]*y+B[b+WTY];return out;};
NativeWorldMatrix.prototype.applyInverse=function(pos,out){out=out||new PIXI.Point();var b=this.owner.__i*ST,a=B[b+WA],bb=B[b+WB],c=B[b+WC],d=B[b+WD],id=1/(a*d-bb*c),x=pos.x-B[b+WTX],y=pos.y-B[b+WTY];out.x=(d*x-c*y)*id;out.y=(-bb*x+a*y)*id;return out;};

// Read-only world snapshots use Pixi's own Matrix serialization and decomposition.
['clone','copyTo','toArray','decompose'].forEach(function(name){NativeWorldMatrix.prototype[name]=PIXI.Matrix.prototype[name];});

function updateLocal(owner){var b=owner.__i*ST,r=B[b+ROT],kx=B[b+SKX],ky=B[b+SKY],sx=B[b+SX],sy=B[b+SY],a=Math.cos(r+ky)*sx,bb=Math.sin(r+ky)*sx,c=-Math.sin(r-kx)*sy,d=Math.cos(r-kx)*sy,pvx=B[b+PVX],pvy=B[b+PVY],m=owner.transform.localTransform;
  m.a=a;m.b=bb;m.c=c;m.d=d;m.tx=B[b+X]-(pvx*a+pvy*c);m.ty=B[b+Y]-(pvx*bb+pvy*d);return m;}

function tintNumber(v){if(typeof v==='number'&&isFinite(v))return v&0xFFFFFF;try{return PIXI.Color.shared.setValue(v).toNumber()&0xFFFFFF;}catch(e){return 0xFFFFFF;}}
// A Native node is never allowed to reconstruct a temporary Pixi object for an offscreen pass.
// Renderer.render routes RenderTexture work through the same C++ scene tree as the main screen;
// reaching one of these compatibility methods is therefore a coverage failure, not a fallback.
function rejectPixiRender(kind,o){
  var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.rejectedPixiRenders=(st.rejectedPixiRenders||0)+1;
  var samples=st.rejectedPixiRenderSamples||(st.rejectedPixiRenderSamples=[]);if(samples.length<16)samples.push({kind:String(kind||'node'),id:o&&o.__i|0,name:String(o&&o.name||''),type:String(o&&o.constructor&&o.constructor.name||'')});
  if(st.rejectedPixiRenders<=4&&typeof __log==='function')__log('[native-only] blocked Pixi render kind='+String(kind||'node')+' id='+(o&&o.__i|0));
}
globalThis.__nativeRejectPixiRender=rejectPixiRender;
function invalidatePackedOwner(o){var p=o&&o.parent;if(p&&p.__nativeParticleContainer&&R.markVisual)R.markVisual(p);}
function nativeMutationReadBarrier(){if(typeof globalThis.__nativeFguiMutationReadBarrier==='function')globalThis.__nativeFguiMutationReadBarrier();else if(typeof globalThis.__sgBatchReadBarrier==='function')globalThis.__sgBatchReadBarrier();}

// Structure revision for the children-view cache.
// 【教训】原来只包 R.setParent/R.alloc/R.free/R.adopt 四个入口,**漏了 R.remove**——而
// removeChild 走的正是 R.remove。于是 C++ 树里子节点已摘除、JS 快照却还留着它们,
// ParticleContainer 的打包器继续按旧快照生成 Quad,发射结束后再无结构变化触发刷新,
// 最后一帧粒子就永久停在屏幕上(还会挡住上层弹窗的点击)。
// 修法不是再补一个 API,而是不再由本模块枚举"哪些算结构变化":SceneGraph 侧的
// __sgStructVer 覆盖 alloc/free/setParent/remove 全部入口(含批处理队列),这里直接采信它;
// 本地 rev 只保留 R.adopt 这类"槽位归属变化但树形没变"的情形,两者相加即缓存键。
var proxyStructRev=1;
function structRev(){ var v=globalThis.__sgStructVer; return proxyStructRev+(typeof v==='number'?v:0); }
if(!R.__structRevHooked){
  R.__structRevHooked=true;
  (function(){var sp=R.setParent,al=R.alloc,fr=R.free,ad=R.adopt;
    if(typeof sp==='function')R.setParent=function(){proxyStructRev++;return sp.apply(R,arguments);};
    if(typeof al==='function')R.alloc=function(){proxyStructRev++;return al.apply(R,arguments);};
    if(typeof fr==='function')R.free=function(){proxyStructRev++;return fr.apply(R,arguments);};
    if(typeof ad==='function')R.adopt=function(){proxyStructRev++;return ad.apply(R,arguments);};
  })();
}
function proxyChildSnapshot(owner){
  nativeMutationReadBarrier();
  var id=owner&&owner.__i|0;if(!(id>0))return [];
  // Cache per owner keyed by the structure revision: business loops like
  // `for(i<children.length) children[i]` were one bridge crossing + array allocation PER ACCESS
  // (O(n²) per frame for particle containers). Same-revision reads reuse the array.
  var cached=owner.__childSnap;
  var rev=structRev();
  if(cached&&owner.__childSnapRev===rev)return cached;
  var out=[],ids=null;
  if(typeof globalThis.__sgNativeChildren==='function')ids=__sgNativeChildren(id);
  else if(typeof globalThis.__sgNativeChildCount==='function'&&typeof globalThis.__sgNativeChildAt==='function'){ids=[];var count=__sgNativeChildCount(id)|0;for(var i=0;i<count;i++)ids.push(__sgNativeChildAt(id,i)|0);}
  if(ids)for(var j=0;j<ids.length;j++){var childId=ids[j]|0,c=childId>0&&R.objects?R.objects[childId]||null:null;if(c&&c.__i===childId&&!c._destroyed)out.push(c);}
  owner.__childSnap=out;owner.__childSnapRev=rev;
  return out;
}
function proxyChildAt(owner,index){var a=proxyChildSnapshot(owner);return index>=0&&index<a.length?a[index]:null;}
function proxyChildIndex(owner,child){if(!child||!(child.__i>0))return-1;var a=proxyChildSnapshot(owner),id=child.__i|0;for(var i=0;i<a.length;i++)if((a[i].__i|0)===id)return i;return-1;}
function proxyChildCount(owner){return proxyChildSnapshot(owner).length;}
function fguiSemanticBefore(owner,child){
  var ui=owner&&owner.UIOwner,items=ui&&ui.$children,target=child&&child.UIOwner;
  if(!items||!target)return null;var at=items.indexOf(target);if(at<0)return null;
  // FGUI's childStateChanged index counts business children whose displayObject is currently
  // attached. Native static leaves may intentionally have no JS facade, so that semantic index
  // is not comparable with the facade count. Find the next attached business sibling and use its
  // numeric node as the authoritative `before` marker instead.
  for(var i=at+1;i<items.length;i++){
    var display=items[i]&&items[i].displayObject;
    if(display&&display.parent===owner&&display.__i>0&&proxyChildIndex(owner,display)>=0)return display.__i|0;
  }
  return -1;
}
function syncFacadeOrder(owner,list){
  // A Native parent can contain authored nodes that intentionally have no JS facade. Re-appending
  // only the visible facade subset moves every such authored surface in front of it. Permute the
  // known ids inside their existing Native slots and replay the complete order instead.
  var full=typeof globalThis.__sgNativeChildren==='function'?__sgNativeChildren(owner.__i|0):null;
  if(full&&full.length){
    var known=Object.create(null),ordered=[];for(var i=0;i<list.length;i++){var id=list[i]&&list[i].__i|0;if(id>0){known[id]=1;ordered.push(id);}}
    for(var j=0,cursor=0;j<full.length;j++)if(known[full[j]|0])full[j]=ordered[cursor++];
    for(var k=0;k<full.length;k++)if((full[k]|0)>0)R.setParent(full[k]|0,owner.__i,-1);
  }else for(var n=0;n<list.length;n++)if(list[n]&&list[n].__i>0)R.setParent(list[n].__i,owner.__i,-1);
  owner.__nativeOrderDirty=false;owner._boundsID++;
}
function proxyChildrenView(owner){
  if(owner.__childrenView)return owner.__childrenView;var numeric=/^(0|[1-9]\d*)$/;
  return owner.__childrenView=new Proxy([],{
    get:function(target,key){
      if(key==='length')return proxyChildCount(owner);
      if(key===Symbol.iterator)return function*(){var a=proxyChildSnapshot(owner);for(var i=0;i<a.length;i++)yield a[i];};
      if(typeof key==='string'&&numeric.test(key))return proxyChildAt(owner,key|0);
      if(key==='indexOf')return function(child,from){var n=proxyChildCount(owner),i=proxyChildIndex(owner,child),start=from==null?0:(from<0?Math.max(0,n+(from|0)):from|0);return i>=start?i:-1;};
      if(key==='includes')return function(child,from){return owner.children.indexOf(child,from)>=0;};
      if(key==='at')return function(index){index|=0;var n=proxyChildCount(owner);if(index<0)index+=n;return proxyChildAt(owner,index);};
      if(key==='slice')return function(begin,end){return proxyChildSnapshot(owner).slice(begin,end);};
      if(key==='forEach'||key==='map'||key==='some'||key==='every'||key==='find'||key==='findIndex'||key==='filter')return function(){var a=proxyChildSnapshot(owner);return Array.prototype[key].apply(a,arguments);};
      if(key==='push')return function(){owner.addChild.apply(owner,arguments);return proxyChildCount(owner);};
      if(key==='pop')return function(){var c=proxyChildAt(owner,proxyChildCount(owner)-1);return c?owner.removeChild(c):undefined;};
      if(key==='shift')return function(){var c=proxyChildAt(owner,0);return c?owner.removeChild(c):undefined;};
      if(key==='unshift')return function(){for(var i=arguments.length-1;i>=0;i--)owner.addChildAt(arguments[i],0);return proxyChildCount(owner);};
      if(key==='splice')return function(start,remove){var n=proxyChildCount(owner);start=start|0;if(start<0)start=Math.max(0,n+start);else start=Math.min(start,n);remove=remove===undefined?n-start:Math.max(0,Math.min(remove|0,n-start));var out=[];for(var i=0;i<remove;i++){var c=proxyChildAt(owner,start);if(c){out.push(c);owner.removeChild(c);}}for(var j=2;j<arguments.length;j++)owner.addChildAt(arguments[j],start+j-2);return out;};
      if(key==='sort'||key==='reverse')return function(compare){var a=proxyChildSnapshot(owner).slice();key==='sort'?a.sort(compare):a.reverse();syncFacadeOrder(owner,a);return owner.children;};
      return Reflect.get(target,key);
    },
    has:function(target,key){if(typeof key==='string'&&numeric.test(key))return (key|0)<proxyChildCount(owner);return key in target;},
    set:function(target,key,value){if(key==='length'&&Number(value)===0){var a=proxyChildSnapshot(owner);for(var i=a.length-1;i>=0;i--)owner.removeChild(a[i]);return true;}throw new Error('Native children are numeric handles; use addChild/removeChild/setChildIndex');}
  });
}

function NativeNodeProxy(options){
  options=options||{};this.__i=undefined;this.__nativeNodeProxy=true;this.__parentId=-1;this.__parentFallback=null;this.__childrenView=null;this.__nativeOrderDirty=false;this.name=options.name||'';this.cursor=options.cursor||null;
  this._events=Object.create(null);this._eventsCount=0;this._destroyed=false;this._bounds=new PIXI.Bounds();this._boundsID=0;this._lastSortedIndex=0;this.zIndex=0;this.sortDirty=false;this.sortableChildren=false;
  this.__eventMode=options.eventMode||'auto';this.__interactiveChildren=options.interactiveChildren!==false;this.__hitArea=options.hitArea||null;this.isMask=false;this.isSprite=false;
  this._alpha=1;this._visible=true;this._renderable=true;this.__nativePublicationGate=0;this._texture=null;this._anchor=new NativeAnchor(this,0,0);this._nativeWidth=0;this._nativeHeight=0;this._tint=0xFFFFFF;this._blendMode=0;this._premultiply=0;this._flipX=false;this._flipY=false;this._mask=null;this._maskRefCount=0;this._filters=null;this._scrollRect=null;this.$rectMask=null;this.filterArea=null;this.cullable=false;this.cullArea=null;this._cacheAsBitmap=false;
  var adopted=consumePendingFacadeAdopt(this)>0;if(!adopted&&!(R.alloc(this)>0))throw new Error('native scene node capacity exhausted');
  var b=this.__i*ST;if(!adopted){B[b+SX]=1;B[b+SY]=1;B[b+ALPHA]=1;B[b+VISIBLE]=1;B[b+RENDERABLE]=1;}
  // Pixi accepts assignments such as `display.position = new Point(x,y)`. Keep the shared-buffer
  // point identity stable and copy assigned values into it; replacing the bridge object leaves
  // native x/y at zero and misplaces every dynamically attached Spine/particle subtree.
  delete this.position;delete this.scale;delete this.pivot;delete this.skew;delete this.worldTransform;
  this.__positionPoint=new NativePoint(this,X,Y,0,0);this.__scalePoint=new NativePoint(this,SX,SY,1,1);this.__pivotPoint=new NativePoint(this,PVX,PVY,0,0);this.__skewPoint=new NativePoint(this,SKX,SKY,0,0);this.__worldMatrix=new NativeWorldMatrix(this);
  var self=this;this.transform={position:this.__positionPoint,scale:this.__scalePoint,pivot:this.__pivotPoint,skew:this.__skewPoint,worldTransform:this.__worldMatrix,localTransform:new PIXI.Matrix(),_parentID:-1,_worldID:0,updateLocalTransform:function(){return updateLocal(self);},
    onChange:function(){R.markLocal(self.__i);invalidatePackedOwner(self);},
    updateSkew:function(){this.onChange();},
    updateTransform:function(){nativeMutationReadBarrier();updateLocal(self);R.syncWorld(self);this._worldID++;},
    setFromMatrix:function(matrix){matrix.decompose(this);this.onChange();}
  };
  Object.defineProperty(this.transform,'rotation',{get:function(){return self.rotation;},set:function(v){self.rotation=v;}});
  if(!adopted)R.writeHitMeta(this);
  if(options.parent)this.attachTo(options.parent);
  if(options.texture)this.setSprite(options.texture,options);
}
function markNativeOrder(o){if(globalThis.__sgMarkOrderDirty)globalThis.__sgMarkOrderDirty(o);}
Object.defineProperties(NativeNodeProxy.prototype,{
  // Do not retain the reverse half of a JavaScript display tree.  Native SceneGraph owns the
  // hierarchy; the compatibility getter resolves a facade from the numeric parent handle only
  // when business/event code explicitly asks for `parent`.
  parent:{configurable:true,get:function(){var id=this.__parentId|0;return id>0&&R.objects?(R.objects[id]||this.__parentFallback):this.__parentFallback;},set:function(v){
    if(v&&!(v.__i>0)&&R.bindPixi)try{R.bindPixi(v);}catch(e){}
    // Some Native-owned FGUI leaves intentionally have no DisplayObject entry in R.objects.
    // Keep only their lightweight facade as a compatibility fallback; Native still owns the
    // authoritative hierarchy through the numeric parent id.
    if(v&&v.__i>0){this.__parentId=v.__i|0;this.__parentFallback=R.objects&&R.objects[this.__parentId]?null:v;}else{this.__parentId=-1;this.__parentFallback=v||null;}
  }},
  children:{configurable:true,get:function(){return proxyChildrenView(this);}},
  // Native facades allocate/adopt slots without Pixi's bind hook, and native present skips
  // updateTransform. Queue sortDirty writes here so zIndex changes still reach sortChildren.
  sortDirty:{configurable:true,get:function(){return this.__nativeSortDirty===true;},set:function(v){this.__nativeSortDirty=!!v;if(v)markNativeOrder(this);}},
  position:{configurable:true,get:function(){return this.__positionPoint;},set:function(v){if(v)this.__positionPoint.copyFrom(v);}},
  scale:{configurable:true,get:function(){return this.__scalePoint;},set:function(v){if(v)this.__scalePoint.copyFrom(v);}},
  // PIXI exposes these scalar aliases and GSAP discovers them as ordinary tween targets. Keep
  // writes on the stable NativePoint so the shared transform buffer remains authoritative.
  scaleX:{configurable:true,get:function(){return this.scale.x;},set:function(v){this.scale.x=v;}},
  scaleY:{configurable:true,get:function(){return this.scale.y;},set:function(v){this.scale.y=v;}},
  pivot:{configurable:true,get:function(){return this.__pivotPoint;},set:function(v){if(v)this.__pivotPoint.copyFrom(v);}},
  skew:{configurable:true,get:function(){return this.__skewPoint;},set:function(v){if(v)this.__skewPoint.copyFrom(v);}},
  worldTransform:{configurable:true,get:function(){return this.__worldMatrix;},set:function(){}},
  x:{get:function(){return this.position.x;},set:function(v){this.position.x=v;}},y:{get:function(){return this.position.y;},set:function(v){this.position.y=v;}},
  zIndex:{get:function(){return this._zIndex||0;},set:function(v){v=Number(v)||0;if(this._zIndex===v)return;this._zIndex=v;if(this.parent)this.parent.sortDirty=true;}},
  rotation:{get:function(){return B[this.__i*ST+ROT];},set:function(v){this._local(ROT,Number(v)||0);notifyNativeFguiDisplayRotation(this,B[this.__i*ST+ROT]);}},
  angle:{get:function(){return this.rotation*180/Math.PI;},set:function(v){this.rotation=(Number(v)||0)*Math.PI/180;}},
  alpha:{get:function(){return B[this.__i*ST+ALPHA];},set:function(v){this._alpha=Number(v);this._local(ALPHA,this._alpha);notifyNativeFguiDisplayAlpha(this,B[this.__i*ST+ALPHA]);}},
  worldAlpha:{get:function(){return B[this.__i*ST+WALPHA];}},
  visible:{get:function(){return this.__nativePublicationGate>0?this._visible:B[this.__i*ST+VISIBLE]!==0;},set:function(v){this._visible=!!v;if(notifyNativeFguiDisplayVisibility(this,this._visible))return;this._local(VISIBLE,this.__nativePublicationGate>0?0:(this._visible?1:0));}},
  renderable:{get:function(){return B[this.__i*ST+RENDERABLE]!==0;},set:function(v){this._renderable=!!v;if(preservingNativeDescriptor(this))return;var b=this.__i*ST,n=v?1:0;if(B[b+RENDERABLE]!==n){B[b+RENDERABLE]=n;R.markBounds();}}},
  eventMode:{get:function(){return this.__eventMode;},set:function(v){this.__eventMode=v||'auto';if(!preservingNativeDescriptor(this))R.writeHitMeta(this);}},
  interactive:{get:function(){return this.isInteractive();},set:function(v){this.eventMode=v?'static':'auto';}},
  interactiveChildren:{get:function(){return this.__interactiveChildren;},set:function(v){this.__interactiveChildren=!!v;if(!preservingNativeDescriptor(this))R.writeHitMeta(this);}},
  hitArea:{get:function(){return this.__hitArea;},set:function(v){this.__hitArea=v||null;if(v&&R.instrumentHitArea)R.instrumentHitArea(this,v);if(!preservingNativeDescriptor(this))R.writeHitMeta(this);}},
  localTransform:{get:function(){return updateLocal(this);}},
  anchor:{configurable:true,get:function(){return this._anchor;},set:function(v){typeof v==='number'?this._anchor.set(v):this._anchor.copyFrom(v);}},
  width:{configurable:true,get:function(){return this._nativeWidth;},set:function(v){v=Math.max(0,Number(v)||0);if(this._nativeWidth!==v){this._nativeWidth=this._width=v;this._refreshSprite();}}},
  height:{configurable:true,get:function(){return this._nativeHeight;},set:function(v){v=Math.max(0,Number(v)||0);if(this._nativeHeight!==v){this._nativeHeight=this._height=v;this._refreshSprite();}}},
  texture:{configurable:true,get:function(){return this._texture;},set:function(v){if(this._texture===v)return;var old=this._texture;if(old)R.releaseTexture(old,this);this._texture=v||null;if(this._texture){var orig=this._texture.orig;if(!old&&orig){if(!(this._nativeWidth>0))this._nativeWidth=orig.width||0;if(!(this._nativeHeight>0))this._nativeHeight=orig.height||0;}R.retainTexture(this._texture,this);}this.isSprite=!!this._texture;this._refreshSprite();}},
  tint:{configurable:true,get:function(){return this._tint;},set:function(v){this._tint=tintNumber(v);if(preservingNativeDescriptor(this))return;if(this.__i>0)B[this.__i*ST+TINT]=this._tint;R.markBounds();invalidatePackedOwner(this);}},
  tintValue:{get:function(){return this._tint;}},
  blendMode:{configurable:true,get:function(){return this._blendMode;},set:function(v){this._blendMode=Number(v)||0;if(!preservingNativeDescriptor(this)&&this.__i>0)B[this.__i*ST+BLEND]=this._blendMode;}},
  flipX:{get:function(){return this._flipX;},set:function(v){v=!!v;if(this._flipX!==v){this._flipX=v;this._refreshSprite();}}},
  flipY:{get:function(){return this._flipY;},set:function(v){v=!!v;if(this._flipY!==v){this._flipY=v;this._refreshSprite();}}},
  scale9Grid:{get:function(){if(!this.__nativeNineProxy)return null;var o=this._texture&&this._texture.orig,w=o&&o.width||this._nativeWidth,h=o&&o.height||this._nativeHeight;return new PIXI.Rectangle(this.leftWidth,this.topHeight,Math.max(0,w-this.leftWidth-this.rightWidth),Math.max(0,h-this.topHeight-this.bottomHeight));},set:function(v){if(!this.__nativeNineProxy||!v)return;var o=this._texture&&this._texture.orig,w=o&&o.width||this._nativeWidth,h=o&&o.height||this._nativeHeight,x=Number(v.x)||0,y=Number(v.y)||0;this.leftWidth=x;this.topHeight=y;this.rightWidth=Math.max(0,w-x-(Number(v.width)||0));this.bottomHeight=Math.max(0,h-y-(Number(v.height)||0));this._refreshSprite();}},
  tiledSlices:{get:function(){return this._tiledSlices||0;},set:function(v){this._tiledSlices=v|0;}},
  filters:{get:function(){return this._filters;},set:function(v){this._filters=v||null;if(this.__i>0)R.writeVisual(this);}},
  mask:{get:function(){return this._mask;},set:function(v){v=v||null;if(this._mask===v)return;var mo=this._mask&&(this._mask.isMaskData?this._mask.maskObject:this._mask);if(mo){mo._maskRefCount=Math.max(0,(mo._maskRefCount||0)-1);if(mo._maskRefCount===0){mo.renderable=true;mo.isMask=false;}}this._mask=v;mo=v&&(v.isMaskData?v.maskObject:v);if(mo){if((mo._maskRefCount||0)===0){mo.renderable=false;mo.isMask=true;}mo._maskRefCount=(mo._maskRefCount||0)+1;}if(this.__i>0)R.writeVisual(this);}},
  scrollRect:{get:function(){return this._scrollRect;},set:function(rect){
    this._scrollRect=rect||null;
    if(rect){if(!this.$rectMask){this.$rectMask=new NativeGraphicsProxy(null);this.$rectMask.name='native_scroll_rect';this.$rectMask.isMask=true;this.$rectMask.eventMode='none';this.addChild(this.$rectMask);}this.$rectMask.clear();if(rect.width>0&&rect.height>0)this.$rectMask.beginFill(0,1).drawRect(rect.x,rect.y,rect.width,rect.height).endFill();this.mask=this.$rectMask;}
    else this.mask=null;
  }},
  cacheAsBitmap:{get:function(){return false;},set:function(){} }
});
NativeNodeProxy.prototype._local=function(off,v){if(preservingNativeDescriptor(this))return;if(!isFinite(v))v=off===ALPHA?1:0;v=Math.fround(v);var b=this.__i*ST;if(B[b+off]===v)return;B[b+off]=v;R.markLocal(this.__i);invalidatePackedOwner(this);};
function setNativePublicationGate(target,gated){
  if(!target||!target.__nativeNodeProxy)return false;
  var depth=target.__nativePublicationGate|0;
  depth=gated?depth+1:Math.max(0,depth-1);
  target.__nativePublicationGate=depth;
  if(target.__i>0&&typeof target._local==='function')target._local(VISIBLE,depth>0?0:(target._visible===false?0:1));
  return true;
}
globalThis.__nativeSetPublicationGate=setNativePublicationGate;
NativeNodeProxy.prototype.isInteractive=function(){return this.eventMode==='static'||this.eventMode==='dynamic';};
NativeNodeProxy.prototype.onChildrenChange=function(){};

NativeNodeProxy.prototype.attachTo=function(parent,before){
  if(!parent)return this.removeFromParent();if(!(parent.__i>0))R.bindPixi(parent);if(!(parent.__i>0))throw new Error('parent is not native-bound');
  if(parent instanceof NativeNodeProxy){var at=before?parent.children.indexOf(before):-1;if(at<0)parent.addChild(this);else parent.addChildAt(this,at);}
  else{var idx=before&&parent.children?parent.children.indexOf(before):-1;if(idx>=0)parent.addChildAt(this,idx);else parent.addChild(this);}return this;
};
NativeNodeProxy.prototype.__insertFacadeOnly=function(child){if(!child)return child;if(!(child.__i>0))R.bindPixi(child);if(!(child.__i>0))return child;child.parent=this;if(child.transform)child.transform._parentID=-1;return child;};
NativeNodeProxy.prototype.__removeFacadeOnly=function(child){if(child){child.parent=null;if(child.transform)child.transform._parentID=-1;}return child;};
NativeNodeProxy.prototype._adopt=function(child,index,atIndex){if(!child)throw new Error('Invalid child');for(var ancestor=this;ancestor;ancestor=ancestor.parent)if(ancestor===child)throw new Error('Cannot add an ancestor as a child');if(child.parent&&child.parent.removeChild)child.parent.removeChild(child);if(!(child.__i>0))R.bindPixi(child);if(!(child.__i>0))throw new Error('child is not native-bound');if(typeof globalThis.__nativeSceneAdopt==='function')globalThis.__nativeSceneAdopt(child);var count=proxyChildCount(this);if(index<0)index=0;if(index>count)index=count;var before=proxyChildAt(this,index);this.__insertFacadeOnly(child);this.sortDirty=true;
  if(child.__nativeNodeProxy&&child.isSprite)R.writeVisual(child);R.setParent(child.__i,this.__i,before&&before.__i>0?before.__i:-1);
  // 与 COp.addChild 同语义的复挂复活:被场景图 GC 剥离过的缓存对象(quad 存储已回收/子树有洞)
  // 经 proxy 父挂回(FGUI setNativeObject 等)也必须整树复活,否则内容间歇性消失。
  if(R.activateAttached)try{R.activateAttached(child,this);}catch(e){}
  if(this.__i>0&&R.markVisual)R.markVisual(this);this.__nativeOrderDirty=true;markNativeOrder(this);this._boundsID++;this.onChildrenChange(index);if(atIndex){if(child.emit)child.emit('added',this);this.emit('childAdded',child,this,index);}else{this.emit('childAdded',child,this,index);if(child.emit)child.emit('added',this);}return child;};
NativeNodeProxy.prototype.addChild=function(){var first=arguments[0];for(var i=0;i<arguments.length;i++)this._adopt(arguments[i],proxyChildCount(this),false);return first;};
// Return the authoritative direct display order, including Native-FGUI leaves that deliberately
// have no JavaScript facade. `except` is removed before translating a Pixi insertion index so the
// same helper also implements reordering an already-attached child.
function nativeDirectChildOrder(owner,except){
  nativeMutationReadBarrier();var full=typeof globalThis.__sgNativeChildren==='function'?__sgNativeChildren(owner.__i|0):null;
  if(!full)return null;var out=[],skip=except|0;for(var i=0;i<full.length;i++){var id=full[i]|0;if(id>0&&id!==skip)out.push(id);}return out;
}
NativeNodeProxy.prototype.addChildAt=function(child,index){
  if(index<0)throw new Error('addChildAt index out of bounds');
  // Pixi's index addresses displayObject.children, not FairyGUI's semantic GObject list.  The
  // latter may contain GGroup/mask entries that are not direct display children.  Passing such an
  // id as `before` makes SceneGraph::setParent miss it and append the dynamic child, reversing the
  // requested layer (for example QuestBarIcon's addChildAt(logoSpine, 0)).
  // Read the real C++ sibling order instead; it contains both materialised facades and native-only
  // static leaves and therefore matches the browser display list exactly.
  var wasChild=!!(child&&child.parent===this),childId=child&&child.__i|0,nativeOrder=nativeDirectChildOrder(this,childId),before=-1;
  if(nativeOrder){if(index>nativeOrder.length+(wasChild?1:0))throw new Error('addChildAt index out of bounds');before=index<nativeOrder.length?nativeOrder[index]|0:-1;
    var exact=this._adopt(child,Math.min(index|0,proxyChildCount(this)),true);if(child&&child.__i>0)R.setParent(child.__i,this.__i,before);return exact;
  }
  var owner=this.UIOwner,state=owner&&owner.__nativeFguiState,meta=owner&&owner.__nativeFguiMeta;
  var count=proxyChildCount(this),semanticBefore=fguiSemanticBefore(this,child);
  // A FGUI-owned lightweight container may have native static children with no JavaScript
  // facade. Its semantic insertion index can therefore be larger than proxyChildCount even when
  // it is valid in GComponent.$children. Clamp such dynamic/manual insertions to the available
  // facade tail; ordinary Pixi containers retain the strict bounds error.
  if(index>count&&semanticBefore===null&&!this.UIOwner)throw new Error('addChildAt index out of bounds');
  var out=this._adopt(child,Math.min(index|0,count),true);
  if(semanticBefore!==null&&child&&child.__i>0){R.setParent(child.__i,this.__i,semanticBefore);var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.fguiSemanticChildRemaps=(st.fguiSemanticChildRemaps||0)+1;}
  return out;
};
NativeNodeProxy.prototype.getChildAt=function(index){var n=proxyChildCount(this);if(index<0||index>=n)throw new Error('getChildAt index out of bounds');return proxyChildAt(this,index);};
NativeNodeProxy.prototype.getChildByName=function(name,deep){var a=proxyChildSnapshot(this);for(var i=0;i<a.length;i++)if(a[i]&&a[i].name===name)return a[i];if(deep)for(var j=0;j<a.length;j++){var c=a[j];if(c&&c.getChildByName){var r=c.getChildByName(name,true);if(r)return r;}}return null;};
NativeNodeProxy.prototype.getChildIndex=function(child){var i=proxyChildIndex(this,child);if(i<0)throw new Error('child not found');return i;};
NativeNodeProxy.prototype.setChildIndex=function(child,index){var owner=this.UIOwner,state=owner&&owner.__nativeFguiState,meta=owner&&owner.__nativeFguiMeta;
  if(state&&meta){var childId=child&&child.__i|0,full=nativeDirectChildOrder(this,childId),old=proxyChildIndex(this,child);if(old<0)return this.addChildAt(child,index);
    if(full){var nativeCount=full.length+1;if(index<0||index>=nativeCount)throw new Error('setChildIndex index out of bounds');var before=index<full.length?full[index]|0:-1;if(childId>0)R.setParent(childId,this.__i,before);this.onChildrenChange(index|0);return;}
    var count=proxyChildCount(this);if(count<2){if(childId>0)R.setParent(childId,this.__i,-1);return;}var local=Math.max(0,Math.min(index|0,count-1));if(old!==local)this.onChildrenChange(local);var bm=null;try{bm=__nativeFguiChild(state.id,meta.nativeId,index|0);}catch(e){}if(childId>0)R.setParent(childId,this.__i,bm?bm.nativeId|0:-1);return;}
  var a=proxyChildSnapshot(this);if(index<0||index>=a.length)throw new Error('setChildIndex index out of bounds');var old=this.getChildIndex(child);if(old===index)return;a.splice(old,1);a.splice(index,0,child);this.onChildrenChange(index);syncFacadeOrder(this,a);};
NativeNodeProxy.prototype.swapChildren=function(a,b){if(a===b)return;var list=proxyChildSnapshot(this),ia=this.getChildIndex(a),ib=this.getChildIndex(b),item=list[ia];list[ia]=list[ib];list[ib]=item;this.onChildrenChange(Math.min(ia,ib));syncFacadeOrder(this,list);};
NativeNodeProxy.prototype.sortChildren=function(){
  var a=proxyChildSnapshot(this),need=false;for(var i=0;i<a.length;i++){var c=a[i];c._lastSortedIndex=i;if(c.zIndex)need=true;}
  this.sortDirty=false;
  // Pixi calls sortChildren for sortable containers even when every zIndex is zero. The display
  // order is already correct in that case; touching it would collapse a mixed Native/facade tree
  // into facade materialisation order.
  if(!need)return;
  var full=typeof globalThis.__sgNativeChildren==='function'?__sgNativeChildren(this.__i|0):null;
  if(full&&full.length){
    var original=Object.create(null);for(var j=0;j<full.length;j++)original[full[j]|0]=j;
    full.sort(function(x,y){var xo=R.objects&&R.objects[x|0],yo=R.objects&&R.objects[y|0],xz=xo?xo.zIndex||0:0,yz=yo?yo.zIndex||0:0;return xz===yz?original[x|0]-original[y|0]:xz-yz;});
    for(var k=0;k<full.length;k++)R.setParent(full[k]|0,this.__i,-1);
    this.__nativeOrderDirty=false;this._boundsID++;
  }else{a.sort(function(x,y){return x.zIndex===y.zIndex?x._lastSortedIndex-y._lastSortedIndex:x.zIndex-y.zIndex;});syncFacadeOrder(this,a);}
};
NativeNodeProxy.prototype._syncChildOrder=function(){syncFacadeOrder(this,proxyChildSnapshot(this));};
NativeNodeProxy.prototype.removeChild=function(){var first=arguments[0];for(var k=0;k<arguments.length;k++){var child=arguments[k],i=proxyChildIndex(this,child);if(i<0){if(arguments.length===1)return null;continue;}if(child.__i>0)R.remove(child.__i);this.__nativeOrderDirty=true;child.parent=null;if(child.transform)child.transform._parentID=-1;this._boundsID++;this.onChildrenChange(i);if(child.emit)child.emit('removed',this);this.emit('childRemoved',child,this,i);}if(this.__i>0&&R.markVisual)R.markVisual(this);return first;};
NativeNodeProxy.prototype.removeChildAt=function(index){return this.removeChild(this.getChildAt(index));};
NativeNodeProxy.prototype.removeChildren=function(begin,end){var all=proxyChildSnapshot(this),n=all.length;begin=begin===undefined?0:begin;end=end===undefined?n:end;var range=end-begin;if(range===0&&n===0)return[];if(begin<0||end>n||range<=0)throw new RangeError('removeChildren range');var out=all.slice(begin,end);for(var i=0;i<out.length;i++){var child=out[i];if(!child)continue;R.remove(child.__i);child.parent=null;if(child.transform)child.transform._parentID=-1;}this.__nativeOrderDirty=true;this._boundsID++;this.onChildrenChange(begin);for(var j=0;j<out.length;j++){var removed=out[j];if(removed.emit)removed.emit('removed',this);this.emit('childRemoved',removed,this,j);}if(this.__i>0&&R.markVisual)R.markVisual(this);return out;};
NativeNodeProxy.prototype.removeFromParent=function(){var par=this.parent;if(par&&par.children&&par.children.indexOf(this)>=0){par.removeChild(this);return this;}if(this.__i>0)R.remove(this.__i);this.parent=null;return this;};

NativeNodeProxy.prototype.setContainer=function(){if(preservingNativeDescriptor(this))return this;var b=this.__i*ST;globalThis.__sgTypeW(b,0);B[b+TEXID]=0;R.markBounds();return this;};
NativeNodeProxy.prototype.setSprite=function(texture,options){
  options=options||{};var tex=texture&&texture.baseTexture?texture:null,bt=tex?tex.baseTexture:texture,w=Number(options.width),h=Number(options.height);if(!isFinite(w))w=tex&&tex.orig?tex.orig.width:(bt&&bt.width)||0;if(!isFinite(h))h=tex&&tex.orig?tex.orig.height:(bt&&bt.height)||0;
  this.__nativeNineProxy=false;this._anchor._x=Number(options.anchorX)||0;this._anchor._y=Number(options.anchorY)||0;this._nativeWidth=this._width=Math.max(0,w||0);this._nativeHeight=this._height=Math.max(0,h||0);this._tint=options.tint===undefined?0xFFFFFF:tintNumber(options.tint);this._blendMode=options.blendMode||0;this._premultiply=options.premultiply===undefined?(bt&&bt.alphaMode===0?1:0):(options.premultiply?1:0);this.texture=texture;this._refreshSprite();return this;
};
NativeNodeProxy.prototype.setNineSlice=function(texture,grid,tiledSlices){grid=grid||{};this.__nativeNineProxy=true;this.leftWidth=Number(grid.left!==undefined?grid.left:grid.x)||0;this.topHeight=Number(grid.top!==undefined?grid.top:grid.y)||0;var tex=texture&&texture.orig?texture.orig:null,tw=tex&&tex.width||0,th=tex&&tex.height||0,gx=Number(grid.x)||0,gy=Number(grid.y)||0,gw=Number(grid.width)||0,gh=Number(grid.height)||0;this.rightWidth=Math.max(0,tw-gx-gw);this.bottomHeight=Math.max(0,th-gy-gh);this._tiledSlices=tiledSlices|0;this._nativeWidth=this._width=tw;this._nativeHeight=this._height=th;this.texture=texture;this._refreshSprite();return this;};
NativeNodeProxy.prototype._refreshSprite=function(){if(!(this.__i>0)||preservingNativeDescriptor(this))return;if(this.__nativeNineProxy){this.$flipX=this._flipX;this.$flipY=this._flipY;R.writeVisual(this);invalidatePackedOwner(this);return;}var b=this.__i*ST,tex=this._texture,bt=tex&&tex.baseTexture,orig=tex&&tex.orig,trim=tex&&tex.trim,w=this._nativeWidth,h=this._nativeHeight,ax=this._anchor._x||0,ay=this._anchor._y||0;
  if(!tex||!orig){globalThis.__sgTypeW(b,0);B[b+TEXID]=0;R.markBounds();invalidatePackedOwner(this);return;}
  var ox=0,oy=0,rw=w,rh=h;if(trim&&orig.width&&orig.height){var xs=w/orig.width,ys=h/orig.height;ox=trim.x*xs;oy=trim.y*ys;rw=trim.width*xs;rh=trim.height*ys;if(this._flipX)ox=w-ox-rw;if(this._flipY)oy=h-oy-rh;}
  B[b+RX]=ox-ax*w;B[b+RY]=oy-ay*h;B[b+RW]=rw;B[b+RH]=rh;var u=tex._uvs;if(!u&&tex.updateUvs)try{tex.updateUvs();u=tex._uvs;}catch(e){}var uv=u?[u.x0,u.y0,u.x1,u.y1,u.x2,u.y2,u.x3,u.y3]:[0,0,1,0,1,1,0,1];
  if(this._flipX){var q0=uv[0],q1=uv[1],q6=uv[6],q7=uv[7];uv[0]=uv[2];uv[1]=uv[3];uv[2]=q0;uv[3]=q1;uv[6]=uv[4];uv[7]=uv[5];uv[4]=q6;uv[5]=q7;}if(this._flipY){var a0=uv[0],a1=uv[1],a2=uv[2],a3=uv[3];uv[0]=uv[6];uv[1]=uv[7];uv[2]=uv[4];uv[3]=uv[5];uv[6]=a0;uv[7]=a1;uv[4]=a2;uv[5]=a3;}for(var i=0;i<8;i++)B[b+U0+i]=Number(uv[i])||0;
  // alphaMode can be finalized/changed when the asynchronous atlas resource is uploaded. Using
  // the value captured when the proxy was constructed leaves transparent atlas padding visible as
  // coloured rectangles after a resized image is expanded. Match the regular Sprite writer and
  // derive premultiplication from the live BaseTexture on every visual refresh.
  this._premultiply=(bt&&bt.alphaMode===0)?1:0;
  globalThis.__sgTypeW(b,1);B[b+TEXID]=R.textureId(tex)|0;B[b+TINT]=this._tint;B[b+BLEND]=this._blendMode;B[b+PREMUL]=this._premultiply;R.markBounds();R.writeHitMeta(this);invalidatePackedOwner(this);};
NativeNodeProxy.prototype.updateTransform=function(){nativeMutationReadBarrier();this.transform.updateLocalTransform();if(this.sortableChildren&&this.sortDirty)this.sortChildren();R.syncWorld(this);this.transform._worldID++;};
NativeNodeProxy.prototype.displayObjectUpdateTransform=NativeNodeProxy.prototype.updateTransform;
NativeNodeProxy.prototype.calculateBounds=function(){nativeMutationReadBarrier();this._bounds.clear();R.syncWorld(this);var r=R.getBounds(this,NativeNodeProxy._worldBounds);if(r)this._bounds.addFrameMatrix(NativeNodeProxy._identity,r.x,r.y,r.x+r.width,r.y+r.height);this._bounds.updateID=this._boundsID;};
NativeNodeProxy.prototype.getLocalBounds=function(rect){rect=rect||new PIXI.Rectangle();rect.x=0;rect.y=0;rect.width=this._nativeWidth;rect.height=this._nativeHeight;return rect;};
NativeNodeProxy.prototype.getBounds=function(skipUpdate,rect){nativeMutationReadBarrier();R.syncWorld(this);return R.getBounds(this,rect)||rect||new PIXI.Rectangle();};
NativeNodeProxy.prototype.containsPoint=function(point){var p=this.worldTransform.applyInverse(point,NativeNodeProxy._point);if(this.hitArea&&this.hitArea.contains)return this.hitArea.contains(p.x,p.y);var b=this.__i*ST,x=B[b+RX],y=B[b+RY],w=B[b+RW],h=B[b+RH];return p.x>=Math.min(x,x+w)&&p.x<=Math.max(x,x+w)&&p.y>=Math.min(y,y+h)&&p.y<=Math.max(y,y+h);};
NativeNodeProxy._point=new PIXI.Point();
NativeNodeProxy._identity=new PIXI.Matrix();
NativeNodeProxy._worldBounds=new PIXI.Rectangle();
NativeNodeProxy.prototype.toGlobal=function(pos,out){nativeMutationReadBarrier();R.syncWorld(this);return this.worldTransform.apply(pos,out);};
NativeNodeProxy.prototype.toLocal=function(pos,from,out){nativeMutationReadBarrier();if(from)pos=from.toGlobal(pos,out);R.syncWorld(this);return this.worldTransform.applyInverse(pos,out);};
NativeNodeProxy.prototype.getGlobalPosition=function(out,skipUpdate){out=out||new PIXI.Point();return this.parent?this.parent.toGlobal(this.position,out,skipUpdate):this.position.copyTo(out);};
NativeNodeProxy._origin=new PIXI.Point();
NativeNodeProxy.prototype.render=function(renderer){
  if(renderer&&this.visible!==false&&this.renderable!==false)rejectPixiRender('node',this);
};

// Use the same EventEmitter and federated EventTarget contracts as the vendored Pixi.
// This also preserves symbol events, once/context matching, capture and signal options.
['on','addListener','once','removeListener','off','emit','listeners','listenerCount','eventNames','removeAllListeners'].forEach(function(name){
  NativeNodeProxy.prototype[name]=PIXI.utils.EventEmitter.prototype[name];
});
['addEventListener','removeEventListener','dispatchEvent'].forEach(function(name){NativeNodeProxy.prototype[name]=PIXI.DisplayObject.prototype[name];});
NativeNodeProxy.prototype.destroy=function(options){if(this._destroyed)return;if(this.__nativeV8Cache&&this.__nativeDisposeCacheTexture)this.__nativeDisposeCacheTexture();var destroyChildren=options===true||(options&&options.children),c=proxyChildSnapshot(this);if(destroyChildren){for(var i=0;i<c.length;i++)c[i].destroy(options);}else{for(var j=c.length-1;j>=0;j--)c[j].removeFromParent();}this.removeFromParent();if(this._texture)R.releaseTexture(this._texture,this);R.releaseVisual(this);this.mask=null;this._filters=null;this.filterArea=null;this.__hitArea=null;this.__interactiveChildren=false;this._destroyed=true;this.emit('destroyed');this.removeAllListeners();if(typeof globalThis.__nativeSceneDispose==='function')globalThis.__nativeSceneDispose(this);R.free(this);this._destroyed=true;this.__parentId=-1;this.__parentFallback=null;};

function installPixiBase(proto,pixiProto){
  Object.getOwnPropertyNames(NativeNodeProxy.prototype).forEach(function(k){if(k!=='constructor'){var d=Object.getOwnPropertyDescriptor(NativeNodeProxy.prototype,k);d.configurable=true;Object.defineProperty(proto,k,d);}});
  Object.setPrototypeOf(proto,pixiProto);
}
function shadowPixiTransformAccessors(o){Object.defineProperties(o,{position:{configurable:true,writable:true,value:null},scale:{configurable:true,writable:true,value:null},pivot:{configurable:true,writable:true,value:null},skew:{configurable:true,writable:true,value:null},worldTransform:{configurable:true,writable:true,value:null}});}
// A constructor's custom Symbol.hasInstance is inherited by subclasses.  A broad marker-only
// predicate on PIXI.Container therefore made every lightweight container pass `instanceof Spine`
// after Spine started extending NativeDynamicContainer.  Restrict the compatibility predicate to
// the public replacement itself; real subclasses use the ordinary prototype-chain algorithm.
function installNativeHasInstance(ctor,original,marker){
  try{Object.defineProperty(ctor,Symbol.hasInstance,{configurable:true,value:function(o){
    if(this!==ctor)return Function.prototype[Symbol.hasInstance].call(this,o);
    return !!o&&(!!o[marker]||!!original&&Function.prototype[Symbol.hasInstance].call(original,o));
  }});}catch(e){}
}

// Container-compatible native handle for FGUI components. It passes the few strict Pixi
// instanceof checks used by game code, but stores no Pixi Bounds/Transform/renderer state.
function NativeContainerProxy(owner,options){shadowPixiTransformAccessors(this);options=options||{};NativeNodeProxy.call(this,options);this.UIOwner=owner||null;this.__nativeContainerProxy=true;this.setContainer();}
NativeContainerProxy.prototype=Object.create(PIXI.Container.prototype);installPixiBase(NativeContainerProxy.prototype,PIXI.Container.prototype);NativeContainerProxy.prototype.constructor=NativeContainerProxy;
// Match PIXI.Container exactly: a container contributes descendant bounds for layout/rendering,
// but it has no self hit geometry. NativeNodeProxy's generic rectangle containsPoint is copied by
// installPixiBase, so remove it explicitly here. Otherwise an eventMode="auto" decoration (for
// example a non-interactive Spine particle above a button) turns its whole rendered subtree into
// a witness and promotes the nearest interactive ancestor ahead of the real control underneath.
// Interactive FGUI containers already publish an explicit hitArea when they are opaque; Sprite,
// Graphics and Mesh proxies retain their own real geometry tests.
delete NativeContainerProxy.prototype.containsPoint;
NativeContainerProxy.prototype.onChildrenChange=function(){};
NativeContainerProxy.prototype.getLocalBounds=function(rect){
  rect=rect||new PIXI.Rectangle();var nativeBounds=R.getLocalBounds&&R.getLocalBounds(this,rect);if(nativeBounds)return nativeBounds;
  var x0=Infinity,y0=Infinity,x1=-Infinity,y1=-Infinity;
  var scratch=this.__localBoundsScratch||(this.__localBoundsScratch=new PIXI.Rectangle());
  for(var i=0;i<this.children.length;i++){var c=this.children[i];if(!c||c.visible===false||c.renderable===false||!c.getLocalBounds)continue;var r=c.getLocalBounds(scratch),m=c.localTransform||c.transform&&c.transform.localTransform;if(!r||!m)continue;var ax=r.x,ay=r.y,bx=ax+r.width,by=ay+r.height;
    var p0x=m.a*ax+m.c*ay+m.tx,p0y=m.b*ax+m.d*ay+m.ty,p1x=m.a*bx+m.c*ay+m.tx,p1y=m.b*bx+m.d*ay+m.ty,p2x=m.a*bx+m.c*by+m.tx,p2y=m.b*bx+m.d*by+m.ty,p3x=m.a*ax+m.c*by+m.tx,p3y=m.b*ax+m.d*by+m.ty;
    x0=Math.min(x0,p0x,p1x,p2x,p3x);y0=Math.min(y0,p0y,p1y,p2y,p3y);x1=Math.max(x1,p0x,p1x,p2x,p3x);y1=Math.max(y1,p0y,p1y,p2y,p3y);
  }
  if(x0===Infinity){rect.x=rect.y=rect.width=rect.height=0;}else{rect.x=x0;rect.y=y0;rect.width=x1-x0;rect.height=y1-y0;}return rect;
};
NativeContainerProxy._rect=new PIXI.Rectangle();
Object.defineProperties(NativeContainerProxy.prototype,{
  width:{configurable:true,get:function(){return Math.abs(this.scale.x)*this.getLocalBounds(NativeContainerProxy._rect).width;},set:function(v){var w=this.getLocalBounds(NativeContainerProxy._rect).width,s=this.scale.x<0?-1:1;this.scale.x=w?Math.max(0,Number(v)||0)/w*s:s;}},
  height:{configurable:true,get:function(){return Math.abs(this.scale.y)*this.getLocalBounds(NativeContainerProxy._rect).height;},set:function(v){var h=this.getLocalBounds(NativeContainerProxy._rect).height,s=this.scale.y<0?-1:1;this.scale.y=h?Math.max(0,Number(v)||0)/h*s:s;}}
});

// FGUI GGraph facade.  It records only semantic shapes and sends their triangles straight to the
// native SceneGraph. RenderTexture content uses the same Native mesh; no PIXI.Graphics is created.
function NativeGraphicsProxy(owner){
  // PIXI.DisplayObject exposes these as accessors backed by a Pixi Transform. Shadow them before
  // NativeNodeProxy initializes its own shared-buffer-backed transform points.
  shadowPixiTransformAccessors(this);
  NativeNodeProxy.call(this,{eventMode:'none',interactiveChildren:false});this.UIOwner=owner||null;
  this.__nativeGraphicsProxy=true;this._commands=[];this._line={width:0,color:0,alpha:0};this._fill={color:0xFFFFFF,alpha:1,enabled:false};this._currentPath=null;this._holeMode=false;this._holeTarget=null;this._hasNativeBounds=false;this._nativeMinX=this._nativeMinY=0;this.setContainer();
}
// Keep PIXI.Graphics in the prototype chain because FGUI's package loader performs a strict
// instanceof check before accepting an object as a component mask. Copy the lightweight node API
// as own methods; the PIXI constructor is never called and no Pixi geometry/renderer state exists.
NativeGraphicsProxy.prototype=Object.create(PIXI.Graphics.prototype);
installPixiBase(NativeGraphicsProxy.prototype,PIXI.Graphics.prototype);
NativeGraphicsProxy.prototype.constructor=NativeGraphicsProxy;
// PIXI.Graphics inherits Container.width/height: the local geometry stays immutable and assigning
// a displayed extent changes the transform scale. NativeNodeProxy's default accessors instead
// describe resizable sprite quads, so using them here changes only the cached bounds and leaves the
// uploaded Graphics mesh untouched. Full-screen GGraph masks then stop at their pre-resize width,
// while the getter reports the requested width and prevents the next resize pass from repairing it.
var displaySizeContract=globalThis.__nativeDisplaySizeContract;
if(!displaySizeContract)throw new Error('native display-size contract must load before native_node_proxy');
displaySizeContract.installLocalBoundsAccessors(NativeGraphicsProxy.prototype);
NativeGraphicsProxy.prototype.clear=function(){this._commands.length=0;this._currentPath=null;this._holeMode=false;this._holeTarget=null;this._hasNativeBounds=false;this._nativeMinX=this._nativeMinY=0;this._nativeWidth=this._nativeHeight=0;this._syncNativeGraphics();
  // Pixi's Graphics hooks normally invalidate the owner and clear its cached mask rectangle.
  // NativeGraphicsProxy deliberately overrides Graphics.clear(), so mirror that contract here.
  // Without it a cleared/rebuilt scrollRect can keep clipping against its previous dimensions.
  if(this.__maskOf&&this.__i>0){var b=this.__i*ST;B[b+RX]=B[b+RY]=B[b+RW]=B[b+RH]=0;if(R.markVisual)R.markVisual(this.__maskOf);}
  return this;
};
NativeGraphicsProxy.prototype.lineStyle=function(width,color,alpha){
  if(width&&typeof width==='object'){var o=width;width=o.width;color=o.color;alpha=o.alpha;}
  this._line={width:Math.max(0,Number(width)||0),color:tintNumber(color===undefined?0:color),alpha:alpha===undefined?1:Math.max(0,Number(alpha)||0)};return this;
};
NativeGraphicsProxy.prototype.beginFill=function(color,alpha){this._fill={color:tintNumber(color===undefined?0xFFFFFF:color),alpha:alpha===undefined?1:Math.max(0,Number(alpha)||0),enabled:true};return this;};
NativeGraphicsProxy.prototype.endFill=function(){return this;};
NativeGraphicsProxy.prototype._includePoints=function(points){if(!points||points.length<2)return;var fresh=!this._hasNativeBounds,minX=fresh?Infinity:this._nativeMinX,minY=fresh?Infinity:this._nativeMinY,maxX=fresh?-Infinity:this._nativeMinX+this._nativeWidth,maxY=fresh?-Infinity:this._nativeMinY+this._nativeHeight;for(var i=0;i+1<points.length;i+=2){var x=Number(points[i]),y=Number(points[i+1]);if(!isFinite(x)||!isFinite(y))continue;if(x<minX)minX=x;if(x>maxX)maxX=x;if(y<minY)minY=y;if(y>maxY)maxY=y;}if(minX!==Infinity){this._hasNativeBounds=true;this._nativeMinX=minX;this._nativeMinY=minY;this._nativeWidth=Math.max(0,maxX-minX);this._nativeHeight=Math.max(0,maxY-minY);}};
NativeGraphicsProxy.prototype._styleSnapshot=function(){return{line:{width:this._line.width,color:this._line.color,alpha:this._line.alpha},fill:{color:this._fill.color,alpha:this._fill.alpha,enabled:this._fill.enabled}};};
NativeGraphicsProxy.prototype._shape=function(type,x,y,w,h,r){
  x=Number(x)||0;y=Number(y)||0;w=Math.max(0,Number(w)||0);h=Math.max(0,Number(h)||0);r=Math.max(0,Number(r)||0);
  var style=this._styleSnapshot(),c={type:type,x:x,y:y,w:w,h:h,r:r,line:style.line,fill:style.fill,holes:[],closed:true};
  if(this._holeMode&&this._holeTarget)this._holeTarget.holes.push(graphLoop(c,0));else{this._commands.push(c);this._currentPath=null;this._holeTarget=c;this._includePoints([x,y,x+w,y+h]);}
  this._boundsID++;this._syncNativeGraphics();
  // native_scenegraph instruments PIXI.Graphics.drawRect(), but this proxy owns a faster override
  // and therefore bypassed that instrumentation. Keep the authoritative native scissor rectangle
  // and its owner dirty whenever FairyGUI rebuilds a ScrollPane mask after a live window resize.
  if(this.__maskOf&&this.__i>0){if(type==='rect'){var b=this.__i*ST;B[b+RX]=x;B[b+RY]=y;B[b+RW]=w;B[b+RH]=h;}if(R.markVisual)R.markVisual(this.__maskOf);}
  return this;
};
NativeGraphicsProxy.prototype.drawRect=function(x,y,w,h){return this._shape('rect',x,y,w,h,0);};
NativeGraphicsProxy.prototype.drawRoundedRect=function(x,y,w,h,r){return this._shape('round',x,y,w,h,r);};
NativeGraphicsProxy.prototype.drawCircle=function(x,y,r){r=Math.max(0,Number(r)||0);return this._shape('ellipse',(Number(x)||0)-r,(Number(y)||0)-r,r*2,r*2,r);};
NativeGraphicsProxy.prototype.drawEllipse=function(x,y,rx,ry){rx=Math.max(0,Number(rx)||0);ry=Math.max(0,Number(ry)||0);return this._shape('ellipse',(Number(x)||0)-rx,(Number(y)||0)-ry,rx*2,ry*2,Math.max(rx,ry));};
NativeGraphicsProxy.prototype.drawShape=function(shape){
  if(!shape)return this;var sh=PIXI.SHAPES||{};
  if(shape.type===sh.RECT)return this.drawRect(shape.x,shape.y,shape.width,shape.height);
  if(shape.type===sh.RREC)return this.drawRoundedRect(shape.x,shape.y,shape.width,shape.height,shape.radius);
  if(shape.type===sh.CIRC)return this.drawCircle(shape.x,shape.y,shape.radius);
  if(shape.type===sh.ELIP)return this.drawEllipse(shape.x,shape.y,shape.width,shape.height);
  if(shape.type===sh.POLY||shape.points)return this.drawPolygon(shape);return this;
};
NativeGraphicsProxy.prototype.moveTo=function(x,y){x=Number(x)||0;y=Number(y)||0;var points=[x,y];if(this._holeMode&&this._holeTarget){this._holeTarget.holes.push(points);this._currentPath={type:'hole',points:points,target:this._holeTarget};}else{var style=this._styleSnapshot(),c={type:'poly',points:points,holes:[],closed:false,line:style.line,fill:style.fill};this._commands.push(c);this._currentPath=c;this._holeTarget=c;this._includePoints(points);}this._boundsID++;return this;};
// Path edits (including arc's lineTo calls) can refill a previously empty mask. Uploading the
// mesh alone leaves its owner's cached zero-area scissor and mask kind unchanged. Invalidate on
// drawing mutations, not in _syncNativeGraphics: owner refresh itself calls that upload method.
NativeGraphicsProxy.prototype.lineTo=function(x,y){x=Number(x)||0;y=Number(y)||0;if(!this._currentPath)this.moveTo(0,0);this._currentPath.points.push(x,y);if(this._currentPath.type!=='hole')this._includePoints([x,y]);this._boundsID++;this._syncNativeGraphics();if(this.__maskOf&&this.__i>0&&R.markVisual)R.markVisual(this.__maskOf);return this;};
NativeGraphicsProxy.prototype.closePath=function(){if(this._currentPath)this._currentPath.closed=true;this._syncNativeGraphics();return this;};
NativeGraphicsProxy.prototype.beginHole=function(){this._holeMode=true;this._holeTarget=this._commands.length?this._commands[this._commands.length-1]:null;this._currentPath=null;return this;};
NativeGraphicsProxy.prototype.endHole=function(){this._holeMode=false;this._currentPath=null;this._syncNativeGraphics();return this;};
NativeGraphicsProxy.prototype.drawPolygon=function(path){var src=path&&path.points?path.points:path,args=[];if(Array.isArray(src)||ArrayBuffer.isView(src)){for(var i=0;i<src.length;i++){var p=src[i];if(typeof p==='number')args.push(Number(p)||0);else if(p)args.push(Number(p.x)||0,Number(p.y)||0);}}else{for(var ai=0;ai<arguments.length;ai++){var ap=arguments[ai];if(typeof ap==='number')args.push(Number(ap)||0);else if(ap)args.push(Number(ap.x)||0,Number(ap.y)||0);}}if(args.length<4)return this;var style=this._styleSnapshot(),c={type:'poly',points:args,holes:[],closed:path&&path.closeStroke!==undefined?!!path.closeStroke:true,line:style.line,fill:style.fill};if(this._holeMode&&this._holeTarget)this._holeTarget.holes.push(args);else{this._commands.push(c);this._currentPath=c;this._holeTarget=c;this._includePoints(args);}this._boundsID++;this._syncNativeGraphics();if(this.__maskOf&&this.__i>0&&R.markVisual)R.markVisual(this.__maskOf);return this;};
NativeGraphicsProxy.prototype.arc=function(cx,cy,r,start,end,anticlockwise){
  cx=Number(cx)||0;cy=Number(cy)||0;r=Math.max(0,Number(r)||0);start=Number(start)||0;end=Number(end)||0;
  // Match PIXI.Graphics.arc's path contract, including the zero-sweep no-op and the implicit
  // straight segment from the current point to the arc's exact start.  Skipping that start point
  // removes one tessellation wedge from every filled sector; at 100% it leaves a permanent seam.
  if(start===end)return this;var twoPi=Math.PI*2;
  if(!anticlockwise&&end<=start)end+=twoPi;else if(anticlockwise&&start<=end)start+=twoPi;
  var span=end-start;if(span===0)return this;var startX=cx+Math.cos(start)*r,startY=cy+Math.sin(start)*r,points=this._currentPath&&this._currentPath.points;
  if(!points)this.moveTo(startX,startY);else{var pn=points.length,lastX=points[pn-2],lastY=points[pn-1];if(Math.abs(lastX-startX)>=1e-4||Math.abs(lastY-startY)>=1e-4)this.lineTo(startX,startY);}
  var n=Math.max(2,Math.ceil(Math.abs(span)/(Math.PI/16)));for(var i=1;i<=n;i++){var a=start+span*i/n;this.lineTo(cx+Math.cos(a)*r,cy+Math.sin(a)*r);}return this;
};
function graphLoop(c,inset){
  var x=c.x+inset,y=c.y+inset,w=Math.max(0,c.w-inset*2),h=Math.max(0,c.h-inset*2),out=[],i,n=24;
  if(c.type==='ellipse'){for(i=0;i<n;i++){var a=-Math.PI/2+i*Math.PI*2/n;out.push(x+w*.5+Math.cos(a)*w*.5,y+h*.5+Math.sin(a)*h*.5);}return out;}
  if(c.type!=='round')return [x,y,x+w,y,x+w,y+h,x,y+h];
  var rr=Math.max(0,Math.min(w*.5,h*.5,c.r-inset));
  var seg=5,centers=[[x+rr,y+rr,Math.PI,Math.PI*1.5],[x+w-rr,y+rr,Math.PI*1.5,Math.PI*2],[x+w-rr,y+h-rr,0,Math.PI*.5],[x+rr,y+h-rr,Math.PI*.5,Math.PI]];
  for(var q=0;q<4;q++)for(i=0;i<=seg;i++){var e=centers[q],aa=e[2]+(e[3]-e[2])*i/seg;out.push(e[0]+Math.cos(aa)*rr,e[1]+Math.sin(aa)*rr);}return out;
}
NativeGraphicsProxy.prototype._syncNativeGraphics=function(asMask){
  if(this._destroyed||preservingNativeDescriptor(this)||typeof globalThis.__nativeGraphicsMesh!=='function')return;
  var pts=[],idx=[],groups=[],white=R.textureId(PIXI.Texture.WHITE)|0;
  var appendLoopStroke=function(loop,width,closed){if(!(width>0)||!loop||loop.length<4)return 0;var half=width*.5,count=loop.length/2,added=0,segments=closed?count:count-1;for(var li=0;li<segments;li++){var ni=(li+1)%count,x0=loop[li*2],y0=loop[li*2+1],x1=loop[ni*2],y1=loop[ni*2+1],dx=x1-x0,dy=y1-y0,len=Math.sqrt(dx*dx+dy*dy)||1,nx=-dy/len*half,ny=dx/len*half,b=pts.length/2;pts.push(x0+nx,y0+ny,x1+nx,y1+ny,x1-nx,y1-ny,x0-nx,y0-ny);idx.push(b,b+1,b+2,b,b+2,b+3);added+=6;}return added;};
  for(var ci=0;ci<this._commands.length;ci++){
    var c=this._commands[ci],loop=c.type==='poly'?(c.points||[]):graphLoop(c,0),count=loop.length/2,k,start,base,holes=c.holes||[];
    if(c.fill.enabled&&c.fill.alpha>0&&count>=3){start=idx.length;base=pts.length/2;var flat=loop.slice(),holeIdx=[];for(var hi=0;hi<holes.length;hi++){var hl=holes[hi];if(!hl||hl.length<6)continue;holeIdx.push(flat.length/2);for(k=0;k<hl.length;k++)flat.push(hl[k]);}for(k=0;k<flat.length;k++)pts.push(flat[k]);var tri=null;try{tri=PIXI.utils&&PIXI.utils.earcut?PIXI.utils.earcut(flat,holeIdx,2):null;}catch(_earcut){}if(tri&&tri.length){for(k=0;k<tri.length;k++)idx.push(base+(tri[k]|0));}else if(!holeIdx.length){for(k=1;k+1<count;k++)idx.push(base,base+k,base+k+1);}groups.push([white,start,idx.length-start,c.fill.color,c.fill.alpha,0,0]);}
    if(c.line.width>0&&c.line.alpha>0&&count>=2){start=idx.length;appendLoopStroke(loop,c.line.width,c.closed!==false);for(var hli=0;hli<holes.length;hli++)appendLoopStroke(holes[hli],c.line.width,true);groups.push([white,start,idx.length-start,c.line.color,c.line.alpha,0,0]);}
  }
  // Non-empty typed arrays keep JavaScriptCore's byte pointer valid even for a clear operation.
  if(!pts.length)pts=[0,0];if(!idx.length)idx=[0];var meta=[];for(var g=0;g<groups.length;g++)for(var m=0;m<7;m++)meta.push(groups[g][m]);if(!meta.length)meta=[white,0,0,0xFFFFFF,0,0,0];
  var uv=new Float32Array(pts.length);globalThis.__nativeGraphicsMesh(this.__i,new Float32Array(pts),new Uint32Array(idx),uv,new Float32Array(meta),this._blendMode||0,!!asMask);
  B[this.__i*ST+RENDERABLE]=this._renderable===false?0:1;R.writeHitMeta(this);R.markBounds();
};
NativeGraphicsProxy.prototype.getLocalBounds=function(rect){rect=rect||new PIXI.Rectangle();rect.x=this._nativeMinX||0;rect.y=this._nativeMinY||0;rect.width=this._nativeWidth;rect.height=this._nativeHeight;return rect;};
NativeGraphicsProxy.prototype.containsPoint=function(point){var p=this.worldTransform.applyInverse(point,NativeNodeProxy._point),o=this.UIOwner;if(o&&o.$type===2){var rx=this._nativeWidth*.5,ry=this._nativeHeight*.5;if(!(rx>0&&ry>0))return false;var dx=(p.x-(this._nativeMinX||0)-rx)/rx,dy=(p.y-(this._nativeMinY||0)-ry)/ry;return dx*dx+dy*dy<=1;}var x=this._nativeMinX||0,y=this._nativeMinY||0;return p.x>=x&&p.y>=y&&p.x<=x+this._nativeWidth&&p.y<=y+this._nativeHeight;};
NativeGraphicsProxy.prototype.render=function(renderer){
  if(renderer&&this.visible!==false&&this.renderable!==false)rejectPixiRender('graphics',this);
};
var _destroyGraphicsNode=NativeGraphicsProxy.prototype.destroy;
NativeGraphicsProxy.prototype.destroy=function(options){return _destroyGraphicsNode.call(this,options);};

// CoreText-backed FGUI text facade. TextStyle stays in JS because FGUI mutates it directly, while
// glyph layout/raster/texture ownership lives in the Mac host. No Canvas or PIXI.Text is retained.
var nativeTextStyleContract=globalThis.__nativeTextStyleContract;
var nativeTextResolutionContract=globalThis.__nativeTextResolutionContract;
function nativeTextColor(v){if(nativeTextStyleContract&&typeof nativeTextStyleContract.colorCss==='function')return nativeTextStyleContract.colorCss(v,'transparent');if(Array.isArray(v))v=v[0];if(typeof v==='number')return '#'+('000000'+(v>>>0).toString(16)).slice(-6);return String(v==null?'transparent':v);}
function nativeTextGradient(style){return nativeTextStyleContract&&typeof nativeTextStyleContract.gradient==='function'?nativeTextStyleContract.gradient(style||{}):null;}
function nativeTextFontMetrics(font){
  try{var tm=PIXI.TextMetrics,fm=tm&&tm.measureFont&&tm.measureFont(font),a=Number(fm&&fm.ascent),d=Number(fm&&fm.descent);if(isFinite(a)&&isFinite(d)&&a>0&&d>=0)return {ascent:a,descent:d};}catch(_fontMetrics){}
  return {ascent:0,descent:0};
}
function nativeTextClampResolution(v){return nativeTextResolutionContract&&typeof nativeTextResolutionContract.clamp==='function'?nativeTextResolutionContract.clamp(v):Math.max(1,Math.min(3,Number(v)||1));}
function nativeTextAutomaticResolution(){return nativeTextResolutionContract&&typeof nativeTextResolutionContract.automatic==='function'?nativeTextResolutionContract.automatic(globalThis,PIXI):nativeTextClampResolution(globalThis.__nativeTextRasterScale||globalThis.__screenDPR||(PIXI.settings&&PIXI.settings.RESOLUTION)||1);}
function nativeTextInitialResolution(textType){return nativeTextResolutionContract&&typeof nativeTextResolutionContract.initial==='function'?nativeTextResolutionContract.initial(globalThis,PIXI,textType):nativeTextAutomaticResolution();}
function nativeTextGeometryMatches(o,text,anchorX,anchorY){
  // Text metrics may remain valid while a reused/rebound scene slot has lost its sprite
  // descriptor.  A metrics-only cache hit must not suppress the geometry publication: doing so
  // renders centered text from x=0/y=0 until some unrelated style/text mutation invalidates it.
  if(!text)return true;
  if(!o||!(o.__i>0))return false;
  var b=o.__i*ST,w=Number(B[b+RW]),h=Number(B[b+RH]);
  if((B[b+TYPE]|0)!==1||!(B[b+TEXID]>0)||!isFinite(w)||!isFinite(h)||w<=0||h<=0)return false;
  var eps=1/256;
  return Math.abs(Number(B[b+RX])-(-anchorX*w))<=eps&&Math.abs(Number(B[b+RY])-(-anchorY*h))<=eps;
}
// TaggedText keeps a module-level PIXI.Text solely as a font-metrics sizer and reads
// `text.context.font`.  Supplying a semantic context preserves that contract without creating a
// DOM canvas or a CanvasRenderingContext2D.  CoreText still owns all actual layout/raster work.
function NativeTextMeasureContext(owner){this.owner=owner;this._font='';}
Object.defineProperty(NativeTextMeasureContext.prototype,'font',{configurable:true,get:function(){var st=this.owner&&this.owner.style;return st&&typeof st.toFontString==='function'?st.toFontString():(this._font||'26px Arial');},set:function(v){this._font=String(v||'');}});
function installNativeTextStyleHooks(){var p=PIXI.TextStyle&&PIXI.TextStyle.prototype;if(!p||p.__nativeOwnerHooks)return;p.__nativeOwnerHooks=true;Object.getOwnPropertyNames(p).forEach(function(k){var d=Object.getOwnPropertyDescriptor(p,k);if(!d||!d.set||d.configurable===false)return;var set=d.set;d.set=function(v){set.call(this,v);var owners=this.__nativeTextOwners;if(owners)owners.forEach(function(o){if(o&&!o._destroyed)o._invalidateNativeText();});};Object.defineProperty(p,k,d);});}
// 普通文本/Input 的三宽度协议分发:V2 结构化绑定(mac,携带 boxWidth/horizontalFrame)优先;
// V1 位置参数回退(iOS/Android 未迁移平台,box 恒 0=natural——与历史行为一致)。显式 owner-box
// 契约在 V1 回退下无法生效,记一次性警告(禁止静默降级不留痕)。
function dispatchPlainTextUpdate(payload){
  var cs=globalThis.__textLayoutContractStats;
  var v2=globalThis.__nativeTextUpdateV2;
  if(typeof v2==='function'&&payload.horizontalFrame){if(cs)cs.v2Calls++;return v2(payload.node,payload);}
  if(cs)cs.v1Calls++;
  if(payload.horizontalFrame==='owner-box'&&!globalThis.__plainOwnerBoxV1Warned){globalThis.__plainOwnerBoxV1Warned=1;if(typeof __log==='function')__log('[text-layout] plain owner-box 契约需要 V2 绑定,当前平台按 natural 提交(已记录)');}
  return globalThis.__nativeTextUpdate(payload.node,payload.text,payload.font,payload.fill,payload.stroke,payload.strokeWidth,payload.shadow,payload.shadowBlur,payload.shadowX,payload.shadowY,payload.align,payload.padding,payload.resolution,payload.anchorX,payload.anchorY,payload.wrapWidth,payload.lineHeight,payload.letterSpacing,payload.leading,payload.vAlign,payload.gradientColors,payload.gradientStops,payload.gradientType,payload.ascent,payload.descent,payload.autoSize);
}
function watchNativeTextStyle(style,owner){if(!style||!owner)return style;var owners=style.__nativeTextOwners;if(!owners){owners=new Set();try{Object.defineProperty(style,'__nativeTextOwners',{configurable:true,value:owners});}catch(e){return style;}}owners.add(owner);return style;}
installNativeTextStyleHooks();
function NativeTextProxy(owner){NativeNodeProxy.call(this,{eventMode:'none',interactiveChildren:false});this.UIOwner=owner||null;this._style=null;this._text='';this.dirty=true;this.localStyleID=-1;var textType=PIXI.Text||OriginalText;this._autoResolution=textType.defaultAutoResolution!==false;this._resolution=nativeTextInitialResolution(textType);this._frameWidth=0;this._frameHeight=0;this._textWidth=0;this._textHeight=0;this.$minHeight=-1;this.$minHeightID=-1;this.__nativeTextProxy=true;this.context=new NativeTextMeasureContext(this);this.style=new PIXI.TextStyle();this.setContainer();}
NativeTextProxy.prototype=Object.create(NativeNodeProxy.prototype);NativeTextProxy.prototype.constructor=NativeTextProxy;
NativeTextProxy.prototype._invalidateNativeText=function(){this.dirty=true;this.__nativeTextKey='';if(!preservingNativeDescriptor(this)&&this.__i>0&&R.markVisual)R.markVisual(this);};
// FGUI's owner box is a layout input, not the measured/raster size of this display. In
// particular, fixed-box text can be taller than its owner before business font fitting runs.
// Preserve all measurement fields on facade sync; invalidate only a real owner-size change.
// This hook only publishes inputs/dirty state, never renderNow/applyStyle or a raster request.
NativeTextProxy.prototype._syncFguiLayoutSize=function(width,height,sizeChanged){
  var owner=this.UIOwner;
  if(!sizeChanged||!owner||owner.$textField!==this)return;
  // TaggedText owns an independent named-style wrap box; dynamic PIXI.Text width setters
  // scale content. Only ordinary FGUI text accepts its owner's width as a frame constraint.
  if(!this.__nativeDynamicText&&!this.__nativeAttributedTextProxy&&!this.__nativeMultiStyleText&&!owner.$bitmapFont){
    var st=this.style,wordWrap=!owner.$widthAutoSize&&!owner.$singleLine;
    var layoutWidth=(wordWrap||(owner.$autoSize|0)===0)?Math.ceil(width):10000;
    this.width=layoutWidth;
    if(st){
      if(st.wordWrapWidth!==layoutWidth)st.wordWrapWidth=layoutWidth;
      if(st.wordWrap!==wordWrap)st.wordWrap=wordWrap;
      if(st.breakWords!==wordWrap)st.breakWords=wordWrap;
    }
  }
  this._invalidateNativeText();
};
NativeTextProxy.prototype._syncNativeTextAutoResolution=function(){if(!this._autoResolution)return false;var resolution=nativeTextAutomaticResolution();if(this._resolution===resolution)return false;this._resolution=resolution;this.dirty=true;this.__nativeTextKey='';return true;};
NativeTextProxy.prototype._syncNativeText=function(force){nativeMutationReadBarrier();if(this._destroyed||preservingNativeDescriptor(this)||typeof globalThis.__nativeTextUpdate!=='function')return null;this._syncNativeTextAutoResolution();var st=this.style||{},sid=st.styleID||0;
  // Pixi/Canvas treats CR and CRLF as hard line breaks. CoreText does not consistently do so
  // when fed the raw JavaScript string, which collapsed authored button labels such as
  // "Regular\rMissions" onto one line. Keep the public value untouched and normalise only the
  // native layout input so all PIXI.Text and FairyGUI text fields share the browser contract.
  var nativeText=this._text.replace(/\r\n?/g,'\n'),fillGradient=nativeTextGradient(st);
  // 三宽度布局协议(docs/text-layout-contract-v2.md):plain 的 frame/boxWidth 由契约模块统一决议
  // (现状=natural 的显式化,fgui 对齐仍由外部节点定位;显式 owner-box 经 V2 绑定把行对齐收进
  // surface)。决议进缓存键:owner 宽变化时 owner-box 正确重排,policy 本身不随尺寸改变。
  var contractOwner=this.UIOwner,contractIsFgui=!!(contractOwner&&contractOwner.$textField===this);
  var contractOwnerWidth=contractIsFgui?Number(contractOwner.$width!==undefined?contractOwner.$width:contractOwner.width):0;
  // autoSize is mutable independently of TextStyle.  It participates in Native's external
  // natural-surface offset (Shrink applies the live display scale; None does not), so it must be
  // part of both the raster ownership hand-off and this cache key.  Otherwise an authored
  // autoSize=None button changed to Shrink by business code keeps the XML mode in C++ and a long
  // localisation receives the unscaled negative centre offset.
  var contractAutoSize=contractIsFgui?(contractOwner.$autoSize|0):-1;
  var contractModule=globalThis.__nativeTextLayoutContractModule;
  var plainContract=contractModule&&contractModule.resolvePlain?contractModule.resolvePlain(this,contractOwner,st,contractIsFgui,contractOwnerWidth):{horizontalFrame:'natural',boxWidth:0,inferenceSource:'module-missing'};
  var anchorX=Number(this._anchor&&this._anchor._x)||0,anchorY=Number(this._anchor&&this._anchor._y)||0;
  var key=nativeText+'|'+sid+'|'+this._resolution+'|'+this._frameWidth+'x'+this._frameHeight+'|hf:'+plainContract.horizontalFrame+':'+(plainContract.boxWidth||0)+'|as:'+contractAutoSize+'|gradient:'+(fillGradient?(fillGradient.colors+'@'+fillGradient.stops+'@'+fillGradient.type):'none')+'|anchor:'+anchorX+','+anchorY;
  if(!force&&!this.dirty&&this.__nativeTextKey===key){
    if(nativeTextGeometryMatches(this,nativeText,anchorX,anchorY))return this.__nativeMetrics;
    var repairStats=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});repairStats.nativeTextGeometryRepairs=(repairStats.nativeTextGeometryRepairs||0)+1;
  }
  var font=typeof st.toFontString==='function'?st.toFontString():((st.fontSize||26)+'px '+(st.fontFamily||'Arial')),fontMetrics=nativeTextFontMetrics(font),sd=Number(st.dropShadowDistance)||0,sa=Number(st.dropShadowAngle)||0,pad=Number(st.padding)||0;
  // The raster request already owns the live horizontal alignment. Carry the owning FairyGUI
  // field's live vertical alignment in the same hand-off so Native never positions a fresh
  // natural-size surface with stale package-XML layout metadata.
  var owner=this.UIOwner,verticalAlign='';
  if(owner&&owner.$textField===this&&!owner.$bitmapFont)verticalAlign=(owner.$verticalAlign|0)===1?'middle':((owner.$verticalAlign|0)===2?'bottom':'top');
  var plainBoxHeight=plainContract.horizontalFrame==='owner-box'&&contractIsFgui?Math.max(0,Number(contractOwner.$height!==undefined?contractOwner.$height:contractOwner.height)||0):0;
  var m=dispatchPlainTextUpdate({node:this.__i,text:nativeText,font:font,fill:nativeTextColor(st.fill),stroke:nativeTextColor(st.stroke),strokeWidth:Number(st.strokeThickness)||0,shadow:st.dropShadow?nativeTextColor(st.dropShadowColor):'transparent',shadowBlur:Number(st.dropShadowBlur)||0,shadowX:st.dropShadow?sd*Math.cos(sa):0,shadowY:st.dropShadow?sd*Math.sin(sa):0,align:st.align||'left',padding:pad,resolution:Number(this._resolution)||1,anchorX:anchorX,anchorY:anchorY,wrapWidth:plainContract.resolvedWrapWidth!==undefined?plainContract.resolvedWrapWidth:(st.wordWrap?(Number(st.wordWrapWidth)||0):0),lineHeight:Number(st.lineHeight)||0,letterSpacing:Number(st.letterSpacing)||0,leading:Number(st.leading)||0,vAlign:verticalAlign,gradientColors:fillGradient?fillGradient.colors:'',gradientStops:fillGradient?fillGradient.stops:'',gradientType:fillGradient?fillGradient.type:0,ascent:fontMetrics.ascent,descent:fontMetrics.descent,boxWidth:plainContract.boxWidth||0,boxHeight:plainBoxHeight,horizontalFrame:plainContract.horizontalFrame,autoSize:contractAutoSize});
  if(!m)return null;var mw=Number(m.width),mh=Number(m.height),ml=Number(m.lineHeight);if(!isFinite(mw)||mw<0||!isFinite(mh)||mh<0||!isFinite(ml)||ml<0){var ns=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});ns.invalidNativeTextMetrics=(ns.invalidNativeTextMetrics||0)+1;if((ns.invalidNativeTextMetricWarnings||0)<4&&typeof __log==='function'){ns.invalidNativeTextMetricWarnings=(ns.invalidNativeTextMetricWarnings||0)+1;__log('[native-text] invalid metrics text='+JSON.stringify(this._text)+' width='+m.width+' height='+m.height+' lineHeight='+m.lineHeight);}mw=isFinite(mw)&&mw>0?mw:0;mh=isFinite(mh)&&mh>0?mh:0;ml=isFinite(ml)&&ml>0?ml:0;}m.width=mw;m.height=mh;m.lineHeight=ml;this._nativeWidth=mw;this._nativeHeight=mh;this._textWidth=Math.max(0,mw-pad*2);this._textHeight=Math.max(0,mh-pad*2);this.__nativeTextKey=key;this.__nativeMetrics=m;this.localStyleID=sid;this.dirty=false;this._boundsID++;
  // The native FGUI factory can adopt an authored text node without running GTextField.renderNow.
  // A later same-value title assignment is deliberately ignored by FairyGUI, while a business
  // textWidth read still asks this facade for fresh natural CoreText metrics. Mirror those metrics
  // into the owning GTextField and execute its normal alignment step here; otherwise center/right
  // and middle/bottom fields keep the allocation-time zero offset and render at the box origin.
  // This is the same gutter/accounting used by GTextField.renderNow, and applies to every adopted
  // ordinary text field rather than any particular popup or button.
  if(owner&&owner.$textField===this&&!owner.$bitmapFont){
    var ownerTextWidth=Math.ceil(this._textWidth),ownerTextHeight=Math.ceil(this._textHeight);
    if(ownerTextWidth>0)ownerTextWidth+=4;
    if(ownerTextHeight>0)ownerTextHeight+=4;
    // autoSize=Shrink:$widthAutoSize 对它也为 true(fgui.umd setter :12573),但 H5 renderNow 的
    // Shrink 分支(:12646)不收缩字段——w/h 保持 authored,仅内容缩放+layoutAlign 在 authored 框内
    // 对齐(PopupHomepage 勋章名 155 框/内容 96→offset 29 居中;收缩到 96 则 offset=0 贴左)。
    // $textHeight 的超高钳制同样只在 H5 的非 Shrink else 分支。BMFont 桥同款特判已先行。
    var shrinkKeep=(owner.$autoSize|0)===3;
    owner.$textWidth=ownerTextWidth;
    owner.$textHeight=!shrinkKeep&&!owner.$heightAutoSize&&ownerTextHeight>Number(owner.height)?Number(owner.height):ownerTextHeight;
    // ≙ H5 GTextField.renderNow 的 updateBounds 段(fgui.umd:12846-12851):autoSize 字段必须经
    // setSize 把自然宽高回写 GObject——pivot 补偿(GObject.setSize 按 pivot.x*diffWidth 移 x)、
    // 依赖本字段的 relations(width-width 等)、advanced group 包围盒与其 center relation 全挂在
    // 这条链上。此前 plain 缺此环(bitmap 桥 2047 行已有同款),coinBooster 的 upTo_font 划线组
    // (pivot 0.5+width-width 线+组 center relation)整体偏移即此断链。$updatingSize 门同 H5,
    // 防 setSize→handleSizeChanged→render 递归。
    if(!shrinkKeep&&(owner.$widthAutoSize||owner.$heightAutoSize)&&typeof owner.setSize==='function'&&!owner.$updatingSize){
      var sizedW=owner.$widthAutoSize?ownerTextWidth:Number(owner.width)||0;
      var sizedH=owner.$heightAutoSize?owner.$textHeight:Number(owner.height)||0;
      owner.$updatingSize=true;
      try{owner.setSize(sizedW,sizedH);}finally{owner.$updatingSize=false;}
    }
    // Stock GTextField.renderNow applies the Shrink display scale after metrics/updateBounds and
    // before layoutAlign.  Lazy native facades enter here without renderNow; dispatch above has
    // also replaced the authored fitted rectangle with a natural-size facade surface.  Restore
    // that missing display-layer step before alignment so long, same-value authored/localised text
    // cannot remain at scale=1 and overflow its fixed box.
    if(shrinkKeep&&typeof owner.shrinkTextField==='function')owner.shrinkTextField();
    if(typeof owner.layoutAlign==='function')owner.layoutAlign();
  }
  R.markBounds();return m;};
Object.defineProperties(NativeTextProxy.prototype,{
  style:{get:function(){return this._style;},set:function(v){if(this._style&&this._style.__nativeTextOwners)this._style.__nativeTextOwners.delete(this);this._style=watchNativeTextStyle(v instanceof PIXI.TextStyle?v:new PIXI.TextStyle(v||{}),this);this.localStyleID=-1;this._invalidateNativeText();}},
  // FGUI mutates TextStyle in place, then reassigns `text` even when the string did not change.
  // Always invalidate here so style-only changes (fill/font/alignment) also reach native CoreText.
  text:{get:function(){return this._text;},set:function(v){v=String(v==null?'':v);this._text=v;this._invalidateNativeText();}},
  resolution:{get:function(){return this._resolution;},set:function(v){this._autoResolution=false;v=nativeTextClampResolution(v);if(this._resolution!==v){this._resolution=v;this._invalidateNativeText();}}},
  width:{get:function(){return this._frameWidth;},set:function(v){v=Math.max(0,Number(v)||0);if(this._frameWidth!==v){this._frameWidth=v;this._invalidateNativeText();}}},
  height:{get:function(){return this._frameHeight;},set:function(v){v=Math.max(0,Number(v)||0);if(this._frameHeight!==v){this._frameHeight=v;this._invalidateNativeText();}}},
  textWidth:{get:function(){this.updateText(true);return this._textWidth;},set:function(v){if(this.style&&this.style.wordWrapWidth!==v){this.style.wordWrapWidth=v;this._invalidateNativeText();}}},
  textHeight:{get:function(){this.updateText(true);return Math.max(this._textHeight,this.$minHeight>0?this.$minHeight:0);},set:function(){}},
  filters:{get:function(){return this._filters;},set:function(v){this._filters=v||null;if(this.__i>0){R.writeVisual(this);this._syncNativeText(true);}}},
  mask:{get:function(){return this._mask;},set:function(v){this._mask=v||null;if(this.__i>0){R.writeVisual(this);this._syncNativeText(true);}}}
});
NativeTextProxy.prototype.$updateMinHeight=function(){var sid=this.style&&this.style.styleID||0;if(this.$minHeightID===sid&&this.$minHeight>0)return;try{var fm=PIXI.TextMetrics.measureFont(this.style.toFontString());this.$minHeight=Math.ceil(fm.fontSize||fm.ascent+fm.descent||Number(this.style.fontSize)||12);}catch(e){this.$minHeight=Math.ceil(Number(this.style.fontSize)||12);}this.$minHeightID=sid;};
NativeTextProxy.prototype.updateText=function(respectDirty){this._syncNativeTextAutoResolution();var sid=this.style&&this.style.styleID||0;if(this.localStyleID!==sid)this.dirty=true;if(respectDirty&& !this.dirty&&this.__nativeMetrics)return;this._syncNativeText(false);};
NativeTextProxy.prototype.getLocalBounds=function(rect){this.updateText(true);rect=rect||new PIXI.Rectangle();rect.x=0;rect.y=0;rect.width=this._nativeWidth;rect.height=this._nativeHeight;return rect;};
NativeTextProxy.prototype.calculateBounds=function(){this.updateText(true);return NativeNodeProxy.prototype.calculateBounds.call(this);};
NativeTextProxy.prototype.render=function(renderer){if(renderer&&this.visible!==false&&this.renderable!==false)rejectPixiRender('text',this);};
var _destroyNativeNode=NativeTextProxy.prototype.destroy;
NativeTextProxy.prototype.destroy=function(options){if(this._destroyed)return;if(this._style&&this._style.__nativeTextOwners)this._style.__nativeTextOwners.delete(this);if(this.__nativeDynamicText){var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicTextsLive=Math.max(0,(st.dynamicTextsLive||0)-1);}return _destroyNativeNode.call(this,options);};

// Business code also creates PIXI.Text directly (reward values, timers, guide labels). Keep the
// public Pixi type/prototype contract, but construct the same CoreText-backed lightweight handle
// as FGUI. No Canvas, Texture, Sprite vertex buffer or retained Pixi display subtree is allocated.
function NativeDynamicText(text,style){shadowPixiTransformAccessors(this);NativeTextProxy.call(this,null);this.__nativeDynamicText=true;var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicTextsCreated=(st.dynamicTextsCreated||0)+1;st.dynamicTextsLive=(st.dynamicTextsLive||0)+1;this.style=style||{};this.text=text;this._syncNativeText(false);}
NativeDynamicText.prototype=Object.create(OriginalText.prototype);installPixiBase(NativeDynamicText.prototype,OriginalText.prototype);
Object.getOwnPropertyNames(NativeTextProxy.prototype).forEach(function(k){if(k==='constructor'||k==='width'||k==='height')return;var existing=Object.getOwnPropertyDescriptor(NativeDynamicText.prototype,k);if(existing&&existing.configurable===false)return;Object.defineProperty(NativeDynamicText.prototype,k,Object.getOwnPropertyDescriptor(NativeTextProxy.prototype,k));});
NativeDynamicText.prototype.constructor=NativeDynamicText;
Object.defineProperties(NativeDynamicText.prototype,{
  width:{configurable:true,get:function(){this.updateText(true);var scale=Number(this.scale&&this.scale.x),width=Number(this._nativeWidth);return (isFinite(scale)?Math.abs(scale):1)*(isFinite(width)&&width>0?width:0);},set:function(v){this.updateText(true);var current=Number(this.scale&&this.scale.x),sign=isFinite(current)&&current<0?-1:1;this.scale.x=this._nativeWidth?sign*(Number(v)||0)/this._nativeWidth:sign;this._width=Number(v)||0;}},
  height:{configurable:true,get:function(){this.updateText(true);var scale=Number(this.scale&&this.scale.y),height=Number(this._nativeHeight);return (isFinite(scale)?Math.abs(scale):1)*(isFinite(height)&&height>0?height:0);},set:function(v){this.updateText(true);var current=Number(this.scale&&this.scale.y),sign=isFinite(current)&&current<0?-1:1;this.scale.y=this._nativeHeight?sign*(Number(v)||0)/this._nativeHeight:sign;this._height=Number(v)||0;}}
});
NativeDynamicText.prototype.updateTexture=function(){return this._syncNativeText(true);};
NativeDynamicText.defaultResolution=OriginalText.defaultResolution;NativeDynamicText.defaultAutoResolution=OriginalText.defaultAutoResolution;
try{Object.setPrototypeOf(NativeDynamicText,OriginalText);}catch(_dte){}
try{Object.defineProperty(PIXI,'Text',{configurable:true,writable:true,value:NativeDynamicText});}catch(_dte2){try{PIXI.Text=NativeDynamicText;}catch(_dte3){}}

// Native-owned TilingSprite facade. FGUI only needs a texture, size, tint/flip and the two
// tile-transform points; the repeated geometry is generated and retained by the host.
function NativeTilePoint(owner,kind,x,y){this.owner=owner;this.kind=kind;this._x=x;this._y=y;}
Object.defineProperties(NativeTilePoint.prototype,{
  x:{get:function(){return this._x;},set:function(v){v=Number(v);if(!isFinite(v))v=this.kind==='scale'?1:0;if(this._x!==v){this._x=v;this.owner._syncNativeTiling();}}},
  y:{get:function(){return this._y;},set:function(v){v=Number(v);if(!isFinite(v))v=this.kind==='scale'?1:0;if(this._y!==v){this._y=v;this.owner._syncNativeTiling();}}}
});
NativeTilePoint.prototype.set=function(x,y){if(y===undefined)y=x;x=Number(x);y=Number(y);if(!isFinite(x))x=this.kind==='scale'?1:0;if(!isFinite(y))y=this.kind==='scale'?1:0;var changed=this._x!==x||this._y!==y;this._x=x;this._y=y;if(changed)this.owner._syncNativeTiling();return this;};
NativeTilePoint.prototype.copyFrom=function(p){return this.set(p.x,p.y);};
function NativeTilingProxy(owner,texture){
  NativeNodeProxy.call(this,{eventMode:'none',interactiveChildren:false});this.UIOwner=owner||null;this.__nativeTilingProxy=true;
  this.tilePosition=new NativeTilePoint(this,'position',0,0);this.tileScale=new NativeTilePoint(this,'scale',1,1);this.tilePivot=new NativeTilePoint(this,'pivot',0,0);this.tileSkew=new NativeTilePoint(this,'skew',0,0);
  var self=this,tt={position:this.tilePosition,scale:this.tileScale,pivot:this.tilePivot,skew:this.tileSkew,_rotation:0};Object.defineProperty(tt,'rotation',{configurable:true,get:function(){return this._rotation;},set:function(v){v=Number(v)||0;if(this._rotation!==v){this._rotation=v;self._syncNativeTiling();}}});this.tileTransform=tt;this._uvRespectAnchor=false;
  if(texture)this.texture=texture;else this.setContainer();
}
NativeTilingProxy.prototype=Object.create(NativeNodeProxy.prototype);NativeTilingProxy.prototype.constructor=NativeTilingProxy;
Object.defineProperty(NativeTilingProxy.prototype,'uvRespectAnchor',{configurable:true,get:function(){return this._uvRespectAnchor;},set:function(v){v=!!v;if(this._uvRespectAnchor!==v){this._uvRespectAnchor=v;this._syncNativeTiling();}}});
NativeTilingProxy.prototype._refreshSprite=function(){this._syncNativeTiling();};
NativeTilingProxy.prototype._syncNativeTiling=function(){
  if(this._destroyed||!(this.__i>0))return;var tex=this._texture,orig=tex&&tex.orig,bt=tex&&tex.baseTexture;
  if(!tex||!orig){this.setContainer();return;}var id=R.textureId(tex)|0,u=tex._uvs;if(!u&&tex.updateUvs)try{tex.updateUvs();u=tex._uvs;}catch(e){}if(!id||!u)return;
  if(typeof globalThis.__sgWriteTiling==='function'){globalThis.__sgWriteTiling(this);B[this.__i*ST+RENDERABLE]=this._renderable===false?0:1;R.writeHitMeta(this);R.markBounds();return;}
  if(typeof globalThis.__nativeTiling!=='function')return;
  var uv=[u.x0,u.y0,u.x1,u.y1,u.x2,u.y2,u.x3,u.y3];
  if(this._flipX){var a0=uv[0],a1=uv[1],a6=uv[6],a7=uv[7];uv[0]=uv[2];uv[1]=uv[3];uv[2]=a0;uv[3]=a1;uv[6]=uv[4];uv[7]=uv[5];uv[4]=a6;uv[5]=a7;}
  if(this._flipY){var b0=uv[0],b1=uv[1],b2=uv[2],b3=uv[3];uv[0]=uv[6];uv[1]=uv[7];uv[2]=uv[4];uv[3]=uv[5];uv[6]=b0;uv[7]=b1;uv[4]=b2;uv[5]=b3;}
  this._premultiply=(bt&&bt.alphaMode===0)?1:0;
  __nativeTiling(this.__i,id,this._nativeWidth,this._nativeHeight,Math.abs((orig.width||1)*this.tileScale._x),Math.abs((orig.height||1)*this.tileScale._y),this.tilePosition._x,this.tilePosition._y,this._anchor._x||0,this._anchor._y||0,uv[0],uv[1],uv[2],uv[3],uv[4],uv[5],uv[6],uv[7],this._tint,this._blendMode,this._premultiply);
  B[this.__i*ST+RENDERABLE]=this._renderable===false?0:1;R.writeHitMeta(this);R.markBounds();
};
['tint','blendMode','flipX','flipY'].forEach(function(name){var d=Object.getOwnPropertyDescriptor(NativeNodeProxy.prototype,name);Object.defineProperty(NativeTilingProxy.prototype,name,{configurable:true,get:d.get,set:function(v){d.set.call(this,v);this._syncNativeTiling();}});});

function NativeDynamicTilingSprite(texture,width,height){
  shadowPixiTransformAccessors(this);Object.defineProperties(this,{tilePosition:{configurable:true,writable:true,value:null},tileScale:{configurable:true,writable:true,value:null},tilePivot:{configurable:true,writable:true,value:null},tileSkew:{configurable:true,writable:true,value:null},tileTransform:{configurable:true,writable:true,value:null}});var options=width&&typeof width==='object'?width:null;NativeTilingProxy.call(this,null,texture||PIXI.Texture.EMPTY);this.__nativeDynamicTiling=true;
  var orig=this._texture&&this._texture.orig,w=options?options.width:width,h=options?options.height:height;this._nativeWidth=this._width=Math.max(0,Number(w)||(orig&&orig.width)||100);this._nativeHeight=this._height=Math.max(0,Number(h)||(orig&&orig.height)||100);if(options&&options.uvRespectAnchor!==undefined)this._uvRespectAnchor=!!options.uvRespectAnchor;this._syncNativeTiling();
  var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicTilingsCreated=(st.dynamicTilingsCreated||0)+1;st.dynamicTilingsLive=(st.dynamicTilingsLive||0)+1;
}
if(OriginalTilingSprite){
  NativeDynamicTilingSprite.prototype=Object.create(OriginalTilingSprite.prototype);installPixiBase(NativeDynamicTilingSprite.prototype,OriginalTilingSprite.prototype);
  Object.getOwnPropertyNames(NativeTilingProxy.prototype).forEach(function(k){if(k==='constructor')return;var existing=Object.getOwnPropertyDescriptor(NativeDynamicTilingSprite.prototype,k);if(existing&&existing.configurable===false)return;Object.defineProperty(NativeDynamicTilingSprite.prototype,k,Object.getOwnPropertyDescriptor(NativeTilingProxy.prototype,k));});NativeDynamicTilingSprite.prototype.constructor=NativeDynamicTilingSprite;
  var destroyDynamicTiling=NativeDynamicTilingSprite.prototype.destroy;NativeDynamicTilingSprite.prototype.destroy=function(options){if(this._destroyed)return;var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicTilingsLive=Math.max(0,(st.dynamicTilingsLive||0)-1);return destroyDynamicTiling.call(this,options);};
  try{Object.setPrototypeOf(NativeDynamicTilingSprite,OriginalTilingSprite);}catch(_dte0){}
  installNativeHasInstance(NativeDynamicTilingSprite,OriginalTilingSprite,'__nativeTilingProxy');
  try{Object.defineProperty(PIXI,'TilingSprite',{configurable:true,writable:true,value:NativeDynamicTilingSprite});}catch(_dtr){try{PIXI.TilingSprite=NativeDynamicTilingSprite;}catch(_dtr2){}}
}

// store only border widths, target size and a texture handle; the 4x4 grid is emitted natively.
function NativeDynamicNineSlicePlane(texture,leftWidth,topHeight,rightWidth,bottomHeight){
  shadowPixiTransformAccessors(this);NativeNodeProxy.call(this,{eventMode:'auto',interactiveChildren:true});this.__nativeNineProxy=true;this.__nativeDynamicNineSlice=true;this.isSprite=false;this._tiledSlices=0;texture=texture||PIXI.Texture.EMPTY;var borders=texture.defaultBorders||{},orig=texture.orig||{};
  this._origWidth=Number(orig.width)||0;this._origHeight=Number(orig.height)||0;this._nativeWidth=this._width=this._origWidth;this._nativeHeight=this._height=this._origHeight;this._leftWidth=leftWidth==null?(borders.left==null?10:borders.left):Number(leftWidth)||0;this._rightWidth=rightWidth==null?(borders.right==null?10:borders.right):Number(rightWidth)||0;this._topHeight=topHeight==null?(borders.top==null?10:borders.top):Number(topHeight)||0;this._bottomHeight=bottomHeight==null?(borders.bottom==null?10:borders.bottom):Number(bottomHeight)||0;this.texture=texture;this._refresh();
  var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicNineSlicesCreated=(st.dynamicNineSlicesCreated||0)+1;st.dynamicNineSlicesLive=(st.dynamicNineSlicesLive||0)+1;
}
if(OriginalNineSlicePlane){
  NativeDynamicNineSlicePlane.prototype=Object.create(OriginalNineSlicePlane.prototype);installPixiBase(NativeDynamicNineSlicePlane.prototype,OriginalNineSlicePlane.prototype);NativeDynamicNineSlicePlane.prototype.constructor=NativeDynamicNineSlicePlane;
  var nineTextureDescriptor=Object.getOwnPropertyDescriptor(NativeNodeProxy.prototype,'texture');
  Object.defineProperties(NativeDynamicNineSlicePlane.prototype,{
    texture:{configurable:true,get:nineTextureDescriptor.get,set:function(v){var old=this._texture;if(old&&old.off)old.off('update',this.textureUpdated,this);nineTextureDescriptor.set.call(this,v||PIXI.Texture.EMPTY);var tex=this._texture;if(tex&&tex.baseTexture&&!tex.baseTexture.valid&&tex.once)tex.once('update',this.textureUpdated,this);else this.textureUpdated();}},
    width:{configurable:true,get:function(){return this._nativeWidth;},set:function(v){this._nativeWidth=this._width=Math.max(0,Number(v)||0);this._refresh();}},
    height:{configurable:true,get:function(){return this._nativeHeight;},set:function(v){this._nativeHeight=this._height=Math.max(0,Number(v)||0);this._refresh();}},
    leftWidth:{configurable:true,get:function(){return this._leftWidth;},set:function(v){this._leftWidth=Math.max(0,Number(v)||0);this._refresh();}},
    rightWidth:{configurable:true,get:function(){return this._rightWidth;},set:function(v){this._rightWidth=Math.max(0,Number(v)||0);this._refresh();}},
    topHeight:{configurable:true,get:function(){return this._topHeight;},set:function(v){this._topHeight=Math.max(0,Number(v)||0);this._refresh();}},
    bottomHeight:{configurable:true,get:function(){return this._bottomHeight;},set:function(v){this._bottomHeight=Math.max(0,Number(v)||0);this._refresh();}},
    vertices:{configurable:true,get:function(){var sx=this._getMinScale(),x0=0,x1=this._leftWidth*sx,x2=this._nativeWidth-this._rightWidth*sx,x3=this._nativeWidth,y0=0,y1=this._topHeight*sx,y2=this._nativeHeight-this._bottomHeight*sx,y3=this._nativeHeight,out=new Float32Array(32),xs=[x0,x1,x2,x3],ys=[y0,y1,y2,y3],n=0;for(var y=0;y<4;y++)for(var x=0;x<4;x++){out[n++]=xs[x];out[n++]=ys[y];}return out;},set:function(){this._refresh();}}
  });
  NativeDynamicNineSlicePlane.prototype._getMinScale=function(){var sw=this._leftWidth+this._rightWidth,sh=this._topHeight+this._bottomHeight,w=this._nativeWidth>sw?1:(sw?this._nativeWidth/sw:1),h=this._nativeHeight>sh?1:(sh?this._nativeHeight/sh:1);return Math.min(w,h);};
  NativeDynamicNineSlicePlane.prototype.textureUpdated=function(){var orig=this._texture&&this._texture.orig;if(orig){this._origWidth=Number(orig.width)||0;this._origHeight=Number(orig.height)||0;}this._refresh();};
  NativeDynamicNineSlicePlane.prototype.updateHorizontalVertices=function(){this._refresh();};NativeDynamicNineSlicePlane.prototype.updateVerticalVertices=function(){this._refresh();};
  NativeDynamicNineSlicePlane.prototype._refresh=function(){if(this._destroyed)return;this._refreshSprite();};
  NativeDynamicNineSlicePlane.prototype.destroy=function(options){if(this._destroyed)return;var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicNineSlicesLive=Math.max(0,(st.dynamicNineSlicesLive||0)-1);if(this._texture&&this._texture.off)this._texture.off('update',this.textureUpdated,this);return NativeNodeProxy.prototype.destroy.call(this,options);};
  try{Object.setPrototypeOf(NativeDynamicNineSlicePlane,OriginalNineSlicePlane);}catch(_nse){}
  installNativeHasInstance(NativeDynamicNineSlicePlane,OriginalNineSlicePlane,'__nativeNineProxy');
  try{Object.defineProperty(PIXI,'NineSlicePlane',{configurable:true,writable:true,value:NativeDynamicNineSlicePlane});}catch(_nsr){try{PIXI.NineSlicePlane=NativeDynamicNineSlicePlane;}catch(_nsr2){}}
}

// BitmapText without Container/Mesh/Geometry retention. Layout is compatible with Pixi's bitmap
// font metrics; the host receives one compact glyph stream and owns all page quads.
var OriginalBitmapText=PIXI.BitmapText;
var NativeBitmapTextWaitingForFont=new Set();
// Bitmap fonts can be installed after their display objects are constructed. The browser renderer
// naturally validates those objects on its next tree traversal, but native main-screen rendering
// has no Pixi traversal. Wake every waiting proxy when any font install makes its requested name
// available. This is deliberately font-generic; package and machine code remain unaware of it.
if(PIXI.BitmapFont&&typeof PIXI.BitmapFont.install==='function'&&!PIXI.BitmapFont.__nativeBitmapTextInstallHook){
  var installNativeBitmapFont=PIXI.BitmapFont.install;
  PIXI.BitmapFont.install=function(){var font=installNativeBitmapFont.apply(this,arguments);NativeBitmapTextWaitingForFont.forEach(function(o){if(!o||o._destroyed){NativeBitmapTextWaitingForFont.delete(o);return;}var data=PIXI.BitmapFont.available&&PIXI.BitmapFont.available[o._fontName];if(data){NativeBitmapTextWaitingForFont.delete(o);o.dirty=true;o.__nativeBitmapPendingAttempts=0;o._syncIfReady();}});return font;};
  try{Object.defineProperty(PIXI.BitmapFont,'__nativeBitmapTextInstallHook',{configurable:true,value:true});}catch(_bfh){PIXI.BitmapFont.__nativeBitmapTextInstallHook=true;}
}
function BitmapAnchor(owner,x,y){this.owner=owner;this._x=x||0;this._y=y||0;}
Object.defineProperties(BitmapAnchor.prototype,{x:{get:function(){return this._x;},set:function(v){v=Number(v)||0;if(this._x!==v){this._x=v;this.owner.dirty=true;this.owner._syncIfReady();}}},y:{get:function(){return this._y;},set:function(v){v=Number(v)||0;if(this._y!==v){this._y=v;this.owner.dirty=true;this.owner._syncIfReady();}}}});
BitmapAnchor.prototype.set=function(x,y){if(y===undefined)y=x;x=Number(x)||0;y=Number(y)||0;if(this._x!==x||this._y!==y){this._x=x;this._y=y;this.owner.dirty=true;this.owner._syncIfReady();}return this;};BitmapAnchor.prototype.copyFrom=function(p){return this.set(p.x,p.y);};BitmapAnchor.prototype.equals=function(p){return !!p&&p.x===this._x&&p.y===this._y;};
function NativeBitmapText(text,style){
  shadowPixiTransformAccessors(this);NativeNodeProxy.call(this,{eventMode:'none',interactiveChildren:false});style=style||{};var defs=OriginalBitmapText.styleDefaults||{};
  this.__nativeBitmapText=true;this._text='';this._fontName=style.fontName;this._fontSize=style.fontSize;this._align=style.align||defs.align||'left';this._maxWidth=Number(style.maxWidth===undefined?defs.maxWidth:style.maxWidth)||0;this._letterSpacing=Number(style.letterSpacing===undefined?defs.letterSpacing:style.letterSpacing)||0;this._tint=style.tint===undefined?(defs.tint===undefined?0xFFFFFF:defs.tint):tintNumber(style.tint);this._roundPixels=!!(PIXI.settings&&PIXI.settings.ROUND_PIXELS);this._resolution=PIXI.settings&&PIXI.settings.RESOLUTION||1;this._autoResolution=true;this._anchor=new BitmapAnchor(this,0,0);this._textWidth=0;this._textHeight=0;this._maxLineHeight=0;this._nativeBitmapBoundsX=0;this._nativeBitmapBoundsY=0;this._nativeBitmapBoundsWidth=0;this._nativeBitmapBoundsHeight=0;this._font=null;this._fontTextures=Object.create(null);this.__nativeBitmapPendingAttempts=0;this.__nativeBitmapPendingTextureCount=0;this.dirty=true;this.setContainer();this.text=text;
}
NativeBitmapText.prototype=Object.create(OriginalBitmapText.prototype);installPixiBase(NativeBitmapText.prototype,OriginalBitmapText.prototype);NativeBitmapText.prototype.constructor=NativeBitmapText;
// PIXI.BitmapText measures glyphs against maxWidth, not an FGUI owner box. Its measured
// extents and requested display size must likewise survive owner geometry publication.
NativeBitmapText.prototype._syncFguiLayoutSize=function(){};
NativeBitmapText.prototype._syncIfReady=function(){var data=PIXI.BitmapFont&&PIXI.BitmapFont.available&&PIXI.BitmapFont.available[this._fontName];if(!data){NativeBitmapTextWaitingForFont.add(this);return this;}NativeBitmapTextWaitingForFont.delete(this);if(this.__i>0)this.validate();return this;};
NativeBitmapText.prototype.validate=function(){nativeMutationReadBarrier();var data=PIXI.BitmapFont&&PIXI.BitmapFont.available&&PIXI.BitmapFont.available[this._fontName];if(!data){NativeBitmapTextWaitingForFont.add(this);throw new Error('Missing BitmapFont "'+this._fontName+'"');}NativeBitmapTextWaitingForFont.delete(this);if(this._font!==data)this.dirty=true;if(this.dirty)this._syncNativeBitmap(data);return this;};
NativeBitmapText.prototype.__nativeTextureReady=function(){this.__nativeBitmapPendingAttempts=0;this.dirty=true;};
NativeBitmapText.prototype._syncNativeBitmap=function(data){
  if(!data)data=PIXI.BitmapFont.available[this._fontName];if(!data||typeof globalThis.__nativeBitmapGlyphs!=='function')return false;var fontSize=this.fontSize,scale=fontSize/data.size,text=String(this._text||' ').replace(/\r\n|\r/g,'\n'),input=Array.from(text),chars=[],lineWidths=[],lineSpaces=[],x=0,y=0,line=0,lastBreak=-1,lastBreakWidth=0,spacesRemoved=0,spaceCount=0,maxWidth=this._maxWidth*data.size/fontSize,maxLineWidth=0,lastLineWidth=0,maxLineHeight=0,prev=null;
  for(var i=0;i<input.length;i++){var ch=input[i],code=ch.codePointAt(0);if(/\s/.test(ch)){lastBreak=i;lastBreakWidth=lastLineWidth;spaceCount++;}if(ch==='\n'){lineWidths.push(lastLineWidth);lineSpaces.push(-1);maxLineWidth=Math.max(maxLineWidth,lastLineWidth);line++;spacesRemoved++;x=0;y+=data.lineHeight;prev=null;spaceCount=0;continue;}var cd=data.chars[code];if(!cd)continue;if(prev&&cd.kerning&&cd.kerning[prev])x+=cd.kerning[prev];var gx=Math.round(x+cd.xOffset+this._letterSpacing/2),gy=Math.round(y+cd.yOffset);chars.push({texture:cd.texture,line:line,x:gx,y:gy,prevSpaces:spaceCount});lastLineWidth=gx+Math.max(cd.xAdvance-cd.xOffset,cd.texture.orig.width);x+=cd.xAdvance+this._letterSpacing;maxLineHeight=Math.max(maxLineHeight,cd.yOffset+cd.texture.height);prev=code;
    if(lastBreak!==-1&&maxWidth>0&&x>maxWidth){spacesRemoved++;var at=1+lastBreak-spacesRemoved,remove=1+i-lastBreak;if(at<0)at=0;chars.splice(at,remove);i=lastBreak;lastBreak=-1;lineWidths.push(lastBreakWidth);lineSpaces.push(chars.length?chars[chars.length-1].prevSpaces:0);maxLineWidth=Math.max(maxLineWidth,lastBreakWidth);line++;x=0;y+=data.lineHeight;prev=null;spaceCount=0;}
  }
  var last=input[input.length-1];if(last!=='\n'){if(last&&/\s/.test(last))lastLineWidth=lastBreakWidth;lineWidths.push(lastLineWidth);maxLineWidth=Math.max(maxLineWidth,lastLineWidth);lineSpaces.push(-1);}var offsets=[];for(i=0;i<=line;i++){var off=0,w=lineWidths[i]||0;if(this._align==='right')off=maxLineWidth-w;else if(this._align==='center')off=(maxLineWidth-w)/2;else if(this._align==='justify'&&lineSpaces[i]>=0)off=(maxLineWidth-w)/(lineSpaces[i]||1);offsets[i]=off;}
  this._textWidth=maxLineWidth*scale;this._textHeight=(y+data.lineHeight)*scale;this._maxLineHeight=maxLineHeight*scale;this._nativeWidth=this._width=this._textWidth;this._nativeHeight=this._height=this._textHeight;var records=new Float32Array(chars.length*16),used=Object.create(null),submitted=0,pending=0,rotated=0,ax=this._textWidth*this._anchor._x,ay=this._textHeight*this._anchor._y,boundsMinX=Infinity,boundsMinY=Infinity,boundsMaxX=-Infinity,boundsMaxY=-Infinity;
  for(i=0;i<chars.length;i++){var c=chars[i],tex=c.texture,bt=tex&&tex.baseTexture,uid=bt&&bt.uid;if(!tex||!bt){pending++;continue;}var px=(c.x+offsets[c.line]*(this._align==='justify'?c.prevSpaces:1))*scale;if(this._roundPixels)px=Math.round(px);var py=c.y*scale,orig=tex.orig,trim=tex.trim,gx=0,gy=0,gw=0,gh=0;if(orig){gx=px+(trim?trim.x*scale:0)-ax;gy=py+(trim?trim.y*scale:0)-ay;gw=(trim?trim.width:orig.width)*scale;gh=(trim?trim.height:orig.height)*scale;boundsMinX=Math.min(boundsMinX,gx);boundsMinY=Math.min(boundsMinY,gy);boundsMaxX=Math.max(boundsMaxX,gx+gw);boundsMaxY=Math.max(boundsMaxY,gy+gh);}if(!used[uid]){used[uid]=tex;if(!this._fontTextures[uid])R.retainTexture(tex,this);}var tid=R.textureId(tex)|0,uploadPending=bt.__texDataPending===1&&bt.resource&&!bt.__texRealUp,u=tex._uvs;if(!u&&tex.updateUvs)try{tex.updateUvs();u=tex._uvs;}catch(e){}if(!tid||uploadPending||!u||!orig){pending++;continue;}
    // `frame` is the packed atlas rectangle. Its width/height are swapped for rotated glyphs;
    // using it as display geometry turns digits into apparently unrelated symbols even though
    // the UV corner order is correct. Pixi Sprite geometry is based on the unrotated `orig` and
    // optional trim rectangle, so reproduce those vertices exactly.
    if(tex.rotate)rotated++;
    var o=submitted*16;records[o]=tid;records[o+1]=gx;records[o+2]=gy;records[o+3]=gx+gw;records[o+4]=gy+gh;records[o+5]=u.x0;records[o+6]=u.y0;records[o+7]=u.x1;records[o+8]=u.y1;records[o+9]=u.x2;records[o+10]=u.y2;records[o+11]=u.x3;records[o+12]=u.y3;records[o+13]=this._tint;records[o+14]=1;records[o+15]=bt.alphaMode===0?1:0;submitted++;}
  this._nativeBitmapBoundsX=boundsMinX===Infinity?0:boundsMinX;this._nativeBitmapBoundsY=boundsMinY===Infinity?0:boundsMinY;this._nativeBitmapBoundsWidth=boundsMinX===Infinity?0:Math.max(0,boundsMaxX-boundsMinX);this._nativeBitmapBoundsHeight=boundsMinY===Infinity?0:Math.max(0,boundsMaxY-boundsMinY);
  for(var old in this._fontTextures)if(!used[old])R.releaseTexture(this._fontTextures[old],this);this._fontTextures=used;this.__nativeBitmapSourceText=String(this._text||'');this.__nativeBitmapGlyphCount=chars.length;this.__nativeBitmapUsableCount=submitted;this.__nativeBitmapPendingTextureCount=pending;this.__nativeBitmapRotatedCount=rotated;this._font=data;
  // Never commit a partial/empty glyph stream just because an asynchronously decoded font page
  // has an allocated handle before its pixels are uploaded. Retaining the page above installs its
  // update owner; the bounded retry also drives the texture uploader when no extra update fires.
  if(pending){this.dirty=true;if((this.__nativeBitmapPendingAttempts=(this.__nativeBitmapPendingAttempts||0)+1)<=720&&R.markVisual)R.markVisual(this);return false;}
  var dfw=data.distanceFieldType&&data.distanceFieldType!=='none'?(Number(data.distanceFieldRange)||0)*scale*(this._resolution||1):0,packed=submitted===chars.length?records:records.subarray(0,submitted*16),accepted=__nativeBitmapGlyphs(this.__i,packed,this._blendMode||0,dfw);if(accepted===false){this.dirty=true;if((this.__nativeBitmapPendingAttempts=(this.__nativeBitmapPendingAttempts||0)+1)<=720&&R.markVisual)R.markVisual(this);return false;}this.__nativeBitmapSubmittedCount=submitted;this.__nativeBitmapPendingAttempts=0;this.dirty=false;this._boundsID++;R.markBounds();return true;
};
Object.defineProperties(NativeBitmapText.prototype,{
  text:{get:function(){return this._text;},set:function(v){v=String(v==null?'':v);if(this._text!==v){this._text=v;this.dirty=true;this._syncIfReady();}}},fontName:{get:function(){return this._fontName;},set:function(v){if(this._fontName!==v){this._fontName=v;this.dirty=true;this._syncIfReady();}}},fontSize:{get:function(){var f=PIXI.BitmapFont&&PIXI.BitmapFont.available&&PIXI.BitmapFont.available[this._fontName];return this._fontSize==null?(f?f.size:0):this._fontSize;},set:function(v){v=Number(v)||0;if(this._fontSize!==v){this._fontSize=v;this.dirty=true;this._syncIfReady();}}},
  align:{get:function(){return this._align;},set:function(v){if(this._align!==v){this._align=v;this.dirty=true;this._syncIfReady();}}},maxWidth:{get:function(){return this._maxWidth;},set:function(v){v=Number(v)||0;if(this._maxWidth!==v){this._maxWidth=v;this.dirty=true;this._syncIfReady();}}},letterSpacing:{get:function(){return this._letterSpacing;},set:function(v){v=Number(v)||0;if(this._letterSpacing!==v){this._letterSpacing=v;this.dirty=true;this._syncIfReady();}}},anchor:{get:function(){return this._anchor;},set:function(v){typeof v==='number'?this._anchor.set(v):this._anchor.copyFrom(v);}},
  tint:{get:function(){return this._tint;},set:function(v){v=tintNumber(v);if(this._tint!==v){this._tint=v;this.dirty=true;this._syncIfReady();}}},roundPixels:{get:function(){return this._roundPixels;},set:function(v){v=!!v;if(this._roundPixels!==v){this._roundPixels=v;this.dirty=true;this._syncIfReady();}}},resolution:{get:function(){return this._resolution;},set:function(v){this._autoResolution=false;v=Number(v)||1;if(this._resolution!==v){this._resolution=v;this.dirty=true;this._syncIfReady();}}},
  textWidth:{get:function(){this.validate();return this._textWidth;}},textHeight:{get:function(){this.validate();return this._textHeight;}},maxLineHeight:{get:function(){this.validate();return this._maxLineHeight;}},width:{get:function(){this.validate();return this.scale.x*this._nativeBitmapBoundsWidth;},set:function(v){this.validate();var w=this._nativeBitmapBoundsWidth;this.scale.x=w?(Number(v)||0)/w:1;}},height:{get:function(){this.validate();return this.scale.y*this._nativeBitmapBoundsHeight;},set:function(v){this.validate();var h=this._nativeBitmapBoundsHeight;this.scale.y=h?(Number(v)||0)/h:1;}},blendMode:{get:function(){return this._blendMode;},set:function(v){v=Number(v)||0;if(this._blendMode!==v){this._blendMode=v;this.dirty=true;this._syncIfReady();}}}
});
NativeBitmapText.prototype.updateText=function(){this.dirty=true;this.validate();};NativeBitmapText.prototype.getLocalBounds=function(rect){this.validate();rect=rect||new PIXI.Rectangle();rect.x=this._nativeBitmapBoundsX;rect.y=this._nativeBitmapBoundsY;rect.width=this._nativeBitmapBoundsWidth;rect.height=this._nativeBitmapBoundsHeight;return rect;};NativeBitmapText.prototype.calculateBounds=function(){this.validate();return NativeNodeProxy.prototype.calculateBounds.call(this);};
NativeBitmapText.prototype.render=function(renderer){if(renderer&&this.visible!==false&&this.renderable!==false)rejectPixiRender('bitmap-text',this);};
var destroyBitmapNode=NativeBitmapText.prototype.destroy;NativeBitmapText.prototype.destroy=function(options){NativeBitmapTextWaitingForFont.delete(this);for(var k in this._fontTextures)R.releaseTexture(this._fontTextures[k],this);this._fontTextures=Object.create(null);return destroyBitmapNode.call(this,options);};
NativeBitmapText.styleDefaults=OriginalBitmapText.styleDefaults;try{Object.setPrototypeOf(NativeBitmapText,OriginalBitmapText);}catch(e){};
try{Object.defineProperty(PIXI,'BitmapText',{configurable:true,writable:true,value:NativeBitmapText});}catch(_bte){try{PIXI.BitmapText=NativeBitmapText;}catch(_bte2){}}

// Container is the largest remaining source of a retained JavaScript display tree.  Business
// layers, TaggedText line buckets and effect holders only need hierarchy/transform/events, all of
// which NativeContainerProxy already implements with numeric native handles.  Replace future
// `new PIXI.Container()` calls while preserving `instanceof PIXI.Container` for Pixi classes that
// were defined before this host module (Sprite, Graphics and Spine all extend OriginalContainer).
function NativeDynamicContainer(options){NativeContainerProxy.call(this,null,options&&typeof options==='object'?options:null);this.__nativeDynamicContainer=true;var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicContainersCreated=(st.dynamicContainersCreated||0)+1;st.dynamicContainersLive=(st.dynamicContainersLive||0)+1;}
NativeDynamicContainer.prototype=Object.create(NativeContainerProxy.prototype);NativeDynamicContainer.prototype.constructor=NativeDynamicContainer;
var destroyDynamicContainer=NativeDynamicContainer.prototype.destroy;NativeDynamicContainer.prototype.destroy=function(options){if(this._destroyed)return;var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicContainersLive=Math.max(0,(st.dynamicContainersLive||0)-1);return destroyDynamicContainer.call(this,options);};
try{Object.setPrototypeOf(NativeDynamicContainer,OriginalContainer);}catch(_dce){}
installNativeHasInstance(NativeDynamicContainer,OriginalContainer,'__nativeContainerProxy');
try{Object.defineProperty(PIXI,'Container',{configurable:true,writable:true,value:NativeDynamicContainer});}catch(_dcr){try{PIXI.Container=NativeDynamicContainer;}catch(_dcr2){}}

// Sprite facade for business images, particles and TaggedText roots.  It keeps Pixi's public
// texture/anchor/size contract, but owns no Pixi Transform, Bounds, vertex buffer or child array.
// Main-screen geometry and texture lifetime are written directly into the native scene graph.
// FGUI's bitmap-font layout creates a Sprite per glyph only as a temporary x/y/scale/texture
// record. Those records are never display nodes: NativeContainerProxy captures them and emits one
// native multi-quad label. Giving every glyph a scene slot made a store popup allocate hundreds of
// native nodes merely to throw them away after layout. During that tightly-scoped layout phase the
// Sprite constructor therefore returns this allocation-free record instead.
function NativeBitmapGlyphRecord(texture){
  var self=this;this.__nativeBitmapGlyphRecord=true;this.parent=null;this.x=0;this.y=0;this.alpha=1;this.visible=true;this.renderable=true;this._tint=0xFFFFFF;this._texture=texture||PIXI.Texture.EMPTY;this._destroyed=false;
  this.scale={x:1,y:1,set:function(x,y){if(y===undefined)y=x;self.scale.x=Number(x)||0;self.scale.y=Number(y)||0;return self.scale;},copyFrom:function(p){return self.scale.set(p&&p.x,p&&p.y);}};
}
Object.defineProperties(NativeBitmapGlyphRecord.prototype,{
  texture:{get:function(){return this._texture;},set:function(v){this._texture=v||PIXI.Texture.EMPTY;}},
  tint:{get:function(){return this._tint;},set:function(v){this._tint=tintNumber(v);}},
  tintValue:{get:function(){return this._tint;}},destroyed:{get:function(){return this._destroyed;}}
});
NativeBitmapGlyphRecord.prototype.destroy=function(){this.parent=null;this._texture=null;this._destroyed=true;};
NativeBitmapGlyphRecord.prototype.removeFromParent=function(){var p=this.parent;if(p&&p.removeChild)p.removeChild(this);else this.parent=null;return this;};
function NativeDynamicSprite(texture){if((globalThis.__nativeBitmapGlyphMeasureDepth|0)>0){var gs=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});gs.bitmapGlyphRecordsCreated=(gs.bitmapGlyphRecordsCreated||0)+1;return new NativeBitmapGlyphRecord(texture);}texture=texture||PIXI.Texture.EMPTY;shadowPixiTransformAccessors(this);NativeNodeProxy.call(this,{eventMode:'auto',interactiveChildren:true});this.__nativeDynamicSprite=true;this.isSprite=true;this._width=0;this._height=0;this._roundPixels=!!(PIXI.settings&&PIXI.settings.ROUND_PIXELS);this.vertexData=new Float32Array(8);this.vertexTrimmedData=null;this.indices=NativeDynamicSprite.indices;this.uvs=null;this.pluginName='batch';this._textureID=-1;this._textureTrimmedID=-1;this._transformID=-1;this._transformTrimmedID=-1;
  // Pixi's Sprite constructor seeds its anchor from Texture.defaultAnchor before assigning the
  // texture.  Atlas-authored reel symbols rely on that contract: their logical position is the
  // centre of a symbol cell, while the texture's default (usually 0.5, 0.5) moves the bitmap back
  // to the cell's top-left.  Starting every native sprite at (0, 0) therefore displaced the whole
  // reel by one configured viewOffset and clipped its right/bottom edges.
  if(texture.defaultAnchor){this._anchor._x=Number(texture.defaultAnchor.x)||0;this._anchor._y=Number(texture.defaultAnchor.y)||0;}
  this.texture=texture;var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicSpritesCreated=(st.dynamicSpritesCreated||0)+1;st.dynamicSpritesLive=(st.dynamicSpritesLive||0)+1;}
NativeDynamicSprite.indices=new Uint16Array([0,1,2,0,2,3]);
NativeDynamicSprite.prototype=Object.create(OriginalSprite.prototype);installPixiBase(NativeDynamicSprite.prototype,OriginalSprite.prototype);NativeDynamicSprite.prototype.constructor=NativeDynamicSprite;
var spriteTextureDescriptor=Object.getOwnPropertyDescriptor(NativeNodeProxy.prototype,'texture');
Object.defineProperties(NativeDynamicSprite.prototype,{
  texture:{configurable:true,get:spriteTextureDescriptor.get,set:function(v){var old=this._texture;if(old&&old.off)old.off('update',this._onTextureUpdate,this);spriteTextureDescriptor.set.call(this,v||PIXI.Texture.EMPTY);this._textureID=this._textureTrimmedID=-1;if(this._texture&&this._texture.baseTexture&&this._texture.baseTexture.valid)this._onTextureUpdate();else if(this._texture&&this._texture.once)this._texture.once('update',this._onTextureUpdate,this);}},
  width:{configurable:true,get:function(){var o=this._texture&&this._texture.orig;return Math.abs(this.scale.x)*(o?o.width:this._nativeWidth);},set:function(v){v=Number(v)||0;var o=this._texture&&this._texture.orig,w=o?o.width:this._nativeWidth,s=this.scale.x<0?-1:1;this.scale.x=w?s*v/w:s;this._width=v;}},
  height:{configurable:true,get:function(){var o=this._texture&&this._texture.orig;return Math.abs(this.scale.y)*(o?o.height:this._nativeHeight);},set:function(v){v=Number(v)||0;var o=this._texture&&this._texture.orig,h=o?o.height:this._nativeHeight,s=this.scale.y<0?-1:1;this.scale.y=h?s*v/h:s;this._height=v;}},
  roundPixels:{configurable:true,get:function(){return this._roundPixels;},set:function(v){this._roundPixels=!!v;this._transformID=this._transformTrimmedID=-1;}},
  destroyed:{configurable:true,get:function(){return !!this._destroyed;}}
});
NativeDynamicSprite.prototype._onTextureUpdate=function(){var o=this._texture&&this._texture.orig;if(!o)return;this._nativeWidth=o.width||0;this._nativeHeight=o.height||0;if(this._width)this.scale.x=(this.scale.x<0?-1:1)*this._width/(o.width||1);if(this._height)this.scale.y=(this.scale.y<0?-1:1)*this._height/(o.height||1);this._refreshSprite();};
NativeDynamicSprite.prototype._onAnchorUpdate=function(){this._refreshSprite();};
NativeDynamicSprite.prototype.calculateVertices=function(){R.syncWorld(this);var tex=this._texture,orig=tex&&tex.orig,trim=tex&&tex.trim;if(!orig)return;var ax=this._anchor._x||0,ay=this._anchor._y||0,x0=trim?trim.x-ax*orig.width:-ax*orig.width,y0=trim?trim.y-ay*orig.height:-ay*orig.height,x1=x0+(trim?trim.width:orig.width),y1=y0+(trim?trim.height:orig.height),b=this.__i*ST,a=B[b+WA],bb=B[b+WB],c=B[b+WC],d=B[b+WD],tx=B[b+WTX],ty=B[b+WTY],v=this.vertexData,put=function(k,x,y){v[k]=a*x+c*y+tx;v[k+1]=bb*x+d*y+ty;};put(0,x0,y0);put(2,x1,y0);put(4,x1,y1);put(6,x0,y1);if(this._roundPixels){var res=PIXI.settings&&PIXI.settings.RESOLUTION||1;for(var i=0;i<8;i++)v[i]=Math.round(v[i]*res)/res;}var u=tex&&tex._uvs;this.uvs=u&&u.uvsFloat32||null;this._transformID=this.transform._worldID;this._textureID=tex&&tex._updateID;};
NativeDynamicSprite.prototype.calculateTrimmedVertices=function(){this.calculateVertices();if(!this.vertexTrimmedData)this.vertexTrimmedData=new Float32Array(8);this.vertexTrimmedData.set(this.vertexData);};
var destroyDynamicSprite=NativeDynamicSprite.prototype.destroy;NativeDynamicSprite.prototype.destroy=function(options){if(this._destroyed)return;var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicSpritesLive=Math.max(0,(st.dynamicSpritesLive||0)-1);if(this._texture&&this._texture.off)this._texture.off('update',this._onTextureUpdate,this);var tex=this._texture,r=destroyDynamicSprite.call(this,options);if(tex&&(options===true||(options&&options.texture)))tex.destroy(!!(options===true||(options&&options.baseTexture)));return r;};
NativeDynamicSprite.from=function(source,options){var tex=source instanceof PIXI.Texture?source:PIXI.Texture.from(source,options);return new NativeDynamicSprite(tex);};
try{Object.setPrototypeOf(NativeDynamicSprite,OriginalSprite);}catch(_dse){}
installNativeHasInstance(NativeDynamicSprite,OriginalSprite,'__nativeDynamicSprite');
try{Object.defineProperty(PIXI,'Sprite',{configurable:true,writable:true,value:NativeDynamicSprite});}catch(_dsr){try{PIXI.Sprite=NativeDynamicSprite;}catch(_dsr2){}}

// AnimatedSprite only needs its frame list/playback contract on the JavaScript side.  The visible
// object is the same lightweight NativeDynamicSprite handle, so changing a frame updates the
// native texture directly instead of retaining Pixi vertex/bounds/renderer state for the effect.
function NativeDynamicAnimatedSprite(textures,autoUpdate){
  textures=textures&&textures.length?textures:[PIXI.Texture.EMPTY];var first=textures[0] instanceof PIXI.Texture?textures[0]:textures[0].texture;
  NativeDynamicSprite.call(this,first||PIXI.Texture.EMPTY);this.__nativeDynamicAnimatedSprite=true;this._textures=null;this._durations=null;this._autoUpdate=autoUpdate!==false;this._isConnectedToTicker=false;this.animationSpeed=1;this.loop=true;this.updateAnchor=false;this.onComplete=null;this.onFrameChange=null;this.onLoop=null;this._currentTime=0;this._playing=false;this._previousFrame=null;this.textures=textures;
  var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicAnimatedSpritesCreated=(st.dynamicAnimatedSpritesCreated||0)+1;st.dynamicAnimatedSpritesLive=(st.dynamicAnimatedSpritesLive||0)+1;
}
if(OriginalAnimatedSprite){
  NativeDynamicAnimatedSprite.prototype=Object.create(OriginalAnimatedSprite.prototype);installPixiBase(NativeDynamicAnimatedSprite.prototype,OriginalAnimatedSprite.prototype);
  Object.getOwnPropertyNames(NativeDynamicSprite.prototype).forEach(function(k){if(k==='constructor')return;var d=Object.getOwnPropertyDescriptor(NativeDynamicSprite.prototype,k),existing=Object.getOwnPropertyDescriptor(NativeDynamicAnimatedSprite.prototype,k);if(existing&&existing.configurable===false)return;Object.defineProperty(NativeDynamicAnimatedSprite.prototype,k,d);});NativeDynamicAnimatedSprite.prototype.constructor=NativeDynamicAnimatedSprite;
  NativeDynamicAnimatedSprite.prototype.stop=function(){if(!this._playing)return;this._playing=false;if(this._autoUpdate&&this._isConnectedToTicker){PIXI.Ticker.shared.remove(this.update,this);this._isConnectedToTicker=false;}};
  NativeDynamicAnimatedSprite.prototype.play=function(){if(this._playing)return;this._playing=true;if(this._autoUpdate&&!this._isConnectedToTicker){PIXI.Ticker.shared.add(this.update,this,PIXI.UPDATE_PRIORITY&&PIXI.UPDATE_PRIORITY.HIGH);this._isConnectedToTicker=true;}};
  NativeDynamicAnimatedSprite.prototype.gotoAndStop=function(frame){this.stop();this.currentFrame=frame;};
  NativeDynamicAnimatedSprite.prototype.gotoAndPlay=function(frame){this.currentFrame=frame;this.play();};
  NativeDynamicAnimatedSprite.prototype.update=function(deltaTime){
    if(!this._playing||!this._textures||!this._textures.length)return;var elapsed=this.animationSpeed*deltaTime,previous=this.currentFrame;
    if(this._durations){var duration=this._durations[this.currentFrame]||1,lag=this._currentTime%1*duration;lag+=elapsed/60*1000;var sign=Math.sign(this.animationSpeed*deltaTime)||1;for(;lag<0;){this._currentTime--;lag+=this._durations[this.currentFrame]||1;}this._currentTime=Math.floor(this._currentTime);for(;lag>=(this._durations[this.currentFrame]||1);){lag-=(this._durations[this.currentFrame]||1)*sign;this._currentTime+=sign;}this._currentTime+=lag/(this._durations[this.currentFrame]||1);}else this._currentTime+=elapsed;
    if(this._currentTime<0&&!this.loop){this.gotoAndStop(0);if(this.onComplete)this.onComplete();}
    else if(this._currentTime>=this._textures.length&&!this.loop){this.gotoAndStop(this._textures.length-1);if(this.onComplete)this.onComplete();}
    else if(previous!==this.currentFrame){if(this.loop&&this.onLoop&&((this.animationSpeed>0&&this.currentFrame<previous)||(this.animationSpeed<0&&this.currentFrame>previous)))this.onLoop();this.updateTexture();}
  };
  NativeDynamicAnimatedSprite.prototype.updateTexture=function(){var frame=this.currentFrame;if(this._previousFrame===frame)return;this._previousFrame=frame;this.texture=this._textures[frame]||PIXI.Texture.EMPTY;this._textureID=this._textureTrimmedID=-1;this._cachedTint=0xFFFFFF;var u=this._texture&&this._texture._uvs;this.uvs=u&&u.uvsFloat32||null;if(this.updateAnchor&&this._texture&&this._texture.defaultAnchor)this._anchor.copyFrom(this._texture.defaultAnchor);if(this.onFrameChange)this.onFrameChange(frame);};
  Object.defineProperties(NativeDynamicAnimatedSprite.prototype,{
    totalFrames:{configurable:true,get:function(){return this._textures?this._textures.length:0;}},
    textures:{configurable:true,get:function(){return this._textures;},set:function(value){value=value&&value.length?value:[PIXI.Texture.EMPTY];if(value[0] instanceof PIXI.Texture){this._textures=value;this._durations=null;}else{this._textures=[];this._durations=[];for(var i=0;i<value.length;i++){this._textures.push(value[i].texture||PIXI.Texture.EMPTY);this._durations.push(Math.max(1,Number(value[i].time)||1));}}this._previousFrame=null;this.gotoAndStop(0);this.updateTexture();}},
    currentFrame:{configurable:true,get:function(){if(!this._textures||!this._textures.length)return 0;var frame=Math.floor(this._currentTime)%this._textures.length;return frame<0?frame+this._textures.length:frame;},set:function(value){value=Number(value)|0;if(value<0||value>this.totalFrames-1)throw new Error('[AnimatedSprite]: Invalid frame index value '+value+', expected to be between 0 and totalFrames '+this.totalFrames+'.');var previous=this.currentFrame;this._currentTime=value;if(previous!==this.currentFrame)this.updateTexture();}},
    playing:{configurable:true,get:function(){return this._playing;}},
    autoUpdate:{configurable:true,get:function(){return this._autoUpdate;},set:function(value){value=!!value;if(value===this._autoUpdate)return;this._autoUpdate=value;if(!value&&this._isConnectedToTicker){PIXI.Ticker.shared.remove(this.update,this);this._isConnectedToTicker=false;}else if(value&&!this._isConnectedToTicker&&this._playing){PIXI.Ticker.shared.add(this.update,this,PIXI.UPDATE_PRIORITY&&PIXI.UPDATE_PRIORITY.HIGH);this._isConnectedToTicker=true;}}}
  });
  NativeDynamicAnimatedSprite.prototype.destroy=function(options){if(this._destroyed)return;this.stop();var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicAnimatedSpritesLive=Math.max(0,(st.dynamicAnimatedSpritesLive||0)-1);this.onComplete=this.onFrameChange=this.onLoop=null;this._textures=null;this._durations=null;return NativeDynamicSprite.prototype.destroy.call(this,options);};
  NativeDynamicAnimatedSprite.fromFrames=function(frames){var textures=[];for(var i=0;i<frames.length;i++)textures.push(PIXI.Texture.from(frames[i]));return new NativeDynamicAnimatedSprite(textures);};
  NativeDynamicAnimatedSprite.fromImages=function(images){var textures=[];for(var i=0;i<images.length;i++)textures.push(PIXI.Texture.from(images[i]));return new NativeDynamicAnimatedSprite(textures);};
  try{Object.setPrototypeOf(NativeDynamicAnimatedSprite,OriginalAnimatedSprite);}catch(_ase){}
  installNativeHasInstance(NativeDynamicAnimatedSprite,OriginalAnimatedSprite,'__nativeDynamicAnimatedSprite');
  try{Object.defineProperty(PIXI,'AnimatedSprite',{configurable:true,writable:true,value:NativeDynamicAnimatedSprite});}catch(_asr){try{PIXI.AnimatedSprite=NativeDynamicAnimatedSprite;}catch(_asr2){}}
}

// Generic Graphics keeps Pixi's authored geometry and tessellator, with a numeric native
// hierarchy and transforms. No Pixi renderer traversal or approximate replacement shapes.
function NativeDynamicGraphics(geometry){
  shadowPixiTransformAccessors(this);NativeNodeProxy.call(this,{eventMode:'auto'});
  this.__nativeExactGraphics=true;this.__nativeDynamicGraphics=true;
  this.currentPath=null;this._matrix=null;this._holeMode=false;
  this._fillStyle=new PIXI.FillStyle();this._lineStyle=new PIXI.LineStyle();
  this._geometry=geometry||new PIXI.GraphicsGeometry();this._geometry.refCount++;
  this._transformID=-1;this._tintColor=new PIXI.Color(0xffffff);
  var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicGraphicsCreated=(st.dynamicGraphicsCreated||0)+1;st.dynamicGraphicsLive=(st.dynamicGraphicsLive||0)+1;
}
NativeDynamicGraphics.prototype=Object.create(OriginalGraphics.prototype);
installPixiBase(NativeDynamicGraphics.prototype,OriginalGraphics.prototype);
NativeDynamicGraphics.prototype.constructor=NativeDynamicGraphics;
displaySizeContract.installLocalBoundsAccessors(NativeDynamicGraphics.prototype);
// Geometry mutators are Pixi's own implementation. Open paths are flushed at the native
// synchronization boundary, since native presentation does not call Graphics._render().
['lineStyle','lineTextureStyle','beginFill','beginTextureFill','endFill','moveTo','lineTo','quadraticCurveTo','bezierCurveTo','arc','arcTo','closePath','drawShape','beginHole','endHole','clear','setMatrix'].forEach(function(name){
  var method=OriginalGraphics.prototype[name];if(!method)return;
  NativeDynamicGraphics.prototype[name]=function(){var result=method.apply(this,arguments);if(R.markVisual)R.markVisual(this);if(this.__maskOf&&R.markVisual)R.markVisual(this.__maskOf);return result;};
});
NativeDynamicGraphics.prototype.getLocalBounds=function(rect){this.finishPoly();if(R.getLocalBounds){R.writeVisual(this);return R.getLocalBounds(this,rect||new PIXI.Rectangle());}var b=this.geometry.bounds;rect=rect||new PIXI.Rectangle();rect.x=b.minX;rect.y=b.minY;rect.width=b.maxX-b.minX;rect.height=b.maxY-b.minY;return rect;};
NativeDynamicGraphics.prototype.containsPoint=function(point){nativeMutationReadBarrier();R.syncWorld(this);this.finishPoly();return OriginalGraphics.prototype.containsPoint.call(this,point);};
NativeDynamicGraphics.prototype.clone=function(){this.finishPoly();return new NativeDynamicGraphics(this.geometry);};
NativeDynamicGraphics.prototype.destroy=function(options){
  if(this._destroyed)return;var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicGraphicsLive=Math.max(0,(st.dynamicGraphicsLive||0)-1);
  NativeNodeProxy.prototype.destroy.call(this,options);
  if(this._geometry&&--this._geometry.refCount===0)this._geometry.dispose();this._geometry=null;this.currentPath=null;
  this._lineStyle.destroy();this._fillStyle.destroy();this._lineStyle=this._fillStyle=null;
};
try{Object.setPrototypeOf(NativeDynamicGraphics,OriginalGraphics);}catch(_dge){}
installNativeHasInstance(NativeDynamicGraphics,OriginalGraphics,'__nativeExactGraphics');
try{Object.defineProperty(PIXI,'Graphics',{configurable:true,writable:true,value:NativeDynamicGraphics});}catch(_dgr){try{PIXI.Graphics=NativeDynamicGraphics;}catch(_dgr2){}}

// Standard MeshMaterial geometry is already emitted as native triangles by native_scenegraph.
// These facades remove the remaining Pixi Container/DisplayObject half while preserving the
// public Mesh, SimpleMesh, SimplePlane and SimpleRope data APIs used by business effects.
function markNativeMesh(o){if(o&&o.__i>0&&R.markVisual)R.markVisual(o);}
function watchNativeMeshGeometry(o){
  var old=o.__nativeMeshBuffers||[];for(var i=0;i<old.length;i++){var ob=old[i];if(ob&&ob.__nativeMeshOwners)ob.__nativeMeshOwners.delete(o);}
  var g=o._geometry,list=g&&g.buffers?g.buffers.slice():[];if(g&&g.indexBuffer&&list.indexOf(g.indexBuffer)<0)list.push(g.indexBuffer);o.__nativeMeshBuffers=list;
  for(var j=0;j<list.length;j++)(function(bf){if(!bf)return;if(!bf.__nativeMeshOwners)bf.__nativeMeshOwners=new Set();bf.__nativeMeshOwners.add(o);if(bf.__nativeMeshUpdateHook||typeof bf.update!=='function')return;var update=bf.update;bf.__nativeMeshUpdateHook=1;bf.update=function(){var r=update.apply(this,arguments);if(this.__nativeMeshOwners)this.__nativeMeshOwners.forEach(markNativeMesh);return r;};})(list[j]);
}
function copyNativeMeshApi(proto){
  Object.getOwnPropertyNames(NativeDynamicMesh.prototype).forEach(function(k){if(k==='constructor')return;var d=Object.getOwnPropertyDescriptor(NativeDynamicMesh.prototype,k),existing=Object.getOwnPropertyDescriptor(proto,k);if(existing&&existing.configurable===false)return;Object.defineProperty(proto,k,d);});
}
function NativeDynamicMesh(geometry,shader,state,drawMode){
  shadowPixiTransformAccessors(this);NativeNodeProxy.call(this,{eventMode:'auto',interactiveChildren:true});this.__nativeDynamicMesh=true;this.isSprite=false;this._geometry=null;this.shader=shader||null;this.state=state||(PIXI.State&&PIXI.State.for2d?PIXI.State.for2d():{blendMode:0});this.drawMode=drawMode==null?((PIXI.DRAW_MODES&&PIXI.DRAW_MODES.TRIANGLES)||4):drawMode;this.start=0;this.size=0;this.uvs=null;this.indices=null;this.vertexData=new Float32Array(1);this.vertexDirty=-1;this._transformID=-1;this._roundPixels=!!(PIXI.settings&&PIXI.settings.ROUND_PIXELS);this.batchUvs=null;this.geometry=geometry;
  var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicMeshesCreated=(st.dynamicMeshesCreated||0)+1;st.dynamicMeshesLive=(st.dynamicMeshesLive||0)+1;markNativeMesh(this);
}
if(OriginalMesh){
  NativeDynamicMesh.prototype=Object.create(OriginalMesh.prototype);installPixiBase(NativeDynamicMesh.prototype,OriginalMesh.prototype);NativeDynamicMesh.prototype.constructor=NativeDynamicMesh;
  Object.defineProperties(NativeDynamicMesh.prototype,{
    geometry:{configurable:true,get:function(){return this._geometry;},set:function(value){if(this._geometry===value)return;var old=this._geometry;if(old&&old.refCount!==undefined){old.refCount--;if(old.refCount===0&&old.dispose)old.dispose();}this._geometry=value||null;if(this._geometry&&this._geometry.refCount!==undefined)this._geometry.refCount++;this.vertexDirty=-1;watchNativeMeshGeometry(this);markNativeMesh(this);}},
    uvBuffer:{configurable:true,get:function(){return this._geometry&&this._geometry.buffers&&this._geometry.buffers[1];}},
    verticesBuffer:{configurable:true,get:function(){return this._geometry&&this._geometry.buffers&&this._geometry.buffers[0];}},
    material:{configurable:true,get:function(){return this.shader;},set:function(v){if(this.shader!==v){this.shader=v;markNativeMesh(this);}}},
    blendMode:{configurable:true,get:function(){return this.state&&this.state.blendMode!==undefined?this.state.blendMode:this._blendMode||0;},set:function(v){v=Number(v)||0;this._blendMode=v;if(this.state)this.state.blendMode=v;if(this.__i>0)B[this.__i*ST+BLEND]=v;markNativeMesh(this);}},
    roundPixels:{configurable:true,get:function(){return this._roundPixels;},set:function(v){v=!!v;if(this._roundPixels!==v){this._roundPixels=v;this._transformID=-1;}}},
    tint:{configurable:true,get:function(){return this.shader&&'tint' in this.shader?this.shader.tint:null;},set:function(v){if(this.shader&&'tint' in this.shader)this.shader.tint=v;this._tint=tintNumber(v);if(this.__i>0)B[this.__i*ST+TINT]=this._tint;markNativeMesh(this);}},
    tintValue:{configurable:true,get:function(){return this.shader&&this.shader.tintValue!==undefined?this.shader.tintValue:this._tint; }},
    texture:{configurable:true,get:function(){return this.shader&&'texture' in this.shader?this.shader.texture:null;},set:function(v){if(this.shader&&'texture' in this.shader&&this.shader.texture!==v){this.shader.texture=v;markNativeMesh(this);}}}
  });
  NativeDynamicMesh.prototype.calculateUvs=function(){var g=this._geometry,uv=g&&g.buffers&&g.buffers[1],s=this.shader;if(!uv)return null;if(s&&s.uvMatrix&&!s.uvMatrix.isSimple){s.uvMatrix.update();var src=uv.data,out=this.uvs&&this.uvs.length===src.length?this.uvs:new Float32Array(src.length);s.uvMatrix.multiplyUvs(src,out);this.uvs=out;}else this.uvs=uv.data;return this.uvs;};
  NativeDynamicMesh.prototype.calculateVertices=function(){var vb=this.verticesBuffer,src=vb&&vb.data;if(!src)return;R.syncWorld(this);if(!this.vertexData||this.vertexData.length!==src.length)this.vertexData=new Float32Array(src.length);var b=this.__i*ST,a=B[b+WA],bb=B[b+WB],c=B[b+WC],d=B[b+WD],tx=B[b+WTX],ty=B[b+WTY],res=PIXI.settings&&PIXI.settings.RESOLUTION||1;for(var i=0;i<src.length;i+=2){var x=src[i],y=src[i+1],ox=a*x+c*y+tx,oy=bb*x+d*y+ty;if(this._roundPixels){ox=Math.round(ox*res)/res;oy=Math.round(oy*res)/res;}this.vertexData[i]=ox;this.vertexData[i+1]=oy;}this.vertexDirty=vb._updateID;return this.vertexData;};
  NativeDynamicMesh.prototype.getLocalBounds=function(rect){rect=rect||new PIXI.Rectangle();var vb=this.verticesBuffer,v=vb&&vb.data,x0=Infinity,y0=Infinity,x1=-Infinity,y1=-Infinity;if(v)for(var i=0;i+1<v.length;i+=2){var x=v[i],y=v[i+1];if(x<x0)x0=x;if(y<y0)y0=y;if(x>x1)x1=x;if(y>y1)y1=y;}if(x0===Infinity)x0=y0=x1=y1=0;rect.x=x0;rect.y=y0;rect.width=x1-x0;rect.height=y1-y0;return rect;};
  NativeDynamicMesh.prototype.containsPoint=function(point){var p=this.worldTransform.applyInverse(point,NativeNodeProxy._point),g=this._geometry,v=this.verticesBuffer&&this.verticesBuffer.data,ib=g&&(g.getIndex?g.getIndex():g.indexBuffer),idx=ib&&ib.data;if(!v||!idx)return false;var poly=new PIXI.Polygon();for(var i=0;i+2<idx.length;i+=3){var a=(idx[i]|0)*2,b=(idx[i+1]|0)*2,c=(idx[i+2]|0)*2;poly.points=[v[a],v[a+1],v[b],v[b+1],v[c],v[c+1]];if(poly.contains(p.x,p.y))return true;}return false;};
  NativeDynamicMesh.prototype.destroy=function(options){if(this._destroyed)return;var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.dynamicMeshesLive=Math.max(0,(st.dynamicMeshesLive||0)-1);nativeAutoMeshes.delete(this);this.__nativeMeshPrepare=null;if(this.textureUpdated&&this.texture&&this.texture.off)this.texture.off('update',this.textureUpdated,this);this.geometry=null;this.shader=null;this.state=null;this.uvs=this.indices=this.vertexData=this.batchUvs=null;return NativeNodeProxy.prototype.destroy.call(this,options);};
  try{Object.setPrototypeOf(NativeDynamicMesh,OriginalMesh);}catch(_dme){}
  installNativeHasInstance(NativeDynamicMesh,OriginalMesh,'__nativeDynamicMesh');
  try{Object.defineProperty(PIXI,'Mesh',{configurable:true,writable:true,value:NativeDynamicMesh});}catch(_dmr){try{PIXI.Mesh=NativeDynamicMesh;}catch(_dmr2){}}
}

var nativeAutoMeshes=new Set();
function connectNativeMeshRender(o,fn){o.__nativeMeshPrepare=fn;nativeAutoMeshes.add(o);}
globalThis.__nativePrepareMeshRender=function(root){if(globalThis.__nativePreparePixi8Render)globalThis.__nativePreparePixi8Render(root);nativeAutoMeshes.forEach(function(o){
  var p=o,linked=false;while(p){if(!p.visible||!p.renderable||p.culled||p.alpha<=0)break;if(p===root){linked=true;break;}p=p.parent;}
  if(linked&&!o._destroyed)o.__nativeMeshPrepare();
});};
function NativeDynamicSimpleMesh(texture,vertices,uvs,indices,drawMode){var g=new PIXI.MeshGeometry(vertices,uvs,indices);if(g.getBuffer)g.getBuffer('aVertexPosition').static=false;NativeDynamicMesh.call(this,g,new PIXI.MeshMaterial(texture||PIXI.Texture.EMPTY),null,drawMode);this.__nativeSimpleMesh=true;this.autoUpdate=true;connectNativeMeshRender(this,function(){if(this.autoUpdate){var b=this.verticesBuffer;if(b&&b.update)b.update();else markNativeMesh(this);}});}
if(OriginalSimpleMesh&&OriginalMesh){NativeDynamicSimpleMesh.prototype=Object.create(OriginalSimpleMesh.prototype);installPixiBase(NativeDynamicSimpleMesh.prototype,OriginalSimpleMesh.prototype);copyNativeMeshApi(NativeDynamicSimpleMesh.prototype);NativeDynamicSimpleMesh.prototype.constructor=NativeDynamicSimpleMesh;Object.defineProperty(NativeDynamicSimpleMesh.prototype,'vertices',{configurable:true,get:function(){var b=this.verticesBuffer;return b&&b.data;},set:function(v){var b=this.verticesBuffer;if(b){b.data=v;if(b.update)b.update();}}});try{Object.setPrototypeOf(NativeDynamicSimpleMesh,OriginalSimpleMesh);}catch(_sme){}installNativeHasInstance(NativeDynamicSimpleMesh,OriginalSimpleMesh,'__nativeSimpleMesh');try{Object.defineProperty(PIXI,'SimpleMesh',{configurable:true,writable:true,value:NativeDynamicSimpleMesh});}catch(_smr){}}

function NativeDynamicSimplePlane(texture,verticesX,verticesY){texture=texture||PIXI.Texture.EMPTY;NativeDynamicMesh.call(this,new PIXI.PlaneGeometry(texture.width,texture.height,verticesX,verticesY),new PIXI.MeshMaterial(PIXI.Texture.WHITE));this.__nativeSimplePlane=true;this.autoResize=true;this._textureID=-1;this.texture=texture;connectNativeMeshRender(this,function(){if(this._textureID!==this.texture._updateID)this.textureUpdated();});}
if(OriginalSimplePlane&&OriginalMesh){NativeDynamicSimplePlane.prototype=Object.create(OriginalSimplePlane.prototype);installPixiBase(NativeDynamicSimplePlane.prototype,OriginalSimplePlane.prototype);copyNativeMeshApi(NativeDynamicSimplePlane.prototype);NativeDynamicSimplePlane.prototype.constructor=NativeDynamicSimplePlane;NativeDynamicSimplePlane.prototype.textureUpdated=function(){var tex=this.shader&&this.shader.texture,g=this.geometry;if(!tex||!g)return;this._textureID=tex._updateID;if(this.autoResize&&(g.width!==tex.width||g.height!==tex.height)){g.width=tex.width;g.height=tex.height;if(g.build)g.build();}markNativeMesh(this);};Object.defineProperty(NativeDynamicSimplePlane.prototype,'texture',{configurable:true,get:function(){return this.shader&&this.shader.texture;},set:function(v){var old=this.shader&&this.shader.texture;if(old===v)return;if(old&&old.off)old.off('update',this.textureUpdated,this);this.shader.texture=v||PIXI.Texture.EMPTY;this._textureID=-1;if(v&&v.baseTexture&&v.baseTexture.valid)this.textureUpdated();else if(v&&v.once)v.once('update',this.textureUpdated,this);markNativeMesh(this);}});var destroySimplePlane=NativeDynamicSimplePlane.prototype.destroy;NativeDynamicSimplePlane.prototype.destroy=function(options){var t=this.shader&&this.shader.texture;if(t&&t.off)t.off('update',this.textureUpdated,this);return destroySimplePlane.call(this,options);};try{Object.setPrototypeOf(NativeDynamicSimplePlane,OriginalSimplePlane);}catch(_spe){}installNativeHasInstance(NativeDynamicSimplePlane,OriginalSimplePlane,'__nativeSimplePlane');try{Object.defineProperty(PIXI,'SimplePlane',{configurable:true,writable:true,value:NativeDynamicSimplePlane});}catch(_spr){}}

function NativeDynamicSimpleRope(texture,points,textureScale){textureScale=Number(textureScale)||0;texture=texture||PIXI.Texture.EMPTY;if(textureScale>0&&texture.baseTexture&&PIXI.WRAP_MODES)texture.baseTexture.wrapMode=PIXI.WRAP_MODES.REPEAT;NativeDynamicMesh.call(this,new PIXI.RopeGeometry(texture.height,points,textureScale),new PIXI.MeshMaterial(texture));this.__nativeSimpleRope=true;this.autoUpdate=true;connectNativeMeshRender(this,function(){var g=this.geometry;if(g&&(this.autoUpdate||g.width!==this.texture.height)&&g.update){g.width=this.texture.height;g.update();}});}
if(OriginalSimpleRope&&OriginalMesh){NativeDynamicSimpleRope.prototype=Object.create(OriginalSimpleRope.prototype);installPixiBase(NativeDynamicSimpleRope.prototype,OriginalSimpleRope.prototype);copyNativeMeshApi(NativeDynamicSimpleRope.prototype);NativeDynamicSimpleRope.prototype.constructor=NativeDynamicSimpleRope;try{Object.setPrototypeOf(NativeDynamicSimpleRope,OriginalSimpleRope);}catch(_sre){}installNativeHasInstance(NativeDynamicSimpleRope,OriginalSimpleRope,'__nativeSimpleRope');try{Object.defineProperty(PIXI,'SimpleRope',{configurable:true,writable:true,value:NativeDynamicSimpleRope});}catch(_srr){}}

// ParticleContainer only needs a packed native quad owner plus semantic child handles.  Keeping
// Pixi's ParticleBuffer/Geometry objects and a second JS child array provided no pixels on the
// native screen and made every emitter burst allocate a parallel render tree.
function NativeParticleContainer(maxSize,properties,batchSize,autoResize){
  NativeContainerProxy.call(this,null,{eventMode:'none',interactiveChildren:false});
  this.__nativeParticleContainer=true;this._maxSize=Math.max(1,Number(maxSize)||1500);this._batchSize=Math.max(1,Number(batchSize)||16384);this._autoResize=!!autoResize;this.autoResize=!!autoResize;this.roundPixels=true;this.baseTexture=null;this._buffers=[];this._bufferUpdateIDs=[];this._properties={vertices:false,position:true,rotation:false,uvs:false,tint:false};this.setProperties(properties||{});
  var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.particleContainersCreated=(st.particleContainersCreated||0)+1;st.particleContainersLive=(st.particleContainersLive||0)+1;
}
NativeParticleContainer.prototype=Object.create(NativeContainerProxy.prototype);NativeParticleContainer.prototype.constructor=NativeParticleContainer;
NativeParticleContainer.prototype.setProperties=function(properties){properties=properties||{};var p=this._properties;['vertices','position','rotation','uvs','tint'].forEach(function(k){if(properties[k]!==undefined)p[k]=!!properties[k];});if(R.markVisual)R.markVisual(this);return this;};
NativeParticleContainer.prototype.onChildrenChange=function(){if(R.markVisual)R.markVisual(this);};
var destroyParticleContainer=NativeParticleContainer.prototype.destroy;NativeParticleContainer.prototype.destroy=function(options){if(this._destroyed)return;var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.particleContainersLive=Math.max(0,(st.particleContainersLive||0)-1);this._buffers.length=0;this._bufferUpdateIDs.length=0;return destroyParticleContainer.call(this,options);};
if(OriginalParticleContainer){
  try{Object.setPrototypeOf(NativeParticleContainer,OriginalParticleContainer);}catch(_pce){}
  installNativeHasInstance(NativeParticleContainer,OriginalParticleContainer,'__nativeParticleContainer');
  try{Object.defineProperty(PIXI,'ParticleContainer',{configurable:true,writable:true,value:NativeParticleContainer});}catch(_pcr){try{PIXI.ParticleContainer=NativeParticleContainer;}catch(_pcr2){}}
}

// Pixi Application constructs its stage through an internal lexical Container reference, so
// replacing PIXI.Container alone leaves one heavyweight JS root and its children array alive.
// Swap that empty bootstrap stage immediately after Application construction; all subsequent
// game/FGUI children attach to the numeric native hierarchy.
function NativeApplication(){
  var args=Array.prototype.slice.call(arguments),app;
  globalThis.__nativeApplicationBootstrapDepth=(globalThis.__nativeApplicationBootstrapDepth||0)+1;
  try{app=Reflect.construct(OriginalApplication,args,OriginalApplication);}
  finally{globalThis.__nativeApplicationBootstrapDepth=Math.max(0,(globalThis.__nativeApplicationBootstrapDepth||1)-1);}
  var old=app.stage,stage=new NativeDynamicContainer({eventMode:'auto',interactiveChildren:true});
  stage.__nativeApplicationStage=true;stage.name=old&&old.name||'stage';stage.sortableChildren=!!(old&&old.sortableChildren);stage.eventMode=old&&old.eventMode||'auto';stage.interactiveChildren=old?old.interactiveChildren!==false:true;stage.hitArea=old&&old.hitArea||null;
  if(old&&old.children&&old.children.length){var children=old.children.slice();for(var i=0;i<children.length;i++)stage.addChild(children[i]);}
  app.stage=stage;try{if(old&&old.destroy)old.destroy({children:false});}catch(_asd){}
  // Only the shell/game application owns a page footer; offscreen applications keep
  // their requested viewport and never attach shell controls.
  var page=globalThis.__nativeBoot&&globalThis.__nativeBoot.get('page-footer');
  if(page&&args[0]&&args[0].resizeTo&&args[0].resizeTo.id==='game_screen')page.attach(app);
  var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.nativeApplicationStages=(st.nativeApplicationStages||0)+1;return app;
}
if(OriginalApplication){
  NativeApplication.prototype=OriginalApplication.prototype;try{Object.setPrototypeOf(NativeApplication,OriginalApplication);}catch(_aae){}
  try{Object.defineProperty(PIXI,'Application',{configurable:true,writable:true,value:NativeApplication});}catch(_aar){try{PIXI.Application=NativeApplication;}catch(_aar2){}}
}

globalThis.NativeNodeProxy=NativeNodeProxy;
globalThis.NativeContainerProxy=NativeContainerProxy;
globalThis.NativeDynamicContainer=NativeDynamicContainer;
globalThis.NativeDynamicSprite=NativeDynamicSprite;
globalThis.NativeDynamicAnimatedSprite=NativeDynamicAnimatedSprite;
globalThis.NativeDynamicGraphics=NativeDynamicGraphics;
globalThis.NativeDynamicMesh=NativeDynamicMesh;
globalThis.NativeDynamicSimpleMesh=NativeDynamicSimpleMesh;
globalThis.NativeDynamicSimplePlane=NativeDynamicSimplePlane;
globalThis.NativeDynamicSimpleRope=NativeDynamicSimpleRope;
globalThis.NativeParticleContainer=NativeParticleContainer;
globalThis.NativeGraphicsProxy=NativeGraphicsProxy;
globalThis.NativeTextProxy=NativeTextProxy;
globalThis.NativeDynamicText=NativeDynamicText;
globalThis.NativeTilingProxy=NativeTilingProxy;
globalThis.NativeDynamicTilingSprite=NativeDynamicTilingSprite;
globalThis.NativeDynamicNineSlicePlane=NativeDynamicNineSlicePlane;
globalThis.NativeBitmapText=NativeBitmapText;
// Generic factory for high-construction-cost regions (reels, symbol grids, particle hosts).
// All paths are feature-agnostic: callers describe nodes/trees; Native owns slots and paint.
function applyNativeOptions(node, options){
  if(!node||!options)return node;
  if(options.name!=null)node.name=String(options.name);
  if(options.x!=null)node.x=Number(options.x)||0;
  if(options.y!=null)node.y=Number(options.y)||0;
  if(options.scaleX!=null||options.scaleY!=null){
    var sx=options.scaleX!=null?Number(options.scaleX):1,sy=options.scaleY!=null?Number(options.scaleY):1;
    if(node.scale&&node.scale.set)node.scale.set(isFinite(sx)?sx:1,isFinite(sy)?sy:1);
  }else if(options.scale!=null){
    var s=Number(options.scale);if(isFinite(s)&&node.scale&&node.scale.set)node.scale.set(s,s);
  }
  if(options.rotation!=null)node.rotation=Number(options.rotation)||0;
  if(options.angle!=null)node.angle=Number(options.angle)||0;
  if(options.alpha!=null)node.alpha=Number(options.alpha);
  if(options.visible!=null)node.visible=!!options.visible;
  if(options.renderable!=null)node.renderable=!!options.renderable;
  if(options.eventMode!=null)node.eventMode=options.eventMode;
  if(options.interactiveChildren!=null)node.interactiveChildren=!!options.interactiveChildren;
  if(options.hitArea!=null)node.hitArea=options.hitArea;
  if(options.tint!=null)node.tint=options.tint;
  if(options.blendMode!=null)node.blendMode=options.blendMode;
  if(options.zIndex!=null)node.zIndex=Number(options.zIndex)||0;
  if(options.anchor!=null&&node.anchor){
    if(typeof options.anchor==='number')node.anchor.set(options.anchor);
    else if(options.anchor.x!=null||options.anchor.y!=null)node.anchor.set(Number(options.anchor.x)||0,Number(options.anchor.y)||0);
  }
  if(options.width!=null&&'width' in node)node.width=Number(options.width)||0;
  if(options.height!=null&&'height' in node)node.height=Number(options.height)||0;
  if(options.texture!=null&&'texture' in node)node.texture=options.texture;
  return node;
}
function createNativeOfType(type, options){
  options=options||{};type=String(type||options.type||'container').toLowerCase();
  var node=null,tex=options.texture||null;
  if(type==='sprite'||type==='image')node=new NativeDynamicSprite(tex||PIXI.Texture.EMPTY);
  else if(type==='container'||type==='node')node=new NativeDynamicContainer(options);
  else if(type==='nineslice'||type==='nine'||type==='9slice'){
    var g=options.grid||options.scale9Grid||{};
    node=new NativeDynamicNineSlicePlane(tex||PIXI.Texture.EMPTY,
      g.left!=null?g.left:options.leftWidth,
      g.top!=null?g.top:options.topHeight,
      g.right!=null?g.right:options.rightWidth,
      g.bottom!=null?g.bottom:options.bottomHeight);
  }else if(type==='tiling'||type==='tilingsprite')node=new NativeDynamicTilingSprite(tex||PIXI.Texture.EMPTY,options.width,options.height);
  else if(type==='text')node=new NativeDynamicText(options.text||'',options.style||null);
  else if(type==='bitmaptext')node=new NativeBitmapText(options.text||'',options.style||options);
  else if(type==='graphics')node=new NativeDynamicGraphics();
  else if(type==='particle'||type==='particlecontainer')node=new NativeParticleContainer(options.maxSize,options.properties,options.batchSize,options.autoResize);
  else if(type==='animated'||type==='animatedsprite')node=new NativeDynamicAnimatedSprite(options.textures||options.frames||[]);
  else node=new NativeDynamicContainer(options);
  applyNativeOptions(node,options);
  if(options.parent){
    if(typeof node.attachTo==='function'&&options.attachOnly)node.attachTo(options.parent,options.before||null);
    else if(options.parent.addChild)options.parent.addChild(node);
    else if(typeof node.attachTo==='function')node.attachTo(options.parent,options.before||null);
  }
  return node;
}
// Pre-allocate scene slots in one host crossing, then bind each facade via the same
// beginFacadeAdopt path FGUI uses. Falls back to per-node alloc when the host lacks batch.
function allocNativeIds(count){
  count=Math.max(0,count|0);if(!count)return [];
  var ids=new Array(count),n=0,i;
  if(typeof globalThis.__sgAllocBatch==='function'){
    var buf=new Float32Array(count),got=__sgAllocBatch(buf)|0;
    for(i=0;i<got;i++){var id=buf[i]|0;if(id>0)ids[n++]=id;}
  }
  if(n<count&&R&&typeof R.alloc==='function'){
    // R.alloc needs an object; allocate plain placeholders then free the JS side only keeps ids.
    for(;n<count;n++){
      var dummy={};var id2=R.alloc(dummy)|0;if(id2<=0)break;
      // Detach placeholder so the real facade can adopt the slot.
      if(R.objects)R.objects[id2]=undefined;
      dummy.__i=undefined;ids[n]=id2;
    }
  }
  ids.length=n;return ids;
}
function freeReservedSlot(slotId){
  slotId=slotId|0;if(!(slotId>0))return;
  try{
    if(R.objects&&R.objects[slotId]){R.free(R.objects[slotId]);return;}
    // Slot was reserved without a facade (batch pre-alloc). Free by temporary handle.
    if(typeof R.free==='function')R.free({__i:slotId});
  }catch(_f){}
}
function createWithSlot(type, options, slotId){
  options=options||{};slotId=slotId|0;
  if(slotId>0&&R.beginFacadeAdopt&&R.beginFacadeAdopt(slotId,true)){
    var node=null;
    try{node=createNativeOfType(type,options);}
    catch(e){R.finishFacadeAdopt(null,false);freeReservedSlot(slotId);throw e;}
    if(!R.finishFacadeAdopt(node,true)){
      // Constructor took a fresh slot; release the unused reservation.
      freeReservedSlot(slotId);
    }
    return node;
  }
  if(slotId>0)freeReservedSlot(slotId);
  return createNativeOfType(type,options);
}
function createNativeBatch(spec){
  spec=spec||{};
  var type=spec.type||'sprite';
  var items=Array.isArray(spec.items)?spec.items:null;
  var count=items?items.length:Math.max(0,spec.count|0);
  if(!count)return [];
  var defaults=spec.defaults||spec.options||{};
  var parent=spec.parent||defaults.parent||null;
  var attachOnly=!!(spec.attachOnly||defaults.attachOnly);
  var ids=allocNativeIds(count),out=new Array(count),i,opts,node;
  for(i=0;i<count;i++){
    opts=items?Object.assign({},defaults,items[i]||{}):Object.assign({},defaults);
    if(parent&&!opts.parent)opts.parent=null; // attach after so order is stable
    if(opts.attachOnly==null&&attachOnly)opts.attachOnly=attachOnly;
    node=createWithSlot(type,opts,ids[i]|0);
    out[i]=node;
  }
  if(parent){
    for(i=0;i<out.length;i++){
      if(!out[i])continue;
      if(attachOnly&&typeof out[i].attachTo==='function')out[i].attachTo(parent);
      else if(parent.addChild)parent.addChild(out[i]);
      else if(typeof out[i].attachTo==='function')out[i].attachTo(parent);
    }
  }
  var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});
  st.nativeBatchCreates=(st.nativeBatchCreates||0)+1;
  st.nativeBatchNodes=(st.nativeBatchNodes||0)+out.length;
  return out;
}
function createNativeTree(desc, parent){
  if(!desc)return null;
  if(Array.isArray(desc)){
    var list=new Array(desc.length);
    for(var i=0;i<desc.length;i++)list[i]=createNativeTree(desc[i],parent);
    return list;
  }
  var children=desc.children;var copy=Object.assign({},desc);delete copy.children;delete copy.type;
  if(parent&&!copy.parent)copy.parent=null;
  var node=createNativeOfType(desc.type||'container',copy);
  if(parent){
    if(desc.attachOnly&&typeof node.attachTo==='function')node.attachTo(parent);
    else if(parent.addChild)parent.addChild(node);
    else if(typeof node.attachTo==='function')node.attachTo(parent);
  }
  if(children&&children.length){
    for(var c=0;c<children.length;c++)createNativeTree(children[c],node);
  }
  return node;
}
function createNativePool(spec){
  spec=spec||{};
  var type=spec.type||'sprite';
  var size=Math.max(0,spec.size|0);
  var defaults=spec.defaults||spec.options||{};
  var free=[],live=new Set();
  function makeOne(){return createNativeOfType(type,Object.assign({},defaults,{parent:null}));}
  for(var i=0;i<size;i++)free.push(makeOne());
  var pool={
    type:type,
    size:function(){return free.length+live.size;},
    available:function(){return free.length;},
    live:function(){return live.size;},
    acquire:function(overrides){
      var node=free.length?free.pop():makeOne();
      if(overrides)applyNativeOptions(node,overrides);
      if(node.visible===false)node.visible=true;
      if(node.renderable===false)node.renderable=true;
      live.add(node);
      var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});
      st.nativePoolAcquires=(st.nativePoolAcquires||0)+1;
      return node;
    },
    release:function(node){
      if(!node||!live.has(node))return false;
      live.delete(node);
      if(node.parent&&node.parent.removeChild)node.parent.removeChild(node);
      else if(typeof node.removeFromParent==='function')node.removeFromParent();
      node.visible=false;
      free.push(node);
      var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});
      st.nativePoolReleases=(st.nativePoolReleases||0)+1;
      return true;
    },
    releaseAll:function(){
      var arr=Array.from(live);for(var i=0;i<arr.length;i++)pool.release(arr[i]);return arr.length;
    },
    destroy:function(){
      pool.releaseAll();
      for(var i=0;i<free.length;i++)if(free[i]&&free[i].destroy)free[i].destroy({children:true});
      free.length=0;live.clear();
    },
    prewarm:function(n){
      n=Math.max(0,n|0);while(free.length<n)free.push(makeOne());return free.length;
    }
  };
  var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});
  st.nativePoolsCreated=(st.nativePoolsCreated||0)+1;
  st.nativePoolWarm=(st.nativePoolWarm||0)+size;
  return pool;
}
globalThis.NativeScene={
  Node:NativeNodeProxy,Container:NativeContainerProxy,DynamicContainer:NativeDynamicContainer,
  Sprite:NativeDynamicSprite,AnimatedSprite:NativeDynamicAnimatedSprite,NineSlicePlane:NativeDynamicNineSlicePlane,
  Mesh:NativeDynamicMesh,SimpleMesh:NativeDynamicSimpleMesh,SimplePlane:NativeDynamicSimplePlane,SimpleRope:NativeDynamicSimpleRope,
  ParticleContainer:NativeParticleContainer,Graphics:NativeGraphicsProxy,DynamicGraphics:NativeDynamicGraphics,
  Text:NativeTextProxy,DynamicText:NativeDynamicText,Tiling:NativeTilingProxy,DynamicTiling:NativeDynamicTilingSprite,
  BitmapText:NativeBitmapText,
  // Single-node factories
  createNode:function(options){return createNativeOfType((options&&options.type)||'node',options||{});},
  createContainer:function(options){return createNativeOfType('container',options||{});},
  createSprite:function(texture,options){options=options?Object.assign({},options):{};if(texture!=null&&options.texture==null)options.texture=texture;return createNativeOfType('sprite',options);},
  createNineSlice:function(texture,grid,options){options=options?Object.assign({},options):{};if(texture!=null)options.texture=texture;if(grid!=null)options.grid=grid;return createNativeOfType('nineslice',options);},
  createText:function(text,style,options){options=options?Object.assign({},options):{};if(text!=null)options.text=text;if(style!=null)options.style=style;return createNativeOfType('text',options);},
  createTiling:function(texture,width,height,options){options=options?Object.assign({},options):{};if(texture!=null)options.texture=texture;if(width!=null)options.width=width;if(height!=null)options.height=height;return createNativeOfType('tiling',options);},
  createGraphics:function(options){return createNativeOfType('graphics',options||{});},
  createParticleContainer:function(maxSize,properties,options){options=options?Object.assign({},options):{};if(maxSize!=null)options.maxSize=maxSize;if(properties!=null)options.properties=properties;return createNativeOfType('particle',options);},
  // Bulk construction — preferred for reel/symbol grids and other high-count trees.
  createBatch:createNativeBatch,
  createTree:createNativeTree,
  createPool:createNativePool,
  // Hierarchy helpers
  attach:function(parent,child,before){
    if(!parent||!child)return child;
    if(before&&parent.addChildAt&&parent.getChildIndex){try{var at=parent.getChildIndex(before);parent.addChildAt(child,at);return child;}catch(e){}}
    if(parent.addChild)parent.addChild(child);
    else if(typeof child.attachTo==='function')child.attachTo(parent,before||null);
    return child;
  },
  setChildren:function(parent,children){
    if(!parent)return [];
    children=children||[];
    if(parent.removeChildren)try{parent.removeChildren();}catch(e){}
    for(var i=0;i<children.length;i++)if(children[i]){
      if(parent.addChild)parent.addChild(children[i]);
      else if(typeof children[i].attachTo==='function')children[i].attachTo(parent);
    }
    return children;
  },
  applyOptions:applyNativeOptions,
  stats:function(){
    var st=globalThis.__nativeLeanStats||{};
    return {
      batchCreates:st.nativeBatchCreates||0,
      batchNodes:st.nativeBatchNodes||0,
      poolsCreated:st.nativePoolsCreated||0,
      poolWarm:st.nativePoolWarm||0,
      poolAcquires:st.nativePoolAcquires||0,
      poolReleases:st.nativePoolReleases||0,
      dynamicSprites:st.dynamicSpritesLive||0,
      dynamicContainers:st.dynamicContainersLive||0
    };
  }
};
// Runtime audit for business-created bitmap text that is attached below a native FGUI component.
// It reports both JS ownership and the authoritative native world matrix, which makes stale-parent
// and duplicate-visible-node bugs distinguishable from glyph layout bugs.
globalThis.__nativeBitmapReport=function(fontName){
  var out=[],objects=R.objects||[],want=fontName==null?'':String(fontName);
  function label(o){
    if(!o)return 'null';var id=o.__i|0,s=(o.name||o.constructor&&o.constructor.name||'?')+'#'+id;
    if(id>0){var b=id*ST;s+='['+Number(B[b+WA]).toFixed(2)+','+Number(B[b+WB]).toFixed(2)+','+Number(B[b+WC]).toFixed(2)+','+Number(B[b+WD]).toFixed(2)+' @'+Number(B[b+WTX]).toFixed(1)+','+Number(B[b+WTY]).toFixed(1)+' v'+(B[b+VISIBLE]!==0?1:0)+'r'+(B[b+RENDERABLE]!==0?1:0)+']';}
    return s;
  }
  function chain(o){var a=[],seen=[],guard=0;while(o&&guard++<24){if(seen.indexOf(o)>=0){a.push('[cycle]');break;}seen.push(o);a.push(label(o));o=o.parent;}return a.join(' <- ');}
  for(var i=1;i<objects.length;i++){
    var o=objects[i];if(!o||o.__i!==i||!o.__nativeBitmapText||(want&&o._fontName!==want))continue;
    var b=i*ST,text=String(o._text||'');
    out.push('#'+i+' font='+o._fontName+' text="'+text.slice(0,48).replace(/\n/g,'\\n')+'" local='+Number(B[b+X]).toFixed(1)+','+Number(B[b+Y]).toFixed(1)+' scale='+Number(B[b+SX]).toFixed(3)+','+Number(B[b+SY]).toFixed(3)+' visible='+((B[b+VISIBLE]!==0)&&(B[b+RENDERABLE]!==0)?1:0)+' world='+Number(B[b+WTX]).toFixed(1)+','+Number(B[b+WTY]).toFixed(1)+' wm='+Number(B[b+WA]).toFixed(3)+','+Number(B[b+WB]).toFixed(3)+','+Number(B[b+WC]).toFixed(3)+','+Number(B[b+WD]).toFixed(3)+' wa='+Number(B[b+WALPHA]).toFixed(3)+' parent='+chain(o.parent));
  }
  if(typeof __log==='function')__log('[bitmap-report] font='+(want||'*')+' count='+out.length+(out.length?'\n'+out.join('\n'):''));
  return out;
};
if((globalThis.__NATIVE_BITMAP_REPORT_AFTER|0)>0&&typeof setTimeout==='function')setTimeout(function(){globalThis.__nativeBitmapReport(globalThis.__NATIVE_BITMAP_REPORT_FONT||'');},globalThis.__NATIVE_BITMAP_REPORT_AFTER|0);
if(typeof __log==='function')__log('[native-proxy] lightweight NativeNodeProxy ready');
})();
