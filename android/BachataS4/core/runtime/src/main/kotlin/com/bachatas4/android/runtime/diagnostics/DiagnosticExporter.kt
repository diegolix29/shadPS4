package com.bachatas4.android.runtime.diagnostics

import java.io.File
import java.io.OutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * Lightweight path-sanitized ZIP helper kept for compatibility with existing call sites.
 * Prefer [DiagnosticBundleBuilder] for full Release 1 reports.
 */
class DiagnosticExporter(
    private val appRoot: File,
    private val gameRoot: File,
    private val packageName: String? = null,
    private val deviceSerial: String? = null,
) {
    fun export(output: OutputStream, entries: Map<String, String>) {
        val redactor = DiagnosticRedactor(
            appRoot = appRoot.canonicalPath,
            gameRoot = gameRoot.canonicalPath,
            packageName = packageName,
            deviceSerial = deviceSerial,
        )
        ZipOutputStream(output).use { zip ->
            entries.toSortedMap().forEach { (name, content) ->
                require(DiagnosticBundleBuilder.isSafeZipEntryName(name)) {
                    "Unsafe diagnostic entry name"
                }
                val sanitized = redactor.redactText(content)
                zip.putNextEntry(ZipEntry(name))
                zip.write(sanitized.toByteArray())
                zip.closeEntry()
            }
        }
    }
}
