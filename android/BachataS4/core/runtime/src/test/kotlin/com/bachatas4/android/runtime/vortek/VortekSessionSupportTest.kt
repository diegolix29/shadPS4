package com.bachatas4.android.runtime.vortek

import com.bachatas4.android.runtime.process.RuntimeVulkanDriverIds
import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotEquals
import kotlin.test.assertTrue
import kotlinx.coroutines.runBlocking
import org.junit.Test

class VortekSessionSupportTest {
    @Test
    fun serverStartsOnlyWhenRequestedAndAlwaysStops() = runBlocking {
        var startCount = 0
        var stopCount = 0
        var state = 0
        val fake = object : VortekNativeBridgeApi {
            override fun nativeStartServer(
                socketPath: String,
                expectedClientBuild: String,
                serverBuild: String,
            ): NativeVortekResult {
                startCount++
                state = 3 // SOCKET_READY
                return NativeVortekResult(0, "ok")
            }

            override fun nativeGetState(): Int = state
            override fun nativeLastError(): Int = 0
            override fun nativeStopServer(): NativeVortekResult {
                stopCount++
                state = 0
                return NativeVortekResult(0, "ok")
            }

            override fun nativeWaitSocketReady(timeoutMs: Int) = NativeVortekResult(0, "ok")
            override fun nativeWaitContextReady(timeoutMs: Int) = NativeVortekResult(0, "ok")
            override fun nativeProtocolSelfTest(socketPath: String, clientBuild: String) =
                NativeVortekResult(0, "ok")
            override fun nativeHostApiVersion(): String = "1.4.0"
            override fun nativeSetWindowBridge(bridge: Any?) = NativeVortekResult(0, "ok")
        }

        val dir = File(System.getProperty("java.io.tmpdir"), "bachata-vortek-session-unit").apply {
            mkdirs()
        }
        val support = VortekSessionSupport(
            filesDir = dir,
            sessionShortId = "s1",
            controller = VortekServerController(fake),
            log = { _, _ -> },
        )
        val start = support.startServer()
        assertTrue(start.ok, start.message)
        assertEquals(1, startCount)
        assertTrue(support.socketPath!!.contains("/vs/"))
        assertTrue(support.socketPath!!.endsWith(".sock"))

        // Simulate launch failure cleanup.
        val stop = support.stop("launch_failed", guestExitCode = null)
        assertTrue(stop.ok)
        assertEquals(1, stopCount)
        assertEquals(VortekServerState.STOPPED, VortekServerController(fake).state())
    }

    @Test
    fun sequentialSessionsUseDifferentSocketPaths() {
        val dir = File(System.getProperty("java.io.tmpdir"), "bachata-vortek-session-unit2").apply {
            mkdirs()
        }
        val a = VortekServerController.sessionSocketPath(dir, "aaaa")
        val b = VortekServerController.sessionSocketPath(dir, "bbbb")
        assertNotEquals(a, b)
        assertTrue(a.contains("/vs/aaaa.sock") || a.endsWith("aaaa.sock"))
        assertTrue(b.endsWith("bbbb.sock"))
    }

    @Test
    fun nonVortekDriverIdDoesNotMatch() {
        assertFalse(RuntimeVulkanDriverIds.isVortek("system"))
        assertFalse(RuntimeVulkanDriverIds.isVortek(null))
        assertFalse(RuntimeVulkanDriverIds.isVortek("turnip-abc"))
        assertTrue(RuntimeVulkanDriverIds.isVortek(RuntimeVulkanDriverIds.SYSTEM_VORTEK))
    }

    @Test
    fun failureCategoryMapsSocketNotReady() {
        val result = VortekStartResult(
            ok = false,
            state = VortekServerState.FAILED,
            nativeCode = 12,
            message = "vortek_socket_not_ready:state=STARTING",
        )
        assertEquals("vortek_socket_not_ready", result.failureCategory())
        val startFail = result.copy(message = "native boom")
        assertEquals("vortek_server_start_failed", startFail.failureCategory())
    }
}
