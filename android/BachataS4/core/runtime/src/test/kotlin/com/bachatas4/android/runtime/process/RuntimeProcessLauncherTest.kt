package com.bachatas4.android.runtime.process

import com.bachatas4.android.runtime.settings.RuntimeGuestBackend
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.TimeUnit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class RuntimeProcessLauncherTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun buildsNativeFexCommandWithoutBox64() {
        val base = validRequest()
        val fexShadPs4 = Files.write(
            base.runtimeRoot.resolve("host/shadps4-arm64-fex"),
            byteArrayOf(3),
        )
        val request = base.copy(
            shadPs4Executable = fexShadPs4,
            guestBackend = RuntimeGuestBackend.FEX,
        )
        val launcher = RuntimeProcessLauncher { FakeProcessHandle() }

        assertEquals(
            listOf(
                request.nativeLibraryDir.resolve("libbachata_host_loader.so").toRealPath().toString(),
                "--library-path",
                request.runtimeRoot.resolve("host").toRealPath().toString(),
                fexShadPs4.toRealPath().toString(),
                "--override-root",
                request.overrideRoot.toRealPath().toString(),
                "--bachata-storage-root",
                request.storageRoot.toRealPath().toString(),
                "--bachata-socket",
                request.socketPath,
            ),
            launcher.command(request),
        )
    }

    @Test
    fun buildsExactShellFreeCommand() {
        val request = validRequest()
        val launcher = RuntimeProcessLauncher { FakeProcessHandle() }

        assertEquals(
            listOf(
                request.nativeLibraryDir.resolve("libbachata_host_loader.so").toRealPath().toString(),
                "--library-path",
                request.runtimeRoot.resolve("host").toRealPath().toString(),
                request.nativeLibraryDir.resolve("libbachata_host_box64.so").toRealPath().toString(),
                request.shadPs4Executable.toRealPath().toString(),
                "--override-root",
                request.overrideRoot.toRealPath().toString(),
                "--bachata-storage-root",
                request.storageRoot.toRealPath().toString(),
                "--bachata-socket",
                request.socketPath,
            ),
            launcher.command(request),
        )
    }

    @Test
    fun buildsApkNativeBox64CommandForAndroidDriver() {
        val request = validRequest().copy(box64Mode = Box64Mode.APK_NATIVE)
        val launcher = RuntimeProcessLauncher { FakeProcessHandle() }

        assertEquals(
            listOf(
                request.nativeLibraryDir.resolve("libbox64.so").toRealPath().toString(),
                request.shadPs4Executable.toRealPath().toString(),
                "--override-root",
                request.overrideRoot.toRealPath().toString(),
                "--bachata-storage-root",
                request.storageRoot.toRealPath().toString(),
                "--bachata-socket",
                request.socketPath,
            ),
            launcher.command(request),
        )
    }

    @Test
    fun clearsInheritedEnvironmentAndCopiesOnlyAllowlist() {
        val request = validRequest(
            environment = mapOf(
                "HOME" to "/data/home",
                "DISPLAY" to ":0",
                "VK_ICD_FILENAMES" to "/data/turnip.json",
                "BOX64_DYNAREC_CALLRET" to "1",
                "BACHATA_VORTEK_SOCKET" to "/data/user/0/com.bachatas4.android/files/vs/a.sock",
                "BACHATA_VORTEK_HANDSHAKE" to "1",
                "BACHATA_VORTEK_LOG_LEVEL" to "1",
                "BACHATA_VORTEK_TRACE" to "0",
                "PATH" to "/system/bin",
                "LD_PRELOAD" to "/evil.so",
            ),
        )
        var captured: ProcessBuilder? = null
        val launcher = RuntimeProcessLauncher { builder ->
            captured = builder
            FakeProcessHandle()
        }

        launcher.launch(request)

        assertEquals(
            mapOf(
                "HOME" to "/data/home",
                "DISPLAY" to ":0",
                "VK_ICD_FILENAMES" to "/data/turnip.json",
                "BOX64_DYNAREC_CALLRET" to "1",
                "BACHATA_VORTEK_SOCKET" to "/data/user/0/com.bachatas4.android/files/vs/a.sock",
                "BACHATA_VORTEK_HANDSHAKE" to "1",
                "BACHATA_VORTEK_LOG_LEVEL" to "1",
                "BACHATA_VORTEK_TRACE" to "0",
            ),
            captured!!.environment(),
        )
        assertFalse(captured!!.environment().containsKey("PATH"))
        assertFalse(captured!!.environment().containsKey("LD_PRELOAD"))
    }

    @Test
    fun allowsEveryValidatedBox64Variable() {
        val request = validRequest(
            environment = mapOf(
                "BOX64_DYNAREC_BIGBLOCK" to "3",
                "BOX64_DYNAREC_FASTNAN" to "0",
            ),
        )
        var captured: ProcessBuilder? = null
        val launcher = RuntimeProcessLauncher { builder ->
            captured = builder
            FakeProcessHandle()
        }

        launcher.launch(request)

        assertEquals("3", captured!!.environment()["BOX64_DYNAREC_BIGBLOCK"])
        assertEquals("0", captured!!.environment()["BOX64_DYNAREC_FASTNAN"])
    }

    @Test
    fun redirectsChildStreamsSoFullPipesCannotBlockRuntime() {
        val request = validRequest()
        var captured: ProcessBuilder? = null
        val launcher = RuntimeProcessLauncher { builder ->
            captured = builder
            FakeProcessHandle()
        }

        launcher.launch(request)

        assertEquals(ProcessBuilder.Redirect.Type.WRITE, captured!!.redirectOutput().type())
        assertEquals(ProcessBuilder.Redirect.Type.WRITE, captured!!.redirectError().type())
        assertEquals("/dev/null", captured!!.redirectOutput().file().path)
        assertEquals("/dev/null", captured!!.redirectError().file().path)
    }

    @Test
    fun allowsGameOverrideInsideSeparateStorageRoot() {
        val request = validRequest()
        val gameRoot = Files.createDirectories(request.storageRoot.resolve("games/CUSA00001"))
        val launcher = RuntimeProcessLauncher { FakeProcessHandle() }

        val command = launcher.command(request.copy(overrideRoot = gameRoot))

        assertEquals(gameRoot.toRealPath().toString(), command[6])
    }

    @Test
    fun rejectsBox64WhoseCanonicalParentEscapesNativeLibraryDir() {
        val request = validRequest()
        val outside = temporaryFolder.newFile("outside-box64").toPath()
        outside.toFile().setExecutable(true)
        Files.delete(request.nativeLibraryDir.resolve("libbachata_host_box64.so"))
        Files.createSymbolicLink(request.nativeLibraryDir.resolve("libbachata_host_box64.so"), outside)
        val launcher = RuntimeProcessLauncher { FakeProcessHandle() }

        assertThrows(SecurityException::class.java) { launcher.command(request) }
    }

    @Test
    fun rejectsRuntimeExecutableOutsideRuntimeRoot() {
        val request = validRequest()
        val outside = temporaryFolder.newFile("outside-shadps4").toPath()
        val launcher = RuntimeProcessLauncher { FakeProcessHandle() }

        assertThrows(SecurityException::class.java) {
            launcher.command(request.copy(shadPs4Executable = outside))
        }
    }

    @Test
    fun requiresApkHostJniLibsForHostGlibcMode() {
        val request = validRequest(includeApkHostLibs = false)
        val launcher = RuntimeProcessLauncher { FakeProcessHandle() }
        assertThrows(Exception::class.java) { launcher.command(request) }
    }

    private fun validRequest(
        environment: Map<String, String> = emptyMap(),
        includeApkHostLibs: Boolean = true,
    ): RuntimeProcessRequest {
        val base = temporaryFolder.newFolder().toPath()
        val nativeLibraryDir = Files.createDirectories(base.resolve("apk/lib"))
        if (includeApkHostLibs) {
            Files.write(nativeLibraryDir.resolve("libbachata_host_loader.so"), byteArrayOf(1))
            Files.write(nativeLibraryDir.resolve("libbachata_host_box64.so"), byteArrayOf(1))
        }
        Files.write(nativeLibraryDir.resolve("libbox64.so"), byteArrayOf(1)).toFile().setExecutable(true)
        val runtimeRoot = Files.createDirectories(base.resolve("runtime"))
        Files.createDirectories(runtimeRoot.resolve("host"))
        val shadPs4 = Files.write(runtimeRoot.resolve("bin/shadps4").also {
            Files.createDirectories(it.parent)
        }, byteArrayOf(2))
        return RuntimeProcessRequest(
            nativeLibraryDir = nativeLibraryDir,
            runtimeRoot = runtimeRoot,
            overrideRoot = base,
            shadPs4Executable = shadPs4,
            socketPath = base.resolve("runtime.sock").toString(),
            environment = environment,
            box64Mode = Box64Mode.HOST_GLIBC,
            guestBackend = RuntimeGuestBackend.BOX64,
        )
    }
}

private class FakeProcessHandle : RuntimeProcessHandle {
    override val isAlive: Boolean = true
    override val exitCode: Int? = null
    override fun destroy() = Unit
    override fun destroyForcibly() = Unit
    override fun waitFor(timeout: Long, unit: TimeUnit): Boolean = false
}
