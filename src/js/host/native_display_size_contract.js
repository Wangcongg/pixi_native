// Shared Pixi display-size semantics for lightweight native display objects.
//
// Graphics geometry is stored in local coordinates. Setting DisplayObject.width/height must
// therefore update the corresponding transform scale; changing cached local bounds would leave
// the already-uploaded vertices at their old size while making subsequent size checks lie.
(function(root,factory){
  var api=factory();
  if(typeof module==='object'&&module.exports)module.exports=api;
  if(root)root.__nativeDisplaySizeContract=api;
})(typeof globalThis!=='undefined'?globalThis:this,function(){
  'use strict';
  function finite(value,fallback){
    value=Number(value);
    return isFinite(value)?value:fallback;
  }
  function extent(scale,localExtent){
    return Math.abs(finite(scale,1))*Math.max(0,finite(localExtent,0));
  }
  function scaleForExtent(currentScale,localExtent,requestedExtent){
    var sign=finite(currentScale,1)<0?-1:1;
    var local=Math.max(0,finite(localExtent,0));
    var requested=Math.max(0,finite(requestedExtent,0));
    return local>0?sign*requested/local:sign;
  }
  function installLocalBoundsAccessors(prototype){
    Object.defineProperties(prototype,{
      width:{configurable:true,get:function(){return extent(this.scale&&this.scale.x,this._nativeWidth);},set:function(v){this.scale.x=scaleForExtent(this.scale&&this.scale.x,this._nativeWidth,v);this._width=Math.max(0,Number(v)||0);}},
      height:{configurable:true,get:function(){return extent(this.scale&&this.scale.y,this._nativeHeight);},set:function(v){this.scale.y=scaleForExtent(this.scale&&this.scale.y,this._nativeHeight,v);this._height=Math.max(0,Number(v)||0);}}
    });
    return prototype;
  }
  return {extent:extent,scaleForExtent:scaleForExtent,installLocalBoundsAccessors:installLocalBoundsAccessors};
});
