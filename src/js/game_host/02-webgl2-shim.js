// ===== WebGL2 垫片 =====
function makeGL() {
  var gl = {};
  for (var c in K) gl[c] = K[c];

  function ensureIntro(prog){
    if (prog.__intro) return prog.__intro;
    flushPending();                              // create/attach/link → 真 ANGLE
    prog.__intro = JSON.parse(__syncProgram(prog.__id));
    return prog.__intro;
  }

  // ---- 创建 ----
  gl.createShader      = function(t){ var h=mkH('shader',++enc.h.shader); enc.begin(OP.CreateShader); enc.w(h.__id); enc.w(t); enc.end(); h.__type=t; return h; };
  gl.createProgram     = function(){ var h=mkH('program',++enc.h.program); enc.begin(OP.CreateProgram); enc.w(h.__id); enc.end(); return h; };
  gl.createBuffer      = function(){ var h=mkH('buffer',++enc.h.buffer); enc.begin(OP.CreateBuffer); enc.w(h.__id); enc.end(); return h; };
  gl.createTexture     = function(){ var h=mkH('texture',++enc.h.texture); enc.begin(OP.CreateTexture); enc.w(h.__id); enc.end(); return h; };
  gl.createVertexArray = function(){ var h=mkH('vao',++enc.h.vao); enc.begin(OP.CreateVertexArray); enc.w(h.__id); enc.end(); return h; };
  gl.createFramebuffer = function(){ var h=mkH('fb',++enc.h.fb); enc.begin(OP.CreateFramebuffer); enc.w(h.__id); enc.end(); return h; };
  gl.createRenderbuffer = function(){ var h=mkH('renderbuffer',++enc.h.renderbuffer); enc.begin(OP.CreateRenderbuffer); enc.w(h.__id); enc.end(); return h; };

  // ---- shader / program ----
  gl.shaderSource = function(sh, src){ enc.begin(OP.ShaderSource); enc.w(sh.__id); enc.str(src); enc.end(); };
  gl.compileShader = function(sh){ enc.begin(OP.CompileShader); enc.w(sh.__id); enc.end(); };
  gl.attachShader = function(p, sh){ enc.begin(OP.AttachShader); enc.w(p.__id); enc.w(sh.__id); enc.end(); };
  gl.deleteShader = function(sh){ if(sh) { enc.begin(OP.DeleteShader); enc.w(sh.__id); enc.end(); } };
  gl.bindAttribLocation = function(p, idx, name){ enc.begin(OP.BindAttribLocation); enc.w(p.__id); enc.w(idx); enc.str(name); enc.end(); };
  gl.linkProgram = function(p){ enc.begin(OP.LinkProgram); enc.w(p.__id); enc.end(); p.__intro = null; };
  gl.useProgram = function(p){ enc.begin(OP.UseProgram); enc.w(p ? p.__id : 0); enc.end(); };
  gl.validateProgram = function(){};
  // ---- 资源删除（场景切换 dispose 时调；发 Delete opcode 让 native 真正释放 GL 句柄，避免泄漏/崩溃）----
  gl.deleteProgram     = function(p){ if(p && p.__id){ enc.begin(OP.DeleteProgram); enc.w(p.__id); enc.end(); } };
  gl.deleteBuffer      = function(b){ if(b && b.__id){ enc.begin(OP.DeleteBuffer); enc.w(b.__id); enc.end(); } };
  gl.deleteTexture     = function(t){ if(t && t.__id){ var id=t.__id; enc.begin(OP.DeleteTexture); enc.w(id); enc.end(); t.__deleted=true; t.__id=0; } };
  gl.deleteFramebuffer = function(f){ if(f && f.__id){ enc.begin(OP.DeleteFramebuffer); enc.w(f.__id); enc.end(); } };
  gl.deleteVertexArray = function(v){ if(v && v.__id){ enc.begin(OP.DeleteVertexArray); enc.w(v.__id); enc.end(); } };
  gl.deleteRenderbuffer = function(rb){ if(rb && rb.__id){ enc.begin(OP.DeleteRenderbuffer); enc.w(rb.__id); enc.end(); } };
  gl.getShaderParameter = function(sh, pname){ return pname === K.COMPILE_STATUS ? true : (pname===K.DELETE_STATUS?false:0); };
  gl.getProgramParameter = function(p, pname){
    var info = ensureIntro(p);
    if (pname === K.LINK_STATUS || pname === K.VALIDATE_STATUS) return !!info.linkOk;
    if (pname === K.ACTIVE_UNIFORMS) return info.uniforms.length;
    if (pname === K.ACTIVE_ATTRIBUTES) return info.attributes.length;
    if (pname === K.DELETE_STATUS) return false;
    return 0;
  };
  gl.getActiveUniform = function(p, i){ var u = ensureIntro(p).uniforms[i]; return u ? { name:u.name, type:u.type, size:u.size } : null; };
  gl.getActiveAttrib  = function(p, i){ var a = ensureIntro(p).attributes[i]; return a ? { name:a.name, type:a.type, size:a.size } : null; };
  gl.getAttribLocation = function(p, name){ var as = ensureIntro(p).attributes; for (var k=0;k<as.length;k++) if (as[k].name===name) return as[k].location; return -1; };
  gl.getShaderInfoLog = function(){ return ''; };
  gl.getProgramInfoLog = function(){ return ''; };
  gl.getUniformLocation = function(p, name){ var id = ++enc.h.loc; enc.begin(OP.GetUniformLocation); enc.w(id); enc.w(p.__id); enc.str(name); enc.end(); return { __loc:id, name:name }; };

  // ---- uniforms ----
  gl.uniform1i  = function(loc, v){ if(!loc)return; enc.begin(OP.Uniform1i); enc.w(loc.__loc); enc.w(v); enc.end(); };
  gl.uniform4f  = function(loc, x,y,z,w){ if(!loc)return; enc.begin(OP.Uniform4f); enc.w(loc.__loc); enc.f(x); enc.f(y); enc.f(z); enc.f(w); enc.end(); };
  gl.uniform1iv = function(loc, arr){ if(!loc)return; enc.begin(OP.Uniform1iv); enc.w(loc.__loc); enc.w(arr.length); for(var k=0;k<arr.length;k++) enc.w(arr[k]); enc.end(); };
  // 浮点标量/向量 uniform（滤镜用）：统一走 Uniformf/Uniformfv
  function _uf(loc, comps, vals){ enc.begin(OP.Uniformf); enc.w(loc.__loc); enc.w(comps); for(var k=0;k<comps;k++) enc.f(vals[k]); enc.end(); }
  function _ufv(loc, comps, arr){ enc.begin(OP.Uniformfv); enc.w(loc.__loc); enc.w(comps); enc.w((arr.length/comps)|0); for(var k=0;k<arr.length;k++) enc.f(arr[k]); enc.end(); }
  gl.uniform1f = function(loc,x){ if(!loc)return; _uf(loc,1,[x]); };
  gl.uniform2f = function(loc,x,y){ if(!loc)return; _uf(loc,2,[x,y]); };
  gl.uniform3f = function(loc,x,y,z){ if(!loc)return; _uf(loc,3,[x,y,z]); };
  gl.uniform1fv = function(loc,arr){ if(!loc)return; _ufv(loc,1,arr); };
  gl.uniform2fv = function(loc,arr){ if(!loc)return; _ufv(loc,2,arr); };
  gl.uniform3fv = function(loc,arr){ if(!loc)return; _ufv(loc,3,arr); };
  gl.uniform4fv = function(loc,arr){ if(!loc)return; _ufv(loc,4,arr); };
  gl.uniformMatrix3fv = function(loc, transpose, arr){ if(!loc)return; enc.begin(OP.UniformMatrix3fv); enc.w(loc.__loc); enc.w(transpose?1:0); enc.w(1); for(var k=0;k<9;k++) enc.f(arr[k]); enc.end(); };
  gl.uniformMatrix4fv = function(loc, transpose, arr){ if(!loc)return; enc.begin(OP.UniformMatrix4fv); enc.w(loc.__loc); enc.w(transpose?1:0); enc.w(1); for(var k=0;k<16;k++) enc.f(arr[k]); enc.end(); };

  // ---- buffers ----
  gl.bindBuffer = function(target, buf){ enc.begin(OP.BindBuffer); enc.w(target); enc.w(buf ? buf.__id : 0); enc.end(); };
  gl.bufferData = function(target, dataOrSize, usage){
    if (typeof dataOrSize === 'number') { enc.begin(OP.BufferDataSize); enc.w(target); enc.w(dataOrSize); enc.w(usage); enc.end(); }
    else { var b8=viewU8(dataOrSize); __encEnsure(b8.length); enc.begin(OP.BufferData); enc.w(target); enc.w(usage); enc.payload(b8); enc.end(); }
  };
  gl.bufferSubData = function(target, offset, data){ var s8=viewU8(data); __encEnsure(s8.length); enc.begin(OP.BufferSubData); enc.w(target); enc.w(offset); enc.payload(s8); enc.end(); };

  // ---- textures ----
  gl.bindTexture = function(target, tex){ enc.begin(OP.BindTexture); enc.w(target); enc.w(tex ? tex.__id : 0); enc.end(); };
  gl.activeTexture = function(u){ enc.begin(OP.ActiveTexture); enc.w(u); enc.end(); };
  gl.texParameteri = function(t,p,v){ enc.begin(OP.TexParameteri); enc.w(t); enc.w(p); enc.w(v>>>0); enc.end(); };
  gl.texParameterf = function(){};
  gl.generateMipmap = function(t){ enc.begin(OP.GenerateMipmap); enc.w(t); enc.end(); };
  gl.pixelStorei = function(pname, param){
    // WebGL 专有伪参数（flip/premultiply/colorspace）GLES 不认，吞掉避免 GL_INVALID_ENUM
    if (pname===K.UNPACK_FLIP_Y_WEBGL || pname===K.UNPACK_PREMULTIPLY_ALPHA_WEBGL || pname===K.UNPACK_COLORSPACE_CONVERSION_WEBGL) return;
    enc.begin(OP.PixelStorei); enc.w(pname); enc.w((typeof param==='boolean')?(param?1:0):param); enc.end();
  };
  // 【黑图根治】此前 P.payload 在"缓冲当前剩余空间装不下"时静默丢数据写 0 长度(fps.log 实锤:
  // n=67108864B cap=67108864B,4096²=64MB 图集连空缓冲都装不下,永远被丢;32MB 图集则看时机偶发被丢)
  // → 纹理已分配无数据 → GLES 采样黑 → spine/图集黑色剪影,且 pixi 已消费 dirtyId 永不重试。
  // 两板斧:①编码前空间不足先 flushPending 冲刷(已完成命令立即解码执行,腾出整块缓冲);
  // ②payload 超过整个缓冲的巨型纹理(仅 RGBA/UNSIGNED_BYTE):null 分配 + 按行带(≤8MB)分块 TexSubImage2D。
  function __encEnsure(nBytes){
    var need = 1 + ((nBytes+3)>>2) + 32;   // 长度字 + payload 字数 + 命令头余量
    if (enc.i + need > enc.cap) flushPending();
  }
  function __texImageChunked(target, level, internalFormat, w, h, format, type, u8){
    enc.begin(OP.TexImage2D); enc.w(target); enc.w(level); enc.w(internalFormat); enc.w(w); enc.w(h); enc.w(format); enc.w(type);
    enc.payload(new Uint8Array(0)); enc.end();   // null 分配(decoder 补零初始化)
    var rowB = w*4, bandRows = Math.max(1, Math.floor((8<<20)/rowB)), y0 = 0;
    while (y0 < h) {
      var rows = Math.min(bandRows, h - y0), bandBytes = rows*rowB;
      __encEnsure(bandBytes);
      enc.begin(OP.TexSubImage2D); enc.w(target); enc.w(level); enc.w(0); enc.w(y0); enc.w(w); enc.w(rows); enc.w(format); enc.w(type);
      enc.payload(u8.subarray(y0*rowB, y0*rowB + bandBytes)); enc.end();
      y0 += rows;
    }
    globalThis.__texUpDataN=(globalThis.__texUpDataN|0)+1;
    if((globalThis.__texChunkN||0)<12){globalThis.__texChunkN=(globalThis.__texChunkN||0)+1;
      __log('[enc] 巨型纹理分块上传 '+w+'x'+h+' '+(u8.length>>20)+'MB(超命令缓冲,null分配+行带 TexSubImage2D)');}
  }
  gl.texImage2D = function(target, level, internalFormat, a,b,c,d,e,f){
    // 9-arg: (target, level, ifmt, w, h, border, format, type, pixels)
    if (arguments.length >= 8) {
      var w=a, h=b, format=d, type=e, pixels=f;
      var u8 = pixels ? viewU8(ArrayBuffer.isView(pixels)?pixels:new Uint8Array(0)) : new Uint8Array(0);
      if (u8.length > (enc.cap-64)*4 && format===6408 && type===5121) { __texImageChunked(target, level, internalFormat, w, h, format, type, u8); return; }
      if (u8.length) { __encEnsure(u8.length); globalThis.__texUpDataN=(globalThis.__texUpDataN|0)+1; }   // 真实像素上传计数(纹理"确已上传"验证器用)
      enc.begin(OP.TexImage2D); enc.w(target); enc.w(level); enc.w(internalFormat); enc.w(w); enc.w(h); enc.w(format); enc.w(type);
      enc.payload(u8);
      enc.end();
    } else {
      // 6-arg: (target, level, ifmt, format, type, source=Image)。原生已解码成预乘+翻正 RGBA。
      var format=a, type=b, source=c;
      var data = source && source.__data, sw = source && source.width, sh = source && source.height;
      if (data && sw && sh) {
        var du8 = new Uint8Array(data);
        if (du8.length > (enc.cap-64)*4 && format===6408 && type===5121) { __texImageChunked(target, level, internalFormat, sw, sh, format, type, du8); return; }
        __encEnsure(du8.length);
        globalThis.__texUpDataN=(globalThis.__texUpDataN|0)+1;
        enc.begin(OP.TexImage2D); enc.w(target); enc.w(level); enc.w(internalFormat); enc.w(sw); enc.w(sh); enc.w(format); enc.w(type);
        enc.payload(du8); enc.end();
      } else if ((globalThis.__texNoDataN||0)<20) {   // ⚠️ 上传被丢弃→纹理已分配但无数据→采样纯黑。此前仅 __PROF=1 打日志=静默黑图之源
        globalThis.__texNoDataN=(globalThis.__texNoDataN||0)+1;
        __log('[shim] texImage2D 6-arg 源无像素数据(丢弃上传→黑) src='+(source&&source.constructor&&source.constructor.name)
          +' w='+(source&&source.width)+' h='+(source&&source.height)+' complete='+(source&&source.complete)
          +' hasData='+!!(source&&source.__data)+' url='+String(source&&(source.src||source.__srcURL)||'').slice(-90));
      }
    }
  };
  gl.texSubImage2D = function(target, level, xoffset, yoffset, a,b,c,d,e){
    // 9-arg: (target, level, xoff, yoff, w, h, format, type, pixels)
    if (arguments.length >= 9) {
      var w=a, h=b, format=c, type=d, pixels=e;
      var su8 = pixels ? viewU8(ArrayBuffer.isView(pixels)?pixels:new Uint8Array(0)) : new Uint8Array(0);
      if (su8.length) { __encEnsure(su8.length); globalThis.__texUpDataN=(globalThis.__texUpDataN|0)+1; }   // pixi 同尺寸重传走 subimage,也算真实上传
      enc.begin(OP.TexSubImage2D); enc.w(target); enc.w(level); enc.w(xoffset); enc.w(yoffset); enc.w(w); enc.w(h); enc.w(format); enc.w(type);
      enc.payload(su8);
      enc.end();
    } else {
      // 7-arg: (target, level, xoff, yoff, format, type, source=Image/canvas)。原生已解码成预乘+翻正 RGBA。
      var format=a, type=b, source=c;
      var data = source && source.__data, sw = source && source.width, sh = source && source.height;
      if (data && sw && sh) {
        var sdu8 = new Uint8Array(data);
        __encEnsure(sdu8.length);
        globalThis.__texUpDataN=(globalThis.__texUpDataN|0)+1;
        enc.begin(OP.TexSubImage2D); enc.w(target); enc.w(level); enc.w(xoffset); enc.w(yoffset); enc.w(sw); enc.w(sh); enc.w(format); enc.w(type);
        enc.payload(sdu8); enc.end();
      } else { if(!globalThis.__texSubWarned){ globalThis.__texSubWarned=1; __log('[shim] texSubImage2D 源无像素数据'); } }
    }
  };
  // ASTC 压缩纹理上传(pixi CompressedTextureResource → gl.compressedTexImage2D):(target,level,internalformat,w,h,border,data)
  gl.compressedTexImage2D = function(target, level, internalformat, w, h, border, data){
    var u8 = data ? (ArrayBuffer.isView(data) ? viewU8(data) : new Uint8Array(0)) : new Uint8Array(0);
    // 诊断:大图(≥1024宽)或异常尺寸时打一行,核实上传是否正确/被丢弃
    if(globalThis.__ASTC_DBG!==0 && (w>=1024 || u8.length<16)){
      // ASTC 4x4 期望字节数 = ceil(w/4)*ceil(h/4)*16
      var exp=(((w+3)>>2)*((h+3)>>2))*16;
      // 2048宽的加校验和(对比源 bar:sum=256625359 首428888ed 中52880329 尾00000000)
      var ck='';
      if(w>=2048 && u8.length>=16){ var s=0; for(var __i=0;__i<u8.length;__i++) s=(s+u8[__i])>>>0;
        var hx=function(o){ return ((u8[o]<<24|u8[o+1]<<16|u8[o+2]<<8|u8[o+3])>>>0).toString(16); };
        ck=' sum='+s+' 首'+hx(0)+' 中'+hx(u8.length>>1)+' 尾'+hx(u8.length-4); }
      globalThis.__log&&__log('[astc-up] '+w+'x'+h+' fmt=0x'+(internalformat>>>0).toString(16)+' 数据'+u8.length+'B 期望'+exp+'B'+(u8.length!==exp?' ⚠️大小不符':'')+(u8.length<16?' ⚠️空数据':'')+ck);
    }
    if (u8.length) { __encEnsure(u8.length); globalThis.__texUpDataN=(globalThis.__texUpDataN|0)+1; }
    enc.begin(OP.CompressedTexImage2D); enc.w(target); enc.w(level); enc.w(internalformat); enc.w(w>>>0); enc.w(h>>>0);
    enc.payload(u8); enc.end();
  };
  gl.compressedTexSubImage2D = function(){};   // 目前资源无 subimage 压缩上传;留空避免报错

  // ---- VAO / 顶点 ----
  gl.bindVertexArray = function(vao){ enc.begin(OP.BindVertexArray); enc.w(vao ? vao.__id : 0); enc.end(); };
  gl.vertexAttribPointer = function(idx, size, type, norm, stride, off){ enc.begin(OP.VertexAttribPointer); enc.w(idx); enc.w(size); enc.w(type); enc.w(norm?1:0); enc.w(stride); enc.w(off); enc.end(); };
  gl.enableVertexAttribArray = function(idx){ enc.begin(OP.EnableVertexAttribArray); enc.w(idx); enc.end(); };
  gl.disableVertexAttribArray = function(){};
  gl.vertexAttribDivisor = function(){}; // 实例化（本 demo 不用）

  // ---- framebuffer ----
  gl.bindFramebuffer = function(target, fb){ enc.begin(OP.BindFramebuffer); enc.w(target); enc.w(fb ? fb.__id : 0); enc.end(); };
  gl.framebufferTexture2D = function(target, attachment, textarget, tex, level){ enc.begin(OP.FramebufferTexture2D); enc.w(target); enc.w(attachment); enc.w(textarget); enc.w(tex ? tex.__id : 0); enc.w(level||0); enc.end(); };
  // ---- renderbuffer（PopupHomePage 等带 mask 的 RenderTexture 需 depth/stencil 附件）----
  gl.bindRenderbuffer = function(target, rb){ enc.begin(OP.BindRenderbuffer); enc.w(target); enc.w(rb ? rb.__id : 0); enc.end(); };
  gl.renderbufferStorage = function(target, internalformat, w, h){ enc.begin(OP.RenderbufferStorage); enc.w(target); enc.w(internalformat); enc.w(w); enc.w(h); enc.end(); };
  gl.renderbufferStorageMultisample = function(target, samples, internalformat, w, h){ enc.begin(OP.RenderbufferStorage); enc.w(target); enc.w(internalformat); enc.w(w); enc.w(h); enc.end(); }; // 退化为非多重采样
  gl.framebufferRenderbuffer = function(target, attachment, rbtarget, rb){ enc.begin(OP.FramebufferRenderbuffer); enc.w(target); enc.w(attachment); enc.w(rbtarget); enc.w(rb ? rb.__id : 0); enc.end(); };
  gl.checkFramebufferStatus = function(){ return K.FRAMEBUFFER_COMPLETE; };

  // ---- 状态 ----
  gl.viewport = function(x,y,w,h){ enc.begin(OP.Viewport); enc.w(x); enc.w(y); enc.w(w); enc.w(h); enc.end(); };
  // ---- S10: Mask（scissor 矩形 / stencil 任意形状）----
  gl.scissor = function(x,y,w,h){ enc.begin(OP.Scissor); enc.w(x); enc.w(y); enc.w(w); enc.w(h); enc.end(); };
  gl.stencilFunc = function(func,ref,mask){ enc.begin(OP.StencilFunc); enc.w(func); enc.w(ref); enc.w(mask>>>0); enc.end(); };
  gl.stencilOp = function(sf,zf,zp){ enc.begin(OP.StencilOp); enc.w(sf); enc.w(zf); enc.w(zp); enc.end(); };
  gl.clearStencil = function(s){ enc.begin(OP.ClearStencil); enc.w(s>>>0); enc.end(); };
  gl.clearColor = function(r,g,b,a){ enc.begin(OP.ClearColor); enc.f(r); enc.f(g); enc.f(b); enc.f(a); enc.end(); };
  gl.clear = function(m){ enc.begin(OP.Clear); enc.w(m); enc.end(); };
  gl.enable = function(c){ enc.begin(OP.Enable); enc.w(c); enc.end(); };
  gl.disable = function(c){ enc.begin(OP.Disable); enc.w(c); enc.end(); };
  gl.depthMask = function(f){ enc.begin(OP.DepthMask); enc.w(f?1:0); enc.end(); };
  gl.colorMask = function(r,g,b,a){ enc.begin(OP.ColorMask); enc.w(r?1:0); enc.w(g?1:0); enc.w(b?1:0); enc.w(a?1:0); enc.end(); };
  gl.frontFace = function(m){ enc.begin(OP.FrontFace); enc.w(m); enc.end(); };
  gl.blendFunc = function(s,d){ enc.begin(OP.BlendFunc); enc.w(s); enc.w(d); enc.end(); };
  gl.blendFuncSeparate = function(sr,dr,sa,da){ enc.begin(OP.BlendFuncSeparate); enc.w(sr); enc.w(dr); enc.w(sa); enc.w(da); enc.end(); };
  gl.blendEquationSeparate = function(mr,ma){ enc.begin(OP.BlendEquationSeparate); enc.w(mr); enc.w(ma); enc.end(); };
  gl.blendEquation = function(){};

  // ---- 绘制 ----
  gl.drawArrays = function(mode,first,count){ enc.begin(OP.DrawArrays); enc.w(mode); enc.w(first); enc.w(count); enc.end(); };
  gl.drawElements = function(mode,count,type,offset){ enc.begin(OP.DrawElements); enc.w(mode); enc.w(count); enc.w(type); enc.w(offset); enc.end(); };
  gl.flush = function(){ enc.begin(OP.Flush); enc.end(); };
  gl.finish = function(){};

  // ---- 查询（canned，无命令）----
  gl.getParameter = function(p){
    switch (p) {
      case K.VERSION: return 'WebGL 2.0 (pixi-native)';
      case K.SHADING_LANGUAGE_VERSION: return 'WebGL GLSL ES 3.00 (pixi-native)';
      case K.VENDOR: return 'pixi-native'; case K.RENDERER: return 'pixi-native ANGLE/Metal';
      case K.MAX_TEXTURE_SIZE: case K.MAX_CUBE_MAP_TEXTURE_SIZE: case K.MAX_RENDERBUFFER_SIZE: case K.MAX_3D_TEXTURE_SIZE: return 4096;
      case K.MAX_TEXTURE_IMAGE_UNITS: return 16;
      case K.MAX_COMBINED_TEXTURE_IMAGE_UNITS: return 32;
      case K.MAX_VERTEX_TEXTURE_IMAGE_UNITS: return 16;
      case K.MAX_VERTEX_ATTRIBS: return 16;
      case K.MAX_VERTEX_UNIFORM_VECTORS: case K.MAX_FRAGMENT_UNIFORM_VECTORS: return 1024;
      case K.MAX_VARYING_VECTORS: return 30;
      case K.MAX_SAMPLES: return 4; case K.MAX_DRAW_BUFFERS: return 8; case K.MAX_COLOR_ATTACHMENTS: return 8;
      case K.MAX_ARRAY_TEXTURE_LAYERS: return 256;
      case K.MAX_TEXTURE_MAX_ANISOTROPY_EXT: return 16;
      case K.MAX_UNIFORM_BUFFER_BINDINGS: return 24; case K.UNIFORM_BUFFER_OFFSET_ALIGNMENT: return 256;
      case K.SCISSOR_BOX: case K.VIEWPORT: case K.MAX_VIEWPORT_DIMS: return new Int32Array([0,0,4096,4096]);
      case K.ACTIVE_TEXTURE: return K.TEXTURE0;
      case K.UNPACK_ALIGNMENT: case K.PACK_ALIGNMENT: return 4;
      default: return 0;
    }
  };
  gl.getContextAttributes = function(){ return { alpha:false, antialias:false, depth:false, stencil:true, premultipliedAlpha:true, preserveDrawingBuffer:false, powerPreference:'default' }; };
  // Keep the advertised compressed-texture capability aligned with the native platform contract.
  // iOS enables ASTC through __isMobile; native macOS injects __NATIVE_FORCE_ASTC so both shells
  // select the same authored KTX2 siblings. Browser/desktop H5 keeps its normal WebP fallback.
  // __NATIVE_DISABLE_ASTC remains an explicit A/B escape hatch for texture diagnostics.
  var nativeAstcEnabled=globalThis.__NATIVE_DISABLE_ASTC!==true&&
    (globalThis.__NATIVE_FORCE_ASTC===true||globalThis.__isMobile===true);
  gl.getExtension = function(name){
    // anisotropic 提供常量；其余 null（webgl2 多为核心）
    if (name && name.indexOf('texture_filter_anisotropic')>=0) return { TEXTURE_MAX_ANISOTROPY_EXT:0x84FE, MAX_TEXTURE_MAX_ANISOTROPY_EXT:0x84FF };
    // ASTC 压缩纹理:iOS 部署目标 13.0(A9+)全系原生支持 ASTC LDR。返回扩展对象 → 游戏 isASTCEnabled()=true → 选 .ktx2 直传
    if (nativeAstcEnabled && name && name.indexOf('compressed_texture_astc')>=0) return {
      COMPRESSED_RGBA_ASTC_4x4_KHR:0x93B0, COMPRESSED_RGBA_ASTC_5x4_KHR:0x93B1, COMPRESSED_RGBA_ASTC_5x5_KHR:0x93B2,
      COMPRESSED_RGBA_ASTC_6x5_KHR:0x93B3, COMPRESSED_RGBA_ASTC_6x6_KHR:0x93B4, COMPRESSED_RGBA_ASTC_8x5_KHR:0x93B5,
      COMPRESSED_RGBA_ASTC_8x6_KHR:0x93B6, COMPRESSED_RGBA_ASTC_8x8_KHR:0x93B7, COMPRESSED_RGBA_ASTC_10x5_KHR:0x93B8,
      COMPRESSED_RGBA_ASTC_10x6_KHR:0x93B9, COMPRESSED_RGBA_ASTC_10x8_KHR:0x93BA, COMPRESSED_RGBA_ASTC_10x10_KHR:0x93BB,
      COMPRESSED_RGBA_ASTC_12x10_KHR:0x93BC, COMPRESSED_RGBA_ASTC_12x12_KHR:0x93BD,
      COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR:0x93D0, COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR:0x93D1, COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR:0x93D2,
      COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR:0x93D3, COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR:0x93D4, COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR:0x93D5,
      COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR:0x93D6, COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR:0x93D7, COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR:0x93D8,
      COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR:0x93D9, COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR:0x93DA, COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR:0x93DB,
      COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR:0x93DC, COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR:0x93DD,
      getSupportedProfiles:function(){ return ['ldr']; }
    };
    return null;
  };
  gl.getSupportedExtensions = function(){ return nativeAstcEnabled?['WEBGL_compressed_texture_astc']:[]; };
  gl.getShaderPrecisionFormat = function(){ return { precision:23, rangeMin:127, rangeMax:127 }; }; // highp float
  // 如实上报不支持 MSAA:命令协议无 blitFramebuffer/renderbufferStorageMultisample(已退化),
  // 谎报 [4,2,1] 会诱导 pixi 走 msaa renderbuffer+blit resolve 路径 → FBO 坏/readPixels 读不到。
  gl.getInternalformatParameter = function(){ return new Int32Array([0]); };
  gl.getError = function(){ return K.NO_ERROR; };
  gl.isContextLost = function(){ return false; };
  gl.hint = function(){};
  // pixi Extract(截图→客服邮件附件等)用:先 flush 命令缓冲让排队的渲染真正执行,再从当前绑定 FBO 同步读回。
  gl.readPixels = function(x,y,w,h,format,type,buf){
    if (typeof __glReadPixels !== 'function' || !buf){ __log('[shim] readPixels 不可用(native 无 __glReadPixels)'); return; }
    flushPending();
    var ab = __glReadPixels(x|0, y|0, w|0, h|0);   // ArrayBuffer,RGBA,GL 行序(pixi 自行 flipY)
    if (!ab) return;
    var src = new Uint8Array(ab);
    var dst = (buf.buffer !== undefined) ? new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength) : new Uint8Array(buf);
    dst.set(src.subarray(0, Math.min(src.length, dst.length)));
    var nz = 0; for (var i = 0; i < src.length; i += 4097) if (src[i]) nz++;   // 抽样统计,验证截图非全黑
    __log('[shim] readPixels '+w+'x'+h+' 非零抽样 '+nz+'/'+Math.ceil(src.length/4097));
  };

  return gl;
}

// ImageData 垫片(pixi Extract.canvas 组装像素→putImageData 用)
globalThis.ImageData = function(a, b, c){
  if (typeof a === 'number'){ this.width = a|0; this.height = b|0; this.data = new Uint8ClampedArray(this.width*this.height*4); }
  else { this.data = a; this.width = b|0; this.height = (c==null ? ((a.length/4/(b|0))|0) : c|0); }
};
