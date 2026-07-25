package com.bachatas4.android.runtime.vortek

import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlinx.coroutines.runBlocking
import org.junit.Test

class VortekServerControllerTest {
    @Test
    fun sessionSocketPath_isShortAndUnderFilesDir() {
        val files = File("/data/user/0/com.bachatas4.android/files")
        val path = VortekServerController.sessionSocketPath(files, "abc123session")
        assertTrue(path.startsWith(files.absolutePath))
        assertTrue(path.contains("/vs/"))
        assertTrue(path.endsWith(".sock"))
        assertTrue(path.toByteArray().size <= VortekServerController.MAX_SOCKET_PATH_BYTES)
    }

    @Test
    fun validateSocketPath_rejectsEmptyAndWinlator() {
        assertEquals(2, VortekServerController.validateSocketPath("")?.first)
        assertEquals(2, VortekServerController.validateSocketPath("relative.sock")?.first)
        assertNotNull(VortekServerController.validateSocketPath("/data/data/com.winlator/files/x.sock"))
    }

    @Test
    fun validateSocketPath_rejectsTooLong() {
        val long = "/" + "a".repeat(200)
        assertEquals(3, VortekServerController.validateSocketPath(long)?.first)
        assertEquals("socket_path_too_long", VortekServerController.validateSocketPath(long)?.second)
    }

    @Test
    fun start_usesBridgeAndReportsAlreadyRunningSemantics() = runBlocking {
        val fake = object : VortekNativeBridgeApi {
            var starts = 0
            var state = 0
            override fun nativeStartServer(socketPath: String, expectedClientBuild: String, serverBuild: String): NativeVortekResult {
                starts++
                if (starts > 1) return NativeVortekResult(1, "already_running")
                state = 3 // SOCKET_READY
                return NativeVortekResult(0, "ok")
            }
            override fun nativeGetState(): Int = state
            override fun nativeLastError(): Int = 0
            override fun nativeStopServer(): NativeVortekResult {
                state = 0
                return NativeVortekResult(0, "ok")
            }
            override fun nativeWaitSocketReady(timeoutMs: Int) = NativeVortekResult(0, "ok")
            override fun nativeWaitContextReady(timeoutMs: Int) = NativeVortekResult(0, "ok")
            override fun nativeProtocolSelfTest(socketPath: String, clientBuild: String) =
                NativeVortekResult(0, "ok")
            override fun nativeHostApiVersion(): String = "1.1.128"
            override fun nativeSetWindowBridge(bridge: Any?) = NativeVortekResult(0, "ok")
        }
        val controller = VortekServerController(fake)
        val dir = File(System.getProperty("java.io.tmpdir"), "bachata-vortek-unit").apply { mkdirs() }
        val sock = File(dir, "t.sock").absolutePath
        val first = controller.start(VortekServerConfig(sock))
        assertTrue(first.ok)
        assertEquals(VortekServerState.SOCKET_READY, first.state)
        assertEquals("1.1.128", first.hostApiVersion)
        val second = controller.start(VortekServerConfig(sock))
        assertEquals(1, second.nativeCode)
        assertEquals("already_running", second.message)
        val stop = controller.stop("test")
        assertTrue(stop.ok)
    }

    @Test
    fun validateSocketPath_acceptsAppStylePath() {
        assertNull(
            VortekServerController.validateSocketPath(
                "/data/user/0/com.bachatas4.android/files/vs/a1.sock",
            ),
        )
    }
}
