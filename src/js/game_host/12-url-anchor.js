// ===== URL 解析 + anchor 元素（axios isURLSameOrigin 用 createElement('a') 读 protocol/host/pathname；缺则 charAt of undefined）=====
// WHATWG URL parser preprocessing: strip leading/trailing C0 controls or spaces, then remove
// ASCII tabs/newlines anywhere in the input. Browser XMLHttpRequest gets this behavior from the
// platform URL parser. Our JSC fallback must do it explicitly before handing the URL to Foundation;
// otherwise an accidental trailing space becomes a literal `%20` route instead of being ignored.
function __cleanURLInput(value){
  return String(value==null?'':value)
    .replace(/^[\u0000-\u0020]+|[\u0000-\u0020]+$/g,'')
    .replace(/[\u0009\u000A\u000D]/g,'');
}
function __parseAbs(href){
  href = __cleanURLInput(href);
  var m = /^([a-z][a-z0-9+.\-]*:)\/\/([^/?#]*)([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(href||'') || [];
  var hostport = (m[2]||'').replace(/^[^@]*@/,'');           // 去 userinfo
  var hm = /^([^:]*)(?::(\d+))?$/.exec(hostport) || [];
  return { protocol:m[1]||'http:', host:hostport, hostname:hm[1]||'', port:hm[2]||'',
           pathname:m[3]||'/', search:m[4]||'', hash:m[5]||'', origin:(m[1]||'http:')+'//'+hostport };
}
function __resolveURL(url, base){
  url = __cleanURLInput(url);
  base = __cleanURLInput(base || (globalThis.location && globalThis.location.href) || 'http://localhost/');
  // Axios can pass a configured development server through as `host:port/path`
  // without a scheme. Browsers treat that as a relative URL, which would attach
  // the API request (and its login body) to the page origin. Recognise local
  // IPv4/localhost authorities before resolving ordinary relative asset paths.
  if (/^(?:localhost|\d{1,3}(?:\.\d{1,3}){3}):\d+(?:[/?#]|$)/i.test(url)) return 'http://'+url;
  try { if (typeof globalThis.URL==='function'){ var _u=new globalThis.URL(url, base); if(_u && typeof _u.href==='string' && _u.href) return _u.href; } } catch(e){}  // 真 URL 才用（native JSC 无 URL，我们的空壳无 href→退回正则）
  if (/^[a-z][a-z0-9+.\-]*:\/\//i.test(url)) return url;                          // 已是绝对
  if (/^\/\//.test(url)) { var bs=/^([a-z][a-z0-9+.\-]*:)/i.exec(base); return (bs?bs[1]:'http:')+url; }  // 协议相对
  var b = __parseAbs(base);
  if (url.charAt(0)==='/') {
    if (/^\/(libs|modules|assets|js)\//.test(url)) return b.protocol+'//'+b.host+'/dev1'+url;
    return b.protocol+'//'+b.host+url;                                             // 绝对路径
  }
  if (url.charAt(0)==='?') return b.protocol+'//'+b.host+b.pathname+url;
  if (url.charAt(0)==='#') return b.protocol+'//'+b.host+b.pathname+b.search+url;
  var dir = b.pathname.replace(/[^/]*$/,'');                                       // 相对路径（相对 base 目录）
  return b.protocol+'//'+b.host+dir+url;
}
function __urlParts(href){
  try { if (typeof globalThis.URL==='function'){ var u=new globalThis.URL(href); if(u && typeof u.href==='string' && u.href) return { href:u.href, protocol:u.protocol, host:u.host, hostname:u.hostname, port:u.port, pathname:u.pathname, search:u.search, hash:u.hash, origin:u.origin }; } } catch(e){}
  var p = __parseAbs(href); p.href = href; return p;
}
function makeAnchor(){
  var a = makeEl('a'); var _href = '';
  function apply(v){
    _href = __resolveURL(v, globalThis.location && globalThis.location.href);
    var p = __urlParts(_href);
    a.protocol=p.protocol; a.host=p.host; a.hostname=p.hostname; a.port=p.port;
    a.pathname=p.pathname; a.search=p.search; a.hash=p.hash; a.origin=p.origin;
  }
  Object.defineProperty(a,'href',{ get:function(){ return _href; }, set:apply, configurable:true });
  var origSet = a.setAttribute;
  a.setAttribute = function(k,v){ if(String(k).toLowerCase()==='href') apply(v); else origSet.call(a,k,v); };
  apply('');  // 初始空，protocol/host/pathname 等就位（避免 undefined.charAt）
  return a;
}
