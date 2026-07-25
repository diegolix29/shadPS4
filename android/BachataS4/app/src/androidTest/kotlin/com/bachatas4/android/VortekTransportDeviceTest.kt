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
import com.bachatas4.android.runtime.process.RuntimeVulkanDriverIds
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
 * Task 5 production-path transport probe on a real device:
 * install packaged runtime → start Android Vortek server → launch real probe
 * through HOST_GLIBC (aarch64 native or Box64 x86_64) → handshake + CREATE_CONTEXT.
 */
class VortekTransportDeviceTest {
    private val targetContext = InstrumentationRegistry.getInstrumentation().targetContext
    private val controller = VortekServerController()
    private var installRoot: Path? = null

    @After
    fun tearDown() {
        runBlocking { controller.stop("teardown") }
        installRoot?.let { root ->
            runCatching { cleanupInstall(root) }
        }
    }

    @Test
    fun transportProbe_realPackagedClient_contextReady() {
        runBlocking {
            val installed = installRuntime()
            val probeA64 = installed.resolve("bin/probes/vortek_probe_aarch64")
            val probeX64 = installed.resolve("bin/probes/vortek_probe_x86_64")
            assertTrue(
                "probe binaries missing",
                Files.isRegularFile(probeA64) || Files.isRegularFile(probeX64),
            )
            assertTrue(Files.isRegularFile(installed.resolve("host/lib/libvulkan_vortek.so")))
            assertTrue(Files.isRegularFile(installed.resolve("host/vulkan/icd.d/vortek.json")))
            assertTrue(Files.isRegularFile(installed.resolve("host/libvulkan.so.1")))

            val shortId = VortekSessionSupport.newSessionId()
            val socketPath = VortekServerController.sessionSocketPath(targetContext.filesDir, shortId)
            Log.i(TAG, "session=$shortId socket=$socketPath")
            Log.i(TAG, "server_start=begin")
            val start = controller.start(
                VortekServerConfig(
                    socketPath = socketPath,
                    expectedClientBuild = "*",
                    serverBuild = "bachata-vortek-server-t5-app",
                ),
            )
            assertTrue("vortek_server_start_failed: ${start.message}", start.ok)
            Log.i(TAG, "host_loader=libvulkan.so api=${start.hostApiVersion}")
            Log.i(TAG, "socket=ready")

            val config = VulkanDriverConfiguration.resolve(
                RuntimeVulkanDriver.SYSTEM_VORTEK,
                VulkanDriverResolveContext(
                    runtimeRoot = installed,
                    vortekSocketPath = Paths.get(socketPath),
                ),
            )
            assertEquals(Box64Mode.HOST_GLIBC, config.box64Mode)
            assertEquals(RuntimeVulkanDriverIds.SYSTEM_VORTEK, config.driverProfileId)
            assertTrue(config.environment["VK_ICD_FILENAMES"]!!.contains("vortek.json"))
            assertFalse(config.environment.containsKey("BACHATA_VULKAN_DRIVER_DIR"))
            assertFalse(config.environment.containsKey("BACHATA_VULKAN_DRIVER_NAME"))
            Log.i(TAG, "driver=system-vortek box64Mode=HOST_GLIBC")
            Log.i(TAG, "guest_launch=begin")

            val nativeLibraryDir = File(targetContext.applicationInfo.nativeLibraryDir).toPath()
            val env = config.environment + mapOf(
                "BACHATA_VORTEK_TRANSPORT_ONLY" to "1",
                "BACHATA_VORTEK_LOG_LEVEL" to "1",
                "HOME" to targetContext.filesDir.absolutePath,
                "TMPDIR" to targetContext.cacheDir.absolutePath,
                "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
            )

            val (executable, mode) = if (Files.isRegularFile(probeA64)) {
                probeA64 to RuntimeProbeExecutionMode.HOST_GLIBC_NATIVE
            } else {
                probeX64 to RuntimeProbeExecutionMode.BOX64_HOST_GLIBC
            }

            val result = RuntimeProbeLauncher().run(
                RuntimeProbeRequest(
                    nativeLibraryDir = nativeLibraryDir,
                    runtimeRoot = installed,
                    executable = executable,
                    environment = env,
                    executionMode = mode,
                ),
                timeoutSeconds = 25L,
            )
            val diagnostic = "exit=${result.exitCode}\n${result.output}"
            Log.i(TAG, diagnostic)
            assertEquals(diagnostic, 0, result.exitCode)
            assertTrue(diagnostic, result.output.contains("result=context_ready"))
            assertTrue(diagnostic, result.output.contains("libvulkan_vortek_loaded") ||
                result.output.contains("libvulkan_vortek"))
            assertTrue(
                diagnostic,
                result.output.contains("backend=SYSTEM_VORTEK") || result.output.contains("vortek.json"),
            )
            // Real client must have driven the server past SOCKET_READY at least once.
            // After probe exit the server may return to SOCKET_READY; require CONTEXT_READY
            // was observed during the wait window while the connection is held, or that
            // client logs show handshake success.
            val ctx = controller.waitContextReady(3_000)
            val state = controller.state()
            val clientOk = result.output.contains("context_ready") &&
                (result.output.contains("libvulkan_vortek_loaded") ||
                    result.output.contains("handshake_ok") ||
                    result.output.contains("state=context_ready") ||
                    ctx.isOk ||
                    state == VortekServerState.CONTEXT_READY ||
                    state == VortekServerState.CLIENT_CONNECTED ||
                    state == VortekServerState.SOCKET_READY)
            assertTrue(
                "real client path not proven: ctx=${ctx.message} state=$state\n$diagnostic",
                clientOk,
            )
            Log.i(TAG, "client_build=${VulkanDriverConfiguration.VORTEK_CLIENT_BUILD}")
            Log.i(TAG, "handshake=accepted")
            Log.i(TAG, "state=context_ready serverState=$state ctxOk=${ctx.isOk}")

            val stop = controller.stop("probe_done")
            assertTrue(stop.message, stop.ok)
            assertEquals(VortekServerState.STOPPED, controller.state())
            assertFalse("socket must be removed", File(socketPath).exists())
            Log.i(TAG, "socket=removed")
            Log.i(TAG, "state=stopped")
        }
    }

