package com.bachatas4.android.runtime.process

import java.io.InputStream
import java.io.InterruptedIOException
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.TimeUnit

data class RuntimeProbeRequest(
    val nativeLibraryDir: Path,
    val runtimeRoot: Path,
    val executable: Path,
    val environment: Map<String, String> = emptyMap(),
    val executionMode: RuntimeProbeExecutionMode = RuntimeProbeExecutionMode.BOX64_APK_NATIVE,
    val arguments: List<String> = emptyList(),
)

enum class Box64Mode {
    APK_NATIVE,
    HOST_GLIBC,
}

enum class RuntimeProbeExecutionMode {
    BOX64_APK_NATIVE,
    BOX64_HOST_GLIBC,
    HOST_GLIBC_NATIVE,
}

data class RuntimeProbeResult(
    val exitCode: Int,
    val output: String,
)

internal fun readProcessOutput(input: InputStream, output: StringBuilder) {
    try {
        input.bufferedReader().useLines { lines ->
            lines.forEach { line ->
                if (output.length < 64 * 1024) output.appendLine(line)
            }
        }
    } catch (_: InterruptedIOException) {
        // Android closes the process pipe from another thread after forced termination.
    }
}

class RuntimeProbeLauncher {
    fun command(request: RuntimeProbeRequest): List<String> {
        val runtimeRoot = request.runtimeRoot.toRealPath()
        val executable = request.executable.toRealPath()
        validateContainedFile(runtimeRoot, executable, "Probe executable")
        request.arguments.forEach { argument ->
            require('\u0000' !in argument) { "Probe argument contains NUL" }
        }
        return when (request.executionMode) {
            RuntimeProbeExecutionMode.BOX64_APK_NATIVE -> apkNativeCommand(request, executable)
            RuntimeProbeExecutionMode.BOX64_HOST_GLIBC -> hostGlibcCommand(request, runtimeRoot, executable)
            RuntimeProbeExecutionMode.HOST_GLIBC_NATIVE -> hostGlibcNativeCommand(request, runtimeRoot, executable)
        } + request.arguments
    }

    private fun apkNativeCommand(request: RuntimeProbeRequest, executable: Path): List<String> {
        val nativeLibraryDir = request.nativeLibraryDir.toRealPath()
        val box64 = request.nativeLibraryDir.resolve(BOX64_LIBRARY).toRealPath()
        if (box64.parent != nativeLibraryDir) {
            throw SecurityException("Box64 must be owned by nativeLibraryDir")
        }
        require(Files.isRegularFile(box64) && Files.isExecutable(box64)) {
            "Box64 is not an executable APK native library: $box64"
        }
        return listOf(box64.toString(), executable.toString())
    }

    private fun hostGlibcCommand(request: RuntimeProbeRequest, runtimeRoot: Path, executable: Path): List<String> {
        val nativeLibraryDir = request.nativeLibraryDir.toRealPath()
        val loader = request.nativeLibraryDir.resolve(HOST_LOADER_LIBRARY).toRealPath()
        val box64 = request.nativeLibraryDir.resolve(HOST_BOX64_LIBRARY).toRealPath()
        val hostDirectory = runtimeRoot.resolve(HOST_DIRECTORY).toRealPath()
        // Must be APK nativeLibraryDir — Android blocks execve on filesDir host/ copies.
        validateNativeFile(nativeLibraryDir, loader, "Host glibc loader")
        validateNativeFile(nativeLibraryDir, box64, "Host Box64")
        validateContainedDirectory(runtimeRoot, hostDirectory, "Host runtime directory")
        return listOf(
            loader.toString(),
            "--library-path",
            hostDirectory.toString(),
            box64.toString(),
            executable.toString(),
        )
    }

    private fun hostGlibcNativeCommand(request: RuntimeProbeRequest, runtimeRoot: Path, executable: Path): List<String> {
        val nativeLibraryDir = request.nativeLibraryDir.toRealPath()
        val loader = request.nativeLibraryDir.resolve(HOST_LOADER_LIBRARY).toRealPath()
        val hostDirectory = runtimeRoot.resolve(HOST_DIRECTORY).toRealPath()
        validateNativeFile(nativeLibraryDir, loader, "Host glibc loader")
        validateContainedDirectory(runtimeRoot, hostDirectory, "Host runtime directory")
        validateContainedFile(runtimeRoot, executable, "Host glibc native probe")
        return listOf(
            loader.toString(),
            "--library-path",
            hostDirectory.toString(),
            executable.toString(),
        )
    }

    fun processBuilder(request: RuntimeProbeRequest): ProcessBuilder {
        val command = command(request)
        executablePaths(request).forEach { executable ->
            if (Files.isExecutable(executable)) return@forEach
            val file = executable.toFile()
            check(file.setExecutable(true, true) && Files.isExecutable(file.toPath())) {
                "Unable to make verified executable owner-executable: $file"
            }
        }
        return ProcessBuilder(command).apply {
            directory(request.runtimeRoot.toRealPath().toFile())
            redirectErrorStream(true)
            environment().apply {
                clear()
                request.environment.forEach { (name, value) ->
                    if (name in ALLOWED_ENVIRONMENT) put(name, value)
                }
            }
        }
    }

