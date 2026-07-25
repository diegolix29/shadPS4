package com.bachatas4.android

import android.graphics.PixelFormat
import android.media.ImageReader
import android.util.Log
import androidx.test.platform.app.InstrumentationRegistry
import com.bachatas4.android.runtime.display.WinlatorEmbeddedXServer
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
import com.bachatas4.android.runtime.vortek.VortekWindowBridge
import com.winlator.xconnector.UnixSocketConfig
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
 * Task 7: Xlib WSI + swapchain present through real packaged Vortek + Android surface path.
 */
class VortekWsiDeviceTest {
    private val targetContext = InstrumentationRegistry.getInstrumentation().targetContext
    private val controller = VortekServerController()
    private var installRoot: Path? = null
    private var xServer: WinlatorEmbeddedXServer? = null
    private var imageReader: ImageReader? = null

    @After
    fun tearDown() {
        runBlocking {
            controller.setWindowBridge(null)
            controller.stop("teardown")
            xServer?.stop()
            xServer = null
            imageReader?.close()
            imageReader = null
        }
        installRoot?.let { runCatching { cleanupInstall(it) } }
    }

    @Test
    fun wsiPresentLoop_visibleFrames() {
        runBlocking {
            val installed = installRuntime()
            val probe = pickProbe(installed)
            startDisplay()
            val socketPath = VortekServerController.sessionSocketPath(
                targetContext.filesDir,
                VortekSessionSupport.newSessionId(),
            )
            assertTrue(controller.start(VortekServerConfig(socketPath)).ok)
            attachWindowBridge()

            val config = VulkanDriverConfiguration.resolve(
                RuntimeVulkanDriver.SYSTEM_VORTEK,
                VulkanDriverResolveContext(installed, vortekSocketPath = Paths.get(socketPath)),
            )
            assertEquals(Box64Mode.HOST_GLIBC, config.box64Mode)

            val result = runWsiProbe(installed, probe, config, seconds = 60)
            Log.i(TAG, "exit=${result.exitCode}")
            result.output.lineSequence()
                .filter { line ->
                    line.contains("[Vortek.") || line.contains("Adreno") || line.contains("stats")
                }
                .forEach { Log.i(TAG, it) }
            assertEquals(result.output, 0, result.exitCode)
            assertTrue(result.output, result.output.contains("libvulkan_vortek_loaded"))
            assertTrue(result.output, result.output.contains("stage=surface_created"))
            assertTrue(result.output, result.output.contains("stage=swapchain_created"))
            assertTrue(result.output, result.output.contains("stage=frame_acquired"))
            assertTrue(result.output, result.output.contains("stage=frame_presented"))
            assertTrue(result.output, result.output.contains("[Vortek.WSI] result=success"))
            assertTrue(result.output, result.output.contains("stage=cleanup_complete"))
            assertFalse(result.output.contains("turnip"))

            controller.stop("wsi_done")
            assertFalse(File(socketPath).exists())
            assertEquals(VortekServerState.STOPPED, controller.state())
        }
    }

    @Test
    fun sequentialWsiSessions_twoDistinctSockets() {
        runBlocking {
            val installed = installRuntime()
            val probe = pickProbe(installed)
            startDisplay()
            val sockets = mutableListOf<String>()
            repeat(2) { i ->
                val path = VortekServerController.sessionSocketPath(targetContext.filesDir, "wseq$i")
                sockets += path
                assertTrue(controller.start(VortekServerConfig(path)).ok)
                attachWindowBridge()
                val config = VulkanDriverConfiguration.resolve(
                    RuntimeVulkanDriver.SYSTEM_VORTEK,
                    VulkanDriverResolveContext(installed, vortekSocketPath = Paths.get(path)),
                )
                // Shorter present for sequential coverage.
                val result = runWsiProbe(installed, probe, config, seconds = 8)
                assertEquals(result.output, 0, result.exitCode)
                assertTrue(result.output, result.output.contains("[Vortek.WSI] result=success"))
                controller.setWindowBridge(null)
                controller.stop("wseq$i")
                assertFalse(File(path).exists())
            }
            assertEquals(2, sockets.distinct().size)
        }
    }

