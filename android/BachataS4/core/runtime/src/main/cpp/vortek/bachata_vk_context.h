#pragma once

#include <jni.h>
#include "ring_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Open system libvulkan.so and resolve global entry points. Idempotent. */
int bachata_vk_init_loader(void);

/**
 * Attach or clear the Java window bridge used by WSI (getWindowWidth/Height/AHB/update).
 * Pass NULL to clear. Must be called from a JNI thread with a valid env.
 */
int bachata_vk_set_window_bridge(JNIEnv* env, jobject bridge);

/**
 * Start request dispatch on rings already created by CREATE_CONTEXT.
 * serverRing/clientRing ownership stays with the session; this only uses them.
 */
int bachata_vk_start_dispatch(RingBuffer* serverRing, RingBuffer* clientRing, int clientFd);

/** Signal EXIT, join request thread, free VkContext (not the rings). */
void bachata_vk_stop_dispatch(void);

int bachata_vk_dispatch_running(void);

/**
 * Matched Winlator SEND_EXTRA_DATA path: read `requestLength` bytes from the
 * active dispatch client socket and queue them for the request thread.
 * Call from the control-socket session loop after CREATE_CONTEXT.
 */
bool bachata_vk_handle_extra_data(uint16_t requestId, int requestLength);

#ifdef __cplusplus
}
#endif
