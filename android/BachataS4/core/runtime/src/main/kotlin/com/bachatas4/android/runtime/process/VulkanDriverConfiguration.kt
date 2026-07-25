package com.bachatas4.android.runtime.process

import com.bachatas4.android.runtime.driver.DriverAbi
import com.bachatas4.android.runtime.driver.InstalledDriver
import java.nio.file.Files
import java.nio.file.Path

enum class RuntimeVulkanDriver {
    SYSTEM,
    SYSTEM_VORTEK,
    CUSTOM,
    TURNIP_25_0_0,
    TURNIP_25_3_0_R11,
    TURNIP_26_1_0,
}

/**
 * Profile / backend driver ids used by [com.bachatas4.android.runtime.settings.RuntimeProfile.driverId].
 * Synthetic ids (not present under vulkan-drivers/installed).
 */
object RuntimeVulkanDriverIds {
    const val SYSTEM = "system"
    const val SYSTEM_VORTEK = "system-vortek"

    fun isSynthetic(id: String): Boolean =
        id == SYSTEM || id == SYSTEM_VORTEK

    fun isVortek(id: String?): Boolean = id == SYSTEM_VORTEK
}

object RuntimeVulkanDriverPreference {
    const val FILE_NAME = "emulator_settings"
    const val KEY = "vulkan_driver"
    /** Safe default remains legacy SYSTEM (APK native bridge), not experimental Vortek. */
    val DEFAULT = RuntimeVulkanDriver.SYSTEM

    /**
     * Decode a stored preference value.
     *
     * Migration policy:
     * - `SYSTEM` stays [RuntimeVulkanDriver.SYSTEM] (existing production behavior).
     * - `SYSTEM_VORTEK` is opt-in experimental only.
     * - Unknown / null / blank → [DEFAULT] (never auto-select Vortek).
     * - Enum names are matched case-sensitively.
     */
    fun decode(value: String?): RuntimeVulkanDriver {
        if (value.isNullOrBlank()) return DEFAULT
        return RuntimeVulkanDriver.entries.firstOrNull { it.name == value } ?: DEFAULT
    }
}

/**
 * Session-scoped inputs for driver resolution.
 * Socket path is owned by the session lifecycle — never generated here.
 */
data class VulkanDriverResolveContext(
    val runtimeRoot: Path,
    val customDriverRoot: Path? = null,
    val vortekSocketPath: Path? = null,
)

