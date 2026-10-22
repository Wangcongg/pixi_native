// Shared Native text raster-resolution contract.
//
// The scene graph uses logical pixels, but CoreText raster surfaces may need more backing pixels
// than the main drawable (for example, a Retina Mac window whose game drawable intentionally stays
// at one pixel per AppKit point). Pixi Text normally follows renderer.resolution automatically;
// Native text needs the equivalent host-provided backing scale without changing scene coordinates.
(function(root,factory){
'use strict';
var api=factory();
if(typeof module==='object'&&module&&module.exports)module.exports=api;
if(root)root.__nativeTextResolutionContract=api;
})(typeof globalThis!=='undefined'?globalThis:this,function(){
'use strict';

function positive(value){
  value=Number(value);
  return isFinite(value)&&value>0?value:0;
}

// NativeTextSystem supports 1x..3x surfaces on every host. Keep cache keys and JS state inside
// that same range instead of asking the host to silently clamp a different value.
function clamp(value){
  value=positive(value)||1;
  return Math.max(1,Math.min(3,value));
}

function rendererResolution(root){
  if(!root)return 0;
  var app=root.__PIXI_APP__||root.__pixiApp;
  return positive(app&&app.renderer&&app.renderer.resolution);
}

function automatic(root,pixi){
  // A host hint is authoritative because a Native drawable may deliberately use logical 1x
  // coordinates while its independently sampled text textures still need Retina backing pixels.
  var value=positive(root&&root.__nativeTextRasterScale)||rendererResolution(root)||
    positive(root&&root.__screenDPR)||positive(pixi&&pixi.settings&&pixi.settings.RESOLUTION)||1;
  return clamp(value);
}

function initial(root,pixi,textType){
  var configured=positive(textType&&textType.defaultResolution);
  return configured?clamp(configured):automatic(root,pixi);
}

return {clamp:clamp,automatic:automatic,initial:initial};
});
