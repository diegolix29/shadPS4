package com.bachatas4.android.feature.drivers

import com.bachatas4.android.data.RuntimeProfileStore
import com.bachatas4.android.runtime.driver.DriverAbi
import com.bachatas4.android.runtime.driver.InstalledDriver
import com.bachatas4.android.runtime.driver.InstalledDriverMetadata
import com.bachatas4.android.runtime.driver.TurnipReleaseAsset
import com.bachatas4.android.runtime.process.VulkanDriverConfiguration
import com.bachatas4.android.runtime.settings.ProfileScope
import java.nio.file.Path
import java.nio.file.Paths
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class DriverManagerViewModelTest {
    @get:Rule val temporaryFolder = TemporaryFolder()
    private val dispatcher = UnconfinedTestDispatcher()

    @Before fun setUp() { Dispatchers.setMain(dispatcher) }
    @After fun tearDown() { Dispatchers.resetMain() }

    @Test
    fun loadsTrustedReleasesAndSelectsInstalledDriver() = runBlocking {
        val backend = FakeBackend(remote = true)
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = DriverManagerViewModel(backend, store)
        withTimeout(2_000) { viewModel.state.first { it.available.isNotEmpty() } }
        viewModel.select(backend.driver.metadata.id)
        withTimeout(2_000) { viewModel.state.first { it.selectedDriverId == backend.driver.metadata.id } }
        assertEquals(backend.driver.metadata.id, store.load(ProfileScope.Global).driverId)
    }

    @Test
    fun playBackendHidesRemoteCatalogAndRejectsDownload() = runBlocking {
        val backend = FakeBackend(remote = false)
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = DriverManagerViewModel(backend, store)
        withTimeout(2_000) {
            viewModel.state.first {
                it.installed.isNotEmpty() && !it.capabilities.remoteCatalogEnabled && it.available.isEmpty()
            }
        }
        viewModel.download(TurnipReleaseAsset("v1", "today", "x-EMULATOR.zip", 1, "https://example.com/x.zip"))
        withTimeout(2_000) { viewModel.state.first { it.error != null } }
        assertTrue(viewModel.state.value.error!!.contains("not available"))
    }

    @Test
    fun playBackendRemapsStaleDriverSelection() = runBlocking {
        val backend = FakeBackend(remote = false)
        val store = RuntimeProfileStore(temporaryFolder.root)
        store.update(ProfileScope.Global) { it.copy(driverId = "turnip-deadbeefdeadbeef") }
        val viewModel = DriverManagerViewModel(backend, store)
        withTimeout(2_000) {
            viewModel.state.first {
                it.selectedDriverId == backend.driver.metadata.id
            }
        }
        assertEquals(backend.driver.metadata.id, store.load(ProfileScope.Global).driverId)
    }

    private class FakeBackend(private val remote: Boolean) : DriverManagerBackend {
        val driver = InstalledDriver(
            InstalledDriverMetadata(
                id = "turnip-0123456789abcdef",
                displayName = if (remote) "Turnip test" else "Bundled Turnip 26.1.0",
                assetName = "test.zip",
                sha256 = "0".repeat(64),
                installedAtMs = 1,
                abi = DriverAbi.ANDROID_BIONIC,
                libraryRelativePath = "vulkan.turnip.so",
            ),
            Paths.get("/tmp/turnip-0123456789abcdef"),
        )
        private val asset = TurnipReleaseAsset(
            "v1",
            "today",
            "Turnip-EMULATOR.zip",
            1,
            "https://github.com/JICA98/bachata-s4-drivers/releases/download/v1/Turnip-EMULATOR.zip",
        )

        override fun capabilities() = DriverManagerCapabilities(
            remoteCatalogEnabled = remote,
            importEnabled = remote,
            deleteEnabled = remote,
            statusMessage = if (remote) null else "Bundled only",
        )

        override fun installed() = listOf(driver)
        override fun releases(force: Boolean) = if (remote) listOf(asset) else emptyList()
        override fun download(asset: TurnipReleaseAsset, progress: (Long, Long) -> Unit) =
            if (remote) driver else error("Remote driver downloads are not available in this build")

        override fun importZip(bytes: ByteArray, assetName: String) =
            if (remote) driver else error("Driver ZIP import is not available in this build")

        override fun remove(id: String) = remote
        override fun configurationFor(
            driverId: String,
            context: com.bachatas4.android.runtime.process.VulkanDriverResolveContext,
        ): VulkanDriverConfiguration {
            error("not used in tests")
        }
    }
}
