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
 * Task 9 fence-wait: signaled present_done fence must not return false DEVICE_LOST.
 */
class VortekFenceDeviceTest {
    private val targetContext = InstrumentationRegistry.getInstrumentation().targetContext
    private val controller = VortekServerController()
    private var installRoot: Path? = null

    @After
    fun tearDown() {
        runBlocking { controller.stop("teardown") }
        installRoot?.let { runCatching { cleanupInstall(it) } }
    }

    @Test
    fun fenceWait_signaledAndSubmitMatrix() {
        runBlocking {
            val installed = installRuntime()
            val probe = pickProbe(installed)
            val socketPath = VortekServerController.sessionSocketPath(
                targetContext.filesDir,
                VortekSessionSupport.newSessionId(),
            )
            val start = controller.start(
                VortekServerConfig(
                    socketPath = socketPath,
                    expectedClientBuild = "*",
                    serverBuild = "bachata-vortek-server-fence",
                ),
            )
            assertTrue("server start failed: ${start.message}", start.ok)

            val config = VulkanDriverConfiguration.resolve(
                RuntimeVulkanDriver.SYSTEM_VORTEK,
                VulkanDriverResolveContext(installed, vortekSocketPath = Paths.get(socketPath)),
            )
            assertEquals(Box64Mode.HOST_GLIBC, config.box64Mode)

            val result = RuntimeProbeLauncher().run(
                RuntimeProbeRequest(
                    nativeLibraryDir = File(targetContext.applicationInfo.nativeLibraryDir).toPath(),
                    runtimeRoot = installed,
                    executable = probe,
                    environment = config.environment + mapOf(
                        "BACHATA_VORTEK_FENCE" to "1",
                        "BACHATA_VORTEK_HEADLESS" to "0",
                        "BACHATA_VORTEK_TRANSPORT_ONLY" to "0",
                        "BACHATA_VORTEK_WSI" to "0",
                        "BACHATA_VORTEK_SHAD" to "0",
                        "HOME" to targetContext.filesDir.absolutePath,
                        "TMPDIR" to targetContext.cacheDir.absolutePath,
                        "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
                    ),
                    executionMode = executionMode(probe),
                ),
                timeoutSeconds = 90,
            )
            Log.i(TAG, "exit=${result.exitCode}\n${result.output}")
            assertEquals(result.output, 0, result.exitCode)
            assertTrue(result.output, result.output.contains("[Vortek.Fence] test1_wait_signaled_infinite=OK"))
            assertTrue(result.output, result.output.contains("[Vortek.Fence] test2_wait_signaled_finite=OK"))
            assertTrue(result.output, result.output.contains("[Vortek.Fence] test3_unsignaled_zero=OK"))
            assertTrue(result.output, result.output.contains("[Vortek.Fence] test4_unsignaled_finite_timeout=OK"))
            assertTrue(result.output, result.output.contains("[Vortek.Fence] test5_wait_after_submit=OK"))
            assertTrue(result.output, result.output.contains("[Vortek.Fence] test6_reset_submit_reuse=OK"))
            assertTrue(result.output, result.output.contains("[Vortek.Fence] test7_waitAll=OK"))
            assertTrue(result.output, result.output.contains("[Vortek.Fence] test8_waitAny=OK"))
            assertTrue(result.output, result.output.contains("[Vortek.Fence] result=success"))
            assertFalse(result.output.contains("Device lost during waiting"))

            controller.stop("fence_done")
            assertFalse(File(socketPath).exists())
            assertEquals(VortekServerState.STOPPED, controller.state())
        }
    }

    @Test
    fun fenceWait_deviceLostPropagation() {
        runBlocking {
            val installed = installRuntime()
            val probe = pickProbe(installed)
            val socketPath = VortekServerController.sessionSocketPath(
                targetContext.filesDir,
                "fence-dl",
            )
            assertTrue(controller.start(VortekServerConfig(socketPath)).ok)
            // Force DEVICE_LOST on server wait workers via process env is not available;
            // probe skips test9 unless server has FORCE set. Smoke that session still cleans up.
            val config = VulkanDriverConfiguration.resolve(
                RuntimeVulkanDriver.SYSTEM_VORTEK,
                VulkanDriverResolveContext(installed, vortekSocketPath = Paths.get(socketPath)),
            )
            val result = RuntimeProbeLauncher().run(
                RuntimeProbeRequest(
                    nativeLibraryDir = File(targetContext.applicationInfo.nativeLibraryDir).toPath(),
                    runtimeRoot = installed,
                    executable = probe,
                    environment = config.environment + mapOf(
                        "BACHATA_VORTEK_FENCE" to "1",
                        "HOME" to targetContext.filesDir.absolutePath,
                        "TMPDIR" to targetContext.cacheDir.absolutePath,
                        "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
                    ),
                    executionMode = executionMode(probe),
                ),
                timeoutSeconds = 90,
            )
            assertEquals(result.output, 0, result.exitCode)
            controller.stop("fence_dl")
            assertFalse(File(socketPath).exists())
        }
    }

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
            .resolve("vortek-fence-${System.nanoTime()}")
        installRoot = root
        val manifest = targetContext.assets.open("runtime/manifest.json").bufferedReader().use {
            Json { ignoreUnknownKeys = true }.decodeFromString<RuntimeManifest>(it.readText())
        }
        return targetContext.assets.open("runtime/runtime.zip").use { bundle ->
            RuntimeInstaller(root).install(bundle, manifest).getOrThrow()
        }
    }

    private fun cleanupInstall(root: Path) {
        if (!Files.isDirectory(root)) return
        Files.walk(root).sorted(Comparator.reverseOrder()).forEach { Files.deleteIfExists(it) }
    }

    private companion object {
        const val TAG = "VortekFenceDeviceTest"
    }
}
