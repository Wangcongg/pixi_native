import test from 'node:test';
import assert from 'node:assert/strict';
import {createNativeHost} from './helpers/native-host.mjs';

function near(a,b){assert.ok(Math.abs(a-b)<0.0001,`${a} != ${b}`);}
function xy(a,b){near(a.x,b.x);near(a.y,b.y);}

test('native global positions preserve Pixi pivot, parent and rotation semantics',()=>{
  const {P,original}=createNativeHost(),np=new P.Container(),nc=new P.Container(),op=new original.Container(),oc=new original.Container();
  np.addChild(nc);op.addChild(oc);
  for(const [a,b] of [[np,op],[nc,oc]]){a.position.set(25,30);b.position.set(25,30);a.scale.set(1.5,2);b.scale.set(1.5,2);a.rotation=b.rotation=.4;a.pivot.set(8,11);b.pivot.set(8,11);}
  xy(nc.getGlobalPosition(),oc.getGlobalPosition());xy(np.getGlobalPosition(),op.getGlobalPosition());
  xy(nc.toGlobal(new P.Point(3,7)),oc.toGlobal(new P.Point(3,7)));
  xy(nc.toLocal(new P.Point(100,80)),oc.toLocal(new P.Point(100,80)));
});

test('matrix decomposition writes stable native transform points and supports world snapshots',()=>{
  const {P,original}=createNativeHost(),n=new P.Container(),t=new P.Transform();
  const points=[n.position,n.scale,n.skew,n.pivot],m=new P.Matrix().setTransform(31,43,0,0,2,3,.3,.1,.2);
  n.transform.setFromMatrix(m);t.setFromMatrix(m);n.transform.updateLocalTransform();t.updateLocalTransform();
  for(const k of ['a','b','c','d','tx','ty'])near(n.localTransform[k],t.localTransform[k]);
  assert.deepEqual([n.position,n.scale,n.skew,n.pivot],points);n.toGlobal(new P.Point());
  const snapshot=n.worldTransform.clone();assert.ok(snapshot instanceof original.Matrix);
  assert.deepEqual(Array.from(n.worldTransform.toArray(true)),Array.from(snapshot.toArray(true)));
  const copy=new P.Matrix();assert.equal(n.worldTransform.copyTo(copy),copy);n.position.set(999,999);n.toGlobal(new P.Point());near(copy.tx,31);
});

test('native events match Pixi capture, once, context and symbol events',()=>{
  const {P}=createNativeHost(),n=new P.Container();let calls=0;const listener={handleEvent(){assert.equal(this,listener);calls++;}};
  n.addEventListener('pointerdown',listener,{capture:true,once:true});n.emit('pointerdown',{});assert.equal(calls,0);
  n.emit('pointerdowncapture',{});n.emit('pointerdowncapture',{});assert.equal(calls,1);
  n.addEventListener('pointerdown',listener,true);n.removeEventListener('pointerdown',listener,true);assert.equal(n.listenerCount('pointerdowncapture'),0);
  const symbol=Symbol('scene');n.on(symbol,()=>{});assert.ok(n.eventNames().includes(symbol));
  const fn=function(){calls++;},a={},b={};n.once('tick',fn,a);n.once('tick',fn,b);n.emit('tick');assert.equal(calls,3);assert.equal(n.listenerCount('tick'),0);
});

test('deep child lookup checks direct children first and prevents cyclic reparent without mutation',()=>{
  const {P}=createNativeHost(),root=new P.Container(),branch=new P.Container(),direct=new P.Container(),deep=new P.Container();
  direct.name=deep.name='match';root.addChild(branch,direct);branch.addChild(deep);
  assert.equal(root.getChildByName('match',true),direct);
  assert.throws(()=>deep.addChild(root),/ancestor/);assert.equal(branch.parent,root);assert.equal(deep.parent,branch);assert.equal(root.parent,null);
});

test('destroy emits once, releases mask ownership and detaches descendants',()=>{
  const {P}=createNativeHost(),root=new P.Container(),child=new P.Container(),mask=new P.Container();root.addChild(child);root.mask=mask;
  assert.equal(mask.renderable,false);let count=0;root.on('destroyed',()=>{assert.equal(root.destroyed,true);count++;});root.destroy();root.destroy();
  assert.equal(count,1);assert.equal(child.parent,null);assert.equal(child.destroyed,false);assert.equal(mask.renderable,true);assert.equal(mask._maskRefCount,0);
});