data class VulkanDriverConfiguration(
    val box64Mode: Box64Mode,
    val environment: Map<String, String>,
    /** Stable log / diagnostics id (e.g. system-vortek, turnip-...). */
    val driverProfileId: String = "unknown",
) {
    companion object {
        const val VORTEK_CLIENT_BUILD =
            "ab7329c4b445a4abd9b9af91b8148e1ca41464fa"
        const val VORTEK_ICD_RELATIVE = "host/vulkan/icd.d/vortek.json"
        const val VORTEK_LIBRARY_RELATIVE = "host/lib/libvulkan_vortek.so"
        const val HOST_LOADER_RELATIVE = "host/libvulkan.so.1"

        fun resolve(driver: InstalledDriver, runtimeRoot: Path): VulkanDriverConfiguration =
            when (driver.metadata.abi) {
                DriverAbi.LINUX_GLIBC -> VulkanDriverConfiguration(
                    box64Mode = Box64Mode.HOST_GLIBC,
                    environment = mapOf(
                        "SDL_VULKAN_LIBRARY" to runtimeRoot.resolve(HOST_LOADER_RELATIVE).toString(),
                        "VK_ICD_FILENAMES" to requireNotNull(driver.icdManifest).toString(),
                    ),
                    driverProfileId = driver.metadata.id,
                )
                DriverAbi.ANDROID_BIONIC -> VulkanDriverConfiguration(
                    box64Mode = Box64Mode.APK_NATIVE,
                    environment = mapOf(
                        "SDL_VULKAN_LIBRARY" to "libvulkan.so.1",
                        "BACHATA_VULKAN_DRIVER_DIR" to driver.root.toString() + "/",
                        "BACHATA_VULKAN_DRIVER_NAME" to driver.library.fileName.toString(),
                        "BACHATA_VULKAN_TMPDIR" to runtimeRoot.resolve("tmp").toString(),
                    ),
                    driverProfileId = driver.metadata.id,
                )
            }

        fun resolve(
            driver: RuntimeVulkanDriver,
            runtimeRoot: Path,
            customDriverRoot: Path? = null,
            vortekSocketPath: Path? = null,
        ): VulkanDriverConfiguration =
            resolve(
                driver,
                VulkanDriverResolveContext(
                    runtimeRoot = runtimeRoot,
                    customDriverRoot = customDriverRoot,
                    vortekSocketPath = vortekSocketPath,
                ),
            )

        fun resolve(
            driver: RuntimeVulkanDriver,
            context: VulkanDriverResolveContext,
        ): VulkanDriverConfiguration {
            val runtimeRoot = context.runtimeRoot
            return when (driver) {
                RuntimeVulkanDriver.SYSTEM -> VulkanDriverConfiguration(
                    box64Mode = Box64Mode.APK_NATIVE,
                    environment = mapOf(
                        "SDL_VULKAN_LIBRARY" to "libvulkan.so.1",
                    ),
                    driverProfileId = RuntimeVulkanDriverIds.SYSTEM,
                )
                RuntimeVulkanDriver.SYSTEM_VORTEK -> resolveVortek(context)
                RuntimeVulkanDriver.CUSTOM -> VulkanDriverConfiguration(
                    box64Mode = Box64Mode.HOST_GLIBC,
                    environment = mapOf(
                        "SDL_VULKAN_LIBRARY" to runtimeRoot.resolve(HOST_LOADER_RELATIVE).toString(),
                        "VK_ICD_FILENAMES" to requireNotNull(context.customDriverRoot) {
                            "Custom Vulkan driver is not installed"
                        }.resolve("freedreno_icd.aarch64.json").toString(),
                    ),
                    driverProfileId = "custom",
                )
                RuntimeVulkanDriver.TURNIP_25_0_0 -> VulkanDriverConfiguration(
                    box64Mode = Box64Mode.HOST_GLIBC,
                    environment = mapOf(
                        "SDL_VULKAN_LIBRARY" to runtimeRoot.resolve(HOST_LOADER_RELATIVE).toString(),
                        "VK_ICD_FILENAMES" to runtimeRoot.resolve("host/vulkan/icd.d/turnip-25.0.0.json").toString(),
                    ),
                    driverProfileId = "turnip-25.0.0",
                )
                RuntimeVulkanDriver.TURNIP_25_3_0_R11 -> VulkanDriverConfiguration(
                    box64Mode = Box64Mode.APK_NATIVE,
                    environment = mapOf(
                        "SDL_VULKAN_LIBRARY" to "libvulkan.so.1",
                        "BACHATA_VULKAN_DRIVER_DIR" to
                            runtimeRoot.resolve("drivers/turnip-25.3.0-r11").toString() + "/",
                        "BACHATA_VULKAN_DRIVER_NAME" to "vulkan.ad07xx.so",
                        "BACHATA_VULKAN_TMPDIR" to runtimeRoot.resolve("tmp").toString(),
                    ),
                    driverProfileId = "turnip-25.3.0-r11",
                )
                RuntimeVulkanDriver.TURNIP_26_1_0 -> VulkanDriverConfiguration(
                    box64Mode = Box64Mode.HOST_GLIBC,
                    environment = mapOf(
                        "SDL_VULKAN_LIBRARY" to runtimeRoot.resolve(HOST_LOADER_RELATIVE).toString(),
                        "VK_ICD_FILENAMES" to runtimeRoot.resolve("host/vulkan/icd.d/turnip-26.1.0.json").toString(),
                    ),
                    driverProfileId = "turnip-26.1.0",
                )
            }
        }

        fun resolveByDriverId(
            driverId: String,
            context: VulkanDriverResolveContext,
            installedResolver: (String) -> InstalledDriver? = { null },
        ): VulkanDriverConfiguration =
            when (driverId) {
                RuntimeVulkanDriverIds.SYSTEM -> resolve(RuntimeVulkanDriver.SYSTEM, context)
                RuntimeVulkanDriverIds.SYSTEM_VORTEK -> resolve(RuntimeVulkanDriver.SYSTEM_VORTEK, context)
                else -> {
                    val installed = installedResolver(driverId)
                        ?: throw IllegalStateException(
                            "Selected Vulkan driver '$driverId' is not installed; open Turnip drivers and select another driver",
                        )
                    resolve(installed, context.runtimeRoot)
                }
            }

        private fun resolveVortek(context: VulkanDriverResolveContext): VulkanDriverConfiguration {
            val socket = context.vortekSocketPath
                ?: throw IllegalArgumentException(
                    "SYSTEM_VORTEK requires session-owned vortekSocketPath (session lifecycle must supply it)",
                )
            val socketPath = socket.toString()
            require(socketPath.isNotBlank() && socketPath.startsWith("/")) {
                "Invalid Vortek socket path"
            }
            require("com.winlator" !in socketPath) { "Vortek socket must not use a Winlator path" }

            val runtimeRoot = context.runtimeRoot
            val loader = runtimeRoot.resolve(HOST_LOADER_RELATIVE)
            val library = runtimeRoot.resolve(VORTEK_LIBRARY_RELATIVE)
            val icd = runtimeRoot.resolve(VORTEK_ICD_RELATIVE)

            require(Files.isRegularFile(loader)) {
                "Missing packaged host Vulkan loader: $loader"
            }
            require(Files.isRegularFile(library)) {
                "Missing packaged Vortek client library: $library"
            }
            require(Files.isRegularFile(icd)) {
                "Missing packaged Vortek ICD: $icd"
            }
            val icdText = icd.toFile().readText()
            require("com.winlator" !in icdText) {
                "Vortek ICD must not contain a Winlator path"
            }

            return VulkanDriverConfiguration(
                box64Mode = Box64Mode.HOST_GLIBC,
                environment = mapOf(
                    "SDL_VULKAN_LIBRARY" to loader.toString(),
                    "VK_ICD_FILENAMES" to icd.toString(),
                    "BACHATA_VORTEK_SOCKET" to socketPath,
                    "BACHATA_VORTEK_HANDSHAKE" to "1",
                    "BACHATA_CRASH_REGISTERS" to "1",
                    "BACHATA_VORTEK_PROC_AUDIT" to "1",
                ),
                driverProfileId = RuntimeVulkanDriverIds.SYSTEM_VORTEK,
            )
        }
    }
}
