#!/usr/bin/env node
// Independent, bounded cold-start verification. Always select an explicit device.
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import fs from 'node:fs/promises';
const exec=promisify(execFile),args=process.argv.slice(2);
const option=name=>args[args.indexOf(name)+1];
if(!args.includes('--serial'))throw new Error('Usage: node tools/check-android-runtime.mjs --serial DEVICE [--adb PATH] [--apk PATH] [--out DIRECTORY]');
const serial=option('--serial'),adb=args.includes('--adb')?option('--adb'):'adb',out=args.includes('--out')?option('--out'):'artifacts/runtime-audit';
const pkg='org.pixinative.conformance';
const run=async(...argv)=>(await exec(adb,['-s',serial,...argv],{maxBuffer:16*1024*1024,timeout:60000})).stdout;
await fs.mkdir(out,{recursive:true});
if(args.includes('--apk'))console.log((await run('install','-r',option('--apk'))).trim());
console.log((await run('shell','am','start','-S','-n',pkg+'/org.pixinative.runtime.MainActivity')).trim());
const deadline=Date.now()+45000;let logs='',report;
while(Date.now()<deadline){
  const pid=(await run('shell','pidof',pkg).catch(()=>'' )).trim().split(/\s/)[0];
  if(pid){logs=await run('logcat','-d','-v','brief','--pid='+pid);const match=logs.match(/\[native-conformance\] RESULT (\{[^\n]+\})/);if(match){await fs.writeFile(out+'/android-logcat.txt',logs);report=JSON.parse(match[1]);break;}if(logs.includes('[native-conformance] FATAL '))break;}
  await new Promise(resolve=>setTimeout(resolve,500));
}
await fs.writeFile(out+'/android-logcat.txt',logs);
if(!report)throw new Error('No conformance report before deadline; inspect '+out+'/android-logcat.txt');
if(report.schemaVersion>=2)report.results=Array.from(logs.matchAll(/\[native-conformance\] CASE (\{[^\n]+\})/g),m=>JSON.parse(m[1]));
if(args.includes('--native-input')){
  await run('shell','input','tap','12','200');
  const pid=(await run('shell','pidof',pkg)).trim().split(/\s/)[0];logs=await run('logcat','-d','-v','brief','--pid='+pid);
  const events=Array.from(logs.matchAll(/\[native-conformance\] INPUT (\{[^\n]+\})/g),m=>JSON.parse(m[1]));
  report.nativeInput={pass:events.length===3&&events.map(e=>e.type).join(',')==='pointerdown,pointerup,pointertap'&&events.every(e=>e.pointerId===2&&e.pointerType==='touch'&&e.isPrimary),events};
  await fs.writeFile(out+'/android-logcat.txt',logs);
}
report.device={serial,model:(await run('shell','getprop','ro.product.model')).trim(),sdk:(await run('shell','getprop','ro.build.version.sdk')).trim()};
await fs.writeFile(out+'/android-report.json',JSON.stringify(report,null,2)+'\n');
console.log(JSON.stringify(report,null,2));
process.exitCode=report.failed||report.nativeInput?.pass===false?1:0;
