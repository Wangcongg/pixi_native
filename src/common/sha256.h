#pragma once

// Small dependency-free SHA-256 implementation shared by the native download bridges. The
// publishing tools use Node's crypto implementation; this implementation verifies the exact
// downloaded bytes before they enter a persistent cache.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nativecrypto {

class Sha256 {
    std::array<uint32_t, 8> state_{{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u }};
    std::array<uint8_t, 64> buffer_{};
    uint64_t bytes_ = 0;
    size_t buffered_ = 0;

    static uint32_t rotate(uint32_t value, unsigned count) {
        return (value >> count) | (value << (32 - count));
    }
    void transform(const uint8_t* block) {
        static constexpr uint32_t constants[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u };
        uint32_t words[64];
        for (size_t index = 0; index < 16; ++index) {
            const size_t offset = index * 4;
            words[index] = ((uint32_t)block[offset] << 24) |
                ((uint32_t)block[offset + 1] << 16) |
                ((uint32_t)block[offset + 2] << 8) | block[offset + 3];
        }
        for (size_t index = 16; index < 64; ++index) {
            const uint32_t s0 = rotate(words[index - 15], 7) ^ rotate(words[index - 15], 18) ^
                (words[index - 15] >> 3);
            const uint32_t s1 = rotate(words[index - 2], 17) ^ rotate(words[index - 2], 19) ^
                (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        uint32_t a=state_[0],b=state_[1],c=state_[2],d=state_[3];
        uint32_t e=state_[4],f=state_[5],g=state_[6],h=state_[7];
        for (size_t index = 0; index < 64; ++index) {
            const uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const uint32_t choice = (e & f) ^ (~e & g);
            const uint32_t first = h + s1 + choice + constants[index] + words[index];
            const uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t second = s0 + majority;
            h=g;g=f;f=e;e=d+first;d=c;c=b;b=a;a=first+second;
        }
        state_[0]+=a;state_[1]+=b;state_[2]+=c;state_[3]+=d;
        state_[4]+=e;state_[5]+=f;state_[6]+=g;state_[7]+=h;
    }

public:
    void update(const void* source, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(source);
        bytes_ += size;
        while (size) {
            const size_t count = std::min(size, buffer_.size() - buffered_);
            for (size_t index = 0; index < count; ++index) buffer_[buffered_ + index] = bytes[index];
            buffered_ += count; bytes += count; size -= count;
            if (buffered_ == buffer_.size()) { transform(buffer_.data()); buffered_ = 0; }
        }
    }
    std::string finishHex() {
        const uint64_t bits = bytes_ * 8;
        buffer_[buffered_++] = 0x80;
        if (buffered_ > 56) {
            while (buffered_ < 64) buffer_[buffered_++] = 0;
            transform(buffer_.data()); buffered_ = 0;
        }
        while (buffered_ < 56) buffer_[buffered_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8) buffer_[buffered_++] = (uint8_t)(bits >> shift);
        transform(buffer_.data());
        static constexpr char hex[] = "0123456789abcdef";
        std::string result(64, '0');
        for (size_t index = 0; index < state_.size(); ++index) {
            for (size_t byte = 0; byte < 4; ++byte) {
                const uint8_t value = (uint8_t)(state_[index] >> (24 - byte * 8));
                result[index * 8 + byte * 2] = hex[value >> 4];
                result[index * 8 + byte * 2 + 1] = hex[value & 15];
            }
        }
        return result;
    }
};

inline std::string sha256Hex(const void* data, size_t size) {
    Sha256 hash;
    hash.update(data, size);
    return hash.finishHex();
}

} // namespace nativecrypto
