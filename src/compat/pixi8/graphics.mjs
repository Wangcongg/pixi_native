import {P,V8,unsupported} from './environment.mjs'
import {nativeTexture} from './texture.mjs'
import {installContainerAPI,applyOptions} from './container.mjs'
export {FillGradient} from './gradient.mjs'
const R=globalThis.__nativeSceneRuntime
export const GraphicsPath=V8.GraphicsPath
export class FillPattern extends V8.FillPattern{constructor(...args){super(...args);this.texture.source.style.update()}}
export class GraphicsContext extends V8.GraphicsContext{
  stroke(style){if(style?.pixelLine||(!style&&this.strokeStyle.pixelLine))unsupported('Graphics stroke.pixelLine');return super.stroke(style)}
  svg(value){if(typeof value==='string'){if(typeof globalThis.__nativeFguiXmlParse!=='function')unsupported('GraphicsContext.svg XML parser');value=svgElement(globalThis.__nativeFguiXmlParse(value));if(!value||value.nodeName.toLowerCase()!=='svg')throw new Error('SVG root missing')}return super.svg(value)}
}
function svgElement(node){if(!node)return null;const attrs=node.$attributes||node.attributes||{},children=(node.$children||node.children||[]).map(svgElement);return {nodeName:node.nodeName||node.name,id:attrs.id,children,getAttribute:name=>attrs[name]??null,querySelectorAll(name){const out=[];function visit(n){for(const c of n.children){if(c.nodeName.toLowerCase()===name.toLowerCase())out.push(c);visit(c)}}visit(this);return out}}}
class NativeContextGeometry extends P.GraphicsGeometry{
  constructor(owner){super();this.owner=owner;this.gpu={geometryData:{vertices:[],uvs:[],indices:[]},batches:[]};this.revision=-1}
  updateBatches(){const context=this.owner.context;if(!context||this.revision===context.__nativeRevision)return;for(const batch of this.gpu.batches)V8.BigPool.return(batch);V8.buildContextBatches(context,this.gpu);const d=this.gpu.geometryData;this.points=d.vertices.slice();this.uvs=d.uvs.slice();this.indices=d.indices.slice();this.batches=this.gpu.batches.map(b=>{if(b.topology!=='triangle-list')unsupported('Graphics stroke.pixelLine');return {start:b.indexOffset,size:b.indexSize,style:{color:b.baseColor,alpha:b.alpha,texture:b.texture===V8.Texture.WHITE?P.Texture.WHITE:nativeTexture(b.texture)}}});this.revision=context.__nativeRevision;this.dirty++;this.batchDirty++}
  dispose(){for(const batch of this.gpu.batches)V8.BigPool.return(batch);this.gpu.batches.length=0;super.dispose()}
}
export class Graphics extends P.Graphics{
  constructor(options={}){super();this.__nativeV8Graphics=true;if(options instanceof V8.GraphicsContext)options={context:options};this._ownedContext=options.context?null:new GraphicsContext();this._context=null;this._geometry.refCount--;this._geometry.dispose();this._geometry=new NativeContextGeometry(this);this._geometry.refCount=1;this.context=options.context||this._ownedContext;const nativeTransform=this.transform,self=this;const drawingTransform=function(...args){self.context.transform(...args);return self};Object.defineProperties(drawingTransform,Object.getOwnPropertyDescriptors(nativeTransform));this.transform=drawingTransform;applyOptions(this,options)}
  get context(){return this._context}set context(value){if(this._context===value)return;this._context?.off('update',this.onContextUpdate,this);this._context?.off('destroy',this.onContextDestroy,this);this._context=value;if(value){value.__nativeRevision??=0;value.on('update',this.onContextUpdate,this);value.on('destroy',this.onContextDestroy,this)}this.onContextUpdate()}
  onContextUpdate(){if(this._context)this._context.__nativeRevision++;R.markVisual(this);if(this.__maskOf)R.markVisual(this.__maskOf)}
  onContextDestroy(){this.context=null}
  finishPoly(){} // V8 paths are committed by fill/stroke, never by presentation.
  containsPoint(point){return this.context?.containsPoint(point)||false}
  getLocalBounds(){return this.context?this.context.bounds:new V8.Bounds()}
  get fillStyle(){return this.context.fillStyle}set fillStyle(v){this.context.fillStyle=v}
  get strokeStyle(){return this.context.strokeStyle}set strokeStyle(v){this.context.strokeStyle=v}
  clone(deep=false){return new Graphics(deep?this.context.clone():this.context)}
  destroy(options){if(this.destroyed)return;const owned=this._ownedContext,context=this.context;this._context?.off('update',this.onContextUpdate,this);this._context?.off('destroy',this.onContextDestroy,this);super.destroy(options);this._context=null;if(options===true||options?.context===true||(owned&&options?.context!==false))context?.destroy();this._ownedContext=null}
}
Graphics.prototype.__nativeV8LocalHit=true
const contextBounds=Graphics.prototype.getLocalBounds
installContainerAPI(Graphics.prototype)
Graphics.prototype.getLocalBounds=contextBounds
// Use the public Graphics forwarding methods verbatim; V8 deliberately renames
// scale/rotate/translate to avoid colliding with Container transform properties.
for(const name of Object.getOwnPropertyNames(V8.Graphics.prototype)){
  if(['constructor','context','bounds','updateBounds','containsPoint','destroy','_onTouch','clone','fillStyle','strokeStyle','transform'].includes(name)||Object.hasOwn(Graphics.prototype,name))continue
  const descriptor=Object.getOwnPropertyDescriptor(V8.Graphics.prototype,name);if(typeof descriptor.value!=='function')continue
  Object.defineProperty(Graphics.prototype,name,{...descriptor,configurable:true})
}
