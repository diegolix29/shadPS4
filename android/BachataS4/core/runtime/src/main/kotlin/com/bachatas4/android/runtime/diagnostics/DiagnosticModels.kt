package com.bachatas4.android.runtime.diagnostics

import kotlinx.serialization.Serializable

/** Versioned diagnostic report schema (Release 1). */
@Serializable
data class DiagnosticReport(
    val schemaVersion: Int = SCHEMA_VERSION,
    val reportId: String,
    val createdAtUtc: String,
    val app: DiagnosticAppInfo,
    val game: DiagnosticGameInfo,
    val device: DiagnosticDeviceInfo,
    val driver: DiagnosticDriverInfo,
    val execution: DiagnosticExecutionInfo,
    val attachments: List<DiagnosticAttachment> = emptyList(),
    val privacy: DiagnosticPrivacyInfo = DiagnosticPrivacyInfo(),
    val userDescription: String? = null,
) {
    companion object {
        const val SCHEMA_VERSION = 1
    }
}

@Serializable
data class DiagnosticAppInfo(
    val versionName: String,
    val versionCode: Long,
    val packageName: String,
    val distribution: String,
    val sourceCommit: String = "unknown",
    val runtimeRevision: String = "unknown",
    val debuggable: Boolean = false,
)

@Serializable
data class DiagnosticGameInfo(
    val title: String,
    val cusaId: String,
    val baseVersion: String? = null,
    val updateVersion: String? = null,
    val contentLabel: String? = null,
)

@Serializable
data class DiagnosticDeviceInfo(
    val manufacturer: String,
    val model: String,
    val device: String,
    val androidRelease: String,
    val sdkInt: Int,
    val supportedAbis: List<String> = emptyList(),
    val soc: String? = null,
    val gpuRenderer: String? = null,
    val gpuVendor: String? = null,
    val gpuVersion: String? = null,
    val ramTotalMb: Long? = null,
)

@Serializable
data class DiagnosticDriverInfo(
    val type: String,
    val name: String,
    val version: String? = null,
    val build: String? = null,
    val source: String? = null,
    val packageSha256: String? = null,
    val profileId: String? = null,
)

@Serializable
data class DiagnosticExecutionInfo(
    val guestBackend: String,
    val lastCheckpoint: String? = null,
    val firstFrameReached: Boolean = false,
    val userRequestedStop: Boolean = false,
    val failedProcess: String? = null,
    val terminationKind: String,
    val exitCode: Int? = null,
    val rawWaitStatus: Int? = null,
    val signalNumber: Int? = null,
    val signalName: String? = null,
    val coreDumped: Boolean? = null,
    val runtimeErrorCode: String? = null,
    val processStartUtc: String? = null,
    val processEndUtc: String? = null,
)

@Serializable
data class DiagnosticAttachment(
    val filename: String,
    val present: Boolean,
    val required: Boolean = false,
    val logicalSource: String,
    val byteSize: Long? = null,
    val truncated: Boolean = false,
)

@Serializable
data class DiagnosticPrivacyInfo(
    val redactionVersion: Int = DiagnosticRedactor.REDACTION_VERSION,
    val screenshotIncluded: Boolean = false,
    val automaticUpload: Boolean = false,
)

@Serializable
data class DiagnosticManifest(
    val schemaVersion: Int = 1,
    val reportId: String,
    val createdAtUtc: String,
    val redactionVersion: Int = DiagnosticRedactor.REDACTION_VERSION,
    val entries: List<DiagnosticManifestEntry>,
)

@Serializable
data class DiagnosticManifestEntry(
    val filename: String,
    val byteSize: Long,
    val sha256: String,
    val truncated: Boolean = false,
    val logicalSource: String,
    val redactionVersion: Int = DiagnosticRedactor.REDACTION_VERSION,
)

/**
 * Lightweight context retained after a session ends so the UI can offer a report
 * without holding log contents in Compose state.
 */
@Serializable
data class DiagnosticReportContext(
    val reportId: String,
    val sessionDirectory: String,
    val gameTitle: String,
    val cusaId: String,
    val guestBackend: String,
    val lastCheckpoint: String?,
    val firstFrameReached: Boolean,
    val userRequestedStop: Boolean,
    val termination: ProcessTerminationInfo,
    val driver: DiagnosticDriverInfo,
    val app: DiagnosticAppInfo,
    val device: DiagnosticDeviceInfo,
    val baseVersion: String? = null,
    val updateVersion: String? = null,
    val processStartUtc: String? = null,
    val processEndUtc: String? = null,
    val runtimeErrorCode: String? = null,
    val settingsJson: String? = null,
)

@Serializable
enum class DiagnosticCheckpoint {
    SESSION_CREATED,
    SESSION_DIRECTORY_READY,
    RUNTIME_VERIFIED,
    DRIVER_SELECTED,
    DRIVER_LOADED,
    DISPLAY_READY,
    VORTEK_READY,
    BACKEND_LAUNCHED,
    CONTROL_SOCKET_CONNECTED,
    SHADPS4_RUNNING,
    PS4_EXECUTABLE_LOADED,
    GRAPHICS_INITIALIZED,
    FIRST_FRAME_SUBMITTED,
    FIRST_FRAME_PRESENTED,
    SESSION_STOPPING,
    SESSION_FINISHED,
    ;

    fun displayLabel(): String = name.lowercase().split('_').joinToString(" ") {
        it.replaceFirstChar { c -> c.titlecase() }
    }
}

@Serializable
enum class ProcessRole {
    BACKEND,
    FEX,
    BOX64,
    VORTEK,
    UNKNOWN,
}

@Serializable
enum class TerminationKind {
    EXITED,
    SIGNALED,
    LAUNCH_FAILED,
    CANCELLED_BY_USER,
    TIMEOUT,
    UNKNOWN,
}

@Serializable
data class ProcessTerminationInfo(
    val processRole: ProcessRole = ProcessRole.BACKEND,
    val terminationKind: TerminationKind = TerminationKind.UNKNOWN,
    val exitCode: Int? = null,
    val rawWaitStatus: Int? = null,
    val signalNumber: Int? = null,
    val signalName: String? = null,
    val coreDumped: Boolean? = null,
    val userRequestedStop: Boolean = false,
    val runtimeErrorCode: String? = null,
    val processStartUtc: String? = null,
    val processEndUtc: String? = null,
    val pid: Int? = null,
)
