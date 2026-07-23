// Portable crypto for PS4 PKG extract (no CryptoPP).
// Algorithms aligned with shadPS4 Crypto / LibOrbisPkg.
#include "pkg_crypto.h"
#include "keys.h"
#include "pkg_rsa_bridge.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace bachata_pkg {
namespace {

// ---------------- SHA-256 ----------------
struct Sha256Ctx {
    uint64_t bitlen = 0;
    uint32_t state[8]{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    uint8_t buf[64]{};
    size_t buflen = 0;
};

constexpr uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void sha256_transform(Sha256Ctx& ctx, const uint8_t data[64]) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };
    uint32_t m[64];
    for (int i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = (uint32_t(data[j]) << 24) | (uint32_t(data[j + 1]) << 16) |
               (uint32_t(data[j + 2]) << 8) | uint32_t(data[j + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
        const uint32_t s1 = rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3];
    uint32_t e = ctx.state[4], f = ctx.state[5], g = ctx.state[6], h = ctx.state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + k[i] + m[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
    ctx.state[4] += e; ctx.state[5] += f; ctx.state[6] += g; ctx.state[7] += h;
}

void sha256_update(Sha256Ctx& ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx.buf[ctx.buflen++] = data[i];
        if (ctx.buflen == 64) {
            sha256_transform(ctx, ctx.buf);
            ctx.bitlen += 512;
            ctx.buflen = 0;
        }
    }
}

void sha256_final(Sha256Ctx& ctx, uint8_t out[32]) {
    size_t i = ctx.buflen;
    ctx.buf[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx.buf[i++] = 0;
        sha256_transform(ctx, ctx.buf);
        i = 0;
    }
    while (i < 56) ctx.buf[i++] = 0;
    ctx.bitlen += ctx.buflen * 8;
    for (int j = 0; j < 8; ++j) {
        ctx.buf[63 - j] = static_cast<uint8_t>((ctx.bitlen >> (8 * j)) & 0xff);
    }
    sha256_transform(ctx, ctx.buf);
    for (int j = 0; j < 8; ++j) {
        out[j * 4] = static_cast<uint8_t>((ctx.state[j] >> 24) & 0xff);
        out[j * 4 + 1] = static_cast<uint8_t>((ctx.state[j] >> 16) & 0xff);
        out[j * 4 + 2] = static_cast<uint8_t>((ctx.state[j] >> 8) & 0xff);
        out[j * 4 + 3] = static_cast<uint8_t>(ctx.state[j] & 0xff);
    }
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    Sha256Ctx ctx;
    sha256_update(ctx, data, len);
    sha256_final(ctx, out);
}

// ---------------- AES-128 (column-major, NIST-compatible) ----------------
// State layout matches FIPS-197 / tiny-AES-c: s[r + 4*c]
const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};
const uint8_t rsbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};
const uint8_t rcon[11] = {0x8d,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ (((x >> 7) & 1) * 0x1b)); }

void aes_key_expansion(const uint8_t key[16], uint8_t round_keys[176]) {
    std::memcpy(round_keys, key, 16);
    uint8_t tempa[4];
    int i = 4;
    while (i < 44) {
        tempa[0] = round_keys[(i - 1) * 4 + 0];
        tempa[1] = round_keys[(i - 1) * 4 + 1];
        tempa[2] = round_keys[(i - 1) * 4 + 2];
        tempa[3] = round_keys[(i - 1) * 4 + 3];
        if (i % 4 == 0) {
            const uint8_t u = tempa[0];
            tempa[0] = tempa[1];
            tempa[1] = tempa[2];
            tempa[2] = tempa[3];
            tempa[3] = u;
            tempa[0] = sbox[tempa[0]];
            tempa[1] = sbox[tempa[1]];
            tempa[2] = sbox[tempa[2]];
            tempa[3] = sbox[tempa[3]];
            tempa[0] ^= rcon[i / 4];
        }
        round_keys[i * 4 + 0] = round_keys[(i - 4) * 4 + 0] ^ tempa[0];
        round_keys[i * 4 + 1] = round_keys[(i - 4) * 4 + 1] ^ tempa[1];
        round_keys[i * 4 + 2] = round_keys[(i - 4) * 4 + 2] ^ tempa[2];
        round_keys[i * 4 + 3] = round_keys[(i - 4) * 4 + 3] ^ tempa[3];
        ++i;
    }
}

