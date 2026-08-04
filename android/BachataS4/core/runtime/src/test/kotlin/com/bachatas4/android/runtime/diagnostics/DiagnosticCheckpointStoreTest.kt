package com.bachatas4.android.runtime.diagnostics

import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticCheckpointStoreTest {
    @Test
    fun persistsCheckpointAtomicallyAndSurvivesReload() {
        val dir = Files.createTempDirectory("diag-checkpoint")
        val log = SessionLog(dir)
        val store = DiagnosticCheckpointStore(dir, log)
        store.mark(DiagnosticCheckpoint.SESSION_CREATED)
        store.mark(DiagnosticCheckpoint.BACKEND_LAUNCHED)
        assertEquals(DiagnosticCheckpoint.BACKEND_LAUNCHED, store.lastCheckpoint)
        assertTrue(Files.isRegularFile(store.metadataFile))
        val loaded = store.load()
        assertNotNull(loaded)
        assertEquals("BACKEND_LAUNCHED", loaded!!.lastCheckpoint)
        assertTrue(loaded.checkpoints.size >= 2)
        val appLog = Files.readString(log.applicationLog)
        assertTrue("checkpoint=BACKEND_LAUNCHED" in appLog)
        // Partial temp must not be treated as valid final metadata.
        assertTrue(Files.notExists(dir.resolve("session.json.tmp")))
    }
}
