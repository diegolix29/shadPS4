package com.bachatas4.android.runtime.vortek

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.bachatas4.android.runtime.process.Box64Mode
import com.bachatas4.android.runtime.process.RuntimeProbeExecutionMode
import com.bachatas4.android.runtime.process.RuntimeProbeLauncher
import com.bachatas4.android.runtime.process.RuntimeProbeRequest
import com.bachatas4.android.runtime.process.RuntimeVulkanDriver
import com.bachatas4.android.runtime.process.RuntimeVulkanDriverIds
import com.bachatas4.android.runtime.process.VulkanDriverConfiguration
import com.bachatas4.android.runtime.process.VulkanDriverResolveContext
import java.io.File
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Task 5 on-device gate (runtime module). Prefers a pre-installed managed runtime under
 * filesDir/runtime/. Full install-from-assets path lives in app VortekTransportDeviceTest.
 */
@RunWith(AndroidJUnit4::class)
class VortekTransportInstrumentedTest {
    private val context = InstrumentationRegistry.getInstrumentation().targetContext
    private val controller = VortekServerController()

    @After
    fun tearDown() = runBlocking {
        controller.stop("teardown")
        Unit
    }

    @Test
    fun transportProbe_hostGlibc_handshakeAndContextReady() = runBlocking {
        val runtimeRoot = findRuntimeRoot()
        assumeTrue("managed runtime with Vortek assets not installed", runtimeRoot != null)
        val root = runtimeRoot!!

        val probeX64 = root.resolve("bin/probes/vortek_probe_x86_64")
        val probeA64 = root.resolve("bin/probes/vortek_probe_aarch64")
        val useA64 = Files.isRegularFile(probeA64)
        val useX64 = Files.isRegularFile(probeX64)
        assumeTrue("vortek probe binaries missing under runtime", useA64 || useX64)

        val shortId = VortekSessionSupport.newSessionId()
        val socketPath = VortekServerController.sessionSocketPath(context.filesDir, shortId)
        val start = controller.start(
            VortekServerConfig(
                socketPath = socketPath,
                expectedClientBuild = "*",
                serverBuild = "bachata-vortek-server-t5",
            ),
        )
        assertTrue(start.ok, "server start failed: ${start.message}")
        assertTrue(File(socketPath).exists(), "socket must exist after start")

        val config = VulkanDriverConfiguration.resolve(
            RuntimeVulkanDriver.SYSTEM_VORTEK,
            VulkanDriverResolveContext(
                runtimeRoot = root,
                vortekSocketPath = Paths.get(socketPath),
            ),
        )
        assertEquals(Box64Mode.HOST_GLIBC, config.box64Mode)
        assertEquals(RuntimeVulkanDriverIds.SYSTEM_VORTEK, config.driverProfileId)
        assertTrue(config.environment["VK_ICD_FILENAMES"]!!.contains("vortek.json"))
        assertFalse(config.environment.containsKey("BACHATA_VULKAN_DRIVER_DIR"))

        val nativeLibraryDir = Paths.get(context.applicationInfo.nativeLibraryDir)
        val env = config.environment + mapOf(
            "BACHATA_VORTEK_TRANSPORT_ONLY" to "1",
            "BACHATA_VORTEK_LOG_LEVEL" to "1",
            "HOME" to context.filesDir.absolutePath,
            "TMPDIR" to context.cacheDir.absolutePath,
            "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
        )

        val result = if (useA64) {
            RuntimeProbeLauncher().run(
                RuntimeProbeRequest(
                    nativeLibraryDir = nativeLibraryDir,
                    runtimeRoot = root,
                    executable = probeA64,
                    environment = env,
                    executionMode = RuntimeProbeExecutionMode.HOST_GLIBC_NATIVE,
                ),
                timeoutSeconds = 20L,
            )
        } else {
            RuntimeProbeLauncher().run(
                RuntimeProbeRequest(
                    nativeLibraryDir = nativeLibraryDir,
                    runtimeRoot = root,
                    executable = probeX64,
                    environment = env,
                    executionMode = RuntimeProbeExecutionMode.BOX64_HOST_GLIBC,
                ),
                timeoutSeconds = 20L,
            )
        }

        android.util.Log.i(TAG, "probe_exit=${result.exitCode}\n${result.output}")
        assertTrue(
            result.output.contains("backend=SYSTEM_VORTEK") || result.output.contains("context_ready"),
            "probe output missing transport markers:\n${result.output}",
        )
        assertTrue(
            result.output.contains("result=context_ready") ||
                result.output.contains("stage=transport_ready"),
            "probe did not report context_ready:\n${result.output}",
        )
        assertEquals(0, result.exitCode)

        controller.waitContextReady(5_000)
        val stop = controller.stop("probe_done")
        assertTrue(stop.ok, stop.message)
        assertFalse(File(socketPath).exists(), "socket must be removed after stop")
    }

