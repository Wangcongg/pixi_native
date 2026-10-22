import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {loadConfig} from '../tools/config.mjs';

const sample=JSON.parse(fs.readFileSync(new URL('../examples/hello-pixi/app.config.json',import.meta.url)));
function fixture(change){
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'kit-配置 test-'));
  fs.mkdirSync(path.join(dir,'assets'));fs.writeFileSync(path.join(dir,'main.js'),'');
  const config=structuredClone(sample);change(config,dir);
  const file=path.join(dir,'app.config.json');fs.writeFileSync(file,JSON.stringify(config));
  return {file,cleanup:()=>fs.rmSync(dir,{recursive:true,force:true})};
}
test('sample is a self-contained project',()=>{
  const loaded=loadConfig(fileURLToPath(new URL('../examples/hello-pixi/app.config.json',import.meta.url)));
  assert.equal(loaded.config.app.id,'org.pixinative.hello');
});

test('entry cannot escape the project through a symlink',()=>{
  const f=fixture((c,dir)=>{fs.symlinkSync('/etc/hosts',path.join(dir,'external.js'));c.entry='external.js';});
  try{assert.throws(()=>loadConfig(f.file),/symlink escapes/);}finally{f.cleanup();}
});

test('runtime ABI mismatch is rejected',()=>{
  const f=fixture(c=>{c.runtimeAbi=2;});
  try{assert.throws(()=>loadConfig(f.file),/runtimeAbi/);}finally{f.cleanup();}
});
test('project fonts resolve without relying on machine font aliases',()=>{
  const f=fixture((c,dir)=>{fs.writeFileSync(path.join(dir,'assets/custom.ttf'),'font');c.fonts={'Custom Family':'assets/custom.ttf'};});
  try{assert.equal(loadConfig(f.file).fontFiles['Custom Family'],fs.realpathSync(path.join(path.dirname(f.file),'assets/custom.ttf')));}finally{f.cleanup();}
});
test('project font registration cannot escape through a symlink',()=>{
  const f=fixture((c,dir)=>{fs.symlinkSync('/etc/hosts',path.join(dir,'external.ttf'));c.fonts={External:'external.ttf'};});
  try{assert.throws(()=>loadConfig(f.file),/symlink escapes/);}finally{f.cleanup();}
});
