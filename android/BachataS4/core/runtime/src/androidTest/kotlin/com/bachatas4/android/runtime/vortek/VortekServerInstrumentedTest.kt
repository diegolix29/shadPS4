package com.bachatas4.android.runtime.vortek

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Test
import org.junit.runner.RunWith

/**
 * On-device Task 4 gate: loader, socket, handshake, CREATE_CONTEXT rings, sequential sessions.
 */
@RunWith(AndroidJUnit4::class)
class VortekServerInstrumentedTest {
    private val controller = VortekServerController()
    private val context = InstrumentationRegistry.getInstrumentation().targetContext

    @After
    fun tearDown() = runBlocking {
        controller.stop("teardown")
        Unit
    }

    @Test
    fun startStopWithoutClient_removesSocket() = runBlocking {
        val path = VortekServerController.sessionSocketPath(context.filesDir, "t4a")
        val start = controller.start(
            VortekServerConfig(
                socketPath = path,
                expectedClientBuild = "*",
                serverBuild = "bachata-vortek-server-instrumented",
            ),
        )
        assertTrue(start.ok, start.message)
        assertTrue(start.hostApiVersion.isNotBlank(), "host vulkan version missing")
        assertTrue(File(path).exists(), "socket file should exist while listening")
        val stop = controller.stop("no-client")
        assertTrue(stop.ok, stop.message)
        assertEquals(VortekServerState.STOPPED, controller.state())
        assertFalse(File(path).exists(), "socket must be removed after stop")
    }

    @Test
    fun protocolSelfTest_handshakeAndCreateContext_twice() = runBlocking {
        val path = VortekServerController.sessionSocketPath(context.filesDir, "t4b")
        val start = controller.start(
            VortekServerConfig(socketPath = path, serverBuild = "bachata-vortek-server-instrumented"),
        )
        assertTrue(start.ok, start.message)

        val first = controller.runProtocolSelfTest("self-test-1")
        assertTrue(first.isOk, "first self-test failed: ${first.message} code=${first.code}")
        controller.waitContextReady(5_000)

        // After client disconnect, server returns to socket_ready; run again.
        Thread.sleep(200)
        val second = controller.runProtocolSelfTest("self-test-2")
        assertTrue(second.isOk, "second self-test failed: ${second.message} code=${second.code}")

        controller.stop("done")
        assertFalse(File(path).exists())
    }

    @Test
    fun repeatedStart_returnsAlreadyRunning() = runBlocking {
        val path = VortekServerController.sessionSocketPath(context.filesDir, "t4c")
        val first = controller.start(VortekServerConfig(path))
        assertTrue(first.ok, first.message)
        val second = controller.start(VortekServerConfig(path))
        assertEquals(1, second.nativeCode)
        assertEquals("already_running", second.message)
        controller.stop("done")
        Unit
    }

    @Test
    fun socketPathTooLong_failsWithoutBind() = runBlocking {
        val longPath = context.filesDir.absolutePath + "/" + "x".repeat(120) + ".sock"
        val result = controller.start(VortekServerConfig(longPath))
        assertFalse(result.ok)
        assertEquals(3, result.nativeCode)
        assertEquals("socket_path_too_long", result.message)
    }

    @Test
    fun missingParent_fails() = runBlocking {
        val missing = File(context.filesDir, "no-such-dir-t4/child.sock").absolutePath
        // Controller creates parent via mkdirs; force native validation by using invalid absolute without create.
        // Use a path under a file-as-parent.
        val blocker = File(context.filesDir, "blocker-file-t4").apply {
            writeText("not-a-dir")
        }
        val bad = File(blocker, "x.sock").absolutePath
        val result = controller.start(VortekServerConfig(bad))
        assertFalse(result.ok)
        controller.stop("cleanup")
        blocker.delete()
        Unit
    }

    @Test
    fun existingRegularFile_isUnsafe() = runBlocking {
        val dir = File(context.filesDir, "vs").apply { mkdirs() }
        val file = File(dir, "regular-t4.sock").apply {
            writeText("not-a-socket")
        }
        // Native lstat sees regular file — must not unlink.
        val result = controller.start(VortekServerConfig(file.absolutePath))
        assertFalse(result.ok)
        assertEquals(4, result.nativeCode)
        assertTrue(file.exists(), "regular file must remain")
        file.delete()
        Unit
    }
}
