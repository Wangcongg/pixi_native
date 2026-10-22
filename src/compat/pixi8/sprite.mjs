import {P,V8} from './environment.mjs'
import {Texture} from './texture.mjs'
import {destroyOwnedTextures,withoutTextureOwnership} from './lifetime.mjs'
import {applyOptions,installContainerAPI,installViewHitAPI} from './container.mjs'
const optionsOf=o=>o instanceof P.Texture?{texture:o}:o||{}
function destroyOptions(o){return o&&typeof o==='object'?{...o,baseTexture:o.textureSource??o.baseTexture}:o}
export class Sprite extends P.Sprite{
  constructor(options){const o=optionsOf(options);super(o.texture);applyOptions(this,o)}
  containsPoint(point){const r=this.texture.orig,x=-r.width*this.anchor.x,y=-r.height*this.anchor.y;return point.x>=x&&point.x<x+r.width&&point.y>=y&&point.y<y+r.height}
  static from(source,skipCache){return new Sprite(Texture.from(source,skipCache))}
  destroy(o){super.destroy(destroyOptions(o))}
}
Sprite.prototype.__nativeV8LocalHit=true
installContainerAPI(Sprite.prototype);installViewHitAPI(Sprite.prototype)
export class AnimatedSprite extends P.AnimatedSprite{
  constructor(options,autoUpdate){const o=Array.isArray(options)?{textures:options,autoUpdate}:options;super(o.textures,false);this._v8AutoUpdate=o.autoUpdate??true;for(const k of ['animationSpeed','loop','updateAnchor','onComplete','onFrameChange','onLoop'])if(o[k]!==undefined)this[k]=o[k];applyOptions(this,o);if(o.autoPlay)this.play()}
  get autoUpdate(){return this._v8AutoUpdate}set autoUpdate(v){this._v8AutoUpdate=!!v;if(this.playing){this.disconnect();if(v)this.connect()}}
  connect(){if(!this._v8Connected){V8.Ticker.shared.add(this.update,this,V8.UPDATE_PRIORITY.HIGH);this._v8Connected=true}}
  disconnect(){if(this._v8Connected){V8.Ticker.shared.remove(this.update,this);this._v8Connected=false}}
  play(){if(this.playing)return;this._playing=true;if(this.autoUpdate)this.connect()}
  stop(){this._playing=false;this.disconnect()}
  update(ticker){V8.AnimatedSprite.prototype.update.call(this,typeof ticker==='number'?{deltaTime:ticker}:ticker)}
  _updateTexture(){this.updateTexture()}
  destroy(o){if(this.destroyed)return;const textures=this.textures.slice();this.disconnect();super.destroy(withoutTextureOwnership(o));destroyOwnedTextures(textures,o)}
  static fromFrames(frames){return new AnimatedSprite(frames.map(f=>Texture.from(f)))}
  static fromImages(images){return new AnimatedSprite(images.map(f=>Texture.from(f)))}
}
installContainerAPI(AnimatedSprite.prototype);installViewHitAPI(AnimatedSprite.prototype)
export class NineSliceSprite extends P.NineSlicePlane{
  constructor(options={}){const o=optionsOf(options),b=o.texture?.defaultBorders;super(o.texture,o.leftWidth??b?.left,o.topHeight??b?.top,o.rightWidth??b?.right,o.bottomHeight??b?.bottom);applyOptions(this,o)}
  destroy(o){if(this.destroyed)return;const texture=this.texture;super.destroy(withoutTextureOwnership(o));destroyOwnedTextures([texture],o)}
}
installContainerAPI(NineSliceSprite.prototype);installViewHitAPI(NineSliceSprite.prototype)
export class TilingSprite extends P.TilingSprite{
  constructor(options={}){const o=optionsOf(options);super(o.texture,o.width??o.texture?.width??100,o.height??o.texture?.height??100);this.applyAnchorToTexture=o.applyAnchorToTexture??false;applyOptions(this,o);for(const k of ['tilePosition','tileScale'])if(o[k])this[k].copyFrom(o[k]);if(o.tileRotation!==undefined)this.tileRotation=o.tileRotation}
  get applyAnchorToTexture(){return this.uvRespectAnchor}set applyAnchorToTexture(v){this.uvRespectAnchor=v}
  get tileRotation(){return this.tileTransform.rotation}set tileRotation(v){this.tileTransform.rotation=v;globalThis.__nativeSceneRuntime.markVisual(this)}
  static from(source,options={}){return new TilingSprite({...options,texture:Texture.from(source)})}
  destroy(o){if(this.destroyed)return;const texture=this.texture;super.destroy(withoutTextureOwnership(o));destroyOwnedTextures([texture],o)}
}
installContainerAPI(TilingSprite.prototype);installViewHitAPI(TilingSprite.prototype)
