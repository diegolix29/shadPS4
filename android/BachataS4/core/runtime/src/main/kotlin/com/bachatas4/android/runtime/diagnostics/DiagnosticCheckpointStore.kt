package com.bachatas4.android.runtime.diagnostics

import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.time.Instant
import java.util.concurrent.atomic.AtomicReference
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

/**
 * Persists startup checkpoints for a single emulation session.
 * Updates in-memory state, appends to application.log, and atomically rewrites session.json.
 */
class DiagnosticCheckpointStore(
    private val sessionDirectory: Path,
    private val sessionLog: SessionLog? = null,
    private val json: Json = DEFAULT_JSON,
    private val clock: () -> Instant = Instant::now,
) {
    private val current = AtomicReference<DiagnosticCheckpoint?>(null)
    private val history = AtomicReference<List<CheckpointRecord>>(emptyList())

    val lastCheckpoint: DiagnosticCheckpoint?
        get() = current.get()

    val metadataFile: Path
        get() = sessionDirectory.resolve(SESSION_METADATA_NAME)

    fun mark(checkpoint: DiagnosticCheckpoint) {
        current.set(checkpoint)
        val record = CheckpointRecord(checkpoint.name, clock().toString())
        history.updateAndGet { it + record }
        sessionLog?.info("Diagnostics", "checkpoint=${checkpoint.name}")
        persistSafely()
    }

    fun snapshot(): SessionMetadata = SessionMetadata(
        lastCheckpoint = current.get()?.name,
        checkpoints = history.get(),
        updatedAtUtc = clock().toString(),
    )

    fun load(): SessionMetadata? {
        val file = metadataFile
        if (!Files.isRegularFile(file)) return null
        return runCatching {
            val text = Files.readAllBytes(file).toString(Charsets.UTF_8)
            json.decodeFromString(SessionMetadata.serializer(), text)
        }.getOrNull()
    }

    private fun persistSafely() {
        runCatching {
            Files.createDirectories(sessionDirectory)
            val payload = json.encodeToString(SessionMetadata.serializer(), snapshot())
            val tmp = sessionDirectory.resolve("$SESSION_METADATA_NAME.tmp")
            Files.write(
                tmp,
                payload.toByteArray(Charsets.UTF_8),
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING,
                StandardOpenOption.WRITE,
            )
            try {
                Files.move(
                    tmp,
                    metadataFile,
                    StandardCopyOption.REPLACE_EXISTING,
                    StandardCopyOption.ATOMIC_MOVE,
                )
            } catch (_: java.nio.file.AtomicMoveNotSupportedException) {
                Files.move(tmp, metadataFile, StandardCopyOption.REPLACE_EXISTING)
            }
        }.onFailure {
            // Never crash emulation for diagnostic I/O.
            sessionLog?.warning("Diagnostics", "checkpoint persist failed: ${it.message.orEmpty()}")
            runCatching { Files.deleteIfExists(sessionDirectory.resolve("$SESSION_METADATA_NAME.tmp")) }
        }
    }

    @Serializable
    data class CheckpointRecord(
        val checkpoint: String,
        val atUtc: String,
    )

    @Serializable
    data class SessionMetadata(
        val lastCheckpoint: String? = null,
        val checkpoints: List<CheckpointRecord> = emptyList(),
        val updatedAtUtc: String? = null,
    )

    companion object {
        const val SESSION_METADATA_NAME = "session.json"
        private val DEFAULT_JSON = Json {
            ignoreUnknownKeys = true
            encodeDefaults = true
            prettyPrint = true
        }
    }
}
