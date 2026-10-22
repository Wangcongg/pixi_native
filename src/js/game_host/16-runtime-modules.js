// Required native rendering contract and project entry. No game framework or login gate.
globalThis.__nativeQuadRefBatch=function(ids,n){
  enc.begin(OP.QuadRefBatch);enc.w(n);
  for(var i=0;i<n;i++)enc.w(ids[i]);
  enc.end();
};
var modules=[
  'native_boot_contract','native_frame_seal','native_filters','native_mask_contract',
  'native_filter_payload_contract','native_display_size_contract','native_asset_alias',
  'native_texture_variant','native_bitmap_font_asset','native_text_style_contract',
  'native_text_resolution_contract','text_layout_contract','native_slot_lease_contract',
  'native_scenegraph','native_node_proxy','native_assets'
];
['__sgAlloc','__sgFree','__sgCompute','__sgHit','__nativePresent'].forEach(function(name){
  if(typeof globalThis[name]!=='function')throw new Error('Required native capability missing: '+name);
});
modules.forEach(function(name){__loadScriptByLocal('host/'+name+'.js');});
__loadScriptByLocal('pixi8-native.js');
globalThis.PIXI8=globalThis.PixiNative8;
if(globalThis.__PIXI_NATIVE_PROJECT__.features.spine){
  __loadScriptByLocal('spine-pixi-v7.js');
  if(!globalThis.spine)throw new Error('Spine runtime did not register');
  PIXI.spine=globalThis.spine;
  if(typeof globalThis.__nativeSpineRuntimeReady==='function')__nativeSpineRuntimeReady();
}
__loadScriptByLocal('app/main.js');
if(!globalThis.PixiNativeApp||typeof globalThis.PixiNativeApp.start!=='function')
  throw new Error('Entry must register PixiNativeApp.start(context)');
Promise.resolve(globalThis.PixiNativeApp.start({PIXI:globalThis.__PIXI_NATIVE_PROJECT__.pixiVersion===8?PIXI8:PIXI,PIXI8:PIXI8,
  config:globalThis.__PIXI_NATIVE_PROJECT__})).then(function(app){
    if(!app||!app.stage||!app.renderer)throw new Error('Entry must return a Pixi Application');
    globalThis.__PIXI_APP__=app;
    app.view.isConnected=true;
    __log('[pixi-native] project ready: '+globalThis.__PIXI_NATIVE_PROJECT__.app.name);
    // The native splash overlay is a host concern; it never selects an account or scene.
    // The platform dismisses its splash after the first presented frame.
  }).catch(function(error){
    __log('[pixi-native] startup failed: '+(error.stack||error.message||error));
    if(typeof __showReload==='function')__showReload(String(error.message||error));
    throw error;
  });
})();
