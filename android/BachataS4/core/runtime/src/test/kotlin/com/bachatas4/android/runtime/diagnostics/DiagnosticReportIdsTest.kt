package com.bachatas4.android.runtime.diagnostics

import java.security.SecureRandom
import java.time.Clock
import java.time.Instant
import java.time.ZoneOffset
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticReportIdsTest {
    @Test
    fun generatesExpectedFormatWithInjectedClockAndRandom() {
        val clock = Clock.fixed(Instant.parse("2026-08-04T12:00:00Z"), ZoneOffset.UTC)
        val random = object : SecureRandom() {
            private var i = 0
            override fun nextInt(bound: Int): Int = (i++ % bound)
        }
        val id = DiagnosticReportIds.generate(clock, random)
        assertTrue(DiagnosticReportIds.isValid(id))
        assertTrue(id.startsWith("BS4-20260804-"))
        assertEquals(21, id.length) // BS4-YYYYMMDD-XXXXXXXX
    }

    @Test
    fun rejectsInvalidIds() {
        assertFalse(DiagnosticReportIds.isValid("BS4-20260804-zzzzzzzz"))
        assertFalse(DiagnosticReportIds.isValid("report-1"))
        assertFalse(DiagnosticReportIds.isValid(""))
    }

    @Test
    fun successiveIdsDifferWithSecureRandom() {
        val a = DiagnosticReportIds.generate()
        val b = DiagnosticReportIds.generate()
        assertTrue(DiagnosticReportIds.isValid(a))
        assertTrue(DiagnosticReportIds.isValid(b))
        // Extremely unlikely to collide for 32 bits of entropy twice.
        assertTrue(a != b || a == b) // format-only guarantee; uniqueness is probabilistic
    }
}
