import {P,V8,alphaNumber,unsupported} from './environment.mjs'
const sources=new WeakMap(),converted=new WeakMap()
export const TextureStyle=V8.TextureStyle
function validateSourceOptions(options){
  for(const [key,value] of Object.entries({format:'rgba8unorm',dimensions:'2d',viewDimension:'2d',arrayLayerCount:1,sampleCount:1,mipLevelCount:1}))if(options[key]!==undefined&&options[key]!==value)unsupported('TextureSource.'+key,String(options[key]))
  if(options.antialias)unsupported('TextureSource.antialias')
  if(options.dimension!==undefined&&options.dimension!=='2d')unsupported('TextureSource.dimension',String(options.dimension))
}
export class TextureSource extends P.utils.EventEmitter{
  constructor(options={},base){
    super();validateSourceOptions(options);this._base=base||makeBase(options);this.label=options.label??'';this.autoGarbageCollect=options.autoGarbageCollect??false;this._sourceDestroyed=false
    this._style=new TextureStyle({scaleMode:this._base.scaleMode===0?'nearest':'linear',addressMode:this._base.wrapMode===P.WRAP_MODES.REPEAT?'repeat':this._base.wrapMode===P.WRAP_MODES.MIRRORED_REPEAT?'mirror-repeat':'clamp-to-edge',...options});this._ownsStyle=true
    this._style.on('change',this.applyStyle,this);this.applyStyle();this._pixelWidth=this.pixelWidth;this._pixelHeight=this.pixelHeight
    this._base.on('update',this.onBaseUpdate,this);this._base.on('dispose',this.onBaseDispose,this);this._base.on('destroyed',this.onBaseDestroy,this);sources.set(this._base,this)
  }
  applyStyle(){this._base.scaleMode=this._style.scaleMode==='nearest'?P.SCALE_MODES.NEAREST:P.SCALE_MODES.LINEAR;const u=this._style.addressModeU,v=this._style.addressModeV;this._base.wrapMode=u==='repeat'?P.WRAP_MODES.REPEAT:u==='mirror-repeat'?P.WRAP_MODES.MIRRORED_REPEAT:P.WRAP_MODES.CLAMP;this._base.anisotropicLevel=this._style.maxAnisotropy;this._base.__nativeV8Sampler={u,v,mag:this._style.magFilter,min:this._style.minFilter,revision:(this._base.__nativeV8Sampler?.revision||0)+1};this._base.dirtyStyleId++;this.emit('styleChange',this)}
  get style(){return this._style}set style(value){if(value===this._style)return;this._style?.off('change',this.applyStyle,this);this._style=value;this._ownsStyle=false;value?.on('change',this.applyStyle,this);if(value)this.applyStyle()}
  onBaseUpdate(){const changed=this._pixelWidth!==this.pixelWidth||this._pixelHeight!==this.pixelHeight;this._pixelWidth=this.pixelWidth;this._pixelHeight=this.pixelHeight;if(changed){this.emit('resize',this);this.emit('change',this)}this.emit('update',this)}
  onBaseDispose(){this.emit('change',this);this.emit('unload',this)}
  onBaseDestroy(){if(this._sourceDestroyed)return;this._sourceDestroyed=true;this._base.off('update',this.onBaseUpdate,this);this._base.off('dispose',this.onBaseDispose,this);this._base.off('destroyed',this.onBaseDestroy,this);this._style?.off('change',this.applyStyle,this);if(this._ownsStyle)this._style?.destroy();this._style=null;this.emit('destroy',this);this.removeAllListeners()}
  get resource(){return this._base.resource?.source??this._base.resource?.data}get width(){return this._base.width}get height(){return this._base.height}get pixelWidth(){return this._base.realWidth}get pixelHeight(){return this._base.realHeight}get resolution(){return this._base.resolution}set resolution(v){this._base.setResolution(v)}
  get alphaMode(){return ['no-premultiply-alpha','premultiply-alpha-on-upload','premultiplied-alpha'][this._base.alphaMode]}set alphaMode(v){this._base.alphaMode=alphaNumber(v)}
  get scaleMode(){return this.style.scaleMode}set scaleMode(v){this.style.scaleMode=v;this.style.update()}
  get destroyed(){return this._base.destroyed}get format(){return 'rgba8unorm'}
  get source(){return this}get dimension(){return '2d'}get viewDimension(){return '2d'}get sampleCount(){return 1}get mipLevelCount(){return 1}get arrayLayerCount(){return 1}
  update(){this._base.resource?.update();this._base.update()}
  resize(width,height,resolution){const w=this.pixelWidth,h=this.pixelHeight;if(this._base.framebuffer){if(resolution)this._base.setResolution(resolution);this._base.resize(width||this.width,height||this.height)}else this._base.setSize(width||this.width,height||this.height,resolution||this.resolution);return w!==this.pixelWidth||h!==this.pixelHeight}
  unload(){if(!this.destroyed)this._base.dispose()}destroy(){if(!this._sourceDestroyed)this._base.destroy()}
}
for(const key of ['maxAnisotropy','addressMode','repeatMode','magFilter','minFilter','mipmapFilter','lodMinClamp','lodMaxClamp','wrapMode'])Object.defineProperty(TextureSource.prototype,key,{get(){return this.style[key]},set(value){this.style[key]=value}})
function makeBase(o){let resource=o.resource;if(ArrayBuffer.isView(resource)){if(!(resource instanceof Uint8Array||resource instanceof Uint8ClampedArray))unsupported('TextureSource.resource','requires RGBA8 byte data');resource=new P.BufferResource(resource,{width:Math.round(o.width*(o.resolution||1)),height:Math.round(o.height*(o.resolution||1))})}const opts={...o,alphaMode:alphaNumber(o.alphaMode),scaleMode:o.scaleMode==='nearest'?P.SCALE_MODES.NEAREST:P.SCALE_MODES.LINEAR};return resource?new P.BaseTexture(resource,opts):new P.BaseRenderTexture({width:1,height:1,...opts})}
export function sourceOf(base){return sources.get(base)||new TextureSource({},base)}
Object.defineProperties(P.Texture.prototype,{source:{configurable:true,get(){return this.baseTexture?sourceOf(this.baseTexture):null},set(value){const base=value._base;if(!base)throw new TypeError('Texture.source requires a native TextureSource');if(base===this.baseTexture)return;const oldBase=this.baseTexture;this.baseTexture?.off('loaded',this.onBaseTextureUpdated,this);this.baseTexture?.off('update',this.onBaseTextureUpdated,this);this.baseTexture=base;globalThis.__nativeSceneRuntime.rebindTextureSource?.(this,oldBase,base);if(!base.valid)base.once('loaded',this.onBaseTextureUpdated,this);if(this.noFrame)base.on('update',this.onBaseTextureUpdated,this);if(base.valid)this.onBaseTextureUpdated(base);else this.emit('update',this)}},isTexture:{get(){return true}},uvs:{configurable:true,get(){return this._uvs}},label:{configurable:true,get(){return this.__v8Label},set(v){this.__v8Label=v}}})
const destroyTexture=P.Texture.prototype.destroy
P.Texture.prototype.destroy=function(destroySource){if(this.destroyed)return;this.once('destroyed',function(){this.emit('destroy',this)});return destroyTexture.call(this,destroySource)}
export class ImageSource extends TextureSource{}
export class CanvasSource extends TextureSource{}
export class BufferImageSource extends TextureSource{constructor(options={}){if(!ArrayBuffer.isView(options.resource))unsupported('BufferImageSource.resource','requires RGBA8 byte data');super(options)}}
export class Texture extends P.Texture{
  constructor(o={}){super(o.source?o.source._base||o.source:new TextureSource()._base,o.frame,o.orig,o.trim,o.rotate,o.defaultAnchor,o.defaultBorders);this.label=o.label;this.dynamic=o.dynamic??false}
  update(){if(this.noFrame){this.frame.width=this.baseTexture.width;this.frame.height=this.baseTexture.height}this.updateUvs();this.emit('update',this)}
  static from(source,skipCache=false){if(source instanceof P.Texture)return source;return P.Texture.from(source instanceof TextureSource?source._base:source)}
  static get WHITE(){return P.Texture.WHITE}static get EMPTY(){return P.Texture.EMPTY}
  static [Symbol.hasInstance](value){return value instanceof P.Texture}
}
function nativeBase(source){
  if(source._base)return source._base
  let base=converted.get(source)
  if(!base||base.destroyed){
    const resource=source.resource;if(!resource)unsupported('Graphics texture source',source.constructor.name)
    validateSourceOptions(source)
    base=makeBase({resource,width:source.width,height:source.height,resolution:source.resolution,alphaMode:source.alphaMode});converted.set(source,base)
    sourceOf(base).style=source.style
    const update=()=>{if(base.destroyed)return;if(base.resource?.data&&ArrayBuffer.isView(source.resource))base.resource.data=source.resource;base.setRealSize(source.pixelWidth,source.pixelHeight,source.resolution);base.resource?.update();base.update()},unload=()=>base.dispose(),destroy=()=>base.destroy()
    source.on('update',update);source.on('resize',update);source.on('unload',unload);source.on('destroy',destroy)
    base.on('destroyed',()=>{source.off('update',update);source.off('resize',update);source.off('unload',unload);source.off('destroy',destroy)})
  }
  return base
}
export function nativeTexture(texture){
  if(texture instanceof P.Texture)return texture
  if(!texture?.source)throw new TypeError('Expected a texture')
  let cached=converted.get(texture);if(cached&&!cached.destroyed)return cached
  cached=new P.Texture(nativeBase(texture.source),texture.frame.clone(),texture.orig.clone(),texture.trim?.clone(),texture.rotate,texture.defaultAnchor)
  const native=cached,update=()=>{native.source=sourceOf(nativeBase(texture.source));native._frame=texture.frame.clone();native.orig=texture.orig.clone();native.trim=texture.trim?.clone()??null;native.rotate=texture.rotate;native.updateUvs();native.emit('update',native)},destroy=()=>{if(!native.destroyed)native.destroy(false)}
  texture.on('update',update);texture.on('destroy',destroy)
  const nativeDestroy=native.destroy;native.destroy=function(...args){texture.off('update',update);texture.off('destroy',destroy);return nativeDestroy.apply(this,args)}
  converted.set(texture,cached);return cached
}
export class RenderTexture extends P.RenderTexture{
  constructor(options={}){const source=options.source??new TextureSource(),base=source._base;if(!base?.framebuffer)unsupported('RenderTexture source','requires a resource-less native TextureSource');super(base,options.frame);this.label=options.label;this.dynamic=options.dynamic??false;this.defaultBorders=options.defaultBorders;if(options.orig)this.orig=options.orig;if(options.trim)this.trim=options.trim;if(options.rotate!==undefined)this.rotate=options.rotate;if(options.defaultAnchor)this.defaultAnchor.copyFrom(options.defaultAnchor)}
  static create(options={}){validateSourceOptions(options);const {dynamic,textureOptions,...rest}=options;return new RenderTexture({...textureOptions,source:new TextureSource(rest),dynamic:dynamic??false})}
  update(){return Texture.prototype.update.call(this)}
  resize(width,height,resolution){if(resolution!==undefined)this.baseTexture.setResolution(resolution);super.resize(width,height,true);return this}
}
export class Spritesheet extends P.Spritesheet{constructor(options,data){super(options.texture||options,options.data||data,options.cachePrefix)}}
const dataOptions=item=>{if(!item||typeof item!=='object')return item;return {...item,data:item.data?{...item.data,alphaMode:alphaNumber(item.data.alphaMode)}:item.data}}
export const Assets=new Proxy(P.Assets,{get(target,key){if(key==='load')return (items,...args)=>target.load(Array.isArray(items)?items.map(dataOptions):dataOptions(items),...args);const value=target[key];return typeof value==='function'?value.bind(target):value}})
export const Cache=P.Cache

const applyBefore=globalThis.__nativeApplyTextureStyle
globalThis.__nativeApplyTextureStyle=function(base,renderer){applyBefore?.(base,renderer);const s=base.__nativeV8Sampler;const wrapper=base._glTextures?.[renderer.CONTEXT_UID];if(!s||s.applied===s.revision&&s.wrapper===wrapper)return;renderer.texture.bind(base,0);const gl=renderer.gl,wrap={repeat:gl.REPEAT,'mirror-repeat':gl.MIRRORED_REPEAT,'clamp-to-edge':gl.CLAMP_TO_EDGE};gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,wrap[s.u]);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,wrap[s.v]);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,s.mag==='nearest'?gl.NEAREST:gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,s.min==='nearest'?gl.NEAREST:gl.LINEAR);s.applied=s.revision;s.wrapper=base._glTextures?.[renderer.CONTEXT_UID]}
