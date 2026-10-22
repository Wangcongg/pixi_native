    // ---- 临时排查巡检(轮盘/弹窗偶现错位;定位根因后整段删除) ----
    // 每 5 秒对渲染根做"影子对比":JS 侧局部影子(position/scale 的 _x/_y) vs 共享槽 X/Y/SX/SY。
    // mode4 下 worldTransform 由 buffer 回写,两侧可能同错,__sgCompare 有盲区;影子对比才能抓
    // "业务以为在 A、槽停在 B"的卡死形态。fgui proxy(NativePoint)读的就是槽,差恒 0,天然无害。
    // tween 驱动期间两侧短暂不同步属预期(值在动)——连续两拍同节点同现场(静止)才判卡死打印。
    // 变换一致性排查已定案(2026-08-25 收档):巡检/强制诊断/全量对拍全部退回默认关,
    // 宿主注入或控制台置 1 可随时重开(__SG_WATCH/__SG_DIAG 上方 hook 段有 ===undefined 注入判断)。
    globalThis.__SG_WATCH=globalThis.__SG_WATCH===undefined?0:globalThis.__SG_WATCH;
    // 全量对拍(=2,C++ 每帧从根重算与剪枝结果比对)审计已完成,退回 1=纯脏剪枝。
    globalThis.__SG_DIRTY=1;
    // Phase A-3 溯源已完成使命(dirty=154 生产者与帧内时序均已定位),默认关闭;排障时手动置 1。
    globalThis.__SG_TRACE_DIRTY=0;   // B0-3.1② 归因完成(头号=dat_gui 幂等假写,已修 proxy fround 判等);排障时置 1
    // 依赖 dev_debug 的 __dbgScene(DebugSceneHandler);进=scene 名取主题表含'29'项(fallback 首项)。
    globalThis.__sgCycleOn=0;
    globalThis.__sgCycleTick=function(){
      if(globalThis.__sgCycleOn!==1)return;
      var h=globalThis.__dbgScene;
      if(!h){__log('[sg-cycle] __dbgScene 未就绪,5s 后重试');globalThis.__setTimer(globalThis.__sgCycleTick,5000);return;}
      try{
        var scName=(h.current_scene&&h.current_scene.name)||'';
        var inLobby=!scName||String(scName).toLowerCase().indexOf('lobby')>=0||((globalThis.__sgCycleBackN|0)>=2);
        if(inLobby){globalThis.__sgCycleBackN=0;}else{globalThis.__sgCycleBackN=(globalThis.__sgCycleBackN|0)+1;}
        __log('[sg-cycle] scene='+scName+' inLobby='+(inLobby?1:0));
        if(inLobby){
          var names=(h.themesCfgProxy&&h.themesCfgProxy.all_themes_name)||[];var tgt=null;
          for(var i=0;i<names.length;i++)if(String(names[i]).indexOf('29')>=0){tgt=names[i];break;}
          if(!tgt&&names.length)tgt=names[0];
          if(tgt){globalThis.__sgCycleN=(globalThis.__sgCycleN|0)+1;__log('[sg-cycle] 第'+globalThis.__sgCycleN+'轮 进机台 '+tgt);h.scene=tgt;}
          globalThis.__setTimer(globalThis.__sgCycleTick,25000);   // 25s 后出
        }else{
          __log('[sg-cycle] 回大厅');h.backLobby();
          globalThis.__setTimer(globalThis.__sgCycleTick,12000);   // 12s 后再进
        }
      }catch(e){__log('[sg-cycle] 异常 '+e.message);globalThis.__setTimer(globalThis.__sgCycleTick,8000);}
    };
    var __watchLast=null;
    globalThis.__sgWatch=function(root){
      root=root||globalThis.__sgStage; if(!root)return '渲染根未挂载(等首帧 present)';
      var worst=null,wd=0,n=0;
      var swalk=function(o){ if(!o)return;
        if(o.__i>0&&o.transform){ var b=o.__i*ST,pos=o.transform.position,sc=o.transform.scale;
          var px=pos?+pos._x:NaN,py=pos?+pos._y:NaN,sx=sc?+sc._x:NaN,sy=sc?+sc._y:NaN,d=0;
          if(isFinite(px)&&isFinite(py))d=Math.max(d,Math.abs(px-SGB[b]),Math.abs(py-SGB[b+1]));
          if(isFinite(sx)&&isFinite(sy))d=Math.max(d,Math.abs(sx-SGB[b+2]),Math.abs(sy-SGB[b+3]));
          if(d>wd){wd=d;worst=o;} n++; }
        var ch=o.children; if(ch)for(var i=0;i<ch.length;i++)swalk(ch[i]); };
      swalk(root);
      var line=null;
      if(worst&&wd>0.5){ var b2=worst.__i*ST,p2=worst.transform.position,s2=worst.transform.scale;
        line='i'+worst.__i+' '+(worst.name||(worst.constructor&&worst.constructor.name)||'?')
          +' JS('+(+p2._x).toFixed(1)+','+(+p2._y).toFixed(1)+' s'+(+s2._x).toFixed(2)+')'
          +' buf('+SGB[b2].toFixed(1)+','+SGB[b2+1].toFixed(1)+' s'+SGB[b2+2].toFixed(2)+')'; }
      return {max:wd,node:worst,checked:n,line:line};
    };
    var __watchTick=function(){
      if(globalThis.__SG_WATCH!==1)return;
      var r=globalThis.__sgWatch(); if(typeof r==='string')return;
      var key=r.line;
      if(key&&__watchLast===key){
        __log('[sg-watch] 影子失同步(连续两拍静止=疑似卡死): '+key+' 误差='+r.max.toFixed(1));
        if(globalThis.__sgStage)__sgCompare(globalThis.__sgStage);   // 顺带打 world 断链祖先链([sg-worst])
      }
      __watchLast=key;
    };
    if(typeof globalThis.__setTimer==='function')globalThis.__setTimer(__watchTick,5000,true);
    // F9 现场快照:大厅卡片叠住这类"持续错位"状态下按一下,列出同父下世界位置重合(<8px)的
    // 可见渲染节点对(各自 local/world 坐标),定位"谁把谁摆到了同一处"。影子巡检对 fgui 节点
    // 是盲区(NativePoint 读的就是槽),重叠检测不依赖影子,正好补上。
    globalThis.__sgDump=function(){
      var root=globalThis.__sgStage; if(!root){__log('[sg-dump] 无渲染根');return 0;}
      var overlaps=0;
      var scan=function(o){ if(!o||!o.children)return; var ch=o.children,pts=[];
        for(var i=0;i<ch.length;i++){var c=ch[i];if(!c||c.visible===false||!(c.__i>0))continue;var cb=c.__i*ST;
          if(SGB[cb+10]===0)continue;   // buf visible=0 跳过
          if(SGB[cb+11]===0&&SGB[cb+26]===0&&SGB[cb+43]===0)continue;   // 纯容器(无 TYPE/TEXID/quad)不算重叠:全屏层都在原点属正常
          pts.push({o:c,x:SGB[cb+16],y:SGB[cb+17]});}
        for(var a=0;a<pts.length;a++)for(var b2=a+1;b2<pts.length;b2++){
          if(Math.abs(pts[a].x-pts[b2].x)<8&&Math.abs(pts[a].y-pts[b2].y)<8){
            var A=pts[a].o,B2=pts[b2].o;
            if(overlaps++<12)__log('[sg-dump] 重叠 parent='+(o.name||(o.constructor&&o.constructor.name)||o.__i)
              +' | '+(A.name||A.__i)+' i'+A.__i+' local('+SGB[A.__i*ST].toFixed(0)+','+SGB[A.__i*ST+1].toFixed(0)+') world('+pts[a].x.toFixed(0)+','+pts[a].y.toFixed(0)+')'
              +' | '+(B2.name||B2.__i)+' i'+B2.__i+' local('+SGB[B2.__i*ST].toFixed(0)+','+SGB[B2.__i*ST+1].toFixed(0)+') world('+pts[b2].x.toFixed(0)+','+pts[b2].y.toFixed(0)+')');
          }}
        for(var k=0;k<ch.length;k++)scan(ch[k]); };
      scan(root);
      __log('[sg-dump] 完成 重叠对='+overlaps+' worldMax='+__sgCompare(root).toFixed(2)+' 影子='+JSON.stringify((globalThis.__sgWatch()||{}).line||null));
      return overlaps;
    };
    // Ctrl+E 屏幕中心体检:命中链(命中节点→根,每层 TYPE/VISIBLE/RENDERABLE/ALPHA/WALPHA/TEXID)
    // + 命中节点父容器的子节点清单。"轮子空"这类内容缺失,按一下直接看到中央实际是什么、
    // 符号们在不在/什么状态(V=0? A=0? TEX=0? 根本没有子节点?)。
    globalThis.__sgProbe=function(px,py){
      var root=globalThis.__sgStage; if(!root){__log('[sg-probe] 无渲染根');return;}
      px=px===undefined?((globalThis.__screenW||1590)/2):px;
      py=py===undefined?((globalThis.__screenH||960)/2):py;
      // __sgHit 返回候选数;(target,witness)对写在 SGB[HR+1+i*2](按命中优先级排)。取第一候选为主链。
      var hitN=__sgHit(root.__i,px,py,false)|0,cand='';if(globalThis.__sgWriteback)__sgWriteback();
      for(var hi=0;hi<Math.min(hitN,5);hi++)cand+=' t'+(SGB[HR+1+hi*2]|0)+'/w'+(SGB[HR+2+hi*2]|0);
      var id=hitN>0?(SGB[HR+1]|0):0;
      __log('[sg-probe] hit@('+px.toFixed(0)+','+py.toFixed(0)+') 候选'+hitN+':'+(cand||' 无')+' → 主链从 i'+id);
      // la(a,b,c,d,tx,ty)=LA 槽 20-25(C++ 由分量重组的局部矩阵缓存)。la 全零/identity 而 xy 有值
      // =「分量→LA 重组从未发生」(节点从未被 computeNode 访问);la 正常而 w=identity=下行组合丢失。
      var fmt=function(cur){ var b=cur*ST; var o2=__sgObjs[cur],nm=o2?(o2.name||(o2.constructor&&o2.constructor.name)||''):''; return 'i'+cur+(nm?'('+nm+')':'')+' T'+(SGB[b+11]|0)+' V'+(SGB[b+10]|0)+' R'+(SGB[b+28]|0)+' A'+SGB[b+9].toFixed(2)+' WA'+SGB[b+18].toFixed(2)+' TEX'+(SGB[b+26]|0)+' q'+(SGB[b+43]|0)+' ch'+(__sgNativeChildCount(cur)|0)+' xy('+SGB[b].toFixed(0)+','+SGB[b+1].toFixed(0)+') la('+SGB[b+20].toFixed(2)+','+SGB[b+21].toFixed(2)+','+SGB[b+22].toFixed(2)+','+SGB[b+23].toFixed(2)+','+SGB[b+24].toFixed(0)+','+SGB[b+25].toFixed(0)+') w('+SGB[b+16].toFixed(0)+','+SGB[b+17].toFixed(0)+')'; };
      var chain=[],cur=id,guard=0;
      while(cur>0&&guard++<24){ chain.push(fmt(cur)); cur=__sgNativeParent(cur)|0; }
      __log('[sg-probe] 链(命中→根): '+(chain.length?chain.join(' ← '):'无命中'));
      // 「戳一下」实验:对主链每个节点补一次 local 脏标记。若下一帧 world 恢复正常(再按一次
      // Ctrl+E 对比)=丢失的就是脏标记(写入方漏 mark/队列消费丢弃);若不恢复=compute 下行有病。
      if(id>0){ var pk=id,pg2=0,pn=0; while(pk>0&&pg2++<24){ __markD(pk*ST); pn++; pk=__sgNativeParent(pk)|0; } globalThis.__sgDirty=1; __log('[sg-probe] 已对主链 '+pn+' 节点补脏(戳一下实验)——请再按一次 Ctrl+E 对比 w 是否恢复'); }
      if(hitN>1){ var w=SGB[HR+2]|0; if(w>0&&w!==id)__log('[sg-probe] witness: '+fmt(w)); }
      var p=id>0?(__sgNativeParent(id)|0):(root.__i|0);   // 无命中时兜底列渲染根直接子
      if(p>0){ var n=__sgNativeChildCount(p)|0,rows=[];
        for(var i=0;i<Math.min(n,24);i++){ var c=__sgNativeChildAt(p,i)|0; if(!(c>0))continue; rows.push(fmt(c)); }
        __log('[sg-probe] '+(id>0?'父':'渲染根')+' i'+p+' 共 '+n+' 子: '+rows.join(' | ')); }
      // 左上角区域扫描:「轮子跑左上角」时按 Ctrl+E,列出实际渲染在左上角矩形内的实体节点
      // (V=1 且有内容,world 平移落在 0..260)+各自 parent 链到根——断链(parent≤0 早停)、
      // 挂错树、world 退化 identity 三种形态直接现形。限 14 行防刷屏。
      var tl=0;
      for(var ni=1;ni<__sgObjs.length;ni++){
        var no=__sgObjs[ni]; if(!no||no.__i!==ni)continue; var nb=ni*ST;
        if(SGB[nb+10]===0)continue;                                    // 不可见跳过
        if(SGB[nb+11]===0&&SGB[nb+26]===0&&SGB[nb+43]===0)continue;    // 纯容器跳过
        var wtx=SGB[nb+16],wty=SGB[nb+17];
        if(wtx<-4||wtx>260||wty<-4||wty>260)continue;
        if(tl===0)__log('[sg-probe] —— 左上角(0..260)实体节点 ——');
        if(tl++>=14){__log('[sg-probe] 左上角还有更多,已截断');break;}
        var pl=[],pc=__sgNativeParent(ni)|0,pg=0;
        while(pc>0&&pg++<12){pl.push('i'+pc+(__sgObjs[pc]&&__sgObjs[pc].name?'('+__sgObjs[pc].name+')':''));pc=__sgNativeParent(pc)|0;}
        __log('[sg-probe] TL '+fmt(ni)+' 父链:'+(pl.length?pl.join('←'):'(无父/断链 parent='+(__sgNativeParent(ni)|0)+')'));
      }
      if(tl===0)__log('[sg-probe] 左上角无实体节点(轮子不在左上角或已回正)');
    };
    var __armDumpKey=function(){
      var orig=globalThis.__keyboardEvent;
      if(typeof orig!=='function'||orig.__sgDumpArmed)return false;
      // 触发键 = Ctrl+D。mac 宿主的功能键(F9 等)不在映射表里,JS 收到的是私有区字符无法匹配;
      // 字母键经 charactersIgnoringModifiers 稳定产出 code="KeyD",配 ctrl 修饰不会误触业务输入。
      var wrapped=function(type,key,code,keyCode,repeat,alt,ctrl){
        if(type==='keydown'&&ctrl&&code==='KeyD'){try{__log('[sg-dump] Ctrl+D 触发');globalThis.__sgDump();}catch(e){__log('[sg-dump] 异常 '+e.message);}}
        if(type==='keydown'&&ctrl&&code==='KeyE'){try{__log('[sg-probe] Ctrl+E 触发');globalThis.__sgProbe();}catch(e){__log('[sg-probe] 异常 '+e.message);}}
        if(type==='keydown'&&ctrl&&code==='KeyB'){try{globalThis.__sgCycleOn=globalThis.__sgCycleOn===1?0:1;__log('[sg-cycle] '+(globalThis.__sgCycleOn===1?'启动自动进出机台循环':'停止'));if(globalThis.__sgCycleOn===1)globalThis.__sgCycleTick();}catch(e){__log('[sg-cycle] 异常 '+e.message);}}
        if(type==='keydown'&&ctrl&&code==='KeyT'){try{__log('[text-space-probe] Ctrl+T 触发 dump');if(typeof globalThis.__dumpTextProbe==='function')globalThis.__dumpTextProbe();}catch(e){__log('[text-space-probe] dump 异常 '+e.message);}}
        // Ctrl+G:文本节点缩放链查询(宠物页 timeFont 字小排查):遍历 pixi 显示树找目标文本,
        // 打节点及父链每级 x/y/scale 与 worldTransform 的 a/d(累计缩放),定位丢失的适配 scale。
        // Ctrl+G:设置菜单按钮点击失效排查——打 fanpage/help/account 按钮的 GObject 与
        // displayObject 事件状态(touchable/eventMode/监听数/几何/父链),并直测 __nativeOpenURL 桥。
        if(type==='keydown'&&ctrl&&code==='KeyG'){try{
          var gHits=[];
          gHits.push({openType:typeof globalThis.open,bridge:typeof globalThis.__nativeOpenURL});
          (function walkG(o,depth){
            if(!o||depth>16)return;
            try{
              var nm=String(o.name||'');
              if(gHits.length<8&&/wheel_view|wheel_disc|WheelView/i.test(nm)){
                var d=o.displayObject,rec={n:nm,tch:o.touchable,vis:o.visible,fv:o.finalVisible,
                  x:Math.round((o.x||0)*10)/10,y:Math.round((o.y||0)*10)/10,w:o.width,h:o.height,
                  sx:o.scaleX!==undefined?o.scaleX:(o.scale&&o.scale.x)};
                if(d){
                  var wt=null;try{wt=d.worldTransform;}catch(_w){}
                  rec.d={em:d.eventMode,ia:d.interactive,ic:d.interactiveChildren,
                    taps:typeof d.listenerCount==='function'?d.listenerCount('pointertap'):-1,
                    wtx:wt?Math.round(wt.tx*10)/10:null,wty:wt?Math.round(wt.ty*10)/10:null,
                    wa:wt?Math.round(wt.a*1000)/1000:null,vis:d.visible};
                  var chain=[],p=d.parent,g=0;
                  while(p&&g++<7){chain.push({n:p.name||'?',em:p.eventMode,ic:p.interactiveChildren,vis:p.visible});p=p.parent;}
                  rec.pchain=chain;
                }
                gHits.push(rec);
                if(nm==='fanpage'&&d&&!d.__evProbe){d.__evProbe=1;var od=d.emit;d.emit=function(t){var ts=String(t);if(ts.indexOf('pointer')===0||ts==='click'||ts==='tap')__log('[fanpage-ev] '+ts);return od.apply(this,arguments);};}
              }
            }catch(_e){}
            var ch=o._children||o.$children;
            if(ch)for(var gi=0;gi<ch.length;gi++)walkG(ch[gi],depth+1);
          })(globalThis.fgui&&fgui.GRoot&&fgui.GRoot.inst,0);
          __log('[menu-diag] '+JSON.stringify(gHits));
          globalThis.__HIT_DIAG=1;globalThis.__hitDiagN=0;
          __log('[menu-diag] HIT_DIAG 已开(接下来 60 次点击打命中路径)');
        }catch(e){__log('[menu-diag] 异常 '+e.message);}}
        // Ctrl+Y:富文本 kern 端到端注入(token 级排版验证)。同字体下四个 markup 变体直调
        // __nativeRichTextUpdate 比对返回宽度:span 标签边界不得吞空格(B≈A,D≈A-空格宽)。
        if(type==='keydown'&&ctrl&&code==='KeyY'){try{
          var F=globalThis.__nativeRichTextUpdate;
          if(typeof F==='function'){
            var font='normal normal normal 26px "Arial"';
            var mk=[['A-plain','Play on 2 different slots.'],
                    ['B-span','Play on <span style="color:#f9f301">2</span> different slots.'],
                    ['C-min','X<span style="color:#f9f301">2</span> Y'],
                    ['D-nospace','Play on <span style="color:#f9f301">2</span>different slots.']];
            for(var yi=0;yi<mk.length;yi++){
              var ym=F(4933,mk[yi][1],font,'#ffffff','transparent',0,'left',1,0,0,0,0,0,0,'top');
              __log('[kern-e2e] '+mk[yi][0]+' width='+(ym&&ym.width)+' height='+(ym&&ym.height));
            }
          }
        }catch(e){__log('[kern-e2e] 异常 '+e.message);}}
        // Ctrl+U:三宽度布局协议端到端注入(670/540 弹窗场景)。同参数仅 horizontalFrame 不同:
        // owner-box 期望每行中心=335(670/2),paragraph-box 期望 270(540/2)且行数与前者相同,
        // invalid 期望被 V2 绑定拒绝(null)。配 NATIVE_RICHTEXT_LINE_TRACE=1 看逐行几何。
        if(type==='keydown'&&ctrl&&code==='KeyU'){try{
          var F2=globalThis.__nativeRichTextUpdateV2;
          if(typeof F2!=='function'){__log('[frame-e2e] V2 绑定不存在');}
          else{
            var fbase={markup:'The quick brown fox jumps over the lazy dog again and again',font:'normal normal normal 26px "Arial"',fill:'#ffffff',stroke:'transparent',strokeWidth:0,align:'center',resolution:1,anchorX:0,anchorY:0,wrapWidth:540,lineHeight:0,boxWidth:670,boxHeight:0,vAlign:'top'};
            var fcases=[['owner-box','owner-box'],['paragraph-box','paragraph-box'],['invalid','nope']];
            for(var fi=0;fi<fcases.length;fi++){
              var fp={};for(var fk in fbase)fp[fk]=fbase[fk];fp.horizontalFrame=fcases[fi][1];
              var fm=F2(4933,fp);
              __log('[frame-e2e] '+fcases[fi][0]+' -> '+(fm?('w='+fm.width+' h='+fm.height):'null(rejected)'));
            }
            // breakWords(zh-TW 语义):相连 span 词在 breakWords=1 时逐 segment 断行(3 行),
            // =0 时 AA+BB 整词同行溢出(2 行)——高度差即证据。
            var bwBase={markup:'<span style="color:#f9f301">AA</span><span style="color:#00ff00">BB</span> C',font:'normal normal normal 26px "Arial"',fill:'#ffffff',stroke:'transparent',strokeWidth:0,align:'left',resolution:1,anchorX:0,anchorY:0,wrapWidth:40,lineHeight:0,boxWidth:0,boxHeight:0,vAlign:'top',horizontalFrame:'paragraph-box'};
            var bw0={};for(var bk in bwBase)bw0[bk]=bwBase[bk];bw0.breakWords=0;
            var bw1={};for(var bk2 in bwBase)bw1[bk2]=bwBase[bk2];bw1.breakWords=1;
            var bm0=F2(4933,bw0),bm1=F2(4933,bw1);
            __log('[frame-e2e] breakWords=0 -> h='+(bm0&&bm0.height)+' / breakWords=1 -> h='+(bm1&&bm1.height)+(bm0&&bm1&&bm1.height>bm0.height?'   ← segment 边界断行生效':''));
            // plain V2:owner-box(box=670,行对齐收进 surface)应接受;paragraph-box 对 plain 应拒绝。
            var FP=globalThis.__nativeTextUpdateV2;
            if(typeof FP==='function'){
              var pbase={text:'Reach level 100 to unlock',font:'normal normal normal 26px "Arial"',fill:'#ffffff',stroke:'transparent',strokeWidth:0,shadow:'transparent',shadowBlur:0,shadowX:0,shadowY:0,align:'center',padding:0,resolution:1,anchorX:0,anchorY:0,wrapWidth:540,lineHeight:0,letterSpacing:0,leading:0,vAlign:'top',gradientColors:'',gradientStops:'',gradientType:0,ascent:20,descent:6};
              var pcases=[['plain-owner-box',{horizontalFrame:'owner-box',boxWidth:670,boxHeight:0}],
                          ['plain-natural',{horizontalFrame:'natural',boxWidth:0,boxHeight:0}],
                          ['plain-paragraph(应拒)',{horizontalFrame:'paragraph-box',boxWidth:540,boxHeight:0}]];
              for(var pi=0;pi<pcases.length;pi++){
                var pp={};for(var pk in pbase)pp[pk]=pbase[pk];for(var pk2 in pcases[pi][1])pp[pk2]=pcases[pi][1][pk2];
                var pm=FP(4933,pp);
                __log('[frame-e2e] '+pcases[pi][0]+' -> '+(pm?('w='+pm.width+' h='+pm.height):'null(rejected)'));
              }
            }else __log('[frame-e2e] plain V2 绑定不存在');
            if(typeof globalThis.__dumpTextLayoutContracts==='function')globalThis.__dumpTextLayoutContracts();
          }
        }catch(e){__log('[frame-e2e] 异常 '+e.message);}}
        return orig.apply(this,arguments);
      };
      wrapped.__sgDumpArmed=true;
      globalThis.__keyboardEvent=wrapped;
      return true;
    };
    if(!__armDumpKey()&&typeof globalThis.__setTimer==='function'){   // __keyboardEvent 定义晚于本模块时延迟武装
      var __armTries=0,__armT=globalThis.__setTimer(function(){ if(__armDumpKey()||++__armTries>20)globalThis.__clearTimer&&globalThis.__clearTimer(__armT); },1000,true);
    }
    // Phase F-3:worldTransform=只读 committed facade 的可观测证明。mode4 不装 getter/setter
    // (mode2 getter 读桥是实测性能死路;writeback 的 _worldID/_boundsID bump 是 pixi 顶点/bounds
    // 缓存驱动源)——「JS wt 不是第二真值」由结构(回写恒覆盖)+发散审计(30s 抽查)双重保证:
    // 树内非 spine 节点的 JS wt 与槽 WA..WTY 不一致=有人把 JS wt 当真值写且未被回写纠正。
    var __sgWtAudit=function(){
      var objs=__sgObjs,div=0,n=objs.length;
      for(var i=1;i<n;i++){
        var o=objs[i]; if(!o||o.__i!==i||o._destroyed) continue;
        if(__nativeParentOf(i)<0) continue;                       // 树外(缓存池)不回写,不审计
        var b=i*ST; if((SGB[b+11]|0)===4) continue;               // spine slot:JS 自算域,有意差异
        var t=o.transform; if(!t) continue; var wt=t._wt||t.worldTransform; if(!wt) continue;
        if(wt.a!==SGB[b+12]||wt.b!==SGB[b+13]||wt.c!==SGB[b+14]||wt.d!==SGB[b+15]||wt.tx!==SGB[b+16]||wt.ty!==SGB[b+17]){
          div++;
          if((globalThis.__sgWtDivLog|0)<6){globalThis.__sgWtDivLog=(globalThis.__sgWtDivLog|0)+1;
            __log('[wt-div] i'+i+' jsWt=('+wt.tx.toFixed(1)+','+wt.ty.toFixed(1)+') 槽=('+SGB[b+16].toFixed(1)+','+SGB[b+17].toFixed(1)+')');}
        }
      }
      globalThis.__sgWtDivergence=div;   // 快照式(非累计):当前发散节点数,恒 0=facade 契约成立
      return div;
    };
    globalThis.__sgWtAudit=__sgWtAudit;
    // 临时诊断探针(PiggyBank 二次打开定位):__sgProbeNode(o,tag,frames) 注册后每帧打
    // 三值对比(JS 门面 x/y/pivot/scale vs 槽 X/Y/PVX/PVY/SX vs world WTX/WTY),
    // 首帧附 authorityDump。「没写进」=三者全旧;「写进没算」=槽新 world 旧;「算了被盖」=槽被改回。
    var __sgProbes=[];
    globalThis.__sgProbeNode=function(o,tag,frames){
      var d=o&&(o.displayObject||o._displayObject||o);
      if(!d||!(d.__i>0)){__log('[probe-'+tag+'] 对象无 __i,放弃');return;}
      __sgProbes.push({d:d,o:o,tag:tag,left:frames||40,first:1});
      __log('[probe-'+tag+'] 注册 i'+d.__i+' 连续 '+(frames||40)+' 帧');
    };
    globalThis.__sgProbeFlush=function(){
      if(!__sgProbes.length)return;
      for(var i=__sgProbes.length-1;i>=0;i--){
        var pr=__sgProbes[i],d=pr.d,b=(d.__i|0)*ST;
        if(!(d.__i>0)){__sgProbes.splice(i,1);continue;}
        var g=pr.o&&pr.o!==d?pr.o:null;   // fgui GObject(有 pivot 语义)
        __log('[probe-'+pr.tag+'] f='+(globalThis.__frameNo|0)
          +' js(x='+(g?g.x:d.x).toFixed(1)+',y='+(g?g.y:d.y).toFixed(1)
          +(g&&g.pivotX!==undefined?(',pvx='+g.pivotX+',pvy='+g.pivotY+(g.pivotAsAnchor!==undefined?',anchor='+(g.pivotAsAnchor?1:0):'')):'')
          +(g?(',w='+(g.width!==undefined?Number(g.width).toFixed(1):'?')+',h='+(g.height!==undefined?Number(g.height).toFixed(1):'?')
             +',gsx='+(g.scaleX!==undefined?Number(g.scaleX).toFixed(3):'?')
             +',srcW='+(g.sourceWidth!==undefined?g.sourceWidth:(g._sourceWidth!==undefined?g._sourceWidth:'?'))
             +',initW='+(g.initWidth!==undefined?g.initWidth:(g._initWidth!==undefined?g._initWidth:'?'))):'')
          +',dx='+(d.x!==undefined?Number(d.x).toFixed(1):'?')+',dy='+(d.y!==undefined?Number(d.y).toFixed(1):'?')+')'
          +' slot(X='+SGB[b+oX].toFixed(1)+',Y='+SGB[b+oY].toFixed(1)+',PVX='+SGB[b+oPVX].toFixed(1)+',PVY='+SGB[b+6].toFixed(1)+',SX='+SGB[b+oSX].toFixed(2)+')'
          +' world(WTX='+SGB[b+oWTX].toFixed(1)+',WTY='+SGB[b+17].toFixed(1)+',WA='+SGB[b+oWA].toFixed(2)+')'
          +' vis='+(SGB[b+oVIS]|0)+' parent='+(typeof __nativeParentOf==='function'?__nativeParentOf(d.__i|0):'?'));
        if(pr.first){pr.first=0;if(typeof globalThis.__sgAuthorityDump==='function')try{globalThis.__sgAuthorityDump(d.__i|0);}catch(e){}}
        if(--pr.left<=0){__sgProbes.splice(i,1);__log('[probe-'+pr.tag+'] 结束');}
      }
    };
    // 排查心跳:每 30 秒一行运行时状态,日志里没有它=本模块没加载/没执行到这——不再靠猜。
    // 默认关(排查已定案):__SG_DIAG / __SG_WATCH / __NATIVE_RUNTIME_DIAG 任一为 1 才起心跳。
    if(typeof globalThis.__setTimer==='function'&&(globalThis.__SG_DIAG===1||globalThis.__SG_WATCH===1||globalThis.__NATIVE_RUNTIME_DIAG===1))globalThis.__setTimer(function(){
      var kb=globalThis.__keyboardEvent;
      __log('[sg-hb] DIAG='+globalThis.__SG_DIAG+' watch='+globalThis.__SG_WATCH
        +' stage='+(globalThis.__sgStage?(globalThis.__sgStage.__i|0):'无')
        +' CtrlD='+(kb&&kb.__sgDumpArmed?'已武装':'未武装')
        +' lateMut='+(globalThis.__sgLateMut|0)+' unsealed='+(globalThis.__sgUnsealedPresent|0)+' phRegress='+(globalThis.__sgPhaseRegress|0)
        +' leaseFbR='+(globalThis.__sgLeaseReadFallbacks|0)+' leaseFbW='+(globalThis.__sgLeaseWriteFallbacks|0)+' 单源='+(globalThis.__SG_SINGLE_SOURCE|0)
        +' staleW='+(globalThis.__sgStaleSlotWriteDrops|0)+' wtDiv='+__sgWtAudit());
    },30000,true);
    globalThis.__SG = __SG_MODE;
    __log('[sg] pixi 接入已装载,__SG='+__SG_MODE+(__SG_MODE===4?' 增量回写模式(C++算 transform,回写 JS,native 主屏)':''));
  } else throw new Error('缺少 PIXI 或 native scenegraph 宿主能力');
} catch(e){ __log('[sg] 接入异常 '+e.message+' '+(e.stack||'')); throw e; }
})();
