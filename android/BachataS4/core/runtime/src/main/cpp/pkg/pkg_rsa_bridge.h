#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Decrypt 256-byte RSAES-PKCS1-v1_5 ciphertext via Java javax.crypto.
// Writes 32-byte key material to out32. Returns 0 on success, non-zero on failure.
int bachata_pkg_rsa_decrypt(const uint8_t* ciphertext256, uint8_t* out32, int is_dk3);

#ifdef __cplusplus
}
#endif
