package com.bachatas4.android.runtime.diagnostics

import java.io.File
import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.security.MessageDigest
import java.util.zip.ZipFile
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticBundleBuilderTest {
    @Test
    fun buildsZipWithManifestHashesAndMissingOptionalLogs() {
        val session = Files.createTempDirectory("diag-session")
        val reports = Files.createTempDirectory("diag-reports")
        val appRoot = Files.createTempDirectory("diag-app").toFile()
        val gameRoot = File(appRoot, "games/CUSA00000").also { it.mkdirs() }
        Files.writeString(session.resolve("application.log"), "app ${appRoot.canonicalPath}/cache\n")
        Files.writeString(session.resolve("shadps4.log"), "backend ok path=${gameRoot.canonicalPath}/eboot.bin\n")
        // intentionally no shadps4-internal.log

        val context = sampleContext(session.toString(), exitCode = 133)
        val builder = DiagnosticBundleBuilder(reports)
        val result = builder.build(
            DiagnosticBundleBuilder.BuildRequest(
                context = context,
                includeScreenshot = false,
                appRoot = appRoot,
                gameRoot = gameRoot,
                packageName = "com.bachatas4.android",
            ),
        )
        assertTrue(Files.isRegularFile(result.zipPath))
        assertFalse(result.report.privacy.screenshotIncluded)
        assertEquals("unknown", result.report.execution.terminationKind)
        assertNullSafe(result.report.execution.signalName)

        ZipFile(result.zipPath.toFile()).use { zip ->
            val names = zip.entries().asSequence().map { it.name }.toSet()
            assertTrue("report.json" in names)
            assertTrue("manifest.json" in names)
            assertTrue("application.log" in names)
            assertTrue("shadps4.log" in names)
            assertTrue("process-exit.json" in names)
            assertFalse("screenshot.webp" in names)
            assertFalse(names.any { it.contains("..") || it.contains("/") })

            val appLog = zip.getInputStream(zip.getEntry("application.log")).readBytes().decodeToString()
            assertFalse(appRoot.canonicalPath in appLog)
            assertTrue("<APP_DATA>" in appLog)

            val shad = zip.getInputStream(zip.getEntry("shadps4.log")).readBytes().decodeToString()
            assertFalse(gameRoot.canonicalPath in shad)
            assertTrue("<GAME_ROOT>" in shad)

            // Manifest hashes match entry bytes
            for (entry in result.manifest.entries.filter { it.filename != "manifest.json" }) {
                val bytes = zip.getInputStream(zip.getEntry(entry.filename)).readBytes()
                assertEquals(entry.byteSize, bytes.size.toLong())
                assertEquals(entry.sha256, sha256(bytes))
            }
            assertTrue(result.report.attachments.any { it.filename == "shadps4-internal.log" && !it.present })
        }
    }

    @Test
    fun truncatesOversizedLogs() {
        val session = Files.createTempDirectory("diag-session-big")
        val reports = Files.createTempDirectory("diag-reports-big")
        val appRoot = Files.createTempDirectory("diag-app-big").toFile()
        val huge = "HEAD_MARKER\n" + ("x".repeat(1000) + "\n").repeat(6000) + "TAIL_MARKER\n"
        Files.writeString(session.resolve("application.log"), huge)
        Files.writeString(session.resolve("shadps4.log"), "small\n")
        val builder = DiagnosticBundleBuilder(reports, maxLogBytes = 8_000)
        val result = builder.build(
            DiagnosticBundleBuilder.BuildRequest(
                context = sampleContext(session.toString()),
                appRoot = appRoot,
            ),
        )
        ZipFile(result.zipPath.toFile()).use { zip ->
            val app = zip.getInputStream(zip.getEntry("application.log")).readBytes().decodeToString()
            assertTrue(app.contains("HEAD_MARKER") || app.contains("TAIL_MARKER"))
            assertTrue(app.contains("truncated") || app.length < huge.length)
            val entry = result.manifest.entries.first { it.filename == "application.log" }
            assertTrue(entry.truncated)
        }
    }

    @Test
    fun rejectsUnsafeEntryNames() {
        assertFalse(DiagnosticBundleBuilder.isSafeZipEntryName("../etc/passwd"))
        assertFalse(DiagnosticBundleBuilder.isSafeZipEntryName("a/b.log"))
        assertFalse(DiagnosticBundleBuilder.isSafeZipEntryName(""))
        assertTrue(DiagnosticBundleBuilder.isSafeZipEntryName("application.log"))
    }

    @Test
    fun retentionKeepsThreeNewestAndDeletesTmp() {
        val reports = Files.createTempDirectory("diag-retention")
        for (i in 1..5) {
            val f = reports.resolve("bachata-diagnostic-BS4-2026080$i-AAAAAAAA.zip")
            Files.writeString(f, "x".repeat(100))
            Thread.sleep(5)
        }
        Files.writeString(reports.resolve("orphan.zip.tmp"), "tmp")
        DiagnosticRetention.cleanup(reports, maxCount = 3, maxTotalBytes = 50L * 1024 * 1024)
        val zips = Files.list(reports).use { stream ->
            stream.filter { it.fileName.toString().endsWith(".zip") }.count()
        }
        assertEquals(3, zips)
        assertTrue(Files.notExists(reports.resolve("orphan.zip.tmp")))
    }

    private fun sampleContext(sessionDir: String, exitCode: Int? = 133): DiagnosticReportContext {
        val termination = ProcessTerminationClassifier.fromJavaExitValue(exitCode, userRequestedStop = false)
        return DiagnosticReportContext(
            reportId = "BS4-20260804-AABBCCDD",
            sessionDirectory = sessionDir,
            gameTitle = "Demo Title",
            cusaId = "CUSA00000",
            guestBackend = "fex",
            lastCheckpoint = DiagnosticCheckpoint.BACKEND_LAUNCHED.name,
            firstFrameReached = false,
            userRequestedStop = false,
            termination = termination,
            driver = DiagnosticDriverInfo(type = "turnip", name = "Mesa Turnip", version = "26.3.0"),
            app = DiagnosticAppInfo(
                versionName = "0.1.7",
                versionCode = 17,
                packageName = "com.bachatas4.android",
                distribution = "playstore",
            ),
            device = DiagnosticDeviceInfo(
                manufacturer = "Test",
                model = "Device",
                device = "dev",
                androidRelease = "14",
                sdkInt = 34,
            ),
        )
    }

    private fun assertNullSafe(value: String?) {
        assertEquals(null, value)
    }

    private fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
}
