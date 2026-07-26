/* VkContext + request-dispatch loop for Bachata Task 6/7.
 * Reuses matched upstream request_handler; optional JNI window bridge for WSI.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>
#include <jni.h>

#include "arrays.h"
#include "vk_context.h"
#include "request_handler.h"
#include "request_codes.h"
#include "vulkan_helper.h"
#include "vulkan_wrapper.h"
#include "vortek.h"
#include "winlator.h"
#include "events.h"
#include "bachata_vk_context.h"
#include "bachata_vortek_io.h"

#define BACHATA_LOG(...) __android_log_print(ANDROID_LOG_INFO, "Bachata.Vortek", __VA_ARGS__)
#define BACHATA_ERR(...) __android_log_print(ANDROID_LOG_ERROR, "Bachata.Vortek", __VA_ARGS__)

/* Global dispatch table (normally defined in Winlator main.c). */
VulkanWrapper vulkanWrapper = {0};

static void* g_libvulkan = NULL;
static VkContext* g_dispatch_ctx = NULL;
static pthread_mutex_t g_dispatch_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Optional WSI window bridge (Java object with getWindow* methods). */
static JavaVM* g_jvm = NULL;
static jobject g_window_bridge = NULL; /* global ref */

static void disableUnsupportedDeviceExtensions(VkContext* context) {
    static const char* unsupported[] = {
        "VK_EXT_vertex_input_dynamic_state",
    };

    if (!context->disabledDeviceExtensions) {
        context->disabledDeviceExtensions =
            ArrayList_fromStrings(unsupported, sizeof(unsupported) / sizeof(unsupported[0]));
        return;
    }

    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        bool alreadyDisabled = false;
        for (int j = 0; j < context->disabledDeviceExtensions->size; j++) {
            const char* extension = context->disabledDeviceExtensions->elements[j];
            if (strcmp(extension, unsupported[i]) == 0) {
                alreadyDisabled = true;
                break;
            }
        }
        if (!alreadyDisabled) {
            ArrayList_add(context->disabledDeviceExtensions, strdup(unsupported[i]));
        }
    }
}

static void loadJMethods(JMethods* jmethods) {
    if (!jmethods->jvm || !jmethods->obj) {
        BACHATA_LOG("jmethods=absent (headless)");
        return;
    }
    JNIEnv* env = NULL;
    (*jmethods->jvm)->AttachCurrentThread(jmethods->jvm, &env, NULL);
    jmethods->env = env;
    jclass cls = (*env)->GetObjectClass(env, jmethods->obj);
    jmethods->getWindowWidth = (*env)->GetMethodID(env, cls, "getWindowWidth", "(I)I");
    jmethods->getWindowHeight = (*env)->GetMethodID(env, cls, "getWindowHeight", "(I)I");
    jmethods->getWindowHardwareBuffer = (*env)->GetMethodID(env, cls, "getWindowHardwareBuffer", "(IZ)J");
    jmethods->updateWindowContent = (*env)->GetMethodID(env, cls, "updateWindowContent", "(I)V");
    if (!jmethods->getWindowWidth || !jmethods->getWindowHeight ||
        !jmethods->getWindowHardwareBuffer || !jmethods->updateWindowContent) {
        BACHATA_ERR("jmethods=resolve_failed");
    } else {
        BACHATA_LOG("jmethods=ready");
    }
    (*env)->DeleteLocalRef(env, cls);
}

/* Matched Winlator path: large payloads arrive on the control socket first
 * (SEND_EXTRA_DATA), then a packed ring header references the request id. */
static ExtraDataRequest* waitForExtraDataRequest(VkContext* context, uint16_t requestId) {
    ExtraDataRequest* result = NULL;
    uint32_t busyWaitIter = 0;

    while (context->status >= 0) {
        result = NULL;
        pthread_mutex_lock(&context->extraDataRequestsMutex);
        for (int i = 0; i < context->extraDataRequests.size; i++) {
            ExtraDataRequest* extraDataRequest = context->extraDataRequests.elements[i];
            if (extraDataRequest->requestId == requestId) {
                result = extraDataRequest;
                ArrayList_removeAt(&context->extraDataRequests, i);
                break;
            }
        }
        pthread_mutex_unlock(&context->extraDataRequestsMutex);

        if (result) break;
        busyWait(&busyWaitIter);
    }

    return context->status >= 0 ? result : NULL;
}

