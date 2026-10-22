// Regenerate the official API reference/CPU algorithms from a stock pixi.js@8.20.0 installation.
// No project code or configuration is bundled. esbuild is a build-time tool only.
import fs from 'node:fs/promises';import path from 'node:path';import {createRequire} from 'node:module';import {fileURLToPath} from 'node:url';
const source=path.resolve(process.argv[2]||'node_modules/pixi.js'),root=fileURLToPath(new URL('..',import.meta.url));
const pkg=JSON.parse(await fs.readFile(path.join(source,'package.json'),'utf8'));if(pkg.name!=='pixi.js'||pkg.version!=='8.20.0')throw new Error('Requires unmodified pixi.js@8.20.0');
const require=createRequire(path.join(source,'package.json'));const {build}=require('esbuild');
await build({absWorkingDir:source,stdin:{contents:"export * from './lib/index.mjs';\nexport {buildContextBatches} from './lib/scene/graphics/shared/utils/buildContextBatches.mjs';\nexport {BigPool} from './lib/utils/pool/PoolGroup.mjs';\nexport {BatchableGraphics} from './lib/scene/graphics/shared/BatchableGraphics.mjs';",resolveDir:source,sourcefile:'native-api-reference.mjs'},bundle:true,format:'esm',platform:'browser',target:'es2022',outfile:path.join(root,'vendor/pixi.v8.20.0.mjs'),banner:{js:'/*! PixiJS v8.20.0 | Copyright PixiJS team | MIT License | Official API and CPU algorithms; native rendering is provided separately. */'}});
await fs.copyFile(path.join(source,'LICENSE'),path.join(root,'vendor/pixi.v8.20.0.LICENSE'));
