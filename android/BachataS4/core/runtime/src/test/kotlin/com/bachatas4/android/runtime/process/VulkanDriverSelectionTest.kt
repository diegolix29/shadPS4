package com.bachatas4.android.runtime.process

import com.bachatas4.android.runtime.driver.DriverAbi
import com.bachatas4.android.runtime.driver.DriverPackageSource
import com.bachatas4.android.runtime.driver.TurnipPackageInstaller
import com.bachatas4.android.runtime.driver.turnipZip
import java.nio.file.Files
import java.nio.file.Paths
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class VulkanDriverSelectionTest {
    private val runtimeRoot = Paths.get("/data/user/0/com.bachatas4.android/files/runtime/current")

    @Test
    fun selectsImportedGlibcDriverManifest() {
        val customRoot = Paths.get("/data/user/0/com.bachatas4.android/files/vulkan-drivers/custom")

        val configuration = VulkanDriverConfiguration.resolve(
            RuntimeVulkanDriver.CUSTOM,
            runtimeRoot,
            customRoot,
        )

        assertEquals(Box64Mode.HOST_GLIBC, configuration.box64Mode)
        assertEquals(runtimeRoot.resolve("host/libvulkan.so.1").toString(), configuration.environment["SDL_VULKAN_LIBRARY"])
        assertEquals(customRoot.resolve("freedreno_icd.aarch64.json").toString(), configuration.environment["VK_ICD_FILENAMES"])
    }

    @Test
    fun selectsGlibcLoaderAndLatestWinlatorTurnip() {
        val configuration = VulkanDriverConfiguration.resolve(
            RuntimeVulkanDriver.TURNIP_26_1_0,
            runtimeRoot,
        )

        assertEquals(Box64Mode.HOST_GLIBC, configuration.box64Mode)
        assertEquals(runtimeRoot.resolve("host/libvulkan.so.1").toString(), configuration.environment["SDL_VULKAN_LIBRARY"])
        assertEquals(runtimeRoot.resolve("host/vulkan/icd.d/turnip-26.1.0.json").toString(), configuration.environment["VK_ICD_FILENAMES"])
        assertFalse(configuration.environment.containsKey("BACHATA_VULKAN_BRIDGE"))
        assertFalse(configuration.environment.containsKey("BACHATA_VORTEK_SOCKET"))
    }

    @Test
    fun retainsGlibcLoaderAndIcdForTurnipFallback() {
        val configuration = VulkanDriverConfiguration.resolve(
            RuntimeVulkanDriver.TURNIP_25_0_0,
            runtimeRoot,
        )

        assertEquals(Box64Mode.HOST_GLIBC, configuration.box64Mode)
        assertEquals(runtimeRoot.resolve("host/libvulkan.so.1").toString(), configuration.environment["SDL_VULKAN_LIBRARY"])
        assertEquals(runtimeRoot.resolve("host/vulkan/icd.d/turnip-25.0.0.json").toString(), configuration.environment["VK_ICD_FILENAMES"])
        assertFalse(configuration.environment.containsKey("BACHATA_VULKAN_BRIDGE"))
    }

    @Test
    fun selectsApkNativeBox64AndAdrenoToolsForTurnipR11() {
        val configuration = VulkanDriverConfiguration.resolve(
            RuntimeVulkanDriver.TURNIP_25_3_0_R11,
            runtimeRoot,
        )

        assertEquals(Box64Mode.APK_NATIVE, configuration.box64Mode)
        assertEquals("libvulkan.so.1", configuration.environment["SDL_VULKAN_LIBRARY"])
        assertEquals(
            runtimeRoot.resolve("drivers/turnip-25.3.0-r11").toString() + "/",
            configuration.environment["BACHATA_VULKAN_DRIVER_DIR"],
        )
        assertEquals("vulkan.ad07xx.so", configuration.environment["BACHATA_VULKAN_DRIVER_NAME"])
        assertEquals(runtimeRoot.resolve("tmp").toString(), configuration.environment["BACHATA_VULKAN_TMPDIR"])
        assertFalse(configuration.environment.containsKey("VK_ICD_FILENAMES"))
    }

    @Test
    fun selectsDownloadedBionicDriverThroughApkNativeBridge() {
        val installed = TurnipPackageInstaller(Files.createTempDirectory("downloaded-turnip"), 37).install(
            turnipZip(DriverAbi.ANDROID_BIONIC),
            DriverPackageSource(assetName = "Turnip-26-1.1-EMULATOR.zip"),
        )

        val configuration = VulkanDriverConfiguration.resolve(installed, runtimeRoot)

        assertEquals(Box64Mode.APK_NATIVE, configuration.box64Mode)
        assertEquals(installed.root.toString() + "/", configuration.environment["BACHATA_VULKAN_DRIVER_DIR"])
        assertEquals(installed.library.fileName.toString(), configuration.environment["BACHATA_VULKAN_DRIVER_NAME"])
    }

    @Test
    fun systemDriverDoesNotRequireVortekSocket() {
        val configuration = VulkanDriverConfiguration.resolve(
            RuntimeVulkanDriver.SYSTEM,
            VulkanDriverResolveContext(runtimeRoot = runtimeRoot),
        )
        assertEquals(Box64Mode.APK_NATIVE, configuration.box64Mode)
        assertFalse(configuration.environment.containsKey("BACHATA_VORTEK_SOCKET"))
        assertEquals(RuntimeVulkanDriverIds.SYSTEM, configuration.driverProfileId)
    }

    @Test(expected = IllegalArgumentException::class)
    fun vortekResolveRequiresSocket() {
        VulkanDriverConfiguration.resolve(
            RuntimeVulkanDriver.SYSTEM_VORTEK,
            VulkanDriverResolveContext(runtimeRoot = runtimeRoot, vortekSocketPath = null),
        )
    }

    @Test
    fun vortekResolveUsesHostGlibcAndVortekIcd() {
        val root = Files.createTempDirectory("vortek-runtime")
        Files.createDirectories(root.resolve("host/lib"))
        Files.createDirectories(root.resolve("host/vulkan/icd.d"))
        Files.writeString(root.resolve("host/libvulkan.so.1"), "loader")
        Files.writeString(root.resolve("host/lib/libvulkan_vortek.so"), "client")
        Files.writeString(
            root.resolve("host/vulkan/icd.d/vortek.json"),
            """{"file_format_version":"1.0.0","ICD":{"library_path":"./libvulkan_vortek.so","api_version":"1.1.128"}}""",
        )
        val socket = root.resolve("vs/s1.sock")
        val configuration = VulkanDriverConfiguration.resolve(
            RuntimeVulkanDriver.SYSTEM_VORTEK,
            VulkanDriverResolveContext(runtimeRoot = root, vortekSocketPath = socket),
        )

        assertEquals(Box64Mode.HOST_GLIBC, configuration.box64Mode)
        assertEquals(root.resolve("host/libvulkan.so.1").toString(), configuration.environment["SDL_VULKAN_LIBRARY"])
        assertEquals(root.resolve("host/vulkan/icd.d/vortek.json").toString(), configuration.environment["VK_ICD_FILENAMES"])
        assertEquals(socket.toString(), configuration.environment["BACHATA_VORTEK_SOCKET"])
        assertEquals("1", configuration.environment["BACHATA_VORTEK_HANDSHAKE"])
        assertFalse(configuration.environment.containsKey("BACHATA_VULKAN_DRIVER_DIR"))
        assertFalse(configuration.environment.containsKey("BACHATA_VULKAN_DRIVER_NAME"))
        assertFalse(configuration.environment.containsKey("BACHATA_VULKAN_TMPDIR"))
        assertTrue(configuration.environment["VK_ICD_FILENAMES"]!!.contains("vortek.json"))
        assertFalse(configuration.environment["VK_ICD_FILENAMES"]!!.contains("turnip"))
        assertEquals(RuntimeVulkanDriverIds.SYSTEM_VORTEK, configuration.driverProfileId)
    }

    @Test(expected = IllegalArgumentException::class)
    fun vortekMissingRuntimeFilesFailBeforeLaunch() {
        val root = Files.createTempDirectory("vortek-missing")
        // No host loader / ICD / client library.
        VulkanDriverConfiguration.resolve(
            RuntimeVulkanDriver.SYSTEM_VORTEK,
            VulkanDriverResolveContext(
                runtimeRoot = root,
                vortekSocketPath = root.resolve("vs/x.sock"),
            ),
        )
    }
}
