// Shared mask metadata contract for the JavaScript scene facade and the native batcher.
// Keep this module data-only so the classification can be regression-tested without a renderer.
(function(root,factory){
  var api=factory();
  if(typeof module==='object'&&module.exports)module.exports=api;
  if(root)root.__nativeMaskContract=api;
})(typeof globalThis!=='undefined'?globalThis:this,function(){
  'use strict';
  var GEOMETRY_MASK=64;
  // Some native-only render facades expose exact triangles solely to preserve the public
  // "make this object interactive" contract. Their JavaScript/H5 counterpart is a Container
  // without containsPoint, so those synthetic triangles must not become an inherited witness
  // merely because an ancestor is interactive. They remain valid when the facade itself is
  // static/dynamic. Keep this distinction in shared hit metadata instead of naming renderers.
  var SELF_INTERACTION_MASK=256;
  // Native renderers may collapse a DisplayObject's visual descendants onto the owner's single
  // SceneGraph node.  The geometry is still semantically owned by those descendants: a browser
  // EventBoundary must not see it when `interactiveChildren=false`, even though Native has no
  // child nodes left to prune.  Keep that provenance separate from SELF_INTERACTION_MASK; the
  // latter describes synthetic self geometry, while this bit describes compressed child geometry.
  var DESCENDANT_GEOMETRY_MASK=512;
  // A Native-FGUI GGraph facade can expose a NativeGraphicsProxy while its triangles still live
  // exclusively in C++.  An empty `_commands` array in that state is not an empty Graphics; it
  // means JavaScript has never taken ownership of the authored geometry.  Keep this distinction
  // data-only so mask dispatch and regression tests use one ownership rule.
  function usesRetainedGraphics(mask){
    return !!(mask&&mask.__nativeGraphicsProxy&&mask.__nativeFguiRetainedGraphDescriptor&&
      (!mask._commands||mask._commands.length===0));
  }
  function hasRuntimeProxyGeometry(mask){
    return !!(mask&&mask.__nativeGraphicsProxy&&!usesRetainedGraphics(mask));
  }
  function hasGeometry(mask){
    return !!(mask&&(mask.__nativeGeometryMask||mask.__nativeProxyGeometryMask||
      mask.__nativeRetainedGeometryMask));
  }
  function hitFlags(object,hasHitArea,hasContainsPoint){
    if(!object)return 0;
    return (hasHitArea?1:0)|(hasContainsPoint?2:0)|((object.isMask||object.__maskOf)?4:0)|
      (object.__nativeAlphaMask?8:0)|(object.__nativeEllipseMask?16:0)|
      (object.__nativeHitGeometry?32:0)|(hasGeometry(object)?GEOMETRY_MASK:0)|
      (object.__nativeFguiOverflowHit?128:0)|
      (object.__nativeHitGeometrySelfOnly?SELF_INTERACTION_MASK:0)|
      (object.__nativeHitGeometryFromChildren?DESCENDANT_GEOMETRY_MASK:0)|
      (object.__nativeFguiMaskFlags||0)|(object.__nativeCachedTextureActive?1024:0);
  }
  return {GEOMETRY_MASK:GEOMETRY_MASK,SELF_INTERACTION_MASK:SELF_INTERACTION_MASK,
    DESCENDANT_GEOMETRY_MASK:DESCENDANT_GEOMETRY_MASK,
    usesRetainedGraphics:usesRetainedGraphics,
    hasRuntimeProxyGeometry:hasRuntimeProxyGeometry,hasGeometry:hasGeometry,hitFlags:hitFlags};
});
