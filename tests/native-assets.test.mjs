import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

function host(page="probe.png"){
  const reads=[],c=vm.createContext({console,setTimeout,clearTimeout});
  const run=file=>vm.runInContext(fs.readFileSync(new URL('../'+file,import.meta.url),'utf8'),c,{filename:file});
  run('vendor/pixi.v7.4.3.js');const P=c.PIXI;
  const white=new P.Texture(new P.BaseTexture(new P.BufferResource(new Uint8Array(16),{width:2,height:2})));
  P.loadTextures.load=async function(url){reads.push(url);const texture=new P.Texture(new P.BaseTexture(new P.BufferResource(new Uint8Array(16),{width:2,height:2})));texture.textureCacheIds.push(url);texture.baseTexture.resource.src=url;return texture;};
  // Stock .fnt text parser and BitmapFont.install remain real; only native byte I/O is doubled.
  P.settings.ADAPTER={...P.settings.ADAPTER,getBaseUrl:()=>'',getFontFaceSet:()=>null,fetch:async url=>{reads.push(url);return {text:async()=>font};}};
  const font='info face="ProbeFont" size=12\ncommon lineHeight=12 base=10 scaleW=2 scaleH=2 pages=1 packed=0\npage id=0 file="'+page+'"\nchars count=1\nchar id=65 x=0 y=0 width=2 height=2 xoffset=0 yoffset=0 xadvance=3 page=0 chnl=15';
  run('src/js/host/native_bitmap_font_asset.js');run('src/js/host/native_assets.js');
  return {P,c,reads};
}
test('bitmap font unload outside Assets cannot leave a stale successful loader promise',async()=>{
  const {P,reads}=host();const first=await P.Assets.loader.load('probe.fnt');assert.equal(first.font,'ProbeFont');const texture=first.pageTextures[0];
  P.BitmapFont.uninstall('ProbeFont');assert.equal(texture.destroyed,true);
  const second=await P.Assets.loader.load('probe.fnt');assert.notEqual(second,first);assert.ok(P.BitmapFont.available.ProbeFont);assert.equal(second.pageTextures[0].destroyed,false);
  assert.equal(reads.filter(x=>x.endsWith('.fnt')).length,2);assert.equal(reads.filter(x=>x.endsWith('.png')).length,2);
});
test('nonmatching authored page names are reloaded after font destruction',async()=>{
  const {P,reads}=host('atlas-page-01.png');await P.Assets.loader.load('probe.fnt');P.BitmapFont.uninstall('ProbeFont');await P.Assets.loader.load('probe.fnt');assert.equal(reads.filter(x=>x.endsWith('atlas-page-01.png')).length,2);
});

test('valid bitmap font caches are reused and page URLs stay authored',async()=>{
  const {P,reads}=host();const first=await P.Assets.loader.load('probe.fnt');assert.equal(await P.Assets.loader.load('probe.fnt'),first);assert.deepEqual(reads,['probe.fnt','probe.png']);
});
test('KTX2 parser accepts explicit texture URLs including query parameters and uses native image decoder',async()=>{
  const {P,reads}=host();const texture=await P.Assets.loader.load('page.ktx2?v=123');assert.ok(texture instanceof P.Texture);assert.deepEqual(reads,['page.ktx2?v=123']);
});

test('concurrent font loads share one parser promise, including after uninstall',async()=>{
  const {P,reads}=host();let fonts=await Promise.all([P.Assets.loader.load('probe.fnt'),P.Assets.loader.load('probe.fnt')]);assert.equal(fonts[0],fonts[1]);
  P.BitmapFont.uninstall('ProbeFont');fonts=await Promise.all([P.Assets.loader.load('probe.fnt'),P.Assets.loader.load('probe.fnt')]);assert.equal(fonts[0],fonts[1]);assert.equal(fonts[0].pageTextures[0].destroyed,false);
  assert.equal(reads.filter(x=>x.endsWith('.fnt')).length,2);
});
