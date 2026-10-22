// Enumerate the fixed official export surface, without treating presence as compatibility.
import {buildSync} from 'esbuild';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
globalThis.navigator??={userAgent:'Native Kit API audit'};
const official=await import('../vendor/pixi.v8.20.0.mjs');
const built=buildSync({absWorkingDir:root,entryPoints:['src/compat/pixi8.mjs'],bundle:true,format:'esm',write:false,metafile:true});
const implemented=new Set(Object.values(built.metafile.outputs)[0].exports);
const explicitUnsupported=new Set(['Filter','Shader','GlProgram','GpuProgram']);
const helper=new Set(['buildContextBatches','BigPool','BatchableGraphics']);
const exports=Object.keys(official).filter(k=>!helper.has(k)).sort().map(name=>({name,status:explicitUnsupported.has(name)?'explicitly-unsupported':implemented.has(name)?'adapter-present-requires-semantic-coverage':'unadapted'}));
const classes=['Application','Ticker','Container','Sprite','AnimatedSprite','TilingSprite','NineSliceSprite','Graphics','GraphicsContext','GraphicsPath','FillGradient','FillPattern','Text','TextStyle','BitmapText','BitmapFont','Texture','TextureSource','TextureStyle','RenderTexture','Assets','Mesh','Geometry','Buffer','MeshGeometry','PlaneGeometry','RopeGeometry','PerspectivePlaneGeometry','PerspectiveMesh','MeshPlane','MeshRope','TexturePoolClass','TextureMatrix','TextureUvs','Transform','Pool','PoolGroupClass','Particle','ParticleContainer','Culler'];
const prototypes=Object.fromEntries(classes.map(name=>[name,Object.getOwnPropertyNames(official[name]?.prototype??{}).filter(k=>k!=='constructor').sort()]));
const report={baseline:'pixi.js@8.20.0',source:'https://pixijs.download/v8.20.0/docs/index.html',note:'Export presence is not full compatibility. This inventory includes public renderer infrastructure as well as scene APIs. See pixi8-compatibility.md for semantic limits and device evidence.',summary:{official:exports.length,adapterPresent:exports.filter(x=>x.status.startsWith('adapter-present')).length,explicitlyUnsupported:exports.filter(x=>x.status==='explicitly-unsupported').length,unadapted:exports.filter(x=>x.status==='unadapted').length},exports,officialOwnPrototypes:prototypes};
const out=path.join(root,'docs/pixi8-api-audit.json');
fs.writeFileSync(out,JSON.stringify(report,null,2)+'\n');
console.log(JSON.stringify(report.summary));
