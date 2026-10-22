    // (6) 1c 切换:worldTransform / worldAlpha 从 buffer 读;updateTransform 只调 C++ compute。
    var _Mtx = PIXI.Matrix;
    var _wtd = Object.getOwnPropertyDescriptor(TRp, 'worldTransform'); // 可能是数据属性(实例上),这里在原型加 getter
    // __sgBD>0 = 处在 getLocalBounds 作用域:域内所有变换/getter 回退到 pixi 原生 JS(把子树重算到局部空间),
    // 域外(主渲染)读 C++ buffer。因为 buffer 是权威,域内对 _wt/_lt 的临时改写出域后会被 getter 从 buffer 覆盖 → 瞬态无害。
    globalThis.__sgBD = 0;
    // (mode2/3 读桥 getter 块已删除——Phase F-8。__SG_MODE 恒 4(13 行硬编码),该分支为死代码;
    //  mode2 getter 读桥是实测性能死路(记忆固化)。mode4 语义:worldTransform 保持 pixi 原生
    //  数据属性,值由回写循环(committed 发布)维护,facade 契约由 wtDiv 审计保证恒 0。)
    // mode 4:每帧 compute 后把"变化节点"的 world/local 矩阵+worldAlpha 回写成普通数据属性,并 bump _worldID/_boundsID
    // (静止节点跳过 → sprite 顶点缓存照常生效)。渲染/逻辑/bounds 全读数据属性,与原生 pixi 语义完全一致。
    // 回写脏化:优先按 C++ 已算清单(COMPUTED 区)只处理本帧真变化的节点;溢出/全脏帧退全量遍历。
    // 全量版曾是每帧数千对象×6 浮点比较(sg 2.66ms 大头);清单版只碰 7-200 个。
    var CB = 32768*48 + 1 + 4096;   // COMPUTED_BASE(与 scenegraph.h 一致)
    var __wbOne = function(i, objs){
      var o=objs[i]; if(!o || __nativeParentOf(i)<0) return 0; var t=o.transform; if(!t) return 0;
      var b=i*ST, wt=t.worldTransform;
      var a=SGB[b+12],bb=SGB[b+13],cc=SGB[b+14],d=SGB[b+15],tx=SGB[b+16],ty=SGB[b+17];
      var chg=0;
      if (wt.a!==a||wt.b!==bb||wt.c!==cc||wt.d!==d||wt.tx!==tx||wt.ty!==ty){
        wt.a=a;wt.b=bb;wt.c=cc;wt.d=d;wt.tx=tx;wt.ty=ty;
        var lt=t.localTransform; lt.a=SGB[b+20];lt.b=SGB[b+21];lt.c=SGB[b+22];lt.d=SGB[b+23];lt.tx=SGB[b+24];lt.ty=SGB[b+25];
        t._worldID=((t._worldID||0)+1)|0; o._boundsID=((o._boundsID||0)+1)|0; chg=1;
      }
      var wa2=SGB[b+18]; if (o.worldAlpha!==wa2) o.worldAlpha=wa2;
      return chg;
    };
    globalThis.__sgWriteback = function(){
      var objs=__sgObjs, c=0;
      var cn = SGB[CB];
      if (cn >= 0 && cn <= 4096){   // 清单有效:只回写本帧重算过的节点
        for (var k=0;k<cn;k++) c += __wbOne(SGB[CB+1+k]|0, objs);
        return c;
      }
      var n=objs.length;
      for (var i=1;i<n;i++){
        var o=objs[i]; if(!o || __nativeParentOf(i)<0) continue; var t=o.transform; if(!t) continue;   // spine 子树 JS 自算;树外对象(缓存池)不回写
        var b=i*ST, wt=t.worldTransform;
        var a=SGB[b+12],bb=SGB[b+13],cc=SGB[b+14],d=SGB[b+15],tx=SGB[b+16],ty=SGB[b+17];
        if (wt.a!==a||wt.b!==bb||wt.c!==cc||wt.d!==d||wt.tx!==tx||wt.ty!==ty){
          wt.a=a;wt.b=bb;wt.c=cc;wt.d=d;wt.tx=tx;wt.ty=ty;
          var lt=t.localTransform; lt.a=SGB[b+20];lt.b=SGB[b+21];lt.c=SGB[b+22];lt.d=SGB[b+23];lt.tx=SGB[b+24];lt.ty=SGB[b+25];
          t._worldID=((t._worldID||0)+1)|0; o._boundsID=((o._boundsID||0)+1)|0; c++;
        }
        var wa=SGB[b+18]; if (o.worldAlpha!==wa) o.worldAlpha=wa;
      }
      return c;
    };
    // 业务坐标查询必须看到 native 权威 world matrix。mode 4 的常规回写只处理 C++ 当帧
    // computed 清单；新弹窗的静态节点若在回写前后被查询，JS worldTransform 可能仍是组装期值，
    // 即使 native 主屏已经按正确矩阵绘制。典型症状是飞行特效的 localToGlobal 起终点落在左上角。
    // 这里只在显式坐标查询时同步一次根树，并直接回写查询对象，不引入逐帧全树扫描。
    var __coordSyncing = 0;
    var __flushMutationReads=function(){if(typeof globalThis.__nativeFguiMutationReadBarrier==='function')globalThis.__nativeFguiMutationReadBarrier();else if(typeof globalThis.__sgBatchReadBarrier==='function')globalThis.__sgBatchReadBarrier();};
    var __copyNativeWorld = function(o){
      if(!o || !(o.__i>0) || !o.transform) return;
      var b=o.__i*ST, wt=o.transform.worldTransform;
      wt.a=SGB[b+12]; wt.b=SGB[b+13]; wt.c=SGB[b+14]; wt.d=SGB[b+15]; wt.tx=SGB[b+16]; wt.ty=SGB[b+17];
      o.worldAlpha=SGB[b+18];
    };
    // P2-C(二次复审):live 查询把 scratch 写进 JS worldTransform 后必须恢复——否则 facade
    // 与 committed 槽在查询后分叉(下一次 canonical writeback 前普通读者看到 live 值,
    // 30s wtDiv 审计也会把合法查询报成发散)。sync 返回快照,包装层 finally 恢复。
    var __syncWorldForCoordinateQuery = function(o){
      if(__coordSyncing || globalThis.__SG<2 || globalThis.__sgBD || !o || !(o.__i>0)) return null;
      __coordSyncing=1;
      try{
        __flushMutationReads();
        if(typeof globalThis.__sgChainWorld==='function'){
          SGB[LSC+7]=0;
          globalThis.__sgChainWorld(o.__i|0);
          if(SGB[LSC+7]===1&&o.transform){   // P1-3:live 结果在 scratch,committed 槽未被污染
            var wt=o.transform.worldTransform;
            var snap={o:o,a:wt.a,b:wt.b,c:wt.c,d:wt.d,tx:wt.tx,ty:wt.ty,wa:o.worldAlpha};
            wt.a=SGB[LSC];wt.b=SGB[LSC+1];wt.c=SGB[LSC+2];wt.d=SGB[LSC+3];wt.tx=SGB[LSC+4];wt.ty=SGB[LSC+5];
            o.worldAlpha=SGB[LSC+6];
            return snap;   // 包装层 finally 恢复 committed facade
          }
          __copyNativeWorld(o);   // scratch 无效(极端态):读槽=committed,无需恢复
          return null;
        }
        __sgCanonicalBarrier(__nativeRootId(o.__i)); __copyNativeWorld(o);   // 桥缺失回退:canonical 原子入口
        return null;
      }finally{ __coordSyncing=0; }
    };
    var __restoreCommittedFacade = function(snap){
      if(!snap||!snap.o||!snap.o.transform) return;
      var wt=snap.o.transform.worldTransform;
      wt.a=snap.a;wt.b=snap.b;wt.c=snap.c;wt.d=snap.d;wt.tx=snap.tx;wt.ty=snap.ty;
      snap.o.worldAlpha=snap.wa;
    };
    // P1-C(三审):canonicalizing barrier 的唯一 JS 原子入口——compute(canonical) 与 facade
    // 回写不可分离。分离的后果链:查询 canonical 清位+生成 COMPUTED 清单→无人消费→下一次
    // phase5 canonical 债务已清不重算并把清单重置 0→JS worldTransform 永久停旧值。
    var __sgCanonicalBarrier=function(rootId){
      __sgCompute(rootId|0,1); /*canonical-barrier*/
      if(globalThis.__sgWriteback)globalThis.__sgWriteback();
    };
    globalThis.__sgCanonicalBarrier=__sgCanonicalBarrier;
    var _toGlobal=DOp.toGlobal, _toLocal=DOp.toLocal, _getGlobalPosition=DOp.getGlobalPosition;
    // P2-E(三审):sync 必须在 try 内——第二对象 sync 抛异常时第一对象才能被 finally 恢复。
    // P2-F(三审):尊重 Pixi skipUpdate 语义——true=读现有 facade,不触发 live chain(与 bounds 一致)。
    if(_toGlobal) DOp.toGlobal=function(position,point,skipUpdate){
      var s1=null;
      try{ if(!skipUpdate)s1=__syncWorldForCoordinateQuery(this); return _toGlobal.apply(this,arguments); }
      finally { __restoreCommittedFacade(s1); }
    };
    if(_toLocal) DOp.toLocal=function(position,from,point,skipUpdate){
      var s1=null,s2=null;
      try{
        if(!skipUpdate){ s1=__syncWorldForCoordinateQuery(this); if(from)s2=__syncWorldForCoordinateQuery(from); }
        return _toLocal.apply(this,arguments);
      } finally { __restoreCommittedFacade(s2); __restoreCommittedFacade(s1); }
    };
    if(_getGlobalPosition) DOp.getGlobalPosition=function(point,skipUpdate){
      var s1=null;
      try{
      if(!skipUpdate)s1=__syncWorldForCoordinateQuery(this);
      if(this.__nativeParentlessFacade&&this.__i>0){
        var out=point||new PIXI.Point(),id=this.__i|0,pid=__nativeParentOf(id),b=id*ST,x=SGB[b+oX],y=SGB[b+oY];
        if(pid>0){
          // P1-D(三审):Pixi 语义=「父 world × local position」(pivot 下与对象原点不同)。父的
          // 正式槽是 working/旧 seal 值,父本帧变化时陈旧——对 pid 再做链式 scratch 取 live 父矩阵。
          var pa,pb2,pc,pd,ptx,pty,useSlot=1;
          if(!skipUpdate&&typeof globalThis.__sgChainWorld==='function'){
            SGB[LSC+7]=0;globalThis.__sgChainWorld(pid);
            if(SGB[LSC+7]===1){pa=SGB[LSC];pb2=SGB[LSC+1];pc=SGB[LSC+2];pd=SGB[LSC+3];ptx=SGB[LSC+4];pty=SGB[LSC+5];useSlot=0;}
          }
          if(useSlot){var pb=pid*ST;pa=SGB[pb+12];pb2=SGB[pb+13];pc=SGB[pb+14];pd=SGB[pb+15];ptx=SGB[pb+16];pty=SGB[pb+17];}
          out.x=pa*x+pc*y+ptx;out.y=pb2*x+pd*y+pty;
        }else{out.x=x;out.y=y;}
        return out;
      }
      if(this.parent) __copyNativeWorld(this.parent);
      return _getGlobalPosition.apply(this,arguments);
      } finally { __restoreCommittedFacade(s1); }
    };
    globalThis.__sgObjCount = function(){ return __sgObjs.length; };   // ≈ slot 高水位(容量兜底判定用)
    // Pixi 的生命周期契约是 removeChild != destroy：对象池、弹窗缓存和临时换父都会让
    // 一个仍然存活的对象长时间处于 parent=null。宿主过去用“连续两次脱树”猜测死亡，
    // 会在第二轮扫描时释放仍被 JS 持有的 Native slot；之后复用同一外壳时，Spine、FGUI
    // native-only 子树等并不能从一个普通 addChild 完整重建，于是出现相位相关的随机缺图。
    //
    // Native 不再猜测 JS 所有权。JS 堆由 JavaScriptCore 像浏览器一样按可达性管理，
    // Native/WebGL 资源则由 Pixi 的 destroy()/dispose() 统一钩子确定地 __sgFree。
    // 这个接口仅供开发者手动审计业务层是否忘记销毁，生产渲染循环绝不调度它。
    globalThis.__sgDetachedAudit = function(rootObj){
      var objs=__sgObjs, n=objs.length,rootId=rootObj&&rootObj.__i|0,detached=0;
      for (var i=1;i<n;i++){ var o=objs[i]; if(!o || i===rootId || o.__i!==i) continue;
        if (__nativeParentOf(i)<0) detached++;
      }
      return {detached:detached,slots:n};
    };
