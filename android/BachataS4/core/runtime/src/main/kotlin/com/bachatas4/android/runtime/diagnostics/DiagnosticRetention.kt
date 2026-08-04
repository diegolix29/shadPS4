package com.bachatas4.android.runtime.diagnostics

import java.nio.file.Files
import java.nio.file.Path
import java.util.stream.Collectors

/**
 * Retention policy for generated diagnostic ZIP reports.
 * Does not touch per-session log directories (SessionLog.prune handles those).
 */
object DiagnosticRetention {
    const val MAX_REPORT_COUNT = 3
    const val MAX_TOTAL_BYTES = 50L * 1024L * 1024L // 50 MB

    /**
     * Delete abandoned `.tmp` files and enforce count + byte caps.
     * @param protectedPaths absolute paths that must not be deleted (e.g. active share).
     */
    fun cleanup(
        reportsDirectory: Path,
        maxCount: Int = MAX_REPORT_COUNT,
        maxTotalBytes: Long = MAX_TOTAL_BYTES,
        protectedPaths: Set<Path> = emptySet(),
    ) {
        if (!Files.isDirectory(reportsDirectory)) return
        val protected = protectedPaths.map { it.toAbsolutePath().normalize() }.toSet()

        // Abandoned temporary files first.
        listFiles(reportsDirectory)
            .filter { it.fileName.toString().endsWith(".tmp") || it.fileName.toString().endsWith(".zip.tmp") }
            .filter { it.toAbsolutePath().normalize() !in protected }
            .forEach { runCatching { Files.deleteIfExists(it) } }

        val zips = listFiles(reportsDirectory)
            .filter { it.fileName.toString().endsWith(".zip") && Files.isRegularFile(it) }
            .sortedByDescending { Files.getLastModifiedTime(it).toMillis() }

        // Count limit: keep newest maxCount.
        zips.drop(maxCount.coerceAtLeast(0))
            .filter { it.toAbsolutePath().normalize() !in protected }
            .forEach { runCatching { Files.deleteIfExists(it) } }

        // Byte limit: drop oldest first among remaining.
        var remaining = listFiles(reportsDirectory)
            .filter { it.fileName.toString().endsWith(".zip") && Files.isRegularFile(it) }
            .sortedByDescending { Files.getLastModifiedTime(it).toMillis() }
            .toMutableList()
        var total = remaining.sumOf { runCatching { Files.size(it) }.getOrDefault(0L) }
        while (total > maxTotalBytes && remaining.isNotEmpty()) {
            val victim = remaining.removeAt(remaining.lastIndex)
            if (victim.toAbsolutePath().normalize() in protected) continue
            val size = runCatching { Files.size(victim) }.getOrDefault(0L)
            if (runCatching { Files.deleteIfExists(victim) }.getOrDefault(false)) {
                total -= size
            }
        }
    }

    private fun listFiles(directory: Path): List<Path> {
        if (!Files.isDirectory(directory)) return emptyList()
        return Files.list(directory).use { stream ->
            stream.collect(Collectors.toList())
        }
    }
}
