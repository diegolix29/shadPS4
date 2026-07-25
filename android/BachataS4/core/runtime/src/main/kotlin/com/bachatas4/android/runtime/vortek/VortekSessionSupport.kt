package com.bachatas4.android.runtime.vortek

import com.bachatas4.android.runtime.process.VulkanDriverConfiguration
import java.io.File
import java.nio.file.Path
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Session-owned Vortek helper used by EmulationService (and tests).
 * Not a second session coordinator — only owns server start/stop + socket identity.
 */
class VortekSessionSupport(
    private val filesDir: File,
    private val sessionShortId: String,
    val controller: VortekServerController = VortekServerController(),
    private val log: (String, String) -> Unit = { tag, msg -> android.util.Log.i(tag, msg) },
) {
    private val socketPathRef = AtomicReference<String?>(null)

    val socketPath: String?
        get() = socketPathRef.get()

    val isStarted: Boolean
        get() = controller.state() != VortekServerState.STOPPED &&
            controller.state() != VortekServerState.FAILED

    /**
     * Create app-owned socket path, start server, wait until SOCKET_READY.
     * Does not launch the guest.
     */
    suspend fun startServer(
        expectedClientBuild: String = VulkanDriverConfiguration.VORTEK_CLIENT_BUILD,
        socketReadyTimeoutMs: Int = SOCKET_READY_TIMEOUT_MS,
    ): VortekStartResult = withContext(Dispatchers.IO) {
        val path = VortekServerController.sessionSocketPath(filesDir, sessionShortId)
        socketPathRef.set(path)
        log(LOG_TAG, "session=$sessionShortId")
        log(LOG_TAG, "server_start=begin")
        log(LOG_TAG, "host_loader=libvulkan.so")
        // start() already blocks until SOCKET_READY (or failure) with a bounded wait.
        val start = controller.start(
            VortekServerConfig(
                socketPath = path,
                expectedClientBuild = expectedClientBuild,
                serverBuild = "bachata-vortek-server",
            ),
        )
        if (!start.ok) {
            log(LOG_TAG, "server_start=failed code=${start.nativeCode} msg=${start.message}")
            return@withContext start
        }
        if (start.hostApiVersion.isNotBlank()) {
            log(LOG_TAG, "host_api=${start.hostApiVersion}")
        }
        val state = controller.state()
        if (state.ordinal < VortekServerState.SOCKET_READY.ordinal) {
            log(LOG_TAG, "socket=not_ready state=$state")
            controller.stop("socket_not_ready")
            return@withContext VortekStartResult(
                ok = false,
                state = VortekServerState.FAILED,
                nativeCode = 12,
                message = "vortek_socket_not_ready:state=$state",
                hostApiVersion = start.hostApiVersion,
            )
        }
        // socketReadyTimeoutMs reserved for future extra wait; start() covers the Task 4 path.
        @Suppress("UNUSED_VARIABLE")
        val ignoredTimeout = socketReadyTimeoutMs
        log(LOG_TAG, "socket=ready path=$path")
        start.copy(state = state)
    }

    suspend fun waitContextReady(timeoutMs: Int = CONTEXT_READY_TIMEOUT_MS): NativeVortekResult {
        log(LOG_TAG, "context=create_requested")
        val result = controller.waitContextReady(timeoutMs)
        if (result.isOk || controller.state() == VortekServerState.CONTEXT_READY) {
            log(LOG_TAG, "state=context_ready")
            log(LOG_TAG, "client_build=${VulkanDriverConfiguration.VORTEK_CLIENT_BUILD}")
            log(LOG_TAG, "handshake=accepted")
        } else {
            log(LOG_TAG, "context=timeout code=${result.code} msg=${result.message}")
        }
        return result
    }

    suspend fun stop(reason: String, guestExitCode: Int? = null): VortekStopResult =
        withContext(Dispatchers.IO) {
            guestExitCode?.let { log(LOG_TAG, "guest_exit=$it") }
            log(LOG_TAG, "server_stop=begin reason=$reason")
            val path = socketPathRef.get()
            val result = controller.stop(reason)
            log(LOG_TAG, "resources=released")
            if (path != null && !File(path).exists()) {
                log(LOG_TAG, "socket=removed")
            } else if (path != null) {
                // Best-effort unlink if native left a stale path
                runCatching { File(path).delete() }
                if (!File(path).exists()) log(LOG_TAG, "socket=removed")
            }
            socketPathRef.set(null)
            log(LOG_TAG, "state=stopped")
            result
        }

    fun socketAsPath(): Path? = socketPath?.let { java.nio.file.Paths.get(it) }

    companion object {
        const val LOG_TAG = "Bachata.Vortek"
        const val SOCKET_READY_TIMEOUT_MS = 5_000
        const val CONTEXT_READY_TIMEOUT_MS = 15_000
        const val CLIENT_CONNECT_TIMEOUT_MS = 20_000

        fun newSessionId(): String =
            java.util.UUID.randomUUID().toString().replace("-", "").take(8)
    }
}

/** Map start failures to stable session error categories. */
fun VortekStartResult.failureCategory(): String =
    when {
        !ok && message.startsWith("vortek_socket_not_ready") -> "vortek_socket_not_ready"
        !ok -> "vortek_server_start_failed"
        else -> "ok"
    }
