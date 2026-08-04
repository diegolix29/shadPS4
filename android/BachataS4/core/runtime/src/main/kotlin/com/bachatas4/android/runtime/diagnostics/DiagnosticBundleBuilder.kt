package com.bachatas4.android.runtime.diagnostics

import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.ByteArrayInputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths
import java.nio.file.StandardCopyOption
import java.security.DigestOutputStream
import java.security.MessageDigest
import java.time.Instant
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

/**
 * Builds a privacy-sanitized diagnostic ZIP from a retained session directory.
 */
class DiagnosticBundleBuilder(
    private val reportsDirectory: Path,
    private val json: Json = DEFAULT_JSON,
    private val maxLogBytes: Long = DEFAULT_MAX_LOG_BYTES,
    private val clock: () -> Instant = Instant::now,
) {
    data class BuildRequest(
        val context: DiagnosticReportContext,
        val includeScreenshot: Boolean = false,
        val screenshotPath: Path? = null,
        val userDescription: String? = null,
        val appRoot: File,
        val gameRoot: File? = null,
        val packageName: String? = null,
        val deviceSerial: String? = null,
    )

    data class BuildResult(
        val reportId: String,
        val zipPath: Path,
        val report: DiagnosticReport,
        val manifest: DiagnosticManifest,
        val sizeBytes: Long,
    )

    fun build(request: BuildRequest): BuildResult {
        val context = request.context
        val reportId = context.reportId
        require(DiagnosticReportIds.isValid(reportId) || reportId.startsWith("BS4-")) {
            "Invalid report id"
        }
        Files.createDirectories(reportsDirectory)
        DiagnosticRetention.cleanup(reportsDirectory)

        val sessionDir = Paths.get(context.sessionDirectory)
        val redactor = DiagnosticRedactor(
            appRoot = request.appRoot.canonicalPath,
            gameRoot = request.gameRoot?.canonicalPath,
            packageName = request.packageName,
            deviceSerial = request.deviceSerial,
        )

        val createdAt = clock().toString()
        val attachmentMeta = mutableListOf<DiagnosticAttachment>()
        val manifestEntries = mutableListOf<DiagnosticManifestEntry>()
        val pendingEntries = mutableListOf<PreparedEntry>()

        fun prepareText(name: String, logicalSource: String, content: String, required: Boolean) {
            val redacted = redactor.redactText(content)
            val bytes = redacted.toByteArray(StandardCharsets.UTF_8)
            val (payload, truncated) = truncateBytes(bytes, maxLogBytes)
            pendingEntries += PreparedEntry(name, logicalSource, payload, truncated)
            attachmentMeta += DiagnosticAttachment(
                filename = name,
                present = true,
                required = required,
                logicalSource = logicalSource,
                byteSize = payload.size.toLong(),
                truncated = truncated,
            )
        }

        fun prepareFile(name: String, logicalSource: String, source: Path?, required: Boolean) {
            if (source == null || !Files.isRegularFile(source)) {
                attachmentMeta += DiagnosticAttachment(
                    filename = name,
                    present = false,
                    required = required,
                    logicalSource = logicalSource,
                )
                return
            }
            val tmp = Files.createTempFile(reportsDirectory, "entry-", ".tmp")
            try {
                val result = FileInputStream(source.toFile()).use { input ->
                    FileOutputStream(tmp.toFile()).use { output ->
                        redactor.redactStream(BufferedInputStream(input), output, maxLogBytes)
                    }
                }
                val bytes = Files.readAllBytes(tmp)
                pendingEntries += PreparedEntry(name, logicalSource, bytes, result.truncated)
                attachmentMeta += DiagnosticAttachment(
                    filename = name,
                    present = true,
                    required = required,
                    logicalSource = logicalSource,
                    byteSize = bytes.size.toLong(),
                    truncated = result.truncated,
                )
            } finally {
                Files.deleteIfExists(tmp)
            }
        }

        // Required / optional session logs
        prepareFile("application.log", "session.application", sessionDir.resolve("application.log"), required = true)
        prepareFile("shadps4.log", "session.shadps4", sessionDir.resolve("shadps4.log"), required = true)
        prepareFile(
            "shadps4-internal.log",
            "session.shadps4-internal",
            sessionDir.resolve("shadps4-internal.log"),
            required = false,
        )
        prepareFile("runtime.log", "session.runtime", sessionDir.resolve("runtime.log"), required = false)
        prepareFile("session.json", "session.metadata", sessionDir.resolve("session.json"), required = false)

        val processExit = context.termination
        prepareText(
            "process-exit.json",
            "process.exit",
            json.encodeToString(ProcessTerminationInfo.serializer(), processExit),
            required = true,
        )

        val settingsJson = context.settingsJson
            ?: """{"note":"settings snapshot unavailable"}"""
        prepareText("settings.json", "session.settings", settingsJson, required = false)

        if (request.includeScreenshot && request.screenshotPath != null && Files.isRegularFile(request.screenshotPath)) {
            val bytes = Files.readAllBytes(request.screenshotPath)
            pendingEntries += PreparedEntry("screenshot.webp", "session.screenshot", bytes, truncated = false)
            attachmentMeta += DiagnosticAttachment(
                filename = "screenshot.webp",
                present = true,
                required = false,
                logicalSource = "session.screenshot",
                byteSize = bytes.size.toLong(),
                truncated = false,
            )
        } else {
            attachmentMeta += DiagnosticAttachment(
                filename = "screenshot.webp",
                present = false,
                required = false,
                logicalSource = "session.screenshot",
            )
        }

        val report = DiagnosticReport(
            reportId = reportId,
            createdAtUtc = createdAt,
            app = context.app,
            game = DiagnosticGameInfo(
                title = context.gameTitle,
                cusaId = context.cusaId,
                baseVersion = context.baseVersion,
                updateVersion = context.updateVersion,
            ),
            device = context.device,
            driver = context.driver,
            execution = DiagnosticExecutionInfo(
                guestBackend = context.guestBackend,
                lastCheckpoint = context.lastCheckpoint,
                firstFrameReached = context.firstFrameReached,
                userRequestedStop = context.userRequestedStop,
                failedProcess = context.termination.processRole.name.lowercase(),
                terminationKind = context.termination.terminationKind.name.lowercase(),
                exitCode = context.termination.exitCode,
                rawWaitStatus = context.termination.rawWaitStatus,
                signalNumber = context.termination.signalNumber,
                signalName = context.termination.signalName,
                coreDumped = context.termination.coreDumped,
                runtimeErrorCode = context.runtimeErrorCode ?: context.termination.runtimeErrorCode,
                processStartUtc = context.processStartUtc ?: context.termination.processStartUtc,
                processEndUtc = context.processEndUtc ?: context.termination.processEndUtc,
            ),
            attachments = attachmentMeta,
            privacy = DiagnosticPrivacyInfo(
                redactionVersion = DiagnosticRedactor.REDACTION_VERSION,
                screenshotIncluded = request.includeScreenshot &&
                    attachmentMeta.any { it.filename == "screenshot.webp" && it.present },
                automaticUpload = false,
            ),
            userDescription = request.userDescription?.takeIf { it.isNotBlank() },
        )

        val reportBytes = json.encodeToString(DiagnosticReport.serializer(), report)
            .toByteArray(StandardCharsets.UTF_8)
        pendingEntries.add(0, PreparedEntry("report.json", "report", reportBytes, truncated = false))

        // Build ZIP to temp then atomic rename.
        val finalName = "bachata-diagnostic-$reportId.zip"
        require(isSafeZipName(finalName)) { "Unsafe ZIP name" }
        val finalPath = reportsDirectory.resolve(finalName)
        val tmpPath = reportsDirectory.resolve("$finalName.tmp")
        Files.deleteIfExists(tmpPath)

        val seenNames = mutableSetOf<String>()
        ZipOutputStream(BufferedOutputStream(FileOutputStream(tmpPath.toFile()))).use { zip ->
            for (entry in pendingEntries) {
                require(isSafeZipEntryName(entry.name)) { "Unsafe ZIP entry: ${entry.name}" }
                require(entry.name !in seenNames) { "Duplicate ZIP entry: ${entry.name}" }
                seenNames += entry.name
                val digest = MessageDigest.getInstance("SHA-256")
                zip.putNextEntry(ZipEntry(entry.name))
                val digestOut = DigestOutputStream(zip, digest)
                digestOut.write(entry.bytes)
                digestOut.flush()
                zip.closeEntry()
                manifestEntries += DiagnosticManifestEntry(
                    filename = entry.name,
                    byteSize = entry.bytes.size.toLong(),
                    sha256 = digest.digest().toHex(),
                    truncated = entry.truncated,
                    logicalSource = entry.logicalSource,
                    redactionVersion = DiagnosticRedactor.REDACTION_VERSION,
                )
            }

            val manifest = DiagnosticManifest(
                reportId = reportId,
                createdAtUtc = createdAt,
                redactionVersion = DiagnosticRedactor.REDACTION_VERSION,
                entries = manifestEntries,
            )
            val manifestBytes = json.encodeToString(DiagnosticManifest.serializer(), manifest)
                .toByteArray(StandardCharsets.UTF_8)
            require("manifest.json" !in seenNames)
            val digest = MessageDigest.getInstance("SHA-256")
            zip.putNextEntry(ZipEntry("manifest.json"))
            val digestOut = DigestOutputStream(zip, digest)
            digestOut.write(manifestBytes)
            digestOut.flush()
            zip.closeEntry()
            manifestEntries += DiagnosticManifestEntry(
                filename = "manifest.json",
                byteSize = manifestBytes.size.toLong(),
                sha256 = digest.digest().toHex(),
                truncated = false,
                logicalSource = "manifest",
            )
        }

        // Rewrite ZIP once more is expensive; instead write final manifest hash into report is optional.
        // Re-open approach: include final manifest as last entry (already done). Build result manifest
        // object from collected entries.
        val finalManifest = DiagnosticManifest(
            reportId = reportId,
            createdAtUtc = createdAt,
            redactionVersion = DiagnosticRedactor.REDACTION_VERSION,
            entries = manifestEntries.toList(),
        )

        try {
            Files.move(
                tmpPath,
                finalPath,
                StandardCopyOption.REPLACE_EXISTING,
                StandardCopyOption.ATOMIC_MOVE,
            )
        } catch (_: java.nio.file.AtomicMoveNotSupportedException) {
            Files.move(tmpPath, finalPath, StandardCopyOption.REPLACE_EXISTING)
        }
        DiagnosticRetention.cleanup(reportsDirectory, protectedPaths = setOf(finalPath))

        return BuildResult(
            reportId = reportId,
            zipPath = finalPath,
            report = report,
            manifest = finalManifest,
            sizeBytes = Files.size(finalPath),
        )
    }

    private data class PreparedEntry(
        val name: String,
        val logicalSource: String,
        val bytes: ByteArray,
        val truncated: Boolean,
    )

    companion object {
        const val DEFAULT_MAX_LOG_BYTES = 4L * 1024L * 1024L // 4 MB
        const val TARGET_ZIP_MAX_BYTES = 15L * 1024L * 1024L

        private val DEFAULT_JSON = Json {
            ignoreUnknownKeys = true
            encodeDefaults = true
            prettyPrint = true
        }

        private val SAFE_ENTRY = Regex("^[A-Za-z0-9._-]+$")

        fun isSafeZipEntryName(name: String): Boolean {
            if (name.isBlank() || name.length > 128) return false
            if (name.contains("..") || name.contains('/') || name.contains('\\')) return false
            return SAFE_ENTRY.matches(name)
        }

        fun isSafeZipName(name: String): Boolean = isSafeZipEntryName(name)

        fun truncateBytes(bytes: ByteArray, maxBytes: Long): Pair<ByteArray, Boolean> {
            if (bytes.size.toLong() <= maxBytes) return bytes to false
            val head = (maxBytes * 0.25).toInt().coerceAtLeast(1)
            val tail = (maxBytes - head).toInt().coerceAtLeast(1)
            val marker = "\n... [truncated for diagnostic size limits] ...\n\n".toByteArray(StandardCharsets.UTF_8)
            val out = ByteArray(head + marker.size + tail)
            System.arraycopy(bytes, 0, out, 0, head)
            System.arraycopy(marker, 0, out, head, marker.size)
            System.arraycopy(bytes, bytes.size - tail, out, head + marker.size, tail)
            return out to true
        }

        private fun ByteArray.toHex(): String =
            joinToString("") { b -> "%02x".format(b) }
    }
}
