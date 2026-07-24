package com.bachatas4.android.runtime.device

import com.bachatas4.android.model.DeviceProfile
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class DeviceCapabilityProbeTest {
    @Test
    fun acceptsAnySocGpuPairing() {
        assertTrue(classify("SM8650", "Adreno 750").supported)
        assertTrue(classify("SM8750", "Adreno 830").supported)
        assertTrue(classify("SM8150", "Adreno 640").supported)
        assertTrue(classify("Tensor G5", "Mali").supported)
        assertTrue(classify("SM8850", "Adreno 840").supported)
    }

    @Test
    fun unverifiedGpuDoesNotBlock() {
        val probe = DeviceCapabilityProbe(
            socModelProvider = SocModelProvider { "SM8650" },
            gpuCapabilityProvider = GpuCapabilityProvider { GpuCapability.Unverified },
        )

        assertEquals(
            DeviceProfile(soc = "SM8650", gpu = "unverified", supported = true),
            probe.probe(),
        )
    }

    @Test
    fun verifiedGpuUsesClassifierWithoutAndroidRuntime() {
        val probe = DeviceCapabilityProbe(
            socModelProvider = SocModelProvider { "SM8750" },
            gpuCapabilityProvider = GpuCapabilityProvider {
                GpuCapability.Verified(model = "Adreno 830")
            },
        )

        assertEquals(
            DeviceProfile(soc = "SM8750", gpu = "Adreno 830", supported = true),
            probe.probe(),
        )
    }
}