void add_round_key(uint8_t state[16], const uint8_t* rk) {
    for (int i = 0; i < 16; ++i) state[i] ^= rk[i];
}
void sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; ++i) state[i] = sbox[state[i]];
}
void inv_sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; ++i) state[i] = rsbox[state[i]];
}
void shift_rows(uint8_t s[16]) {
    uint8_t temp;
    // Row 1
    temp = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = temp;
    // Row 2
    temp = s[2]; s[2] = s[10]; s[10] = temp; temp = s[6]; s[6] = s[14]; s[14] = temp;
    // Row 3
    temp = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = temp;
}
void inv_shift_rows(uint8_t s[16]) {
    uint8_t temp;
    temp = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = temp;
    temp = s[2]; s[2] = s[10]; s[10] = temp; temp = s[6]; s[6] = s[14]; s[14] = temp;
    temp = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = temp;
}
void mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; ++c) {
        const uint8_t a0 = s[c * 4 + 0];
        const uint8_t a1 = s[c * 4 + 1];
        const uint8_t a2 = s[c * 4 + 2];
        const uint8_t a3 = s[c * 4 + 3];
        // column-major: bytes of column c are consecutive at c*4
        s[c * 4 + 0] = xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3;
        s[c * 4 + 1] = a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3;
        s[c * 4 + 2] = a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3;
        s[c * 4 + 3] = xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3);
    }
}
uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        const bool hi = (a & 0x80) != 0;
        a = static_cast<uint8_t>(a << 1);
        if (hi) a ^= 0x1b;
        b = static_cast<uint8_t>(b >> 1);
    }
    return p;
}
void inv_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; ++c) {
        const uint8_t a0 = s[c * 4 + 0];
        const uint8_t a1 = s[c * 4 + 1];
        const uint8_t a2 = s[c * 4 + 2];
        const uint8_t a3 = s[c * 4 + 3];
        s[c * 4 + 0] = mul(a0, 0x0e) ^ mul(a1, 0x0b) ^ mul(a2, 0x0d) ^ mul(a3, 0x09);
        s[c * 4 + 1] = mul(a0, 0x09) ^ mul(a1, 0x0e) ^ mul(a2, 0x0b) ^ mul(a3, 0x0d);
        s[c * 4 + 2] = mul(a0, 0x0d) ^ mul(a1, 0x09) ^ mul(a2, 0x0e) ^ mul(a3, 0x0b);
        s[c * 4 + 3] = mul(a0, 0x0b) ^ mul(a1, 0x0d) ^ mul(a2, 0x09) ^ mul(a3, 0x0e);
    }
}

void aes_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    std::memcpy(s, in, 16);
    add_round_key(s, rk);
    for (int r = 1; r < 10; ++r) {
        sub_bytes(s);
        shift_rows(s);
        mix_columns(s);
        add_round_key(s, rk + r * 16);
    }
    sub_bytes(s);
    shift_rows(s);
    add_round_key(s, rk + 160);
    std::memcpy(out, s, 16);
}
void aes_decrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    std::memcpy(s, in, 16);
    add_round_key(s, rk + 160);
    for (int r = 9; r > 0; --r) {
        inv_shift_rows(s);
        inv_sub_bytes(s);
        add_round_key(s, rk + r * 16);
        inv_mix_columns(s);
    }
    inv_shift_rows(s);
    inv_sub_bytes(s);
    add_round_key(s, rk);
    std::memcpy(out, s, 16);
}

void aes_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len) {
    uint8_t rk[176];
    aes_key_expansion(key, rk);
    uint8_t prev[16];
    std::memcpy(prev, iv, 16);
    for (size_t i = 0; i < len; i += 16) {
        uint8_t block[16];
        aes_decrypt_block(rk, in + i, block);
        for (int j = 0; j < 16; ++j) out[i + j] = static_cast<uint8_t>(block[j] ^ prev[j]);
        std::memcpy(prev, in + i, 16);
    }
}

