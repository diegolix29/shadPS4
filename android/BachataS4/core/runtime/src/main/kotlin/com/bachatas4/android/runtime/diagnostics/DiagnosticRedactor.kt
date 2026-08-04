package com.bachatas4.android.runtime.diagnostics

import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStream
import java.io.InputStreamReader
import java.io.OutputStream
import java.io.OutputStreamWriter
import java.nio.charset.StandardCharsets

/**
 * Deterministic privacy redaction for exported diagnostic copies.
 * Never mutates original session logs.
 */
class DiagnosticRedactor(
    appRoot: String? = null,
    gameRoot: String? = null,
    packageName: String? = null,
    deviceSerial: String? = null,
    extraPathRoots: List<Pair<String, String>> = emptyList(),
) {
    private val pathReplacements: List<Pair<String, String>>
    private val serial: String?

    init {
        val pairs = mutableListOf<Pair<String, String>>()
        fun addPath(raw: String?, placeholder: String) {
            val path = raw?.trim().orEmpty()
            if (path.isEmpty()) return
            val normalized = path.trimEnd('/')
            pairs += normalized to placeholder
            // Common /data/user/0 vs /data/data aliases
            if (normalized.startsWith("/data/user/0/")) {
                pairs += normalized.replaceFirst("/data/user/0/", "/data/data/") to placeholder
            } else if (normalized.startsWith("/data/data/")) {
                pairs += normalized.replaceFirst("/data/data/", "/data/user/0/") to placeholder
            }
        }
        addPath(gameRoot, PLACEHOLDER_GAME_ROOT)
        addPath(appRoot, PLACEHOLDER_APP_DATA)
        extraPathRoots.forEach { (path, placeholder) -> addPath(path, placeholder) }
        packageName?.takeIf { it.isNotBlank() }?.let { pkg ->
            addPath("/data/user/0/$pkg", PLACEHOLDER_APP_DATA)
            addPath("/data/data/$pkg", PLACEHOLDER_APP_DATA)
        }
        // Longest first so nested roots replace correctly.
        pathReplacements = pairs.distinctBy { it.first }.sortedByDescending { it.first.length }
        serial = deviceSerial?.takeIf { it.isNotBlank() }
    }

    fun redactLine(line: String): String {
        var result = line
        for ((path, placeholder) in pathReplacements) {
            if (path.isNotEmpty()) {
                result = result.replace(path, placeholder)
            }
        }
        result = EXTERNAL_STORAGE_REGEX.replace(result, PLACEHOLDER_EXTERNAL_STORAGE)
        result = SDCARD_REGEX.replace(result, PLACEHOLDER_EXTERNAL_STORAGE)
        result = USER_HOME_REGEX.replace(result, PLACEHOLDER_USER)
        result = EMAIL_REGEX.replace(result, PLACEHOLDER_EMAIL)
        result = BEARER_REGEX.replace(result, "$1$PLACEHOLDER_TOKEN")
        result = AUTH_HEADER_REGEX.replace(result, "$1$PLACEHOLDER_TOKEN")
        result = API_KEY_REGEX.replace(result, "$1$PLACEHOLDER_TOKEN")
        result = SIGNED_URL_REGEX.replace(result) { match ->
            "${match.groupValues[1]}=$PLACEHOLDER_TOKEN"
        }
        serial?.let { s ->
            if (s.length >= 4) {
                result = result.replace(s, PLACEHOLDER_DEVICE_SERIAL)
            }
        }
        result = ADB_SERIAL_REGEX.replace(result) { match ->
            "${match.groupValues[1]}$PLACEHOLDER_DEVICE_SERIAL"
        }
        return result
    }

    fun redactText(text: String): String =
        text.lineSequence().joinToString("\n") { redactLine(it) }.let { out ->
            if (text.endsWith("\n") && !out.endsWith("\n")) out + "\n" else out
        }

    /**
     * Stream-redact [input] into [output]. Optional [maxBytes] truncates with head+tail
     * preservation. Returns (bytesWritten, truncated).
     */
    fun redactStream(
        input: InputStream,
        output: OutputStream,
        maxBytes: Long = Long.MAX_VALUE,
    ): RedactStreamResult {
        if (maxBytes <= 0L) {
            return RedactStreamResult(bytesWritten = 0, truncated = true)
        }
        val reader = BufferedReader(InputStreamReader(input, StandardCharsets.UTF_8))
        val writer = BufferedWriter(OutputStreamWriter(output, StandardCharsets.UTF_8))
        var written = 0L
        var truncated = false
        val headBudget = (maxBytes * HEAD_FRACTION).toLong().coerceAtLeast(1L)
        val tailBudget = (maxBytes - headBudget).coerceAtLeast(1L)
        val head = StringBuilder()
        val tail = ArrayDeque<String>()
        var tailBytes = 0L
        var pastHead = false

        fun appendHead(line: String): Boolean {
            val redacted = redactLine(line) + "\n"
            val bytes = redacted.toByteArray(StandardCharsets.UTF_8).size.toLong()
            if (written + bytes > headBudget) {
                pastHead = true
                return false
            }
            head.append(redacted)
            written += bytes
            return true
        }

        fun pushTail(line: String) {
            val redacted = redactLine(line) + "\n"
            val bytes = redacted.toByteArray(StandardCharsets.UTF_8).size.toLong()
            tail.addLast(redacted)
            tailBytes += bytes
            while (tailBytes > tailBudget && tail.isNotEmpty()) {
                val removed = tail.removeFirst()
                tailBytes -= removed.toByteArray(StandardCharsets.UTF_8).size.toLong()
                truncated = true
            }
        }

        var line: String?
        while (reader.readLine().also { line = it } != null) {
            val current = line ?: break
            if (!pastHead) {
                if (!appendHead(current)) {
                    truncated = true
                    pushTail(current)
                }
            } else {
                pushTail(current)
            }
        }

        writer.write(head.toString())
        if (truncated && tail.isNotEmpty()) {
            val marker = "\n... [truncated for diagnostic size limits] ...\n\n"
            writer.write(marker)
            written += marker.toByteArray(StandardCharsets.UTF_8).size.toLong()
        }
        for (chunk in tail) {
            writer.write(chunk)
            written += chunk.toByteArray(StandardCharsets.UTF_8).size.toLong()
        }
        writer.flush()
        return RedactStreamResult(bytesWritten = written, truncated = truncated)
    }

    data class RedactStreamResult(
        val bytesWritten: Long,
        val truncated: Boolean,
    )

    companion object {
        const val REDACTION_VERSION = 1
        const val PLACEHOLDER_APP_DATA = "<APP_DATA>"
        const val PLACEHOLDER_GAME_ROOT = "<GAME_ROOT>"
        const val PLACEHOLDER_EXTERNAL_STORAGE = "<EXTERNAL_STORAGE>"
        const val PLACEHOLDER_USER = "<USER>"
        const val PLACEHOLDER_EMAIL = "<EMAIL>"
        const val PLACEHOLDER_TOKEN = "<TOKEN>"
        const val PLACEHOLDER_DEVICE_SERIAL = "<DEVICE_SERIAL>"

        private const val HEAD_FRACTION = 0.25

        private val EXTERNAL_STORAGE_REGEX =
            Regex("/storage/emulated/\\d+(?:/[^\\s\"']*)?")
        private val SDCARD_REGEX =
            Regex("/sdcard(?:/[^\\s\"']*)?")
        private val USER_HOME_REGEX =
            Regex("/home/[A-Za-z0-9._-]+(?:/[^\\s\"']*)?")
        private val EMAIL_REGEX =
            Regex("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}")
        private val BEARER_REGEX =
            Regex("(Bearer\\s+)[A-Za-z0-9._\\-+=/]+", RegexOption.IGNORE_CASE)
        private val AUTH_HEADER_REGEX =
            Regex("(Authorization:\\s*(?:Bearer\\s+|Basic\\s+)?)[A-Za-z0-9._\\-+=/]+", RegexOption.IGNORE_CASE)
        private val API_KEY_REGEX =
            Regex(
                "((?:api[_-]?key|access[_-]?token|secret[_-]?key|x-api-key)\\s*[:=]\\s*)[A-Za-z0-9._\\-+=/]{8,}",
                RegexOption.IGNORE_CASE,
            )
        private val SIGNED_URL_REGEX =
            Regex(
                "([?&](?:X-Amz-Signature|X-Amz-Credential|X-Amz-Security-Token|Signature|sig|token|key)=)([^&\\s]+)",
                RegexOption.IGNORE_CASE,
            )
        private val ADB_REGEX_DEVICE =
            Regex("\\b(?:emulator-\\d{4}|[0-9a-f]{8,16})\\b", RegexOption.IGNORE_CASE)
        // Only redact explicit serial= / android_serial= forms to avoid eating hashes.
        private val ADB_SERIAL_REGEX =
            Regex(
                "((?:serial|android_serial|device_serial|adb[_-]?serial)\\s*[:=]\\s*)([A-Za-z0-9._:-]{4,})",
                RegexOption.IGNORE_CASE,
            )
    }
}
