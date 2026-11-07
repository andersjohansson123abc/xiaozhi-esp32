#ifndef SOLISCLOUD_CRYPTO_H
#define SOLISCLOUD_CRYPTO_H

// MD5, HMAC-SHA1 and Base64 for the SolisCloud request signature.
//
// SolisCloud dictates these algorithms. They live here rather than behind mbedTLS
// because IDF 6 moved to PSA Crypto, where MD5 and SHA-1 depend on the build's
// crypto configuration, and they are constexpr so that test/ can check them
// against the RFC vectors at compile time. Nothing here protects data at rest or
// in transit: that is TLS and its own primitives.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace soliscloud {

constexpr uint32_t RotateLeft(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

// Feeds `message` with its padding to `process` in 64 byte blocks. The two hashes
// differ only in the byte order of the trailing bit length.
template <typename Process>
constexpr void ForEachBlock(std::string_view message, bool big_endian, Process&& process) {
    uint8_t block[64] = {};
    size_t offset = 0;
    for (; offset + 64 <= message.size(); offset += 64) {
        for (int i = 0; i < 64; i++) {
            block[i] = static_cast<uint8_t>(message[offset + i]);
        }
        process(block);
    }

    const size_t tail = message.size() - offset;
    for (int i = 0; i < 64; i++) {
        block[i] = 0;
    }
    for (size_t i = 0; i < tail; i++) {
        block[i] = static_cast<uint8_t>(message[offset + i]);
    }
    block[tail] = 0x80;
    if (tail >= 56) {
        process(block);
        for (int i = 0; i < 64; i++) {
            block[i] = 0;
        }
    }

    const uint64_t bits = static_cast<uint64_t>(message.size()) * 8;
    for (int i = 0; i < 8; i++) {
        block[big_endian ? 63 - i : 56 + i] = static_cast<uint8_t>(bits >> (8 * i));
    }
    process(block);
}

constexpr std::array<uint8_t, 16> Md5(std::string_view message) {
    constexpr uint8_t kShift[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    constexpr uint32_t kSine[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391,
    };

    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    ForEachBlock(message, false, [&state, &kShift, &kSine](const uint8_t* block) {
        uint32_t words[16] = {};
        for (int i = 0; i < 16; i++) {
            words[i] = static_cast<uint32_t>(block[i * 4]) |
                       (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
                       (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
                       (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        for (int i = 0; i < 64; i++) {
            uint32_t mix = 0;
            int word = 0;
            if (i < 16) {
                mix = (b & c) | (~b & d);
                word = i;
            } else if (i < 32) {
                mix = (d & b) | (~d & c);
                word = (5 * i + 1) % 16;
            } else if (i < 48) {
                mix = b ^ c ^ d;
                word = (3 * i + 5) % 16;
            } else {
                mix = c ^ (b | ~d);
                word = (7 * i) % 16;
            }
            const uint32_t next = d;
            d = c;
            c = b;
            b += RotateLeft(a + mix + kSine[i] + words[word], kShift[i]);
            a = next;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    });

    std::array<uint8_t, 16> digest{};
    for (int i = 0; i < 16; i++) {
        digest[i] = static_cast<uint8_t>(state[i / 4] >> (8 * (i % 4)));
    }
    return digest;
}

constexpr std::array<uint8_t, 20> Sha1(std::string_view message) {
    uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    ForEachBlock(message, true, [&state](const uint8_t* block) {
        uint32_t words[80] = {};
        for (int i = 0; i < 16; i++) {
            words[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                       (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                       static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; i++) {
            words[i] =
                RotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        for (int i = 0; i < 80; i++) {
            uint32_t mix = 0;
            uint32_t constant = 0;
            if (i < 20) {
                mix = (b & c) | (~b & d);
                constant = 0x5A827999;
            } else if (i < 40) {
                mix = b ^ c ^ d;
                constant = 0x6ED9EBA1;
            } else if (i < 60) {
                mix = (b & c) | (b & d) | (c & d);
                constant = 0x8F1BBCDC;
            } else {
                mix = b ^ c ^ d;
                constant = 0xCA62C1D6;
            }
            const uint32_t next = RotateLeft(a, 5) + mix + e + constant + words[i];
            e = d;
            d = c;
            c = RotateLeft(b, 30);
            b = a;
            a = next;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    });

    std::array<uint8_t, 20> digest{};
    for (int i = 0; i < 20; i++) {
        digest[i] = static_cast<uint8_t>(state[i / 4] >> (24 - 8 * (i % 4)));
    }
    return digest;
}

constexpr std::array<uint8_t, 20> HmacSha1(std::string_view key, std::string_view message) {
    std::array<uint8_t, 64> block{};
    if (key.size() > block.size()) {
        const auto digest = Sha1(key);
        for (size_t i = 0; i < digest.size(); i++) {
            block[i] = digest[i];
        }
    } else {
        for (size_t i = 0; i < key.size(); i++) {
            block[i] = static_cast<uint8_t>(key[i]);
        }
    }

    std::string inner;
    std::string outer;
    inner.reserve(block.size() + message.size());
    outer.reserve(block.size() + 20);
    for (uint8_t byte : block) {
        inner.push_back(static_cast<char>(byte ^ 0x36));
        outer.push_back(static_cast<char>(byte ^ 0x5C));
    }
    inner.append(message);
    for (uint8_t byte : Sha1(inner)) {
        outer.push_back(static_cast<char>(byte));
    }
    return Sha1(outer);
}

constexpr std::string Base64(const uint8_t* data, size_t size) {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                                (i + 1 < size ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
                                (i + 2 < size ? static_cast<uint32_t>(data[i + 2]) : 0);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < size ? kAlphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < size ? kAlphabet[triple & 0x3F] : '=');
    }
    return out;
}

inline constexpr std::string_view kContentType = "application/json;charset=UTF-8";

constexpr std::string ContentMd5(std::string_view body) {
    const auto digest = Md5(body);
    return Base64(digest.data(), digest.size());
}

// The value of the Authorization header's signature part.
constexpr std::string Sign(std::string_view secret, std::string_view content_md5,
                           std::string_view date, std::string_view resource) {
    std::string text = "POST\n";
    text.append(content_md5);
    text.push_back('\n');
    text.append(kContentType);
    text.push_back('\n');
    text.append(date);
    text.push_back('\n');
    text.append(resource);
    const auto digest = HmacSha1(secret, text);
    return Base64(digest.data(), digest.size());
}

}  // namespace soliscloud

#endif  // SOLISCLOUD_CRYPTO_H
