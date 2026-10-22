import {buildSync} from 'esbuild';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
export function buildPixi8(outfile=path.join(root,'generated/pixi8-native.js')){
  buildSync({absWorkingDir:root,entryPoints:['src/compat/pixi8.mjs'],bundle:true,format:'iife',globalName:'PixiNative8',target:'es2020',outfile,keepNames:true,legalComments:'eof',minify:true});
  return outfile;
}
if(process.argv[1]===fileURLToPath(import.meta.url))console.log(buildPixi8());
