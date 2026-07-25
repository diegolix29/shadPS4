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
 * Task 6: headless non-WSI Vulkan through real packaged Vortek client + Android server.
 */
class VortekHeadlessDeviceTest {
    private val targetContext = InstrumentationRegistry.getInstrumentation().targetContext
    private val controller = VortekServerController()
    private var installRoot: Path? = null

    @After
    fun tearDown() {
        runBlocking { controller.stop("teardown") }
        installRoot?.let { runCatching { cleanupInstall(it) } }
    }

    @Test
    fun headlessProbe_instanceDeviceSubmitCleanup() {
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
                    serverBuild = "bachata-vortek-server-t6",
                ),
            )
            assertTrue("server start failed: ${start.message}", start.ok)

            val config = VulkanDriverConfiguration.resolve(
                RuntimeVulkanDriver.SYSTEM_VORTEK,
                VulkanDriverResolveContext(installed, vortekSocketPath = Paths.get(socketPath)),
            )
            assertEquals(Box64Mode.HOST_GLIBC, config.box64Mode)

            val result = runProbe(installed, probe, config, headless = true)
            Log.i(TAG, "exit=${result.exitCode}\n${result.output}")
            assertEquals(result.output, 0, result.exitCode)
            assertTrue(result.output, result.output.contains("libvulkan_vortek_loaded"))
            assertTrue(result.output, result.output.contains("stage=instance_created"))
            assertTrue(result.output, result.output.contains("stage=physical_device name="))
            assertTrue(result.output, result.output.contains("stage=device_created"))
            assertTrue(result.output, result.output.contains("stage=queue_retrieved"))
            assertTrue(result.output, result.output.contains("stage=command_buffer_recorded"))
            assertTrue(result.output, result.output.contains("queue_submit result=VK_SUCCESS") ||
                result.output.contains("stage=queue_submit result=0"))
            assertTrue(result.output, result.output.contains("stage=device_idle"))
            assertTrue(result.output, result.output.contains("stage=cleanup_complete"))
            assertTrue(result.output, result.output.contains("result=success"))
            assertFalse(result.output.contains("turnip"))

            controller.stop("probe_done")
            assertFalse(File(socketPath).exists())
            assertEquals(VortekServerState.STOPPED, controller.state())
        }
    }

    @Test
    fun sequentialHeadlessProbes_twoDistinctSockets() {
        runBlocking {
            val installed = installRuntime()
            val probe = pickProbe(installed)
            val sockets = mutableListOf<String>()
            repeat(2) { i ->
                val path = VortekServerController.sessionSocketPath(targetContext.filesDir, "hseq$i")
                sockets += path
                assertTrue(controller.start(VortekServerConfig(path)).ok)
                val config = VulkanDriverConfiguration.resolve(
                    RuntimeVulkanDriver.SYSTEM_VORTEK,
                    VulkanDriverResolveContext(installed, vortekSocketPath = Paths.get(path)),
                )
                val result = runProbe(installed, probe, config, headless = true)
                assertEquals(result.output, 0, result.exitCode)
                assertTrue(result.output.contains("result=success"))
                controller.stop("hseq$i")
                assertFalse(File(path).exists())
            }
            assertEquals(2, sockets.distinct().size)
        }
    }

    @Test
    fun transportOnlyStillWorks_regression() {
        runBlocking {
            val installed = installRuntime()
            val probe = pickProbe(installed)
            val path = VortekServerController.sessionSocketPath(targetContext.filesDir, "t5reg")
            assertTrue(controller.start(VortekServerConfig(path)).ok)
            val config = VulkanDriverConfiguration.resolve(
                RuntimeVulkanDriver.SYSTEM_VORTEK,
                VulkanDriverResolveContext(installed, vortekSocketPath = Paths.get(path)),
            )
            val result = runProbe(installed, probe, config, headless = false, transportOnly = true)
            assertEquals(result.output, 0, result.exitCode)
            assertTrue(result.output.contains("result=context_ready"))
            controller.stop("t5reg")
            assertFalse(File(path).exists())
        }
    }

    @Test
    fun nonVortek_noServer() {
        val installed = installRuntime()
        val turnip = VulkanDriverConfiguration.resolve(RuntimeVulkanDriver.TURNIP_26_1_0, installed)
        assertFalse(turnip.environment.containsKey("BACHATA_VORTEK_SOCKET"))
        assertEquals(VortekServerState.STOPPED, controller.state())
    }

    private fun runProbe(
        installed: Path,
        probe: Path,
        config: VulkanDriverConfiguration,
        headless: Boolean,
        transportOnly: Boolean = false,
    ) = RuntimeProbeLauncher().run(
        RuntimeProbeRequest(
            nativeLibraryDir = File(targetContext.applicationInfo.nativeLibraryDir).toPath(),
            runtimeRoot = installed,
            executable = probe,
            environment = config.environment + mapOf(
                "BACHATA_VORTEK_HEADLESS" to if (headless) "1" else "0",
                "BACHATA_VORTEK_TRANSPORT_ONLY" to if (transportOnly) "1" else "0",
                "BACHATA_VORTEK_LOG_LEVEL" to "1",
                "HOME" to targetContext.filesDir.absolutePath,
                "TMPDIR" to targetContext.cacheDir.absolutePath,
                "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
            ),
            executionMode = if (probe.fileName.toString().contains("aarch64")) {
                RuntimeProbeExecutionMode.HOST_GLIBC_NATIVE
            } else {
                RuntimeProbeExecutionMode.BOX64_HOST_GLIBC
            },
        ),
        timeoutSeconds = 45L,
    )

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
            .resolve("vortek-t6-${System.nanoTime()}")
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
        check(child.fileName.toString().startsWith("vortek-t6-"))
        if (!Files.exists(child)) return
        Files.walk(child).use { paths ->
            paths.sorted(Comparator.reverseOrder()).forEach(Files::deleteIfExists)
        }
    }

    companion object {
        private const val TAG = "Bachata.Vortek.T6"
    }
}
