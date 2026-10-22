import fs from 'node:fs';
import path from 'node:path';

export function loadConfig(filename) {
  const configFile=path.resolve(filename), root=path.dirname(configFile);
  const config=JSON.parse(fs.readFileSync(configFile,'utf8'));
  const fail=message=>{throw new Error(`${configFile}: ${message}`);};
  if(config.pixiVersion!==undefined&&config.pixiVersion!==7&&config.pixiVersion!==8)fail('pixiVersion must be 7 or 8');
  if(config.schemaVersion!==1||config.runtimeAbi!==1)fail('schemaVersion and runtimeAbi must be 1');
  if(!config.app||!/^[A-Za-z][A-Za-z0-9]*(\.[A-Za-z][A-Za-z0-9]*){2,}$/.test(config.app.id))fail('app.id must be a reverse-DNS application id');
  if(typeof config.app.name!=='string'||!config.app.name.trim())fail('app.name is required');
  if(!/^\d+\.\d+\.\d+$/.test(config.app.version)||!Number.isSafeInteger(config.app.build)||config.app.build<1)fail('invalid app version/build');
  if(!['development','production'].includes(config.environment))fail('environment must be development or production');
  if(!['landscape','portrait'].includes(config.orientation))fail('orientation must be landscape or portrait');
  if(config.links?.supportUrl){
    const url=new URL(config.links.supportUrl);
    if(url.protocol!=='https:'||!url.host)fail('links.supportUrl must be an HTTPS URL');
  }
  function relative(value,label,directory=false){
    if(typeof value!=='string'||!value||path.isAbsolute(value))fail(`${label} must be a project-relative path`);
    const resolved=path.resolve(root,value),realRoot=fs.realpathSync(root);
    if(!resolved.startsWith(root+path.sep))fail(`${label} escapes project directory`);
    if(!fs.existsSync(resolved))fail(`${label} does not exist: ${value}`);
    const real=fs.realpathSync(resolved);
    if(!real.startsWith(realRoot+path.sep))fail(`${label} symlink escapes project directory`);
    if(directory!==fs.statSync(real).isDirectory())fail(`${label} must be a ${directory?'directory':'file'}`);
    return real;
  }
  const entry=relative(config.entry,'entry'),assets=relative(config.assets,'assets',true);
  const fontFiles={};
  if(config.fonts!==undefined){
    if(!config.fonts||typeof config.fonts!=='object'||Array.isArray(config.fonts))fail('fonts must map family names to project-relative files');
    for(const [family,file] of Object.entries(config.fonts)){
      if(!family.trim()||/[\r\n]/.test(family))fail('invalid font family');
      fontFiles[family]=relative(file,`fonts.${family}`);
      if(!/\.(ttf|otf)$/i.test(file))fail(`fonts.${family} must be a TTF or OTF file`);
    }
  }
  if(!config.features||typeof config.features.spine!=='boolean')fail('features.spine must be a boolean');
  if(!Array.isArray(config.android.abis)||!config.android.abis.length||config.android.abis.some(v=>!['arm64-v8a','x86_64'].includes(v)))fail('unsupported Android ABI');
  if(!config.ios||typeof config.ios.developmentTeam!=='string')fail('ios.developmentTeam must be a string');
  if(config.services && Object.values(config.services).some(v=>v?.enabled))fail('Public runtime does not include service SDKs');
  return {config,configFile,root,entry,assets,fontFiles};
}

export const xml=value=>String(value).replace(/[&<>"']/g,char=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&apos;'}[char]));
