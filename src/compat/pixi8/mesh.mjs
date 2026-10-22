import {P,V8,unsupported} from './environment.mjs'
import {applyOptions,installContainerAPI,installViewHitAPI} from './container.mjs'
import {destroyOwnedTextures,withoutTextureOwnership} from './lifetime.mjs'
function destroyMesh(options){if(this.destroyed)return;const texture=this.texture;P.Mesh.prototype.destroy.call(this,withoutTextureOwnership(options));destroyOwnedTextures([texture],options)}
export class Buffer extends P.Buffer{constructor(options){if(ArrayBuffer.isView(options)||options===undefined)super(options);else{super(options.data,!(options.usage&V8.BufferUsage.COPY_DST),!!(options.usage&V8.BufferUsage.INDEX));this.usage=options.usage;this.label=options.label;this.shrinkToFit=options.shrinkToFit??true}}get size(){return this.data.byteLength}}
export class MeshGeometry extends P.MeshGeometry{
  constructor(options={}){super(options.positions||new Float32Array([0,0,1,0,1,1,0,1]),options.uvs||new Float32Array(options.positions?.length??[0,0,1,0,1,1,0,1]),options.indices||new Uint32Array([0,1,2,0,2,3]));this.topology=options.topology??'triangle-list';this.attributes={...this.attributes,aPosition:{buffer:super.getBuffer('aVertexPosition')},aUV:{buffer:super.getBuffer('aTextureCoord')}};this.indexBuffer=super.getIndex()}
  getBuffer(name){return super.getBuffer(name==='aPosition'?'aVertexPosition':name==='aUV'?'aTextureCoord':name)}
  get positions(){return this.getBuffer('aPosition').data}set positions(v){const b=this.getBuffer('aPosition');b.data=v;b.update()}
  get uvs(){return this.getBuffer('aUV').data}set uvs(v){const b=this.getBuffer('aUV');b.data=v;b.update()}
  get indices(){return this.getIndex().data}set indices(v){const b=this.getIndex();b.data=v;b.update()}
}
const modes={'triangle-list':4,'triangle-strip':5,'line-list':1,'line-strip':3,'point-list':0}
export class Mesh extends P.Mesh{
  constructor(options={}){if(options.shader)unsupported('Mesh custom Shader');const g=options.geometry||new MeshGeometry();if(!(g.topology in modes))unsupported('Mesh geometry topology',g.topology);super(g,new P.MeshMaterial(options.texture||P.Texture.EMPTY),undefined,modes[g.topology]);applyOptions(this,options)}
  destroy(o){destroyMesh.call(this,o)}
}
installContainerAPI(Mesh.prototype);installViewHitAPI(Mesh.prototype)
export class MeshSimple extends P.SimpleMesh{constructor(o={}){if(o.shader)unsupported('MeshSimple custom Shader');super(o.texture,o.vertices||o.positions,o.uvs,o.indices,modes[o.topology??'triangle-list']);this.autoUpdate=o.autoUpdate??true;applyOptions(this,o)}}
MeshSimple.prototype.destroy=destroyMesh
installContainerAPI(MeshSimple.prototype);installViewHitAPI(MeshSimple.prototype)
export class MeshPlane extends P.SimplePlane{constructor(o={}){super(o.texture,o.verticesX??10,o.verticesY??10);this.autoResize=o.autoResize??true;applyOptions(this,o)}}
MeshPlane.prototype.destroy=destroyMesh
installContainerAPI(MeshPlane.prototype);installViewHitAPI(MeshPlane.prototype)
export class MeshRope extends P.SimpleRope{constructor(o={}){super(o.texture,o.points,o.textureScale??0);this.autoUpdate=o.autoUpdate??true;applyOptions(this,o)}}
MeshRope.prototype.destroy=destroyMesh
installContainerAPI(MeshRope.prototype);installViewHitAPI(MeshRope.prototype)
