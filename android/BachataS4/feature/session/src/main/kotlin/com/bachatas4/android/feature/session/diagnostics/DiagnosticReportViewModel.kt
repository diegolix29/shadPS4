package com.bachatas4.android.feature.session.diagnostics

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.bachatas4.android.runtime.diagnostics.DiagnosticAttachment
import com.bachatas4.android.runtime.diagnostics.DiagnosticBundleBuilder
import com.bachatas4.android.runtime.diagnostics.DiagnosticReportContext
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import java.nio.file.Files
import java.nio.file.Paths
import javax.inject.Inject
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

@HiltViewModel
class DiagnosticReportViewModel @Inject constructor(
    @ApplicationContext private val context: Context,
) : ViewModel() {
    private val mutex = Mutex()
    private val mutableState = MutableStateFlow(DiagnosticReportUiState())
    val uiState: StateFlow<DiagnosticReportUiState> = mutableState

    fun openReview(reportContext: DiagnosticReportContext) {
        val attachments = provisionalAttachments(reportContext)
        mutableState.value = DiagnosticReportUiState(
            phase = DiagnosticReportPhase.REVIEW,
            context = reportContext,
            includeScreenshot = false,
            attachments = attachments,
        )
    }

    fun setIncludeScreenshot(include: Boolean) {
        mutableState.update { it.copy(includeScreenshot = include) }
    }

    fun setUserDescription(text: String) {
        mutableState.update { it.copy(userDescription = text.take(2000)) }
    }

    fun cancel() {
        mutableState.value = DiagnosticReportUiState(phase = DiagnosticReportPhase.CANCELLED)
    }

    fun dismiss() {
        mutableState.value = DiagnosticReportUiState()
    }

    fun createAndShare(onReady: (File) -> Unit) {
        generate { file ->
            mutableState.update { it.copy(phase = DiagnosticReportPhase.SHARING, zipFile = file) }
            onReady(file)
        }
    }

    fun createAndSave(onReady: (File) -> Unit) {
        generate { file ->
            mutableState.update { it.copy(phase = DiagnosticReportPhase.SAVING, zipFile = file) }
            onReady(file)
        }
    }

    private fun generate(onSuccess: (File) -> Unit) {
        val snapshot = mutableState.value
        val reportContext = snapshot.context ?: return
        if (snapshot.phase == DiagnosticReportPhase.PREPARING) return
        viewModelScope.launch {
            mutex.withLock {
                mutableState.update {
                    it.copy(phase = DiagnosticReportPhase.PREPARING, errorMessage = null)
                }
                val result = withContext(Dispatchers.IO) {
                    runCatching {
                        val reportsDir = DiagnosticShare.reportsDirectoryPath(context)
                        val builder = DiagnosticBundleBuilder(reportsDir)
                        val gameRoot = File(context.filesDir, "games")
                        builder.build(
                            DiagnosticBundleBuilder.BuildRequest(
                                context = reportContext,
                                includeScreenshot = snapshot.includeScreenshot,
                                screenshotPath = null,
                                userDescription = snapshot.userDescription,
                                appRoot = context.filesDir.parentFile ?: context.filesDir,
                                gameRoot = gameRoot.takeIf { it.isDirectory },
                                packageName = context.packageName,
                            ),
                        )
                    }
                }
                result.fold(
                    onSuccess = { built ->
                        val file = built.zipPath.toFile()
                        mutableState.update {
                            it.copy(
                                phase = DiagnosticReportPhase.READY,
                                zipFile = file,
                                attachments = built.report.attachments,
                                errorMessage = null,
                            )
                        }
                        onSuccess(file)
                    },
                    onFailure = { error ->
                        mutableState.update {
                            it.copy(
                                phase = DiagnosticReportPhase.FAILED,
                                errorMessage = error.message ?: error.javaClass.simpleName,
                            )
                        }
                    },
                )
            }
        }
    }

    private fun provisionalAttachments(reportContext: DiagnosticReportContext): List<DiagnosticAttachment> {
        val dir = Paths.get(reportContext.sessionDirectory)
        fun present(name: String) = Files.isRegularFile(dir.resolve(name))
        return listOf(
            DiagnosticAttachment("application.log", present("application.log"), true, "session.application"),
            DiagnosticAttachment("shadps4.log", present("shadps4.log"), true, "session.shadps4"),
            DiagnosticAttachment(
                "shadps4-internal.log",
                present("shadps4-internal.log"),
                false,
                "session.shadps4-internal",
            ),
            DiagnosticAttachment("runtime.log", present("runtime.log"), false, "session.runtime"),
            DiagnosticAttachment("session.json", present("session.json"), false, "session.metadata"),
            DiagnosticAttachment("process-exit.json", true, true, "process.exit"),
            DiagnosticAttachment("settings.json", reportContext.settingsJson != null, false, "session.settings"),
            DiagnosticAttachment("screenshot.webp", false, false, "session.screenshot"),
        )
    }
}
