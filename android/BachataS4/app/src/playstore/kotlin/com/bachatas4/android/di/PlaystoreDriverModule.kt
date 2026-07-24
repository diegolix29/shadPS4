package com.bachatas4.android.di

import android.content.Context
import com.bachatas4.android.feature.drivers.DriverManagerBackend
import com.bachatas4.android.feature.drivers.DriverManagerCapabilities
import com.bachatas4.android.runtime.driver.BundledTurnipInstaller
import com.bachatas4.android.runtime.driver.BundledTurnipPackage
import com.bachatas4.android.runtime.driver.BundledTurnipSpec
import com.bachatas4.android.runtime.driver.InstalledDriver
import com.bachatas4.android.runtime.driver.TurnipReleaseAsset
import com.bachatas4.android.runtime.process.VulkanDriverConfiguration
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import java.nio.file.Path
import javax.inject.Singleton

/**
 * Play Store driver backend: APK-bundled Turnip packages only
 * (mojo-26.1, mojo-25.0, gen8). No catalogue fetch, archive download, or ZIP import.
 */
internal class PlaystoreDriverManagerBackend(context: Context) : DriverManagerBackend {
    private val assets = context.assets
    private val root = context.filesDir.toPath().resolve("vulkan-drivers/installed")
    private val packages: List<BundledTurnipPackage> = BundledTurnipSpec.ALL
    private val installers: List<BundledTurnipInstaller> = packages.map { pkg ->
        BundledTurnipInstaller(
            registryRoot = root,
            openAsset = { assets.open(pkg.assetPath) },
            packageSpec = pkg,
        )
    }

    override fun capabilities() = DriverManagerCapabilities(
        remoteCatalogEnabled = false,
        importEnabled = false,
        deleteEnabled = false,
        statusMessage = PLAY_STATUS,
    )

    override fun installed(): List<InstalledDriver> = ensureAllInstalled()

    override fun releases(force: Boolean): List<TurnipReleaseAsset> = emptyList()

    override fun download(asset: TurnipReleaseAsset, progress: (Long, Long) -> Unit): InstalledDriver {
        throw UnsupportedOperationException(REMOTE_DISABLED)
    }

    override fun importZip(bytes: ByteArray, assetName: String): InstalledDriver {
        throw UnsupportedOperationException(REMOTE_DISABLED)
    }

    override fun remove(id: String): Boolean {
        throw UnsupportedOperationException("Bundled Turnip drivers cannot be removed")
    }

    override fun configurationFor(driverId: String, runtimeRoot: Path): VulkanDriverConfiguration {
        val installed = ensureAllInstalled()
        val driver = installed.firstOrNull { it.metadata.id == driverId }
            ?: installed.first() // default mojo-26.1 (BundledTurnipSpec.DEFAULT / ALL order)
        return VulkanDriverConfiguration.resolve(driver, runtimeRoot)
    }

    override fun autoSelectDriverId(): String = ensureAllInstalled().first().metadata.id

    private fun ensureAllInstalled(): List<InstalledDriver> = installers.map { it.ensureInstalled() }

    companion object {
        const val REMOTE_DISABLED =
            "Remote and imported drivers are not available in this build. " +
                "Turnip updates are delivered through app updates."
        const val PLAY_STATUS =
            "Bundled Turnip lines: mojo-26.1, mojo-25.0, and gen8. " +
                "Driver updates are delivered through app updates."
    }
}

@Module
@InstallIn(SingletonComponent::class)
object PlaystoreDriverModule {
    @Provides
    @Singleton
    fun backend(@ApplicationContext context: Context): DriverManagerBackend =
        PlaystoreDriverManagerBackend(context)
}
