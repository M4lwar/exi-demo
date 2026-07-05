// uuid_hash.hpp: textbook MD5 (RFC 1321) and SHA-1 (RFC 3174) reference
// implementations, self-contained aside from a handful of standard headers.
// Used to build RFC 4122 name-based (v3/v5) UUIDs; see exi_demo.cpp's
// run_uuid. Not optimized, not constant-time, not for cryptographic use.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace uuid_hash {

// ---------------------------------------------------------------- MD5 ----

inline std::array<uint8_t, 16> md5(const uint8_t* data, size_t len) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const uint32_t S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

    // Padding: 0x80, zeros, then 64-bit little-endian bit length, to a
    // multiple of 64 bytes.
    std::vector<uint8_t> msg(data, data + len);
    uint64_t bit_len = uint64_t(len) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    for (int i = 0; i < 8; ++i) msg.push_back(uint8_t(bit_len >> (8 * i)));

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = uint32_t(msg[chunk + 4 * i]) | (uint32_t(msg[chunk + 4 * i + 1]) << 8) |
                   (uint32_t(msg[chunk + 4 * i + 2]) << 16) | (uint32_t(msg[chunk + 4 * i + 3]) << 24);

        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t F;
            uint32_t g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7 * i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + ((F << S[i]) | (F >> (32 - S[i])));
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }

    std::array<uint8_t, 16> out;
    uint32_t words[4] = {a0, b0, c0, d0};
    for (int w = 0; w < 4; ++w)
        for (int i = 0; i < 4; ++i)
            out[4 * w + i] = uint8_t(words[w] >> (8 * i));
    return out;
}

// --------------------------------------------------------------- SHA-1 ----

inline std::array<uint8_t, 20> sha1(const uint8_t* data, size_t len) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

    // Padding: 0x80, zeros, then 64-bit big-endian bit length, to a
    // multiple of 64 bytes.
    std::vector<uint8_t> msg(data, data + len);
    uint64_t bit_len = uint64_t(len) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    for (int i = 7; i >= 0; --i) msg.push_back(uint8_t(bit_len >> (8 * i)));

    auto rol = [](uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); };

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(msg[chunk + 4 * i]) << 24) | (uint32_t(msg[chunk + 4 * i + 1]) << 16) |
                   (uint32_t(msg[chunk + 4 * i + 2]) << 8) | uint32_t(msg[chunk + 4 * i + 3]);
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    std::array<uint8_t, 20> out;
    uint32_t words[5] = {h0, h1, h2, h3, h4};
    for (int w = 0; w < 5; ++w)
        for (int i = 0; i < 4; ++i)
            out[4 * w + i] = uint8_t(words[w] >> (8 * (3 - i)));
    return out;
}

}  // namespace uuid_hash
