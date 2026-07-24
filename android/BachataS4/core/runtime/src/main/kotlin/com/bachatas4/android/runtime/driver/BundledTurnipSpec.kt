package com.bachatas4.android.runtime.driver

/**
 * One Play Store-bundled Turnip package (asset + provenance).
 *
 * Source artifacts come from [bachata-s4-drivers](https://github.com/JICA98/bachata-s4-drivers)
 * `*-EMULATOR.zip` releases (glibc Turnip lines: mojo-26.1, mojo-25.0, gen8).
 */
data class BundledTurnipPackage(
    val assetPath: String,
    val assetName: String,
    val displayLabel: String,
    val versionMarker: String,
    val releaseTag: String,
    /** SHA-256 of the playstore asset at [assetPath]. */
    val sha256: String,
    val sourceRepository: String = "JICA98/bachata-s4-drivers",
)

/**
 * Play Store bundled Turnip packages.
 *
 * Default launch selection is [MOJO_26_1]. [ALL] is extracted on first use so the
 * Drivers tab can switch between lines without network access.
 */
object BundledTurnipSpec {
    const val SOURCE_REPOSITORY = "JICA98/bachata-s4-drivers"

    val MOJO_26_1 = BundledTurnipPackage(
        assetPath = "drivers/turnip-26.1.0-EMULATOR.zip",
        assetName = "Turnip-mojo-26.1-v2-08e7443-EMULATOR.zip",
        displayLabel = "Bundled Turnip mojo-26.1",
        versionMarker = "26.1.0-v2-08e7443",
        releaseTag = "mojo-26.1-v2",
        sha256 = "9ab3d1103079f705ce16d991105d9cc379476927114f02c50b92dca16bd023b9",
    )

    val MOJO_25_0 = BundledTurnipPackage(
        assetPath = "drivers/turnip-mojo-25.0-EMULATOR.zip",
        assetName = "Turnip-mojo-25.0-v2-2a6fe3d-EMULATOR.zip",
        displayLabel = "Bundled Turnip mojo-25.0",
        versionMarker = "mojo-25.0-v2-2a6fe3d",
        releaseTag = "mojo-25.0-v2",
        sha256 = "1288ca73a492a8b2019382004069fe8384a95b613ff88c973b4bf56f29957c4c",
    )

    val GEN8 = BundledTurnipPackage(
        assetPath = "drivers/turnip-gen8-EMULATOR.zip",
        assetName = "Turnip-gen8-v2-7fdde2f-EMULATOR.zip",
        displayLabel = "Bundled Turnip gen8",
        versionMarker = "gen8-v2-7fdde2f",
        releaseTag = "gen8-v2",
        sha256 = "5144c05fc8f724cd282807e3c3a2acfb3524df743f8c8ef9591e93744f097297",
    )

    /** Default auto-select package (setup continue on Play). */
    val DEFAULT: BundledTurnipPackage = MOJO_26_1

    /** All packages shipped in the playstore APK, default first. */
    val ALL: List<BundledTurnipPackage> = listOf(MOJO_26_1, MOJO_25_0, GEN8)

    // Legacy single-package constants (mojo-26.1) for older call sites / tests.
    const val ASSET_PATH = "drivers/turnip-26.1.0-EMULATOR.zip"
    const val ASSET_NAME = "Turnip-mojo-26.1-v2-08e7443-EMULATOR.zip"
    const val DISPLAY_LABEL = "Bundled Turnip mojo-26.1"
    const val VERSION_MARKER = "26.1.0-v2-08e7443"
    const val RELEASE_TAG = "mojo-26.1-v2"
    const val SHA256 = "9ab3d1103079f705ce16d991105d9cc379476927114f02c50b92dca16bd023b9"
}
