package com.bachatas4.android.feature.session.diagnostics

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.bachatas4.android.designsystem.theme.BachataPalette
import com.bachatas4.android.runtime.diagnostics.DiagnosticCheckpoint
import com.bachatas4.android.runtime.diagnostics.DiagnosticReportContext

@Composable
fun DiagnosticReportSheet(
    state: DiagnosticReportUiState,
    onDescriptionChange: (String) -> Unit,
    onScreenshotToggle: (Boolean) -> Unit,
    onCreateAndShare: () -> Unit,
    onSaveLocally: () -> Unit,
    onCancel: () -> Unit,
    onRetry: () -> Unit,
) {
    val context = state.context ?: return
    // Fixed header actions stay visible; long report details scroll underneath.
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(max = 640.dp)
            .padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(
            text = "Diagnostic report",
            color = BachataPalette.Primary,
            fontWeight = FontWeight.Bold,
            style = androidx.compose.material3.MaterialTheme.typography.titleLarge,
        )
        Text(
            text = "Review what will be included. Nothing is sent until you share or save.",
            color = BachataPalette.Secondary,
            style = androidx.compose.material3.MaterialTheme.typography.bodyMedium,
        )

        when (state.phase) {
            DiagnosticReportPhase.PREPARING -> Text("Preparing report…", color = BachataPalette.Primary)
            DiagnosticReportPhase.FAILED -> Text(
                text = "Failed: ${state.errorMessage ?: "unknown error"}",
                color = androidx.compose.ui.graphics.Color(0xFFD32F2F),
            )
            DiagnosticReportPhase.READY, DiagnosticReportPhase.SHARING, DiagnosticReportPhase.SAVING -> {
                state.zipFile?.let {
                    Text("ZIP ready: ${it.name}", color = BachataPalette.Primary)
                }
            }
            else -> Unit
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            when (state.phase) {
                DiagnosticReportPhase.FAILED -> {
                    Button(onClick = onRetry) { Text("Retry") }
                    OutlinedButton(onClick = onCancel) { Text("Cancel") }
                }
                DiagnosticReportPhase.PREPARING -> {
                    OutlinedButton(onClick = onCancel, enabled = false) { Text("Cancel") }
                }
                else -> {
                    Button(
                        onClick = onCreateAndShare,
                        enabled = state.phase != DiagnosticReportPhase.PREPARING,
                    ) { Text("Create and share") }
                    OutlinedButton(
                        onClick = onSaveLocally,
                        enabled = state.phase != DiagnosticReportPhase.PREPARING,
                    ) { Text("Save locally") }
                    OutlinedButton(onClick = onCancel) { Text("Cancel") }
                }
            }
        }

        Spacer(Modifier.height(4.dp))

        Column(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f, fill = false)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            InfoRow("Report ID", context.reportId)
            InfoRow("App", "${context.app.versionName} (${context.app.versionCode})")
            InfoRow("Runtime", context.app.runtimeRevision)
            InfoRow("Distribution", context.app.distribution)
            InfoRow("Title", context.gameTitle)
            InfoRow("CUSA", context.cusaId)
            InfoRow(
                "Device",
                listOfNotNull(
                    context.device.manufacturer,
                    context.device.model,
                    "Android ${context.device.androidRelease}",
                    context.device.soc,
                ).joinToString(" · "),
            )
            InfoRow(
                "GPU",
                listOfNotNull(context.device.gpuVendor, context.device.gpuRenderer, context.device.gpuVersion)
                    .joinToString(" · ")
                    .ifBlank { "unavailable" },
            )
            InfoRow(
                "Driver",
                listOfNotNull(
                    context.driver.name,
                    context.driver.version,
                    context.driver.build,
                    context.driver.type,
                ).joinToString(" · "),
            )
            InfoRow("Backend", context.guestBackend)
            InfoRow(
                "Last stage",
                context.lastCheckpoint?.let { name ->
                    runCatching { DiagnosticCheckpoint.valueOf(name).displayLabel() }.getOrDefault(name)
                } ?: "unknown",
            )
            InfoRow("Process result", formatTermination(context))
            InfoRow("First frame", if (context.firstFrameReached) "yes" else "no")

            Spacer(Modifier.height(4.dp))
            Text("Included files", color = BachataPalette.Primary, fontWeight = FontWeight.SemiBold)
            state.attachments.forEach { attachment ->
                val status = when {
                    !attachment.present -> "missing"
                    attachment.truncated -> "truncated"
                    else -> attachment.byteSize?.let { "$it B" } ?: "ready"
                }
                Text(
                    text = "• ${attachment.filename} ($status)",
                    color = BachataPalette.Secondary,
                    style = androidx.compose.material3.MaterialTheme.typography.bodySmall,
                )
            }

            Spacer(Modifier.height(4.dp))
            Text(
                text = state.privacyNotice,
                color = BachataPalette.Secondary,
                style = androidx.compose.material3.MaterialTheme.typography.bodySmall,
            )

            OutlinedTextField(
                value = state.userDescription,
                onValueChange = onDescriptionChange,
                label = { Text("Optional description") },
                modifier = Modifier.fillMaxWidth(),
                minLines = 2,
                maxLines = 4,
            )

            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = "Include screenshot (off by default)",
                    color = BachataPalette.Primary,
                    modifier = Modifier.weight(1f),
                )
                Switch(
                    checked = state.includeScreenshot,
                    onCheckedChange = onScreenshotToggle,
                )
            }
            if (state.includeScreenshot) {
                Text(
                    text = "Screenshot capture is not available in this build preview; toggle records consent only.",
                    color = BachataPalette.Secondary,
                    style = androidx.compose.material3.MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Column {
        Text(label, color = BachataPalette.Secondary, style = androidx.compose.material3.MaterialTheme.typography.labelSmall)
        Text(value, color = BachataPalette.Primary, style = androidx.compose.material3.MaterialTheme.typography.bodyMedium)
    }
}

private fun formatTermination(context: DiagnosticReportContext): String {
    val t = context.termination
    val parts = mutableListOf<String>()
    parts += t.terminationKind.name.lowercase()
    t.exitCode?.let { parts += "exit $it" }
    if (t.signalNumber != null) {
        parts += "signal ${t.signalNumber}${t.signalName?.let { " ($it)" } ?: ""}"
    } else if (t.terminationKind.name != "SIGNALED") {
        parts += "signal n/a"
    }
    parts += "role ${t.processRole.name.lowercase()}"
    return parts.joinToString(" · ")
}
