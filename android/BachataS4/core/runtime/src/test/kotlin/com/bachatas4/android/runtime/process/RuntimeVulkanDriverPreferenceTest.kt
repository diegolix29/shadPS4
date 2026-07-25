package com.bachatas4.android.runtime.process

import org.junit.Assert.assertEquals
import org.junit.Test

class RuntimeVulkanDriverPreferenceTest {
    @Test
    fun defaultsToSystemWhenPreferenceIsMissingOrInvalid() {
        assertEquals(RuntimeVulkanDriver.SYSTEM, RuntimeVulkanDriverPreference.decode(null))
        assertEquals(RuntimeVulkanDriver.SYSTEM, RuntimeVulkanDriverPreference.decode(""))
        assertEquals(RuntimeVulkanDriver.SYSTEM, RuntimeVulkanDriverPreference.decode("   "))
        assertEquals(RuntimeVulkanDriver.SYSTEM, RuntimeVulkanDriverPreference.decode("unknown"))
        assertEquals(RuntimeVulkanDriver.SYSTEM, RuntimeVulkanDriverPreference.decode("system"))
        assertEquals(RuntimeVulkanDriver.SYSTEM, RuntimeVulkanDriverPreference.decode("System"))
    }

    @Test
    fun decodesEveryValidEnumValue() {
        for (driver in RuntimeVulkanDriver.entries) {
            assertEquals(driver, RuntimeVulkanDriverPreference.decode(driver.name))
        }
    }

    @Test
    fun legacySystemStaysSystemNotVortek() {
        // Explicit migration policy: do not silently migrate SYSTEM → SYSTEM_VORTEK.
        assertEquals(RuntimeVulkanDriver.SYSTEM, RuntimeVulkanDriverPreference.decode("SYSTEM"))
        assertEquals(
            RuntimeVulkanDriver.SYSTEM_VORTEK,
            RuntimeVulkanDriverPreference.decode("SYSTEM_VORTEK"),
        )
    }

    @Test
    fun decodesTurnipNamesInsteadOfCollapsingToSystem() {
        assertEquals(
            RuntimeVulkanDriver.TURNIP_25_0_0,
            RuntimeVulkanDriverPreference.decode(RuntimeVulkanDriver.TURNIP_25_0_0.name),
        )
        assertEquals(
            RuntimeVulkanDriver.TURNIP_26_1_0,
            RuntimeVulkanDriverPreference.decode(RuntimeVulkanDriver.TURNIP_26_1_0.name),
        )
    }

    @Test
    fun defaultIsNotVortek() {
        assertEquals(RuntimeVulkanDriver.SYSTEM, RuntimeVulkanDriverPreference.DEFAULT)
        assertEquals(RuntimeVulkanDriverIds.SYSTEM, RuntimeVulkanDriverIds.SYSTEM)
        assertEquals("system-vortek", RuntimeVulkanDriverIds.SYSTEM_VORTEK)
    }
}
