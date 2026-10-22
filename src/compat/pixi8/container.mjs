import {P,V8,blendNumber,nativeBlendModes,unsupported} from './environment.mjs'
const R=globalThis.__nativeSceneRuntime,onRenderOwners=new Set()
const blendNames=Object.fromEntries(Object.entries(nativeBlendModes).map(([k,v])=>[v,k]))
function descriptor(proto,name){while(proto){const d=Object.getOwnPropertyDescriptor(proto,name);if(d)return d;proto=Object.getPrototypeOf(proto)}}
export function boundsOf(rect,out=new V8.Bounds()){out.clear();out.addFrame(rect.x,rect.y,rect.x+rect.width,rect.y+rect.height);return out}
export function applyOptions(node,options={}){
  for(const k of ['label','x','y','rotation','angle','alpha','visible','renderable','eventMode','interactiveChildren','hitArea','cursor','tint','blendMode','zIndex','sortableChildren','mask','filters','filterArea','cullable','cullableChildren','cullArea','boundsArea','roundPixels','onRender'])if(options[k]!==undefined)node[k]=options[k]
  for(const k of ['position','scale','pivot','skew','origin','anchor'])if(options[k]!==undefined&&node[k]){const v=options[k];typeof v==='number'?node[k].set(v):node[k].copyFrom(v)}
  for(const k of ['width','height'])if(options[k]!==undefined)node[k]=options[k]
  if(options.children)node.addChild(...options.children)
  if(options.isRenderGroup)node.enableRenderGroup()
  return node
}
export function installContainerAPI(proto){
  const base=Object.getPrototypeOf(proto),blend=descriptor(base,'blendMode'),local=base.getLocalBounds,global=base.getBounds,renderable=descriptor(base,'renderable')
  Object.defineProperties(proto,{
    origin:{configurable:true,get(){if(!this.__v8Origin){const self=this;this.__v8Origin=new V8.ObservablePoint({ _onUpdate(){for(const point of [self.position,self.pivot]){const x=point.x,y=point.y;point.__offsetX=self.__v8Origin.x;point.__offsetY=self.__v8Origin.y;point.set(x,y)}}},0,0)}return this.__v8Origin},set(value){this.origin.copyFrom(value)}},
    onRender:{configurable:true,get(){return this.__v8OnRender??null},set(v){this.__v8OnRender=v;if(v)onRenderOwners.add(this);else onRenderOwners.delete(this)}},
    renderable:{configurable:true,get(){return this.__v8Renderable??renderable.get.call(this)},set(v){this.__v8Renderable=!!v;renderable.set.call(this,v&&!this.__v8Culled)}},
    isCachedAsTexture:{get(){return this.cacheAsBitmap}},
    culled:{get(){return this.__v8Culled||false},set(v){this.__v8Renderable=this.renderable;this.__v8Culled=!!v;renderable.set.call(this,this.__v8Renderable&&!v)}},
    isRenderable:{get(){return this.visible&&this.renderable&&this.alpha>0}},
    label:{configurable:true,get(){return this.__v8Label??null},set(v){this.__v8Label=v}},
    blendMode:{configurable:true,get(){return blendNames[blend.get.call(this)]??'normal'},set(v){blend.set.call(this,blendNumber(v))}},
    worldTint:{get(){return this.getGlobalTint()}},
  })
  Object.assign(proto,{
    getChildByLabel:V8.Container.prototype.getChildByLabel,
    getChildrenByLabel:V8.Container.prototype.getChildrenByLabel,
    setFromMatrix(matrix){matrix.decompose(this);return this},
    updateLocalTransform(){this.transform.updateLocalTransform()},
    updateTransform(options={}){for(const k of ['x','y','rotation'])if(options[k]!==undefined)this[k]=options[k];for(const k of ['scale','pivot','skew','origin']){if(options[k+'X']!==undefined)this[k].x=options[k+'X'];if(options[k+'Y']!==undefined)this[k].y=options[k+'Y']}return this},
    getLocalBounds(){return this.boundsArea?boundsOf(this.boundsArea):boundsOf(local.call(this))},
    getBounds(skipUpdate=false,out){return boundsOf(global.call(this,skipUpdate),out)},
    getFastGlobalBounds(skipUpdate=false,out){return this.getBounds(skipUpdate,out)},
    getSize(out={}){out.width=this.width;out.height=this.height;return out},
    setSize(value,height){if(typeof value==='object'){height=value.height??value.width;value=value.width}height??=value;if(value!==undefined)this.width=value;if(height!==undefined)this.height=height},
    reparentChild(...children){children.forEach(c=>this.reparentChildAt(c,this.children.length));return children[0]},
    reparentChildAt(child,index){if(child.parent===this){this.setChildIndex(child,index);return child}child.toGlobal(new P.Point());const old=child.worldTransform.clone();this.toGlobal(new P.Point());const next=this.worldTransform.clone().invert().append(old);this.addChildAt(child,index);child.setFromMatrix(next);return child},
    replaceChild(old,next){this.addChildAt(next,this.getChildIndex(old));old.transform.updateLocalTransform();next.setFromMatrix(old.localTransform);this.removeChild(old)},
    getGlobalTransform(out=new V8.Matrix(),skipUpdate=false){if(!skipUpdate)this.toGlobal(new P.Point());this.worldTransform.copyTo(out);return out},
    getGlobalAlpha(skipUpdate=false){let alpha=1;for(let p=this;p;p=p.parent)alpha*=p.alpha;return alpha},
    getGlobalTint(skipUpdate=false){let r=255,g=255,b=255;for(let p=this;p;p=p.parent){const c=p.tint??0xffffff;r=r*((c>>16)&255)/255;g=g*((c>>8)&255)/255;b=b*(c&255)/255}return (Math.round(r)<<16)|(Math.round(g)<<8)|Math.round(b)},
    cacheAsTexture(value){if(value&&typeof value==='object'&&(value.resolution!==undefined||value.antialias!==undefined))unsupported('cacheAsTexture options');this.cacheAsBitmap=!!value},
    updateCacheTexture(){this.cacheAsBitmap=false;this.cacheAsBitmap=true},
    setMask(options){if(options.inverse)unsupported('Container.setMask inverse');if(options.channel&&options.channel!=='red')unsupported('Container.setMask channel',options.channel);if(options.mask!==undefined)this.mask=options.mask},
    enableRenderGroup(){unsupported('Container.enableRenderGroup')},
    disableRenderGroup(){unsupported('Container.disableRenderGroup')},
  })
  return proto
}
export class Container extends P.Container{constructor(options){super();this.cullableChildren=true;applyOptions(this,options)}}
installContainerAPI(Container.prototype)

const before=globalThis.__nativePreparePixi8Render
globalThis.__nativePreparePixi8Render=function(root){before?.(root);for(const o of onRenderOwners){if(o.destroyed){onRenderOwners.delete(o);continue}let linked=false,visible=true;for(let p=o;p;p=p.parent){visible&&=p.visible&&p.renderable&&!p.culled&&p.alpha>0;if(p===root){linked=true;break}}if(linked&&visible)o.__v8OnRender.call(o,globalThis.__nativeRenderer)}}

export function installViewHitAPI(proto){if(!Object.hasOwn(proto,'containsPoint')){const contains=proto.containsPoint;proto.containsPoint=function(point){return contains.call(this,this.toGlobal(point))}}proto.__nativeV8LocalHit=true}
