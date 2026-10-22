import fs from 'node:fs';
import vm from 'node:vm';

export function createNativeHost(){
  const c=vm.createContext({console});
  const run=file=>vm.runInContext(fs.readFileSync(new URL('../../'+file,import.meta.url),'utf8'),c,{filename:file});
  run('vendor/pixi.v7.4.3.js');const P=c.PIXI,original={};
  for(const name of ['Container','Graphics','Sprite','Text','BitmapText','AnimatedSprite','Matrix','Transform'])original[name]=P[name];
  const white=new P.Texture(new P.BaseTexture(new P.BufferResource(new Uint8Array([255,255,255,255]),{width:1,height:1})));
  Object.defineProperty(P.Texture,'WHITE',{value:white});Object.defineProperty(P.Texture,'EMPTY',{value:white});
  run('src/js/host/native_display_size_contract.js');
  const children=new Map(),calls=[],meshes=new Map();let next=0;
  const R=c.__nativeSceneRuntime={
    buffer:new Float32Array(64*1024),stride:64,objects:[null],
    alloc(node){node.__i=++next;this.objects[next]=node;children.set(next,[]);return next;},
    markLocal(){},markBounds(){},writeHitMeta(){},markVisual(node){calls.push(node);},
    writeVisual(){},retainTexture(){},releaseTexture(){},releaseVisual(){},textureId(){return 1;},
    setParent(id,parent,before){this.remove(id);const list=children.get(parent);let at=list.indexOf(before);if(at<0)at=list.length;list.splice(at,0,id);},
    remove(id){for(const list of children.values()){const at=list.indexOf(id);if(at>=0)list.splice(at,1);}c.__sgStructVer=(c.__sgStructVer||0)+1;},
    free(node){this.remove(node.__i);this.objects[node.__i]=null;},
    syncWorld(node){if(node.parent)this.syncWorld(node.parent);const local=node.localTransform.clone(),world=node.parent?node.parent.worldTransform.clone().append(local):local;
      const a=['a','b','c','d','tx','ty'];for(let i=0;i<6;i++)this.buffer[node.__i*64+12+i]=world[a[i]];
    }
  };
  c.__sgNativeChildren=id=>children.get(id).slice();c.__sgTypeW=(b,type)=>{R.buffer[b+11]=type;};
  c.__nativeGraphicsMesh=(id,points,indices,uvs,batches,blend,mask)=>{meshes.set(id,{points,indices,uvs,batches,blend,mask});return true;};
  run('src/js/host/native_node_proxy.js');
  return {c,P,original,R,calls,meshes,run};
}
