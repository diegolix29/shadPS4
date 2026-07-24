package com.bachatas4.android.feature.setup

import android.widget.ImageView
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.hilt.navigation.compose.hiltViewModel
import com.bachatas4.android.designsystem.BachataActionBar
import com.bachatas4.android.designsystem.BachataPanel
import com.bachatas4.android.designsystem.BachataPrimaryButton
import com.bachatas4.android.designsystem.ForwardFab
import com.bachatas4.android.designsystem.theme.BachataPalette

@Composable
fun SetupScreen(
    onContinue: () -> Unit,
    viewModel: SetupViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsState()
    SetupContent(
        state = state,
        downloadRuntimeEnabled = viewModel.downloadRuntime,
        onDownload = viewModel::downloadRuntime,
        onContinue = onContinue,
    )
}

@Composable
fun SetupContent(
    state: SetupUiState,
    downloadRuntimeEnabled: Boolean,
    onDownload: () -> Unit,
    onContinue: () -> Unit,
) {
    val readinessText = when (state.readiness) {
        SetupReadiness.Ready -> "Ready to enter your library"
        SetupReadiness.RuntimeRequired -> "Emulation runtime is required"
        SetupReadiness.IntegrityRequired -> "Runtime verification is required"
    }
    Scaffold(
        containerColor = BachataPalette.Canvas,
        bottomBar = { BachataActionBar("A  CONTINUE", "B  EXIT") },
        floatingActionButton = {
            if (state.canEnterLibrary) {
                ForwardFab(onClick = onContinue)
            }
        },
    ) { contentPadding ->
        Column(
            modifier = Modifier.fillMaxSize().padding(contentPadding).padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            AndroidView(
                modifier = Modifier.size(96.dp),
                factory = { viewContext ->
                    ImageView(viewContext).apply {
                        setImageResource(viewContext.applicationInfo.icon)
                        contentDescription = "Bachata S4 logo"
                    }
                },
            )
            Text(
                "BACHATA S4",
                modifier = Modifier.padding(top = 16.dp),
                style = MaterialTheme.typography.headlineLarge,
                color = BachataPalette.Primary,
            )
            Text(
                "A focused, controller-first game library.",
                modifier = Modifier.padding(top = 4.dp),
                color = BachataPalette.Secondary,
            )
            BachataPanel(
                modifier = Modifier.padding(top = 24.dp),
                color = BachataPalette.Surface,
            ) {
                Column(modifier = Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(readinessText, color = BachataPalette.Primary, style = MaterialTheme.typography.titleMedium)
                    Text("SoC: ${state.deviceProfile.soc}  •  GPU: ${state.deviceProfile.gpu}", color = BachataPalette.Secondary)
                    Text(state.legalNotice, color = Color(0xFFFFDDB5))
                }
            }
            if (downloadRuntimeEnabled && !state.runtimeInstalled) {
                BachataPrimaryButton(
                    onClick = onDownload,
                    enabled = !state.isDownloading,
                    modifier = Modifier.padding(top = 16.dp),
                ) {
                    Text(if (state.isDownloading) "Downloading…" else "Download emulation assets")
                }
            }
        }
    }
}
