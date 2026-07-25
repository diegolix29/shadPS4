package com.bachatas4.android

import android.util.Log
import androidx.test.platform.app.InstrumentationRegistry
import com.bachatas4.android.runtime.install.RuntimeInstaller
import com.bachatas4.android.runtime.install.RuntimeManifest
import com.bachatas4.android.runtime.process.Box64Mode
import com.bachatas4.android.runtime.process.RuntimeProbeExecutionMode
import com.bachatas4.android.runtime.process.RuntimeProbeLauncher
import com.bachatas4.android.runtime.process.RuntimeProbeRequest
import com.bachatas4.android.runtime.process.RuntimeVulkanDriver
import com.bachatas4.android.runtime.process.VulkanDriverConfiguration
import com.bachatas4.android.runtime.process.VulkanDriverResolveContext
import com.bachatas4.android.runtime.vortek.VortekServerConfig
import com.bachatas4.android.runtime.vortek.VortekServerController
import com.bachatas4.android.runtime.vortek.VortekServerState
import com.bachatas4.android.runtime.vortek.VortekSessionSupport
import java.io.File
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths
import java.util.Comparator
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Task 8: shadPS4-matching Vulkan 1.3 capability probe through real Vortek path.
 */
class VortekShadProbeDeviceTest {
    private val targetContext = InstrumentationRegistry.getInstrumentation().targetContext
    private val controller = VortekServerController()
    private var installRoot: Path? = null

    @After
    fun tearDown() {
        runBlocking { controller.stop("teardown") }
        installRoot?.let { runCatching { cleanupInstall(it) } }
    }

    @Test
    fun shadProbe_instanceDeviceFeaturesTimelineDynamicRendering() {
        runBlocking {
            val installed = installRuntime()
            val probe = pickProbe(installed)
            val socketPath = VortekServerController.sessionSocketPath(
                targetContext.filesDir,
                VortekSessionSupport.newSessionId(),
            )
            Log.i(TAG, "server_start=begin socket=$socketPath")
            val start = controller.start(
                VortekServerConfig(
                    socketPath = socketPath,
                    expectedClientBuild = "*",
                    serverBuild = "bachata-vortek-server-t8",
                ),
            )
            assertTrue("server start failed: ${start.message}", start.ok)

            val config = VulkanDriverConfiguration.resolve(
                RuntimeVulkanDriver.SYSTEM_VORTEK,
                VulkanDriverResolveContext(installed, vortekSocketPath = Paths.get(socketPath)),
            )
            assertEquals(Box64Mode.HOST_GLIBC, config.box64Mode)

            val result = runShadProbe(installed, probe, config)
            Log.i(TAG, "exit=${result.exitCode}\n${result.output}")
            assertEquals(result.output, 0, result.exitCode)
            assertTrue(result.output, result.output.contains("libvulkan_vortek_loaded"))
            assertTrue(result.output, result.output.contains("[Vortek.ShadProbe] required_api=1.3"))
            assertTrue(result.output, result.output.contains("stage=instance_version"))
            assertTrue(result.output, result.output.contains("stage=entry_points_ok") ||
                result.output.contains("stage=instance_created"))
            assertTrue(result.output, result.output.contains("stage=physical_device name="))
            assertTrue(result.output, result.output.contains("stage=features2"))
            assertTrue(result.output, result.output.contains("stage=properties2") ||
                result.output.contains("maxPushDescriptors="))
            assertTrue(result.output, result.output.contains("extension_ok name=VK_KHR_swapchain"))
            assertTrue(result.output, result.output.contains("extension_ok name=VK_KHR_push_descriptor"))
            assertTrue(
                result.output,
                result.output.contains("extension_ok name=VK_EXT_vertex_attribute_divisor"),
            )
            assertTrue(result.output, result.output.contains("stage=device_created result=0"))
            assertTrue(result.output, result.output.contains("stage=queue_retrieved"))
            assertTrue(
                result.output,
                result.output.contains("stage=timeline_ok") ||
                    result.output.contains("stage=timeline_skipped feature=false"),
            )
            assertTrue(
                result.output,
                result.output.contains("stage=dynamic_rendering_ok") ||
                    result.output.contains("stage=dynamic_rendering_skipped"),
            )
            assertTrue(result.output, result.output.contains("[Vortek.ShadProbe] result=success"))
            assertTrue(result.output, result.output.contains("first_missing=none"))
            assertFalse(result.output.contains("turnip"))

            controller.stop("probe_done")
            assertFalse(File(socketPath).exists())
            assertEquals(VortekServerState.STOPPED, controller.state())
        }
    }

