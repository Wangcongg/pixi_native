import test from 'node:test';
import assert from 'node:assert/strict';
import {createNativeHost} from './helpers/native-host.mjs';
const {P,R}=createNativeHost();globalThis.PIXI=P;globalThis.__nativeSceneRuntime=R;
globalThis.navigator??={userAgent:'Native Kit lifetime regression'};
globalThis.requestAnimationFrame=()=>1;globalThis.cancelAnimationFrame=()=>{};
P.settings.ADAPTER={...P.settings.ADAPTER,createCanvas:()=>({getContext:()=>null})};
const N=await import('../src/compat/pixi8.mjs'),O=await import('../vendor/pixi.v8.20.0.mjs');
function texture(A=N){return new A.Texture({source:new A.BufferImageSource({resource:new Uint8Array(16),width:2,height:2})})}

test('AnimatedSprite alternate signature, autoPlay and variable-frame timing match official',()=>{
 const legacy=new N.AnimatedSprite([N.Texture.WHITE],false);assert.equal(legacy.autoUpdate,false);legacy.destroy();
 const a=new N.AnimatedSprite({textures:[N.Texture.WHITE],autoUpdate:false,autoPlay:true});assert.equal(a.playing,true);a.destroy();
 const make=A=>new A.AnimatedSprite({textures:[{texture:A.Texture.WHITE,time:3},{texture:A.Texture.WHITE,time:17},{texture:A.Texture.WHITE,time:23}],autoUpdate:false,autoPlay:true});
 const n=make(N),o=make(O);for(const speed of [1,-1,.2]){n.animationSpeed=o.animationSpeed=speed;for(const deltaTime of [1,4,2000,3]){n.update({deltaTime});o.update({deltaTime});assert.equal(n.currentFrame,o.currentFrame);assert.ok(Math.abs(n._currentTime-o._currentTime)<1e-6)}}n.destroy();o.destroy();
});
for(const kind of ['Sprite','TilingSprite','NineSliceSprite','Mesh','MeshSimple','MeshPlane','MeshRope','ParticleContainer','AnimatedSprite'])for(const options of [false,{texture:true,textureSource:false},{texture:true,textureSource:true},true])test(`${kind} preserves explicit texture ownership: ${JSON.stringify(options)}`,()=>{
 const t=texture(),source=t.source,other=new N.Sprite({texture:t});
 const args=kind==='AnimatedSprite'?{textures:[t,texture(),texture()],autoUpdate:false}:kind==='ParticleContainer'?{particles:[new N.Particle({texture:t})]}:{texture:t,points:[new N.Point(0,0),new N.Point(10,10)]};
 const n=new N[kind](args),frames=kind==='AnimatedSprite'?n.textures.slice():[t];
 n.destroy(options);n.destroy(options);
 const owned=options===true||options?.texture===true,sourceOwned=options===true||options?.textureSource===true;
 for(const f of frames)assert.equal(f.destroyed,owned);
 assert.equal(source.destroyed,sourceOwned);
 if(!owned)assert.equal(other.texture,t);
 other.destroy();for(const f of frames)if(!f.destroyed)f.destroy(true);if(!source.destroyed)source.destroy();
});
test('shared texture style replacement and destruction preserve other subscribers',()=>{
 const a=texture().source,b=texture().source,style=new N.TextureStyle();a.style=style;b.style=style;
 a.style=new N.TextureStyle();assert.equal(style.listenerCount('change'),1);
 a.style=style;let external=0;style.on('change',()=>external++);a.destroy();external=0;
 style.scaleMode='nearest';style.update();assert.equal(external,1);assert.equal(b._base.scaleMode,P.SCALE_MODES.NEAREST);
 b.destroy();assert.equal(style.listenerCount('change'),1);style.destroy();
});
test('texture-source events cover texture-owned destruction, unload and resize exactly once',()=>{
 const t=texture(),source=t.source,events=[];for(const e of ['unload','resize','destroy'])source.on(e,()=>events.push(e));
 assert.equal(source.resize(2,2),false);assert.equal(source.resize(4,6,2),true);assert.equal(source.pixelWidth,8);source.unload();t.destroy(true);source.destroy();
 assert.deepEqual(events,['resize','unload','unload','destroy']);
});
test('RenderTexture high-DPI resize updates dimensions, framebuffer and chain result',()=>{
 const rt=N.RenderTexture.create({width:8,height:8});assert.equal(rt.resize(20,30,2),rt);assert.equal(rt.source.resolution,2);assert.equal(rt.source.pixelWidth,40);assert.equal(rt.baseTexture.framebuffer.width,40);assert.equal(rt.baseTexture.framebuffer.height,60);rt.destroy(true);
});
test('RenderTexture source resize also resizes the actual framebuffer',()=>{
 const rt=N.RenderTexture.create({width:8,height:8});rt.source.resize(20,30,2);assert.equal(rt.baseTexture.framebuffer.width,40);assert.equal(rt.baseTexture.framebuffer.height,60);rt.destroy(true);
});
test('RenderTexture constructor and textureOptions preserve Pixi 8 metadata and source identity',()=>{
 const source=new N.TextureSource({width:8,height:12,resolution:2}),a=new N.RenderTexture({source,dynamic:true,label:'direct'});
 assert.equal(a.source,source);assert.equal(a.label,'direct');assert.equal(a.dynamic,true);assert.equal(a.frame.width,8);assert.equal(a.framebuffer.width,16);
 const b=N.RenderTexture.create({width:6,height:7,dynamic:true,textureOptions:{label:'factory',defaultAnchor:{x:.3,y:.6}}});
 assert.equal(b.label,'factory');assert.equal(b.dynamic,true);assert.equal(b.defaultAnchor.x,.3);assert.equal(b.defaultAnchor.y,.6);a.destroy(true);b.destroy(true);
});
test('empty texture owns its default source and never destroys the shared default',()=>{
 const a=new N.Texture(),b=new N.Texture();assert.notEqual(a.source,b.source);assert.notEqual(a.source,N.Texture.EMPTY.source);a.destroy(true);assert.equal(b.source.destroyed,false);assert.equal(N.Texture.EMPTY.source.destroyed,false);b.destroy(true);
});
test('texture source replacement removes old subscriptions and emits destroy after owned source',()=>{
 const t=texture(),old=t.source,next=new N.TextureSource({width:5,height:6}),events=[];t.source=next;assert.equal(t.width,5);assert.equal(t.height,6);
 old.resize(10,12);assert.equal(t.width,5);next.resize(7,8);assert.equal(t.width,7);
 next.on('destroy',()=>events.push('source'));t.on('destroy',()=>events.push('texture'));t.destroy(true);t.destroy(true);assert.deepEqual(events,['source','texture']);old.destroy();
});
test('TextureSource sampler aliases update all shared samplers and emit styleChange',()=>{
 const source=texture().source;let updates=0;source.on('styleChange',()=>updates++);source.addressMode='repeat';source.magFilter='nearest';source.minFilter='linear';source.style.update();
 assert.equal(source.source,source);assert.equal(source.style.addressModeU,'repeat');assert.equal(source._base.__nativeV8Sampler.mag,'nearest');assert.equal(source._base.__nativeV8Sampler.min,'linear');assert.equal(updates,1);source.destroy();
});
test('converted Graphics textures release their native image when a gradient is destroyed',()=>{
 const gradient=new N.FillGradient(),g=new N.Graphics().rect(0,0,10,10).fill(gradient);g.geometry.updateBatches();const t=g.geometry.batches[0].style.texture,base=t.baseTexture;
 gradient.destroy();assert.equal(t.destroyed,true);assert.equal(base.destroyed,true);g.destroy();
});
test('converted official texture frame changes invalidate native UVs and sampler changes propagate',async()=>{
 const {nativeTexture}=await import('../src/compat/pixi8/texture.mjs'),source=new O.BufferImageSource({resource:new Uint8Array(4*4*4),width:4,height:4,format:'rgba8unorm'}),t=new O.Texture({source,frame:new O.Rectangle(0,0,2,2)}),native=nativeTexture(t);
 t.frame.x=1;t.update();assert.equal(native.frame.x,1);source.style.scaleMode='nearest';source.style.update();assert.equal(native.baseTexture.scaleMode,P.SCALE_MODES.NEAREST);t.destroy(true);assert.equal(native.destroyed,true);
});
test('converted official texture source replacement switches native storage and lifecycle',async()=>{
 const {nativeTexture}=await import('../src/compat/pixi8/texture.mjs'),source=()=>new O.BufferImageSource({resource:new Uint8Array(64),width:4,height:4,format:'rgba8unorm'}),a=source(),b=source(),t=new O.Texture({source:a}),native=nativeTexture(t),old=native.baseTexture;
 t.source=b;t.update();assert.notEqual(native.baseTexture,old);const current=native.baseTexture;a.destroy();assert.equal(old.destroyed,true);assert.equal(current.destroyed,false);t.destroy(true);assert.equal(current.destroyed,true);assert.equal(native.destroyed,true);
});
test('unsupported source and render-texture capabilities fail explicitly',()=>{
 for(const A of [N.TextureSource,N.BufferImageSource])for(const options of [{format:'rgba16float'},{sampleCount:4},{dimensions:'3d'},{arrayLayerCount:2},{antialias:true}])assert.throws(()=>new A(options),N.UnsupportedNativeAPIError);
 assert.throws(()=>N.RenderTexture.create({format:'rgba16float'}),N.UnsupportedNativeAPIError);
 for(const resource of [new Float32Array(16),new Uint16Array(16),new Uint32Array(16)])assert.throws(()=>new N.BufferImageSource({resource,width:2,height:2}),N.UnsupportedNativeAPIError);
 assert.throws(()=>new N.BufferImageSource({width:2,height:2}),N.UnsupportedNativeAPIError);
});
test('high-DPI buffer sources use pixel dimensions for their actual upload resource',()=>{
 const source=new N.BufferImageSource({resource:new Uint8Array(4*6*4),width:2,height:3,resolution:2});assert.equal(source.width,2);assert.equal(source.height,3);assert.equal(source.pixelWidth,4);assert.equal(source.pixelHeight,6);assert.equal(source._base.resource.width,4);assert.equal(source._base.resource.height,6);source.destroy();
});
test('DisplacementFilter accepts the Pixi 8 two-axis scale without producing NaN',()=>{
 const sprite=new N.Sprite({texture:N.Texture.WHITE}),f=new N.DisplacementFilter({sprite,scale:{x:8,y:13}});assert.equal(f.scale.x,8);assert.equal(f.scale.y,13);f.destroy();sprite.destroy();
});
test('renderer rejects unsupported generateTexture MSAA and forwards screenshot crop frames',async()=>{
 const {adaptRenderer}=await import('../src/compat/pixi8/application.mjs'),calls=[],frame=new N.Rectangle(1,2,3,4),renderer=adaptRenderer({render(){},resize(){},generateTexture(){throw new Error('must reject before generation')},extract:{image(...args){calls.push(args)},base64(...args){calls.push(args)}}});
 assert.throws(()=>renderer.generateTexture({target:new N.Container(),antialias:true}),N.UnsupportedNativeAPIError);const target={};renderer.extract.image({target,frame,format:'image/png'});renderer.extract.base64({target,frame,format:'image/png'});assert.equal(calls[0][3],frame);assert.equal(calls[1][3],frame);
});
test('particle prepare skips hidden, culled, transparent and detached branches and resumes',()=>{
 const root=new N.Container(),parent=new N.Container(),c=new N.ParticleContainer();root.addChild(parent);parent.addChild(c);let prepared=0;c.prepare=()=>prepared++;
 for(const [owner,key,value]of [[parent,'visible',false],[c,'renderable',false],[parent,'alpha',0],[c,'culled',true]]){owner[key]=value;for(let i=0;i<100;i++)globalThis.__nativePreparePixi8Render(root);assert.equal(prepared,0);owner[key]=key==='alpha'?1:key==='culled'?false:true;}
 c.removeFromParent();globalThis.__nativePreparePixi8Render(root);assert.equal(prepared,0);parent.addChild(c);globalThis.__nativePreparePixi8Render(root);assert.equal(prepared,1);root.destroy({children:true});
});
