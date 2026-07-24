package com.bachatas4.android.runtime.device

import android.os.Build
import com.bachatas4.android.model.DeviceProfile

fun interface SocModelProvider {
    fun socModel(): String
}

fun interface GpuCapabilityProvider {
    fun capability(): GpuCapability
}

sealed interface GpuCapability {
    data object Unverified : GpuCapability

    data class Verified(val model: String) : GpuCapability
}

class DeviceCapabilityProbe(
    private val socModelProvider: SocModelProvider = AndroidSocModelProvider,
    private val gpuCapabilityProvider: GpuCapabilityProvider =
        GpuCapabilityProvider { GpuCapability.Unverified },
) {
    fun probe(): DeviceProfile {
        val soc = socModelProvider.socModel()
        return when (val gpu = gpuCapabilityProvider.capability()) {
            GpuCapability.Unverified -> DeviceProfile(
                soc = soc,
                gpu = UNVERIFIED_GPU,
                supported = true,
            )
            is GpuCapability.Verified -> classify(soc, gpu.model)
        }
    }
}

/** Classify hardware for display; no SoC/GPU allowlist gate. */
internal fun classify(soc: String, gpu: String): DeviceProfile =
    DeviceProfile(soc = soc, gpu = gpu, supported = true)

private data object AndroidSocModelProvider : SocModelProvider {
    override fun socModel(): String = Build.SOC_MODEL
}

private const val UNVERIFIED_GPU = "unverified"
