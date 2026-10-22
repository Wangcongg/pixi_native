import {P,V8,unsupported} from './environment.mjs'
import {applyOptions,installContainerAPI} from './container.mjs'
import {destroyOwnedTextures,withoutTextureOwnership} from './lifetime.mjs'
const owners=new Set(),R=globalThis.__nativeSceneRuntime
export class Particle extends V8.Particle{
  constructor(options){super(options instanceof P.Texture?{texture:options}:options)}
}
export class ParticleContainer extends P.ParticleContainer{
  constructor(options={}){super();if(options.shader)unsupported('ParticleContainer custom Shader');this.particleChildren=options.particles??[];this.texture=options.texture??null;this.dynamicProperties={position:true,rotation:false,vertex:false,uvs:false,color:false,...options.dynamicProperties};this.__nativeV8ParticleViews=[];this._childrenDirty=true;this.roundPixels=options.roundPixels??false;owners.add(this);applyOptions(this,options)}
  update(){this._childrenDirty=true;R.markVisual(this)}
  onViewUpdate(){this.update()}
  prepare(){
    const particles=this.particleChildren,views=this.__nativeV8ParticleViews,dirty=this._childrenDirty,d=this.dynamicProperties
    if(!this.texture&&particles.length)this.texture=particles[0].texture
    if(dirty)views.length=particles.length
    for(let i=0;i<views.length;i++){
      const p=particles[i];if(!p)continue
      let v=views[i];if(!v||dirty)v=views[i]={parent:this,isSprite:true,position:{x:0,y:0},scale:{x:1,y:1},anchor:{x:0,y:0},pivot:{x:0,y:0},skew:{x:0,y:0},alpha:1,tint:0xffffff,rotation:0}
      if(dirty||d.position){v.position.x=p.x;v.position.y=p.y}
      if(dirty||d.rotation)v.rotation=p.rotation
      if(dirty||d.vertex){v.anchor.x=p.anchorX;v.anchor.y=p.anchorY;v.scale.x=p.scaleX;v.scale.y=p.scaleY}
      if(dirty||d.uvs){v.texture=p.texture;if(v.texture.baseTexture!==this.texture.baseTexture)throw new Error('ParticleContainer particles must share one TextureSource')}
      if(dirty||d.color){v.tint=p.tint;v.alpha=p.alpha}
    }
    this._childrenDirty=false;R.markVisual(this)
  }
  getLocalBounds(){return this.boundsArea?new V8.Bounds(this.boundsArea.x,this.boundsArea.y,this.boundsArea.x+this.boundsArea.width,this.boundsArea.y+this.boundsArea.height):new V8.Bounds(0,0,0,0)}
  destroy(options){if(this.destroyed)return;const texture=this.texture??this.particleChildren[0]?.texture;owners.delete(this);this.__nativeV8ParticleViews.length=0;this.particleChildren=[];super.destroy(withoutTextureOwnership(options));this.texture=null;destroyOwnedTextures([texture],options)}
}
for(const method of ['addParticle','removeParticle','removeParticles','removeParticleAt','addParticleAt'])ParticleContainer.prototype[method]=V8.ParticleContainer.prototype[method]
const bounds=ParticleContainer.prototype.getLocalBounds
installContainerAPI(ParticleContainer.prototype)
ParticleContainer.prototype.getLocalBounds=bounds
const before=globalThis.__nativePreparePixi8Render
globalThis.__nativePreparePixi8Render=function(root){before?.(root);for(const o of owners){for(let p=o;p;p=p.parent){if(!p.visible||!p.renderable||p.culled||p.alpha<=0)break;if(p===root){o.prepare();break}}}}