bool handleExtraDataRequest(VkContext* context, uint16_t requestId, int requestLength) {
    if (!context) return false;
    void* data = NULL;
    if (requestLength > 0) {
        data = calloc((size_t)requestLength, 1);
        if (!data) return false;
        const int bytesRead = bachata_vortek_sock_read(context->clientFd, (char*)data, requestLength);
        if (bytesRead != requestLength) {
            free(data);
            return false;
        }
    }

    ExtraDataRequest* extraDataRequest = calloc(1, sizeof(ExtraDataRequest));
    if (!extraDataRequest) {
        free(data);
        return false;
    }
    extraDataRequest->requestId = requestId;
    extraDataRequest->size = requestLength;
    extraDataRequest->data = data;

    pthread_mutex_lock(&context->extraDataRequestsMutex);
    ArrayList_add(&context->extraDataRequests, extraDataRequest);
    pthread_mutex_unlock(&context->extraDataRequestsMutex);
    BACHATA_LOG("extra_data=queued id=%u size=%d", (unsigned)requestId, requestLength);
    return true;
}

bool bachata_vk_handle_extra_data(uint16_t requestId, int requestLength) {
    pthread_mutex_lock(&g_dispatch_mutex);
    VkContext* context = g_dispatch_ctx;
    pthread_mutex_unlock(&g_dispatch_mutex);
    if (!context) {
        BACHATA_ERR("extra_data=no_context id=%u", (unsigned)requestId);
        return false;
    }
    return handleExtraDataRequest(context, requestId, requestLength);
}

static void* request_thread(void* param) {
    VkContext* context = (VkContext*)param;
    ExtraDataRequest* extraDataRequest = NULL;

    loadJMethods(&context->jmethods);
    BACHATA_LOG("dispatch=start");

    while (context->status >= 0) {
        extraDataRequest = NULL;
        int requestCode = vt_recv(context->serverRing, &context->inputBuffer, &context->inputBufferSize,
                                  &context->memoryPool);
        if (requestCode < 0) break;

        if (requestCode > INT16_MAX) {
            /* PACK16(vulkanCode, requestId) — payload already on control socket. */
            uint16_t requestId = (uint16_t)(requestCode & 0xffff);
            requestCode = requestCode >> 16;
            BACHATA_LOG("extra_data=wait id=%u code=%d", (unsigned)requestId, requestCode);
            extraDataRequest = waitForExtraDataRequest(context, requestId);
            if (!extraDataRequest) {
                BACHATA_ERR("extra_data=missing id=%u code=%d", (unsigned)requestId, requestCode);
                break;
            }
            context->inputBufferSize = extraDataRequest->size;
            context->inputBuffer = extraDataRequest->data;
        }

        HandleRequestFunc handleRequestFunc = getHandleRequestFunc((short)requestCode);
        if (handleRequestFunc) {
            handleRequestFunc(context);
            if (requestCode == REQUEST_CODE_VK_CREATE_INSTANCE) {
                disableUnsupportedDeviceExtensions(context);
            }
        } else {
            BACHATA_ERR("unsupported_request=code=%d size=%d", requestCode, context->inputBufferSize);
            if (context->clientRing) {
                vt_send(context->clientRing, VK_ERROR_FEATURE_NOT_PRESENT, NULL, 0);
            }
        }

        vt_free(&context->memoryPool);
        if (extraDataRequest) {
            MEMFREE(extraDataRequest->data);
            MEMFREE(extraDataRequest);
            extraDataRequest = NULL;
        }
        context->inputBuffer = NULL;
        context->inputBufferSize = 0;
    }

    if (context->jmethods.jvm) {
        (*context->jmethods.jvm)->DetachCurrentThread(context->jmethods.jvm);
        context->jmethods.env = NULL;
    }
    BACHATA_LOG("dispatch=stop status=%d", (int)context->status);
    vt_free(&context->memoryPool);
    return NULL;
}

