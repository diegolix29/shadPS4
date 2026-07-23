package com.bachatas4.android.data

import com.bachatas4.android.model.RuntimeErrorCode
import java.io.ByteArrayInputStream
import java.io.File
import java.io.InputStream
import java.security.MessageDigest
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class ContentImporterTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    private val sourceUri: String = "content://game"

    @Test
    fun importGameWritesUnderGamesDirectory() = runTest {
        val bytes = "payload".toByteArray()
        val importer = importerFor(bytes)

        val result = importer.importGame(
            ContentImportRequest(
                id = "CUSA00001",
                title = "Test Game",
                sourceUri = sourceUri,
                expectedBytes = bytes.size.toLong(),
                expectedSha256 = sha256(bytes),
            ),
        )

        assertEquals("games/CUSA00001", result.game.relativePath)
        assertEquals(bytes.size.toLong(), result.bytesCopied)
        assertEquals(sha256(bytes), result.sha256)
        assertTrue(File(temporaryFolder.root, "games/CUSA00001/eboot.bin").isFile)
        assertFalse(File(temporaryFolder.root, "games/.import-fixed").exists())
    }

    @Test
    fun duplicateGameIdDoesNotDeleteExistingGame() = runTest {
        val existing = File(temporaryFolder.root, "games/CUSA00001")
        existing.mkdirs()
        File(existing, "eboot.bin").writeText("current")
        val importer = importerFor("new".toByteArray())

        val error = assertImportException {
            importer.importGame(
                ContentImportRequest(
                    id = "CUSA00001",
                    title = "Duplicate",
                    sourceUri = sourceUri,
                ),
            )
        }

        assertEquals(RuntimeErrorCode.CONTENT_INVALID, error.code)
        assertEquals("current", File(existing, "eboot.bin").readText())
    }

    @Test
    fun byteMismatchRemovesStagingAndDestination() = runTest {
        val importer = importerFor("short".toByteArray())

        val error = assertImportException {
            importer.importGame(
                ContentImportRequest(
                    id = "CUSA00002",
                    title = "Bad Game",
                    sourceUri = sourceUri,
                    expectedBytes = 99,
                ),
            )
        }

        assertEquals(RuntimeErrorCode.CONTENT_INVALID, error.code)
        assertFalse(File(temporaryFolder.root, "games/CUSA00002").exists())
        assertFalse(File(temporaryFolder.root, "games/.import-fixed").exists())
    }

    @Test
    fun cancellationRemovesStaging() {
        val importer = ContentImporter(
            filesDir = temporaryFolder.root,
            source = ImportSource { CancellingInputStream() },
            idFactory = { "fixed" },
        )

        assertThrows(CancellationException::class.java) {
            runTest {
                importer.importGame(
                    ContentImportRequest(
                        id = "CUSA00003",
                        title = "Cancelled",
                        sourceUri = sourceUri,
                    ),
                )
            }
        }

        assertFalse(File(temporaryFolder.root, "games/CUSA00003").exists())
        assertFalse(File(temporaryFolder.root, "games/.import-fixed").exists())
    }

    @Test
    fun lostUriPermissionMapsToRuntimeError() = runTest {
        val importer = ContentImporter(
            filesDir = temporaryFolder.root,
            source = ImportSource { throw SecurityException("revoked") },
            idFactory = { "fixed" },
        )

        val error = assertImportException {
            importer.importGame(
                ContentImportRequest(
                    id = "CUSA00004",
                    title = "Permission",
                    sourceUri = sourceUri,
                ),
            )
        }

        assertEquals(RuntimeErrorCode.CONTENT_PERMISSION_LOST, error.code)
    }

    @Test
    fun invalidGameIdCannotEscapeGamesDirectory() = runTest {
        val importer = importerFor("payload".toByteArray())

        val error = assertImportException {
            importer.importGame(
                ContentImportRequest(
                    id = "../escape",
                    title = "Escape",
                    sourceUri = sourceUri,
                ),
            )
        }

        assertEquals(RuntimeErrorCode.CONTENT_INVALID, error.code)
        assertFalse(File(temporaryFolder.root, "escape").exists())
    }

    @Test
    fun importGameTreePreservesFilesAndRequiresEboot() = runTest {
        val files = mapOf(
            "content://eboot" to "exe".toByteArray(),
            "content://param" to "meta".toByteArray(),
        )
        val importer = ContentImporter(
            filesDir = temporaryFolder.root,
            source = ImportSource { uri -> ByteArrayInputStream(files.getValue(uri)) },
            idFactory = { "fixed" },
        )

        val result = importer.importGameTree(
            ContentImportRequest("CUSA00005", "Tree", "content://tree"),
            listOf(
                ContentTreeEntry("eboot.bin", "content://eboot"),
                ContentTreeEntry("sce_sys/param.sfo", "content://param"),
            ),
        )

        assertEquals(7, result.bytesCopied)
        assertEquals("exe", File(temporaryFolder.root, "games/CUSA00005/eboot.bin").readText())
        assertEquals("meta", File(temporaryFolder.root, "games/CUSA00005/sce_sys/param.sfo").readText())
    }

    @Test
    fun importGameTreeRejectsEscapingPath() = runTest {
        val importer = importerFor("payload".toByteArray())

        val error = assertImportException {
            importer.importGameTree(
                ContentImportRequest("CUSA00006", "Escape", "content://tree"),
                listOf(ContentTreeEntry("../outside", sourceUri)),
            )
        }

        assertEquals(RuntimeErrorCode.CONTENT_INVALID, error.code)
        assertFalse(File(temporaryFolder.root, "outside").exists())
    }

    @Test
    fun importGameTreeRejectsInsufficientStorageBeforeOpeningFiles() = runTest {
        var opened = false
        val importer = ContentImporter(
            filesDir = temporaryFolder.root,
            source = ImportSource {
                opened = true
                ByteArrayInputStream(byteArrayOf())
            },
            idFactory = { "fixed" },
            availableBytes = { 8L * 1024 * 1024 * 1024 },
        )

        val error = assertImportException {
            importer.importGameTree(
                ContentImportRequest("CUSA00900", "Large", "content://tree"),
                listOf(
                    ContentTreeEntry("eboot.bin", "content://eboot", 90L * 1024 * 1024),
                    ContentTreeEntry("data.pkg", "content://data", 25L * 1024 * 1024 * 1024),
                ),
            )
        }

        assertEquals(RuntimeErrorCode.CONTENT_INVALID, error.code)
        assertTrue(error.message!!.contains("requires"))
        assertFalse(opened)
        assertFalse(File(temporaryFolder.root, "games/.import-fixed").exists())
    }

    @Test
    fun finalizeStagingTreePromotesToGamesDir() = runTest {
        val gamesDir = File(temporaryFolder.root, "games")
        val staging = File(gamesDir, ".import-pkg")
        File(staging, "sce_sys").mkdirs()
        File(staging, "sce_sys/param.sfo").writeText("sfo")
        File(staging, "eboot.bin").writeText("boot")
        val importer = importerFor(ByteArray(0))

        val result = importer.finalizeStagingTree(
            ContentImportRequest("CUSA00007", "Pkg Game", "content://pkg.pkg"),
            staging,
        )

        assertEquals("games/CUSA00007", result.game.relativePath)
        assertEquals("pkg-extract", result.sha256)
        assertEquals(7L, result.bytesCopied)
        assertEquals("boot", File(temporaryFolder.root, "games/CUSA00007/eboot.bin").readText())
        assertEquals("sfo", File(temporaryFolder.root, "games/CUSA00007/sce_sys/param.sfo").readText())
        assertFalse(staging.exists())
    }

    @Test
    fun finalizeStagingTreeRejectsMissingEboot() = runTest {
        val gamesDir = File(temporaryFolder.root, "games")
        val staging = File(gamesDir, ".import-pkg")
        File(staging, "sce_sys").mkdirs()
        File(staging, "sce_sys/param.sfo").writeText("sfo")
        val importer = importerFor(ByteArray(0))

        val error = assertImportException {
            importer.finalizeStagingTree(
                ContentImportRequest("CUSA00008", "Bad", "content://pkg.pkg"),
                staging,
            )
        }

        assertEquals(RuntimeErrorCode.CONTENT_INVALID, error.code)
        assertTrue(error.message!!.contains("eboot.bin"))
    }

    @Test
    fun progressCallbackFiresAfterEachFile() = runTest {
        val files = mapOf(
            "content://eboot" to "exe".toByteArray(),
            "content://param" to "meta".toByteArray(),
        )
        val importer = ContentImporter(
            filesDir = temporaryFolder.root,
            source = ImportSource { uri -> ByteArrayInputStream(files.getValue(uri)) },
            idFactory = { "fixed" },
        )
        val progressEvents = mutableListOf<Triple<Long, Long, String>>()

        importer.importGameTree(
            ContentImportRequest("CUSA09999", "CB Test", "content://tree"),
            listOf(
                ContentTreeEntry("eboot.bin", "content://eboot"),
                ContentTreeEntry("sce_sys/param.sfo", "content://param"),
            ),
            onProgress = { copied, total, file ->
                progressEvents.add(Triple(copied, total, file))
            },
        )

        assertEquals(2, progressEvents.size)
        assertEquals("eboot.bin", progressEvents[0].third)
        assertEquals(3L, progressEvents[0].first)
        assertEquals(0L, progressEvents[0].second)
        assertEquals("sce_sys/param.sfo", progressEvents[1].third)
        assertEquals(7L, progressEvents[1].first)
        assertEquals(0L, progressEvents[1].second)
    }

    private fun importerFor(bytes: ByteArray): ContentImporter =
        ContentImporter(
            filesDir = temporaryFolder.root,
            source = ImportSource { ByteArrayInputStream(bytes) },
            idFactory = { "fixed" },
        )

    private suspend fun assertImportException(block: suspend () -> Unit): ContentImportException =
        try {
            block()
            throw AssertionError("Expected ContentImportException")
        } catch (error: ContentImportException) {
            error
        }
}

private fun sha256(bytes: ByteArray): String =
    MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }

private class CancellingInputStream : InputStream() {
    override fun read(): Int {
        throw CancellationException("cancelled")
    }
}
