package com.bachatas4.android.runtime.diagnostics

import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticModelsTest {
    private val json = Json { encodeDefaults = true; ignoreUnknownKeys = true }

    @Test
    fun schemaVersionIsOneAndSerializesUtcFields() {
        val report = DiagnosticReport(
            reportId = "BS4-20260804-AABBCCDD",
            createdAtUtc = "2026-08-04T13:45:10.481Z",
            app = DiagnosticAppInfo(
                versionName = "0.1.7",
                versionCode = 17,
                packageName = "com.bachatas4.android",
                distribution = "playstore",
                sourceCommit = "abc123",
                runtimeRevision = "runtime-1",
            ),
            game = DiagnosticGameInfo(title = "Demo", cusaId = "CUSA00000"),
            device = DiagnosticDeviceInfo(
                manufacturer = "Google",
                model = "Pixel",
                device = "pixel",
                androidRelease = "14",
                sdkInt = 34,
            ),
            driver = DiagnosticDriverInfo(type = "turnip", name = "Mesa Turnip", version = "26.3.0"),
            execution = DiagnosticExecutionInfo(
                guestBackend = "fex",
                lastCheckpoint = DiagnosticCheckpoint.BACKEND_LAUNCHED.name,
                terminationKind = TerminationKind.UNKNOWN.name.lowercase(),
                exitCode = 133,
                rawWaitStatus = null,
                signalNumber = null,
                signalName = null,
            ),
        )
        assertEquals(1, report.schemaVersion)
        val encoded = json.encodeToString(report)
        assertTrue("schemaVersion" in encoded)
        assertTrue("2026-08-04T13:45:10.481Z" in encoded)
        assertTrue("CUSA00000" in encoded)
        assertFalse("androidId" in encoded.lowercase())
        assertFalse("imei" in encoded.lowercase())
        val decoded = json.decodeFromString(DiagnosticReport.serializer(), encoded)
        assertNull(decoded.execution.signalName)
        assertEquals(133, decoded.execution.exitCode)
    }

    @Test
    fun checkpointNamesAreStable() {
        assertEquals("BACKEND_LAUNCHED", DiagnosticCheckpoint.BACKEND_LAUNCHED.name)
        assertEquals("FIRST_FRAME_PRESENTED", DiagnosticCheckpoint.FIRST_FRAME_PRESENTED.name)
        assertEquals(16, DiagnosticCheckpoint.entries.size)
    }
}