    @Test
    fun headlessStillWorks_regression() {
        runBlocking {
            val installed = installRuntime()
            val probe = pickProbe(installed)
            val path = VortekServerController.sessionSocketPath(targetContext.filesDir, "t6reg")
            assertTrue(controller.start(VortekServerConfig(path)).ok)
            // No window bridge — headless must not require Surface.
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
                        "BACHATA_VORTEK_WSI" to "0",
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
            controller.stop("t6reg")
        }
    }

    @Test
    fun nonVortek_noServerOrBridge() {
        val installed = installRuntime()
        val turnip = VulkanDriverConfiguration.resolve(RuntimeVulkanDriver.TURNIP_26_1_0, installed)
        assertFalse(turnip.environment.containsKey("BACHATA_VORTEK_SOCKET"))
        assertEquals(VortekServerState.STOPPED, controller.state())
    }

    private suspend fun startDisplay() {
        val reader = ImageReader.newInstance(1280, 720, PixelFormat.RGBA_8888, 2)
        imageReader = reader
        val server = WinlatorEmbeddedXServer(
            context = targetContext,
            socketRoot = File(targetContext.cacheDir, "vortek-wsi-x").apply { mkdirs() },
            useAbstractXSocket = true,
            xSocketPath = UnixSocketConfig.XSERVER_PATH,
            useSharedMemoryAudio = false,
        )
        server.start(reader.surface, 1280, 720)
        xServer = server
        Log.i(TAG, "x_server=started display=${server.display}")
    }

    private suspend fun attachWindowBridge() {
        val xs = xServer?.xServer ?: error("x_server_missing")
        val bridge = VortekWindowBridge(xs)
        val result = controller.setWindowBridge(bridge)
        assertTrue("window bridge: ${result.message}", result.isOk)
        Log.i(TAG, "window_bridge=attached")
    }

    private fun runWsiProbe(
        installed: Path,
        probe: Path,
        config: VulkanDriverConfiguration,
        seconds: Int,
    ) = RuntimeProbeLauncher().run(
        RuntimeProbeRequest(
            nativeLibraryDir = File(targetContext.applicationInfo.nativeLibraryDir).toPath(),
            runtimeRoot = installed,
            executable = probe,
            environment = config.environment + mapOf(
                "BACHATA_VORTEK_WSI" to "1",
                "BACHATA_VORTEK_WSI_SECONDS" to seconds.toString(),
                "BACHATA_VORTEK_WSI_RESIZE" to "1",
                "BACHATA_VORTEK_HEADLESS" to "0",
                "BACHATA_VORTEK_TRANSPORT_ONLY" to "0",
                "BACHATA_VORTEK_LOG_LEVEL" to "1",
                "DISPLAY" to (xServer?.display ?: ":0"),
                "HOME" to targetContext.filesDir.absolutePath,
                "TMPDIR" to targetContext.cacheDir.absolutePath,
                "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
                // Match Gate3: emulated X11 libs for Box64 HOST_GLIBC guest.
                "BOX64_EMULATED_LIBS" to listOf(
                    "libX11.so.6",
                    "libX11-xcb.so.1",
                    "libXext.so.6",
                    "libXau.so.6",
                    "libXdmcp.so.6",
                    "libxcb.so.1",
                ).joinToString(":"),
            ),
            executionMode = executionMode(probe),
        ),
        timeoutSeconds = (seconds + 45).toLong(),
    )

    private fun executionMode(probe: Path) =
        if (probe.fileName.toString().contains("aarch64")) {
            RuntimeProbeExecutionMode.HOST_GLIBC_NATIVE
        } else {
            RuntimeProbeExecutionMode.BOX64_HOST_GLIBC
        }

    private fun pickProbe(installed: Path): Path {
        // Prefer aarch64 HOST_GLIBC_NATIVE (same proven path as Task 5/6).
        // x86-64 Box64 probe is also packaged; try it when aarch64 is absent.
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
            .resolve("vortek-t7-${System.nanoTime()}")
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
        check(child.fileName.toString().startsWith("vortek-t7-"))
        if (!Files.exists(child)) return
        Files.walk(child).use { paths ->
            paths.sorted(Comparator.reverseOrder()).forEach(Files::deleteIfExists)
        }
    }

    companion object {
        private const val TAG = "Bachata.Vortek.T7"
    }
}
