    // ===== conch P1:Quad 引用流(__QUADREF=1 启用)=====
    // 普通 sprite(TYPE=1 且纹理已上传)渲染时不再由 JS 算顶点/打包 attribute,只把节点号写进
    // QuadRefBatch 命令;native 从场景图 slot 读 world+rect/uv/tint 组顶点合批。复杂节点(TYPE≠1)
    // 照旧走 pixi 原路径——pixi ObjectRenderer 切换机制自动 flush 两侧,paint order 天然保序。
    // 段后 renderer.reset() 清 pixi 状态缓存(native 改了 program/buffer/blend,防"跳过冗余"用错状态)。
    if (PIXI.ObjectRenderer && PIXI.extensions && PIXI.ExtensionType){
      // 注意:PIXI.ObjectRenderer 是 ES6 class,必须用 class 继承(ES5 _super.call 会抛
      // "Cannot call a class constructor without |new|",且发生在 Renderer 构造期 → 整个 app 建不起来黑屏)
      class QuadRefRenderer extends PIXI.ObjectRenderer {
        constructor(r){ super(r); this.ids = []; }
        start(){ this.ids.length = 0; }
        stop(){ this.flush(); }
        render(sp){ this.ids.push(sp.__i); }
        flush(){
          var n = this.ids.length; if (!n) return;
          if (typeof globalThis.__nativeQuadRefBatch === 'function') globalThis.__nativeQuadRefBatch(this.ids, n);
          this.ids.length = 0;
          // 精准失效 native 段改过的状态缓存(shader/geometry/buffer/blend/纹理绑定)。
          // 曾用 renderer.reset() 全量:每段触发 pixi 全状态重建命令,段多时命令暴涨(解码 3ms→12ms)。
          var r = this.renderer;
          try { r.shader.reset(); } catch(e){}
          try { r.geometry.reset(); } catch(e){}
          try { if (r.buffer && r.buffer.reset) r.buffer.reset(); } catch(e){}
          try { r.state.reset(); } catch(e){}
          try { r.texture.reset(); } catch(e){}
        }
      }
      QuadRefRenderer.extension = { name:'quadref', type: PIXI.ExtensionType.RendererPlugin };
      PIXI.extensions.add(QuadRefRenderer);
      // 分流判定(sprite 与 text 共用):TYPE=1(__wrType 已排除 filter/cache/非矩形mask/重写render)+
      // 纹理已上传 + 常规 blend(native 只映射 0-3)+ 主屏(renderTexture/滤镜内投影不同,走原路径)
      var __qrTry = function(o, renderer){
        var qb = o.__i*ST;
        if (SGB[qb+oTYPE]===1 && SGB[qb+oTEXID]>0 && o.blendMode<=3 && !renderer.renderTexture.current){
          var qp = renderer.plugins && renderer.plugins.quadref;
          if (qp){ renderer.batch.setObjectRenderer(qp); qp.render(o); return true; }
        }
        return false;
      };
      var SP5 = PIXI.Sprite && PIXI.Sprite.prototype;
      if (SP5 && SP5._render){
        var _spr5 = SP5._render;
        SP5._render = function(renderer){
          if (globalThis.__QUADREF===1 && this.__i>0 && __qrTry(this, renderer)) return;
          return _spr5.call(this, renderer);
        };
      }
      // Text 同样下沉:它就是一张单纹理 quad,且 UI 中 sprite/text 交错排列——不下沉则每个 text
      // 都截断引用流(段数≈列表项数,段边界 reset+状态重建成本 × 几十段=吃光收益)。
      // updateText 照调保持文本刷新;新纹理未上传(TEXID=0)时自动走原路径一次触发上传+回填。
      var TX5 = PIXI.Text && PIXI.Text.prototype;
      if (TX5 && TX5._render){
        var _txr5 = TX5._render;
        TX5._render = function(renderer){
          if (globalThis.__QUADREF===1 && this.__i>0){
            try { this.updateText(true); } catch(e){}
            if (__qrTry(this, renderer)) return;
          }
          return _txr5.call(this, renderer);
        };
      }
      __log('[quadref] conch P1 已装载(__QUADREF='+globalThis.__QUADREF+')');
    }
    // 树结构变化统一置脏并记录代次。native present 会用该代次判断 compute 之后是否又发生
    // alloc/free/reparent，命中时在同一帧内补算，不再通过 renderable=false 跨帧隐藏。
    // 结构版本:alloc/free/setParent/remove 四个入口全覆盖(含批处理队列路径),是"树形结构是否
    // 变过"的唯一权威。每次推进都镜像到 globalThis.__sgStructVer,让 native_node_proxy 的子节点
    // 快照缓存直接据此失效——避免各模块自己枚举"哪些 API 算结构变化"(漏掉 remove 就会让粒子
    // 容器按过期快照继续打包已摘除的粒子,表现为最后一帧永久残留)。
