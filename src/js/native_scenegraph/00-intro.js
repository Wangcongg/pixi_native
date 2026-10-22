// native_scenegraph.js —— PIXI DisplayObject ↔ native scenegraph / native screen bridge。
// 由 game_host.js 在 PIXI/spine/游戏全局就位后加载。宿主命令编码仍留在 game_host.js;
// 本模块只负责 PIXI patch 和 scene tree 填充/接管。
(function(){
'use strict';
var PIXI = globalThis.PIXI;

// ================= PIXI ↔ native 场景图 =================
// 把每个 DisplayObject 绑一个 native 节点,变换分量/树结构同步进共享 buffer,C++ 每帧算 world 变换。
// JS 保留 PIXI API/事件/动画状态；共享场景树和主屏绘制由 native 权威执行。
var __WANT_SG = true;
// mode4 = C++ 算 transform + 增量回写普通数据属性；主屏由文件末尾 native-only hook 提交。
var __SG_MODE = 4;      // 装载时决定:4=增量回写(getter 不装);3=conch;2=getter 读桥;1=并行校验;0=关
try {
  if (__WANT_SG && PIXI && typeof __sgAlloc==='function' && globalThis.__sg) {
    var SGB = new Float32Array(globalThis.__sg), ST=48;
    var oX=0,oY=1,oSX=2,oSY=3,oROT=4,oPVX=5,oSKX=7,oALPHA=9,oVIS=10,oWA=12,oWTX=16,oWALPHA=18;
    var OPp = PIXI.ObservablePoint.prototype, TRp = PIXI.Transform.prototype, DOp = PIXI.DisplayObject.prototype, COp = PIXI.Container.prototype;
    var __stockContainerGetLocalBounds=COp.getLocalBounds;
    var __spineLayoutBounds=function(o,rect){
      // Full-native Spine facades intentionally carry no JS SlotMesh children. Query the
      // authoritative spine-cpp pose before falling back to Pixi's display-tree bounds; this
      // keeps the ordinary width/height contract working for every responsive Spine view.
      if(o&&o.__spxFacade&&typeof globalThis.__spxLocalBounds==='function')try{
        var inst=typeof o.__ensureInst==='function'?o.__ensureInst():(o.__spxInst|0),b=inst>0?globalThis.__spxLocalBounds(inst):null;
        if(b&&b.ok&&b.width>0&&b.height>0){rect=rect||new PIXI.Rectangle();rect.x=b.x;rect.y=b.y;rect.width=b.width;rect.height=b.height;return rect;}
      }catch(_nativeSpinexBoundsError){}
      try{var r=__stockContainerGetLocalBounds.call(o,rect);if(r&&r.width>0&&r.height>0)return r;}catch(_stockSpineBoundsError){}
      return o.getLocalBounds(rect);
    };
    var __pendingSpineBounds=new PIXI.Rectangle(),__applyPendingSpineSize=function(o){if(!o||(o.__nativePendingWidth==null&&o.__nativePendingHeight==null))return;var r=__spineLayoutBounds(o,__pendingSpineBounds),w=r&&r.width||0,h=r&&r.height||0;if(o.__nativePendingWidth!=null&&w>0){var sx=o.scale.x<0?-1:1;o.scale.x=Math.max(0,Number(o.__nativePendingWidth)||0)/w*sx;o.__nativePendingWidth=null;}if(o.__nativePendingHeight!=null&&h>0){var sy=o.scale.y<0?-1:1;o.scale.y=Math.max(0,Number(o.__nativePendingHeight)||0)/h*sy;o.__nativePendingHeight=null;}};
    globalThis.__nativeApplyPendingSpineSize=__applyPendingSpineSize;
    // DisplayObject owns Pixi's public size accessors. Wrap that canonical layer as well so Spine
    // subclasses captured by different bundles all receive the same native-aware sizing rule.
    (function(){var wd=Object.getOwnPropertyDescriptor(DOp,'width'),hd=Object.getOwnPropertyDescriptor(DOp,'height'),rect=new PIXI.Rectangle();if(!wd||!hd||wd.configurable===false||hd.configurable===false)return;
      Object.defineProperties(DOp,{
        width:{configurable:true,get:function(){if(!this.skeleton)return wd.get.call(this);var r=__spineLayoutBounds(this,rect);return Math.abs(this.scale.x)*(r&&r.width||0);},set:function(v){if(!this.skeleton)return wd.set.call(this,v);var r=__spineLayoutBounds(this,rect),w=r&&r.width||0,s=this.scale.x<0?-1:1;if(w)this.scale.x=Math.max(0,Number(v)||0)/w*s;}},
        height:{configurable:true,get:function(){if(!this.skeleton)return hd.get.call(this);var r=__spineLayoutBounds(this,rect);return Math.abs(this.scale.y)*(r&&r.height||0);},set:function(v){if(!this.skeleton)return hd.set.call(this,v);var r=__spineLayoutBounds(this,rect),h=r&&r.height||0,s=this.scale.y<0?-1:1;if(h)this.scale.y=Math.max(0,Number(v)||0)/h*s;}}
      });
    })();
    // 动态注册表只保留 Spine 根。其它 DisplayObject 即使重写 updateTransform，也由属性脏钩子/native tree 驱动，
    // 不再为了兼容 Pixi 渲染路径逐帧遍历其 JS 子树。
    // Spine 的 runtime 对象仍保留业务 API，但骨骼 world matrix 与 slotObject 场景矩阵由 C++ 计算。
    // 子树内 transform 打 __spT/__spTree，避免 Pixi 再做一遍场景树矩阵传播。
    var __ov = globalThis.__sgOv = new Set();
    // Slot objects are uncommon, but the old native-screen path inspected every live Spine root
    // on every frame just to discover them.  Maintain the small subset at the three public write
    // APIs instead.  The wrappers live on the Spine prototype so pooled/reused instances and
    // objects created after this hook are covered without another global scan.
    // spine 驱动闸门:可见实例每个 ticker 帧完整驱动；不可见/屏外实例完全冻结
    // (时间也停,复显从暂停点继续,产品语义按用户要求"不可见就停止播放")。
    // ①可见链(沿 C++ 父链读共享 VISIBLE 位;ScrollPane 用 mask 裁剪不设 visible,故还需②)
    // ②屏外 AABB(渲染子树盒 vs 视口 pad64;bounds 随 C++ transform 传播更新,与驱动无关,空盒保守)
    // Install on the concrete instance: bundled spine-pixi/a10 wrappers can insert another
    // prototype above Spine.prototype, but responsive sizing must always reach native scale.
    var __installSpineSizeAccessors=function(c){
      if(!Object.prototype.hasOwnProperty.call(c,'__nativeSizeAccessors'))try{var sizeRect=new PIXI.Rectangle();Object.defineProperties(c,{
        width:{configurable:true,get:function(){var r=__spineLayoutBounds(this,sizeRect);return Math.abs(this.scale.x)*(r&&r.width||0);},set:function(value){var r=__spineLayoutBounds(this,sizeRect),w=r&&r.width||0,sign=this.scale.x<0?-1:1;if(w)this.scale.x=Math.max(0,Number(value)||0)/w*sign;else this.__nativePendingWidth=Number(value)||0;}},
        height:{configurable:true,get:function(){var r=__spineLayoutBounds(this,sizeRect);return Math.abs(this.scale.y)*(r&&r.height||0);},set:function(value){var r=__spineLayoutBounds(this,sizeRect),h=r&&r.height||0,sign=this.scale.y<0?-1:1;if(h)this.scale.y=Math.max(0,Number(value)||0)/h*sign;else this.__nativePendingHeight=Number(value)||0;}}
      });c.__nativeSizeAccessors=1;}catch(_spineInstanceSizeError){}
    };
    var __ovChk = function(c){ if(c && c.updateTransform!==COp.updateTransform && c.updateTransform!==DOp.updateTransform){
      if (!c.skeleton) return;
      // 只剩门面一种形态:legacy spine 的驱动闸门/帧库 baker/蒙皮桥/slot 钩子已整体退役,
      // 门面的驱动、蒙皮、发射、冻结、prime、slotObject 全在 C++。非门面 spine 也不可能再出现——
      // 全库骨架 4.2+.skel、包内已无 .json,构造不出 handle 时 NativeSpine 直接点名抛错。
      // 仅保留业务布局在用的响应式 width/height。
      if (c.__spxH) c.__spxWake = 1;   // 门面挂树:唤醒本帧 __spxFacadeTick 巡检(离树休眠的对称点)
      __installSpineSizeAccessors(c);
    } };
    // Lightweight Native containers do not execute Pixi Container.addChild, so their adoption
    // path must notify this registry directly.  Keeping this as a single child hook also covers
    // Spine subclasses and pooled instances without traversing a JavaScript display tree.
    globalThis.__nativeSceneAdopt=function(child){__ovChk(child);};
