// ===== OP 表（必须与 protocol.h 枚举顺序一致）=====
var OP = {
  Nop:0, CreateShader:1, ShaderSource:2, CompileShader:3, CreateProgram:4, AttachShader:5,
  BindAttribLocation:6, LinkProgram:7, UseProgram:8, GetUniformLocation:9, Uniform1i:10,
  CreateBuffer:11, BindBuffer:12, BufferData:13, CreateTexture:14, BindTexture:15,
  TexImage2D:16, TexParameteri:17, ActiveTexture:18, VertexAttribPointer:19,
  EnableVertexAttribArray:20, Viewport:21, ClearColor:22, Clear:23, Enable:24, Disable:25, DrawArrays:26,
  BufferDataSize:27, BufferSubData:28, DrawElements:29, CreateVertexArray:30, BindVertexArray:31,
  CreateFramebuffer:32, BindFramebuffer:33, DeleteShader:34, GenerateMipmap:35, Flush:36,
  PixelStorei:37, FrontFace:38, DepthMask:39, BlendFunc:40, BlendFuncSeparate:41,
  BlendEquationSeparate:42, Uniform4f:43, Uniform1iv:44, UniformMatrix3fv:45, UniformMatrix4fv:46,
  FramebufferTexture2D:47, Uniformf:48, Uniformfv:49,
  Scissor:50, StencilFunc:51, StencilOp:52, ColorMask:53, ClearStencil:54,
  DeleteBuffer:55, DeleteTexture:56, DeleteFramebuffer:57, DeleteVertexArray:58, DeleteProgram:59,
  TexSubImage2D:60,
  CreateRenderbuffer:61, BindRenderbuffer:62, RenderbufferStorage:63, FramebufferRenderbuffer:64, DeleteRenderbuffer:65,
  QuadRefBatch:66,   // conch P1:count + nodeIdx 流(native 从场景图 slot 组 quad 合批)
  CompressedTexImage2D:67   // ASTC 压缩纹理上传(target,level,ifmt,w,h,<压缩字节>)
};