    @Test
    fun sequentialShadProbes_twoSessions() {
        runBlocking {
            val installed = installRuntime()
            val probe = pickProbe(installed)
            repeat(2) { i ->
                val path = VortekServerController.sessionSocketPath(targetContext.filesDir, "s8seq$i")
                assertTrue(controller.start(VortekServerConfig(path)).ok)
                val config = VulkanDriverConfiguration.resolve(
                    RuntimeVulkanDriver.SYSTEM_VORTEK,
                    VulkanDriverResolveContext(installed, vortekSocketPath = Paths.get(path)),
                )
                val result = runShadProbe(installed, probe, config)
                Log.i(TAG, "seq$i exit=${result.exitCode}")
                assertEquals(result.output, 0, result.exitCode)
                assertTrue(result.output.contains("[Vortek.ShadProbe] result=success"))
                controller.stop("s8seq$i")
                assertFalse(File(path).exists())
            }
        }
    }

    @Test
    fun headlessRegression_stillPasses() {
        runBlocking {
            val installed = installRuntime()
            val probe = pickProbe(installed)
            val path = VortekServerController.sessionSocketPath(targetContext.filesDir, "s8hl")
            assertTrue(controller.start(VortekServerConfig(path)).ok)
            val config = VulkanDriverConfiguration.resolve(
                RuntimeVulkanDriver.SYSTEM_VORTEK,
                VulkanDriverResolveContext(installed, vortekSocketPath = Paths.get(path)),
            )
            val result = RuntimeProbeLauncher().run(
                RuntimeProbeRequest(
                    nativeLibraryDir = File(targetContext.applicationInfo.nativeLibraryDir).toPath(),
                    runtimeRoot = installed,
                    executable = probe,
                    environment = config.environment + mapOf(
                        "BACHATA_VORTEK_HEADLESS" to "1",
                        "BACHATA_VORTEK_TRANSPORT_ONLY" to "0",
                        "BACHATA_VORTEK_SHAD" to "0",
                        "BACHATA_VORTEK_LOG_LEVEL" to "1",
                        "HOME" to targetContext.filesDir.absolutePath,
                        "TMPDIR" to targetContext.cacheDir.absolutePath,
                        "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
                    ),
                    executionMode = executionMode(probe),
                ),
                timeoutSeconds = 45L,
            )
            assertEquals(result.output, 0, result.exitCode)
            assertTrue(result.output.contains("result=success"))
            controller.stop("s8hl")
        }
    }

