import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

function host(){
  const context=vm.createContext({});
  for(const file of ['vendor/pixi.v7.4.3.js','src/js/host/native_display_size_contract.js'])
    vm.runInContext(fs.readFileSync(new URL('../'+file,import.meta.url),'utf8'),context);
  const dirty=[];
  context.__nativeSceneRuntime={
    buffer:new Float32Array(64*16),stride:64,objects:[null],
    alloc(node){node.__i=this.objects.length;this.objects.push(node);return node.__i;},
    markLocal:id=>dirty.push(id),markBounds(){},writeHitMeta(){}
  };
  vm.runInContext(fs.readFileSync(new URL('../src/js/host/native_node_proxy.js',import.meta.url),'utf8'),context);
  return {context,dirty};
}

test('native transform clones retain Pixi coordinates without aliasing the shared buffer',()=>{
  const {context,dirty}=host(),node=new context.NativeNodeProxy();
  for(const key of ['position','scale','pivot','skew']){
    const point=node[key];point.set(12.5,-7.25);
    const clone=point.clone();
    assert.ok(clone instanceof context.PIXI.ObservablePoint);
    assert.ok(point.equals(clone));
    assert.ok(clone.equals(point));
    dirty.length=0;clone.set(90,80);
    assert.deepEqual([point.x,point.y],[12.5,-7.25]);
    assert.deepEqual(dirty,[node.__i]);
    point.set(20,30);
    assert.deepEqual([clone.x,clone.y],[90,80]);
    assert.equal(point.equals(clone),false);
    // Assigning the snapshot keeps the native bridge point itself stable.
    node[key]=clone;
    assert.equal(node[key],point);
    assert.deepEqual([point.x,point.y],[90,80]);
  }
});

test('native point clone supports the ObservablePoint callback and scope overrides',()=>{
  const {context,dirty}=host(),node=new context.NativeNodeProxy();
  node.position.set(3,4);dirty.length=0;
  const scope={calls:0};
  const clone=node.position.clone(function(){this.calls++;},scope);
  clone.set(3,4);assert.equal(scope.calls,0);
  clone.x=5;assert.equal(scope.calls,1);
  assert.deepEqual(dirty,[]);
  assert.deepEqual([node.position.x,node.position.y],[3,4]);
});
