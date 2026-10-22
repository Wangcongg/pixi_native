import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
function host(){const c=vm.createContext({performance,setTimeout,clearTimeout});vm.runInContext(fs.readFileSync(new URL('../src/js/game_host/06a-web-platform.js',import.meta.url),'utf8'),c);return c;}

test('EventTarget deduplicates callbacks, handles objects, once, capture and removal during dispatch',()=>{
  const c=host(),target=new c.EventTarget(),seen=[];const object={handleEvent(e){seen.push(this===object);}};
  const second=()=>seen.push('removed');target.addEventListener('test',()=>target.removeEventListener('test',second));target.addEventListener('test',second);
  target.addEventListener('test',object,{once:true});target.addEventListener('test',object,{once:true});
  target.dispatchEvent(new c.Event('test'));target.dispatchEvent(new c.Event('test'));assert.deepEqual(seen,[true]);
  let calls=0;const fn=()=>calls++;target.addEventListener('capture',fn);target.addEventListener('capture',fn,true);target.removeEventListener('capture',fn,true);target.dispatchEvent(new c.Event('capture'));assert.equal(calls,1);
});
test('AbortController removes listeners, preserves reason and rejects already aborted subscriptions',()=>{
  const c=host(),target=new c.EventTarget(),controller=new c.AbortController(),reason=new Error('cancelled');let calls=0;
  target.addEventListener('test',()=>calls++,{signal:controller.signal});controller.abort(reason);controller.abort(new Error('other'));
  assert.equal(controller.signal.reason,reason);assert.throws(()=>controller.signal.throwIfAborted(),e=>e===reason);
  target.addEventListener('test',()=>calls++,{signal:controller.signal});target.dispatchEvent(new c.Event('test'));assert.equal(calls,0);
  assert.equal(c.AbortSignal.abort().reason.name,'AbortError');
});
test('passive listeners do not cancel events, nested redispatch is rejected and target cleanup is stable',()=>{
  const c=host(),target=new c.EventTarget(),e=new c.Event('test',{cancelable:true});
  target.addEventListener('test',event=>{event.preventDefault();assert.throws(()=>target.dispatchEvent(event),{name:'InvalidStateError'});},{passive:true});
  assert.equal(target.dispatchEvent(e),true);assert.equal(e.currentTarget,null);assert.equal(e.eventPhase,0);
});
test('native document and window dispatch remain distinct while native input shares their registry',()=>{
  const c=host();vm.runInContext(fs.readFileSync(new URL('../src/js/game_host/07-render-tree-enum.js',import.meta.url),'utf8'),c);let doc=0,win=0;
  c.document.addEventListener('scene',()=>doc++);c.addEventListener('scene',()=>win++);c.dispatchEvent(new c.Event('scene'));assert.deepEqual([doc,win],[0,1]);c.document.dispatchEvent(new c.Event('scene'));assert.deepEqual([doc,win],[1,1]);
  c.addEventListener('pointerup',()=>win++,{once:true});c._docLst.pointerup[0]({type:'pointerup'});assert.equal(c._docLst.pointerup.length,0);assert.equal(win,2);
});

for(const text of ['中文 English 😀','\ud800X\udc00','\ufeff你好','', 'a'.repeat(20000)])test('UTF-8 encoder matches standard for '+JSON.stringify(text.slice(0,20)),()=>{
  const c=host();assert.deepEqual(Array.from(new c.TextEncoder().encode(text)),Array.from(new TextEncoder().encode(text)));
});
test('UTF-8 encodeInto never writes partial code points and reports UTF-16 read units',()=>{
  const c=host();for(let n=0;n<12;n++){const actual=vm.runInContext(`new Uint8Array(${n})`,c),expected=new Uint8Array(n);const a=new c.TextEncoder().encodeInto('你😀ab',actual),b=new TextEncoder().encodeInto('你😀ab',expected);assert.deepEqual({...a},b);assert.deepEqual(Array.from(actual),Array.from(expected));}
});
test('UTF-8 decoder matches standard malformed boundaries and ArrayBuffer view offsets',()=>{
  const c=host();const vectors=[[0xff],[0xc0,0x80],[0xed,0xa0,0x80],[0xe2,0x28,0xa1],[0xf4,0x90,0x80,0x80],[0xe2,0x82],[0xf0,0x9f,0x98,0x80],[0xef,0xbb,0xbf,65]];
  for(const data of vectors){const wrapped=Uint8Array.from([99,...data,99]),view=wrapped.subarray(1,-1);assert.equal(new c.TextDecoder().decode(view),new TextDecoder().decode(view));}
  assert.throws(()=>new c.TextDecoder('utf-8',{fatal:true}).decode(Uint8Array.of(0xff)),{name:'TypeError'});assert.throws(()=>new c.TextDecoder('latin-1'),{name:'RangeError'});
});
test('UTF-8 decoder streaming retains incomplete code points and BOM state',()=>{
  const c=host(),bytes=new TextEncoder().encode('\ufeff你😀a');for(let split=0;split<=bytes.length;split++){const a=new c.TextDecoder(),b=new TextDecoder();assert.equal(a.decode(bytes.subarray(0,split),{stream:true})+a.decode(bytes.subarray(split)),b.decode(bytes.subarray(0,split),{stream:true})+b.decode(bytes.subarray(split)));}
});


test('native vibration forwards bounded on/off patterns, cancellation and real status',()=>{
  const calls=[],c=vm.createContext({navigator:{},document:{hidden:false},__nativeVibrate:pattern=>{calls.push(JSON.parse(pattern));return true;}});
  vm.runInContext(fs.readFileSync(new URL('../src/js/game_host/06a-web-platform.js',import.meta.url),'utf8'),c);
  assert.equal(c.navigator.vibrate(40),true);assert.deepEqual(calls.pop(),[40]);
  c.navigator.vibrate([20,30,40,50]);assert.deepEqual(calls.pop(),[20,30,40]);
  c.navigator.vibrate(0);assert.deepEqual(calls.pop(),[]);
  c.navigator.vibrate([]);assert.deepEqual(calls.pop(),[]);
  c.navigator.vibrate(12000);assert.deepEqual(calls.pop(),[10000]);
  c.document.hidden=true;assert.equal(c.navigator.vibrate(20),false);assert.equal(calls.length,0);
  c.navigator.vibrate(0);assert.deepEqual(calls.pop(),[]);
  c.document.hidden=false;c.__nativeVibrate=()=>false;assert.equal(c.navigator.vibrate(20),false);
  const unsupported=vm.createContext({navigator:{}});vm.runInContext(fs.readFileSync(new URL('../src/js/game_host/06a-web-platform.js',import.meta.url),'utf8'),unsupported);assert.equal(unsupported.navigator.vibrate,undefined);
});
