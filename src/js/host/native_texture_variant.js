// Select one physical texture variant for native ASTC-capable shells.
//
// Resource descriptors intentionally keep their browser-facing .png names.  Without a single
// selection point, Pixi bitmap-font parsing and FairyGUI atlas loading start a second PNG request
// after the native resource path has already selected KTX2.  This module keeps those logical names
// as cache aliases, but makes KTX2 the only physical request unless it actually fails to load.
(function(root,factory){
  var api=factory(root);
  if(typeof module==='object'&&module.exports)module.exports=api;
  if(root)root.__nativeTextureVariant=api;
})(typeof globalThis!=='undefined'?globalThis:this,function(root){
  'use strict';

  var ASTC_EXCLUDE=[];

  function unique(values){
    var out=[],seen=Object.create(null);
    for(var i=0;i<values.length;i++){
      var value=String(values[i]==null?'':values[i]);
      if(value&&!seen[value]){seen[value]=1;out.push(value);}
    }
    return out;
  }

  function astcEnabled(){
    if(root.__NATIVE_DISABLE_ASTC===true)return false;
    return root.__NATIVE_FORCE_ASTC===true||root.__isMobile===true;
  }

  function resolve(url){
    url=String(url==null?'':url);
    if(!url||!astcEnabled())return url;
    for(var i=0;i<ASTC_EXCLUDE.length;i++)if(url.indexOf(ASTC_EXCLUDE[i])>=0)return url;
    var suffixAt=url.search(/[?#]/),path=suffixAt<0?url:url.slice(0,suffixAt),suffix=suffixAt<0?'':url.slice(suffixAt);
    path=path.replace(/\.\{png,webp\}$/i,'.ktx2').replace(/\.(?:png|jpe?g|webp)$/i,'.ktx2');
    return path+suffix;
  }

  function cacheFallbackAliases(pixi,aliases,fallbackUrl,asset){
    var keys=aliases.slice(),resolver=pixi.Assets&&pixi.Assets.resolver;
    keys.push(fallbackUrl);
    try{
      var resolved=resolver&&resolver.resolve(fallbackUrl),entry=resolved&&resolved[fallbackUrl];
      if(entry&&entry.src)keys.push(entry.src);
    }catch(e){}
    keys=unique(keys);
    if(keys.length&&pixi.Assets.cache&&typeof pixi.Assets.cache.set==='function')pixi.Assets.cache.set(keys,asset);
    return asset;
  }

  function samePhysicalUrl(cacheKey,url){
    cacheKey=String(cacheKey||'');url=String(url||'');
    return cacheKey===url||cacheKey.slice(-url.length)===url;
  }

  // UIPackage.dispose removes its public atlas cache alias after destroying the Pixi Texture, but
  // Pixi Loader keeps the completed URL promise separately.  On re-entry that promise would return
  // the destroyed Texture without issuing a new request.  An absent public alias is the generation
  // boundary: remove stale URL cache entries and promises before loading the package again.
  function evictOrphanedLoad(pixi,selected,aliases){
    var assets=pixi&&pixi.Assets,cache=assets&&assets.cache,hasAlias=false;
    if(cache&&typeof cache.has==='function')for(var i=0;i<aliases.length;i++)if(cache.has(aliases[i])){hasAlias=true;break;}
    if(hasAlias)return 0;
    var removed=0,keys=unique([selected].concat(aliases));
    if(cache&&typeof cache.has==='function'&&typeof cache.remove==='function')for(i=0;i<keys.length;i++){
      try{if(cache.has(keys[i])){cache.remove(keys[i]);removed++;}}catch(e){}
    }
    var promises=assets&&assets.loader&&assets.loader.promiseCache;
    if(promises)for(var promiseKey in promises)if(samePhysicalUrl(promiseKey,selected)){
      delete promises[promiseKey];removed++;
    }
    return removed;
  }

  // Load a FairyGUI atlas under its authored logical alias while selecting exactly one physical
  // variant.  Loading the fallback by URL avoids reusing the alias->KTX2 resolver entry left by a
  // failed first attempt; the cache group is then rebound so package removal clears every alias.
  function loadTexture(pixi,logicalUrl,aliases,fallbackUrl,log){
    if(!pixi||!pixi.Assets||typeof pixi.Assets.load!=='function')return Promise.reject(new Error('PIXI.Assets unavailable'));
    logicalUrl=String(logicalUrl||'');fallbackUrl=String(fallbackUrl||logicalUrl);aliases=unique(Array.isArray(aliases)?aliases:[aliases]);
    var selected=resolve(logicalUrl),spec=aliases.length?{alias:aliases.length===1?aliases[0]:aliases,src:selected}:selected;
    evictOrphanedLoad(pixi,selected,aliases);
    return Promise.resolve(pixi.Assets.load(spec)).then(function nativeTextureVariantReady(asset){
      if(!asset)throw new Error('empty texture asset: '+selected);
      return asset;
    }).catch(function nativeTextureVariantFallback(error){
      if(selected===fallbackUrl)throw error;
      if(typeof log==='function')log('[texture-variant] KTX2 failed, fallback '+fallbackUrl+' '+String(error&&error.message||error));
      return Promise.resolve(pixi.Assets.load(fallbackUrl)).then(function nativeTextureFallbackReady(asset){
        if(!asset)throw new Error('empty fallback texture asset: '+fallbackUrl);
        return cacheFallbackAliases(pixi,aliases,fallbackUrl,asset);
      });
    });
  }

  // Pixi's stock bitmap-font parser joins the page name from the .fnt and calls the raw Loader
  // the authored PNG key expected by BitmapFont.install.
  function installBitmapFontParser(pixi,log){
    var parser=pixi&&pixi.loadBitmapFont;
    if(!parser||typeof parser.parse!=='function')return false;
    if(parser.__nativeTextureVariant)return true;
    var original=parser.parse;
    parser.parse=function(asset,data,loader){
      if(!loader||typeof loader.load!=='function')return original.apply(this,arguments);
      var proxy=Object.create(loader),rawLoad=loader.load;
      proxy.load=function(sources){
        var args=Array.prototype.slice.call(arguments),single=!Array.isArray(sources),authored=single?[sources]:sources.slice(),selected=[];
        for(var i=0;i<authored.length;i++)selected.push(resolve(authored[i]));
        var changed=false;for(i=0;i<authored.length;i++)if(String(authored[i])!==String(selected[i])){changed=true;break;}
        if(!changed)return rawLoad.apply(loader,args);
        args[0]=single?selected[0]:selected;
        return Promise.resolve(rawLoad.apply(loader,args)).then(function nativeBitmapFontPages(result){
          if(single)return result;
          var mapped={};
          for(var j=0;j<authored.length;j++){
            var texture=result&&result[selected[j]];
            mapped[authored[j]]=texture;mapped[selected[j]]=texture;
          }
          return mapped;
        }).catch(function nativeBitmapFontPageFallback(error){
          if(typeof log==='function')log('[texture-variant] font KTX2 failed, fallback '+String(error&&error.message||error));
          args[0]=sources;return rawLoad.apply(loader,args);
        });
      };
      return original.call(this,asset,data,proxy);
    };
    parser.__nativeTextureVariant=true;
    parser.__nativeOriginalParse=original;
    if(typeof log==='function')log('[texture-variant] bitmap-font page resolver installed');
    return true;
  }

  return {astcEnabled:astcEnabled,resolve:resolve,loadTexture:loadTexture,evictOrphanedLoad:evictOrphanedLoad,installBitmapFontParser:installBitmapFontParser};
});
