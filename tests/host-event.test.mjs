import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

function host(){
  const c=vm.createContext({performance:{now:()=>123}});
  vm.runInContext(fs.readFileSync(new URL('../src/js/game_host/06a-web-platform.js',import.meta.url),'utf8'),c);
  vm.runInContext(fs.readFileSync(new URL('../src/js/game_host/07-render-tree-enum.js',import.meta.url),'utf8'),c);
  return c;
}

test('constructed Event synchronously reaches every registered window listener',()=>{
  const c=host(),received=[];
  const first=e=>received.push([e.type,e.target===e.currentTarget,e.eventPhase]);
  c.addEventListener('preference-changed',first);
  c.addEventListener('preference-changed',e=>received.push([e.type,e.target===e.currentTarget,e.eventPhase]));
  const e=new c.Event('preference-changed');
  assert.equal(c.dispatchEvent(e),true);
  assert.deepEqual(received,[['preference-changed',true,2],['preference-changed',true,2]]);
  assert.equal(e.currentTarget,null);assert.equal(e.eventPhase,0);assert.equal(e.timeStamp,123);
  c.removeEventListener('preference-changed',first);received.length=0;c.dispatchEvent(e);assert.equal(received.length,1);
});

test('Event cancellation and immediate propagation affect synchronous dispatch',()=>{
  const c=host();let later=0;
  c.addEventListener('cancel',e=>{e.preventDefault();e.stopImmediatePropagation();});
  c.addEventListener('cancel',()=>later++);
  const plain=new c.Event('cancel');assert.equal(c.dispatchEvent(plain),true);assert.equal(plain.defaultPrevented,false);
  const cancelable=new c.Event('cancel',{cancelable:true});assert.equal(c.dispatchEvent(cancelable),false);
  assert.equal(cancelable.defaultPrevented,true);assert.equal(later,0);
});

test('CustomEvent retains detail and inherits Event behavior',()=>{
  const c=host(),detail={value:42};
  const e=new c.CustomEvent('bridge-reply',{detail,cancelable:true});
  assert.ok(e instanceof c.Event);assert.equal(e.detail,detail);
  c.addEventListener('bridge-reply',event=>event.preventDefault());
  assert.equal(c.dispatchEvent(e),false);
  assert.equal(new c.CustomEvent('empty').detail,null);
});
