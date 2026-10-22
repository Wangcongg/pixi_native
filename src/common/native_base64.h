#pragma once
// native_base64.h — 严格 base64(RFC 4648,带 '=' 补齐;拒绝非法字符/错误补齐/换行),三端共用。
// 语义对齐 Apple 的 Data(base64Encoded:)(非法输入返回 nil)——目录信封的 payload/signature/公钥都经它解码。
#include <cstdint>
#include <string>
#include <vector>

namespace nb64 {

inline int valueOf(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// 严格解码:长度必须是 4 的倍数,只允许末尾 1~2 个 '=' 补齐;失败返回 false(out 未定义)。
inline bool decode(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    if (text.empty()) return true;
    if (text.size() % 4 != 0) return false;
    out.reserve(text.size() / 4 * 3);
    size_t pad = 0;
    if (text[text.size() - 1] == '=') pad++;
    if (text.size() >= 2 && text[text.size() - 2] == '=') pad++;
    for (size_t i = 0; i < text.size(); i += 4) {
        int v[4];
        for (int k = 0; k < 4; k++) {
            const char c = text[i + k];
            const bool tail = (i + 4 == text.size()) && (k >= 4 - (int)pad);
            if (c == '=') { if (!tail) return false; v[k] = 0; continue; }
            v[k] = valueOf(c);
            if (v[k] < 0) return false;
        }
        const uint32_t acc = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        out.push_back((uint8_t)(acc >> 16));
        if (!(i + 4 == text.size() && pad >= 2)) out.push_back((uint8_t)((acc >> 8) & 0xff));
        if (!(i + 4 == text.size() && pad >= 1)) out.push_back((uint8_t)(acc & 0xff));
    }
    return true;
}

inline std::string encode(const uint8_t* data, size_t size) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b0 = data[i], b1 = i + 1 < size ? data[i + 1] : 0, b2 = i + 2 < size ? data[i + 2] : 0;
        const uint32_t acc = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(alphabet[(acc >> 18) & 63]);
        out.push_back(alphabet[(acc >> 12) & 63]);
        out.push_back(i + 1 < size ? alphabet[(acc >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? alphabet[acc & 63] : '=');
    }
    return out;
}

}  // namespace nb64
