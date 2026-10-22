      // ---- SG_MUTATION_BATCH_CORE_END ----
      // UIPackage.internalCreateObject is the outer semantic construction boundary. Nested package
      // children stay inside the same command stream and the native tree becomes visible atomically.
      var nativeTweenForeign=globalThis.__nativeTweenRecordConsumers||(globalThis.__nativeTweenRecordConsumers=new Map());
      var installTweens=function(f){if(typeof globalThis.__nativeTweenStartEx!=='function'||typeof globalThis.__nativeTweenAdvanceEx!=='function'||!f.Transition||f.Transition.prototype.__nativeFullTimeline)return;
        var tp=f.Transition.prototype,oldStart=tp.startTween,oldDispose=tp.disposeTween,oldInternal=tp.internalPlay;
        var active=new Map(),out=new Float32Array(4096*6),nextId=1,easeCache=new Map(),noneSlots=new Int32Array([-1,-1,-1,-1]);
        var linear=new Float32Array([0,1]);
        var curveOf=function(fn){if(typeof fn!=='function')return linear;if(easeCache.has(fn))return easeCache.get(fn);var a=new Float32Array(65);for(var i=0;i<a.length;i++){var x=i/64,y=x;try{y=Number(fn(x));}catch(e){}if(!isFinite(y))y=x;a[i]=y;}easeCache.set(fn,a);return a;};
        var colorParts=function(c){c=Number(c)||0;return [(c>>16)&255,(c>>8)&255,c&255,0];};
        var valueSpec=function(item,to){var ty=item.type,v=item.value,from,toA;
          if(ty===0||ty===1||ty===2||ty===3||ty===13){from=new Float32Array([v.f1||0,v.f2||0]);toA=new Float32Array([to.f1||0,to.f2||0]);}
          else if(ty===4){from=new Float32Array([v.f1||0]);toA=new Float32Array([to.f1||0]);}
          else if(ty===5){from=new Float32Array([v.i||0]);toA=new Float32Array([to.i||0]);}
          else if(ty===6){from=new Float32Array(colorParts(v.c));toA=new Float32Array(colorParts(to.c));}
          else if(ty===12){from=new Float32Array([v.f1||0,v.f2||0,v.f3||0,v.f4||0]);toA=new Float32Array([to.f1||0,to.f2||0,to.f3||0,to.f4||0]);}
          else return null;return {from:from,to:toA};
        };
        var setSemanticValue=function(r,a,o){var it=r.item,ty=it.type,v=it.value;
          if(ty===0||ty===1||ty===2||ty===3||ty===13){v.f1=a[o];v.f2=a[o+1];}
          else if(ty===4)v.f1=a[o];else if(ty===5)v.i=a[o];
          else if(ty===6)v.c=((Math.max(0,Math.min(255,Math.round(a[o])))<<16)|(Math.max(0,Math.min(255,Math.round(a[o+1])))<<8)|Math.max(0,Math.min(255,Math.round(a[o+2]))));
          else if(ty===12){v.f1=a[o];v.f2=a[o+1];v.f3=a[o+2];v.f4=a[o+3];}
        };
        var allocId=function(){var id=nextId++;if(nextId>16000000)nextId=1;while(active.has(id)){id=nextId++;if(nextId>16000000)nextId=1;}return id;};
        var schedule=function(trans,item,delay,isTween){
          var id=allocId(),ty=item.type,target=item.target,d=target&&target.displayObject,to={},spec=null,mode=0,duration=.1,repeat=0,yoyo=false,curve=linear;
          if(isTween){
            trans.prepareValue(item,to,trans.$reversed);
            if(ty===6){item.value.c=trans.$reversed?item.endValue.c:item.startValue.c;to.c=trans.$reversed?item.startValue.c:item.endValue.c;}
            trans.applyValue(item,item.value);trans.prepareValue(item,to,trans.$reversed);
            if(ty===6){item.value.c=trans.$reversed?item.endValue.c:item.startValue.c;}
            spec=valueSpec(item,to);if(!spec)return false;duration=Math.max(.1,item.duration*1000);repeat=item.repeat|0;yoyo=!!item.yoyo;curve=curveOf(item.easeType);
          }else if(ty===11){mode=1;duration=Math.max(.1,(Number(item.value.f2)||0)*1000);spec={from:new Float32Array([Number(item.value.f1)||0,0]),to:new Float32Array([0,0])};}
          else{mode=2;duration=.1;spec={from:new Float32Array([0]),to:new Float32Array([1])};}
          if(d&&!(d.__i>0))__bind(d);var node=d&&d.__i>0?d.__i:0;
          if(!__nativeTweenStartEx(id,node,noneSlots,spec.from,spec.to,duration,performance.now(),Math.max(0,delay)*1000,repeat,yoyo,true,mode,curve))return false;
          item.completed=false;trans.$totalTasks++;item.tweener={__nativeTweenId:id,paused:false,removeAllEventListeners:function(){}};
          var rec={trans:trans,item:item,tween:!!isTween,mode:mode,lastX:0,lastY:0,hookFired:false};active.set(id,rec);
          if(delay<=0&&item.hook){item.hook.call(item.hookObj);rec.hookFired=true;}
          return true;
        };
        globalThis.__nativeTweenFrame=function(now){var n=__nativeTweenAdvanceEx(now,out)|0;
          for(var i=0;i<n;i++){var o=i*6,id=out[o]|0,status=out[o+1]|0,r=active.get(id);if(!r){var consumer=nativeTweenForeign.get(id);if(consumer)consumer(status,out[o+2],out[o+3],out[o+4],out[o+5]);continue;}var it=r.item,tr=r.trans;
            if((status&2)&&!r.hookFired&&r.tween&&it.hook){it.hook.call(it.hookObj);r.hookFired=true;}
            if(r.mode===1){var nx=out[o+2],ny=out[o+3],t=it.target;if(t){t.$gearLocked=true;t.setXY(t.x-r.lastX+nx,t.y-r.lastY+ny);t.$gearLocked=false;}r.lastX=nx;r.lastY=ny;}
            else if(r.tween){setSemanticValue(r,out,o+2);tr.applyValue(it,it.value);}
            if(status&1){active.delete(id);it.tweener=null;
              if(r.tween){tr.$totalTasks--;it.completed=true;if(it.hook2)it.hook2.call(it.hook2Obj);tr.checkAllComplete();}
              else{tr.$totalTasks--;it.completed=true;if(r.mode!==1)tr.applyValue(it,it.value);if(!r.hookFired&&it.hook)it.hook.call(it.hookObj);tr.checkAllComplete();}
            }
          }
        };
        tp.disposeTween=function(item){var tw=item&&item.tweener,id=tw&&tw.__nativeTweenId;if(id){__nativeTweenCancel(id);active.delete(id);item.tweener=null;return;}return oldDispose.call(this,item);};
        tp.startTween=function(item){if(schedule(this,item,0,true))return;return oldStart.call(this,item);};
        tp.internalPlay=function(delay){delay=Number(delay)||0;this.$ownerBaseX=this.$owner.x;this.$ownerBaseY=this.$owner.y;this.$totalTasks=0;
          for(var ii=0;ii<this.$items.length;ii++){var item=this.$items[ii];item.target=item.targetId?this.$owner.getChildById(item.targetId):this.$owner;if(!item.target)continue;this.disposeTween(item);
            var startTime=this.$reversed?(delay+this.$maxTime-item.time-(item.tween?item.duration:0)):(delay+item.time);if(startTime<0)startTime=0;
            if(item.tween){if(!schedule(this,item,startTime,true)){oldStart.call(this,item);}}
            else if(item.type===11){if(!schedule(this,item,startTime,false)){this.applyValue(item,item.value);}}
            else if(startTime<=0){item.completed=true;this.applyValue(item,item.value);if(item.hook)item.hook.call(item.hookObj);}
            else if(!schedule(this,item,startTime,false)){oldInternal.call(this,startTime);return;}
          }
        };
        tp.__nativeTransformTween=1;tp.__nativeFullTimeline=1;__log('[native-tween] complete FGUI Transition timeline runs in C++ (delay/repeat/yoyo/reverse/shake/color/filter/discrete events)');
      };
      // GImage 的普通图片默认长期保留 UIImage(Container)+FguiSprite 两个完整 Pixi 对象。
      // Native 主屏只需要一个 slot；构造完成后用 duck-typed NativeNodeProxy 替换这两层，FGUI
      // 仍能照常 addChildAt/setChildIndex/发事件。NineSlice/平铺图继续走各自已验证的 native 路径。
      var installLeanImages=function(f){var gp=f.GImage&&f.GImage.prototype;
        if(!gp||gp.__nativeLeafProxy||typeof globalThis.NativeNodeProxy!=='function')return false;
        gp.__nativeLeafProxy=1;var originalCreate=gp.createDisplayObject,original=gp.constructFromResource;
        var countLeaf=function(){var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.fguiImages=(st.fguiImages||0)+1;if(st.fguiFallbackRenders===undefined)st.fguiFallbackRenders=0;if(st.fguiImages===1)__log('[native-leaf] first GImage created without Pixi Sprite/Container');};
        var initNativeImage=function(item){var owner=this.UIOwner;
          if(item&&item.scaleByTile){
            item.load();var tile=new globalThis.NativeTilingProxy(owner,item.texture);tile._nativeWidth=tile._width=Math.max(0,Number(item.width)||0);tile._nativeHeight=tile._height=Math.max(0,Number(item.height)||0);tile._syncNativeTiling();
            owner.$content=tile;owner.setDisplayObject(tile);this.destroy();countLeaf();return;
          }
          if(item){item.load();if(item.scale9Grid)this.setNineSlice(item.texture,item.scale9Grid,item.tiledSlices);else{this._nativeWidth=this._width=Math.max(0,Number(item.width)||0);this._nativeHeight=this._height=Math.max(0,Number(item.height)||0);this.texture=item.texture;}}
          countLeaf();
        };
        gp.createDisplayObject=function(){var proxy=new globalThis.NativeNodeProxy({eventMode:'none',interactiveChildren:false});proxy.UIOwner=this;proxy.__fguiFallbackCreate=originalCreate;proxy.$initDisp=initNativeImage;this.$content=proxy;this.setDisplayObject(proxy);};
        gp.constructFromResource=function(){var result=original.apply(this,arguments),item=this.packageItem,shell=this.$content,disp=shell&&shell.$disp;
          if(!item||item.scaleByTile||item.scale9Grid||!disp||!(PIXI.Sprite&&disp instanceof PIXI.Sprite))return result;
          var proxy=null;
          try{
            proxy=new globalThis.NativeNodeProxy({texture:disp.texture,width:this.$width||this.$sourceWidth,height:this.$height||this.$sourceHeight,eventMode:'none',interactiveChildren:false});
            proxy.UIOwner=this;proxy.name=shell.name||'';proxy.tint=disp.tint;proxy.blendMode=shell.blendMode||disp.blendMode||0;
            proxy.position.set(shell.x||0,shell.y||0);proxy.scale.set(shell.scale?shell.scale.x:1,shell.scale?shell.scale.y:1);
            proxy.pivot.set(shell.pivot?shell.pivot.x:0,shell.pivot?shell.pivot.y:0);proxy.skew.set(shell.skew?shell.skew.x:0,shell.skew?shell.skew.y:0);
            proxy.rotation=shell.rotation||0;proxy.alpha=shell.alpha===undefined?1:shell.alpha;proxy.visible=shell.visible!==false;proxy.renderable=shell.renderable!==false;
            shell.removeChild(disp);shell.$disp=null;this.$content=proxy;this.setDisplayObject(proxy);this.$nativeLeafProxy=1;
            disp.destroy();shell.destroy({children:false});
            countLeaf();
          }catch(e){
            if(proxy)try{proxy.destroy();}catch(_e){}
            if(shell&&!shell.$disp&&disp&&!disp._destroyed)try{shell.$disp=disp;shell.addChild(disp);this.$content=shell;this.setDisplayObject(shell);}catch(_e2){}
            if((globalThis.__nativeLeafErrN||0)<8){globalThis.__nativeLeafErrN=(globalThis.__nativeLeafErrN||0)+1;__log('[native-leaf] GImage fallback '+e.message);}
          }
          return result;
        };
        if(globalThis.__NATIVE_RUNTIME_DIAG===1)__log('[native-leaf] plain FGUI images retain lightweight native handles only');return true;
      };
      var installLeanTexts=function(f){var gp=f.GTextField&&f.GTextField.prototype;
        if(!gp||gp.__nativeTextProxy||typeof globalThis.NativeTextProxy!=='function')return false;
        gp.__nativeTextProxy=1;gp.createDisplayObject=function(){var proxy=new globalThis.NativeTextProxy(this);this.$textField=proxy;this.setDisplayObject(proxy);var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.fguiTexts=(st.fguiTexts||0)+1;if(st.fguiFallbackRenders===undefined)st.fguiFallbackRenders=0;if(st.fguiTexts===1)__log('[native-leaf] first FGUI Text created without retained PIXI.Text');};
        __log('[native-leaf] regular FGUI Text fields retain CoreText handles only');return true;
      };
      var installLeanGraphs=function(f){var gp=f.GGraph&&f.GGraph.prototype;
        if(!gp||gp.__nativeGraphicsProxy||typeof globalThis.NativeGraphicsProxy!=='function')return false;
        gp.__nativeGraphicsProxy=1;gp.createDisplayObject=function(){var proxy=new globalThis.NativeGraphicsProxy(this);this.$displayObject=proxy;var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.fguiGraphics=(st.fguiGraphics||0)+1;if(st.fguiFallbackRenders===undefined)st.fguiFallbackRenders=0;if(st.fguiGraphics===1)__log('[native-leaf] first GGraph created without retained PIXI.Graphics');};
        // InitGuideCmd runs synchronously during startup, while this installer is waiting on the
        // first timer turn.  Retrofit every already-bound GGraph once so its legacy UISprite gets
        // the same native geometry/bounds/hit metadata as graphs created after this patch.  This is
        // intentionally type-wide; guide masks, popup blockers and any other early modal graph all
        // share the same lifecycle race.
        var legacy=0;
        for(var gi=1;gi<__sgObjs.length;gi++){
          var gd=__sgObjs[gi],go=gd&&gd.UIOwner;
          if(!gd||gd.__i!==gi||gd.__nativeGraphicsProxy||!go||!(go instanceof f.GGraph))continue;
          __wrType(gd);legacy++;
        }
        if(legacy)__log('[native-leaf] retrofitted '+legacy+' early FGUI GGraph node(s)');
        __log('[native-leaf] FGUI Graph shapes retain native geometry handles only');return true;
      };
      var installLeanMovieClips=function(f){var gp=f.GMovieClip&&f.GMovieClip.prototype;
        if(!gp||gp.__nativeMovieClipProxy||typeof globalThis.NativeNodeProxy!=='function')return false;
        gp.__nativeMovieClipProxy=1;gp.createDisplayObject=function(){var proxy=new globalThis.NativeNodeProxy({eventMode:'none',interactiveChildren:false});proxy.UIOwner=this;proxy.playing=true;proxy.currentFrame=0;proxy.interval=0;proxy.repeatDelay=0;proxy.swing=false;proxy.frames=null;proxy.setPlaySettings=function(){};this.$movieClip=proxy;this.setDisplayObject(proxy);var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.fguiMovieClips=(st.fguiMovieClips||0)+1;if(st.fguiMovieClips===1)__log('[native-leaf] first GMovieClip created without Pixi MovieClip/Ticker');};
        __log('[native-leaf] FGUI MovieClip frames and timing retain a native handle only');return true;
      };
      var installLeanContainers=function(f){if(globalThis.__NATIVE_LEAN_CONTAINERS===0||typeof globalThis.NativeContainerProxy!=='function')return false;var count=0,types=globalThis.__NATIVE_LEAN_CONTAINER_TYPES===undefined?15:(globalThis.__NATIVE_LEAN_CONTAINER_TYPES|0);
        var note=function(){var st=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});st.fguiContainers=(st.fguiContainers||0)+1;if(st.fguiFallbackRenders===undefined)st.fguiFallbackRenders=0;if(!count++){__log('[native-handle] first FGUI container created without retained PIXI.Container');}};
        var cp=f.GComponent&&f.GComponent.prototype;if((types&1)&&cp&&!cp.__nativeContainerProxy){cp.__nativeContainerProxy=1;cp.createDisplayObject=function(){var c=new globalThis.NativeContainerProxy(this,{eventMode:'static',interactiveChildren:true});c.interactive=true;this.$rootContainer=c;this.setDisplayObject(c);this.$container=c;note();};}
        var group=f.GGroup&&f.GGroup.prototype;if((types&2)&&group&&!group.__nativeContainerProxy){group.__nativeContainerProxy=1;group.createDisplayObject=function(){var c=new globalThis.NativeContainerProxy(this,{eventMode:'none',interactiveChildren:false});c.interactive=false;this.setDisplayObject(c);note();};}
        var loader=f.GLoader&&f.GLoader.prototype;if((types&4)&&loader&&!loader.__nativeContainerProxy){loader.__nativeContainerProxy=1;loader.createDisplayObject=function(){var c=new globalThis.NativeContainerProxy(this,{eventMode:'static',interactiveChildren:false});c.hitArea=new PIXI.Rectangle();this.$container=c;this.setDisplayObject(c);note();};}
        var text=f.GTextField&&f.GTextField.prototype;if((types&8)&&text&&!text.__nativeBitmapContainer){text.__nativeBitmapContainer=1;text.switchBitmapMode=function(val){if(val&&this.displayObject===this.$textField){if(!this.$btContainer){var box=this.$btContainer=new globalThis.NativeContainerProxy(this,{eventMode:'none',interactiveChildren:false});box.__nativeBitmapText=true;box.validate=function(){};
                // The stock FGUI layout method may keep using Sprite as a glyph measurement
                // record, but these instance-local list methods never bind those records into
                // either the JS display hierarchy or the native scene graph.
                box.__glyphMeasurements=[];box.addChild=function(child){if(!child)return child;if(child.parent&&child.parent!==box&&child.parent.removeChild)child.parent.removeChild(child);child.parent=box;box.__glyphMeasurements.push(child);return child;};
                box.getChildAt=function(index){if(index<0||index>=box.__glyphMeasurements.length)throw new Error('getChildAt index out of bounds');return box.__glyphMeasurements[index];};
                box.removeChildren=function(begin,end){begin=begin===undefined?0:begin|0;end=end===undefined?box.__glyphMeasurements.length:end|0;var out=box.__glyphMeasurements.splice(begin,Math.max(0,end-begin));for(var i=0;i<out.length;i++)out[i].parent=null;return out;};note();}this.switchDisplayObject(this.$btContainer);}else if(!val&&this.displayObject===this.$btContainer)this.switchDisplayObject(this.$textField);};
          var bitmapRender=text.renderWithBitmapFont;text.renderWithBitmapFont=function(){var result;globalThis.__nativeBitmapGlyphMeasureDepth=(globalThis.__nativeBitmapGlyphMeasureDepth|0)+1;try{result=bitmapRender.apply(this,arguments);}finally{globalThis.__nativeBitmapGlyphMeasureDepth=Math.max(0,(globalThis.__nativeBitmapGlyphMeasureDepth|0)-1);}var box=this.$btContainer;if(!box||!box.__glyphMeasurements)return result;var glyphs=box.__glyphMeasurements.slice();
            if(R.writeBitmapGlyphs)R.writeBitmapGlyphs(box,glyphs);box.__glyphMeasurements.length=0;
            for(var bi=0;bi<glyphs.length;bi++){var glyph=glyphs[bi];if(!glyph)continue;glyph.parent=null;if(this.$bitmapPool.indexOf(glyph)<0)this.$bitmapPool.push(glyph);}
            box.__nativeBitmapSourceText=String(this.$text||'');box.__nativeBitmapGlyphCount=glyphs.length;
            var bs=globalThis.__nativeLeanStats||(globalThis.__nativeLeanStats={});bs.bitmapPackedLabels=(bs.bitmapPackedLabels||0)+1;bs.bitmapPackedGlyphs=(bs.bitmapPackedGlyphs||0)+glyphs.length;return result;};
          var scheduledBitmapRender=text.render;text.render=function(){
            // Bitmap labels are layout-only work followed by one native multi-quad write. Doing
            // this immediately prevents a one-frame empty adopted slot and avoids losing the
            // update when a popup mutates/destroys several GTimer callLater records together.
            if(this.$bitmapFont&&this.$btContainer&&this.displayObject===this.$btContainer&&!this.$inProgressBuilding){this.renderNow(true);return;}
            return scheduledBitmapRender.apply(this,arguments);
          };
        }
        __log('[native-handle] FGUI lightweight container types='+types+' (1=Component 2=Group 4=Loader 8=BitmapText)');return true;
      };
      var tries=0,installTimer=0,install=function(){globalThis.__nativeFguiRuntimeInstallWhenReady=install;var f=globalThis.fgui,p=f&&f.UIPackage&&f.UIPackage.prototype;
        if(!p||typeof p.internalCreateObject!=='function'||typeof globalThis.NativeNodeProxy!=='function'){
          tries++;if(!installTimer)installTimer=setTimeout(function(){installTimer=0;install();},tries<600?16:250);return;
        }
        if(installTimer){clearTimeout(installTimer);installTimer=0;}
        if(p.__nativeConstructBatch)return;p.__nativeConstructBatch=1;
        if(globalThis.__nativeBoot&&globalThis.__nativeBoot.isSealed()){var bsSeal=globalThis.__nativeBootStats||(globalThis.__nativeBootStats={});bsSeal.installAfterSealCount=(bsSeal.installAfterSealCount||0)+1;__log('[native-boot] 警告: fgui-runtime 在 seal 后才安装(编排断裂)');}
        var original=p.internalCreateObject;
        if(typeof globalThis.__nativeFguiInflate==='function'&&!p.__nativePackageInflate){p.__nativePackageInflate=1;var oldInflate=p.decompressPackage,oldDescriptor=p.getResDescriptor,oldPackageCreate=p.create;
          p.decompressPackage=function(buf){var tree=null;try{tree=__nativeFguiInflate(buf,this.$resKey||'');}catch(e){}if(tree&&tree.__nativeEntryCount>0){this.$resData=tree;this.__nativeBlueprint=!!tree.__nativeBlueprint;var stat=globalThis.__nativeFguiStats||(globalThis.__nativeFguiStats={});stat.packages=(stat.packages||0)+1;stat.entries=(stat.entries||0)+(tree.__nativeEntryCount|0);return;}return oldInflate.call(this,buf);};
          p.getResDescriptor=function(name){var data=this.$resData,key=data&&data.__nativePackageKey;if(key&&typeof globalThis.__nativeFguiEntry==='function'){
              if(Object.prototype.hasOwnProperty.call(data,name))return data[name];var value=__nativeFguiEntry(key,String(name));if(value!==null&&value!==undefined){data[name]=value;return value;}}
            return oldDescriptor.call(this,name);};
          // Native already parsed package.xml and sprites.bytes on assetQueue. Keep both resource
          // records and sprite Rectangles lazy: opening a package now creates only id/name plus two
          // Proxy maps; a PackageItem facade appears when business/FGUI first asks for it.
          if(typeof globalThis.__nativeFguiManifest==='function'&&typeof globalThis.__nativeFguiItem==='function'&&typeof globalThis.__nativeFguiItemsByType==='function'&&typeof globalThis.__nativeFguiSprite==='function'){
            var lazyAtlasTable=function(owner,key){var cache=Object.create(null);return new Proxy(cache,{get:function(target,prop){
              if(typeof prop!=='string')return target[prop];if(Object.prototype.hasOwnProperty.call(target,prop))return target[prop];var row=null;
              try{row=globalThis.__nativeFguiSprite(key,prop);}catch(e){}if(!row||row.length<6){target[prop]=undefined;return undefined;}
              var frame=new PIXI.Rectangle(Number(row[1])||0,Number(row[2])||0,Number(row[3])||0,Number(row[4])||0),rotate=row[5]?6:0;
              var cfg={atlasName:String(row[0]||''),frame:frame,rotate:rotate,orig:rotate?new PIXI.Rectangle(0,0,frame.height,frame.width):null,trim:null};target[prop]=cfg;
              var stat=globalThis.__nativeFguiStats||(globalThis.__nativeFguiStats={});stat.lazySprites=(stat.lazySprites||0)+1;return cfg;
            }});};
            // FairyGUI permits an image and an exported component to have the same resource name.
            // Its package parser resolves getItemByName with declaration-order/last-writer
            // semantics, while getItemById still exposes both records.  Do not let an id lookup of
            // the internal image populate (or overwrite) the by-name cache: doing so makes the
            // result depend on which nested image happened to materialise first.  Smash Stash has
            // exactly this layout for check_a/check_b; after the first button accessed its n0 image,
            // the next UIPackage.createObject("check_a") used to return a GImage instead of GButton.
            var packageItemFromRow=function(owner,row,byName){if(!row||row.length<13)return null;var targets=owner.__nativeFguiItemTargets,id=String(row[1]||''),existing=targets.id[id];if(existing){if(byName&&existing.name!=null)targets.name[String(existing.name)]=existing;return existing;}
              var pi=new f.PackageItem();pi.type=row[0]|0;pi.id=id;pi.name=row[2];pi.file=row[3];pi.width=Number(row[4])||0;pi.height=Number(row[5])||0;
              if(row[7]){pi.scale9Grid=new PIXI.Rectangle(Number(row[8])||0,Number(row[9])||0,Number(row[10])||0,Number(row[11])||0);pi.tiledSlices=row[12]|0;}else if(row[6])pi.scaleByTile=true;
              pi.owner=owner;owner.$items.push(pi);targets.id[id]=pi;if(byName&&pi.name!=null)targets.name[String(pi.name)]=pi;var stat=globalThis.__nativeFguiStats||(globalThis.__nativeFguiStats={});stat.lazyItems=(stat.lazyItems||0)+1;return pi;
            };
            var lazyItemTable=function(owner,key,byName,target){return new Proxy(target,{get:function(cache,prop){if(typeof prop!=='string')return cache[prop];if(Object.prototype.hasOwnProperty.call(cache,prop))return cache[prop];var row=null;try{row=globalThis.__nativeFguiItem(key,prop,byName);}catch(e){}var pi=packageItemFromRow(owner,row,byName);cache[prop]=pi||undefined;return pi||undefined;}});};
            var oldGetItemByType=p.getItemByType,oldPackageInit=p.init;
            p.getItemByType=function(type){if(!this.__nativeLazyItems)return oldGetItemByType.call(this,type);var ids=null;try{ids=globalThis.__nativeFguiItemsByType(this.$resKey,type|0);}catch(e){}if(!ids)return [];var out=[];for(var i=0;i<ids.length;i++){var pi=this.$itemsById[ids[i]];if(pi)out.push(pi);}return out;};
            p.init=function(){if(!this.__nativeLazyItems)return oldPackageInit.call(this);var fonts=this.getItemByType(6);for(var i=0;i<fonts.length;i++){var pi=fonts[i];this.loadFont(pi);if(pi.bitmapFont)f.UIPackage.$bitmapFonts[pi.bitmapFont.id]=pi.bitmapFont;}if(typeof globalThis.__nativeFguiPackageDidInit==='function')globalThis.__nativeFguiPackageDidInit(this);};
            p.create=function(resKey){this.$resKey=resKey;var buf=PIXI.Assets.cache.get('fgui.'+resKey);if(!buf)buf=PIXI.Assets.cache.get(resKey+'_fui');
              if(!buf||!(buf instanceof ArrayBuffer))return oldPackageCreate.call(this,resKey);this.decompressPackage(buf);var manifest=null;
              if(this.__nativeBlueprint)try{manifest=globalThis.__nativeFguiManifest(resKey);}catch(e){}if(!manifest||!manifest.lazy)return oldPackageCreate.call(this,resKey);
              this.$id=String(manifest.id||'');this.$name=String(manifest.name||'');this.$items=[];var idTarget=Object.create(null),nameTarget=Object.create(null);this.__nativeFguiItemTargets={id:idTarget,name:nameTarget};this.__nativeLazyItems=true;
              this.$itemsById=lazyItemTable(this,resKey,false,idTarget);this.$itemsByName=lazyItemTable(this,resKey,true,nameTarget);this.$atlasConfigs=lazyAtlasTable(this,resKey);
              var stat=globalThis.__nativeFguiStats||(globalThis.__nativeFguiStats={});stat.manifests=(stat.manifests||0)+1;stat.manifestItems=(stat.manifestItems||0)+(manifest.itemCount|0);return;
            };
            __log('[native-fgui] package manifest/items/sprite frames are native-backed and lazy');
          }
        }
        var xp=f.XmlParser;if(xp&&typeof globalThis.__nativeFguiXmlParse==='function'&&!xp.__nativeParser){xp.__nativeParser=1;var oldTry=xp.tryParse,oldChildren=xp.getChildNodes,oldAttrs=xp.getNodeAttributes,oldRoot=xp.getXmlRoot;
          xp.tryParse=function(source,mime){var node=null;try{node=__nativeFguiXmlParse(source);}catch(e){}if(node&&node.__nativeXml){var stat=globalThis.__nativeFguiStats||(globalThis.__nativeFguiStats={});stat.xml=(stat.xml||0)+1;return node;}return oldTry.call(this,source,mime);};
          xp.getChildNodes=function(node,match){if(node&&node.__nativeXml){var list=node.$children||[];if(!match)return list;var want=String(match).toLowerCase(),out=[];for(var xi=0;xi<list.length;xi++)if(String(list[xi].nodeName).toLowerCase()===want)out.push(list[xi]);return out;}return oldChildren.call(this,node,match);};
          xp.getNodeAttributes=function(node){return node&&node.__nativeXml?(node.$attributes||{}):oldAttrs.call(this,node);};xp.getXmlRoot=function(node){return node&&node.__nativeXml?node:oldRoot.call(this,node);};
          __log('[native-fgui] raw-deflate archive and XML parsing run in Foundation/zlib');
        }
        p.internalCreateObject=function(){globalThis.__sgBatchBegin();try{return original.apply(this,arguments);}finally{globalThis.__sgBatchEnd();}};
        installTweens(f);installLeanImages(f);installLeanTexts(f);installLeanGraphs(f);installLeanMovieClips(f);installLeanContainers(f);
        __log('[native-fgui] component construction uses reserved slots + batched tree commands');
      };install();
      // 启动契约注册(fgui-runtime,required):驱动器复用 WhenReady 链,契约锚点使安装在依赖
      // 出现的那个脚本边界立即完成;既有降频兜底轮询保留(native_fgui_delayed_boot_test 锁定)。
      if(globalThis.__nativeBoot)globalThis.__nativeBoot.register({id:'fgui-runtime',required:true,install:function(){
        var ff=globalThis.fgui,fpp=ff&&ff.UIPackage&&ff.UIPackage.prototype;
        if(fpp&&fpp.__nativeConstructBatch)return 'installed';
        if(typeof globalThis.__nativeFguiRuntimeInstallWhenReady==='function')globalThis.__nativeFguiRuntimeInstallWhenReady();
        ff=globalThis.fgui;fpp=ff&&ff.UIPackage&&ff.UIPackage.prototype;
        return (fpp&&fpp.__nativeConstructBatch)?'installed':'pending';
      }});
    })();
    // GSAP remains the public business API, but its common fire-and-forget transform tweens do not
    // need a JavaScript tick for every target.  Exact NativeNodeProxy/NativePoint targets with only
    // x/y/scale/rotation/alpha properties run on the host timeline. Complex plugins, onUpdate,
    // keyframes and unsupported controls stay on stock GSAP.
    // 契约化(gsap-native-timeline,optional):本 host 模块 eval 时 gsap(业务包)必然未加载,
    // 旧的一次性 IIFE 在此静默放弃——该优化实际从未生效,整场 legacy 且无日志。现在状态显式
    // (seal 报告可见);__NATIVE_GSAP_TIMELINE=1 显式开启才安装——路径未经真机长跑验证,默认
    // 保持 legacy 不改变现网行为,验证通过后再翻转默认。
    var installNativeGsapTweens=function(){
      var gs=globalThis.gsap,consumers=globalThis.__nativeTweenRecordConsumers;
      if(gs&&gs.to&&gs.to.__nativeTransformTimeline)return 'installed';
      if((globalThis.__NATIVE_GSAP_TIMELINE|0)!==1)return 'legacy';
      if(!gs||!consumers||typeof gs.to!=='function'||typeof globalThis.__nativeTweenStartEx!=='function')return 'pending';
      var originalTo=gs.to,nextId=8000000,curveCache=new Map(),fallbackOut=new Float32Array(2048*6),ownsFrame=!globalThis.__nativeTweenFrame;
      var originalKillTweensOf=gs.killTweensOf,originalGetTweensOf=gs.getTweensOf,originalIsTweening=gs.isTweening;
      var allowed={x:1,y:1,scale:1,scaleX:1,scaleY:1,rotation:1,alpha:1,duration:1,delay:1,repeat:1,yoyo:1,ease:1,onStart:1,onComplete:1,onInterrupt:1,onStartParams:1,onCompleteParams:1,onInterruptParams:1,callbackScope:1,overwrite:1,immediateRender:1,id:1,data:1,inherit:1};
      var allocId=function(){for(var n=0;n<7000000;n++){var id=nextId++;if(nextId>=15000000)nextId=8000000;if(!consumers.has(id))return id;}return 0;};
      var curveFor=function(ease){var key=ease||'none';if(curveCache.has(key))return curveCache.get(key);var fn=null;try{fn=typeof ease==='function'?ease:gs.parseEase&&gs.parseEase(ease||'none');}catch(e){}var out=new Float32Array(65);for(var i=0;i<65;i++){var x=i/64,y=fn?Number(fn(x)):x;out[i]=isFinite(y)?y:x;}curveCache.set(key,out);return out;};
      var numeric=function(value,current){if(typeof value==='string'){var m=/^([+-])=\s*(-?\d+(?:\.\d+)?)$/.exec(value);if(m)return current+(m[1]==='-'?-1:1)*Number(m[2]);}value=Number(value);return isFinite(value)?value:null;};
      var call=function(fn,params,scope,facade){if(typeof fn!=='function')return;var a=Array.isArray(params)?params.slice():[];for(var i=0;i<a.length;i++)if(a[i]==='{self}')a[i]=facade;fn.apply(scope||facade,a);};
      // facade 不进 GSAP 全局 timeline,killTweensOf/getTweensOf/isTweening/overwrite 找不到它们。
      // 按业务 target 建索引,包装这三个入口,否则"已取消"的元件仍被 C++ timeline 驱动。
      var targetTweens=new WeakMap();
      var trackFacade=function(f){if(!f._target||typeof f._target!=='object')return;var s=targetTweens.get(f._target);if(!s){s=new Set();targetTweens.set(f._target,s);}s.add(f);};
      var untrackFacade=function(f){if(!f._target||typeof f._target!=='object')return;var s=targetTweens.get(f._target);if(s)s.delete(f);};
      var SLOT_OF={x:0,y:1,scaleX:2,scaleY:3,rotation:4,alpha:9};
      var slotsForProps=function(props){if(props==null)return null;var out=[],parts=String(props).split(',');for(var i=0;i<parts.length;i++){var p=parts[i].replace(/\s+/g,'');if(p==='scale')out.push(2,3);else if(SLOT_OF[p]!==undefined)out.push(SLOT_OF[p]);}return out;};
      var facadeMatchesSlots=function(f,slotList){if(!slotList||!f._slots)return true;for(var i=0;i<f._slots.length;i++)if(slotList.indexOf(f._slots[i])>=0)return true;return false;};
      var killNativeTweensOf=function(target,props){
        if(!target)return;
        if(Array.isArray(target)){for(var i=0;i<target.length;i++)killNativeTweensOf(target[i],props);return;}
        if(typeof target!=='object')return;
        var s=targetTweens.get(target);if(!s||!s.size)return;
        var slotList=props===undefined?null:slotsForProps(props);
        var list=Array.from(s);
        for(var j=0;j<list.length;j++)if(list[j]._active&&facadeMatchesSlots(list[j],slotList))list[j].kill();
      };
      var collectNativeTweens=function(target){var out=[];if(!target||typeof target!=='object')return out;var s=targetTweens.get(target);if(s)s.forEach(function(f){if(f._active)out.push(f);});return out;};
      var targetSpec=function(target,vars){
        var owner=target&&target.__nativeNodeProxy?target:(target&&target.owner&&target.owner.__nativeNodeProxy?target.owner:null);if(!owner||!(owner.__i>0)||owner._destroyed)return null;
        var slots=[],from=[],to=[],seen=Object.create(null),add=function(slot,value,current){if(seen[slot])return false;var end=numeric(value,current);if(end===null)return false;seen[slot]=1;slots.push(slot);from.push(current);to.push(end);return true;};
        if(target===owner){if(vars.x!==undefined&&!add(0,vars.x,owner.x))return null;if(vars.y!==undefined&&!add(1,vars.y,owner.y))return null;if(vars.scale!==undefined){if(!add(2,vars.scale,owner.scale.x)||!add(3,vars.scale,owner.scale.y))return null;}if(vars.scaleX!==undefined&&!add(2,vars.scaleX,owner.scale.x))return null;if(vars.scaleY!==undefined&&!add(3,vars.scaleY,owner.scale.y))return null;if(vars.rotation!==undefined&&!add(4,vars.rotation,owner.rotation))return null;if(vars.alpha!==undefined&&!add(9,vars.alpha,owner.alpha))return null;}
        else{if(vars.scale!==undefined)return null;if(vars.x!==undefined&&!add(target.ox|0,vars.x,target.x))return null;if(vars.y!==undefined&&!add(target.oy|0,vars.y,target.y))return null;if(vars.scaleX!==undefined||vars.scaleY!==undefined||vars.rotation!==undefined||vars.alpha!==undefined)return null;}
        return slots.length&&slots.length<=4?{owner:owner,slots:new Int32Array(slots),from:new Float32Array(from),to:new Float32Array(to)}:null;
      };
      var tryNative=function(target,vars){
        if(!vars||typeof vars!=='object'||vars.paused||vars.onUpdate||vars.repeatDelay||vars.yoyoEase||vars.keyframes||vars.stagger||vars.motionPath||vars.pixi||vars.modifiers||vars.snap||vars.startAt)return null;
        var keys=Object.keys(vars);for(var i=0;i<keys.length;i++)if(!allowed[keys[i]])return null;
        var spec=targetSpec(target,vars);if(!spec)return null;var duration=vars.duration==null?Number(gs.defaults&&gs.defaults().duration)||.5:Number(vars.duration),delay=Number(vars.delay)||0,repeat=vars.repeat==null?0:(vars.repeat|0);if(!(duration>0)||delay<0)return null;
        if(vars.overwrite===true||vars.overwrite==='auto'){
          var owProps=null;
          if(vars.overwrite==='auto'){var op=[];for(var ok in SLOT_OF)if(vars[ok]!==undefined)op.push(ok);if(vars.scale!==undefined)op.push('scale');owProps=op.join(',');}
          killNativeTweensOf(target,vars.overwrite===true?undefined:owProps);
          if(typeof originalKillTweensOf==='function')try{vars.overwrite===true?originalKillTweensOf.call(gs,target):originalKillTweensOf.call(gs,target,owProps);}catch(e){}
        }
        var id=allocId();if(!id)return null;var facade={_ids:[id],_target:target,_vars:vars,_slots:spec.slots,_active:true,_started:false,_done:false,_killed:false,_start:performance.now(),_duration:duration,_delay:delay,_repeat:repeat,_yoyo:!!vars.yoyo,_resolve:null};
        facade._promise=new Promise(function(resolve){facade._resolve=resolve;});
        facade.kill=function(){if(!this._active)return this;for(var i=0;i<this._ids.length;i++){__nativeTweenCancel(this._ids[i]);consumers.delete(this._ids[i]);}this._active=false;this._killed=true;untrackFacade(this);call(this._vars.onInterrupt,this._vars.onInterruptParams,this._vars.callbackScope,this);if(this._resolve)this._resolve();return this;};
        facade.pause=function(){if(this._active&&!this._paused&&typeof __nativeTweenPause==='function'){var now=performance.now();for(var i=0;i<this._ids.length;i++)__nativeTweenPause(this._ids[i],true,now);this._paused=true;this._pauseAt=now;}return this;};
        facade.play=function(){if(this._active&&this._paused&&typeof __nativeTweenPause==='function'){var now=performance.now();for(var i=0;i<this._ids.length;i++)__nativeTweenPause(this._ids[i],false,now);this._start+=Math.max(0,now-this._pauseAt);this._paused=false;}return this;};facade.resume=facade.play;
        facade.isActive=function(){return this._active&&!this._paused;};facade.targets=function(){return [this._target];};facade.duration=function(v){return v===undefined?this._duration:this;};facade.delay=function(v){return v===undefined?this._delay:this;};facade.totalDuration=function(){return this._delay+this._duration*(this._repeat<0?Infinity:this._repeat+1);};
        facade.progress=function(v){if(v!==undefined)return this;var elapsed=Math.max(0,performance.now()-this._start-this._delay*1000);return Math.max(0,Math.min(1,elapsed/(this._duration*1000)));};facade.totalProgress=facade.progress;
        facade.time=function(v){if(v!==undefined)return this;var e=(performance.now()-this._start)/1000-this._delay;return Math.max(0,Math.min(this._duration,e));};facade.totalTime=facade.time;
        facade.seek=function(){return this;};facade.timeScale=function(v){return v===undefined?1:this;};facade.reverse=function(){return this;};facade.invalidate=function(){return this;};facade.revert=function(){return this.kill();};
        facade.eventCallback=function(type,fn,params,scope){if(arguments.length===1)return this._vars[type];this._vars[type]=fn;if(params)this._vars[type+'Params']=params;if(scope)this._vars.callbackScope=scope;return this;};
        facade.then=function(done,fail){return this._promise.then(function(){return typeof done==='function'?done():undefined;},fail);};
        var onRecord=function(status){if(!facade._active)return;if((status&2)&&!facade._started){facade._started=true;call(vars.onStart,vars.onStartParams,vars.callbackScope,facade);}if(status&1){consumers.delete(id);facade._active=false;facade._done=true;untrackFacade(facade);call(vars.onComplete,vars.onCompleteParams,vars.callbackScope,facade);if(facade._resolve)facade._resolve();}};
        consumers.set(id,onRecord);
        facade.restart=function(){if(this._killed||!(spec.owner.__i>0))return this;var now=performance.now();for(var i=0;i<this._ids.length;i++)__nativeTweenCancel(this._ids[i]);this._start=now;this._started=false;this._done=false;this._paused=false;consumers.set(id,onRecord);if(__nativeTweenStartEx(id,spec.owner.__i,spec.slots,spec.from,spec.to,duration*1000,now,delay*1000,repeat,!!vars.yoyo,false,0,curveFor(vars.ease))){if(!this._active){this._active=true;trackFacade(this);}}else{consumers.delete(id);this._active=false;}return this;};
        if(!__nativeTweenStartEx(id,spec.owner.__i,spec.slots,spec.from,spec.to,duration*1000,facade._start,delay*1000,repeat,!!vars.yoyo,false,0,curveFor(vars.ease))){consumers.delete(id);return null;}
        trackFacade(facade);
        var stat=globalThis.__nativeTweenStats||(globalThis.__nativeTweenStats={});stat.gsapTransformStarts=(stat.gsapTransformStarts||0)+1;return facade;
      };
      var nativeTo=function(target,vars){
        // A raw Pixi display transform followed by a semantic GObject tween is an ownership
        // handoff. Let Native FGUI atomically adopt the visible starting transform before GSAP
        // plugins cache properties or publish correlated axes one at a time.
        if(arguments.length===2&&!Array.isArray(target)&&typeof globalThis.__nativeFguiPrepareSemanticTween==='function')try{__nativeFguiPrepareSemanticTween(target,vars);}catch(_fguiTweenHandoffError){}
        if(arguments.length===2&&!Array.isArray(target)){var facade=tryNative(target,vars);if(facade)return facade;}var stat=globalThis.__nativeTweenStats||(globalThis.__nativeTweenStats={});stat.gsapFallbacks=(stat.gsapFallbacks||0)+1;return originalTo.apply(this,arguments);};nativeTo.__nativeTransformTimeline=1;nativeTo.__nativeOriginal=originalTo;gs.to=nativeTo;
      gs.killTweensOf=function(target,props){try{killNativeTweensOf(target,props);}catch(e){}return typeof originalKillTweensOf==='function'?originalKillTweensOf.apply(this,arguments):undefined;};
      gs.getTweensOf=function(target){var mine=collectNativeTweens(target),rest=typeof originalGetTweensOf==='function'?originalGetTweensOf.apply(this,arguments):[];return mine.length?mine.concat(rest||[]):(rest||[]);};
      gs.isTweening=function(target){var mine=collectNativeTweens(target);for(var i=0;i<mine.length;i++)if(!mine[i]._paused)return true;return typeof originalIsTweening==='function'?!!originalIsTweening.apply(this,arguments):false;};
      if(ownsFrame)globalThis.__nativeTweenFrame=function(now){var n=__nativeTweenAdvanceEx(now,fallbackOut)|0;for(var i=0;i<n;i++){var o=i*6,cb=consumers.get(fallbackOut[o]|0);if(cb)cb(fallbackOut[o+1]|0,fallbackOut[o+2],fallbackOut[o+3],fallbackOut[o+4],fallbackOut[o+5]);}};
      __log('[native-tween] GSAP pure Native transforms run on C++ timeline; complex/plugin tweens retain GSAP');
      return 'installed';
    };
    if(globalThis.__nativeBoot)globalThis.__nativeBoot.register({id:'gsap-native-timeline',install:installNativeGsapTweens});
    else installNativeGsapTweens();
    // 内容变化只把对应节点放进渲染脏集合；真正的几何/纹理同步在 present 前集中处理。
    { var GRd=PIXI.Graphics&&PIXI.Graphics.prototype;
      if(GRd&&GRd.clear){ var _gclr=GRd.clear; GRd.clear=function(){ __markRender(this); if(this.__maskOf)__markRender(this.__maskOf); return _gclr.apply(this,arguments); }; }
      if(GRd&&GRd.drawShape){ var _gdsh=GRd.drawShape; GRd.drawShape=function(){ var r=_gdsh.apply(this,arguments); __markRender(this); if(this.__maskOf)__markRender(this.__maskOf); return r; }; }
      var TXd=PIXI.Text&&PIXI.Text.prototype;
      if(TXd&&TXd.updateText){ var _utx=TXd.updateText; TXd.updateText=function(){ var need=!!this.dirty; var r=_utx.apply(this,arguments); if(need&&!this.__sgWriting)__markRender(this); return r; }; }
      var BTd=PIXI.BitmapText&&PIXI.BitmapText.prototype;
      if(BTd&&BTd.updateText){ var _ubt=BTd.updateText; BTd.updateText=function(){ var r=_ubt.apply(this,arguments); if(!this.__sgWriting)__markRender(this); return r; }; }
      var __hookRenderSetter=function(proto,name){ var d=proto&&Object.getOwnPropertyDescriptor(proto,name); if(!d||!d.set||!d.configurable)return;
        Object.defineProperty(proto,name,{configurable:true,get:d.get,set:function(v){ d.set.call(this,v); if(name==='geometry'&&typeof __watchMeshBuffers==='function')__watchMeshBuffers(this); if(name==='style'&&typeof __watchTextStyle==='function')__watchTextStyle(this); __markRender(this); }}); };
      if(TXd){ __hookRenderSetter(TXd,'text'); __hookRenderSetter(TXd,'style'); __hookRenderSetter(TXd,'resolution'); }
      if(BTd){ ['text','tint','align','fontName','fontSize','anchor','maxWidth','letterSpacing','roundPixels','resolution'].forEach(function(n){__hookRenderSetter(BTd,n);}); }
      var NSd=PIXI.NineSlicePlane&&PIXI.NineSlicePlane.prototype; if(NSd){
        ['width','height','leftWidth','rightWidth','topHeight','bottomHeight'].forEach(function(n){__hookRenderSetter(NSd,n);});
        // NineSlicePlane.texture is inherited from SimplePlane, so looking for a texture setter on
        // Mesh/NineSlicePlane does not install a hook. FGUI frequently creates the plane with an
        // empty texture and assigns the real texture later; textureUpdated() then reaches _refresh().
        // Mark here so texture swaps and asynchronous texture readiness both rewrite native quads.
        if(NSd._refresh&&!NSd.__sgRefreshHook){
          NSd.__sgRefreshHook=1;
          var _nsRefresh=NSd._refresh;
          NSd._refresh=function(){ var r=_nsRefresh.apply(this,arguments); if(!this.__sgWriting)__markRender(this); return r; };
        }
      }
      var TSd=PIXI.TilingSprite&&PIXI.TilingSprite.prototype; if(TSd){ __hookRenderSetter(TSd,'width'); __hookRenderSetter(TSd,'height'); __hookRenderSetter(TSd,'uvRespectAnchor'); }
      if(GRd){ __hookRenderSetter(GRd,'tint'); __hookRenderSetter(GRd,'blendMode'); }
      var MEd=PIXI.Mesh&&PIXI.Mesh.prototype; if(MEd){ __hookRenderSetter(MEd,'blendMode'); __hookRenderSetter(MEd,'tint'); __hookRenderSetter(MEd,'texture'); __hookRenderSetter(MEd,'geometry'); }
    }
    var __sgObjs = [];   // native 节点 idx → JS 外壳(Pixi 或 NativeNodeProxy);destroy 时清,防泄漏