// ===== WebGL2 常量（pixi 以 gl.XXX 读取）=====
var K = {
  DEPTH_BUFFER_BIT:0x0100, STENCIL_BUFFER_BIT:0x0400, COLOR_BUFFER_BIT:0x4000,
  POINTS:0, LINES:1, LINE_LOOP:2, LINE_STRIP:3, TRIANGLES:4, TRIANGLE_STRIP:5, TRIANGLE_FAN:6,
  ZERO:0, ONE:1, SRC_COLOR:0x0300, ONE_MINUS_SRC_COLOR:0x0301, SRC_ALPHA:0x0302,
  ONE_MINUS_SRC_ALPHA:0x0303, DST_ALPHA:0x0304, ONE_MINUS_DST_ALPHA:0x0305,
  DST_COLOR:0x0306, ONE_MINUS_DST_COLOR:0x0307, SRC_ALPHA_SATURATE:0x0308,
  FUNC_ADD:0x8006, BLEND_EQUATION:0x8009, BLEND_EQUATION_RGB:0x8009, BLEND_EQUATION_ALPHA:0x883D,
  FUNC_SUBTRACT:0x800A, FUNC_REVERSE_SUBTRACT:0x800B, MIN:0x8007, MAX:0x8008,
  BLEND_DST_RGB:0x80C8, BLEND_SRC_RGB:0x80C9, BLEND_DST_ALPHA:0x80CA, BLEND_SRC_ALPHA:0x80CB,
  CONSTANT_COLOR:0x8001, ONE_MINUS_CONSTANT_COLOR:0x8002, CONSTANT_ALPHA:0x8003,
  ONE_MINUS_CONSTANT_ALPHA:0x8004, BLEND_COLOR:0x8005,
  ARRAY_BUFFER:0x8892, ELEMENT_ARRAY_BUFFER:0x8893, STREAM_DRAW:0x88E0, STATIC_DRAW:0x88E4, DYNAMIC_DRAW:0x88E8,
  FRONT:0x0404, BACK:0x0405, FRONT_AND_BACK:0x0408,
  CULL_FACE:0x0B44, BLEND:0x0BE2, DITHER:0x0BD0, STENCIL_TEST:0x0B90, DEPTH_TEST:0x0B71,
  SCISSOR_TEST:0x0C11, POLYGON_OFFSET_FILL:0x8037, SAMPLE_ALPHA_TO_COVERAGE:0x809E, SAMPLE_COVERAGE:0x80A0,
  NO_ERROR:0, CW:0x0900, CCW:0x0901,
  DEPTH_WRITEMASK:0x0B72, VIEWPORT:0x0BA2, SCISSOR_BOX:0x0C10,
  UNPACK_ALIGNMENT:0x0CF5, PACK_ALIGNMENT:0x0D05, MAX_TEXTURE_SIZE:0x0D33, MAX_VIEWPORT_DIMS:0x0D3A,
  TEXTURE_BINDING_2D:0x8069, SAMPLE_BUFFERS:0x80A8, SAMPLES:0x80A9,
  NEVER:0x0200, LESS:0x0201, EQUAL:0x0202, LEQUAL:0x0203, GREATER:0x0204, NOTEQUAL:0x0205, GEQUAL:0x0206, ALWAYS:0x0207,
  KEEP:0x1E00, REPLACE:0x1E01, INCR:0x1E02, DECR:0x1E03, INVERT:0x150A, INCR_WRAP:0x8507, DECR_WRAP:0x8508,
  VENDOR:0x1F00, RENDERER:0x1F01, VERSION:0x1F02, EXTENSIONS:0x1F03,
  NEAREST:0x2600, LINEAR:0x2601, NEAREST_MIPMAP_NEAREST:0x2700, LINEAR_MIPMAP_NEAREST:0x2701,
  NEAREST_MIPMAP_LINEAR:0x2702, LINEAR_MIPMAP_LINEAR:0x2703,
  TEXTURE_MAG_FILTER:0x2800, TEXTURE_MIN_FILTER:0x2801, TEXTURE_WRAP_S:0x2802, TEXTURE_WRAP_T:0x2803,
  TEXTURE_2D:0x0DE1, TEXTURE:0x1702, TEXTURE_CUBE_MAP:0x8513, MAX_CUBE_MAP_TEXTURE_SIZE:0x851C,
  TEXTURE_CUBE_MAP_POSITIVE_X:0x8515, TEXTURE_CUBE_MAP_NEGATIVE_X:0x8516,
  TEXTURE_CUBE_MAP_POSITIVE_Y:0x8517, TEXTURE_CUBE_MAP_NEGATIVE_Y:0x8518,
  TEXTURE_CUBE_MAP_POSITIVE_Z:0x8519, TEXTURE_CUBE_MAP_NEGATIVE_Z:0x851A,
  TEXTURE_BINDING_CUBE_MAP:0x8514,
  TEXTURE0:0x84C0, ACTIVE_TEXTURE:0x84E0, MAX_COMBINED_TEXTURE_IMAGE_UNITS:0x8B4D,
  MAX_TEXTURE_IMAGE_UNITS:0x8872, MAX_VERTEX_TEXTURE_IMAGE_UNITS:0x8B4C,
  REPEAT:0x2901, CLAMP_TO_EDGE:0x812F, MIRRORED_REPEAT:0x8370,
  FLOAT_VEC2:0x8B50, FLOAT_VEC3:0x8B51, FLOAT_VEC4:0x8B52, INT_VEC2:0x8B53, INT_VEC3:0x8B54, INT_VEC4:0x8B55,
  BOOL:0x8B56, FLOAT_MAT2:0x8B5A, FLOAT_MAT3:0x8B5B, FLOAT_MAT4:0x8B5C, SAMPLER_2D:0x8B5E, SAMPLER_CUBE:0x8B60,
  COMPILE_STATUS:0x8B81, LINK_STATUS:0x8B82, VALIDATE_STATUS:0x8B83, DELETE_STATUS:0x8B80,
  ATTACHED_SHADERS:0x8B85, ACTIVE_UNIFORMS:0x8B86, ACTIVE_ATTRIBUTES:0x8B89,
  SHADER_TYPE:0x8B4F, FRAGMENT_SHADER:0x8B30, VERTEX_SHADER:0x8B31,
  MAX_VERTEX_ATTRIBS:0x8869, MAX_VERTEX_UNIFORM_VECTORS:0x8DFB, MAX_VARYING_VECTORS:0x8DFC,
  MAX_FRAGMENT_UNIFORM_VECTORS:0x8DFD, SHADING_LANGUAGE_VERSION:0x8B8C, CURRENT_PROGRAM:0x8B8D,
  BYTE:0x1400, UNSIGNED_BYTE:0x1401, SHORT:0x1402, UNSIGNED_SHORT:0x1403, INT:0x1404, UNSIGNED_INT:0x1405,
  FLOAT:0x1406, HALF_FLOAT:0x140B,
  DEPTH_COMPONENT:0x1902, ALPHA:0x1906, RGB:0x1907, RGBA:0x1908, LUMINANCE:0x1909, LUMINANCE_ALPHA:0x190A,
  UNSIGNED_SHORT_4_4_4_4:0x8033, UNSIGNED_SHORT_5_5_5_1:0x8034, UNSIGNED_SHORT_5_6_5:0x8363,
  FRAMEBUFFER:0x8D40, RENDERBUFFER:0x8D41, COLOR_ATTACHMENT0:0x8CE0, DEPTH_ATTACHMENT:0x8D00,
  STENCIL_ATTACHMENT:0x8D20, DEPTH_STENCIL_ATTACHMENT:0x821A, DEPTH_STENCIL:0x84F9, STENCIL_INDEX8:0x8D48,
  FRAMEBUFFER_COMPLETE:0x8CD5, MAX_RENDERBUFFER_SIZE:0x84E8,
  UNPACK_FLIP_Y_WEBGL:0x9240, UNPACK_PREMULTIPLY_ALPHA_WEBGL:0x9241,
  UNPACK_COLORSPACE_CONVERSION_WEBGL:0x9243, BROWSER_DEFAULT_WEBGL:0x9244,
  // WebGL2
  READ_BUFFER:0x0C02, UNPACK_ROW_LENGTH:0x0CF2, MAX_3D_TEXTURE_SIZE:0x8073, MAX_DRAW_BUFFERS:0x8824,
  TEXTURE_2D_ARRAY:0x8C1A, TEXTURE_3D:0x806F, MAX_ARRAY_TEXTURE_LAYERS:0x88FF,
  RGBA8:0x8058, RGB8:0x8051, R8:0x8229, RG8:0x822B, SRGB8_ALPHA8:0x8C43, RGBA32F:0x8814, RGBA16F:0x881A,
  R16F:0x822D, R32F:0x822E, DEPTH24_STENCIL8:0x88F0, DEPTH_COMPONENT24:0x81A6,
  DRAW_FRAMEBUFFER:0x8CA9, READ_FRAMEBUFFER:0x8CA8, UNIFORM_BUFFER:0x8A11, MAX_SAMPLES:0x8D57,
  VERTEX_ARRAY_BINDING:0x85B5, COLOR_ATTACHMENT1:0x8CE1, MAX_COLOR_ATTACHMENTS:0x8CDF,
  HALF_FLOAT_OES:0x8D61, RED:0x1903, RG:0x8227, MAX_TEXTURE_MAX_ANISOTROPY_EXT:0x84FF,
  RENDERBUFFER_SAMPLES:0x8CAB, FRAMEBUFFER_BINDING:0x8CA6, TEXTURE_WRAP_R:0x8072,
  UNIFORM_BUFFER_OFFSET_ALIGNMENT:0x8A34, MAX_UNIFORM_BUFFER_BINDINGS:0x8A2F,
  // sampler 类型（uniform 反射可能遇到）
  SAMPLER_2D_ARRAY:0x8DC1, SAMPLER_3D:0x8B5F, INT_SAMPLER_2D:0x8DCA, UNSIGNED_INT_SAMPLER_2D:0x8DD2,
  UNSIGNED_INT_VEC2:0x8DC6, UNSIGNED_INT_VEC3:0x8DC7, UNSIGNED_INT_VEC4:0x8DC8,
  HIGH_FLOAT:0x8DF2, MEDIUM_FLOAT:0x8DF1, LOW_FLOAT:0x8DF0
};

