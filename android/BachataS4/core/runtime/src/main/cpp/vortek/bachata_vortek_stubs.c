/* Bachata stubs for optional Winlator Vortek features not required for Task 6/7. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <android/log.h>
#include <android/hardware_buffer.h>
#include "native_handle.h"

#define LOG_TAG "Bachata.Vortek"
#define STUB_LOG(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "stub: " __VA_ARGS__)

/* Real AHB FD helper (matched to winlator gpu_image.c) for resource_memory. */
extern const native_handle_t* AHardwareBuffer_getNativeHandle(const AHardwareBuffer* buffer);

int AHardwareBuffer_getFd(AHardwareBuffer* hardwareBuffer) {
    if (!hardwareBuffer) return -1;
    const native_handle_t* nativeHandle = AHardwareBuffer_getNativeHandle(hardwareBuffer);
    if (!nativeHandle) return -1;
    return nativeHandle->numFds > 0 ? nativeHandle->data[0] : -1;
}

/* ---- Texture decoder (optional; not required for WSI present probe) ---- */
void* TextureDecoder_create(void) { return NULL; }
void TextureDecoder_destroy(void* d) { (void)d; }
void TextureDecoder_decodeAll(void* d) { (void)d; }
void TextureDecoder_addBoundBuffer(void* d, void* a, void* b) { (void)d; (void)a; (void)b; }
void TextureDecoder_removeBoundBuffer(void* d, void* a) { (void)d; (void)a; }
void* TextureDecoder_createImage(void* d, void* a, void* b, void* c) {
    (void)d; (void)a; (void)b; (void)c;
    return NULL;
}
int TextureDecoder_containsImage(void* d, void* img) { (void)d; (void)img; return 0; }
void TextureDecoder_destroyImage(void* d, void* img) { (void)d; (void)img; }
void TextureDecoder_copyBufferToImage(void* d, void* a, void* b, void* c, void* e, void* f) {
    (void)d; (void)a; (void)b; (void)c; (void)e; (void)f;
}
int isCompressedFormat(int format) { (void)format; return 0; }
void getCompressedImageFormatProperties(void* a, void* b, void* c) {
    (void)a; (void)b; (void)c;
}

/* AsyncPipelineCreator_create provided by matched upstream async_pipeline_creator.c (Task 9). */

/* TimelineSemaphore_asyncWait provided by matched upstream timeline_semaphore.c (Task 8). */

/* Global from Winlator main.c that request_handler expects. */
int vortekSerializerCastVkObject = 1;

void bachata_vortek_join_fence_wait_workers(void) {}
