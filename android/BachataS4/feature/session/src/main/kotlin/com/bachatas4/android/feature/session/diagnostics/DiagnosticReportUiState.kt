package com.bachatas4.android.feature.session.diagnostics

import com.bachatas4.android.runtime.diagnostics.DiagnosticAttachment
import com.bachatas4.android.runtime.diagnostics.DiagnosticReportContext
import java.io.File

enum class DiagnosticReportPhase {
    IDLE,
    REVIEW,
    PREPARING,
    READY,
    SHARING,
    SAVING,
    FAILED,
    CANCELLED,
}

data class DiagnosticReportUiState(
    val phase: DiagnosticReportPhase = DiagnosticReportPhase.IDLE,
    val context: DiagnosticReportContext? = null,
    val includeScreenshot: Boolean = false,
    val userDescription: String = "",
    val attachments: List<DiagnosticAttachment> = emptyList(),
    val zipFile: File? = null,
    val errorMessage: String? = null,
    val privacyNotice: String = PRIVACY_NOTICE,
) {
    companion object {
        const val PRIVACY_NOTICE =
            "Diagnostic reports are optional and created only when you choose. " +
                "Release 1 never uploads a report automatically. " +
                "You pick the destination app through Android Sharesheet or Save. " +
                "Reports include device model, Android version, selected graphics driver, " +
                "game title/CUSA ID, startup checkpoints, process exit details, and app-owned logs. " +
                "Technical paths and tokens are redacted where possible, but logs may still contain " +
                "sensitive technical details. Screenshots are included only if you enable the toggle."
    }
}
