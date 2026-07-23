#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BachataPkgProbe {
    char content_id[0x30];
    uint64_t package_size;
    uint64_t pfs_image_size;
    char title_hint[0x80];
    int status; // 0 OK, 1 NEED_PASSCODE, 2 CANCELLED, 3 ERROR
    char message[256];
} BachataPkgProbe;

int bachata_pkg_probe(int fd, BachataPkgProbe* out);
int bachata_pkg_extract(
    int fd,
    const char* out_path,
    const char* passcode_or_null,
    void (*progress)(void* ctx, uint64_t done, uint64_t total, const char* file),
    void* progress_ctx);
void bachata_pkg_cancel(void);

#ifdef __cplusplus
}
#endif
