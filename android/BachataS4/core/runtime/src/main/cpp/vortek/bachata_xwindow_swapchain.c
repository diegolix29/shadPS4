/*
 * Bachata Vortek WSI synchronization adapter.
 *
 * Upstream Vortek calls updateWindowContent immediately from vkQueuePresentKHR.
 * Bachata's Canvas compositor then CPU-locks and copies the swapchain AHB, so the
 * host queue must finish the preceding render submission before that read.
 */
#include <android/log.h>

#define XWindowSwapchain_presentImage BachataUpstreamXWindowSwapchain_presentImage
#include "xwindow_swapchain.c"
#undef XWindowSwapchain_presentImage

void XWindowSwapchain_presentImage(XWindowSwapchain* swapchain) {
    VkResult result = vulkanWrapper.vkQueueWaitIdle(swapchain->queue);
    if (result != VK_SUCCESS) {
        __android_log_print(
            ANDROID_LOG_ERROR,
            "Bachata.Vortek",
            "present_sync_failed result=%d",
            (int)result
        );
        return;
    }
    BachataUpstreamXWindowSwapchain_presentImage(swapchain);
}
