#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int bachata_ahb_copy_to_buffer(int64_t hardwareBufferPtr, void* dst, int dstBytes,
                               int expectedWidth, int expectedHeight);

#ifdef __cplusplus
}
#endif