    @Test
    fun sequentialTransportSessions_useDistinctSockets() = runBlocking {
        val runtimeRoot = findRuntimeRoot()
        assumeTrue("managed runtime missing", runtimeRoot != null)
        val root = runtimeRoot!!
        val probe = sequenceOf(
            root.resolve("bin/probes/vortek_probe_aarch64"),
            root.resolve("bin/probes/vortek_probe_x86_64"),
        ).firstOrNull { Files.isRegularFile(it) }
        assumeTrue("probe missing", probe != null)

        val paths = mutableListOf<String>()
        repeat(2) { index ->
            val id = "t5s$index"
            val socketPath = VortekServerController.sessionSocketPath(context.filesDir, id)
            paths += socketPath
            val start = controller.start(VortekServerConfig(socketPath = socketPath))
            assertTrue(start.ok, start.message)
            val config = VulkanDriverConfiguration.resolve(
                RuntimeVulkanDriver.SYSTEM_VORTEK,
                VulkanDriverResolveContext(root, vortekSocketPath = Paths.get(socketPath)),
            )
            val mode = if (probe!!.fileName.toString().contains("aarch64")) {
                RuntimeProbeExecutionMode.HOST_GLIBC_NATIVE
            } else {
                RuntimeProbeExecutionMode.BOX64_HOST_GLIBC
            }
            val result = RuntimeProbeLauncher().run(
                RuntimeProbeRequest(
                    nativeLibraryDir = Paths.get(context.applicationInfo.nativeLibraryDir),
                    runtimeRoot = root,
                    executable = probe,
                    environment = config.environment + mapOf(
                        "BACHATA_VORTEK_TRANSPORT_ONLY" to "1",
                        "HOME" to context.filesDir.absolutePath,
                        "TMPDIR" to context.cacheDir.absolutePath,
                        "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
                    ),
                    executionMode = mode,
                ),
                timeoutSeconds = 20L,
            )
            assertEquals(0, result.exitCode, result.output)
            controller.stop("seq_$index")
            assertFalse(File(socketPath).exists())
        }
        assertEquals(2, paths.distinct().size)
    }

    @Test
    fun guestLaunchFailure_stopsServerAndRemovesSocket() = runBlocking {
        val path = VortekServerController.sessionSocketPath(context.filesDir, "t5fail")
        val start = controller.start(VortekServerConfig(path))
        assertTrue(start.ok, start.message)
        assertTrue(File(path).exists())
        val stop = controller.stop("vortek_guest_launch_failed")
        assertTrue(stop.ok, stop.message)
        assertFalse(File(path).exists())
        assertEquals(VortekServerState.STOPPED, controller.state())
    }

    @Test
    fun clientTimeout_stopsServerAndRemovesSocket() = runBlocking {
        val path = VortekServerController.sessionSocketPath(context.filesDir, "t5to")
        val start = controller.start(VortekServerConfig(path))
        assertTrue(start.ok, start.message)
        val ctx = controller.waitContextReady(1_500)
        assertFalse(ctx.isOk, "expected context timeout without client")
        val stop = controller.stop("vortek_client_timeout")
        assertTrue(stop.ok, stop.message)
        assertFalse(File(path).exists())
    }

    @Test
    fun nonVortekPath_neverStartsServer() {
        val before = controller.state()
        assertEquals(VortekServerState.STOPPED, before)
        val vs = File(context.filesDir, "vs")
        val snapshot = vs.list()?.toSet().orEmpty()
        val runtimeRoot = findRuntimeRoot()
        if (runtimeRoot != null) {
            val turnip = VulkanDriverConfiguration.resolve(
                RuntimeVulkanDriver.TURNIP_26_1_0,
                runtimeRoot,
            )
            assertFalse(turnip.environment.containsKey("BACHATA_VORTEK_SOCKET"))
            assertEquals(Box64Mode.HOST_GLIBC, turnip.box64Mode)
        }
        assertEquals(VortekServerState.STOPPED, controller.state())
        val after = vs.list()?.toSet().orEmpty()
        assertEquals(snapshot, after)
    }

    private fun findRuntimeRoot(): Path? {
        val base = File(context.filesDir, "runtime")
        if (!base.isDirectory) return null
        return base.listFiles()
            ?.filter { it.isDirectory }
            ?.map { it.toPath() }
            ?.firstOrNull { root ->
                Files.isRegularFile(root.resolve("host/lib/libvulkan_vortek.so")) &&
                    Files.isRegularFile(root.resolve("host/vulkan/icd.d/vortek.json")) &&
                    Files.isRegularFile(root.resolve("host/libvulkan.so.1"))
            }
    }

    companion object {
        private const val TAG = "Bachata.Vortek.T5"
    }
}
