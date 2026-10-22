    // ---- SG_MUTATION_BATCH_CORE_BEGIN ----
    var __sgStructureVersion=0;globalThis.__sgStructVer=0;
    // Native V2 receipt ABI (one receipt per 8-word command row): 0=unchecked,
    // 1=Applied, 2=Idempotent, 3=ManagedFreeDeferred, negatives are terminal failures.
    // Keep parsing and sidecar publication in JS so legacy hosts can coexist during rollout.
    var __sgCommandDispatchDepth=0,__sgApplyConfirmedCommands=function(){},__sgCommandGenerationResolver=
      (typeof U32!=='undefined'&&typeof NGB==='number')?function(id){return U32[NGB+(id|0)]>>>0;}:null;
    var __sgReceiptSucceeded=function(code){code=Number(code);return code===1||code===2||code===3;};
    var __sgReceiptName=function(code){return code===-1?'InvalidOrDead':code===-2?'GenerationMismatch':code===-3?'Cycle':code===-4?'BeforeNotSibling':('receipt-'+code);};
    var __sgLegacyReceipt=function(raw,queued){
      var valid=typeof raw==='number'&&isFinite(raw)&&Math.floor(raw)===raw&&raw>=0;
      // The old number is a count, not a prefix receipt. A partial count must never advance the
      // sidecar because a skipped middle command and a stopped prefix are indistinguishable.
      return {ok:valid&&raw===queued,appliedPrefix:valid&&raw===queued?queued:0,checked:valid?raw:0,
        faultIndex:valid&&raw<queued?raw:0,code:valid?'legacy-partial':'legacy-invalid'};
    };
    var __sgV2Receipt=function(raw,receipts,queued){
      var checked=Number(raw),valid=isFinite(checked)&&Math.floor(checked)===checked&&checked>=0&&checked<=queued;
      if(!valid)return {ok:false,appliedPrefix:0,checked:0,faultIndex:0,code:'invalid-checked'};
      var prefix=0,faultIndex=-1,faultCode=0;
      for(var i=0;i<checked;i++){
        var code=receipts[i]|0;
        if(__sgReceiptSucceeded(code)&&faultIndex<0){prefix++;continue;}
        faultIndex=i;faultCode=code;break;
      }
      if(faultIndex<0&&checked<queued){faultIndex=checked;faultCode=0;}
      var ok=faultIndex<0&&checked===queued&&prefix===queued;
      return {ok:ok,appliedPrefix:prefix,checked:checked,faultIndex:faultIndex<0?queued:faultIndex,
        code:faultCode<0?__sgReceiptName(faultCode):(faultCode===0?'unchecked':'invalid-positive-order')};
    };
    globalThis.__sgSetCommandGenerationResolver=function(resolver){
      if(resolver!==null&&typeof resolver!=='function')throw new TypeError('scenegraph generation resolver must be a function or null');
      var previous=__sgCommandGenerationResolver;__sgCommandGenerationResolver=resolver;return previous;
    };
    // V2 must use an explicitly installed shared-memory resolver.  Falling back to a native
    // function here would add three synchronous JSC -> C++ reads per hierarchy mutation and erase
    // the point of batching; hosts publish their generation plane through the resolver hook.
    var __sgHasCommandGenerationResolver=function(){return typeof __sgCommandGenerationResolver==='function';};
    var __sgCommandGeneration=function(id){
      id=id|0;if(id<=0)return 0;var resolver=__sgCommandGenerationResolver;
      var generation=Number(resolver(id));if(!isFinite(generation)||generation<=0||generation>4294967295||Math.floor(generation)!==generation)throw new Error('scenegraph generation unavailable id='+id);
      return generation>>>0;
    };
    (function(){
      var _sal=globalThis.__sgAlloc, _sfr=globalThis.__sgFree, _ssp=globalThis.__sgSetParent, _srm=globalThis.__sgRemove;
      var reserve=new Float32Array(256),reserveN=0,cmd=new Int32Array(4096),cmdGenerations=new Uint32Array(3072),cmdN=0,cmdGenerationRows=0,batchDepth=0,
          v2cmd=new Int32Array(8192),v2receipts=new Int32Array(1024),batchFault=null,batchFaultNotified=false;
      // A semantic UI operation (for example populating a store panel) can create many separate
      // FGUI components.  Each component already has an inner construction batch, but without an
      // outer transaction every inner end still crosses JSC -> C++ separately.  Keep lightweight
      // counters here so an opt-in page profiler can prove the number of crossings removed without
      // wrapping every scenegraph function on the hot path.
      var mutationStats=globalThis.__sgMutationBatchStats||(globalThis.__sgMutationBatchStats={begins:0,ends:0,flushes:0,commands:0,avoidedCrossings:0,readBarriers:0,maxDepth:0,maxCommands:0,faults:0,stoppedCommands:0,v2Flushes:0,legacyFlushes:0});
      var coordinatorHook=null;
      globalThis.__sgSetTransactionCoordinatorHook=function(hook){if(hook!==null&&typeof hook!=='function')throw new TypeError('scenegraph transaction coordinator hook must be a function or null');var previous=coordinatorHook;coordinatorHook=hook;return previous;};
      var faultSnapshot=function(){return {depth:batchDepth|0,pending:cmdN>>2,fault:true,message:String(batchFault&&batchFault.message||batchFault||''),faultIndex:(batchFault&&batchFault.faultIndex)|0,code:String(batchFault&&batchFault.code||'')};};
      var notifyCoordinator=function(){if(batchFaultNotified||!batchFault)return;batchFaultNotified=true;if(typeof coordinatorHook==='function')try{coordinatorHook(batchFault,faultSnapshot());}catch(e){mutationStats.coordinatorFaults=(mutationStats.coordinatorFaults||0)+1;try{(globalThis.__log||console.log)('[sg-batch] transaction coordinator hook failed '+String(e&&e.message||e));}catch(_logError){}}};
      var setBatchFault=function(message,info,cause){
        if(batchFault)return batchFault;
        var error=cause&&typeof cause.message==='string'?cause:new Error(message);
        try{error.__sgBatchFault=true;error.faultIndex=info&&info.faultIndex|0;error.code=String(info&&info.code||'bridge-error');error.appliedPrefix=info&&info.appliedPrefix|0;}catch(_annotateError){}
        batchFault=error;mutationStats.faults=(mutationStats.faults||0)+1;return error;
      };
      var flushCmd=function(){
        if(batchFault)throw batchFault;if(!cmdN)return 0;
        var queued=cmdN>>2,info=null,raw=0,fallbackApplied=0,mode='fallback',dispatchError=null;
        try{
          __sgCommandDispatchDepth++;
          if(typeof globalThis.__sgCommandsV2==='function'&&cmdGenerationRows===queued){
            mode='v2';
            // Generations were captured with each enqueue. Reading them here would permit an ABA:
            // a synchronous native free/reuse between enqueue and flush could lend an old command
            // the replacement owner's generation.
            for(var vi=0;vi<queued;vi++){var si=vi*4,di=vi*8,gi=vi*3;v2cmd[di]=cmd[si];v2cmd[di+1]=cmd[si+1];v2cmd[di+2]=cmd[si+2];v2cmd[di+3]=cmd[si+3];v2cmd[di+4]=cmdGenerations[gi];v2cmd[di+5]=cmdGenerations[gi+1];v2cmd[di+6]=cmdGenerations[gi+2];v2cmd[di+7]=0;v2receipts[vi]=0;}
            raw=__sgCommandsV2(v2cmd,queued*8,v2receipts);info=__sgV2Receipt(raw,v2receipts,queued);
          }else if(typeof globalThis.__sgCommands==='function'){
            mode='legacy';raw=__sgCommands(cmd,cmdN);info=__sgLegacyReceipt(raw,queued);
          }else{
            for(var i=0;i+3<cmdN;i+=4){
              var fallbackOp=cmd[i],a=cmd[i+1],fallbackExpected=fallbackOp===1?(cmd[i+2]|0):-1,fallbackResult;
              if(fallbackOp===1)fallbackResult=_ssp(a,cmd[i+2],cmd[i+3]);else if(fallbackOp===2)fallbackResult=_srm(a);else if(fallbackOp===3)fallbackResult=_sfr(a);else throw new Error('invalid scenegraph fallback op '+fallbackOp);
              var fallbackOK=__sgReceiptSucceeded(fallbackResult);
              if(fallbackResult===undefined&&typeof globalThis.__sgNativeParent==='function')try{fallbackOK=(__sgNativeParent(a)|0)===fallbackExpected;}catch(_fallbackParentReadError){}
              if(!fallbackOK){var fallbackCode=typeof fallbackResult==='number'?(fallbackResult|0):0;info={ok:false,appliedPrefix:fallbackApplied,checked:fallbackApplied+1,faultIndex:fallbackApplied,code:fallbackCode<0?__sgReceiptName(fallbackCode):(fallbackCode===0?'fallback-unconfirmed':'invalid-positive-order')};throw new Error('scenegraph fallback command rejected index='+fallbackApplied+' code='+info.code);}
              fallbackApplied++;
            }
            info={ok:fallbackApplied===queued,appliedPrefix:fallbackApplied,checked:fallbackApplied,faultIndex:fallbackApplied,code:'fallback'};
          }
        }catch(e){dispatchError=e;info=info||{ok:false,appliedPrefix:fallbackApplied,checked:fallbackApplied,faultIndex:fallbackApplied,code:'bridge-throw'};}
        finally{__sgCommandDispatchDepth--;}
        var confirmed=Math.max(0,Math.min(queued,info&&info.appliedPrefix|0));
        if(confirmed)__sgApplyConfirmedCommands(cmd,confirmed,4);
        // Failure-only evidence for lifecycle probes. Keep the captured generations intact: do
        // not "repair" an ABA rejection by borrowing the replacement owner's current generation.
        if(mode==='v2'&&info&&!info.ok){
          var faultRow=info.faultIndex|0;
          if(faultRow>=0&&faultRow<queued)try{
            var fi=faultRow*8,faultIds=[v2cmd[fi+1],v2cmd[fi+2],v2cmd[fi+3]],actual=[];
            for(var fj=0;fj<faultIds.length;fj++){try{actual.push(__sgCommandGeneration(faultIds[fj]));}catch(_generationError){actual.push(-1);}}
            var evidence={op:v2cmd[fi],ids:faultIds,expected:[v2cmd[fi+4],v2cmd[fi+5],v2cmd[fi+6]],actual:actual,index:faultRow,queued:queued,depth:batchDepth};
            mutationStats.lastFailure=evidence;
            (globalThis.__log||console.log)('[sg-batch-fault] '+JSON.stringify(evidence));
          }catch(_diagnosticError){}
        }
        mutationStats.flushes++;mutationStats.commands+=confirmed;mutationStats.avoidedCrossings+=Math.max(0,confirmed-1);mutationStats.stoppedCommands+=(queued-confirmed);if(mode==='v2')mutationStats.v2Flushes++;else if(mode==='legacy')mutationStats.legacyFlushes++;if(queued>mutationStats.maxCommands)mutationStats.maxCommands=queued;cmdN=0;cmdGenerationRows=0;
        if(dispatchError||!info||!info.ok)throw setBatchFault('scenegraph command batch failed mode='+mode+' checked='+(info&&info.checked|0)+'/'+queued+' index='+(info&&info.faultIndex|0)+' code='+String(info&&info.code||'unknown'),info,dispatchError);
        return confirmed;
      };
      var pushCmd=function(op,a,b,c){
        if(batchFault)throw batchFault;if(cmdN+4>cmd.length)flushCmd();if(batchFault)throw batchFault;
        if(op===1&&b<0&&globalThis.__NATIVE_TEST_POPUP)try{(globalThis.__log||console.log)('[sg-batch-invalid-parent] child='+a+' parent='+b+' before='+c+' '+String(new Error('enqueue invalid parent').stack||''));}catch(_enqueueDiagnosticError){}
        var row=cmdN>>2,capture=typeof globalThis.__sgCommandsV2==='function'&&__sgHasCommandGenerationResolver();
        if(capture)try{var gi=row*3;cmdGenerations[gi]=__sgCommandGeneration(a);cmdGenerations[gi+1]=__sgCommandGeneration(b);cmdGenerations[gi+2]=__sgCommandGeneration(c);cmdGenerationRows++;}
        catch(e){throw setBatchFault('scenegraph command generation capture failed index='+row,{faultIndex:row,appliedPrefix:0,code:'generation-capture'},e);}
        cmd[cmdN++]=op;cmd[cmdN++]=a|0;cmd[cmdN++]=b|0;cmd[cmdN++]=c|0;
      };
      globalThis.__sgBatchBegin=function(){if(batchDepth===0){cmdN=0;cmdGenerationRows=0;batchFault=null;batchFaultNotified=false;}else if(batchFault)throw batchFault;batchDepth++;mutationStats.begins++;if(batchDepth>mutationStats.maxDepth)mutationStats.maxDepth=batchDepth;return batchDepth;};
      globalThis.__sgBatchEnd=function(){var result=0;if(batchDepth>0){batchDepth--;mutationStats.ends++;}if(batchDepth){if(batchFault)throw batchFault;return result;}if(!batchFault)try{result=flushCmd();}catch(e){if(!batchFault)setBatchFault('scenegraph command batch end failed',{faultIndex:0,appliedPrefix:0,code:'batch-end-throw'},e);}if(batchFault){cmdN=0;notifyCoordinator();throw batchFault;}return result;};
      globalThis.__sgBatchFlush=function(){try{return flushCmd();}catch(e){if(!batchDepth)notifyCoordinator();throw e;}};
      globalThis.__sgBatchActive=function(){return batchDepth>0;};
      // Native hierarchy reads normally use the eagerly maintained numeric parent sidecar.  A
      // rare caller that must inspect the authoritative C++ child list while a semantic batch is
      // active can explicitly request this barrier; ordinary construction therefore remains one
      // crossing while correctness-sensitive readers have a safe escape hatch.
      globalThis.__sgBatchReadBarrier=function(){if(batchDepth&&batchFault)throw batchFault;if(batchDepth&&cmdN){mutationStats.readBarriers++;return flushCmd();}return 0;};
      globalThis.__sgBatchSnapshot=function(){return {begins:mutationStats.begins|0,ends:mutationStats.ends|0,flushes:mutationStats.flushes|0,commands:mutationStats.commands|0,avoidedCrossings:mutationStats.avoidedCrossings|0,readBarriers:mutationStats.readBarriers|0,maxDepth:mutationStats.maxDepth|0,maxCommands:mutationStats.maxCommands|0,depth:batchDepth|0,pending:cmdN>>2,fault:!!batchFault,faultMessage:String(batchFault&&batchFault.message||''),faultIndex:(batchFault&&batchFault.faultIndex)|0,faultCode:String(batchFault&&batchFault.code||'')};};
      if(typeof globalThis.__nativeFrameSealRegister==='function')globalThis.__nativeFrameSealRegister('scenegraph',{
        snapshot:function(root){var batch=globalThis.__sgBatchSnapshot();root=root|0;return {
          structureVersion:globalThis.__sgStructVer>>>0,mutationEpoch:U32[EPB]>>>0,ackedEpoch:U32[EPB+1]>>>0,
          rootDebt:root>0&&root<NCAP?Number(SGB[DBB+root])||0:0,batchDepth:batch.depth|0,
          batchPending:batch.pending|0,batchFault:!!batch.fault
        };},
        validate:function(state){var bad=[];if(state.rootDebt>0.5)bad.push('root-debt='+state.rootDebt);
          if(state.mutationEpoch!==state.ackedEpoch)bad.push('epoch='+state.mutationEpoch+'/'+state.ackedEpoch);
          if(state.batchDepth||state.batchPending)bad.push('batch-open='+state.batchDepth+'/'+state.batchPending);
          if(state.batchFault)bad.push('batch-fault');return bad;}
      });
      globalThis.__sgAlloc=function(){
        // 只推进内部版本(present 原子重试要看它),**不镜像给 __sgStructVer**:新分配的节点还没
        // 有父子关系,任何既有 children 快照都不受影响。构建密集帧里 alloc 极多,镜像出去只会
        // 让代理层的子节点快照缓存被无谓地整体作废、每次访问都重新跨桥取一遍。
        ++__sgStructureVersion;globalThis.__sgDirty=1;
        if(typeof globalThis.__sgAllocBatch==='function'){
          if(reserveN<=0)reserveN=__sgAllocBatch(reserve)|0;
          if(reserveN>0)return reserve[--reserveN]|0;
        }
        return _sal.apply(this,arguments);
      };
      globalThis.__sgFree=function(a){globalThis.__sgStructVer=++__sgStructureVersion;globalThis.__sgDirty=1;if(batchDepth){pushCmd(3,a,0,0);return;}if(__sgHasCommandGenerationResolver())return _sfr(a,__sgCommandGeneration(a));return _sfr(a);};
      globalThis.__sgSetParent=function(a,b,c){globalThis.__sgStructVer=++__sgStructureVersion;globalThis.__sgDirty=1;c=c===undefined?-1:c|0;if(batchDepth){pushCmd(1,a,b,c);return;}if(__sgHasCommandGenerationResolver())return _ssp(a,b,c,__sgCommandGeneration(a),__sgCommandGeneration(b),__sgCommandGeneration(c));return _ssp(a,b,c);};
      globalThis.__sgRemove=function(a){globalThis.__sgStructVer=++__sgStructureVersion;globalThis.__sgDirty=1;if(batchDepth){pushCmd(2,a,0,0);return;}if(__sgHasCommandGenerationResolver())return _srm(a,__sgCommandGeneration(a));return _srm(a);};
