package com.bachatas4.android.runtime.vortek

/**
 * Narrow JNI surface for Task 4. Does not expose native pointers.
 */
internal object VortekNativeBridge {
    init {
        System.loadLibrary("bachata_vortek_server")
    }

    @JvmStatic
    external fun nativeStartServer(
        socketPath: String,
        expectedClientBuild: String,
        serverBuild: String,
    ): NativeVortekResult

    @JvmStatic
    external fun nativeGetState(): Int

    @JvmStatic
    external fun nativeLastError(): Int

    @JvmStatic
    external fun nativeStopServer(): NativeVortekResult

    @JvmStatic
    external fun nativeWaitSocketReady(timeoutMs: Int): NativeVortekResult

    @JvmStatic
    external fun nativeWaitContextReady(timeoutMs: Int): NativeVortekResult

    @JvmStatic
    external fun nativeProtocolSelfTest(socketPath: String, clientBuild: String): NativeVortekResult

    @JvmStatic
    external fun nativeHostApiVersion(): String

    /** Attach or clear WSI window bridge (null clears). */
    @JvmStatic
    external fun nativeSetWindowBridge(bridge: Any?): NativeVortekResult
}
