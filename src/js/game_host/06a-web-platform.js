// Browser-shaped primitives shared by both native JS engines. No DOM rendering emulation.
(function(g){
'use strict';
g.__nativeSetVisibility=function(visible){
  var doc=g.document;if(!doc)return;var hidden=!visible;if(doc.hidden===hidden)return;
  doc.hidden=hidden;doc.visibilityState=hidden?'hidden':'visible';
  if(hidden){if(typeof g.__nativeCancelPointers==='function')g.__nativeCancelPointers();if(typeof g.__nativeVibrate==='function')g.__nativeVibrate('[]');}
  if(typeof doc.dispatchEvent==='function')doc.dispatchEvent(new g.Event('visibilitychange'));
};
// Advertise vibration only where a real pattern/cancel bridge exists.
if(g.navigator&&typeof g.__nativeVibrate==='function'){
  g.navigator.vibrate=function(pattern){
    var input=Array.isArray(pattern)?pattern:[pattern],out=[];
    for(var i=0;i<Math.min(input.length,128);i++)out.push(Math.min(10000,Number(input[i])>>>0));
    if(out.length%2===0)out.pop();
    if(out.every(function(ms){return ms===0;}))out=[];
    if(out.length&&g.document&&g.document.hidden)return false;
    return !!g.__nativeVibrate(JSON.stringify(out));
  };
}

if(typeof g.DOMException!=='function'){
  g.DOMException=function DOMException(message,name){this.message=String(message||'');this.name=String(name||'Error');};
  g.DOMException.prototype=Object.create(Error.prototype);g.DOMException.prototype.constructor=g.DOMException;
}
if(typeof g.Event!=='function'){
  g.Event=function Event(type,options){
    if(!arguments.length)throw new TypeError('Event requires a type');options=options||{};
    this.type=String(type);this.bubbles=!!options.bubbles;this.cancelable=!!options.cancelable;this.composed=!!options.composed;
    this.defaultPrevented=false;this.cancelBubble=false;this.target=null;this.currentTarget=null;this.eventPhase=0;this.isTrusted=false;
    this.timeStamp=typeof performance!=='undefined'?performance.now():Date.now();
  };
  g.Event.prototype.preventDefault=function(){if(this.cancelable&&!this.__passive)this.defaultPrevented=true;};
  g.Event.prototype.stopPropagation=function(){this.cancelBubble=true;};
  g.Event.prototype.stopImmediatePropagation=function(){this.cancelBubble=true;this.__stopImmediate=true;};
  g.Event.prototype.composedPath=function(){return this.target?[this.target]:[];};
  for(var key of ['NONE','CAPTURING_PHASE','AT_TARGET','BUBBLING_PHASE']){var value=['NONE','CAPTURING_PHASE','AT_TARGET','BUBBLING_PHASE'].indexOf(key);g.Event[key]=g.Event.prototype[key]=value;}
}
if(typeof g.CustomEvent!=='function'){
  g.CustomEvent=function CustomEvent(type,options){g.Event.call(this,type,options);this.detail=options&&options.detail!==undefined?options.detail:null;};
  g.CustomEvent.prototype=Object.create(g.Event.prototype);g.CustomEvent.prototype.constructor=g.CustomEvent;
}
function report(error){if(typeof g.__log==='function')g.__log('[event] '+String(error&&error.stack||error));else if(typeof console!=='undefined')console.error(error);}
function install(target,nativeRegistry){
  var lists=Object.create(null);
  target.addEventListener=function(type,callback,options){
    if(callback==null)return;type=String(type);var capture=typeof options==='boolean'?options:!!(options&&options.capture),opts=typeof options==='object'&&options||{};
    if(opts.signal&&opts.signal.aborted)return;
    var list=lists[type]||(lists[type]=[]);for(var i=0;i<list.length;i++)if(list[i].callback===callback&&list[i].capture===capture)return;
    var record={callback:callback,capture:capture,once:!!opts.once,passive:!!opts.passive,signal:opts.signal,removed:false};
    record.invoke=function(event){
      if(record.removed)return;if(record.once)target.removeEventListener(type,callback,capture);
      var previous=event.currentTarget,passive=event.__passive;event.currentTarget=target;event.__passive=record.passive;
      try{if(typeof callback==='function')callback.call(target,event);else if(typeof callback.handleEvent==='function')callback.handleEvent(event);}
      catch(error){report(error);}finally{event.currentTarget=previous;event.__passive=passive;}
    };
    list.push(record);if(nativeRegistry)(nativeRegistry[type]||(nativeRegistry[type]=[])).push(record.invoke);
    if(record.signal){record.abort=function(){target.removeEventListener(type,callback,capture);};record.signal.addEventListener('abort',record.abort,{once:true});}
  };
  target.removeEventListener=function(type,callback,options){
    type=String(type);var list=lists[type],capture=typeof options==='boolean'?options:!!(options&&options.capture);if(!list)return;
    for(var i=0;i<list.length;i++){var r=list[i];if(r.callback!==callback||r.capture!==capture)continue;r.removed=true;list.splice(i,1);
      if(nativeRegistry){var nativeList=nativeRegistry[type]||[],at=nativeList.indexOf(r.invoke);if(at>=0)nativeList.splice(at,1);}
      if(r.signal)r.signal.removeEventListener('abort',r.abort);return;
    }
  };
  target.dispatchEvent=function(event){
    if(!event||typeof event.type!=='string')throw new TypeError('dispatchEvent requires an Event');if(event.__dispatching)throw new g.DOMException('Event is already dispatching','InvalidStateError');
    event.__dispatching=true;event.target=target;event.currentTarget=target;event.eventPhase=2;event.__stopImmediate=false;
    try{var snapshot=(lists[event.type]||[]).slice();for(var i=0;i<snapshot.length;i++){snapshot[i].invoke(event);if(event.__stopImmediate)break;}}
    finally{event.currentTarget=null;event.eventPhase=0;event.__dispatching=false;event.__stopImmediate=false;}
    return !event.defaultPrevented;
  };
  return target;
}
g.__nativeInstallEventTarget=install;
if(typeof g.EventTarget!=='function')g.EventTarget=function EventTarget(){install(this);};
if(typeof g.AbortController!=='function'){
  function AbortSignal(){install(this);this.aborted=false;this.reason=undefined;this.onabort=null;}
  AbortSignal.prototype.throwIfAborted=function(){if(this.aborted)throw this.reason;};
  function abort(signal,reason){if(signal.aborted)return;signal.aborted=true;signal.reason=reason===undefined?new g.DOMException('This operation was aborted','AbortError'):reason;var event=new g.Event('abort');signal.dispatchEvent(event);if(typeof signal.onabort==='function')try{signal.onabort(event);}catch(e){report(e);}}
  AbortSignal.abort=function(reason){var signal=new AbortSignal();abort(signal,reason);return signal;};
  AbortSignal.timeout=function(ms){if(!Number.isInteger(ms)||ms<0)throw new RangeError('Invalid timeout');var signal=new AbortSignal();setTimeout(function(){abort(signal,new g.DOMException('The operation timed out','TimeoutError'));},ms);return signal;};
  g.AbortSignal=AbortSignal;g.AbortController=function AbortController(){this.signal=new AbortSignal();};g.AbortController.prototype.abort=function(reason){abort(this.signal,reason);};
}
// UTF-8 decoding honors views, replacement boundaries, BOM, fatal errors and streaming.
if(typeof g.TextDecoder!=='function'){
  g.TextDecoder=function TextDecoder(label,options){label=String(label||'utf-8').trim().toLowerCase();if(!['utf-8','utf8','unicode-1-1-utf-8'].includes(label))throw new RangeError('Unsupported encoding: '+label);this.encoding='utf-8';this.fatal=!!(options&&options.fatal);this.ignoreBOM=!!(options&&options.ignoreBOM);this._pending=[];this._bomSeen=false;};
  g.TextDecoder.prototype.decode=function(input,options){
    var view=input==null?new Uint8Array(0):ArrayBuffer.isView(input)?new Uint8Array(input.buffer,input.byteOffset,input.byteLength):new Uint8Array(input),bytes=this._pending.concat(Array.from(view)),stream=!!(options&&options.stream),out='',i=0;
    this._pending=[];var self=this;
    function emit(cp){if(!self._bomSeen){self._bomSeen=true;if(cp===0xfeff&&!self.ignoreBOM)return;}out+=String.fromCodePoint(cp);}
    function invalid(){if(self.fatal){self._pending=[];self._bomSeen=false;throw new TypeError('The encoded data was not valid UTF-8');}emit(0xfffd);}
    while(i<bytes.length){var start=i,b=bytes[i++],needed=0,cp=0,lower=0x80,upper=0xbf;
      if(b<0x80){emit(b);continue;}if(b>=0xc2&&b<=0xdf){needed=1;cp=b&31;}else if(b>=0xe0&&b<=0xef){needed=2;cp=b&15;if(b===0xe0)lower=0xa0;if(b===0xed)upper=0x9f;}else if(b>=0xf0&&b<=0xf4){needed=3;cp=b&7;if(b===0xf0)lower=0x90;if(b===0xf4)upper=0x8f;}else{invalid();continue;}
      var valid=true;for(var n=0;n<needed;n++){if(i>=bytes.length){if(stream){this._pending=bytes.slice(start);i=bytes.length;valid=false;}else{invalid();valid=false;}break;}
        var next=bytes[i];if(next<(n===0?lower:0x80)||next>(n===0?upper:0xbf)){invalid();valid=false;break;}i++;cp=(cp<<6)|(next&63);
      }if(valid)emit(cp);
    }if(!stream){this._pending=[];this._bomSeen=false;}return out;
  };
}
if(typeof g.TextEncoder!=='function'){
  g.TextEncoder=function TextEncoder(){};g.TextEncoder.prototype.encoding='utf-8';
  function codePoint(text,index){var first=text.charCodeAt(index);if(first>=0xd800&&first<=0xdbff){var next=text.charCodeAt(index+1);if(next>=0xdc00&&next<=0xdfff)return [0x10000+((first-0xd800)<<10)+next-0xdc00,2];return [0xfffd,1];}return [first>=0xdc00&&first<=0xdfff?0xfffd:first,1];}
  function bytes(cp){return cp<0x80?[cp]:cp<0x800?[0xc0|(cp>>6),0x80|(cp&63)]:cp<0x10000?[0xe0|(cp>>12),0x80|((cp>>6)&63),0x80|(cp&63)]:[0xf0|(cp>>18),0x80|((cp>>12)&63),0x80|((cp>>6)&63),0x80|(cp&63)];}
  g.TextEncoder.prototype.encode=function(value){var text=value===undefined?'':String(value),out=[];for(var i=0;i<text.length;){var cp=codePoint(text,i);out.push.apply(out,bytes(cp[0]));i+=cp[1];}return new Uint8Array(out);};
  g.TextEncoder.prototype.encodeInto=function(value,destination){if(!(destination instanceof Uint8Array))throw new TypeError('Expected Uint8Array');var text=String(value),read=0,written=0;while(read<text.length){var cp=codePoint(text,read),encoded=bytes(cp[0]);if(written+encoded.length>destination.length)break;destination.set(encoded,written);written+=encoded.length;read+=cp[1];}return {read:read,written:written};};
}
})(globalThis);
