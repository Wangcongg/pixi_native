import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

function host(beforeHook){
  const calls=[];
  const c=vm.createContext({setTimeout,clearTimeout,performance,
    __imgTk:0,__imgPending:{},__blobReg:{},__IO_PACE:0,
    __nativeSupportsImageSourcePMA:true});
  vm.runInContext(fs.readFileSync(new URL('../vendor/pixi.v7.4.3.js',import.meta.url),'utf8'),c);
  vm.runInContext(fs.readFileSync(new URL('../src/js/game_host/08-texture-ownership.js',import.meta.url),'utf8'),c);
  c.__loadImageAsync=(url,token,pma)=>calls.push({url,token,pma});
  if(beforeHook)beforeHook(c.PIXI.loadTextures);
  const source=fs.readFileSync(new URL('../src/js/native_scenegraph/03-render-sync.js',import.meta.url),'utf8');
  vm.runInContext(source.slice(source.indexOf('    // Pixi\'s PMA metadata'),source.indexOf('    var __nativeNeedsPremultiplyOf')),c);
  return {c,calls};
}

test('real Pixi image loading carries source PMA per request through concurrent decoding and reload',async()=>{
  const {c,calls}=host(),P=c.PIXI,loader={promiseCache:{}};
  const a=P.loadTextures.load('pma.png',{data:{alphaMode:P.ALPHA_MODES.PMA}},loader);
  const b=P.loadTextures.load('straight.png',{data:{alphaMode:P.ALPHA_MODES.UNPACK}},loader);
  assert.deepEqual(calls.map(x=>x.pma),[true,false]);
  assert.equal(c.__nativeImageSourcePremultiplied,undefined);
  // Resolve out of order, after the loader's scoped option has been restored.
  c.__imgDone(calls[1].token,2,2,null,102,false);
  c.__imgDone(calls[0].token,2,2,null,101,false);
  const [pma,straight]=await Promise.all([a,b]);
  assert.equal(pma.baseTexture.alphaMode,P.ALPHA_MODES.PMA);
  assert.equal(straight.baseTexture.alphaMode,P.ALPHA_MODES.UNPACK);
  pma.baseTexture.resource.source.src='pma.png';
  straight.baseTexture.resource.source.src='straight.png';
  assert.deepEqual(calls.slice(2).map(x=>x.pma),[true,false]);
});

test('PMA decode option is restored even when a texture parser throws synchronously',()=>{
  const {c}=host(parser=>{parser.load=()=>{throw new Error('decode rejected');};});
  c.__nativeImageSourcePremultiplied=true;
  assert.throws(()=>c.PIXI.loadTextures.load('broken.png',{data:{}},{}),/decode rejected/);
  assert.equal(c.__nativeImageSourcePremultiplied,true);
});

test('blob images pass the captured PMA option to native compressed-byte decoding',()=>{
  const {c}=host(),calls=[];
  c.__blobReg['blob:pn/1']=new Uint8Array([1,2,3]);
  c.__decodeImageAsync=(token,bytes,pma)=>{calls.push(pma);return true;};
  c.__nativeImageSourcePremultiplied=true;
  const pma=new c.Image();
  c.__nativeImageSourcePremultiplied=false;
  pma.src='blob:pn/1';
  assert.deepEqual(calls,[true]);
});
