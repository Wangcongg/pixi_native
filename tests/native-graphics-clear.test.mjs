import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

function host(){
  const c=vm.createContext({});
  vm.runInContext(fs.readFileSync(new URL('../vendor/pixi.v7.4.3.js',import.meta.url),'utf8'),c);
  const P=c.PIXI;
  const white=new P.Texture(new P.BaseTexture(new P.BufferResource(new Uint8Array([255,255,255,255]),{width:1,height:1})));
  Object.defineProperty(P.Texture,'WHITE',{value:white});
  // Exercise the real retained-Graphics writer with a native mesh boundary double.
  // A nonempty native payload survives TYPE=container until explicitly replaced.
  const meshes=new Map(),calls=[];
  Object.assign(c,{SGB:new Float32Array(48*8),ST:48,oQUADCNT:43,
    __sgTypeW:(b,type)=>{c.SGB[b+11]=type;},__qRelease(){},__markBoundsNode(){},
    __setNativeGeometryHit:(o,on)=>{o.__nativeHitGeometry=on;},
    __whiteTexId:()=>1,__tintOf:()=>0xffffff,__premulOf:()=>0,
    __setEsc:()=>{throw new Error('Graphics escaped');},
    __nativeGraphicsMesh:(id,points,indices,uvs,batches,blend,mask)=>{
      assert.ok(points.length>0&&indices.length>0&&uvs.length>0&&batches.length>0);
      const count=Array.from(batches).filter((_,i)=>i%7===2).reduce((a,b)=>a+b,0);
      meshes.set(id,count);calls.push({count,mask});return true;
    }});
  const source=fs.readFileSync(new URL('../src/js/native_scenegraph/03-render-sync.js',import.meta.url),'utf8');
  vm.runInContext(source.slice(source.indexOf('    var __emptyGfxPoints='),source.indexOf('    // M5-S1:')),c);
  return {c,meshes,calls};
}

for(const mask of [false,true])test(`Graphics clear releases retained ${mask?'stencil':'drawable'} geometry and permits redraw`,()=>{
  const {c,meshes,calls}=host(),g=new c.PIXI.Graphics();g.__i=1;
  g.beginFill(0x50bda3).drawCircle(20,30,10).endFill();c.__wrGfx(g,mask);
  assert.ok(meshes.get(1)>0);assert.equal(g.__sgNativeGraphicsPayload,true);
  g.clear();c.__wrGfx(g,mask);
  assert.equal(meshes.get(1),0);assert.equal(c.SGB[48+11],0);
  assert.equal(g.__nativeHitGeometry,false);assert.equal(calls.at(-1).mask,mask);
  const count=calls.length;c.__wrGfx(g,mask);assert.equal(calls.length,count);
  g.beginFill(0xff0000).drawRect(1,2,3,4).endFill();c.__wrGfx(g,mask);
  assert.ok(meshes.get(1)>0);
  // Removing geometry follows the same lifetime contract as Graphics.clear().
  Object.defineProperty(g,'geometry',{value:null});c.__wrGfx(g,mask);
  assert.equal(meshes.get(1),0);
});
