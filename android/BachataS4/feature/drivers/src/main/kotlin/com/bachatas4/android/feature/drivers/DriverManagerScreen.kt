package com.bachatas4.android.feature.drivers

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import com.bachatas4.android.designsystem.BachataActionBar
import com.bachatas4.android.designsystem.BachataPanel
import com.bachatas4.android.designsystem.BachataPrimaryButton
import com.bachatas4.android.designsystem.BachataScreenHeader
import com.bachatas4.android.designsystem.ForwardFab
import com.bachatas4.android.designsystem.theme.BachataPalette
import com.bachatas4.android.runtime.driver.TurnipReleaseClient
import com.bachatas4.android.runtime.process.RuntimeVulkanDriverIds
import com.bachatas4.android.runtime.settings.ProfileScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.InputStream

@Composable
fun DriverManagerScreen(
    scope: ProfileScope = ProfileScope.Global,
    onBack: () -> Unit,
    onContinue: (() -> Unit)? = null,
    standalone: Boolean = true,
    viewModel: DriverManagerViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsState()
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()

    LaunchedEffect(scope) {
        viewModel.selectScope(scope)
    }

    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        coroutineScope.launch {
            runCatching {
                withContext(Dispatchers.IO) {
                    context.contentResolver.openInputStream(uri)?.use {
                        val bytes = it.readLimited(TurnipReleaseClient.MAX_ASSET_BYTES.toInt() + 1)
                        require(bytes.size <= TurnipReleaseClient.MAX_ASSET_BYTES) { "Driver ZIP exceeds 32 MiB" }
                        bytes
                    }
                }
            }.onSuccess { bytes ->
                if (bytes != null) viewModel.importZip(bytes, uri.lastPathSegment ?: "imported.zip")
            }
        }
    }

    val content: LazyListScope.() -> Unit = {
        if (standalone) {
            item {
                BachataScreenHeader(
                    title = if (onContinue != null) "Select Turnip Driver" else "Turnip Drivers",
                    onBack = onBack,
                )
            }
        }

        item {
                BachataPanel(
                    modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
                    color = BachataPalette.RaisedSurface
                ) {
                    Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text(
                            text = "Turnip Drivers",
                            style = MaterialTheme.typography.titleMedium,
                            color = BachataPalette.Primary,
                            fontWeight = FontWeight.Bold
                        )
                        if (state.capabilities.remoteCatalogEnabled) {
                            Text(
                                text = "Trusted source: ${TurnipReleaseClient.REPOSITORY}",
                                style = MaterialTheme.typography.bodySmall,
                                color = BachataPalette.Secondary
                            )
                        }
                        state.capabilities.statusMessage?.let { message ->
                            Text(
                                text = message,
                                style = MaterialTheme.typography.bodySmall,
                                color = BachataPalette.Secondary
                            )
                        }
                        Text(
                            text = "Active Scope: ${if (state.scope is ProfileScope.Game) "Game (${(state.scope as ProfileScope.Game).gameId})" else "Global"}",
                            style = MaterialTheme.typography.labelSmall,
                            color = BachataPalette.Accent,
                            fontWeight = FontWeight.SemiBold
                        )
                        when (state.selectedDriverId) {
                            RuntimeVulkanDriverIds.SYSTEM -> Text(
                                text = "Selected Driver: System driver",
                                style = MaterialTheme.typography.bodySmall,
                                color = BachataPalette.Secondary
                            )
                            RuntimeVulkanDriverIds.SYSTEM_VORTEK -> Text(
                                text = "Selected Driver: System Driver (Vortek, Experimental)",
                                style = MaterialTheme.typography.bodySmall,
                                color = BachataPalette.Accent
                            )
                            else -> {
                                val label = state.installed.firstOrNull { it.metadata.id == state.selectedDriverId }
                                    ?.metadata?.displayName
                                    ?: state.selectedDriverId
                                Text(
                                    text = "Selected Driver: $label",
                                    style = MaterialTheme.typography.bodySmall,
                                    color = BachataPalette.Primary
                                )
                            }
                        }
                    }
                }
            }

            item {
                LazyRow(
                    modifier = Modifier.fillMaxWidth(),
                    contentPadding = PaddingValues(horizontal = 16.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    if (state.capabilities.remoteCatalogEnabled) {
                        item {
                            BachataPrimaryButton(
                                onClick = { viewModel.refresh() },
                                enabled = !state.loading
                            ) {
                                Text("Refresh Available")
                            }
                        }
                    }
                    if (state.capabilities.importEnabled) {
                        item {
                            BachataPrimaryButton(
                                onClick = { importer.launch(arrayOf("application/zip", "application/x-zip-compressed")) }
                            ) {
                                Text("Import ZIP")
                            }
                        }
                    }
                    if (state.selectedDriverId != RuntimeVulkanDriverIds.SYSTEM) {
                        item {
                            TextButton(
                                onClick = { viewModel.select(RuntimeVulkanDriverIds.SYSTEM) }
                            ) {
                                Text("Switch to System Driver")
                            }
                        }
                    }
                    if (state.selectedDriverId != RuntimeVulkanDriverIds.SYSTEM_VORTEK) {
                        item {
                            TextButton(
                                onClick = { viewModel.select(RuntimeVulkanDriverIds.SYSTEM_VORTEK) }
                            ) {
                                Text("Use Vortek (Experimental)")
                            }
                        }
                    }
                }
            }

            item {
                BachataPanel(
                    modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
                    color = BachataPalette.RaisedSurface
                ) {
                    Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text(
                            text = "System Driver (Vortek, Experimental)",
                            style = MaterialTheme.typography.titleSmall,
                            color = BachataPalette.Accent,
                            fontWeight = FontWeight.Bold,
                        )
                        Text(
                            text = "Uses the device’s Android Vulkan driver through the experimental " +
                                "Vortek compatibility layer. Compatibility varies by device and game. " +
                                "Opt-in only — never selected automatically.",
                            style = MaterialTheme.typography.bodySmall,
                            color = BachataPalette.Secondary,
                        )
                        if (state.selectedDriverId == RuntimeVulkanDriverIds.SYSTEM_VORTEK) {
                            Text(
                                text = "Currently selected",
                                style = MaterialTheme.typography.labelSmall,
                                color = BachataPalette.Accent,
                                fontWeight = FontWeight.SemiBold,
                            )
                        } else {
                            BachataPrimaryButton(
                                onClick = { viewModel.select(RuntimeVulkanDriverIds.SYSTEM_VORTEK) },
                                enabled = !state.loading,
                            ) {
                                Text("Select Vortek")
                            }
                        }
                    }
                }
            }

        state.error?.let { err ->
            item {
                BachataPanel(
                    modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
                    color = Color(0xFF3D2121)
                ) {
                    Text(
                        text = err,
                        modifier = Modifier.padding(12.dp),
                        color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
            }
        }

        state.downloadAsset?.let { asset ->
            item {
                val progress = if (state.downloadTotal > 0) state.downloaded.toFloat() / state.downloadTotal else 0f
                val percentage = (progress * 100).toInt()
                val downloadedStr = formatBytes(state.downloaded)
                val totalStr = formatBytes(state.downloadTotal)
                
                BachataPanel(
                    modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
                    color = BachataPalette.RaisedSurface
                ) {
                    Column(
                        modifier = Modifier.padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text(
                                text = "Downloading Driver",
                                style = MaterialTheme.typography.titleMedium,
                                color = BachataPalette.Primary,
                                fontWeight = FontWeight.Bold
                            )
                            Text(
                                text = "$percentage%",
                                style = MaterialTheme.typography.titleMedium,
                                color = BachataPalette.Accent,
                                fontWeight = FontWeight.Bold
                            )
                        }
                        Text(
                            text = asset,
                            style = MaterialTheme.typography.bodySmall,
                            color = BachataPalette.Secondary
                        )
                        androidx.compose.material3.LinearProgressIndicator(
                            progress = { progress },
                            modifier = Modifier.fillMaxWidth().height(4.dp),
                            color = BachataPalette.Accent,
                            trackColor = BachataPalette.Canvas.copy(alpha = 0.5f)
                        )
                        Text(
                            text = "$downloadedStr / $totalStr",
                            style = MaterialTheme.typography.labelSmall,
                            color = BachataPalette.Secondary,
                            modifier = Modifier.align(Alignment.End)
                        )
                    }
                }
            }
        }

        state.pendingDeleteId?.let { id ->
            item {
                BachataPanel(
                    modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
                    color = Color(0xFF3A2B18)
                ) {
                    Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(
                            text = "This driver is selected for the current scope. Switch to system and delete?",
                            color = Color(0xFFFFDDB5)
                        )
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            BachataPrimaryButton(onClick = viewModel::confirmDelete) { Text("Confirm") }
                            TextButton(onClick = viewModel::cancelDelete) { Text("Cancel") }
                        }
                    }
                }
            }
        }

        item {
            Text(
                text = "Installed Drivers",
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
                color = BachataPalette.Primary
            )
        }

        if (state.installed.isEmpty()) {
            item {
                Text(
                    text = "No custom drivers installed",
                    modifier = Modifier.padding(horizontal = 16.dp),
                    style = MaterialTheme.typography.bodyMedium,
                    color = BachataPalette.Secondary
                )
            }
        } else {
            items(state.installed, key = { it.metadata.id }) { driver ->
                val isSelected = state.selectedDriverId == driver.metadata.id
                BachataPanel(
                    modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
                    color = if (isSelected) BachataPalette.Surface else BachataPalette.RaisedSurface
                ) {
                    Column(
                        modifier = Modifier.padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text(
                                text = driver.metadata.displayName,
                                style = MaterialTheme.typography.titleMedium,
                                color = BachataPalette.Primary,
                                fontWeight = FontWeight.SemiBold
                            )
                            if (isSelected) {
                                Text(
                                    text = "Selected",
                                    color = BachataPalette.Accent,
                                    style = MaterialTheme.typography.labelMedium,
                                    fontWeight = FontWeight.Bold
                                )
                            }
                        }
                        Text(
                            text = "${driver.metadata.releaseTag ?: "Imported"} · ${driver.metadata.abi} · ${driver.metadata.sha256.take(12)}",
                            color = BachataPalette.Secondary,
                            style = MaterialTheme.typography.bodySmall
                        )
                        Text(
                            text = "Asset: ${driver.metadata.assetName}",
                            color = BachataPalette.Secondary,
                            style = MaterialTheme.typography.labelSmall
                        )
                        Row(
                            horizontalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            if (!isSelected) {
                                BachataPrimaryButton(
                                    onClick = { viewModel.select(driver.metadata.id) }
                                ) {
                                    Text("Select")
                                }
                            }
                            if (state.capabilities.deleteEnabled) {
                                TextButton(
                                    onClick = { viewModel.requestDelete(driver.metadata.id) }
                                ) {
                                    Text("Delete", color = MaterialTheme.colorScheme.error)
                                }
                            }
                        }
                    }
                }
            }
        }

        if (state.capabilities.remoteCatalogEnabled) {
            item {
                Text(
                    text = "Available Drivers",
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    color = BachataPalette.Primary
                )
            }

            if (state.available.isEmpty()) {
                item {
                    Text(
                        text = "No available release packages. Click Refresh to load.",
                        modifier = Modifier.padding(horizontal = 16.dp),
                        style = MaterialTheme.typography.bodyMedium,
                        color = BachataPalette.Secondary
                    )
                }
            } else {
                items(state.available, key = { it.downloadUrl }) { asset ->
                    BachataPanel(
                        modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
                        color = BachataPalette.RaisedSurface
                    ) {
                        Column(
                            modifier = Modifier.padding(16.dp),
                            verticalArrangement = Arrangement.spacedBy(6.dp)
                        ) {
                            Text(
                                text = asset.releaseTag,
                                style = MaterialTheme.typography.titleMedium,
                                color = BachataPalette.Primary,
                                fontWeight = FontWeight.SemiBold
                            )
                            Text(
                                text = "${asset.name} · ${(asset.size / 1024.0 / 1024.0).let { "%.2f".format(it) }} MiB",
                                color = BachataPalette.Secondary,
                                style = MaterialTheme.typography.bodySmall
                            )
                            Text(
                                text = "Published: ${asset.publishedAt}",
                                color = BachataPalette.Secondary,
                                style = MaterialTheme.typography.labelSmall
                            )
                            Spacer(modifier = Modifier.height(4.dp))
                            BachataPrimaryButton(
                                onClick = { viewModel.download(asset) },
                                enabled = state.downloadAsset == null
                            ) {
                                Text("Download & Install")
                            }
                        }
                    }
                }
            }
        }
    }

    if (standalone) {
        val actionBarHints = if (onContinue != null) {
            arrayOf("A  CONTINUE", "B  BACK")
        } else {
            arrayOf("B  BACK")
        }
        Scaffold(
            containerColor = BachataPalette.Canvas,
            bottomBar = { BachataActionBar(*actionBarHints) },
            floatingActionButton = {
                onContinue?.let { continueAction ->
                    ForwardFab(onClick = continueAction)
                }
            },
        ) { contentPadding ->
            LazyColumn(
                modifier = Modifier.fillMaxSize().padding(contentPadding),
                contentPadding = PaddingValues(bottom = 16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
                content = content,
            )
        }
    } else {
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
            content = content,
        )
    }
}

private fun InputStream.readLimited(limit: Int): ByteArray {
    val output = ByteArrayOutputStream()
    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
    while (output.size() < limit) {
        val count = read(buffer, 0, minOf(buffer.size, limit - output.size()))
        if (count < 0) break
        output.write(buffer, 0, count)
    }
    return output.toByteArray()
}

private fun formatBytes(bytes: Long): String {
    return when {
        bytes >= 1024 * 1024 -> "%.2f MB".format(bytes / (1024.0 * 1024.0))
        bytes >= 1024 -> "%.2f KB".format(bytes / 1024.0)
        else -> "$bytes B"
    }
}
