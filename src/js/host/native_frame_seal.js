(function(){
'use strict';

// Cross-domain presentation audit. Each state domain owns its own clock; comparing clocks between
// domains is meaningless. We instead capture one immutable vector after every domain has closed
// its local debt, then require each component to remain byte-for-byte stable until native draw.
// Mode 1 is audit-only. Mode 2 is reserved for a future committed-frame host gate and intentionally
// behaves like mode 1 here: this module must not silently turn a JS skip into a backbuffer swap.
var providers=Object.create(null), expected=[], serial=0;
var stats=globalThis.__nativeFrameSealStats||(globalThis.__nativeFrameSealStats={
  captures:0,checks:0,ok:0,failures:0,validationFailures:0,missingFailures:0,providerErrors:0,lastFailure:null
});

function mode(){var value=Number(globalThis.__NATIVE_FRAME_SEAL_AUDIT)||0;return value===1||value===2?value:0;}
function log(message){try{(globalThis.__log||console.log)(message);}catch(_logError){}}
function isPlain(value){if(value===null||typeof value!=='object')return true;if(Array.isArray(value))return true;var proto=Object.getPrototypeOf(value);return proto===null||Object.prototype.toString.call(value)==='[object Object]';}
function clone(value,seen){
  if(value===null||typeof value==='string'||typeof value==='boolean')return value;
  if(typeof value==='number')return isFinite(value)?value:String(value);
  if(typeof value==='bigint')return String(value);
  if(typeof value==='undefined')return '__undefined__';
  if(typeof value==='function'||typeof value==='symbol')throw new TypeError('frame seal snapshots must contain data only');
  if(!isPlain(value))throw new TypeError('frame seal snapshot contains non-plain object');
  seen=seen||[];if(seen.indexOf(value)>=0)throw new TypeError('frame seal snapshot contains a cycle');seen.push(value);
  var out,i,keys;
  if(Array.isArray(value)){out=new Array(value.length);for(i=0;i<value.length;i++)out[i]=clone(value[i],seen);}
  else{out={};keys=Object.keys(value).sort();for(i=0;i<keys.length;i++)out[keys[i]]=clone(value[keys[i]],seen);}
  seen.pop();return out;
}
function stable(value){return JSON.stringify(clone(value));}
function normaliseValidation(result){
  if(result===undefined||result===null||result===true)return [];
  if(result===false)return ['invalid'];
  if(Array.isArray(result))return result.map(String);
  return [String(result)];
}
function providerNames(){return Object.keys(providers).sort();}
function captureDomains(root){
  var domains={},violations=[],names=providerNames();
  for(var i=0;i<names.length;i++){
    var name=names[i],provider=providers[name],snapshot;
    try{snapshot=clone(provider.snapshot(root));domains[name]=snapshot;
      var bad=normaliseValidation(provider.validate&&provider.validate(snapshot,root));
      for(var j=0;j<bad.length;j++)violations.push(name+':'+bad[j]);
    }catch(error){stats.providerErrors++;domains[name]={__error:String(error&&error.message||error)};violations.push(name+':provider-error');}
  }
  for(i=0;i<expected.length;i++)if(!providers[expected[i]])violations.push(expected[i]+':missing');
  return {domains:domains,violations:violations};
}
function firstDiff(left,right,path){
  if(left===right)return null;path=path||'';
  var lt=left===null?'null':typeof left,rt=right===null?'null':typeof right;
  if(lt!==rt)return path+':type '+lt+'!='+rt;
  if(lt!=='object')return path+':'+String(left)+'!='+String(right);
  var lk=Object.keys(left).sort(),rk=Object.keys(right).sort();
  if(lk.length!==rk.length||lk.join('\n')!==rk.join('\n'))return path+':keys '+lk.join(',')+'!='+rk.join(',');
  for(var i=0;i<lk.length;i++){var key=lk[i],diff=firstDiff(left[key],right[key],path?path+'.'+key:key);if(diff)return diff;}
  return null;
}

globalThis.__nativeFrameSealRegister=function(name,provider){
  name=String(name||'');if(!name||!provider||typeof provider.snapshot!=='function')throw new TypeError('invalid frame seal provider');
  providers[name]=provider;return function(){if(providers[name]===provider)delete providers[name];};
};
globalThis.__nativeFrameSealExpect=function(names){expected=(names||[]).map(String).filter(Boolean).sort();return expected.slice();};
globalThis.__nativeFrameSealCapture=function(root){
  if(!mode())return null;
  var captured=captureDomains(root|0),seal={serial:++serial,frame:globalThis.__frameNo|0,root:root|0,domains:captured.domains,violations:captured.violations};
  seal.digest=stable(seal.domains);stats.captures++;return seal;
};
globalThis.__nativeFrameSealVerify=function(seal){
  if(!seal||!mode())return true;
  stats.checks++;
  var current=captureDomains(seal.root|0),digest=stable(current.domains),diff=firstDiff(seal.domains,current.domains,''),violations=(seal.violations||[]).concat(current.violations||[]);
  if(diff||violations.length){
    stats.failures++;if(violations.length)stats.validationFailures+=violations.length;
    for(var i=0;i<violations.length;i++)if(/:missing$/.test(violations[i]))stats.missingFailures++;
    stats.lastFailure={serial:seal.serial|0,frame:seal.frame|0,root:seal.root|0,diff:diff||'',violations:violations.slice(0,16),before:seal.digest,after:digest};
    log('[frame-seal-audit] FAIL f='+seal.frame+' root='+seal.root+(diff?' diff='+diff:'')+(violations.length?' violations='+violations.join(','):''));
    return false;
  }
  stats.ok++;var every=Math.max(1,globalThis.__NATIVE_FRAME_SEAL_LOG_EVERY|0||256);if(stats.ok===1||(stats.ok%every)===0)log('[frame-seal-audit] OK×'+stats.ok);
  return true;
};
globalThis.__nativeFrameSealSnapshot=function(){return {mode:mode(),providers:providerNames(),expected:expected.slice(),stats:clone(stats)};};
})();
