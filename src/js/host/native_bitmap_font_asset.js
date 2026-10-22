// Make a bitmap font's registration table the source of truth for load readiness.
//
// Popup/scene cleanup intentionally calls BitmapFont.uninstall(), which destroys the glyph page
// textures. Pixi Assets can still retain the parsed .fnt object under its full URL, though, while
// WaitLoadUtils only checks Assets.cache. A later waitLoad(url) or loadBundle(bundleId) can then
// report success without running the bitmap-font parser again. Keep this host patch generic: every
// .fnt load verifies that its face is installed and evicts stale URL/page promises before retrying.
(function(root,factory){
  var api=factory(root);
  if(typeof module==='object'&&module.exports)module.exports=api;
  if(root)root.__nativeBitmapFontAsset=api;
})(typeof globalThis!=='undefined'?globalThis:this,function(root){
  'use strict';

  function cleanUrl(value){return String(value==null?'':value).split('#')[0].split('?')[0];}
  function basename(value){var path=cleanUrl(value),slash=Math.max(path.lastIndexOf('/'),path.lastIndexOf('\\'));return slash>=0?path.substring(slash+1):path;}
  function isFontUrl(value){return /\.fnt(?:[?#]|$)/i.test(String(value==null?'':value));}
  function fontStem(value){return basename(value).replace(/\.fnt$/i,'');}

  function unique(values){
    var out=[],seen=Object.create(null);
    for(var i=0;i<values.length;i++){var value=String(values[i]==null?'':values[i]);if(value&&!seen[value]){seen[value]=1;out.push(value);}}
    return out;
  }

  function cacheGet(cache,key){try{return cache&&typeof cache.get==='function'&&(!cache.has||cache.has(key))?cache.get(key):undefined;}catch(e){return undefined;}}
  function cacheRemove(cache,key){
    try{
      if(!cache||typeof cache.has!=='function'||!cache.has(key))return false;
      if(typeof cache.remove==='function')cache.remove(key);else if(typeof cache.delete==='function')cache.delete(key);
      return true;
    }catch(e){return false;}
  }

  function pageMap(){return root.__nativeBitmapFontPages||(root.__nativeBitmapFontPages=Object.create(null));}

  function faceMap(){return root.__nativeBitmapFontFaces||(root.__nativeBitmapFontFaces=Object.create(null));}
  function urlKeys(urls){
    var values=[];
    for(var i=0;i<urls.length;i++){
      var url=String(urls[i]||''),file=basename(url),stem=fontStem(url),plain=cleanUrl(url);
      values.push(url,plain,file,stem);
    }
    return unique(values);
  }

  function rememberReadyFace(pixi,urls,loaded){
    var available=pixi&&pixi.BitmapFont&&pixi.BitmapFont.available;
    if(!available)return false;
    var keys=urlKeys(urls),cache=pixi.Assets&&pixi.Assets.cache,faces=faceMap(),candidates=[];
    if(loaded&&loaded.font)candidates.push(String(loaded.font));
    for(var i=0;i<keys.length;i++){
      var cached=cacheGet(cache,keys[i]);
      if(cached&&cached.font)candidates.push(String(cached.font));
      if(faces[keys[i]])candidates.push(String(faces[keys[i]]));
    }
    for(var u=0;u<urls.length;u++){var stem=fontStem(urls[u]);if(stem)candidates.push(stem);}
    candidates=unique(candidates);
    for(var c=0;c<candidates.length;c++){
      var face=candidates[c];
      if(!available[face])continue;
      var pages=[],font=loaded||available[face],textures=font&&font.pageTextures||{};
      for(var page in textures){var texture=textures[page];if(!texture)continue;
        pages=pages.concat(texture.textureCacheIds||[]);
        var resource=texture.baseTexture&&texture.baseTexture.resource;if(resource&&resource.src)pages.push(resource.src);
      }
      pages=unique(pages);
      for(var k=0;k<keys.length;k++){faces[keys[k]]=face;if(pages.length)pageMap()[keys[k]]=pages;}
      return true;
    }
    return false;
  }

  function derivedPageUrls(url){
    var plain=cleanUrl(url),stem=plain.replace(/\.fnt$/i,'');
    return [stem+'.png',stem+'.webp',stem+'.ktx2'];
  }

  function evictStaleFont(pixi,urls){
    if(!pixi||!pixi.Assets)return 0;
    var keys=urlKeys(urls),pageUrls=[];for(var keyIndex=0;keyIndex<keys.length;keyIndex++)pageUrls=pageUrls.concat(pageMap()[keys[keyIndex]]||[]);
    for(var i=0;i<urls.length;i++)pageUrls=pageUrls.concat(derivedPageUrls(urls[i]));
    var all=unique(keys.concat(pageUrls)),removed=0,cache=pixi.Assets.cache;
    for(i=0;i<all.length;i++)if(cacheRemove(cache,all[i]))removed++;

    // Loader promises are keyed by resolved URLs, not always by the public cache alias. Remove the
    // .fnt promise and the conventional same-stem page names; otherwise the parser can reinstall a
    // new BitmapFont around an already-destroyed Texture returned by the old page promise.
    var loader=pixi.Assets.loader,promises=loader&&loader.promiseCache;
    if(promises){
      var needles=[];
      for(i=0;i<urls.length;i++)needles.push(cleanUrl(urls[i]));
      for(i=0;i<pageUrls.length;i++)needles.push(cleanUrl(pageUrls[i]));
      needles=unique(needles);
      for(var promiseKey in promises){
        var cleanKey=cleanUrl(promiseKey),match=false;
        for(var n=0;n<needles.length&&!match;n++)match=cleanKey===needles[n]||cleanKey.indexOf(needles[n])>=0;
        if(match){delete promises[promiseKey];removed++;}
      }
    }
    return removed;
  }

  return {isFontUrl:isFontUrl,rememberReadyFace:rememberReadyFace,evictStaleFont:evictStaleFont};
});
