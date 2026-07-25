package com.bachatas4.android.runtime.vortek

/**
 * Task 4 lifecycle types for the session-owned Android Vortek server.
 * Not wired into production emulator launch yet.
 */
enum class VortekServerState {
    STOPPED,
    STARTING,
    LOADER_READY,
    SOCKET_READY,
    CLIENT_CONNECTED,
    CONTEXT_READY,
    STOPPING,
    FAILED,
    ;

    companion object {
        fun fromNative(code: Int): VortekServerState =
            entries.getOrElse(code) { FAILED }
    }
}

data class VortekServerConfig(
    val socketPath: String,
    val expectedClientBuild: String = "*",
    val serverBuild: String = "bachata-vortek-server",
)

data class NativeVortekResult(
    val code: Int,
    val message: String,
) {
    val isOk: Boolean get() = code == 0
}

data class VortekStartResult(
    val ok: Boolean,
    val state: VortekServerState,
    val nativeCode: Int,
    val message: String,
    val hostApiVersion: String = "",
)

data class VortekStopResult(
    val ok: Boolean,
    val message: String,
)
