package com.bachatas4.android.runtime.diagnostics

import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

class SessionLog internal constructor(
    val directory: Path,
) {
    val applicationLog: Path = directory.resolve("application.log")
    val backendLog: Path = directory.resolve("shadps4.log")

    fun info(component: String, message: String) = write(component, "Info", message)

    fun warning(component: String, message: String) = write(component, "Warning", message)

    fun error(component: String, message: String) = write(component, "Error", message)

    @Synchronized
    private fun write(component: String, level: String, message: String) {
        val safeComponent = component.replace(Regex("[^A-Za-z0-9_.-]"), "_")
        val safeMessage = message.replace('\n', ' ').replace('\r', ' ')
        val line = "[${Instant.now()}] [App.$safeComponent] <$level> $safeMessage\n"
        Files.newBufferedWriter(
            applicationLog,
            StandardOpenOption.CREATE,
            StandardOpenOption.APPEND,
        ).use { it.write(line) }
    }

    companion object {
        private val sessionTime = DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss").withZone(ZoneOffset.UTC)

        fun create(root: Path, gameId: String, startedAt: Instant, suffix: String): SessionLog {
            Files.createDirectories(root)
            val safeGameId = gameId.replace(Regex("[^A-Za-z0-9._-]"), "_").ifBlank { "unknown" }
            val safeSuffix = suffix.replace(Regex("[^A-Za-z0-9]"), "").take(12).ifBlank { "session" }
            val directory = root.resolve("${sessionTime.format(startedAt)}-$safeGameId-$safeSuffix")
            Files.createDirectories(directory)
            return SessionLog(directory)
        }

        fun prune(root: Path, keep: Int) {
            require(keep >= 0)
            if (!Files.isDirectory(root)) return
            val directories = Files.list(root).use { stream ->
                stream.iterator().asSequence().filter(Files::isDirectory).sorted().toList()
            }
            directories.dropLast(keep).forEach(::deleteDirectory)
        }

        private fun deleteDirectory(directory: Path) {
            Files.walk(directory).use { paths ->
                paths.sorted(Comparator.reverseOrder()).forEach(Files::deleteIfExists)
            }
        }
    }
}
