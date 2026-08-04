package com.bachatas4.android.runtime.diagnostics

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.ZipInputStream
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticExporterTest {
    @Test
    fun sanitizesPrivateAndGamePaths() {
        val output = ByteArrayOutputStream()
        DiagnosticExporter(File("/data/user/0/app"), File("/data/user/0/app/files/games/GAME-1")).export(
            output,
            mapOf("session.txt" to "/data/user/0/app/files/games/GAME-1/eboot.bin /data/user/0/app/cache/log"),
        )
        val text = ZipInputStream(ByteArrayInputStream(output.toByteArray())).use { zip ->
            zip.nextEntry
            zip.readBytes().decodeToString()
        }
        assertTrue("<GAME_ROOT>/eboot.bin" in text)
        assertTrue("<APP_DATA>/cache/log" in text)
        assertFalse("/data/user" in text)
    }
}
