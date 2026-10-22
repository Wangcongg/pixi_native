import test from 'node:test'
import assert from 'node:assert/strict'
import {createNativeHost} from './helpers/native-host.mjs'
const {P,R}=createNativeHost()
globalThis.PIXI=P;globalThis.__nativeSceneRuntime=R
globalThis.navigator??={userAgent:'Native geometry contracts'}
P.settings.ADAPTER={...P.settings.ADAPTER,createCanvas:()=>({getContext:()=>null})}
const V=await import('../src/compat/pixi8.mjs'),O=await import('../vendor/pixi.v8.20.0.mjs')
const packed=g=>Array.from(g.getBuffer('aVertexPosition').data)
const same=(a,b)=>{assert.deepEqual(Array.from(a.positions),Array.from(b.positions));assert.deepEqual(Array.from(a.uvs),Array.from(b.uvs));assert.deepEqual(Array.from(a.indices),Array.from(b.indices))}

test('generic Geometry packs interleaved attributes and non-indexed vertices for the native renderer',()=>{
  const buffer=new V.Buffer({data:new Float32Array([0,0,0,0,16,0,1,0,0,16,0,1]),usage:V.BufferUsage.VERTEX|V.BufferUsage.COPY_DST})
  const geometry=new V.Geometry({attributes:{aPosition:{buffer,format:'float32x2',stride:16,offset:0},aUV:{buffer,format:'float32x2',stride:16,offset:8}}})
  const mesh=new V.Mesh({geometry,texture:V.Texture.WHITE})
  assert.equal(mesh.geometry,geometry)
  assert.deepEqual(packed(mesh.__nativeMeshGeometry),[0,0,16,0,0,16])
  assert.deepEqual(Array.from(mesh.__nativeMeshGeometry.getBuffer('aTextureCoord').data),[0,0,1,0,0,1])
  assert.deepEqual(Array.from(mesh.__nativeMeshGeometry.getIndex().data),[0,1,2])
  buffer.data[4]=28;buffer.update(4,16)
  assert.equal(packed(mesh.__nativeMeshGeometry)[2],28)
  geometry.addIndex(new Uint32Array([0,2,1]))
  assert.deepEqual(Array.from(mesh.__nativeMeshGeometry.getIndex().data),[0,2,1])
  geometry.getIndex().data[1]=1;geometry.getIndex().update()
  assert.deepEqual(Array.from(mesh.__nativeMeshGeometry.getIndex().data),[0,1,1])
  const before=buffer.listenerCount('update');mesh.destroy()
  assert.ok(buffer.listenerCount('update')<before)
  assert.ok(geometry.attributes,'destroying a view must not destroy shared geometry')
  geometry.destroy(true)
})
test('native PerspectiveMesh matches official subdivided projective geometry, including texture resize',()=>{
  const source=new V.TextureSource({width:64,height:32}),texture=new V.Texture({source})
  const n=new V.PerspectiveMesh({texture,verticesX:4,verticesY:3}),o=new O.PerspectivePlaneGeometry({width:64,height:32,verticesX:4,verticesY:3})
  const corners=[3,7,61,1,49,30,10,26];n.setCorners(...corners);o.setCorners(...corners);same(n.geometry,o)
  assert.equal(packed(n.__nativeMeshGeometry).length,24)
  source.resize(128,48);n.textureUpdated();o.width=128;o.height=48;o.updateProjection();same(n.geometry,o)
  n.destroy({texture:true,textureSource:true,geometry:true});o.destroy(true)
})
test('native PlaneGeometry rebuild and RopeGeometry updates use official CPU geometry and release subscriptions',()=>{
  const plane=new V.PlaneGeometry({width:12,height:16,verticesX:3,verticesY:2}),m=new V.Mesh({geometry:plane})
  plane.width=24;plane.height=32;plane.build({})
  assert.equal(Math.max(...packed(m.__nativeMeshGeometry)),32)
  const points=[new V.Point(0,0),new V.Point(10,4),new V.Point(20,0)]
  const rope=new V.RopeGeometry({width:6,points,textureScale:.5}),o=new O.RopeGeometry({width:6,points,textureScale:.5}),r=new V.Mesh({geometry:rope})
  points[1].y=12;rope.update();o.update();same(rope,o)
  assert.deepEqual(packed(r.__nativeMeshGeometry),Array.from(o.positions))
  m.geometry=rope;plane.destroy(true);assert.equal(m.geometry,rope)
  rope.destroy();assert.equal(m.geometry,null);assert.equal(r.geometry,null)
  m.destroy();r.destroy();o.destroy(true)
})
test('generic Geometry rejects instancing and non-2D attribute layouts instead of silently changing them',()=>{
  const count=R.objects.filter(o=>o&&!o.destroyed).length
  for(const options of [{instanceCount:2},{attributes:{aPosition:{buffer:new Float32Array(9),format:'float32x3'},aUV:new Float32Array(6)}}]){
    const g=new V.Geometry({attributes:{aPosition:{buffer:new Float32Array(6),format:'float32x2'},aUV:{buffer:new Float32Array(6),format:'float32x2'}},...options})
    assert.throws(()=>new V.Mesh({geometry:g}),V.UnsupportedNativeAPIError);g.destroy(true)
  }
  assert.equal(R.objects.filter(o=>o&&!o.destroyed).length,count,'rejection leaked a native scene handle')
})
