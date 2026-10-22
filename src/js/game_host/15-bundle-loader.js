// Generic local bundle loader.
enc = new Enc(__cmd);
function __loadScriptByLocal(rel){
  if(typeof __evalScript!=='function')throw new Error('__evalScript missing');
  if(typeof rel!=='string'||rel.charAt(0)==='/'||rel.indexOf('..')>=0)throw new Error('Invalid script path: '+rel);
  __evalScript(rel);
}
function __loadScriptByUrl(url){ __loadScriptByLocal(String(url).replace(/^file:\/\/\/app\//,'')); }
__loadScriptByLocal('host/native_fetch.js');
var __assetFetch=adapter.fetch.bind(adapter);
adapter.fetch=globalThis.__nativeWrapFetch(__assetFetch);
globalThis.fetch=adapter.fetch;
__loadScriptByLocal('project.js');
if(!globalThis.__PIXI_NATIVE_PROJECT__||globalThis.__PIXI_NATIVE_PROJECT__.runtimeAbi!==1)throw new Error('Project/runtime ABI mismatch');
__loadScriptByLocal('pixi.v7.4.3.js');
if(!globalThis.PIXI||PIXI.VERSION!=='7.4.3')throw new Error('Pixi Native requires Pixi 7.4.3');
PIXI.settings.ADAPTER=adapter;