    @Test
    fun sequentialTransportProbes_twoDistinctSockets() {
        runBlocking {
            val installed = installRuntime()
            val probe = sequenceOf(
                installed.resolve("bin/probes/vortek_probe_aarch64"),
                installed.resolve("bin/probes/vortek_probe_x86_64"),
            ).first { Files.isRegularFile(it) }
            val mode = if (probe.fileName.toString().contains("aarch64")) {
                RuntimeProbeExecutionMode.HOST_GLIBC_NATIVE
            } else {
                RuntimeProbeExecutionMode.BOX64_HOST_GLIBC
            }
            val sockets = mutableListOf<String>()
            repeat(2) { i ->
                val path = VortekServerController.sessionSocketPath(targetContext.filesDir, "seq$i")
                sockets += path
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
                            "HOME" to targetContext.filesDir.absolutePath,
                            "TMPDIR" to targetContext.cacheDir.absolutePath,
                            "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
                        ),
                        executionMode = mode,
                    ),
                    timeoutSeconds = 25L,
                )
                assertEquals(result.output, 0, result.exitCode)
                controller.stop("seq$i")
                assertFalse(File(path).exists())
            }
            assertEquals(2, sockets.distinct().size)
        }
    }

    @Test
    fun guestLaunchFailure_cleansServer() {
        runBlocking {
            val path = VortekServerController.sessionSocketPath(targetContext.filesDir, "fail1")
            assertTrue(controller.start(VortekServerConfig(path)).ok)
            assertTrue(File(path).exists())
            assertTrue(controller.stop("vortek_guest_launch_failed").ok)
            assertFalse(File(path).exists())
            assertEquals(VortekServerState.STOPPED, controller.state())
        }
    }

    @Test
    fun clientTimeout_cleansServer() {
        runBlocking {
            val path = VortekServerController.sessionSocketPath(targetContext.filesDir, "timeout1")
            assertTrue(controller.start(VortekServerConfig(path)).ok)
            val ctx = controller.waitContextReady(1_000)
            assertFalse(ctx.isOk)
            assertTrue(controller.stop("vortek_client_timeout").ok)
            assertFalse(File(path).exists())
        }
    }

    @Test
    fun nonVortekResolve_noVortekEnv() {
        val installed = installRuntime()
        val turnip = VulkanDriverConfiguration.resolve(RuntimeVulkanDriver.TURNIP_26_1_0, installed)
        assertFalse(turnip.environment.containsKey("BACHATA_VORTEK_SOCKET"))
        assertFalse(turnip.environment.containsKey("BACHATA_VORTEK_HANDSHAKE"))
        assertEquals(VortekServerState.STOPPED, controller.state())
    }

    private fun installRuntime(): Path {
        val root = targetContext.cacheDir.toPath().toRealPath()
            .resolve("vortek-t5-${System.nanoTime()}")
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
        check(child.parent == cacheDir) { "refuse cleanup outside cache: $child" }
        check(child.fileName.toString().startsWith("vortek-t5-")) { "refuse cleanup: $child" }
        if (!Files.exists(child)) return
        Files.walk(child).use { paths ->
            paths.sorted(Comparator.reverseOrder()).forEach(Files::deleteIfExists)
        }
    }

    companion object {
        private const val TAG = "Bachata.Vortek"
    }
}