const cases={
  'rounded strokes, caps and joins':g=>g.lineStyle({width:9,color:0xff8800,cap:'round',join:'round',alignment:1}).moveTo(10,10).lineTo(60,10).lineTo(20,70),
  'quadratic and cubic curves':g=>g.lineStyle(5,0xffffff).moveTo(0,0).quadraticCurveTo(30,100,90,0).bezierCurveTo(120,90,150,-80,180,40),
  'arcs and tangent arc':g=>g.beginFill(0xabcdef).moveTo(20,30).arc(50,50,30,-1,3.4).arcTo(100,20,140,70,12).closePath().endFill(),
  'holes and transformed shapes':g=>g.beginFill(0x8844ff).drawRect(0,0,100,100).beginHole().drawCircle(50,50,20).endHole().setMatrix(new g.localTransform.constructor().translate(120,20)).drawRoundedRect(0,0,80,40,12).endFill(),
  'texture fills':g=>g.beginTextureFill({texture:g.constructor.__testTexture,color:0x99ffbb}).drawRect(-5,9,40,25).endFill()
};
for(const [name,draw] of Object.entries(cases))test(`native Graphics matches Pixi tessellation: ${name}`,()=>{
  const {P,original}=createNativeHost();P.Graphics.__testTexture=original.Graphics.__testTexture=P.Texture.WHITE;
  const n=new P.Graphics(),o=new original.Graphics();draw(n);draw(o);n.finishPoly();o.finishPoly();n.geometry.updateBatches();o.geometry.updateBatches();
  for(const key of ['points','indices','uvs'])assert.deepEqual(Array.from(n.geometry[key]),Array.from(o.geometry[key]),key);
  const styles=g=>g.geometry.batches.map(b=>[b.start,b.size,b.style.color,b.style.alpha,b.style.width,b.style.cap,b.style.join]);assert.deepEqual(styles(n),styles(o));
  assert.ok(n.__nativeNodeProxy);assert.ok(n.geometry.points.length>0);
});

test('Graphics clone shares the Pixi geometry lifetime and clear permits redraw',()=>{
  const {P}=createNativeHost(),a=new P.Graphics().beginFill(0xff00ff).drawCircle(0,0,20).endFill(),b=a.clone(),geometry=a.geometry;
  assert.equal(b.geometry,geometry);assert.equal(geometry.refCount,2);a.destroy();assert.equal(geometry.refCount,1);
  b.clear().beginFill(0xffffff).drawRect(0,0,10,20).endFill();b.geometry.updateBatches();assert.ok(b.geometry.points.length>0);b.destroy();assert.equal(geometry.refCount,0);
});

test('SimpleMesh autoUpdate follows each render and stops after destroy',()=>{
  const {P,c,calls}=createNativeHost();P.settings.ADAPTER={...P.settings.ADAPTER,createCanvas:()=>({getContext:()=>null})};const root=new P.Container(),mesh=new P.SimpleMesh(P.Texture.WHITE,new Float32Array([0,0,10,0,0,10]),new Float32Array([0,0,1,0,0,1]),new Uint16Array([0,1,2]));root.addChild(mesh);
  const buffer=mesh.verticesBuffer,id=buffer._updateID;c.__nativePrepareMeshRender(root);assert.equal(buffer._updateID,id+1);mesh.vertices[0]=5;c.__nativePrepareMeshRender(root);assert.equal(buffer._updateID,id+2);
  mesh.autoUpdate=false;c.__nativePrepareMeshRender(root);assert.equal(buffer._updateID,id+2);buffer.update();assert.equal(buffer._updateID,id+3);mesh.destroy();const n=calls.length;c.__nativePrepareMeshRender(root);assert.equal(calls.length,n);
});
test('native auto meshes skip hidden ancestors and resume updates when shown',()=>{
 const {P,c}=createNativeHost();P.settings.ADAPTER={...P.settings.ADAPTER,createCanvas:()=>({getContext:()=>null})};const root=new P.Container(),parent=new P.Container(),mesh=new P.SimpleMesh(P.Texture.WHITE);root.addChild(parent);parent.addChild(mesh);const buffer=mesh.verticesBuffer,id=buffer._updateID;
 for(const [owner,key,value]of [[parent,'visible',false],[mesh,'renderable',false],[parent,'alpha',0]]){owner[key]=value;for(let i=0;i<100;i++)c.__nativePrepareMeshRender(root);assert.equal(buffer._updateID,id);owner[key]=key==='alpha'?1:true;}
 c.__nativePrepareMeshRender(root);assert.equal(buffer._updateID,id+1);root.destroy({children:true});
});
test('native variable-duration animations match original Pixi across large and reverse ticks',()=>{
 const {P,original}=createNativeHost(),frames=[{texture:P.Texture.WHITE,time:3},{texture:P.Texture.WHITE,time:17},{texture:P.Texture.WHITE,time:23}],n=new P.AnimatedSprite(frames,false),o=new original.AnimatedSprite(frames,false);n.play();o.play();
 for(const speed of [1,-1,.2]){n.animationSpeed=o.animationSpeed=speed;for(const delta of [1,4,2000,3]){n.update(delta);o.update(delta);assert.equal(n.currentFrame,o.currentFrame);near(n._currentTime,o._currentTime);}}n.destroy();o.destroy();
});
test('native SimplePlane autoResize follows texture changes on the render path',()=>{
 const {P,c}=createNativeHost();P.settings.ADAPTER={...P.settings.ADAPTER,createCanvas:()=>({getContext:()=>null})};const root=new P.Container(),source=new P.BaseTexture(new P.BufferResource(new Uint8Array(16),{width:2,height:2})),texture=new P.Texture(source),plane=new P.SimplePlane(texture,2,2);root.addChild(plane);source.setSize(8,12);c.__nativePrepareMeshRender(root);assert.equal(plane.geometry.width,8);assert.equal(plane.geometry.height,12);plane.autoResize=false;source.setSize(20,30);c.__nativePrepareMeshRender(root);assert.equal(plane.geometry.width,8);root.destroy({children:true});texture.destroy(true);
});
