// Portable crypto for PS4 PKG extract (no CryptoPP).
// Algorithms aligned with shadPS4 Crypto / LibOrbisPkg.
#include "pkg_crypto.h"
#include "keys.h"
#include "pkg_rsa_bridge.h"
extern "C" {
#include "aes.h"
}

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

// ---------------- AES-128 via tiny-AES-c (kokke, public domain) ----------------
void aes_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    struct AES_ctx ctx;
    AES_init_ctx(&ctx, key);
    std::memcpy(out, in, 16);
    AES_ECB_encrypt(&ctx, out);
}
void aes_decrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    struct AES_ctx ctx;
    AES_init_ctx(&ctx, key);
    std::memcpy(out, in, 16);
    AES_ECB_decrypt(&ctx, out);
}
void aes_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len) {
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, iv);
    std::memcpy(out, in, len);
    AES_CBC_decrypt_buffer(&ctx, out, len);
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
                             std::span<uint8_t, 16> tweakKey,
                             bool new_crypt) {
    std::array<uint8_t, 32> key_material{};
    if (new_crypt) {
        // LibOrbisPkg: HMAC-SHA256(ekpfs, seed) used as HMAC key for index=1
        HmacSha256(ekpfs, seed, key_material);
    } else {
        std::memcpy(key_material.data(), ekpfs.data(), 32);
    }
    uint8_t d[20];
    const uint32_t index = 1;
    std::memcpy(d, &index, 4); // little-endian index
    std::memcpy(d + 4, seed.data(), 16);
    uint8_t out[32];
    HmacSha256(key_material, d, out);
    std::memcpy(tweakKey.data(), out, 16);
    std::memcpy(dataKey.data(), out + 16, 16);
}

void Crypto::decryptPFS(std::span<const uint8_t, 16> dataKey,
                        std::span<const uint8_t, 16> tweakKey,
                        std::span<const uint8_t> src_image,
                        std::span<uint8_t> dst_image,
                        uint64_t sector,
                        uint64_t crypt_start_sector) {
    // LibOrbisPkg XtsDecryptReader: sectors < cryptStartSector are plaintext.
    // Default crypt_start_sector = 16 (BlockSize 0x10000 / 0x1000).
    for (size_t i = 0; i + 0x1000 <= src_image.size() && i + 0x1000 <= dst_image.size(); i += 0x1000) {
        const uint64_t current_sector = sector + (i / 0x1000);
        if (current_sector < crypt_start_sector) {
            std::memcpy(dst_image.data() + i, src_image.data() + i, 0x1000);
            continue;
        }
        uint8_t tweak[16]{};
        std::memcpy(tweak, &current_sector, sizeof(uint64_t));
        uint8_t encrypted_tweak[16];
        aes_encrypt_block(tweakKey.data(), tweak, encrypted_tweak);
        for (int off = 0; off < 0x1000; off += 16) {
            uint8_t block[16];
            for (int j = 0; j < 16; ++j) {
                block[j] = static_cast<uint8_t>(src_image[i + off + j] ^ encrypted_tweak[j]);
            }
            uint8_t dec[16];
            aes_decrypt_block(dataKey.data(), block, dec);
            for (int j = 0; j < 16; ++j) {
                dst_image[i + off + j] = static_cast<uint8_t>(dec[j] ^ encrypted_tweak[j]);
            }
            // GF multiply as LibOrbisPkg (shift + carry into next byte)
            int feedback = 0;
            for (int k = 0; k < 16; ++k) {
                const uint8_t tmp = encrypted_tweak[k];
                encrypted_tweak[k] = static_cast<uint8_t>((2 * encrypted_tweak[k]) | feedback);
                feedback = (tmp & 0x80) >> 7;
            }
            if (feedback != 0) {
                encrypted_tweak[0] ^= 0x87;
            }
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
