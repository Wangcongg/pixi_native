// Native texture decoding and bitmap-font cache lifetime, independent of game frameworks.
(function(g){
'use strict';
var P=g.PIXI;if(!P||!P.Assets)throw new Error('native_assets requires Pixi Assets');
P.loadTextures.config.preferWorkers=false;
P.loadTextures.config.preferCreateImageBitmap=false;
P.extensions.add({
  name:'native-ktx2',extension:{type:P.ExtensionType.LoadParser,priority:P.LoaderParserPriority.High},
  test:function(url){return /\.ktx2(?:[?#]|$)/i.test(url);},
  load:function(url,asset,loader){return P.loadTextures.load(url,asset,loader);},
  unload:function(texture){return P.loadTextures.unload(texture);}
});
var guard=g.__nativeBitmapFontAsset,parser=P.loadBitmapFont,loader=P.Assets.loader;
if(!guard||!parser||!loader)throw new Error('native bitmap-font contract unavailable');
var parse=parser.parse;
parser.parse=async function(asset,data,sourceLoader){
  var font=await parse.call(this,asset,data,sourceLoader);
  if(!font||!guard.rememberReadyFace(P,[data.src],font))throw new Error('Bitmap font did not register: '+data.src);
  return font;
};
var load=loader.load,pending=Object.create(null);
loader.load=function(sources){
  var list=Array.isArray(sources)?sources:[sources],fonts=[];
  for(var i=0;i<list.length;i++){
    var item=list[i],url=typeof item==='string'?item:item&&item.src;
    if(!guard.isFontUrl(url))continue;
    var urls=[url,P.utils.path.toAbsolute(url)];
    var key=urls[1];
    if(!pending[key]&&!guard.rememberReadyFace(P,urls,null))guard.evictStaleFont(P,urls);
    pending[key]=(pending[key]||0)+1;fonts.push(key);
  }
  function done(){for(var f=0;f<fonts.length;f++){var key=fonts[f];if(--pending[key]===0)delete pending[key];}}
  try{return Promise.resolve(load.apply(this,arguments)).then(function(value){done();return value;},function(error){done();throw error;});}
  catch(error){done();throw error;}
};
})(globalThis);
