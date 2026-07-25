package com.bachatas4.android.feature.drivers

import com.bachatas4.android.runtime.driver.InstalledDriver
import com.bachatas4.android.runtime.driver.TurnipReleaseAsset
import com.bachatas4.android.runtime.process.VulkanDriverConfiguration
import com.bachatas4.android.runtime.process.VulkanDriverResolveContext
import java.nio.file.Path

data class DriverManagerCapabilities(
    val remoteCatalogEnabled: Boolean,
    val importEnabled: Boolean,
    val deleteEnabled: Boolean,
    /** Shown in the drivers screen header when non-null. */
    val statusMessage: String? = null,
)

/**
 * Driver catalogue / install backend.
 *
 * Play Store builds provide a backend that only surfaces the bundled Turnip package.
 * Non-Play builds keep remote download + ZIP import.
 */
interface DriverManagerBackend {
    fun capabilities(): DriverManagerCapabilities
    fun installed(): List<InstalledDriver>
    fun releases(force: Boolean): List<TurnipReleaseAsset>
    fun download(asset: TurnipReleaseAsset, progress: (Long, Long) -> Unit): InstalledDriver
    fun importZip(bytes: ByteArray, assetName: String): InstalledDriver
    fun remove(id: String): Boolean

    /**
     * Resolve Vulkan configuration for launch. Play backends may remap stale Turnip ids
     * to the bundled package; non-Play backends load any installed id.
     * [SYSTEM_VORTEK]([com.bachatas4.android.runtime.process.RuntimeVulkanDriverIds.SYSTEM_VORTEK])
     * is never remapped to Turnip and requires a session socket in [context].
     */
    fun configurationFor(driverId: String, context: VulkanDriverResolveContext): VulkanDriverConfiguration

    /** Convenience for non-Vortek resolution without a session socket. */
    fun configurationFor(driverId: String, runtimeRoot: Path): VulkanDriverConfiguration =
        configurationFor(driverId, VulkanDriverResolveContext(runtimeRoot = runtimeRoot))

    /**
     * When non-null, setup should skip the driver picker and persist this id
     * (Play: bundled Turnip). F-Droid returns null and shows the picker.
     * Never returns system-vortek (experimental opt-in only).
     */
    fun autoSelectDriverId(): String? = null
}
