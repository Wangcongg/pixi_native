    // ---- SG_PARENT_SIDECAR_RECEIPT_BEGIN ----
    // Numeric parent sidecar for scheduling/visibility decisions. Structural APIs update it in
    // O(1); nodes built wholly by the native FGUI factory are resolved once through the read-only
    // hierarchy bridge and cached. Rendering code therefore no longer walks DisplayObject.parent.
    var __nativeParents=new Int32Array(32768);for(var __npi=0;__npi<__nativeParents.length;__npi++)__nativeParents[__npi]=-2;__nativeParents[0]=-1;
    var __hostSgAlloc=globalThis.__sgAlloc,__hostSgAllocBatch=globalThis.__sgAllocBatch,__hostSgCommands=globalThis.__sgCommands,__hostSgCommandsV2=globalThis.__sgCommandsV2;
    var __hostSgSetParent=globalThis.__sgSetParent,__hostSgRemove=globalThis.__sgRemove,__hostSgFree=globalThis.__sgFree;
    if(typeof __hostSgAlloc==='function')globalThis.__sgAlloc=function(){var id=__hostSgAlloc();if(id>0)__nativeParents[id]=-1;return id;};
    if(typeof __hostSgAllocBatch==='function')globalThis.__sgAllocBatch=function(out){var n=__hostSgAllocBatch(out)|0;for(var i=0;i<n;i++){var id=out[i]|0;if(id>0)__nativeParents[id]=-1;}return n;};
    __sgApplyConfirmedCommands=function(commands,count,stride){stride=stride|0;if(stride!==8)stride=4;count=Math.max(0,count|0);for(var i=0;i<count;i++){var at=i*stride,op=commands[at]|0,child=commands[at+1]|0;if(child<=0||child>=32768)continue;if(op===1)__nativeParents[child]=commands[at+2]|0;else if(op===2||op===3)__nativeParents[child]=-1;}};
    var __sgSidecarBatchActive=function(){return typeof globalThis.__sgBatchActive==='function'&&globalThis.__sgBatchActive();};
    var __sgImmediateConfirmed=function(out,child,expectedParent){
      if(typeof out==='number')return __sgReceiptSucceeded(out);
      // Existing hosts return undefined for direct mutations.  Confirm through the authoritative
      // hierarchy before publishing the sidecar; a rejected cycle/before constraint must never
      // become a JS-only parent edge.  Production hosts always expose this read-only bridge.
      if(out===undefined&&typeof globalThis.__sgNativeParent==='function')try{return (__sgNativeParent(child)|0)===(expectedParent|0);}catch(_parentReadError){}
      return false;
    };
    if(typeof __hostSgSetParent==='function')globalThis.__sgSetParent=function(child,parent,before){child=child|0;parent=parent|0;var batched=__sgSidecarBatchActive(),out=__hostSgSetParent(child,parent,before===undefined?-1:before);if(!batched&&child>0&&child<32768&&__sgImmediateConfirmed(out,child,parent))__nativeParents[child]=parent;return out;};
    if(typeof __hostSgRemove==='function')globalThis.__sgRemove=function(child){child=child|0;var batched=__sgSidecarBatchActive(),out=__hostSgRemove(child);if(!batched&&child>0&&child<32768&&__sgImmediateConfirmed(out,child,-1))__nativeParents[child]=-1;return out;};
    if(typeof __hostSgFree==='function')globalThis.__sgFree=function(child){child=child|0;var batched=__sgSidecarBatchActive(),out=__hostSgFree(child);if(!batched&&child>0&&child<32768&&__sgImmediateConfirmed(out,child,-1))__nativeParents[child]=-1;return out;};
    if(typeof __hostSgCommands==='function')globalThis.__sgCommands=function(cmd,words){var queued=Math.min(cmd&&cmd.length||0,Math.max(0,words|0))>>2,out=__hostSgCommands(cmd,words);if(__sgCommandDispatchDepth===0){var receipt=__sgLegacyReceipt(out,queued);if(receipt.appliedPrefix)__sgApplyConfirmedCommands(cmd,receipt.appliedPrefix,4);}return out;};
    if(typeof __hostSgCommandsV2==='function')globalThis.__sgCommandsV2=function(cmd,words,receipts){var queued=(Math.min(cmd&&cmd.length||0,Math.max(0,words|0))/8)|0,out=__hostSgCommandsV2(cmd,words,receipts);if(__sgCommandDispatchDepth===0){var receipt=__sgV2Receipt(out,receipts,queued);if(receipt.appliedPrefix)__sgApplyConfirmedCommands(cmd,receipt.appliedPrefix,8);}return out;};
    globalThis.__sgNativeParentHandles=__nativeParents;