/* Truthful bridge API ceiling: intersection of host + client + server + tested path.
 * Task 8: 1.3.0 once Gates A–D (entry points, dispatch, pNext, shad probe) pass. */
#ifndef BACHATA_VORTEK_APPROVED_API_VERSION
#define BACHATA_VORTEK_APPROVED_API_VERSION VK_MAKE_VERSION(1, 3, 0)
#endif

static uint32_t bachata_query_host_api_version(void) {
    if (!vulkanWrapper.vkEnumerateInstanceVersion) {
        return VK_MAKE_VERSION(1, 0, 0);
    }
    uint32_t version = 0;
    if (vulkanWrapper.vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version == 0) {
        return VK_MAKE_VERSION(1, 0, 0);
    }
    return version;
}

static uint32_t bachata_negotiate_vk_max_version(void) {
    const uint32_t host = bachata_query_host_api_version();
    const uint32_t approved = BACHATA_VORTEK_APPROVED_API_VERSION;
    /* Expose only the min of host support and the approved bridge ceiling. */
    uint32_t exposed = host < approved ? host : approved;
    /* Never advertise below 1.1.128 (Task 5–7 baseline) if host is at least that. */
    const uint32_t floor_v = VK_MAKE_VERSION(1, 1, 128);
    if (host >= floor_v && exposed < floor_v) {
        exposed = floor_v;
    }
    BACHATA_LOG("capability host_api=%u.%u.%u approved=%u.%u.%u exposed=%u.%u.%u",
                VK_VERSION_MAJOR(host), VK_VERSION_MINOR(host), VK_VERSION_PATCH(host),
                VK_VERSION_MAJOR(approved), VK_VERSION_MINOR(approved), VK_VERSION_PATCH(approved),
                VK_VERSION_MAJOR(exposed), VK_VERSION_MINOR(exposed), VK_VERSION_PATCH(exposed));
    return exposed;
}

int bachata_vk_init_loader(void) {
    if (g_libvulkan) return 0;
    g_libvulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_libvulkan) {
        g_libvulkan = dlopen(LIBVULKAN_PATH, RTLD_NOW | RTLD_LOCAL);
    }
    if (!g_libvulkan) {
        BACHATA_ERR("host_loader=open_failed error=%s", dlerror());
        return -1;
    }
    initVulkanWrapper(&vulkanWrapper, g_libvulkan);
    if (!vulkanWrapper.vkGetInstanceProcAddr || !vulkanWrapper.vkCreateInstance) {
        BACHATA_ERR("host_loader=missing_entry");
        return -1;
    }
    BACHATA_LOG("host_loader=libvulkan.so dispatch=ready");
    return 0;
}

int bachata_vk_set_window_bridge(JNIEnv* env, jobject bridge) {
    if (!env) return -1;
    if (g_window_bridge) {
        (*env)->DeleteGlobalRef(env, g_window_bridge);
        g_window_bridge = NULL;
        g_jvm = NULL;
    }
    if (!bridge) {
        BACHATA_LOG("window_bridge=cleared");
        return 0;
    }
    if ((*env)->GetJavaVM(env, &g_jvm) != 0 || !g_jvm) {
        BACHATA_ERR("window_bridge=get_jvm_failed");
        return -1;
    }
    g_window_bridge = (*env)->NewGlobalRef(env, bridge);
    if (!g_window_bridge) {
        g_jvm = NULL;
        BACHATA_ERR("window_bridge=global_ref_failed");
        return -1;
    }
    BACHATA_LOG("window_bridge=attached");
    return 0;
}

