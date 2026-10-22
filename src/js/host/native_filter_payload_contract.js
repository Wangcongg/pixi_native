// Detect native-filter payloads that JavaScript can mutate in place without crossing an
// observable property setter (most importantly ColorMatrixFilter.matrix TypedArray indices).
// Keep the snapshot logic renderer-independent so it can be regression-tested in Node.
(function(root,factory){
  var api=factory();
  if(typeof module==='object'&&module.exports)module.exports=api;
  if(root)root.__nativeFilterPayloadContract=api;
})(typeof globalThis!=='undefined'?globalThis:this,function(){
  'use strict';
  function appendSequence(out,value,limit){
    if(!value||typeof value.length!=='number')return false;
    var length=Math.max(0,Math.min(limit===undefined?value.length:limit,value.length));
    out.push(length);
    for(var i=0;i<length;i++)out.push(Number(value[i])||0);
    return true;
  }
  function appendPointSequence(out,value){
    if(!value||typeof value.length!=='number')return false;
    return appendSequence(out,value,2);
  }
  // Return null when every encoded field is observable through the normal scalar/object hooks.
  // The returned flat number array is deliberately compared by value: retaining the source
  // TypedArray would make an in-place mutation change both the live value and the "snapshot".
  function snapshot(filters,kindOf){
    if(!filters||!filters.length||typeof kindOf!=='function')return null;
    var out=[],tracked=false;
    for(var i=0;i<filters.length;i++){
      var filter=filters[i];
      if(!filter||filter.enabled===false)continue;
      var kind=kindOf(filter),uniforms=filter.uniforms||{};
      if(kind===3&&filter.matrix&&filter.matrix.length>=20){
        out.push(i,kind);appendSequence(out,filter.matrix,20);tracked=true;
      }else if(kind===4&&uniforms.color&&typeof uniforms.color.length==='number'){
        out.push(i,kind);appendSequence(out,uniforms.color,3);tracked=true;
      }else if(kind===5){
        var tint=filter._tintFilter&&filter._tintFilter.uniforms&&filter._tintFilter.uniforms.color;
        var blur=filter._blurFilter,kernels=blur&&(blur.kernels||blur._kernels),pixelSize=blur&&(blur.pixelSize||blur._pixelSize);
        if((tint&&typeof tint.length==='number')||(kernels&&typeof kernels.length==='number')||(pixelSize&&typeof pixelSize.length==='number')){
          out.push(i,kind);appendSequence(out,tint||[],3);appendSequence(out,kernels||[],8);appendPointSequence(out,pixelSize||[]);tracked=true;
        }
      }else if(kind===9){
        var scale=filter.scale||uniforms.scale;
        if(scale&&typeof scale.length==='number'){
          out.push(i,kind);appendPointSequence(out,scale);tracked=true;
        }
      }else if(kind===10){
        var kawaseKernels=filter.kernels||filter._kernels,kawasePixels=filter.pixelSize||filter._pixelSize;
        if((kawaseKernels&&typeof kawaseKernels.length==='number')||(kawasePixels&&typeof kawasePixels.length==='number')){
          out.push(i,kind);appendSequence(out,kawaseKernels||[],12);appendPointSequence(out,kawasePixels||[]);tracked=true;
        }
      }
    }
    return tracked?out:null;
  }
  function equal(left,right){
    if(left===right)return true;
    if(!left||!right||left.length!==right.length)return false;
    for(var i=0;i<left.length;i++)if(left[i]!==right[i])return false;
    return true;
  }
  return {snapshot:snapshot,equal:equal};
});
