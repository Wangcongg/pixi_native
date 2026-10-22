// 命令缓冲协议：JS/native 共享的指令编码格式。
// 每条命令 4 字节(uint32 word)对齐：
//   [header: opcode(高8位) | words(低24位, 含 header 的总字数)]
//   [args... 按 opcode 固定布局]
//   [可选变长 payload: 字符串/像素/顶点数据, u32 对齐, 前置 byteLen]
// 对象句柄一律客户端分配 uint32 id（每类独立命名空间, 0=null），
// 解码端维护 client_id -> GLuint 映射，createXxx 零往返。
#pragma once
#include <cstdint>

namespace cmd {

enum class Op : uint8_t {
    Nop = 0,
    // 着色器/程序
    CreateShader = 1,        // cid, glType
    ShaderSource,            // cid, <str>
    CompileShader,           // cid
    CreateProgram,           // cid
    AttachShader,            // programCid, shaderCid
    BindAttribLocation,      // programCid, index, <str>
    LinkProgram,             // programCid
    UseProgram,              // programCid
    GetUniformLocation,      // locCid, programCid, <str>
    Uniform1i,               // locCid, int
    // 缓冲
    CreateBuffer,            // cid
    BindBuffer,              // target, bufferCid
    BufferData,              // target, usage, <bytes>
    // 纹理
    CreateTexture,           // cid
    BindTexture,             // target, textureCid
    TexImage2D,              // target, level, internalFormat, w, h, format, type, <bytes>
    TexParameteri,           // target, pname, param
    ActiveTexture,           // unit
    // 顶点属性
    VertexAttribPointer,     // index, size, type, normalized, stride, offset
    EnableVertexAttribArray, // index
    // 状态 & 绘制
    Viewport,                // x, y, w, h
    ClearColor,              // r, g, b, a (float bits)
    Clear,                   // mask
    Enable,                  // cap
    Disable,                 // cap
    DrawArrays,              // mode, first, count
    // ---- S4: pixi v7 sprite 路径补充（trace-driven）----
    BufferDataSize,          // target, size, usage（仅分配，无数据 → 不塞零负载）
    BufferSubData,           // target, offset, <bytes>
    DrawElements,            // mode, count, type, offset
    CreateVertexArray,       // cid
    BindVertexArray,         // vaoCid (0=unbind)
    CreateFramebuffer,       // cid
    BindFramebuffer,         // target, fbCid (0=默认帧缓冲)
    DeleteShader,            // cid
    GenerateMipmap,          // target
    Flush,                   // -
    PixelStorei,             // pname, param
    FrontFace,               // mode
    DepthMask,               // flag
    BlendFunc,               // sfactor, dfactor
    BlendFuncSeparate,       // srcRGB, dstRGB, srcA, dstA
    BlendEquationSeparate,   // modeRGB, modeAlpha
    Uniform4f,               // locCid, x, y, z, w (float bits)
    Uniform1iv,              // locCid, count, <count ints>
    UniformMatrix3fv,        // locCid, transpose, count, <count*9 floats>
    UniformMatrix4fv,        // locCid, transpose, count, <count*16 floats>
    // ---- S9: 离屏 FBO（滤镜/RenderTexture）----
    FramebufferTexture2D,    // target, attachment, textarget, textureCid, level
    Uniformf,                // locCid, comps(1-4), <comps floats>      —— uniform{1,2,3,4}f
    Uniformfv,               // locCid, comps(1-4), count, <comps*count floats> —— uniform{1,2,3,4}fv
    // ---- S10: Mask（scissor 矩形遮罩 / stencil 任意形状遮罩）----
    Scissor,                 // x, y, w, h
    StencilFunc,             // func, ref, mask
    StencilOp,               // sfail, dpfail, dppass
    ColorMask,               // r, g, b, a (各 0/1)
    ClearStencil,            // s
    // ---- 资源删除（场景切换时释放 GL 资源；缺失会致 pixi dispose 调 gl.deleteXxx 时崩溃/泄漏）----
    DeleteBuffer,            // cid
    DeleteTexture,           // cid
    DeleteFramebuffer,       // cid
    DeleteVertexArray,       // cid
    DeleteProgram,           // cid
    TexSubImage2D,           // target, level, xoff, yoff, w, h, format, type, <payload pixels>
    // ---- renderbuffer（带 mask 的 RenderTexture 的 depth/stencil 附件；缺失会致 gl.createRenderbuffer 抛错冻屏）----
    CreateRenderbuffer,      // cid
    BindRenderbuffer,        // target, rbCid (0=unbind)
    RenderbufferStorage,     // target, internalformat, w, h
    FramebufferRenderbuffer, // target, attachment, rbtarget, rbCid
    DeleteRenderbuffer,      // cid
    // ---- conch P1:Quad 引用流(JS 渲染遍历保序,普通 sprite 只发节点号;C++ 从场景图 slot
    //      读 world+rect/uv/tint 组顶点合批;逃逸节点走原 pixi 命令,段边界由编码顺序天然保序)----
    QuadRefBatch,            // count, <count 个 nodeIdx>
    CompressedTexImage2D,    // target, level, internalFormat(ASTC GL常量), w, h, <bytes 压缩数据>
};

inline uint32_t makeHeader(Op op, uint32_t words) {
    return (uint32_t(uint8_t(op)) << 24) | (words & 0x00FFFFFFu);
}
inline Op       headerOp(uint32_t h)    { return Op(uint8_t(h >> 24)); }
inline uint32_t headerWords(uint32_t h) { return h & 0x00FFFFFFu; }

} // namespace cmd