int bachata_vk_start_dispatch(RingBuffer* serverRing, RingBuffer* clientRing, int clientFd) {
    pthread_mutex_lock(&g_dispatch_mutex);
    if (g_dispatch_ctx) {
        pthread_mutex_unlock(&g_dispatch_mutex);
        BACHATA_ERR("dispatch=already_running");
        return -1;
    }
    if (bachata_vk_init_loader() != 0) {
        pthread_mutex_unlock(&g_dispatch_mutex);
        return -1;
    }

    VkContext* context = calloc(1, sizeof(VkContext));
    if (!context) {
        pthread_mutex_unlock(&g_dispatch_mutex);
        return -1;
    }
    context->clientFd = clientFd;
    context->vkMaxVersion = (int)bachata_negotiate_vk_max_version();
    context->maxDeviceMemory = 0;
    context->imageCacheSize = 0;
    context->resourceMemoryType = 0;
    context->status = VK_SUCCESS;
    context->serverRing = serverRing;
    context->clientRing = clientRing;
    context->memoryPool.data = calloc(1, MEMORY_POOL_MAX_SIZE);
    if (!context->memoryPool.data) {
        free(context);
        pthread_mutex_unlock(&g_dispatch_mutex);
        return -1;
    }
    pthread_mutex_init(&context->extraDataRequestsMutex, NULL);
    context->textureDecoder = NULL;
    context->shaderInspector = NULL;
    /* Timeline WaitSemaphores uses async ThreadPool (matched Winlator path). */
    context->threadPool = ThreadPool_init(THREAD_POOL_NUM_THREADS);
    if (!context->threadPool) {
        BACHATA_ERR("thread_pool=init_failed");
        MEMFREE(context->memoryPool.data);
        free(context);
        pthread_mutex_unlock(&g_dispatch_mutex);
        return -1;
    }
    BACHATA_LOG("thread_pool=ready threads=%d", THREAD_POOL_NUM_THREADS);

    /* Wire optional WSI bridge for X window → AHB path. */
    if (g_jvm && g_window_bridge) {
        context->jmethods.jvm = g_jvm;
        context->jmethods.obj = g_window_bridge;
        BACHATA_LOG("dispatch=wsi_bridge_enabled");
    } else {
        BACHATA_LOG("dispatch=wsi_bridge_absent");
    }

    if (pthread_create(&context->requestHandlerThread, NULL, request_thread, context) != 0) {
        MEMFREE(context->memoryPool.data);
        free(context);
        pthread_mutex_unlock(&g_dispatch_mutex);
        return -1;
    }

    g_dispatch_ctx = context;
    pthread_mutex_unlock(&g_dispatch_mutex);
    BACHATA_LOG("dispatch=started");
    return 0;
}

/* Declared in request_handler.c — drain async fence wait workers on teardown. */
void bachata_vortek_join_fence_wait_workers(void);

void bachata_vk_stop_dispatch(void) {
    pthread_mutex_lock(&g_dispatch_mutex);
    VkContext* context = g_dispatch_ctx;
    g_dispatch_ctx = NULL;
    pthread_mutex_unlock(&g_dispatch_mutex);

    if (!context) return;

    context->status = VK_ERROR_DEVICE_LOST;
    if (context->serverRing) RingBuffer_setStatus(context->serverRing, RING_STATUS_EXIT);
    if (context->clientRing) RingBuffer_setStatus(context->clientRing, RING_STATUS_EXIT);

    if (context->requestHandlerThread) {
        pthread_join(context->requestHandlerThread, NULL);
        context->requestHandlerThread = 0;
    }
    bachata_vortek_join_fence_wait_workers();

    /* Do not DeleteGlobalRef here — bridge lifetime owned by set_window_bridge. */
    context->jmethods.obj = NULL;
    context->jmethods.jvm = NULL;

    if (context->threadPool) {
        ThreadPool_destroy(context->threadPool);
        context->threadPool = NULL;
    }

    ArrayList_free(context->exposedDeviceExtensions, true);
    ArrayList_free(context->disabledDeviceExtensions, true);
    ArrayList_free(&context->extraDataRequests, true);
    pthread_mutex_destroy(&context->extraDataRequestsMutex);
    MEMFREE(context->memoryPool.data);
    MEMFREE(context->engineName);
    free(context);
    BACHATA_LOG("dispatch=stopped");
}

int bachata_vk_dispatch_running(void) {
    return g_dispatch_ctx != NULL;
}