// ---------------- Big int RSA (minimal, 2048-bit) ----------------
// limb = uint32_t little-endian limbs
using Limb = uint32_t;
constexpr int kMaxLimbs = 64; // 2048 bits

struct Big {
    Limb d[kMaxLimbs]{};
    int n = 0; // significant limbs
};

Big big_from_be(const uint8_t* data, size_t len) {
    Big b;
    // strip leading zeros
    size_t start = 0;
    while (start < len && data[start] == 0) ++start;
    const size_t bytes = len - start;
    b.n = static_cast<int>((bytes + 3) / 4);
    if (b.n > kMaxLimbs) b.n = kMaxLimbs;
    for (int i = 0; i < b.n; ++i) {
        Limb v = 0;
        for (int j = 0; j < 4; ++j) {
            const int idx = static_cast<int>(bytes) - 1 - (i * 4 + j);
            if (idx >= 0) v |= Limb(data[start + idx]) << (8 * j);
        }
        b.d[i] = v;
    }
    while (b.n > 0 && b.d[b.n - 1] == 0) --b.n;
    return b;
}

void big_to_be(const Big& b, uint8_t* out, size_t len) {
    std::memset(out, 0, len);
    for (int i = 0; i < b.n; ++i) {
        for (int j = 0; j < 4; ++j) {
            const int idx = static_cast<int>(len) - 1 - (i * 4 + j);
            if (idx >= 0) out[idx] = static_cast<uint8_t>((b.d[i] >> (8 * j)) & 0xff);
        }
    }
}

int big_cmp(const Big& a, const Big& b) {
    if (a.n != b.n) return a.n > b.n ? 1 : -1;
    for (int i = a.n - 1; i >= 0; --i) {
        if (a.d[i] != b.d[i]) return a.d[i] > b.d[i] ? 1 : -1;
    }
    return 0;
}

Big big_sub(const Big& a, const Big& b) { // a >= b
    Big r = a;
    uint64_t borrow = 0;
    for (int i = 0; i < r.n; ++i) {
        const uint64_t bv = (i < b.n ? b.d[i] : 0) + borrow;
        if (r.d[i] >= bv) {
            r.d[i] = static_cast<Limb>(r.d[i] - bv);
            borrow = 0;
        } else {
            r.d[i] = static_cast<Limb>(uint64_t(r.d[i]) + (uint64_t(1) << 32) - bv);
            borrow = 1;
        }
    }
    while (r.n > 0 && r.d[r.n - 1] == 0) --r.n;
    return r;
}

Big big_add(const Big& a, const Big& b) {
    Big r;
    r.n = std::max(a.n, b.n);
    uint64_t carry = 0;
    for (int i = 0; i < r.n; ++i) {
        carry += (i < a.n ? a.d[i] : 0) + (i < b.n ? b.d[i] : 0);
        r.d[i] = static_cast<Limb>(carry);
        carry >>= 32;
    }
    if (carry && r.n < kMaxLimbs) r.d[r.n++] = static_cast<Limb>(carry);
    return r;
}

Big big_shl_limb(const Big& a, int limbs) {
    Big r;
    r.n = std::min(kMaxLimbs, a.n + limbs);
    for (int i = r.n - 1; i >= limbs; --i) r.d[i] = a.d[i - limbs];
    return r;
}

Big big_mul(const Big& a, const Big& b) {
    Big r;
    r.n = std::min(kMaxLimbs, a.n + b.n);
    for (int i = 0; i < a.n; ++i) {
        uint64_t carry = 0;
        for (int j = 0; j < b.n && i + j < kMaxLimbs; ++j) {
            carry += uint64_t(r.d[i + j]) + uint64_t(a.d[i]) * b.d[j];
            r.d[i + j] = static_cast<Limb>(carry);
            carry >>= 32;
        }
        int k = i + b.n;
        while (carry && k < kMaxLimbs) {
            carry += r.d[k];
            r.d[k] = static_cast<Limb>(carry);
            carry >>= 32;
            ++k;
        }
        if (k > r.n) r.n = k;
    }
    while (r.n > 0 && r.d[r.n - 1] == 0) --r.n;
    return r;
}

