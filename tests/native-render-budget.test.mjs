import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import vm from 'node:vm'

function host(nodes,budget=0){
  const writes=[],c=vm.createContext({__qActiveRoot:null,__qTop:0,QCAP:1000,__qSweepTick:0,__renderBootstrapped:1,__sgObjs:[],__renderDirty:new Set(nodes),__renderDetachedDirty:new Set(),__qReclaimDetached(){},__log(){},__nativeRenderWorkBudget:1,__nativeRenderBudgetRemaining:budget,__wrType:o=>writes.push(o),__linkedToRoot(o,root){for(let p=o;p;p=p.parent)if(p===root)return true;return false}})
  const source=fs.readFileSync(new URL('../src/js/native_scenegraph/10-bind-tree-hooks.js',import.meta.url),'utf8')
  vm.runInContext(source.slice(source.indexOf('    var __flushRenderData='),source.indexOf('    // (5)')),c)
  return {c,writes}
}
test('RenderTexture completes its required subtree when screen work quota is exhausted',()=>{
  const stage={__i:1},snapshot={__i:2,parent:stage},unrelated={__i:3,parent:stage},children=Array.from({length:64},(_,i)=>({__i:4+i,parent:snapshot}))
  const {c,writes}=host([unrelated,snapshot,...children])
  c.__sgFlushRenderData(stage,snapshot)
  assert.equal(writes.length,65)
  assert.ok(writes.includes(snapshot)&&children.every(child=>writes.includes(child)))
  assert.deepEqual(Array.from(c.__renderDirty),[unrelated])
  assert.equal(c.__nativeRenderBudgetRemaining,0)
})
test('snapshot work preserves throttling and consumes quota only for unrelated screen work',()=>{
  const stage={__i:1},snapshot={__i:2,parent:stage},a={__i:3,parent:stage},b={__i:4,parent:stage},child={__i:5,parent:snapshot}
  const {c,writes}=host([a,b,snapshot,child],1)
  c.__sgFlushRenderData(stage,snapshot)
  assert.deepEqual(writes,[a,snapshot,child]);assert.deepEqual(Array.from(c.__renderDirty),[b])
  assert.equal(c.__nativeRenderBudgetRemaining,0)
  c.__nativeRenderBudgetRemaining=1;c.__sgFlushRenderData(stage)
  assert.equal(writes.at(-1),b);assert.equal(c.__renderDirty.size,0)
})
test('Pixi 8 rendering reports the original writer failure and always clears the writing guard',()=>{
  const node={__i:1,__nativePixi8:true},failure=new Error('native geometry cannot be published'),{c}=host([node])
  c.__wrType=()=>{throw failure}
  assert.throws(()=>c.__sgFlushRenderData(node,node),error=>error===failure)
  assert.equal(node.__sgWriting,0)
})
