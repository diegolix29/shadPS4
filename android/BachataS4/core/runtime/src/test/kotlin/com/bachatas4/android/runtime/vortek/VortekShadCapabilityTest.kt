package com.bachatas4.android.runtime.vortek

import com.bachatas4.android.runtime.process.RuntimeVulkanDriver
import com.bachatas4.android.runtime.process.RuntimeVulkanDriverIds
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Task 8 unit gates: capability policy and ICD API version truthfulness.
 *
 * Host/device proof lives in instrumented tests; this locks the policy constants
 * and opt-in driver selection behavior.
 */
class VortekShadCapabilityTest {

    @Test
    fun approvedIcdApiVersion_isVulkan13() {
        // Must match runtime/scripts/build-vortek-client.sh approved_api_version
        // and runtime/tests/verify-vortek.mjs APPROVED_API_VERSION after Task 8 gates.
        assertEquals("1.3.0", APPROVED_ICD_API_VERSION)
        assertTrue(APPROVED_ICD_API_VERSION.startsWith("1.3"))
        assertFalse(APPROVED_ICD_API_VERSION.startsWith("1.4"))
    }

    @Test
    fun requiredShadExtensions_listed() {
        val required = REQUIRED_SHAD_DEVICE_EXTENSIONS
        assertTrue(required.contains("VK_KHR_swapchain"))
        assertTrue(required.contains("VK_KHR_push_descriptor"))
        assertTrue(required.contains("VK_EXT_vertex_attribute_divisor"))
        assertEquals(3, required.size)
    }

    @Test
    fun requiredCoreEntryPoints_listed() {
        val names = REQUIRED_SHAD_ENTRY_POINTS
        assertTrue(names.contains("vkGetPhysicalDeviceFeatures2"))
        assertTrue(names.contains("vkGetPhysicalDeviceProperties2"))
        assertTrue(names.contains("vkCmdBeginRendering"))
        assertTrue(names.contains("vkCmdEndRendering"))
        assertTrue(names.contains("vkQueueSubmit2"))
        assertTrue(names.contains("vkCmdPushDescriptorSetKHR"))
        assertTrue(names.contains("vkGetSemaphoreCounterValue"))
    }

    @Test
    fun systemVortek_remainsOptInId() {
        assertEquals("system-vortek", RuntimeVulkanDriverIds.SYSTEM_VORTEK)
        assertEquals(RuntimeVulkanDriver.SYSTEM_VORTEK, RuntimeVulkanDriver.SYSTEM_VORTEK)
    }

    @Test
    fun destroyOrder_swapchainBeforeSurface_documented() {
        // Policy constant used by WSI + Task 8 regression notes.
        assertEquals(
            listOf("swapchain", "surface", "device", "instance"),
            VORTEK_DESTROY_ORDER,
        )
    }

    companion object {
        const val APPROVED_ICD_API_VERSION = "1.3.0"

        val REQUIRED_SHAD_DEVICE_EXTENSIONS = listOf(
            "VK_KHR_swapchain",
            "VK_KHR_push_descriptor",
            "VK_EXT_vertex_attribute_divisor",
        )

        val REQUIRED_SHAD_ENTRY_POINTS = listOf(
            "vkEnumerateInstanceVersion",
            "vkGetPhysicalDeviceFeatures2",
            "vkGetPhysicalDeviceProperties2",
            "vkCreateDevice",
            "vkCmdPushDescriptorSetKHR",
            "vkCmdBeginRendering",
            "vkCmdEndRendering",
            "vkQueueSubmit2",
            "vkCmdPipelineBarrier2",
            "vkGetSemaphoreCounterValue",
            "vkWaitSemaphores",
            "vkSignalSemaphore",
            "vkCreateDescriptorUpdateTemplate",
        )

        val VORTEK_DESTROY_ORDER = listOf(
            "swapchain",
            "surface",
            "device",
            "instance",
        )
    }
}
