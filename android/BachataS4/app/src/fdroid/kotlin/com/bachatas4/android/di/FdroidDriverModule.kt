package com.bachatas4.android.di

import android.content.Context
import com.bachatas4.android.feature.drivers.DriverManagerBackend
import com.bachatas4.android.feature.drivers.DriverManagerCapabilities
import com.bachatas4.android.runtime.driver.DriverPackageSource
import com.bachatas4.android.runtime.driver.DriverRegistry
import com.bachatas4.android.runtime.driver.InstalledDriver
import com.bachatas4.android.runtime.driver.TurnipDownloadManager
import com.bachatas4.android.runtime.driver.TurnipPackageInstaller
import com.bachatas4.android.runtime.driver.TurnipReleaseAsset
import com.bachatas4.android.runtime.driver.TurnipReleaseClient
import com.bachatas4.android.runtime.driver.UrlConnectionDriverAssetTransport
import com.bachatas4.android.runtime.driver.UrlConnectionHttpTransport
import com.bachatas4.android.runtime.process.RuntimeVulkanDriver
import com.bachatas4.android.runtime.process.RuntimeVulkanDriverIds
import com.bachatas4.android.runtime.process.VulkanDriverConfiguration
import com.bachatas4.android.runtime.process.VulkanDriverResolveContext
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import java.io.ByteArrayInputStream
import java.nio.file.Path
import javax.inject.Singleton

private class FdroidDriverManagerBackend(context: Context) : DriverManagerBackend {
    private val root = context.filesDir.toPath().resolve("vulkan-drivers/installed")
    private val registry = DriverRegistry(root)
    private val installer = TurnipPackageInstaller(root)
    private val releaseClient = TurnipReleaseClient(
        UrlConnectionHttpTransport(),
        context.cacheDir.toPath().resolve("turnip-releases.json"),
    )
    private val downloader = TurnipDownloadManager(installer, UrlConnectionDriverAssetTransport())

    override fun capabilities() = DriverManagerCapabilities(
        remoteCatalogEnabled = true,
        importEnabled = true,
        deleteEnabled = true,
        statusMessage = null,
    )

    override fun installed(): List<InstalledDriver> = registry.listInstalled()
    override fun releases(force: Boolean): List<TurnipReleaseAsset> = releaseClient.listReleases(force)
    override fun download(asset: TurnipReleaseAsset, progress: (Long, Long) -> Unit): InstalledDriver =
        downloader.download(asset, progress)

    override fun importZip(bytes: ByteArray, assetName: String): InstalledDriver = installer.install(
        ByteArrayInputStream(bytes),
        DriverPackageSource(assetName = assetName),
    )

    override fun remove(id: String): Boolean = registry.remove(id)

    override fun configurationFor(driverId: String, context: VulkanDriverResolveContext): VulkanDriverConfiguration =
        when (driverId) {
            RuntimeVulkanDriverIds.SYSTEM ->
                VulkanDriverConfiguration.resolve(RuntimeVulkanDriver.SYSTEM, context)
            RuntimeVulkanDriverIds.SYSTEM_VORTEK ->
                VulkanDriverConfiguration.resolve(RuntimeVulkanDriver.SYSTEM_VORTEK, context)
            else -> {
                val driver = registry.resolve(driverId)
                    ?: throw IllegalStateException(
                        "Selected Vulkan driver '$driverId' is not installed; open Turnip drivers and select another driver",
                    )
                VulkanDriverConfiguration.resolve(driver, context.runtimeRoot)
            }
        }
}

@Module
@InstallIn(SingletonComponent::class)
object FdroidDriverModule {
    @Provides
    @Singleton
    fun backend(@ApplicationContext context: Context): DriverManagerBackend =
        FdroidDriverManagerBackend(context)
}
