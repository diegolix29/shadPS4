package com.bachatas4.android.runtime.process

import com.bachatas4.android.runtime.settings.RuntimeGuestBackend
import java.io.File
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths
import java.util.concurrent.TimeUnit

data class RuntimeProcessRequest(
    val nativeLibraryDir: Path,
    val runtimeRoot: Path,
    val overrideRoot: Path,
    val storageRoot: Path = overrideRoot,
    val shadPs4Executable: Path,
    val socketPath: String,
    val environment: Map<String, String> = emptyMap(),
    val arguments: List<String> = emptyList(),
    val outputPath: Path? = null,
    val box64Mode: Box64Mode = Box64Mode.HOST_GLIBC,
    val guestBackend: RuntimeGuestBackend,
)

interface RuntimeProcessHandle {
    val isAlive: Boolean
    val exitCode: Int?

    fun destroy()

    fun destroyForcibly()

    fun waitFor(timeout: Long, unit: TimeUnit): Boolean
}

fun interface RuntimeProcessStarter {
    fun start(processBuilder: ProcessBuilder): RuntimeProcessHandle
}

class RuntimeProcessLauncher(
    private val starter: RuntimeProcessStarter = RuntimeProcessStarter { builder ->
        JavaRuntimeProcessHandle(builder.start())
    },
) {
    fun command(request: RuntimeProcessRequest): List<String> {
        val nativeLibraryDir = request.nativeLibraryDir.toRealPath()
        val runtimeRoot = request.runtimeRoot.toRealPath()
        val hostDirectory = runtimeRoot.resolve(HOST_DIRECTORY).toRealPath()
        require(hostDirectory.startsWith(runtimeRoot) && Files.isDirectory(hostDirectory)) {
            "Host runtime directory escapes runtime root: $hostDirectory"
        }
        val overrideRootArgument = request.overrideRoot.toAbsolutePath().normalize()
        val overrideRoot = request.overrideRoot.toRealPath()
        val storageRoot = request.storageRoot.toRealPath()
        require(overrideRoot.startsWith(storageRoot)) { "Game override escapes app storage" }
        val shadPs4 = request.shadPs4Executable.toRealPath()
        if (!shadPs4.startsWith(runtimeRoot)) {
            throw SecurityException("Runtime executable escapes runtime root: $shadPs4")
        }
        require(Files.isRegularFile(shadPs4) && Files.isReadable(shadPs4)) {
            "Runtime executable is not a readable file: $shadPs4"
        }
        val rawSocketPath = Paths.get(request.socketPath)
        val socketPath = rawSocketPath.parent?.toRealPath()?.resolve(rawSocketPath.fileName)?.normalize()
        require(request.socketPath.isNotBlank() && '\u0000' !in request.socketPath &&
            rawSocketPath.isAbsolute && socketPath?.startsWith(storageRoot) == true
        ) {
            "Invalid runtime socket path"
        }
        request.arguments.forEach { require('\u0000' !in it) { "Runtime argument contains NUL" } }

        val launchCommand = when (request.guestBackend) {
            RuntimeGuestBackend.FEX -> {
                val loader = request.nativeLibraryDir.resolve(HOST_LOADER_LIBRARY).toRealPath()
                validateNativeFile(nativeLibraryDir, loader, "Host glibc loader")
                val expected = hostDirectory.resolve(FEX_SHADPS4).toRealPath()
                require(shadPs4 == expected) { "FEX executable must be the verified native runtime binary" }
                listOf(loader.toString(), "--library-path", hostDirectory.toString())
            }
            RuntimeGuestBackend.BOX64 -> when (request.box64Mode) {
                Box64Mode.APK_NATIVE -> {
                    val box64 = request.nativeLibraryDir.resolve(BOX64_LIBRARY).toRealPath()
                    validateNativeExecutable(nativeLibraryDir, box64, "APK native Box64")
                    listOf(box64.toString())
                }
                Box64Mode.HOST_GLIBC -> {
                    val loader = request.nativeLibraryDir.resolve(HOST_LOADER_LIBRARY).toRealPath()
                    val box64 = request.nativeLibraryDir.resolve(HOST_BOX64_LIBRARY).toRealPath()
                    validateNativeFile(nativeLibraryDir, loader, "Host glibc loader")
                    validateNativeFile(nativeLibraryDir, box64, "Host Box64")
                    listOf(loader.toString(), "--library-path", hostDirectory.toString(), box64.toString())
                }
            }
        }
        return launchCommand + listOf(
            shadPs4.toString(),
            "--override-root",
            overrideRootArgument.toString(),
            "--bachata-storage-root",
            storageRoot.toString(),
            "--bachata-socket",
            socketPath.toString(),
        ) + request.arguments
    }

    private fun validateNativeFile(nativeLibraryDir: Path, path: Path, label: String) {
        if (path.parent != nativeLibraryDir) throw SecurityException("$label must be owned by nativeLibraryDir")
        require(Files.isRegularFile(path) && Files.isReadable(path)) { "$label is not readable: $path" }
    }

    private fun validateNativeExecutable(nativeLibraryDir: Path, path: Path, label: String) {
        validateNativeFile(nativeLibraryDir, path, label)
        require(Files.isExecutable(path)) { "$label is not executable: $path" }
    }

    private fun ensureOwnerExecutable(path: Path) {
        if (Files.isExecutable(path)) return
        check(path.toFile().setExecutable(true, true) && Files.isExecutable(path)) {
            "Unable to make verified executable owner-executable: $path"
        }
    }

    fun launch(request: RuntimeProcessRequest): RuntimeProcessHandle {
        val command = command(request)
        // argv[0] (loader or box64) and shadPS4 must be executable; runtime host/ copies
        // may unpack without the +x bit on some filesystems.
        ensureOwnerExecutable(Paths.get(command.first()))
        val shadPs4 = request.shadPs4Executable.toRealPath()
        ensureOwnerExecutable(shadPs4)
        val builder = ProcessBuilder(command)
        builder.directory(request.runtimeRoot.toRealPath().toFile())
        val outputPath = request.outputPath
        if (outputPath == null) {
            builder.redirectOutput(NULL_DEVICE)
            builder.redirectError(NULL_DEVICE)
        } else {
            val parent = outputPath.toAbsolutePath().normalize().parent.toRealPath()
            require(parent.startsWith(request.storageRoot.toRealPath())) { "Runtime output escapes app storage" }
            builder.redirectErrorStream(true)
            builder.redirectOutput(outputPath.toFile())
        }
        builder.environment().apply {
            clear()
            request.environment.forEach { (name, value) ->
                if (name in ALLOWED_ENVIRONMENT || BOX64_ENVIRONMENT.matches(name)) put(name, value)
            }
        }
        return starter.start(builder)
    }

    private companion object {
        const val HOST_DIRECTORY = "host"
        const val HOST_LOADER_LIBRARY = "libbachata_host_loader.so"
        const val HOST_BOX64_LIBRARY = "libbachata_host_box64.so"
        const val BOX64_LIBRARY = "libbox64.so"
        const val FEX_SHADPS4 = "shadps4-arm64-fex"
        val NULL_DEVICE = File("/dev/null")
        val ALLOWED_ENVIRONMENT = setOf(
            "HOME",
            "LD_LIBRARY_PATH",
            "BOX64_PATH",
            "BOX64_LOG",
            "BOX64_LOAD_ADDR",
            "BOX64_PREFER_WRAPPED",
            "BOX64_LD_LIBRARY_PATH",
            "BOX64_EMULATED_LIBS",
            "BOX64_DYNAREC_CALLRET",
            "BACHATA_ALSA_SOCKET",
            "BACHATA_VULKAN_DRIVER_DIR",
            "BACHATA_VULKAN_DRIVER_NAME",
            "BACHATA_VULKAN_TMPDIR",
            "DISPLAY",
            "SDL_VIDEODRIVER",
            "SDL_VULKAN_LIBRARY",
            "TMPDIR",
            "XDG_CACHE_HOME",
            "XKB_CONFIG_ROOT",
            "MESA_SHADER_CACHE_DIR",
            "VK_ICD_FILENAMES",
            "GLIBC_TUNABLES",
            "BACHATA_VORTEK_SOCKET",
            "BACHATA_VORTEK_HANDSHAKE",
            "BACHATA_VORTEK_LOG_LEVEL",
            "BACHATA_VORTEK_TRACE",
            "BACHATA_VORTEK_TRACE_FENCES",
            "BACHATA_VORTEK_FENCE_WAIT_MODE",
            "BACHATA_VORTEK_FENCE_WAIT_FORCE_DEVICE_LOST",
            "BACHATA_CRASH_REGISTERS",
            "BACHATA_VORTEK_PROC_AUDIT",
            "BACHATA_VORTEK_TRACE_BIND_VERTEX_BUFFERS",
            "BACHATA_FEX_TRACE_SIGSYS",
        )
        val BOX64_ENVIRONMENT = Regex("BOX64_[A-Z0-9_]+")
    }
}

private class JavaRuntimeProcessHandle(
    private val process: Process,
) : RuntimeProcessHandle {
    override val isAlive: Boolean
        get() = process.isAlive

    override val exitCode: Int?
        get() = runCatching(process::exitValue).getOrNull()

    override fun destroy() {
        process.destroy()
    }

    override fun destroyForcibly() {
        process.destroyForcibly()
    }

    override fun waitFor(timeout: Long, unit: TimeUnit): Boolean = process.waitFor(timeout, unit)
}
