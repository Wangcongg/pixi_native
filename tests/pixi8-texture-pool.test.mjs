import test from 'node:test'
import assert from 'node:assert/strict'
import {createNativeHost} from './helpers/native-host.mjs'
const {P,R}=createNativeHost()
globalThis.PIXI=P;globalThis.__nativeSceneRuntime=R
globalThis.navigator??={userAgent:'Native pool contracts'}
const V=await import('../src/compat/pixi8.mjs'),O=await import('../vendor/pixi.v8.20.0.mjs')
test('TexturePool reuses native render targets with official power-of-two frames and high-DPI dimensions',()=>{
  const pool=new V.TexturePoolClass({scaleMode:'nearest'}),reference=new O.TexturePoolClass()
  const a=pool.getOptimalTexture(13,9,2),b=reference.getOptimalTexture(13,9,2)
  assert.ok(a.baseTexture.framebuffer)
  for(const key of ['width','height','pixelWidth','pixelHeight','resolution'])assert.equal(a.source[key],b.source[key],key)
  assert.equal(a.width,13);assert.equal(a.height,9)
  assert.throws(()=>new V.TexturePoolClass().returnTexture(a),/not checked out/)
  pool.returnTexture(a,true);assert.throws(()=>pool.returnTexture(a),/not checked out/)
  const reused=pool.getOptimalTexture(28,18,1)
  assert.equal(reused,a);assert.equal(reused.source.pixelWidth,32);assert.equal(reused.source.width,32)
  reused.baseTexture.update();assert.equal(reused.width,28);assert.equal(reused.height,18)
  const same=pool.getSameSizeTexture(reused);assert.notEqual(same,reused);assert.equal(same.width,28)
  pool.returnTexture(reused);pool.returnTexture(same);pool.clear()
  assert.equal(a.destroyed,true);assert.equal(same.destroyed,true)
  reference.returnTexture(b);reference.clear()
  assert.throws(()=>pool.getOptimalTexture(10,10,1,true),V.UnsupportedNativeAPIError)
  assert.throws(()=>pool.getOptimalTexture(10,10,1,false,true),V.UnsupportedNativeAPIError)
})
test('TextureMatrix and TextureUvs use native atlas and resolution data without NaN clamps or retained listeners',()=>{
  const texture=new V.Texture({source:new V.TextureSource({width:32,height:16,resolution:2}),frame:new V.Rectangle(4,2,12,8)}),matrix=new V.TextureMatrix(texture,.5)
  assert.ok(Array.from(matrix.uClampFrame).every(Number.isFinite))
  assert.equal(matrix.uClampFrame[0],(4+.25)/32)
  assert.deepEqual(Array.from(matrix.multiplyUvs(new Float32Array([0,0,1,1]))),[.125,.125,.5,.625])
  const before=texture.listenerCount('update');matrix.destroy();assert.equal(texture.listenerCount('update'),before-1)
  const uvs=new V.TextureUvs();uvs.set(texture.frame,texture.source,2)
  assert.ok(Array.from(uvs.uvsFloat32).every(Number.isFinite));texture.destroy(true)
})
