import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import {createNativeHost} from './helpers/native-host.mjs';
function host(){
 const events=[],canvas={_lst:{}},c=vm.createContext({performance:{now:()=>123},__PIXI_APP__:{view:canvas},__log:console.log});
 for(const f of ['06a-web-platform.js','07-render-tree-enum.js'])vm.runInContext(fs.readFileSync(new URL('../src/js/game_host/'+f,import.meta.url),'utf8'),c);
 const code=fs.readFileSync(new URL('../src/js/game_host/14-clipboard.js',import.meta.url),'utf8'),start=code.indexOf('var __activeNativePointers'),end=code.indexOf('// AppKit delivers trackpad',start);vm.runInContext(code.slice(start,end),c);
 for(const type of ['pointerdown','pointermove','pointerup','pointercancel'])c.addEventListener(type,e=>events.push({type:e.type,id:e.pointerId,pointerType:e.pointerType,primary:e.isPrimary,pressure:e.pressure,x:e.clientX}));
 return {c,events};
}
test('two concurrent touches retain separate identities and releasing one preserves the other',()=>{
 const {c,events}=host();c.__pointerEvent('pointerdown',10,20,0,1,2,'touch',true);c.__pointerEvent('pointerdown',30,40,0,1,7,'touch',false);
 c.__pointerEvent('pointerup',10,20,0,0,2,'touch',true);c.__pointerEvent('pointermove',50,40,0,1,7,'touch',false,.8);c.__nativeCancelPointers();
 assert.deepEqual(events.map(e=>[e.type,e.id,e.primary]),[['pointerdown',2,true],['pointerdown',7,false],['pointerup',2,true],['pointermove',7,false],['pointercancel',7,false]]);
 assert.equal(events[3].pressure,.8);assert.equal(events[4].pressure,0);assert.equal(events[4].pointerType,'touch');c.__nativeCancelPointers();assert.equal(events.length,5);
});
test('legacy desktop pointer calls retain mouse semantics',()=>{const {c,events}=host();c.__pointerEvent('pointerdown',10,20,0,1);assert.deepEqual(events[0],{type:'pointerdown',id:1,pointerType:'mouse',primary:true,pressure:.5,x:10});});
test('native cancellation reaches real federated capture/bubble listeners and clears press tracking',()=>{
 const {c}=host(),{P,c:pixiContext}=createNativeHost();pixiContext.performance={now:()=>123};const root=new P.Container(),child=new P.Container();root.eventMode=child.eventMode='static';root.addChild(child);
 const boundary=new P.EventBoundary(root),seen=[],from=new P.FederatedPointerEvent();from.type='pointercancel';from.pointerId=7;from.pointerType='touch';from.button=0;
 root.on('pointercancelcapture',e=>seen.push('capture:'+e.pointerId));child.on('pointercancel',e=>seen.push('target:'+e.pointerId));root.on('pointercancel',()=>seen.push('bubble'));child.on('touchcancel',()=>seen.push('touchcancel'));
 boundary.trackingData(7).pressTargetsByButton[0]=[root,child];
 c.__PIXI_APP__.renderer={events:{rootBoundary:boundary,rootPointerEvent:from,bootstrapEvent:()=>from}};
 c.__pointerEvent('pointercancel',4,4,0,0,7,'touch',false);
 assert.deepEqual(seen,['capture:7','target:7','bubble','touchcancel']);assert.equal(boundary.mappingState.trackingData[7],undefined);root.destroy({children:true});
});
test('background visibility cancels active touches, dispatches once and restores state',()=>{
 const {c,events}=host(),visibility=[],vibrations=[];c.document.hidden=false;c.__nativeVibrate=p=>vibrations.push(p);c.document.addEventListener('visibilitychange',()=>visibility.push(c.document.visibilityState));
 c.__pointerEvent('pointerdown',10,20,0,1,2,'touch',true);c.__pointerEvent('pointerdown',30,40,0,1,3,'touch',false);
 c.__nativeSetVisibility(false);c.__nativeSetVisibility(false);assert.equal(c.document.hidden,true);assert.deepEqual(events.slice(-2).map(e=>[e.type,e.id]),[['pointercancel',2],['pointercancel',3]]);assert.deepEqual(vibrations,['[]']);
 c.__nativeSetVisibility(true);assert.equal(c.document.hidden,false);assert.deepEqual(visibility,['hidden','visible']);
});
