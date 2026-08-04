package com.bachatas4.android

import android.content.Context
import androidx.core.content.FileProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.bachatas4.android.feature.session.diagnostics.DiagnosticShare
import com.bachatas4.android.runtime.diagnostics.DiagnosticAppInfo
import com.bachatas4.android.runtime.diagnostics.DiagnosticBundleBuilder
import com.bachatas4.android.runtime.diagnostics.DiagnosticCheckpoint
import com.bachatas4.android.runtime.diagnostics.DiagnosticDeviceInfo
import com.bachatas4.android.runtime.diagnostics.DiagnosticDriverInfo
import com.bachatas4.android.runtime.diagnostics.DiagnosticReportContext
import com.bachatas4.android.runtime.diagnostics.ProcessTerminationClassifier
import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class DiagnosticReportInstrumentedTest {
    @Test
    fun generateReportAndResolveRestrictedFileProviderUri() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val session = File(context.cacheDir, "diag-fixture-session").apply {
            deleteRecursively()
            mkdirs()
        }
        File(session, "application.log").writeText("fixture application log\n")
        File(session, "shadps4.log").writeText("fixture backend log\n")

        val contextModel = DiagnosticReportContext(
            reportId = "BS4-20260804-11223344",
            sessionDirectory = session.absolutePath,
            gameTitle = "Fixture",
            cusaId = "CUSA00000",
            guestBackend = "fex",
            lastCheckpoint = DiagnosticCheckpoint.BACKEND_LAUNCHED.name,
            firstFrameReached = false,
            userRequestedStop = false,
            termination = ProcessTerminationClassifier.fromJavaExitValue(133, false),
            driver = DiagnosticDriverInfo(type = "turnip", name = "fixture"),
            app = DiagnosticAppInfo(
                versionName = "test",
                versionCode = 1,
                packageName = context.packageName,
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

        val reportsDir = DiagnosticShare.reportsDirectoryPath(context)
        val result = DiagnosticBundleBuilder(reportsDir).build(
            DiagnosticBundleBuilder.BuildRequest(
                context = contextModel,
                includeScreenshot = false,
                appRoot = context.filesDir.parentFile ?: context.filesDir,
                packageName = context.packageName,
            ),
        )
        assertTrue(Files.isRegularFile(result.zipPath))
        assertFalse(result.report.privacy.screenshotIncluded)

        val uri = DiagnosticShare.contentUri(context, result.zipPath.toFile())
        assertEquals("content", uri.scheme)
        assertTrue(uri.authority == DiagnosticShare.authority(context.packageName))
        assertFalse(uri.toString().startsWith("file:"))

        // Restricted provider resolves only under diagnostic-reports/
        context.contentResolver.openInputStream(uri).use { stream ->
            assertTrue(stream != null && stream.readBytes().isNotEmpty())
        }
    }
}
