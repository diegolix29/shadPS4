package com.bachatas4.android

import com.bachatas4.android.data.RuntimeProfileStore
import com.bachatas4.android.feature.drivers.DriverManagerBackend
import com.bachatas4.android.feature.drivers.DriverManagerCapabilities
import com.bachatas4.android.runtime.driver.InstalledDriver
import com.bachatas4.android.runtime.driver.TurnipReleaseAsset
import com.bachatas4.android.runtime.process.RuntimeVulkanDriver
import com.bachatas4.android.runtime.process.RuntimeVulkanDriverIds
import com.bachatas4.android.runtime.process.VulkanDriverConfiguration
import com.bachatas4.android.runtime.settings.CompatibilityConstraint
import com.bachatas4.android.runtime.settings.Box64Preset
import com.bachatas4.android.runtime.settings.ProfileScope
import com.bachatas4.android.runtime.settings.RuntimeSettingCatalog
import com.bachatas4.android.runtime.settings.RuntimeGuestBackend
import com.bachatas4.android.runtime.settings.RuntimeSettingSpec
import com.bachatas4.android.runtime.settings.SettingKind
import com.bachatas4.android.runtime.settings.ValueSource
import java.nio.file.Path
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class RuntimeLaunchProfileProviderTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    private val devKit = spec("general.dev_kit_mode", "General.dev_kit_mode", SettingKind.BOOLEAN, JsonPrimitive(false))
    private val gpuCopy = spec("gpu.copy_gpu_buffers", "GPU.copy_gpu_buffers", SettingKind.BOOLEAN, JsonPrimitive(false))
    private val boxLog = spec("box64.log", "BOX64_LOG", SettingKind.ENUM, JsonPrimitive("0"), listOf("0", "1", "2"))
    private val catalog = RuntimeSettingCatalog(listOf(devKit, gpuCopy), listOf(boxLog))
    private val strictBackend = object : DriverManagerBackend {
        override fun capabilities() = DriverManagerCapabilities(false, false, false)
        override fun installed(): List<InstalledDriver> = emptyList()
        override fun releases(force: Boolean): List<TurnipReleaseAsset> = emptyList()
        override fun download(asset: TurnipReleaseAsset, progress: (Long, Long) -> Unit) = error("no")
        override fun importZip(bytes: ByteArray, assetName: String) = error("no")
        override fun remove(id: String) = false
        override fun configurationFor(
            driverId: String,
            context: com.bachatas4.android.runtime.process.VulkanDriverResolveContext,
        ): VulkanDriverConfiguration {
            if (driverId == "system" || driverId == RuntimeVulkanDriverIds.SYSTEM) {
                return VulkanDriverConfiguration.resolve(RuntimeVulkanDriver.SYSTEM, context)
            }
            if (driverId == RuntimeVulkanDriverIds.SYSTEM_VORTEK) {
                return VulkanDriverConfiguration.resolve(RuntimeVulkanDriver.SYSTEM_VORTEK, context)
            }
            throw IllegalStateException("Selected Vulkan driver '$driverId' is not installed")
        }
    }

    @Test
    fun fexIsTheDefaultForEveryGame() = runTest {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val provider = RuntimeLaunchProfileProvider(store, catalog, emptyMap(), strictBackend)

        assertEquals(RuntimeGuestBackend.FEX, provider.resolve("CUSA07023").guestBackend)
        assertEquals(RuntimeGuestBackend.FEX, provider.resolve("CUSA00900").guestBackend)
    }

    @Test
    fun fexForcesGpuCommandCopies() = runTest {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val provider = RuntimeLaunchProfileProvider(store, catalog, emptyMap(), strictBackend)

        val sonic = provider.resolve("CUSA07023")

        assertEquals(JsonPrimitive(true), sonic.settings.getValue(gpuCopy.id).value)
        assertEquals(ValueSource.COMPATIBILITY, sonic.settings.getValue(gpuCopy.id).source)
    }

    @Test
    fun explicitBox64DoesNotReceiveFexCompatibilityConstraints() = runTest {
        val store = RuntimeProfileStore(temporaryFolder.root)
        store.update(ProfileScope.Global) { it.copy(guestBackend = RuntimeGuestBackend.BOX64) }
        val provider = RuntimeLaunchProfileProvider(store, catalog, emptyMap(), strictBackend)

        val resolved = provider.resolve("CUSA00900")

        assertEquals(RuntimeGuestBackend.BOX64, resolved.guestBackend)
        assertEquals(JsonPrimitive(false), resolved.settings.getValue(gpuCopy.id).value)
        assertEquals(ValueSource.DEFAULT, resolved.settings.getValue(gpuCopy.id).source)
    }

    @Test
    fun perGameFexOverridesGlobalBox64() = runTest {
        val store = RuntimeProfileStore(temporaryFolder.root)
        store.update(ProfileScope.Global) { it.copy(guestBackend = RuntimeGuestBackend.BOX64) }
        store.update(ProfileScope.Game("CUSA00900")) { it.copy(guestBackend = RuntimeGuestBackend.FEX) }
        val provider = RuntimeLaunchProfileProvider(store, catalog, emptyMap(), strictBackend)

        val resolved = provider.resolve("CUSA00900")

        assertEquals(RuntimeGuestBackend.FEX, resolved.guestBackend)
        assertEquals(ValueSource.COMPATIBILITY, resolved.settings.getValue(gpuCopy.id).source)
    }

    @Test
    fun resolvesGameOverridesAndCompatibilityConstraints() = runTest {
        val store = RuntimeProfileStore(temporaryFolder.root)
        store.update(ProfileScope.Global) { it.copy(values = mapOf(boxLog.id to JsonPrimitive("1"))) }
        store.update(ProfileScope.Game("CUSA00001")) {
            it.copy(values = mapOf(boxLog.id to JsonPrimitive("2"), devKit.id to JsonPrimitive(false)))
        }
        val provider = RuntimeLaunchProfileProvider(
            store,
            catalog,
            mapOf(devKit.id to CompatibilityConstraint(JsonPrimitive(false), "Retail memory on Android")),
            strictBackend,
        )

        val resolved = provider.resolve("CUSA00001")

        assertEquals(ValueSource.GAME, resolved.settings.getValue(boxLog.id).source)
        assertEquals(ValueSource.COMPATIBILITY, resolved.settings.getValue(devKit.id).source)
        assertEquals(JsonPrimitive(false), resolved.settings.getValue(devKit.id).value)
        assertEquals(
            mapOf("BOX64_LOG" to "2", "BOX64_PROFILE" to "default"),
            provider.box64Environment(resolved),
        )
    }

    @Test
    fun launchOwnedUnknownBox64ValueIsRejected() = runTest {
        val store = RuntimeProfileStore(temporaryFolder.root)
        store.update(ProfileScope.Global) {
            it.copy(unknownBox64 = mapOf("BOX64_PATH" to "/tmp/escape"))
        }
        val provider = RuntimeLaunchProfileProvider(store, catalog, emptyMap(), strictBackend)

        val resolved = provider.resolve("CUSA00001")
        val error = assertThrows(IllegalArgumentException::class.java) {
            provider.box64Environment(resolved)
        }

        assertFalse(error.message.isNullOrBlank())
    }

    @Test
    fun officialPresetUsesBox64ProfileAndCustomOmitsIt() = runTest {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val provider = RuntimeLaunchProfileProvider(store, catalog, emptyMap(), strictBackend)
        store.update(ProfileScope.Global) { it.copy(box64Preset = Box64Preset.FAST) }

        assertEquals("fast", provider.box64Environment(provider.resolve("CUSA00001"))["BOX64_PROFILE"])

        store.update(ProfileScope.Global) { it.copy(box64Preset = Box64Preset.CUSTOM) }
        assertFalse(provider.box64Environment(provider.resolve("CUSA00001")).containsKey("BOX64_PROFILE"))
    }

    @Test
    fun missingSelectedDriverBlocksLaunchWithActionableId() = runTest {
        val store = RuntimeProfileStore(temporaryFolder.root)
        store.update(ProfileScope.Global) { it.copy(driverId = "turnip-0123456789abcdef") }
        val provider = RuntimeLaunchProfileProvider(store, catalog, emptyMap(), strictBackend)
        val resolved = provider.resolve("CUSA00001")

        val error = assertThrows(MissingRuntimeDriverException::class.java) {
            provider.vulkanConfiguration(resolved, temporaryFolder.root.toPath(), temporaryFolder.root.toPath())
        }
        assertEquals("turnip-0123456789abcdef", error.driverId)
    }

    private fun spec(
        id: String,
        nativeKey: String,
        kind: SettingKind,
        defaultValue: JsonPrimitive,
        choices: List<String> = emptyList(),
    ) = RuntimeSettingSpec(
        id = id,
        nativeKey = nativeKey,
        section = if (nativeKey.startsWith("BOX64_")) "Box64" else nativeKey.substringBefore('.'),
        category = "Test",
        title = id,
        help = id,
        kind = kind,
        defaultValue = defaultValue,
        choices = choices,
    )
}
