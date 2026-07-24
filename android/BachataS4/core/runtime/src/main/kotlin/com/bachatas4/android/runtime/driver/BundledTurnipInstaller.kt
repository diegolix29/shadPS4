package com.bachatas4.android.runtime.driver

import java.io.ByteArrayInputStream
import java.io.InputStream
import java.nio.file.Files
import java.nio.file.Path
import java.security.MessageDigest

/**
 * Extracts and validates one Play-bundled Turnip package into the private driver registry.
 *
 * Installs via [TurnipPackageInstaller] (atomic staging, path-traversal checks, ELF validation).
 * Re-extracts when the package version marker or package SHA-256 changes (app update).
 * Multiple installers may share the same [registryRoot]; each uses its own marker file.
 */
class BundledTurnipInstaller(
    private val registryRoot: Path,
    private val openAsset: () -> InputStream,
    private val expectedSha256: String = BundledTurnipSpec.SHA256,
    private val assetName: String = BundledTurnipSpec.ASSET_NAME,
    private val versionMarker: String = BundledTurnipSpec.VERSION_MARKER,
    private val sourceRepository: String = BundledTurnipSpec.SOURCE_REPOSITORY,
    private val releaseTag: String = BundledTurnipSpec.RELEASE_TAG,
    private val deviceApi: Int = android.os.Build.VERSION.SDK_INT,
) {
    constructor(
        registryRoot: Path,
        openAsset: () -> InputStream,
        packageSpec: BundledTurnipPackage,
        deviceApi: Int = android.os.Build.VERSION.SDK_INT,
    ) : this(
        registryRoot = registryRoot,
        openAsset = openAsset,
        expectedSha256 = packageSpec.sha256,
        assetName = packageSpec.assetName,
        versionMarker = packageSpec.versionMarker,
        sourceRepository = packageSpec.sourceRepository,
        releaseTag = packageSpec.releaseTag,
        deviceApi = deviceApi,
    )

    private val installer = TurnipPackageInstaller(registryRoot, deviceApi = deviceApi)
    private val registry = DriverRegistry(registryRoot)
    private val markerFile = registryRoot.resolve(markerFileName(versionMarker))

    fun ensureInstalled(): InstalledDriver {
        findMatchingInstalled()?.let { existing ->
            if (markerMatches()) return existing
        }
        val bytes = openAsset().use { it.readBytes() }
        val actual = sha256(bytes)
        require(actual.equals(expectedSha256, ignoreCase = true)) {
            "Bundled Turnip checksum mismatch for $assetName (expected $expectedSha256, got $actual)"
        }
        val installed = installer.install(
            ByteArrayInputStream(bytes),
            DriverPackageSource(
                repository = sourceRepository,
                releaseTag = releaseTag,
                assetName = assetName,
            ),
        )
        require(installed.metadata.sha256.equals(expectedSha256, ignoreCase = true)) {
            "Installed Turnip checksum mismatch after extract for $assetName"
        }
        Files.createDirectories(registryRoot)
        Files.write(markerFile, versionMarker.toByteArray(Charsets.UTF_8))
        return installed
    }

    fun findMatchingInstalled(): InstalledDriver? =
        registry.listInstalled().firstOrNull {
            it.metadata.sha256.equals(expectedSha256, ignoreCase = true)
        }

    private fun markerMatches(): Boolean =
        Files.isRegularFile(markerFile) &&
            String(Files.readAllBytes(markerFile), Charsets.UTF_8).trim() == versionMarker

    private fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256")
            .digest(bytes)
            .joinToString("") { "%02x".format(it) }

    companion object {
        fun markerFileName(versionMarker: String): String =
            ".bundled-turnip-$versionMarker"

        /** @deprecated Use [markerFileName]; kept for older single-package tests. */
        const val MARKER_FILE = ".bundled-turnip-version"
    }
}
