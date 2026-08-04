package com.bachatas4.android.runtime.diagnostics

import java.security.SecureRandom
import java.time.Clock
import java.time.LocalDate
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

object DiagnosticReportIds {
    private val dateFormat = DateTimeFormatter.ofPattern("yyyyMMdd").withZone(ZoneOffset.UTC)
    private val hexChars = "0123456789ABCDEF".toCharArray()
    private val defaultRandom = SecureRandom()

    /**
     * Format: `BS4-YYYYMMDD-<8 uppercase hex>`.
     * Locally generated; no device identifier embedded.
     */
    fun generate(
        clock: Clock = Clock.systemUTC(),
        random: SecureRandom = defaultRandom,
    ): String {
        val date = dateFormat.format(LocalDate.now(clock))
        val suffix = CharArray(8) { hexChars[random.nextInt(hexChars.size)] }.concatToString()
        return "BS4-$date-$suffix"
    }

    fun isValid(reportId: String): Boolean =
        reportId.matches(Regex("^BS4-\\d{8}-[0-9A-F]{8}$"))
}