// r = a % m (binary long division)
Big big_mod(Big a, const Big& m) {
    if (m.n == 0) return a;
    while (big_cmp(a, m) >= 0) {
        // align m under a
        int shift = a.n - m.n;
        Big ms = big_shl_limb(m, shift);
        if (big_cmp(a, ms) < 0) {
            if (shift == 0) break;
            ms = big_shl_limb(m, shift - 1);
        }
        a = big_sub(a, ms);
    }
    return a;
}

// Modular multiply: (a * b) % m
Big big_modmul(const Big& a, const Big& b, const Big& m) {
    return big_mod(big_mul(a, b), m);
}

Big big_modexp(Big base, Big exp, const Big& mod) {
    Big result;
    result.n = 1;
    result.d[0] = 1;
    base = big_mod(base, mod);
    while (exp.n > 0) {
        if (exp.d[0] & 1) result = big_modmul(result, base, mod);
        base = big_modmul(base, base, mod);
        // exp >>= 1
        uint32_t carry = 0;
        for (int i = exp.n - 1; i >= 0; --i) {
            const uint64_t v = (uint64_t(carry) << 32) | exp.d[i];
            exp.d[i] = static_cast<Limb>(v >> 1);
            carry = static_cast<uint32_t>(v & 1);
        }
        while (exp.n > 0 && exp.d[exp.n - 1] == 0) --exp.n;
    }
    return result;
}

// Improve big_mod using bit-length aligned subtraction.
Big big_mod_fast(Big a, const Big& m) {
    if (m.n == 0 || a.n == 0) return a;
    // Determine bit lengths roughly via top limb
    while (big_cmp(a, m) >= 0) {
        int shift_bits = 0;
        // crude: shift m left until just under a
        Big ms = m;
        // limb-align first
        int limb_diff = a.n - m.n;
        if (limb_diff > 0) {
            ms = big_shl_limb(m, limb_diff);
            if (big_cmp(a, ms) < 0 && limb_diff > 0) {
                ms = big_shl_limb(m, limb_diff - 1);
            }
        }
        // bit-shift within limb if still needed
        while (true) {
            Big twice = big_add(ms, ms);
            if (big_cmp(twice, a) > 0 || twice.n == 0) break;
            // check overflow of limbs
            if (twice.n >= kMaxLimbs && twice.d[kMaxLimbs-1] != 0 && big_cmp(twice, ms) < 0) break;
            ms = twice;
            ++shift_bits;
            if (shift_bits > 32 * kMaxLimbs) break;
        }
        a = big_sub(a, ms);
    }
    return a;
}

Big big_modmul_fast(const Big& a, const Big& b, const Big& m) {
    return big_mod_fast(big_mul(a, b), m);
}

Big big_modexp_fast(Big base, Big exp, const Big& mod) {
    Big result;
    result.n = 1;
    result.d[0] = 1;
    base = big_mod_fast(base, mod);
    while (exp.n > 0) {
        if (exp.d[0] & 1) result = big_modmul_fast(result, base, mod);
        base = big_modmul_fast(base, base, mod);
        uint32_t carry = 0;
        for (int i = exp.n - 1; i >= 0; --i) {
            const uint64_t v = (uint64_t(carry) << 32) | exp.d[i];
            exp.d[i] = static_cast<Limb>(v >> 1);
            carry = static_cast<uint32_t>(v & 1);
        }
        while (exp.n > 0 && exp.d[exp.n - 1] == 0) --exp.n;
    }
    return result;
}

bool pkcs1_unpad(const uint8_t plain[256], uint8_t out_key[32]) {
    if (plain[0] != 0x00 || plain[1] != 0x02) return false;
    size_t i = 2;
    while (i < 256 && plain[i] != 0x00) ++i;
    if (i >= 256 || i < 10) return false;
    ++i;
    const size_t msg_len = 256 - i;
    if (msg_len < 32) {
        std::memset(out_key, 0, 32);
        std::memcpy(out_key + (32 - msg_len), plain + i, msg_len);
    } else {
        std::memcpy(out_key, plain + i, 32);
    }
    return true;
}