    @Test
    fun transportRegression_stillPasses() {
        runBlocking {
            val installed = installRuntime()
            val probe = pickProbe(installed)
            val path = VortekServerController.sessionSocketPath(targetContext.filesDir, "s8t5")
            assertTrue(controller.start(VortekServerConfig(path)).ok)
            val config = VulkanDriverConfiguration.resolve(
                RuntimeVulkanDriver.SYSTEM_VORTEK,
                VulkanDriverResolveContext(installed, vortekSocketPath = Paths.get(path)),
            )
            val result = RuntimeProbeLauncher().run(
                RuntimeProbeRequest(
                    nativeLibraryDir = File(targetContext.applicationInfo.nativeLibraryDir).toPath(),
                    runtimeRoot = installed,
                    executable = probe,
                    environment = config.environment + mapOf(
                        "BACHATA_VORTEK_TRANSPORT_ONLY" to "1",
                        "BACHATA_VORTEK_HEADLESS" to "0",
                        "BACHATA_VORTEK_SHAD" to "0",
                        "BACHATA_VORTEK_LOG_LEVEL" to "1",
                        "HOME" to targetContext.filesDir.absolutePath,
                        "TMPDIR" to targetContext.cacheDir.absolutePath,
                        "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
                    ),
                    executionMode = executionMode(probe),
                ),
                timeoutSeconds = 30L,
            )
            assertEquals(result.output, 0, result.exitCode)
            assertTrue(result.output.contains("result=context_ready"))
            controller.stop("s8t5")
        }
    }

    @Test
    fun nonVortek_noServer() {
        val installed = installRuntime()
        val turnip = VulkanDriverConfiguration.resolve(RuntimeVulkanDriver.TURNIP_26_1_0, installed)
        assertFalse(turnip.environment.containsKey("BACHATA_VORTEK_SOCKET"))
        assertEquals(VortekServerState.STOPPED, controller.state())
    }

    private fun runShadProbe(
        installed: Path,
        probe: Path,
        config: VulkanDriverConfiguration,
    ) = RuntimeProbeLauncher().run(
        RuntimeProbeRequest(
            nativeLibraryDir = File(targetContext.applicationInfo.nativeLibraryDir).toPath(),
            runtimeRoot = installed,
            executable = probe,
            environment = config.environment + mapOf(
                "BACHATA_VORTEK_SHAD" to "1",
                "BACHATA_VORTEK_HEADLESS" to "0",
                "BACHATA_VORTEK_TRANSPORT_ONLY" to "0",
                "BACHATA_VORTEK_WSI" to "0",
                "BACHATA_VORTEK_LOG_LEVEL" to "1",
                "HOME" to targetContext.filesDir.absolutePath,
                "TMPDIR" to targetContext.cacheDir.absolutePath,
                "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
            ),
            executionMode = executionMode(probe),
        ),
        timeoutSeconds = 90L,
    )

    private fun executionMode(probe: Path) =
        if (probe.fileName.toString().contains("aarch64")) {
            RuntimeProbeExecutionMode.HOST_GLIBC_NATIVE
        } else {
            RuntimeProbeExecutionMode.BOX64_HOST_GLIBC
        }

    private fun pickProbe(installed: Path): Path {
        val a64 = installed.resolve("bin/probes/vortek_probe_aarch64")
        val x64 = installed.resolve("bin/probes/vortek_probe_x86_64")
        return when {
            Files.isRegularFile(a64) -> a64
            Files.isRegularFile(x64) -> x64
            else -> error("probe binaries missing")
        }
    }

    private fun installRuntime(): Path {
        val root = targetContext.cacheDir.toPath().toRealPath()
            .resolve("vortek-t8-${System.nanoTime()}")
        installRoot = root
        val manifest = targetContext.assets.open("runtime/manifest.json").bufferedReader().use {
            Json { ignoreUnknownKeys = true }.decodeFromString<RuntimeManifest>(it.readText())
        }
        return targetContext.assets.open("runtime/runtime.zip").use { bundle ->
            RuntimeInstaller(root).install(bundle, manifest).getOrThrow()
        }
    }

    private fun cleanupInstall(child: Path) {
        val cacheDir = targetContext.cacheDir.toPath().toRealPath()
        check(child.parent == cacheDir)
        check(child.fileName.toString().startsWith("vortek-t8-"))
        if (!Files.exists(child)) return
        Files.walk(child).use { paths ->
            paths.sorted(Comparator.reverseOrder()).forEach(Files::deleteIfExists)
        }
    }

    companion object {
        private const val TAG = "Bachata.Vortek.T8"
    }
}
