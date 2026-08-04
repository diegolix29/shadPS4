package com.bachatas4.android.runtime.diagnostics

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticRedactorTest {
    private val redactor = DiagnosticRedactor(
        appRoot = "/data/user/0/com.bachatas4.android",
        gameRoot = "/data/user/0/com.bachatas4.android/files/games/CUSA00000",
        packageName = "com.bachatas4.android",
        deviceSerial = "R58M30ABCDE",
    )

    @Test
    fun redactsSensitivePatternsAndPreservesEvidence() {
        val input = """
            path=/data/user/0/com.bachatas4.android/files/cache/log
            game=/data/user/0/com.bachatas4.android/files/games/CUSA00000/eboot.bin
            external=/storage/emulated/0/Download/dump.txt
            sd=/sdcard/Android/data/foo
            home=/home/alice/.config
            email=user@example.com
            Authorization: Bearer abcdef0123456789
            url=https://cdn.example/file?X-Amz-Signature=deadbeef&other=1
            api_key=supersecretkeyvalue
            serial=R58M30ABCDE
            cusa=CUSA00000 vulkan=VK_KHR_swapchain symbol=main+0x40 pc=0x600000abcd signal=SIGSEGV exit=139
            lib=libvulkan.so driver=Mesa 26.3.0-devel
        """.trimIndent()
        val out = redactor.redactText(input)
        assertFalse("/data/user/0/com.bachatas4.android" in out)
        assertFalse("/storage/emulated/0" in out)
        assertFalse("/sdcard/" in out)
        assertFalse("/home/alice" in out)
        assertFalse("user@example.com" in out)
        assertFalse("abcdef0123456789" in out)
        assertFalse("deadbeef" in out)
        assertFalse("supersecretkeyvalue" in out)
        assertFalse("R58M30ABCDE" in out)
        assertTrue("<APP_DATA>" in out)
        assertTrue("<GAME_ROOT>" in out)
        assertTrue("<EXTERNAL_STORAGE>" in out)
        assertTrue("<USER>" in out)
        assertTrue("<EMAIL>" in out)
        assertTrue("<TOKEN>" in out)
        assertTrue("<DEVICE_SERIAL>" in out)
        assertTrue("CUSA00000" in out)
        assertTrue("VK_KHR_swapchain" in out)
        assertTrue("0x600000abcd" in out)
        assertTrue("SIGSEGV" in out)
        assertTrue("libvulkan.so" in out)
        assertTrue("Mesa 26.3.0-devel" in out)
    }

    @Test
    fun doesNotMutateInputReferenceContentWhenStreamingCopies() {
        val original = "/data/user/0/com.bachatas4.android/files/log"
        val redacted = redactor.redactLine(original)
        assertTrue(original.contains("/data/user/0/com.bachatas4.android"))
        assertTrue(redacted.contains("<APP_DATA>"))
    }
}
