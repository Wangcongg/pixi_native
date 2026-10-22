#pragma once
// 最小 KTX2 容器解析:只覆盖本项目实际用到的形态——ASTC(vkFormat 157-184)、
// supercompressionScheme 0(无) 或 2(zstd)、单层单面。目的是让 spine 页图能像 fgui 图集那样
// 直接以 GPU 压缩格式上传,省掉"PNG→RGBA"的解码与 4 倍显存/带宽。
//
// 在后台队列解码的。把解析放在 C++ 才能保持"像素不进 JSC"这条既有约定(与 spine 骨架预热同构)。
//
// 格式参考 KTX 2.0 规范:12B identifier + header(68B) + level index(24B/level)。
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ktx2 {

struct Level {
    uint64_t byteOffset = 0;      // 在原始文件中的偏移
    uint64_t byteLength = 0;      // 压缩后长度(supercompression=0 时即实际长度)
    uint64_t uncompressedLength = 0;
    int width = 0, height = 0;
};

struct Image {
    bool ok = false;
    uint32_t vkFormat = 0;
    uint32_t glInternalFormat = 0;  // 由 vkFormat 映射
    int width = 0, height = 0;
    uint32_t levelCount = 1;
    uint32_t supercompression = 0;  // 0=无 2=zstd
    // KTX DFD alpha flag. ASTC blocks do not encode whether RGB is already multiplied by alpha;
    // the renderer must preserve this container metadata when choosing its blend/shader path.
    bool alphaPremultiplied = false;
    std::vector<Level> levels;
    std::string error;
};

// KTX2 identifier: «KTX 20»\r\n\x1A\n
inline bool hasMagic(const uint8_t* p, size_t n) {
    static const uint8_t kMagic[12] = {0xAB,0x4B,0x54,0x58,0x20,0x32,0x30,0xBB,0x0D,0x0A,0x1A,0x0A};
    return n >= 12 && std::memcmp(p, kMagic, 12) == 0;
}

// 两条路径必须给出同样的格式,否则同一张图在 fgui 与 spine 下会以不同格式上传。
inline uint32_t glFormatOf(uint32_t vkFormat) {
    if (vkFormat < 157 || vkFormat > 184) return 0;
    // 157 起按 (UNORM, SRGB) 成对排列,对应 GL_COMPRESSED_(RGBA|SRGB8_ALPHA8)_ASTC_*
    static const uint32_t kTable[] = {
        0x93B0,0x93D0, 0x93B1,0x93D1, 0x93B2,0x93D2, 0x93B3,0x93D3,
        0x93B4,0x93D4, 0x93B5,0x93D5, 0x93B6,0x93D6, 0x93B7,0x93D7,
        0x93B8,0x93D8, 0x93B9,0x93D9, 0x93BA,0x93DA, 0x93BB,0x93DB,
        0x93BC,0x93DC, 0x93BD,0x93DD
    };
    return kTable[vkFormat - 157];
}

inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
inline uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p+4) << 32);
}

inline Image parse(const uint8_t* data, size_t size) {
    Image img;
    if (!hasMagic(data, size)) { img.error = "not a KTX2 file"; return img; }
    if (size < 80) { img.error = "truncated header"; return img; }
    img.vkFormat        = rd32(data + 12);
    img.width           = (int)rd32(data + 20);
    img.height          = (int)rd32(data + 24);
    const uint32_t layers = rd32(data + 32);       // layerCount
    const uint32_t faces  = rd32(data + 36);       // faceCount
    img.levelCount      = rd32(data + 40); if (img.levelCount == 0) img.levelCount = 1;
    img.supercompression= rd32(data + 44);
    img.glInternalFormat = glFormatOf(img.vkFormat);
    if (!img.glInternalFormat) { img.error = "unsupported vkFormat " + std::to_string(img.vkFormat); return img; }
    if (img.supercompression != 0 && img.supercompression != 2) {
        img.error = "unsupported supercompression " + std::to_string(img.supercompression); return img;
    }
    if (layers > 1 || faces > 1) { img.error = "array/cubemap not supported"; return img; }
    if (img.width <= 0 || img.height <= 0) { img.error = "bad dimensions"; return img; }
    // Basic DFD word 3 is {colorModel, primaries, transfer, flags}. Flag bit 0 means
    // KHR_DF_FLAG_ALPHA_PREMULTIPLIED; zero means KHR_DF_FLAG_ALPHA_STRAIGHT.
    // A valid KTX2 always has a DFD. Treat missing legacy metadata as straight alpha, which is
    // the conservative choice (otherwise transparent-edge RGB leaks as a white/colored matte).
    const uint32_t dfdOffset = rd32(data + 48), dfdLength = rd32(data + 52);
    if (dfdOffset <= size && dfdLength >= 16 && (size_t)dfdOffset + 16 <= size)
        img.alphaPremultiplied = (data[dfdOffset + 15] & 1u) != 0;
    // level index 紧跟 header:每条 24 字节 = byteOffset(8) + byteLength(8) + uncompressedLength(8)
    if (size < 80 + (size_t)img.levelCount * 24) { img.error = "truncated level index"; return img; }
    img.levels.reserve(img.levelCount);
    for (uint32_t i = 0; i < img.levelCount; ++i) {
        const uint8_t* e = data + 80 + (size_t)i * 24;
        Level lv;
        lv.byteOffset = rd64(e);
        lv.byteLength = rd64(e + 8);
        lv.uncompressedLength = rd64(e + 16);
        lv.width  = img.width  >> i; if (lv.width  < 1) lv.width  = 1;
        lv.height = img.height >> i; if (lv.height < 1) lv.height = 1;
        if (lv.byteOffset + lv.byteLength > size) { img.error = "level out of range"; return img; }
        img.levels.push_back(lv);
    }
    img.ok = true;
    return img;
}

}  // namespace ktx2