    private fun executablePaths(request: RuntimeProbeRequest): List<Path> {
        val probe = request.executable.toRealPath()
        return when (request.executionMode) {
            RuntimeProbeExecutionMode.BOX64_APK_NATIVE -> listOf(probe)
            RuntimeProbeExecutionMode.BOX64_HOST_GLIBC -> listOf(
                request.nativeLibraryDir.resolve(HOST_LOADER_LIBRARY).toRealPath(),
                request.nativeLibraryDir.resolve(HOST_BOX64_LIBRARY).toRealPath(),
                probe,
            )
            RuntimeProbeExecutionMode.HOST_GLIBC_NATIVE -> listOf(
                request.nativeLibraryDir.resolve(HOST_LOADER_LIBRARY).toRealPath(),
            )
        }
    }

    private fun validateNativeFile(nativeLibraryDir: Path, path: Path, label: String) {
        if (path.parent != nativeLibraryDir) {
            throw SecurityException("$label must be owned by nativeLibraryDir: $path")
        }
        require(Files.isRegularFile(path) && Files.isReadable(path)) {
            "$label is not readable: $path"
        }
    }

    private fun validateContainedFile(runtimeRoot: Path, path: Path, label: String) {
        if (!path.startsWith(runtimeRoot)) {
            throw SecurityException("$label escapes runtime root: $path")
        }
        require(Files.isRegularFile(path) && Files.isReadable(path)) {
            "$label is not readable: $path"
        }
    }

    private fun validateContainedDirectory(runtimeRoot: Path, path: Path, label: String) {
        if (!path.startsWith(runtimeRoot)) {
            throw SecurityException("$label escapes runtime root: $path")
        }
        require(Files.isDirectory(path)) { "$label is not a directory: $path" }
    }

    fun run(request: RuntimeProbeRequest, timeoutSeconds: Long = DEFAULT_TIMEOUT_SECONDS): RuntimeProbeResult {
        require(timeoutSeconds > 0) { "Probe timeout must be positive" }
        val process = processBuilder(request).start()
        val output = StringBuilder()
        val reader = Thread {
            readProcessOutput(process.inputStream, output)
        }.apply { start() }

        if (!process.waitFor(timeoutSeconds, TimeUnit.SECONDS)) {
            process.destroyForcibly()
            process.waitFor(KILL_GRACE_SECONDS, TimeUnit.SECONDS)
            process.inputStream.close()
            reader.join(READER_JOIN_MILLIS)
            throw IllegalStateException(
                buildString {
                    append("Runtime probe timed out after $timeoutSeconds seconds")
                    if (output.isNotEmpty()) append("\n").append(output)
                },
            )
        }
        reader.join()
        return RuntimeProbeResult(process.exitValue(), output.toString())
    }

    private companion object {
        const val BOX64_LIBRARY = "libbox64.so"
        const val HOST_DIRECTORY = "host"
        const val HOST_LOADER_LIBRARY = "libbachata_host_loader.so"
        const val HOST_BOX64_LIBRARY = "libbachata_host_box64.so"
        const val DEFAULT_TIMEOUT_SECONDS = 15L
        const val KILL_GRACE_SECONDS = 2L
        const val READER_JOIN_MILLIS = 1_000L
        val ALLOWED_ENVIRONMENT = setOf(
            "HOME",
            "LD_LIBRARY_PATH",
            "BOX64_PATH",
            "BOX64_LOG",
            "BOX64_LD_LIBRARY_PATH",
            "BOX64_EMULATED_LIBS",
            "BACHATA_ALSA_SOCKET",
            "DISPLAY",
            "GLIBC_TUNABLES",
            "SDL_VIDEODRIVER",
            "TMPDIR",
            "XDG_CACHE_HOME",
            "MESA_SHADER_CACHE_DIR",
            "VK_ICD_FILENAMES",
            "SDL_VULKAN_LIBRARY",
            "BACHATA_VORTEK_SOCKET",
            "BACHATA_VORTEK_HANDSHAKE",
            "BACHATA_VORTEK_LOG_LEVEL",
            "BACHATA_VORTEK_TRACE",
            "BACHATA_VORTEK_TRANSPORT_ONLY",
            "BACHATA_VORTEK_HEADLESS",
            "BACHATA_VORTEK_WSI",
            "BACHATA_VORTEK_WSI_SECONDS",
            "BACHATA_VORTEK_WSI_RESIZE",
            "BACHATA_VORTEK_SHAD",
            "BACHATA_VORTEK_FENCE",
            "BACHATA_VORTEK_TRACE_FENCES",
            "BACHATA_VORTEK_FENCE_WAIT_MODE",
            "BACHATA_VORTEK_FENCE_WAIT_FORCE_DEVICE_LOST",
        )
    }
}
