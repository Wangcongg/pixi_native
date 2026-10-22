// HTTP fetch delegates to the native XHR transport so origins, methods, headers and status
// remain intact. Packaged resource reads retain the asset scheduler and signed-resource path.
(function(g){
'use strict';
function Headers(input){this._values=Object.create(null);var self=this;if(input instanceof Headers)input.forEach(function(v,k){self.set(k,v);});else if(Array.isArray(input))input.forEach(function(pair){self.append(pair[0],pair[1]);});else if(input)Object.keys(input).forEach(function(k){self.set(k,input[k]);});}
Headers.prototype.set=function(key,value){this._values[String(key).toLowerCase()]=String(value);};
Headers.prototype.append=function(key,value){var previous=this.get(key);this.set(key,previous===null?value:previous+', '+value);};
Headers.prototype.get=function(key){key=String(key).toLowerCase();return Object.prototype.hasOwnProperty.call(this._values,key)?this._values[key]:null;};
Headers.prototype.has=function(key){return this.get(key)!==null;};Headers.prototype.delete=function(key){delete this._values[String(key).toLowerCase()];};
Headers.prototype.forEach=function(callback,scope){var self=this;Object.keys(this._values).forEach(function(key){callback.call(scope,self._values[key],key,self);});};
Headers.prototype.entries=function(){return Object.entries(this._values)[Symbol.iterator]();};Headers.prototype[Symbol.iterator]=Headers.prototype.entries;
if(!g.Headers)g.Headers=Headers;
function Response(bytes,options){options=options||{};this.status=options.status===undefined?200:options.status;this.statusText=options.statusText||'';this.url=options.url||'';this.ok=this.status>=200&&this.status<300;this.headers=new g.Headers(options.headers);this.bodyUsed=false;this._bytes=bytes||new ArrayBuffer(0);}
Response.prototype.arrayBuffer=function(){if(this.bodyUsed)return Promise.reject(new TypeError('Body has already been consumed'));this.bodyUsed=true;return Promise.resolve(this._bytes.slice(0));};
Response.prototype.text=function(){return this.arrayBuffer().then(function(bytes){return new g.TextDecoder().decode(bytes);});};
Response.prototype.json=function(){return this.text().then(JSON.parse);};
Response.prototype.blob=function(){var self=this;return this.arrayBuffer().then(function(bytes){return {size:bytes.byteLength,type:self.headers.get('content-type')||'',__bytes:bytes,__srcURL:self.url,arrayBuffer:function(){return Promise.resolve(bytes.slice(0));},text:function(){return Promise.resolve(new g.TextDecoder().decode(bytes));}};});};
Response.prototype.clone=function(){if(this.bodyUsed)throw new TypeError('Body has already been consumed');return new Response(this._bytes.slice(0),this);};
if(!g.Response)g.Response=Response;
g.__nativeWrapFetch=function(assetFetch){return function(url,options){
  options=options||{};url=String(url);var signal=options.signal;
  if(signal&&signal.aborted)return Promise.reject(signal.reason);
  if(!/^https?:\/\//i.test(url)){
    if(options.method&&String(options.method).toUpperCase()!=='GET')return Promise.reject(new TypeError('Packaged assets only support GET'));
    if(!signal)return assetFetch(url,options);
    return new Promise(function(resolve,reject){var finished=false;function cleanup(){signal.removeEventListener('abort',onAbort);}function onAbort(){if(finished)return;finished=true;cleanup();reject(signal.reason);}
      signal.addEventListener('abort',onAbort,{once:true});Promise.resolve().then(function(){if(signal.aborted)throw signal.reason;return assetFetch(url,options);}).then(function(response){if(!finished){finished=true;cleanup();resolve(response);}},function(error){if(!finished){finished=true;cleanup();reject(error);}});
    });
  }
  return new Promise(function(resolve,reject){
    var xhr=new g.XMLHttpRequest(),finished=false;
    function cleanup(){if(signal)signal.removeEventListener('abort',onAbort);}
    function fail(error){if(finished)return;finished=true;cleanup();reject(error);}
    function onAbort(){xhr.abort();fail(signal.reason);}
    xhr.open(options.method||'GET',url,true);xhr.responseType='arraybuffer';
    xhr.withCredentials=options.credentials==='include';
    new g.Headers(options.headers).forEach(function(value,key){xhr.setRequestHeader(key,value);});
    xhr.onload=function(){if(finished)return;finished=true;cleanup();var headers=new g.Headers();String(xhr.getAllResponseHeaders()||'').split(/\r?\n/).forEach(function(line){var at=line.indexOf(':');if(at>0)headers.append(line.slice(0,at).trim(),line.slice(at+1).trim());});resolve(new Response(xhr.response,{status:xhr.status,statusText:xhr.statusText,url:xhr.responseURL||url,headers:headers}));};
    xhr.onerror=function(){fail(new TypeError('Network request failed: '+url));};xhr.ontimeout=function(){fail(new g.DOMException('Network request timed out: '+url,'TimeoutError'));};
    xhr.onabort=function(){fail(signal&&signal.aborted?signal.reason:new g.DOMException('Network request aborted','AbortError'));};
    if(signal)signal.addEventListener('abort',onAbort,{once:true});
    try{xhr.send(options.body===undefined?null:options.body);}catch(error){fail(error);}
  });
};};
})(globalThis);
