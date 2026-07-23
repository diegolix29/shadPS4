package com.bachatas4.android.data

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ImportManagerTest {
    @After
    fun tearDown() {
        ImportManager.reset()
    }

    @Test
    fun tryBeginImportClaimsSlotOnce() {
        assertTrue(ImportManager.tryBeginImport())
        assertEquals(ImportProgress.Preparing, ImportManager.progress.value)
        assertTrue(ImportManager.isBusy())
        assertFalse(ImportManager.tryBeginImport())
        assertEquals(ImportProgress.Preparing, ImportManager.progress.value)
    }

    @Test
    fun resetFreesSlotForNextImport() {
        assertTrue(ImportManager.tryBeginImport())
        ImportManager.update(
            ImportProgress.Copying(
                bytesCopied = 10L,
                totalBytes = 100L,
                currentFile = "eboot.bin",
                gameTitle = "Test",
            ),
        )
        assertTrue(ImportManager.isBusy())
        ImportManager.reset()
        assertFalse(ImportManager.isBusy())
        assertTrue(ImportManager.tryBeginImport())
    }

    @Test
    fun successAndFailedAreNotBusy() {
        ImportManager.update(ImportProgress.Success("id", "Title"))
        assertFalse(ImportManager.isBusy())
        assertTrue(ImportManager.tryBeginImport())

        ImportManager.update(ImportProgress.Failed("boom"))
        assertFalse(ImportManager.isBusy())
        assertTrue(ImportManager.tryBeginImport())
    }

    @Test
    fun scanningIsBusy() {
        ImportManager.update(ImportProgress.Scanning("folder"))
        assertTrue(ImportManager.isBusy())
        assertFalse(ImportManager.tryBeginImport())
    }

    @Test
    fun extractingAndFinalizingAreBusy() {
        ImportManager.update(
            ImportProgress.Extracting(0L, 100L, "eboot.bin", "Game"),
        )
        assertTrue(ImportManager.isBusy())
        assertFalse(ImportManager.tryBeginImport())

        ImportManager.update(ImportProgress.Finalizing("Game"))
        assertTrue(ImportManager.isBusy())
        assertFalse(ImportManager.tryBeginImport())
    }

    @Test
    fun needPasscodeIsBusyAndBlocksNewImport() {
        ImportManager.update(
            ImportProgress.NeedPasscode("EP0001-CUSA00000_00-TEST000000000000", "Hint"),
        )
        assertTrue(ImportManager.isBusy())
        assertFalse(ImportManager.tryBeginImport())
    }
}
