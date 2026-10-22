// Preserve the aliases used by business code when a resource manifest namespaces image names.
//
// Some machine manifests use keys such as `m90001_bg_di2.png` to keep bundles collision-free,
// while the scene creates the image with the logical name `bg_di2.png`. Browsers can mask this
// mismatch with a later URL load; the native runtime must bind both names to the already-loaded
// texture so Sprite.from() never creates a detached 0x0 placeholder.
(function(root,factory){
  var api=factory(root);
  if(typeof module==='object'&&module.exports)module.exports=api;
  if(root)root.__nativeAssetAlias=api;
})(typeof globalThis!=='undefined'?globalThis:this,function(root){
  'use strict';

  function basename(path,pathUtils){
    if(pathUtils&&typeof pathUtils.findFile==='function')return pathUtils.findFile(path);
    path=String(path||'');
    var slash=Math.max(path.lastIndexOf('/'),path.lastIndexOf('\\'));
    return slash>=0?path.substring(slash+1):path;
  }

  function rawKey(entry){
    if(entry&&typeof entry==='object')return entry.key;
    return entry;
  }

  function addAlias(bundle,alias,sourceKey){
    if(!alias||!sourceKey||bundle[alias]!==undefined||bundle[sourceKey]===undefined)return false;
    bundle[alias]=bundle[sourceKey];
    return true;
  }

  // branch leaves authored JPG URLs unchanged. Native machine manifests intentionally retain one
  // physical texture variant, so a JPG-authored scene would otherwise request a file that was
  // never downloaded. Select the same physical URL for every alias before the bundle is registered.
  function selectImageVariant(bundle,sourceKey){
    var variant=root&&root.__nativeTextureVariant;
    if(!sourceKey||!variant||typeof variant.resolve!=='function')return false;
    var authored=bundle[sourceKey];
    if(typeof authored!=='string'||!authored)return false;
    var selected=variant.resolve(authored);
    if(!selected||selected===authored)return false;
    for(var key in bundle)if(bundle[key]===authored)bundle[key]=selected;
    return true;
  }

  function expandBundleAliases(cfg,bundle,pathUtils){
    if(!bundle||!cfg||typeof cfg!=='object')return bundle;
    var list=cfg.list&&typeof cfg.list==='object'?cfg.list:cfg;
    var sections=['files','img','img_webp','atlas','sound','font'];

    // ResCfg2BundleAssets reduces path entries to basename keys. Keep the original spelling too,
    // otherwise callers that use a full manifest path can bind another bundle's same-named file.
    for(var s=0;s<sections.length;s++){
      var entries=list[sections[s]];
      if(!Array.isArray(entries))continue;
      for(var i=0;i<entries.length;i++){
        var raw=rawKey(entries[i]);
        if(typeof raw!=='string'||raw.indexOf('/')<0)continue;
        addAlias(bundle,raw,basename(raw,pathUtils));
      }
    }

    // Image manifests commonly namespace a standalone texture with the bundle name while scene
    // code uses its logical name. Publish both aliases before Assets.loadBundle starts so they
    // resolve to one loader promise and one Texture object, including asynchronous native loads.
    var imageSections=['img','img_webp'];
    var prefixes=[];
    var cfgName=typeof cfg.name==='string'?cfg.name:'';
    var baseName=basename(cfg.base_url||'',pathUtils);
    if(cfgName)prefixes.push(cfgName+'_');
    if(baseName&&baseName!==cfgName)prefixes.push(baseName+'_');
    for(var is=0;is<imageSections.length;is++){
      var images=list[imageSections[is]];
      if(!Array.isArray(images))continue;
      for(var j=0;j<images.length;j++){
        var imageRaw=rawKey(images[j]);
        if(typeof imageRaw!=='string')continue;
        var imageKey=basename(imageRaw,pathUtils);
        selectImageVariant(bundle,imageKey);
        for(var p=0;p<prefixes.length;p++){
          var prefix=prefixes[p];
          if(imageKey.indexOf(prefix)!==0||imageKey.length<=prefix.length)continue;
          addAlias(bundle,imageKey.substring(prefix.length),imageKey);
          break;
        }
      }
    }
    return bundle;
  }

  return {expandBundleAliases:expandBundleAliases};
});