bool rsa_pkcs1_v15_decrypt_crt(const uint8_t cipher[256],
                               const uint8_t* p, size_t p_len,
                               const uint8_t* q, size_t q_len,
                               const uint8_t* dp, size_t dp_len,
                               const uint8_t* dq, size_t dq_len,
                               const uint8_t* qinv, size_t qinv_len,
                               const uint8_t* modulus, size_t mod_len,
                               uint8_t out_key[32]) {
    Big c = big_from_be(cipher, 256);
    Big P = big_from_be(p, p_len);
    Big Q = big_from_be(q, q_len);
    Big dP = big_from_be(dp, dp_len);
    Big dQ = big_from_be(dq, dq_len);
    Big qInv = big_from_be(qinv, qinv_len);
    Big m1 = big_modexp_fast(big_mod_fast(c, P), dP, P);
    Big m2 = big_modexp_fast(big_mod_fast(c, Q), dQ, Q);
    // h = qInv * (m1 - m2) mod p
    Big diff;
    if (big_cmp(m1, m2) >= 0) {
        diff = big_sub(m1, m2);
    } else {
        diff = big_sub(big_add(m1, P), m2);
    }
    Big h = big_modmul_fast(qInv, diff, P);
    // m = m2 + h * q
    Big m = big_add(m2, big_mul(h, Q));
    // ensure m < n
    Big n = big_from_be(modulus, mod_len);
    m = big_mod_fast(m, n);
    uint8_t plain[256];
    big_to_be(m, plain, 256);
    return pkcs1_unpad(plain, out_key);
}

void xts_mult(uint8_t t[16]) {
    int feedback = 0;
    for (int k = 0; k < 16; ++k) {
        const int tmp = (t[k] >> 7) & 1;
        t[k] = static_cast<uint8_t>(((t[k] << 1) + feedback) & 0xff);
        feedback = tmp;
    }
    if (feedback != 0) t[0] ^= 0x87;
}

} // namespace

void Crypto::Sha256(std::span<const uint8_t> input, std::span<uint8_t, 32> out) {
    sha256(input.data(), input.size(), out.data());
}

void Crypto::HmacSha256(std::span<const uint8_t> key,
                        std::span<const uint8_t> data,
                        std::span<uint8_t, 32> out) {
    uint8_t k[64]{};
    if (key.size() > 64) {
        sha256(key.data(), key.size(), k);
    } else {
        std::memcpy(k, key.data(), key.size());
    }
    uint8_t i_pad[64], o_pad[64];
    for (int i = 0; i < 64; ++i) {
        i_pad[i] = k[i] ^ 0x36;
        o_pad[i] = k[i] ^ 0x5c;
    }
    Sha256Ctx ctx;
    sha256_update(ctx, i_pad, 64);
    sha256_update(ctx, data.data(), data.size());
    uint8_t inner[32];
    sha256_final(ctx, inner);
    Sha256Ctx ctx2;
    sha256_update(ctx2, o_pad, 64);
    sha256_update(ctx2, inner, 32);
    sha256_final(ctx2, out.data());
}

void Crypto::ivKeyHASH256(std::span<const uint8_t, 64> cipher_input,
                          std::span<uint8_t, 32> ivkey_result) {
    Sha256(cipher_input, ivkey_result);
}

void Crypto::aesCbcCfb128Decrypt(std::span<const uint8_t, 32> ivkey,
                                 std::span<const uint8_t, 256> ciphertext,
                                 std::span<uint8_t, 256> decrypted) {
    uint8_t key[16], iv[16];
    std::memcpy(iv, ivkey.data(), 16);
    std::memcpy(key, ivkey.data() + 16, 16);
    aes_cbc_decrypt(key, iv, ciphertext.data(), decrypted.data(), 256);
}

void Crypto::aesCbcCfb128DecryptEntry(std::span<const uint8_t, 32> ivkey,
                                      std::span<uint8_t> ciphertext,
                                      std::span<uint8_t> decrypted) {
    uint8_t key[16], iv[16];
    std::memcpy(iv, ivkey.data(), 16);
    std::memcpy(key, ivkey.data() + 16, 16);
    const size_t len = std::min(ciphertext.size(), decrypted.size()) & ~size_t(15);
    aes_cbc_decrypt(key, iv, ciphertext.data(), decrypted.data(), len);
}

