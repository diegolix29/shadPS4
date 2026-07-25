package com.bachatas4.android.runtime.vortek

import java.io.File
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/**
 * Owns the single Task 4 Vortek server lifecycle for a session/debug harness.
 * Production emulator launch does not start this yet (Task 5).
 */
class VortekServerController(
    private val bridge: VortekNativeBridgeApi = DefaultVortekNativeBridge,
) {
    private val mutex = Mutex()
    private val configRef = AtomicReference<VortekServerConfig?>(null)

    fun state(): VortekServerState = VortekServerState.fromNative(bridge.nativeGetState())

    suspend fun start(config: VortekServerConfig): VortekStartResult = mutex.withLock {
        withContext(Dispatchers.IO) {
            val validation = validateSocketPath(config.socketPath)
            if (validation != null) {
                return@withContext VortekStartResult(
                    ok = false,
                    state = VortekServerState.FAILED,
                    nativeCode = validation.first,
                    message = validation.second,
                )
            }
            ensureParentDir(config.socketPath)
            val result = bridge.nativeStartServer(
                config.socketPath,
                config.expectedClientBuild,
                config.serverBuild,
            )
            if (result.isOk) {
                configRef.set(config)
                bridge.nativeWaitSocketReady(5_000)
            }
            VortekStartResult(
                ok = result.isOk,
                state = state(),
                nativeCode = result.code,
                message = result.message,
                hostApiVersion = runCatching { bridge.nativeHostApiVersion() }.getOrDefault(""),
            )
        }
    }

    suspend fun stop(reason: String = "requested"): VortekStopResult = mutex.withLock {
        withContext(Dispatchers.IO) {
            runCatching { bridge.nativeSetWindowBridge(null) }
            val result = bridge.nativeStopServer()
            configRef.set(null)
            VortekStopResult(ok = result.isOk, message = "$reason:${result.message}")
        }
    }

    suspend fun runProtocolSelfTest(clientBuild: String = "self-test"): NativeVortekResult =
        mutex.withLock {
            withContext(Dispatchers.IO) {
                val path = configRef.get()?.socketPath
                    ?: return@withContext NativeVortekResult(11, "not_running")
                bridge.nativeProtocolSelfTest(path, clientBuild)
            }
        }

    suspend fun waitContextReady(timeoutMs: Int = 10_000): NativeVortekResult =
        withContext(Dispatchers.IO) { bridge.nativeWaitContextReady(timeoutMs) }

    /**
     * Attach X-server window bridge for WSI (null clears).
     * Must be called before guest CREATE_CONTEXT starts request dispatch that needs AHB.
     * Safe to call while server is SOCKET_READY / idle.
     */
    suspend fun setWindowBridge(windowBridge: Any?): NativeVortekResult =
        withContext(Dispatchers.IO) { bridge.nativeSetWindowBridge(windowBridge) }

    companion object {
        /** Android sun_path capacity is typically 108 including NUL. */
        const val MAX_SOCKET_PATH_BYTES = 107

        fun sessionSocketPath(filesDir: File, shortSessionId: String): String {
            val safe = shortSessionId.filter { it.isLetterOrDigit() || it == '-' || it == '_' }.take(12)
            val dir = File(filesDir, "vs").apply { mkdirs() }
            return File(dir, "$safe.sock").absolutePath
        }

        fun validateSocketPath(path: String): Pair<Int, String>? {
            if (path.isEmpty()) return 2 to "socket_path"
            if (!path.startsWith("/")) return 2 to "socket_path"
            if ("com.winlator" in path) return 2 to "socket_path"
            if (path.toByteArray(Charsets.UTF_8).size > MAX_SOCKET_PATH_BYTES) {
                return 3 to "socket_path_too_long"
            }
            val file = File(path)
            val parent = file.parentFile
            if (parent == null || (!parent.exists() && !parent.mkdirs() && !parent.exists())) {
                // parent may be created by ensureParentDir; missing after that is native error
            }
            if (file.exists()) {
                // Symlink or regular file must not be unlinked blindly (native enforces).
                if (file.isFile && !file.name.endsWith(".sock")) {
                    // Heuristic for pure-Kotlin tests; native uses lstat.
                }
            }
            return null
        }

        private fun ensureParentDir(path: String) {
            File(path).parentFile?.mkdirs()
        }
    }
}

/** Test seam over JNI. */
interface VortekNativeBridgeApi {
    fun nativeStartServer(socketPath: String, expectedClientBuild: String, serverBuild: String): NativeVortekResult
    fun nativeGetState(): Int
    fun nativeLastError(): Int
    fun nativeStopServer(): NativeVortekResult
    fun nativeWaitSocketReady(timeoutMs: Int): NativeVortekResult
    fun nativeWaitContextReady(timeoutMs: Int): NativeVortekResult
    fun nativeProtocolSelfTest(socketPath: String, clientBuild: String): NativeVortekResult
    fun nativeHostApiVersion(): String
    fun nativeSetWindowBridge(bridge: Any?): NativeVortekResult
}

object DefaultVortekNativeBridge : VortekNativeBridgeApi {
    override fun nativeStartServer(socketPath: String, expectedClientBuild: String, serverBuild: String) =
        VortekNativeBridge.nativeStartServer(socketPath, expectedClientBuild, serverBuild)

    override fun nativeGetState(): Int = VortekNativeBridge.nativeGetState()
    override fun nativeLastError(): Int = VortekNativeBridge.nativeLastError()
    override fun nativeStopServer(): NativeVortekResult = VortekNativeBridge.nativeStopServer()
    override fun nativeWaitSocketReady(timeoutMs: Int): NativeVortekResult =
        VortekNativeBridge.nativeWaitSocketReady(timeoutMs)
    override fun nativeWaitContextReady(timeoutMs: Int): NativeVortekResult =
        VortekNativeBridge.nativeWaitContextReady(timeoutMs)
    override fun nativeProtocolSelfTest(socketPath: String, clientBuild: String): NativeVortekResult =
        VortekNativeBridge.nativeProtocolSelfTest(socketPath, clientBuild)
    override fun nativeHostApiVersion(): String = VortekNativeBridge.nativeHostApiVersion()
    override fun nativeSetWindowBridge(bridge: Any?): NativeVortekResult =
        VortekNativeBridge.nativeSetWindowBridge(bridge)
}
