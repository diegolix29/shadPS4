#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// NDK libstdc++ may need explicit span header; cxx_std_20 required.

namespace bachata_pkg {

class Crypto {
public:
    // RSAES-PKCS1-v1_5 decrypt → 32-byte key material (DK3 or EKPFS seed material).
    void RSA2048Decrypt(std::span<uint8_t, 32> out_key,
                        std::span<const uint8_t, 256> ciphertext,
                        bool is_dk3);

    void Sha256(std::span<const uint8_t> input, std::span<uint8_t, 32> out);
    void HmacSha256(std::span<const uint8_t> key,
                    std::span<const uint8_t> data,
                    std::span<uint8_t, 32> out);

    void ivKeyHASH256(std::span<const uint8_t, 64> cipher_input, std::span<uint8_t, 32> ivkey_result);
    void aesCbcCfb128Decrypt(std::span<const uint8_t, 32> ivkey,
                             std::span<const uint8_t, 256> ciphertext,
                             std::span<uint8_t, 256> decrypted);
    void aesCbcCfb128DecryptEntry(std::span<const uint8_t, 32> ivkey,
                                  std::span<uint8_t> ciphertext,
                                  std::span<uint8_t> decrypted);

    // new_crypt: (pfs_flags & 0x2000000000000000) — HMAC(ekpfs, seed) first.
    void PfsGenCryptoKey(std::span<const uint8_t, 32> ekpfs,
                         std::span<const uint8_t, 16> seed,
                         std::span<uint8_t, 16> dataKey,
                         std::span<uint8_t, 16> tweakKey,
                         bool new_crypt = false);

    // crypt_start_sector: LibOrbisPkg default 16 (BlockSize/0x1000). Sectors
    // below that are copied without decryption.
    void decryptPFS(std::span<const uint8_t, 16> dataKey,
                    std::span<const uint8_t, 16> tweakKey,
                    std::span<const uint8_t> src_image,
                    std::span<uint8_t> dst_image,
                    uint64_t sector,
                    uint64_t crypt_start_sector = 16);

    // LibOrbisPkg ComputeKeys(content_id[36], passcode[32], index) → 32 bytes.
    static bool ComputeKeys(const std::string& content_id,
                            const std::string& passcode,
                            uint32_t index,
                            std::span<uint8_t, 32> out);

    // digest = SHA256(dk) XOR dk  (LibOrbisPkg CheckPasscode / CheckDerivedKey).
    static void XorSha256Digest(std::span<const uint8_t, 32> key, std::span<uint8_t, 32> out_digest);
};

} // namespace bachata_pkg
