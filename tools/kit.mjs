#!/usr/bin/env node
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { loadConfig,xml } from './config.mjs';
import {buildPixi8} from './build-pixi8.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
function run(command,args,options={}){
  const result=spawnSync(command,args,{cwd:root,stdio:'inherit',...options});
  if(result.error)throw result.error;
  if(result.status!==0)throw new Error(`${command} exited with ${result.status}`);
}
function write(filename,content){fs.mkdirSync(path.dirname(filename),{recursive:true});fs.writeFileSync(filename,content);}
function plistValue(value){
  if(typeof value==='boolean')return value?'<true/>':'<false/>';
  if(Array.isArray(value))return `<array>${value.map(plistValue).join('')}</array>`;
  if(typeof value==='object')return `<dict>${Object.entries(value).map(([k,v])=>`<key>${xml(k)}</key>${plistValue(v)}`).join('')}</dict>`;
  return `<string>${xml(value)}</string>`;
}
function copyTree(source,destination){
  const treeRoot=fs.realpathSync(source);
  const ancestors=new Set();
  function copy(directory,out){
    if(ancestors.has(directory))throw new Error(`Asset directory symlink cycle: ${directory}`);
    ancestors.add(directory);
    fs.mkdirSync(out,{recursive:true});
    for(const item of fs.readdirSync(directory,{withFileTypes:true})){
      if(item.name.startsWith('.'))continue;
      const from=path.join(directory,item.name),to=path.join(out,item.name),real=fs.realpathSync(from);
      if(!real.startsWith(treeRoot+path.sep))throw new Error(`Asset escapes project: ${from}`);
      if(fs.statSync(real).isDirectory())copy(real,to);else fs.copyFileSync(real,to);
    }
    ancestors.delete(directory);
  }
  copy(treeRoot,destination);
}
export function prepare(filename){
  const loaded=loadConfig(filename),c=loaded.config;
  const stage=fs.mkdtempSync(path.join(os.tmpdir(),'pixi-native-game-'));
  try{
    for(const unit of ['game_host','native_scenegraph']){
      const dir=path.join(root,'src/js',unit),manifest=JSON.parse(fs.readFileSync(path.join(dir,'manifest.json'),'utf8'));
      const output=path.join(stage,manifest.output.replace(/^game\//,''));
      write(output,manifest.parts.map(part=>fs.readFileSync(path.join(dir,part),'utf8')).join(''));
    }
    for(const name of fs.readdirSync(path.join(root,'src/js/host')))fs.copyFileSync(path.join(root,'src/js/host',name),path.join(stage,'host',name));
    buildPixi8(path.join(stage,'pixi8-native.js'));
    fs.copyFileSync(path.join(root,'vendor/pixi.v7.4.3.js'),path.join(stage,'pixi.v7.4.3.js'));
    if(c.features.spine)fs.copyFileSync(path.join(root,'vendor/spine-pixi-v7.js'),path.join(stage,'spine-pixi-v7.js'));
    write(path.join(stage,'app/main.js'),fs.readFileSync(loaded.entry));
    copyTree(loaded.assets,path.join(stage,'assets'));
    const fontManifest={};
    for(const [family,file] of Object.entries(loaded.fontFiles)){
      const name=`fonts/${Object.keys(fontManifest).length}${path.extname(file).toLowerCase()}`;
      write(path.join(stage,name),fs.readFileSync(file));fontManifest[family]=name;
    }
    write(path.join(stage,'fonts/manifest.json'),JSON.stringify(fontManifest)+'\n');
    const runtime=structuredClone(c);
    write(path.join(stage,'project.js'),`globalThis.__PIXI_NATIVE_PROJECT__=${JSON.stringify(runtime)};\n`);
    fs.rmSync(path.join(root,'game'),{recursive:true,force:true});
    fs.cpSync(stage,path.join(root,'game'),{recursive:true});
  }finally{fs.rmSync(stage,{recursive:true,force:true});}
  write(path.join(root,'generated/app.json'),JSON.stringify(c,null,2)+'\n');
  write(path.join(root,'generated/android/res/values/project.xml'),`<resources><string name="app_name">${xml(c.app.name)}</string><color name="splash_bg">#101a2e</color></resources>\n`);
  const info={CFBundleName:c.app.name,CFBundleDisplayName:c.app.name,CFBundleIdentifier:c.app.id,
    CFBundleExecutable:'PixiNativeApp',CFBundlePackageType:'APPL',CFBundleInfoDictionaryVersion:'6.0',
    CFBundleShortVersionString:c.app.version,CFBundleVersion:String(c.app.build)};
  const iosInfo={...info,LSRequiresIPhoneOS:true,UILaunchScreen:{},
    UISupportedInterfaceOrientations:c.orientation==='landscape'?['UIInterfaceOrientationLandscapeLeft','UIInterfaceOrientationLandscapeRight']:['UIInterfaceOrientationPortrait'],
    UIApplicationSceneManifest:{UIApplicationSupportsMultipleScenes:false,UISceneConfigurations:{UIWindowSceneSessionRoleApplication:[{UISceneConfigurationName:'Default Configuration',UISceneDelegateClassName:'SceneDelegate'}]}}};
  for(const [platform,values] of Object.entries({ios:iosInfo,mac:{...info,NSHighResolutionCapable:true,LSMinimumSystemVersion:'12.0'}})){
    write(path.join(root,'generated',platform,'Info.plist'),`<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd"><plist version="1.0">${plistValue(values)}</plist>\n`);
    write(path.join(root,'generated',platform,'project.cmake'),`set(KIT_APP_ID "${c.app.id}")\nset(KIT_APP_VERSION "${c.app.version}")\nset(KIT_APP_BUILD "${c.app.build}")\nset(KIT_DEVELOPMENT_TEAM "${c.ios.developmentTeam.replace(/[^A-Za-z0-9]/g,'')}")\n`);
  }
  console.log(`[kit] prepared ${c.app.name} (${c.app.id}), ABI ${c.runtimeAbi}`);
  return loaded;
}
function doctor(filename){
  const {config}=loadConfig(filename);console.log(`[kit] configuration OK: ${config.app.id}`);
  for(const cmd of ['node','cmake','xcodebuild','pod','java']){
    const result=spawnSync('/usr/bin/which',[cmd],{encoding:'utf8'});
    console.log(`[kit] ${cmd}: ${result.status===0?result.stdout.trim():'NOT FOUND'}`);
  }
  console.log(`[kit] Android SDK: ${process.env.ANDROID_HOME||process.env.ANDROID_SDK_ROOT||'set ANDROID_HOME or android/local.properties'}`);
}
const [command,...args]=process.argv.slice(2);
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)){
  try{
    if(command==='prepare')prepare(args[0]||'examples/hello-pixi/app.config.json');
    else if(command==='doctor')doctor(args[0]||'examples/hello-pixi/app.config.json');
    else if(command==='build'){
      const [platform,filename='examples/hello-pixi/app.config.json']=args;
      if(!['android','ios','mac'].includes(platform))throw new Error('platform must be android, ios or mac');
      const {config}=prepare(filename);
      if(platform==='android')run('./gradlew',[':app:assembleDebug'],{cwd:path.join(root,'android')});
      else{
        const build=path.join(root,platform==='ios'?'build-ios-simulator':'build-mac');
        const flags=platform==='ios'?['-DCMAKE_SYSTEM_NAME=iOS','-DCMAKE_OSX_SYSROOT=iphonesimulator','-DCMAKE_OSX_ARCHITECTURES=arm64']:[];
        if(platform==='mac'&&process.env.ANGLE_ROOT)flags.push('-DANGLE_ROOT='+process.env.ANGLE_ROOT);
        run('cmake',['-S',platform,'-B',build,'-G','Xcode',...flags]);
        run('xcodebuild',['-project',path.join(build,'PixiNativeApp.xcodeproj'),'-scheme','PixiNativeApp','-configuration','Debug',...(platform==='ios'?['-sdk','iphonesimulator']:[]),'-derivedDataPath',path.join(build,'DerivedData'),'CODE_SIGNING_ALLOWED=NO','build']);
      }
      console.log(`[kit] ${platform} debug build complete: ${config.app.id}`);
    }else throw new Error('Usage: node tools/kit.mjs prepare|doctor <config> OR build android|ios|mac <config>');
  }catch(error){console.error('[kit] '+error.message);process.exitCode=1;}
}
