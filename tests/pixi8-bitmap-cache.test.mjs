import test from 'node:test'
import assert from 'node:assert/strict'
import {createNativeHost} from './helpers/native-host.mjs'
const {P,R}=createNativeHost()
globalThis.PIXI=P;globalThis.__nativeSceneRuntime=R
globalThis.navigator??={userAgent:'Native bitmap and cache contracts'}
const V=await import('../src/compat/pixi8.mjs')

test('dynamic BitmapFont appends Unicode code points without replacing live glyph textures',()=>{
  const from=P.BitmapFont.from,allocations=[]
  P.BitmapFont.from=(name,style,options)=>{
    const page={destroy(){}},font={font:name,chars:{},pageTextures:{0:page},destroy(){this.chars=null;this.pageTextures=null}}
    for(const ch of Array.from(options.chars))font.chars[ch.codePointAt(0)]={page:0,texture:{},kerning:{}}
    allocations.push(options.chars);P.BitmapFont.available[name]?.destroy();P.BitmapFont.available[name]=font;return font
  }
  try{
    const font=V.BitmapFont.install({name:'UnicodeContract',chars:'A',style:{fontFamily:'sans-serif',fontSize:18}}),glyph=font.chars[65].texture
    font.ensureCharacters('A中🙂中\n');font.ensureCharacters('中🙂')
    assert.deepEqual(allocations,['A','中🙂'])
    assert.equal(font.chars[65].texture,glyph)
    assert.ok(font.chars[0x1f642]);assert.equal(font.chars[0x1f642].page,1)
    assert.equal(Object.keys(font.pageTextures).length,2)
    assert.equal(P.BitmapFont.available.UnicodeContract,font)
  }finally{P.BitmapFont.from=from;delete P.BitmapFont.available.UnicodeContract}
})
test('cacheAsTexture owns a native RT, accepts resolution and invalidates without changing authored children',()=>{
  const root=new V.Container(),child=new V.Sprite({texture:V.Texture.WHITE}),destroyed=[]
  root.addChild(child);root.cacheAsTexture({resolution:2,antialias:false})
  assert.equal(root.isCachedAsTexture,true);assert.equal(root._cacheAsBitmapResolution,2)
  root._cacheData.sprite={destroy(){destroyed.push('sprite')}}
  root._cacheData.__nativeRT={destroy(source){assert.equal(source,true);destroyed.push('target')}}
  root.__nativeCachedTextureActive=true;root._cacheData.__nativeBakeDirty=0;root.updateCacheTexture()
  assert.equal(root._cacheData.__nativeBakeDirty,1);assert.equal(root.__nativeCachedTextureActive,false)
  assert.equal(child.parent,root);assert.equal(child.renderable,true)
  root.cacheAsTexture(false);assert.equal(root.isCachedAsTexture,false)
  assert.deepEqual(destroyed,['sprite','target']);assert.equal(root._cacheData,null)
  assert.throws(()=>root.cacheAsTexture({antialias:true}),V.UnsupportedNativeAPIError)
  assert.throws(()=>root.cacheAsTexture({resolution:0}),RangeError)
  assert.throws(()=>child.blendMode=4,V.UnsupportedNativeAPIError)
  root.destroy({children:true})
})
test('cache disposal survives a native flag setter that clears its cache record',()=>{
  const root=new V.Container(),destroyed=[];let flag=false
  Object.defineProperty(root,'_cacheAsBitmap',{get(){return flag},set(value){flag=value;if(!value&&this._cacheData){this._cacheData.sprite=null;this._cacheData.__nativeRT=null}}})
  root.cacheAsTexture(true)
  root._cacheData.sprite={destroy(){destroyed.push('view')}}
  root._cacheData.__nativeRT={destroy(){destroyed.push('source')}}
  root.destroy()
  assert.deepEqual(destroyed,['view','source'])
  assert.equal(root.destroyed,true)
})