void Crypto::PfsGenCryptoKey(std::span<const uint8_t, 32> ekpfs,
                             std::span<const uint8_t, 16> seed,
                             std::span<uint8_t, 16> dataKey,
                             std::span<uint8_t, 16> tweakKey) {
    uint8_t d[20];
    const uint32_t index = 1;
    std::memcpy(d, &index, 4); // little-endian index as shadPS4
    std::memcpy(d + 4, seed.data(), 16);
    uint8_t out[32];
    HmacSha256(ekpfs, d, out);
    std::memcpy(tweakKey.data(), out, 16);
    std::memcpy(dataKey.data(), out + 16, 16);
}

void Crypto::decryptPFS(std::span<const uint8_t, 16> dataKey,
                        std::span<const uint8_t, 16> tweakKey,
                        std::span<const uint8_t> src_image,
                        std::span<uint8_t> dst_image,
                        uint64_t sector) {
    uint8_t data_rk[176], tweak_rk[176];
    aes_key_expansion(dataKey.data(), data_rk);
    aes_key_expansion(tweakKey.data(), tweak_rk);
    for (size_t i = 0; i + 0x1000 <= src_image.size() && i + 0x1000 <= dst_image.size(); i += 0x1000) {
        const uint64_t current_sector = sector + (i / 0x1000);
        uint8_t tweak[16]{};
        std::memcpy(tweak, &current_sector, sizeof(uint64_t));
        uint8_t encrypted_tweak[16];
        aes_encrypt_block(tweak_rk, tweak, encrypted_tweak);
        for (int off = 0; off < 0x1000; off += 16) {
            uint8_t block[16];
            for (int j = 0; j < 16; ++j) {
                block[j] = src_image[i + off + j] ^ encrypted_tweak[j];
            }
            uint8_t dec[16];
            aes_decrypt_block(data_rk, block, dec);
            for (int j = 0; j < 16; ++j) {
                dst_image[i + off + j] = dec[j] ^ encrypted_tweak[j];
            }
            xts_mult(encrypted_tweak);
        }
    }
}

void Crypto::RSA2048Decrypt(std::span<uint8_t, 32> out_key,
                            std::span<const uint8_t, 256> ciphertext,
                            bool is_dk3) {
    // Real PKCS#1 v1.5 decrypt via Android javax.crypto (JNI). Hand-rolled RSA
    // was too slow for 2048-bit CRT and hung probe on large titles.
    if (bachata_pkg_rsa_decrypt(ciphertext.data(), out_key.data(), is_dk3 ? 1 : 0) != 0) {
        throw std::runtime_error("RSA decrypt failed (Java PKCS1)");
    }
}

bool Crypto::ComputeKeys(const std::string& content_id,
                         const std::string& passcode,
                         uint32_t index,
                         std::span<uint8_t, 32> out) {
    if (content_id.size() != 36 || passcode.size() != 32) return false;
    uint8_t index_be[4] = {
        static_cast<uint8_t>((index >> 24) & 0xff),
        static_cast<uint8_t>((index >> 16) & 0xff),
        static_cast<uint8_t>((index >> 8) & 0xff),
        static_cast<uint8_t>(index & 0xff),
    };
    uint8_t h_index[32], h_cid[32];
    sha256(index_be, 4, h_index);
    uint8_t cid[48]{};
    std::memcpy(cid, content_id.data(), 36);
    sha256(cid, 48, h_cid);
    uint8_t data[96];
    std::memcpy(data, h_index, 32);
    std::memcpy(data + 32, h_cid, 32);
    std::memcpy(data + 64, passcode.data(), 32);
    sha256(data, 96, out.data());
    return true;
}

void Crypto::XorSha256Digest(std::span<const uint8_t, 32> key, std::span<uint8_t, 32> out_digest) {
    uint8_t hash[32];
    sha256(key.data(), 32, hash);
    for (int i = 0; i < 32; ++i) {
        out_digest[i] = static_cast<uint8_t>(hash[i] ^ key[i]);
    }
}

} // namespace bachata_pkg