// ===== 命令编码器（共享 __cmd，零拷贝）=====
var _fb = new ArrayBuffer(4), _ff = new Float32Array(_fb), _fu = new Uint32Array(_fb);
function fbits(x){ _ff[0] = x; return _fu[0]; }

function Enc(buffer) {
  this.u = new Uint32Array(buffer);   // 字视图
  this.b = new Uint8Array(buffer);    // 字节视图（payload 快拷）
  this.i = 0; this.cmds = 0; this.cur = 0;
  this.cap = this.u.length;           // 容量(words)
  this.RESERVE = this.cap >> 1;       // 命令边界处剩余不足一半(16MB)即先 flush，留足单条最大纹理命令的空间
  this.h = { shader:0, program:0, buffer:0, texture:0, vao:0, fb:0, loc:0, renderbuffer:0 };
}
var P = Enc.prototype;
P.reset = function(){ this.i = 0; this.cmds = 0; };
// 命令边界处快满则中途 flush：cbFlush 只解码不 swap，同一帧可安全分多次提交，根治
// 回大厅等单帧重传大量纹理时 payload 累积超 32MB 导致 b.set 抛 "Range out of bounds"。
P.begin = function(op){
  if (this.i > 0 && (this.cap - this.i) < this.RESERVE) { __flush(this.i); this.reset(); }
  this.cur = this.i; this.u[this.i++] = (op<<24)>>>0; this.cmds++;
};
P.end   = function(){ var w = this.i - this.cur; this.u[this.cur] = ((this.u[this.cur]&0xFF000000)|(w&0xFFFFFF))>>>0; };
P.w = function(v){ if (v===undefined||v===null||v!==v){ if((globalThis.__encWarn=(globalThis.__encWarn||0)+1)<=5) __log('[enc] 警告: 参数 undefined/NaN → 0 (op='+(this.u[this.cur]>>>24)+' 第'+(this.i-this.cur)+'参, 仅报前5次)'); } this.u[this.i++] = v>>>0; };
P.f = function(v){ this.u[this.i++] = fbits(v); };
P.str = function(s){
  var n = s.length; this.u[this.i++] = n;
  var byteBase = this.i*4;
  for (var j=0;j<n;j++) this.b[byteBase+j] = s.charCodeAt(j) & 0xff;
  var words = (n+3)>>2; for (var k=n; k<words*4; k++) this.b[byteBase+k]=0;
  this.i += words;
};
P.payload = function(srcU8){  // srcU8: Uint8Array
  var n = srcU8.length;
  // 兜底：单条 payload 比整个缓冲还大(极端超大纹理) → 写 0 长度避免解码错位/越界崩溃(该纹理留空，不崩进程)
  if ((this.i + 1 + ((n+3)>>2)) > this.cap) {
    __log('[enc] payload 超容量丢弃: n='+n+'B cap='+(this.cap*4)+'B');
    this.u[this.i++] = 0; return;
  }
  this.u[this.i++] = n;
  var byteBase = this.i*4;
  this.b.set(srcU8, byteBase);
  var words = (n+3)>>2; for (var k=n; k<words*4; k++) this.b[byteBase+k]=0;
  this.i += words;
};

// ===== host 桥接 =====
var enc;
function flushPending(){ if (enc.i > 0) { __flush(enc.i); enc.reset(); } }
globalThis.__encFlush = flushPending;   // conch 逃生舱回调末尾用:命令立即解码执行,保证 native/JS 绘制交错顺序

// ArrayBuffer 或 ArrayBufferView → 覆盖其字节的 Uint8Array（pixi 两种都会传）
function viewU8(v){ return (v instanceof ArrayBuffer) ? new Uint8Array(v) : new Uint8Array(v.buffer, v.byteOffset, v.byteLength); }

// ===== 句柄 =====
var HID = 0;
function mkH(kind, id){ return { __id:id, __kind:kind }; }
