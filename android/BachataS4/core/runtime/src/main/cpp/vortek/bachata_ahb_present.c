/* Copy AHardwareBuffer pixels into a direct ByteBuffer for Canvas compositing. */
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <jni.h>
#include <string.h>
#include <stdint.h>

#define LOG_TAG "Bachata.Vortek"
#define AHB_LOG(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define AHB_ERR(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/**
 * Lock AHB for CPU read, copy tightly-packed RGBA/BGRA into |dst| (width*height*4), unlock.
 * Returns 0 on success.
 */
int bachata_ahb_copy_to_buffer(int64_t hardwareBufferPtr, void* dst, int dstBytes,
                               int expectedWidth, int expectedHeight) {
    if (!hardwareBufferPtr || !dst || expectedWidth <= 0 || expectedHeight <= 0) return -1;
    AHardwareBuffer* ahb = (AHardwareBuffer*)(intptr_t)hardwareBufferPtr;

    AHardwareBuffer_Desc desc = {0};
    AHardwareBuffer_describe(ahb, &desc);
    if ((int)desc.width != expectedWidth || (int)desc.height != expectedHeight) {
        AHB_ERR("ahb_size_mismatch ahb=%ux%u expect=%dx%d",
                desc.width, desc.height, expectedWidth, expectedHeight);
        /* Still copy the min region. */
    }

    void* src = NULL;
    int lockUsage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    int rc = AHardwareBuffer_lock(ahb, lockUsage, -1, NULL, &src);
    if (rc != 0 || !src) {
        /* Retry with RARELY if OFtEN not in usage flags. */
        rc = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &src);
    }
    if (rc != 0 || !src) {
        AHB_ERR("ahb_lock_failed rc=%d", rc);
        return -2;
    }

    const int width = (int)desc.width;
    const int height = (int)desc.height;
    const int stridePx = desc.stride > 0 ? (int)desc.stride : width;
    const size_t rowBytes = (size_t)width * 4;
    const size_t need = rowBytes * (size_t)height;
    if ((size_t)dstBytes < need) {
        AHardwareBuffer_unlock(ahb, NULL);
        AHB_ERR("ahb_dst_too_small need=%zu have=%d", need, dstBytes);
        return -3;
    }

    uint8_t* out = (uint8_t*)dst;
    const uint8_t* in = (const uint8_t*)src;
    for (int y = 0; y < height; y++) {
        memcpy(out + (size_t)y * rowBytes, in + (size_t)y * (size_t)stridePx * 4, rowBytes);
    }

    AHardwareBuffer_unlock(ahb, NULL);
    return 0;
}

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jint JNICALL
Java_com_bachatas4_android_runtime_vortek_VortekWindowBridge_nativeCopyAhbToBuffer(
    JNIEnv* env,
    jclass,
    jlong hardwareBufferPtr,
    jobject directBuffer,
    jint expectedWidth,
    jint expectedHeight) {
    if (!directBuffer) return -1;
    void* dst = (*env)->GetDirectBufferAddress(env, directBuffer);
    jlong cap = (*env)->GetDirectBufferCapacity(env, directBuffer);
    if (!dst || cap <= 0) return -1;
    return bachata_ahb_copy_to_buffer(hardwareBufferPtr, dst, (int)cap,
                                      expectedWidth, expectedHeight);
}

#ifdef __cplusplus
}
#endif
