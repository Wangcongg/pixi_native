    // 把 Pixi 内部字段也变成 O(1) 写共享槽。部分旧业务会直接写 point._x/_y、transform._rotation，
    // 之前只能靠每帧全对象扫描兜底；实例级访问器补齐后可移除该扫描。
    //
    // 矩阵校验层（通用，与具体业务/场景无关）：业务在尺寸未就绪时算出 NaN/Infinity（除以 0 的
    // fullScreenScale、空 init_size 等）写进变换是常态。浏览器渲染器每帧从业务字段重算矩阵，
    // 业务稍后写回有效值即自愈；native 增量场景图若把毒值收进共享槽，会被 C++ 缓存进局部矩阵
    // 并沿子树传播（整棵消失、bounds/hit 失效），直到下一次本地写才醒。语义对齐做法：JS 字段
    // 照常存业务写的值（业务读回 NaN，与浏览器一致），共享槽只接受有限值——无效值不落槽
    // （上屏保持最后有效状态）；业务恢复有效值时 NaN!==v 恒真，正常写路径立即修复。
    // 报告按【槽位】去重而不是全局限次:位置/缩放/旋转是不同性质的故障,一个高频槽(如动画
    // 每帧写 rotation)会把全局配额吃光,让另一类(位置)永远不被看见——排障时这等于失明。
    var __sgNanSlots={};
    var __sgReportNaN=function(off,where){
      var slot=off===undefined?-1:(off%ST);
      if((__sgNanSlots[slot]=(__sgNanSlots[slot]||0)+1)>2)return;
      var name=slot===0||slot===1?'position':slot===2||slot===3?'scale':slot===4?'rotation':slot===5||slot===6?'pivot':slot===7||slot===8?'skew':slot===9?'alpha':('slot'+slot);
      __log('[sg-guard] 非有限变换值被拦截 '+name+'(slot='+slot+')'+(where?' @'+where:'')+'（槽保留安全值,业务字段照常存取）');
    };
    // Transform points outlive DisplayObject.destroy(): keep their semantic owner alongside the
    // numeric offset so all public/raw setters above can enforce the common slot-ownership rule.
    // 双态访问器辅助(影子消灭阶段0):__SG_SINGLE_SOURCE=1 且租约有效时读写以槽为真值源
    // (写判等 fround=存储语义,双写期影子只写不读);否则读写落暂存(影子)。
    // 租约有效=三条同时成立(语义=lease contract 的 canReadSlot,契约测试锚定):
    //   ① generation 匹配(__sgLease===owner.__sgBindGeneration) ② owner.__i 指向该槽
    //   ③ registry 仍登记该 owner(__sgObjs[id]===owner)——①与③互补:generation 拦"同对象换代",
    //   registry 拦"native 工厂旁路改登记";任一不能替代另一(评审 P1)。
    // 开关=0 时 getter/setter 走原路径,行为与现状逐位一致。
    var __ptLeaseValid=function(self,off){
      if(off===undefined||self.__sgLease===undefined)return false;
      var ow=self.__sgOwner;if(!ow||ow.__sgBindGeneration!==self.__sgLease)return false;
      var id=(off/ST)|0;return (ow.__i|0)===id&&__sgObjs[id]===ow;
    };
    var __rotLeaseValid=function(self){
      var b=self.__b;if(b===undefined||self.__sgRotLease===undefined)return false;
      var ow=self.__sgOwner;if(!ow||ow.__sgBindGeneration!==self.__sgRotLease)return false;
      var id=(b/ST)|0;return (ow.__i|0)===id&&__sgObjs[id]===ow;
    };
    var __ptSlotGet=function(self,delta){
      if(globalThis.__SG_SINGLE_SOURCE===1&&self.__sgLease!==undefined){
        if(__ptLeaseValid(self,self.__o))return SGB[self.__o+delta];
        globalThis.__sgLeaseReadFallbacks=(globalThis.__sgLeaseReadFallbacks|0)+1;
      }
      return delta===0?self.__sgRawX:self.__sgRawY;
    };
    // 开关开启时接管一切带偏移的写:租约有效→写槽;无效→写暂存并【终止】。绝不回落旧 setter——
    // 旧路径的 __sgOwnsSlot 不校验 generation,同对象同 id 重租后旧 point 会写穿新代槽(评审 P1)。
    var __ptSlotSet=function(self,delta,v){
      if(globalThis.__SG_SINGLE_SOURCE!==1)return false;
      var off=self.__o;
      if(off===undefined)return false;            // 纯未绑定:旧 setter 的暂存路径行为一致
      var ch;
      if(__ptLeaseValid(self,off)){
        if(delta===0)self.__sgRawX=v;else self.__sgRawY=v;   // 双写期镜像(只写不读)
        var w=Math.fround(v);
        if(isFinite(w)){ if(SGB[off+delta]!==w){SGB[off+delta]=w;globalThis.__sgDirty=1;__markD(off-(off%ST));if(self.__sgRO)__markRender(self.__sgRO);} }
        else __sgReportNaN(off+delta);
        return true;
      }
      // 无效租约:写暂存,槽不动(计数供审计);markRender 语义与旧 stale 分支一致(值变仍标)。
      globalThis.__sgLeaseWriteFallbacks=(globalThis.__sgLeaseWriteFallbacks|0)+1;
      ch=(delta===0?self.__sgRawX:self.__sgRawY)!==v;
      if(delta===0)self.__sgRawX=v;else self.__sgRawY=v;
      if(ch&&self.__sgRO)__markRender(self.__sgRO);
      return true;
    };
    var __instrumentPoint=function(pt,off,renderOwner,nativeOwner){
      // NativeNodeProxy 的 NativePoint 已经直接读写共享槽。再次安装 Pixi 的 _x/_y
      // 访问器会把首次捕获的旧值缓存到 __sgRawX/Y，后续 refresh 时反而把最新坐标
      // 覆盖回旧值（常见表现是 FGUI 横向列表所有子项都被写回 x=0）。
      if(!pt||(pt.owner&&pt.owner.__nativeNodeProxy))return; pt.__o=off;pt.__sgOwner=nativeOwner||null;if(renderOwner)pt.__sgRO=renderOwner;
      if(pt.__sgRawHook)return;
      var x=pt._x,y=pt._y; pt.__sgRawX=x;pt.__sgRawY=y;pt.__sgRawHook=1;
      try{
        Object.defineProperty(pt,'_x',{configurable:true,enumerable:true,get:function(){return __ptSlotGet(this,0);},set:function(v){if(__ptSlotSet(this,0,v))return;var ch=this.__sgRawX!==v;this.__sgRawX=v;if(ch&&this.__o!==undefined){var owner=this.__sgOwner;if(!__sgOwnsSlot(owner,this.__o))__sgDropStaleSlotWrite(owner,this.__o,'position/scale');else if(isFinite(v)){SGB[this.__o]=v;globalThis.__sgDirty=1;__markD(this.__o-(this.__o%ST));}else __sgReportNaN(this.__o);}if(ch&&this.__sgRO)__markRender(this.__sgRO);}});
        Object.defineProperty(pt,'_y',{configurable:true,enumerable:true,get:function(){return __ptSlotGet(this,1);},set:function(v){if(__ptSlotSet(this,1,v))return;var ch=this.__sgRawY!==v;this.__sgRawY=v;if(ch&&this.__o!==undefined){var owner=this.__sgOwner;if(!__sgOwnsSlot(owner,this.__o))__sgDropStaleSlotWrite(owner,this.__o,'position/scale');else if(isFinite(v)){SGB[this.__o+1]=v;globalThis.__sgDirty=1;__markD(this.__o-(this.__o%ST));}else __sgReportNaN(this.__o);}if(ch&&this.__sgRO)__markRender(this.__sgRO);}});
      }catch(e){}
    };
    var __instrumentRotation=function(t,b,nativeOwner){
      if(!t)return;t.__sgOwner=nativeOwner||null;
      if(t.__sgRotHook)return; var v=t._rotation;t.__sgRot=v;t.__sgRotHook=1;
      // 双态同 __ptSlotGet/Set:rotation 的租约字段在 transform 上(__b/__sgRotLease)。
      // 有效条件同三条(generation+owner.__i+registry);无效租约在开关开启时写暂存并终止,
      // 绝不回落旧路径(旧 __sgOwnsSlot 不查 generation,同 id 重租会写穿——评审 P1)。
      try{Object.defineProperty(t,'_rotation',{configurable:true,enumerable:true,
        get:function(){
          if(globalThis.__SG_SINGLE_SOURCE===1&&this.__sgRotLease!==undefined){
            if(__rotLeaseValid(this))return SGB[this.__b+oROT];
            globalThis.__sgLeaseReadFallbacks=(globalThis.__sgLeaseReadFallbacks|0)+1;
          }
          return this.__sgRot;
        },
        set:function(n){
          if(globalThis.__SG_SINGLE_SOURCE===1&&this.__b!==undefined){
            if(__rotLeaseValid(this)){
              this.__sgRot=n;   // 双写期镜像
              var w=Math.fround(n);
              if(isFinite(w)){ if(SGB[this.__b+oROT]!==w){SGB[this.__b+oROT]=w;globalThis.__sgDirty=1;__markD(this.__b);} }
              else __sgReportNaN(this.__b+oROT);
              return;
            }
            globalThis.__sgLeaseWriteFallbacks=(globalThis.__sgLeaseWriteFallbacks|0)+1;
            this.__sgRot=n;   // 无效租约:写暂存,槽不动
            return;
          }
          var ch=this.__sgRot!==n;this.__sgRot=n;if(ch&&this.__b!==undefined){var owner=this.__sgOwner;if(!__sgOwnsSlot(owner,this.__b))__sgDropStaleSlotWrite(owner,this.__b,'rotation');else if(isFinite(n)){SGB[this.__b+oROT]=n;globalThis.__sgDirty=1;__markD(this.__b);}else __sgReportNaN(this.__b+oROT);}}});}catch(e){}
    };
    // FGUI virtual lists can replace/reuse a DisplayObject's Transform points after the native id
    // was allocated. Rebind the current point objects and copy their local values immediately;
    // otherwise setters keep updating an obsolete ObservablePoint while the shared scene graph
    // retains the provisional x/y until a later remove/add cycle.
    var __refreshTransformBinding=function(o){
      if(!o||!(o.__i>0)||!o.transform)return false;
      // 轻量 Native 句柄的 Transform/Point 本身就是共享内存视图，不属于需要修复绑定的
      // Pixi Transform。跳过旧 Pixi 兼容路径，避免双向同步互相覆盖。
      if(o.__nativeNodeProxy)return false;
      // runRebind(lease contract):capture→revokeOld→rewire→populate→activate。
      // capture 在更新任何 __o/__b 之前读当前真值源:换入的新 point 未 instrument 读数据字段;
      // 复用的旧 point 租约仍有效时读槽(populate 判等后零写,幂等),已过期读暂存。
      // revokeOld 撤销被替换旧对象的租约(v2 §3.2:现状旧 point 永远悬挂旧偏移)。
      var t=o.transform,b=o.__i*ST,ch=false;
      var __rbIO={
        capture:function(){
          return [t.position&&t.position._x,t.position&&t.position._y,t.scale&&t.scale._x,t.scale&&t.scale._y,
            t.pivot&&t.pivot._x,t.pivot&&t.pivot._y,t.skew&&t.skew._x,t.skew&&t.skew._y,t._rotation];
        },
        revokeOld:function(){ __sgRevokeReplacedTransform(o); },
        rewire:function(){
          t.__b=b;
          __instrumentPoint(t.position,b+oX,null,o);__instrumentPoint(t.scale,b+oSX,null,o);
          __instrumentPoint(t.pivot,b+oPVX,null,o);__instrumentPoint(t.skew,b+oSKX,null,o);__instrumentRotation(t,b,o);
        },
        populate:function(vals){
          var offs=[oX,oY,oSX,oSY,oPVX,6,oSKX,8,oROT];
          // 与 setter/__bind 同守卫。静默丢弃等于把故障藏起来:重挂时业务字段是 NaN 而槽位保留
          // 上一值(新分配的槽=0),表现为"对象跑到左上角"而不是消失——必须点名,才能追到毒源。
          for(var i=0;i<offs.length;i++){
            if(vals[i]===undefined)continue;
            if(!isFinite(vals[i])){__sgReportNaN(b+offs[i],'refreshBinding');continue;}
            if(SGB[b+offs[i]]!==vals[i]){SGB[b+offs[i]]=vals[i];ch=true;}
          }
          if(ch){globalThis.__sgDirty=1;__markD(b);}
        },
        activate:function(){ __sgActivateTransformLease(o); }
      };
      if(__sgLease)__sgLease.runRebind(__rbIO);
      else{var __vv=__rbIO.capture();__rbIO.revokeOld();__rbIO.rewire();__rbIO.populate(__vv);__rbIO.activate();}
      return ch;
    };
    var __instrumentDisplayScalar=function(o,name,off,isBool){
      var tag='__sgRaw_'+name; if(o[tag]!==undefined)return; var value=o[name]; o[tag]=value;
      try{Object.defineProperty(o,name,{configurable:true,enumerable:true,get:function(){return this[tag];},set:function(v){var ch=this[tag]!==v;this[tag]=v;if(ch&&this.__i>0){var b=this.__i*ST,w=isBool?(v?1:0):v;if(!__sgOwnsSlot(this,b))__sgDropStaleSlotWrite(this,b,name);else if(isFinite(w)){SGB[b+off]=w;globalThis.__sgDirty=1;__markD(b);}else __sgReportNaN(b+off);}}});}catch(e){}
    };
    // [sp-cab]:底部按钮的"静态图↔spine"其实是同一 spine 的 cacheAsBitmap 开关(IntervalSpine 播完冻住)。
    // 翻转时打点+武装 [swap-tree]/[sp-first],看冻结/解冻两侧是否有一帧无 quad。
    var __spCabDiag=function(o,v){
      o.__spFirstDiagUntil=(globalThis.__frameNo||0)+8;
      if((globalThis.__spCabLogs||0)<200){globalThis.__spCabLogs=(globalThis.__spCabLogs||0)+1;
        __log('[sp-cab] f='+(globalThis.__frameNo||0)+' name='+(o.name||(o.skeleton&&o.skeleton.data&&o.skeleton.data.name)||'?')+' inst='+(o.__i||0)+' cacheAsBitmap->'+(v?1:0)
          +' phase='+(globalThis.__sgPhase||'?')+' vis='+(o.visible===false?0:1)
          +' sgType='+(o.__i>0?(SGB[o.__i*ST+oTYPE]|0):-1)+' sgQuads='+(o.__i>0?(SGB[o.__i*ST+oQUADCNT]|0):-1));}
      if(__swapWatch.size<3){var __cit=(o.parent&&o.parent.parent)||o.parent||o;var __cdup=false;
        __swapWatch.forEach(function(w){if(w.item===__cit)__cdup=true;});
        if(!__cdup)__swapWatch.add({item:__cit,left:4,tag:'cab:'+(o.name||'?')});}
    };
    var __instrumentRenderField=function(o,name,dflt){
      var own=Object.prototype.hasOwnProperty.call(o,name), d=own&&Object.getOwnPropertyDescriptor(o,name);
      if(d&&(!d.configurable||d.get||d.set))return;
      if(!own){ for(var p=Object.getPrototypeOf(o);p;p=Object.getPrototypeOf(p)){ var pd=Object.getOwnPropertyDescriptor(p,name); if(pd){ if(pd.get||pd.set)return; break; } } }
      var tag='__sgRender_'+name; if(Object.prototype.hasOwnProperty.call(o,tag))return;
      var value=own?o[name]:(o[name]===undefined?dflt:o[name]); o[tag]=value;
      try{Object.defineProperty(o,name,{configurable:true,enumerable:true,get:function(){return this[tag];},set:function(v){var ch=this[tag]!==v;this[tag]=v;if(ch)__markRender(this);
        if(ch&&name==='_cacheAsBitmap'&&this.skeleton&&globalThis.__SP_FIRST_DIAG===1)__spCabDiag(this,v);}});}catch(e){}
    };
    var __watchMeshBuffers=function(o){
      if(!o)return; if(o.__sgMeshBuffers)for(var j=0;j<o.__sgMeshBuffers.length;j++){var old=o.__sgMeshBuffers[j];if(old&&old.__sgOwners)old.__sgOwners.delete(o);} if(!o.geometry)return;
      var list=o.geometry.buffers?o.geometry.buffers.slice():[]; if(o.geometry.indexBuffer)list.push(o.geometry.indexBuffer);
      o.__sgMeshBuffers=list;
      for(var i=0;i<list.length;i++){ var bf=list[i]; if(!bf)continue; if(!bf.__sgOwners)bf.__sgOwners=new Set(); bf.__sgOwners.add(o);
        if(!bf.__sgUpdateHook&&bf.update){ var up=bf.update;bf.__sgUpdateHook=1;bf.update=function(){var r=up.apply(this,arguments);if(this.__sgOwners)this.__sgOwners.forEach(__markRender);return r;}; }
      }
    };
    var __watchTextStyle=function(o){
      if(!o)return; if(o.__sgTextStyle&&o.__sgTextStyle.__sgOwners)o.__sgTextStyle.__sgOwners.delete(o);
      var st=o._style||o.style; if(!st)return; o.__sgTextStyle=st; if(!st.__sgOwners)st.__sgOwners=new Set(); st.__sgOwners.add(o);
      if(st.__sgStyleHook)return; var sid=st.styleID||0;st.__sgStyleID=sid;st.__sgStyleHook=1;
      try{Object.defineProperty(st,'styleID',{configurable:true,enumerable:true,get:function(){return this.__sgStyleID;},set:function(v){var ch=this.__sgStyleID!==v;this.__sgStyleID=v;if(ch&&this.__sgOwners)this.__sgOwners.forEach(__markRender);}});}catch(e){}
    };
