package com.bachatas4.android.feature.setup

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.bachatas4.android.model.DeviceProfile
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import javax.inject.Qualifier
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.Dispatchers
import kotlinx.serialization.json.Json

@Qualifier
@Retention(AnnotationRetention.BINARY)
annotation class DownloadRuntime

data class SetupUiState(
    val deviceProfile: DeviceProfile,
    val runtimeInstalled: Boolean,
    val integrityVerified: Boolean,
    val legalNotice: String,
    val isDownloading: Boolean = false,
) {
    val canEnterLibrary: Boolean
        get() = runtimeInstalled && integrityVerified

    val readiness: SetupReadiness
        get() = when {
            !runtimeInstalled -> SetupReadiness.RuntimeRequired
            !integrityVerified -> SetupReadiness.IntegrityRequired
            else -> SetupReadiness.Ready
        }
}

enum class SetupReadiness {
    Ready,
    RuntimeRequired,
    IntegrityRequired,
}

@HiltViewModel
class SetupViewModel @Inject constructor(
    @com.bachatas4.android.feature.setup.DownloadRuntime val downloadRuntime: Boolean,
    @dagger.hilt.android.qualifiers.ApplicationContext private val context: android.content.Context
) : ViewModel() {
    private val mutableState = MutableStateFlow(
        SetupUiState(
            deviceProfile = DeviceProfile(soc = "unknown", gpu = "unverified", supported = true),
            runtimeInstalled = false,
            integrityVerified = false,
            legalNotice = "Import only games and firmware content you legally own.",
        ),
    )

    val state: StateFlow<SetupUiState> = mutableState

    init {
        val soc = android.os.Build.SOC_MODEL.orEmpty().ifEmpty { "unknown" }
        // No SoC allowlist: report detected hardware but do not block setup.
        val profile = DeviceProfile(
            soc = soc,
            gpu = "unverified",
            supported = true,
        )
        updateDeviceProfile(profile)
        
        val runtimeRoot = java.io.File(context.filesDir, "runtime")
        val isInstalled = runtimeRoot.listFiles()?.any { it.isDirectory && it.name.startsWith("box64-") } == true
        if (isInstalled) {
            mutableState.value = mutableState.value.copy(
                runtimeInstalled = true,
                integrityVerified = true
            )
        } else if (!downloadRuntime) {
            extractRuntimeFromAssets()
        }
    }

    fun checkRuntimeStatus() {
        val runtimeRoot = java.io.File(context.filesDir, "runtime")
        val isInstalled = runtimeRoot.listFiles()?.any { it.isDirectory && it.name.startsWith("box64-") } == true
        mutableState.value = mutableState.value.copy(
            runtimeInstalled = isInstalled,
            integrityVerified = isInstalled
        )
    }

    fun updateDeviceProfile(profile: DeviceProfile) {
        mutableState.value = mutableState.value.copy(deviceProfile = profile)
    }

    fun extractRuntimeFromAssets() {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                mutableState.value = mutableState.value.copy(
                    legalNotice = "Extracting emulation assets..."
                )
                val manifest = context.assets.open("runtime/manifest.json").bufferedReader().use {
                    Json { ignoreUnknownKeys = true }.decodeFromString<com.bachatas4.android.runtime.install.RuntimeManifest>(it.readText())
                }
                val installRoot = java.io.File(context.filesDir, "runtime").toPath()
                context.assets.open("runtime/runtime.zip").use { bundle ->
                    com.bachatas4.android.runtime.install.RuntimeInstaller(installRoot)
                        .install(bundle, manifest)
                        .getOrThrow()
                }
                mutableState.value = mutableState.value.copy(
                    runtimeInstalled = true,
                    integrityVerified = true,
                    legalNotice = "Assets extracted successfully!"
                )
            } catch (e: Exception) {
                mutableState.value = mutableState.value.copy(
                    legalNotice = "Extraction failed: ${e.message}"
                )
            }
        }
    }

    fun downloadRuntime() {
        if (mutableState.value.isDownloading) return
        viewModelScope.launch(Dispatchers.IO) {
            try {
                mutableState.value = mutableState.value.copy(
                    isDownloading = true,
                    legalNotice = "Downloading runtime manifest..."
                )
                
                val manifestUrl = "https://github.com/JICA98/Bachata-S4-Runtimes/releases/download/v0.1.0/manifest.json"
                val manifestConnection = java.net.URL(manifestUrl).openConnection() as java.net.HttpURLConnection
                manifestConnection.connectTimeout = 15000
                manifestConnection.readTimeout = 15000
                val manifestText = manifestConnection.inputStream.bufferedReader().use { it.readText() }
                manifestConnection.disconnect()
                
                val manifest = Json { ignoreUnknownKeys = true }
                    .decodeFromString<com.bachatas4.android.runtime.install.RuntimeManifest>(manifestText)
                
                mutableState.value = mutableState.value.copy(
                    legalNotice = "Downloading runtime zip (0%)..."
                )
                
                val zipUrl = "https://github.com/JICA98/Bachata-S4-Runtimes/releases/download/v0.1.0/runtime.zip"
                val zipConnection = java.net.URL(zipUrl).openConnection() as java.net.HttpURLConnection
                zipConnection.connectTimeout = 15000
                zipConnection.readTimeout = 15000
                val contentLength = zipConnection.contentLengthLong
                val inputStream = zipConnection.inputStream
                
                val installRoot = java.io.File(context.filesDir, "runtime").toPath()
                
                val progressStream = object : java.io.FilterInputStream(inputStream) {
                    private var bytesRead = 0L
                    override fun read(): Int {
                        val byte = super.read()
                        if (byte >= 0) {
                            bytesRead++
                            updateProgress(bytesRead, contentLength)
                        }
                        return byte
                    }
                    override fun read(b: ByteArray, off: Int, len: Int): Int {
                        val count = super.read(b, off, len)
                        if (count >= 0) {
                            bytesRead += count
                            updateProgress(bytesRead, contentLength)
                        }
                        return count
                    }
                }
                
                com.bachatas4.android.runtime.install.RuntimeInstaller(installRoot)
                    .install(progressStream, manifest)
                    .getOrThrow()
                
                zipConnection.disconnect()
                
                mutableState.value = mutableState.value.copy(
                    isDownloading = false,
                    runtimeInstalled = true,
                    integrityVerified = true,
                    legalNotice = "Runtime installed successfully!"
                )
            } catch (e: Exception) {
                mutableState.value = mutableState.value.copy(
                    isDownloading = false,
                    legalNotice = "Download failed: ${e.message}"
                )
            }
        }
    }
    
    private fun updateProgress(bytesRead: Long, totalBytes: Long) {
        val percentage = if (totalBytes > 0) (bytesRead * 100 / totalBytes).toInt() else 0
        mutableState.value = mutableState.value.copy(
            legalNotice = "Downloading runtime zip ($percentage%)..."
        )
    }
}
