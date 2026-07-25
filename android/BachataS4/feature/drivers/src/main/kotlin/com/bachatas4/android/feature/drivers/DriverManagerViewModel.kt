package com.bachatas4.android.feature.drivers

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.bachatas4.android.data.RuntimeProfileStore
import com.bachatas4.android.runtime.driver.InstalledDriver
import com.bachatas4.android.runtime.driver.TurnipReleaseAsset
import com.bachatas4.android.runtime.process.RuntimeVulkanDriverIds
import com.bachatas4.android.runtime.settings.ProfileScope
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class DriverManagerUiState(
    val installed: List<InstalledDriver> = emptyList(),
    val available: List<TurnipReleaseAsset> = emptyList(),
    val scope: ProfileScope = ProfileScope.Global,
    val selectedDriverId: String = RuntimeVulkanDriverIds.SYSTEM,
    val capabilities: DriverManagerCapabilities = DriverManagerCapabilities(
        remoteCatalogEnabled = false,
        importEnabled = false,
        deleteEnabled = false,
    ),
    val loading: Boolean = false,
    val downloadAsset: String? = null,
    val downloaded: Long = 0,
    val downloadTotal: Long = 0,
    val pendingDeleteId: String? = null,
    val error: String? = null,
)

@HiltViewModel
class DriverManagerViewModel @Inject constructor(
    private val backend: DriverManagerBackend,
    private val profiles: RuntimeProfileStore,
) : ViewModel() {
    private val mutableState = MutableStateFlow(DriverManagerUiState(capabilities = backend.capabilities()))
    val state: StateFlow<DriverManagerUiState> = mutableState

    init {
        selectScope(ProfileScope.Global)
        refresh(false)
    }

    fun selectScope(scope: ProfileScope) {
        viewModelScope.launch {
            val selected = profiles.load(scope).driverId ?: RuntimeVulkanDriverIds.SYSTEM
            mutableState.value = mutableState.value.copy(scope = scope, selectedDriverId = selected)
        }
    }

    fun refresh(force: Boolean = true) = work(loading = true) {
        val caps = backend.capabilities()
        val installed = backend.installed()
        val releases = if (caps.remoteCatalogEnabled) backend.releases(force) else emptyList()
        val scope = mutableState.value.scope
        // Prefer persisted profile so Play migration remaps stale Turnip ids even before selectScope settles.
        // system-vortek is synthetic opt-in and must never be remapped to Turnip.
        val selected = profiles.load(scope).driverId ?: RuntimeVulkanDriverIds.SYSTEM
        val knownSynthetic = RuntimeVulkanDriverIds.isSynthetic(selected)
        if (!knownSynthetic && installed.none { it.metadata.id == selected }) {
            val fallback = installed.firstOrNull()?.metadata?.id ?: RuntimeVulkanDriverIds.SYSTEM
            profiles.update(scope) {
                it.copy(driverId = fallback.takeUnless { value -> value == RuntimeVulkanDriverIds.SYSTEM })
            }
            mutableState.value = mutableState.value.copy(
                capabilities = caps,
                installed = installed,
                available = releases,
                selectedDriverId = fallback,
            )
        } else {
            mutableState.value = mutableState.value.copy(
                capabilities = caps,
                installed = installed,
                available = releases,
                selectedDriverId = selected,
            )
        }
    }

    fun download(asset: TurnipReleaseAsset) = work(downloadAsset = asset.name) {
        require(backend.capabilities().remoteCatalogEnabled) {
            "Remote driver downloads are not available in this build"
        }
        backend.download(asset) { copied, total ->
            mutableState.value = mutableState.value.copy(downloaded = copied, downloadTotal = total)
        }
        mutableState.value = mutableState.value.copy(installed = backend.installed())
    }

    fun importZip(bytes: ByteArray, assetName: String) = work(loading = true) {
        require(backend.capabilities().importEnabled) {
            "Driver ZIP import is not available in this build"
        }
        backend.importZip(bytes, assetName)
        mutableState.value = mutableState.value.copy(installed = backend.installed())
    }

    fun select(id: String) {
        require(
            RuntimeVulkanDriverIds.isSynthetic(id) || backend.installed().any { it.metadata.id == id },
        ) { "Driver is not installed" }
        viewModelScope.launch {
            // Persist null for legacy system default; persist system-vortek explicitly.
            val stored = when (id) {
                RuntimeVulkanDriverIds.SYSTEM -> null
                else -> id
            }
            profiles.update(mutableState.value.scope) { it.copy(driverId = stored) }
            mutableState.value = mutableState.value.copy(selectedDriverId = id)
        }
    }

    fun requestDelete(id: String) {
        if (!backend.capabilities().deleteEnabled) {
            mutableState.value = mutableState.value.copy(error = "The bundled Turnip driver cannot be removed")
            return
        }
        if (mutableState.value.selectedDriverId == id) {
            mutableState.value = mutableState.value.copy(pendingDeleteId = id)
        } else {
            delete(id)
        }
    }

    fun confirmDelete() {
        val id = mutableState.value.pendingDeleteId ?: return
        select(RuntimeVulkanDriverIds.SYSTEM)
        delete(id)
    }

    fun cancelDelete() {
        mutableState.value = mutableState.value.copy(pendingDeleteId = null)
    }

    private fun delete(id: String) = work(loading = true) {
        require(backend.capabilities().deleteEnabled) { "Driver deletion is not available in this build" }
        backend.remove(id)
        mutableState.value = mutableState.value.copy(installed = backend.installed(), pendingDeleteId = null)
    }

    private fun work(
        loading: Boolean = false,
        downloadAsset: String? = null,
        block: suspend () -> Unit,
    ) = viewModelScope.launch {
        mutableState.value = mutableState.value.copy(loading = loading, downloadAsset = downloadAsset, error = null)
        runCatching { withContext(Dispatchers.IO) { block() } }
            .onFailure { mutableState.value = mutableState.value.copy(error = it.message ?: it.javaClass.simpleName) }
        mutableState.value = mutableState.value.copy(loading = false, downloadAsset = null)
    }
}
