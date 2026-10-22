// Shared Pixi TextStyle -> native text paint contract.
//
// Pixi represents a linear text gradient as `fill: [color, ...]`.  Passing that array through
// String() produces an invalid CSS colour ("16711680,16776960"), while taking fill[0] silently
// flattens the gradient.  Keep one lossless description for the ordinary CoreText surface and the
// attributed-text span bridge.
(function(root,factory){
'use strict';
var api=factory();
if(typeof module==='object'&&module&&module.exports)module.exports=api;
if(root)root.__nativeTextStyleContract=api;
})(typeof globalThis!=='undefined'?globalThis:this,function(){
'use strict';

function colorCss(value,fallback){
  if(Array.isArray(value))value=value.length?value[0]:fallback;
  if(typeof value==='number'&&isFinite(value))return '#'+('000000'+((value>>>0)&0xffffff).toString(16)).slice(-6);
  return String(value==null?(fallback==null?'transparent':fallback):value).replace(/["<>;]/g,'');
}

function gradient(style){
  style=style||{};
  var fill=style.fill;
  if(!Array.isArray(fill)||fill.length<2)return null;
  var authoredStops=Array.isArray(style.fillGradientStops)?style.fillGradientStops:[];
  var colors=[fill[0]].concat(fill,[fill[fill.length-1]]).map(function(value){return colorCss(value,'#ffffff');});
  var stops=[0],i,n=fill.length;
  for(i=0;i<n;i++){
    var value=Number(authoredStops[i]);
    if(!isFinite(value))value=(i+1)/(n+1);
    stops.push(Math.max(0,Math.min(1,value)));
  }
  stops.push(1);
  // CGGradient requires monotonically increasing locations. Pixi's CanvasGradient contract also
  // rejects decreasing stops, so clamp malformed application data to the last valid location.
  for(i=1;i<stops.length;i++)if(stops[i]<stops[i-1])stops[i]=stops[i-1];
  return {
    solid:colorCss(fill[0],'#ffffff'),
    colors:colors.join('|'),
    stops:stops.join(','),
    type:Number(style.fillGradientType)===1?1:0
  };
}

function fill(style,fallback){
  var value=style&&style.fill,description=gradient(style);
  return {
    solid:description?description.solid:colorCss(value,fallback),
    gradient:description
  };
}

return {colorCss:colorCss,gradient:gradient,fill:fill};
});
