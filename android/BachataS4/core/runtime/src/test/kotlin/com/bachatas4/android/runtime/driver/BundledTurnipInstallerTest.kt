package com.bachatas4.android.runtime.driver

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.nio.file.Files
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class BundledTurnipInstallerTest {
    @get:Rule val temporaryFolder = TemporaryFolder()

    @Test
    fun extractsBundledPackageAndIsIdempotent() {
        val zip = glibcTurnipZip()
        val hash = sha256(zip)
        val root = temporaryFolder.newFolder("drivers").toPath()
        val installer = BundledTurnipInstaller(
            registryRoot = root,
            openAsset = { ByteArrayInputStream(zip) },
            expectedSha256 = hash,
            assetName = "bundled.zip",
            versionMarker = "test-v1",
            deviceApi = 37,
        )
        val first = installer.ensureInstalled()
        val second = installer.ensureInstalled()
        assertEquals(first.metadata.id, second.metadata.id)
        assertEquals(hash, first.metadata.sha256)
        assertTrue(Files.isRegularFile(first.library))
        assertTrue(Files.isRegularFile(requireNotNull(first.icdManifest)))
        assertEquals(
            "test-v1",
            Files.readString(root.resolve(BundledTurnipInstaller.markerFileName("test-v1"))).trim(),
        )
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectsChecksumMismatch() {
        val zip = glibcTurnipZip()
        BundledTurnipInstaller(
            registryRoot = temporaryFolder.newFolder("bad").toPath(),
            openAsset = { ByteArrayInputStream(zip) },
            expectedSha256 = "0".repeat(64),
            assetName = "bundled.zip",
            versionMarker = "test-v1",
            deviceApi = 37,
        ).ensureInstalled()
    }

    @Test
    fun newerBundledVersionReplacesSelectionMarker() {
        val zip = glibcTurnipZip(name = "Turnip A")
        val hash = sha256(zip)
        val root = temporaryFolder.newFolder("migrate").toPath()
        val first = BundledTurnipInstaller(
            registryRoot = root,
            openAsset = { ByteArrayInputStream(zip) },
            expectedSha256 = hash,
            assetName = "a.zip",
            versionMarker = "v1",
            deviceApi = 37,
        ).ensureInstalled()

        val zip2 = glibcTurnipZip(name = "Turnip B", seed = 7)
        val hash2 = sha256(zip2)
        assertNotEquals(hash, hash2)
        val second = BundledTurnipInstaller(
            registryRoot = root,
            openAsset = { ByteArrayInputStream(zip2) },
            expectedSha256 = hash2,
            assetName = "b.zip",
            versionMarker = "v2",
            deviceApi = 37,
        ).ensureInstalled()
        assertNotEquals(first.metadata.id, second.metadata.id)
        assertEquals(
            "v2",
            Files.readString(root.resolve(BundledTurnipInstaller.markerFileName("v2"))).trim(),
        )
        assertEquals(hash2, second.metadata.sha256)
    }

    @Test
    fun multiplePackagesShareRegistryWithSeparateMarkers() {
        val zipA = glibcTurnipZip(name = "Turnip A", seed = 1)
        val zipB = glibcTurnipZip(name = "Turnip B", seed = 2)
        val hashA = sha256(zipA)
        val hashB = sha256(zipB)
        val root = temporaryFolder.newFolder("multi").toPath()

        val a = BundledTurnipInstaller(
            registryRoot = root,
            openAsset = { ByteArrayInputStream(zipA) },
            expectedSha256 = hashA,
            assetName = "a.zip",
            versionMarker = "line-a-v1",
            deviceApi = 37,
        ).ensureInstalled()
        val b = BundledTurnipInstaller(
            registryRoot = root,
            openAsset = { ByteArrayInputStream(zipB) },
            expectedSha256 = hashB,
            assetName = "b.zip",
            versionMarker = "line-b-v1",
            deviceApi = 37,
        ).ensureInstalled()

        assertNotEquals(a.metadata.id, b.metadata.id)
        assertEquals(hashA, a.metadata.sha256)
        assertEquals(hashB, b.metadata.sha256)
        assertTrue(Files.isRegularFile(root.resolve(BundledTurnipInstaller.markerFileName("line-a-v1"))))
        assertTrue(Files.isRegularFile(root.resolve(BundledTurnipInstaller.markerFileName("line-b-v1"))))
        // Re-ensure is idempotent for both.
        val a2 = BundledTurnipInstaller(
            registryRoot = root,
            openAsset = { ByteArrayInputStream(zipA) },
            expectedSha256 = hashA,
            assetName = "a.zip",
            versionMarker = "line-a-v1",
            deviceApi = 37,
        ).ensureInstalled()
        assertEquals(a.metadata.id, a2.metadata.id)
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectsPathTraversalEntries() {
        val zip = glibcTurnipZip(extra = listOf("../evil.so" to byteArrayOf(1)))
        val hash = sha256(zip)
        BundledTurnipInstaller(
            registryRoot = temporaryFolder.newFolder("trav").toPath(),
            openAsset = { ByteArrayInputStream(zip) },
            expectedSha256 = hash,
            assetName = "trav.zip",
            versionMarker = "v1",
            deviceApi = 37,
        ).ensureInstalled()
    }

    private fun glibcTurnipZip(
        name: String = "Turnip test",
        seed: Byte = 0,
        extra: List<Pair<String, ByteArray>> = emptyList(),
    ): ByteArray {
        val libraryName = "libvulkan_freedreno.so"
        val metadata = """
            {
              "schemaVersion": 1,
              "name": "$name",
              "packageVersion": "1",
              "minApi": 31,
              "libraryName": "$libraryName"
            }
        """.trimIndent().toByteArray()
        val library = arm64GlibcElf(seed)
        val output = ByteArrayOutputStream()
        ZipOutputStream(output).use { zip ->
            fun entry(entryName: String, bytes: ByteArray) {
                zip.putNextEntry(ZipEntry(entryName).apply { time = 0L })
                zip.write(bytes)
                zip.closeEntry()
            }
            entry("meta.json", metadata)
            entry(libraryName, library)
            extra.forEach { (n, b) -> entry(n, b) }
        }
        return output.toByteArray()
    }

    private fun arm64GlibcElf(seed: Byte): ByteArray = ByteArray(160).also { bytes ->
        bytes[0] = 0x7f
        bytes[1] = 'E'.code.toByte()
        bytes[2] = 'L'.code.toByte()
        bytes[3] = 'F'.code.toByte()
        bytes[4] = 2
        bytes[5] = 1
        bytes[18] = 183.toByte()
        bytes[19] = 0
        bytes[40] = seed
        "libc.so.6".toByteArray().copyInto(bytes, 64)
    }

    private fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
}
