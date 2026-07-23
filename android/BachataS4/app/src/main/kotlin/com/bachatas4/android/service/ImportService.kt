package com.bachatas4.android.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.net.Uri
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.documentfile.provider.DocumentFile
import com.bachatas4.android.MainActivity
import com.bachatas4.android.data.ContentImportRequest
import com.bachatas4.android.data.ContentImporter
import com.bachatas4.android.data.ContentTreeEntry
import com.bachatas4.android.data.GameMetadataResolver
import com.bachatas4.android.data.GameRepository
import com.bachatas4.android.data.ImportManager
import com.bachatas4.android.data.ImportProgress
import com.bachatas4.android.data.ParamSfoReader
import com.bachatas4.android.data.PkgKeyStore
import com.bachatas4.android.runtime.pkg.PkgExtractor
import com.bachatas4.android.runtime.pkg.PkgStatus
import dagger.hilt.android.AndroidEntryPoint
import java.io.File
import java.util.UUID
import javax.inject.Inject
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Imports a user-selected game folder or PS4 `.pkg` into app storage.
 *
 * Runs as a normal (non-foreground) service. Progress updates [ImportManager]
 * and optional status-bar notifications (no startForeground).
 */
@AndroidEntryPoint
class ImportService : Service() {
    @Inject lateinit var contentImporter: ContentImporter
    @Inject lateinit var gameRepository: GameRepository
    @Inject lateinit var pkgKeyStore: PkgKeyStore

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var importJob: Job? = null
    private var passcodeWaiter: CompletableDeferred<String?>? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ImportManager.ACTION_CANCEL -> {
                Log.i(TAG, "cancel requested")
                PkgExtractor.nativeCancel()
                passcodeWaiter?.complete(null)
                importJob?.cancel()
                getSystemService(NotificationManager::class.java).cancel(NOTIFICATION_ID)
                if (importJob?.isActive != true) {
                    ImportManager.reset()
                    stopSelf()
                }
                return START_NOT_STICKY
            }
            ImportManager.ACTION_SUBMIT_PASSCODE -> {
                val code = intent.getStringExtra(ImportManager.EXTRA_PASSCODE)
                Log.i(TAG, "passcode submitted (len=${code?.length ?: 0})")
                passcodeWaiter?.complete(code)
                return START_NOT_STICKY
            }
            ImportManager.ACTION_IMPORT -> {
                val uriString = intent.getStringExtra(ImportManager.EXTRA_URI) ?: run {
                    Log.e(TAG, "import missing source URI")
                    ImportManager.update(ImportProgress.Failed("Missing source URI"))
                    stopSelf()
                    return START_NOT_STICKY
                }
                val mode = intent.getStringExtra(ImportManager.EXTRA_MODE) ?: ImportManager.MODE_FOLDER
                Log.i(TAG, "import start mode=$mode uri=$uriString")
                if (importJob?.isActive == true) {
                    Log.w(TAG, "import already running — ignore new request")
                    return START_NOT_STICKY
                }
                if (!ImportManager.tryBeginImport()) {
                    ImportManager.reset()
                    if (!ImportManager.tryBeginImport()) {
                        Log.w(TAG, "import slot busy — abort")
                        return START_NOT_STICKY
                    }
                }
                importJob = scope.launch {
                    when (mode) {
                        ImportManager.MODE_PKG -> runPkgImport(uriString)
                        else -> runFolderImport(uriString)
                    }
                }
            }
        }
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        importJob?.cancel()
        passcodeWaiter?.complete(null)
        if (ImportManager.isBusy()) {
            ImportManager.reset()
        }
        scope.cancel()
        super.onDestroy()
    }

    private suspend fun runFolderImport(uriString: String) {
        Log.i(TAG, "folder import prepare uri=$uriString")
        updateNotification("Preparing import…", indeterminate = true)
        try {
            val uri = Uri.parse(uriString)
            runCatching {
                contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }.onFailure { Log.w(TAG, "folder persistable permission: ${it.message}") }

            Log.i(TAG, "folder tree scan start")
            val (folderName, entries) = withContext(Dispatchers.IO) {
                val root = requireNotNull(
                    DocumentFile.fromTreeUri(this@ImportService, uri),
                ) { "Cannot read selected folder" }
                (root.name?.ifBlank { null } ?: "Imported game") to root.toImportEntries()
            }
            Log.i(TAG, "folder tree scan done name=$folderName files=${entries.size}")

            ImportManager.update(ImportProgress.Scanning(folderName))
            updateNotification("Identifying $folderName…", indeterminate = true)

            val sfoEntry = entries.firstOrNull { it.relativePath == "sce_sys/param.sfo" }
            val sfoBytes = sfoEntry?.let { entry ->
                withContext(Dispatchers.IO) {
                    runCatching {
                        contentResolver.openInputStream(Uri.parse(entry.sourceUri))
                            ?.use { it.readBytes() }
                    }.getOrNull()
                }
            }
            val sfo = sfoBytes?.let { ParamSfoReader.parse(it) }
            val resolved = GameMetadataResolver.resolve(folderName = folderName, sfo = sfo)
            Log.i(TAG, "folder copy start id=${resolved.id} title=${resolved.title} files=${entries.size}")

            val result = contentImporter.importGameTree(
                ContentImportRequest(
                    id = resolved.id,
                    title = resolved.title,
                    sourceUri = uriString,
                    subtitle = resolved.subtitle,
                    detail = resolved.detail,
                ),
                entries,
                onProgress = { bytesCopied, totalBytes, currentFile ->
                    ImportManager.update(
                        ImportProgress.Copying(
                            bytesCopied = bytesCopied,
                            totalBytes = totalBytes,
                            currentFile = currentFile,
                            gameTitle = resolved.title,
                        ),
                    )
                    val (max, progress) = scaledProgress(bytesCopied, totalBytes)
                    val sizeText = if (totalBytes > 0) {
                        "${formatBytes(bytesCopied)} / ${formatBytes(totalBytes)}"
                    } else {
                        formatBytes(bytesCopied)
                    }
                    updateNotification(
                        "Importing ${resolved.title} · $sizeText",
                        maxProgress = max,
                        progress = progress,
                        indeterminate = max == 0,
                    )
                },
            )

            gameRepository.addImportedGame(result, uriString, System.currentTimeMillis())
            ImportManager.update(ImportProgress.Success(resolved.id, resolved.title))
            Log.i(TAG, "folder import success id=${resolved.id}")
            notifyDone("${resolved.title} imported")
        } catch (failure: Throwable) {
            handleFailure(failure)
        } finally {
            if (ImportManager.isBusy()) ImportManager.reset()
            Log.i(TAG, "folder import finished (stopSelf)")
            stopSelf()
        }
    }

    private suspend fun runPkgImport(uriString: String) {
        Log.i(TAG, "pkg import prepare uri=$uriString")
        updateNotification("Preparing import…", indeterminate = true)
        var staging: File? = null
        var completed = false
        try {
            val uri = Uri.parse(uriString)
            runCatching {
                contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }.onFailure { Log.w(TAG, "pkg persistable permission: ${it.message}") }

            Log.i(TAG, "pkg openFileDescriptor start")
            val pfd = withContext(Dispatchers.IO) {
                contentResolver.openFileDescriptor(uri, "r")
                    ?: error("Cannot open PKG")
            }
            Log.i(TAG, "pkg openFileDescriptor ok sizeHint=${pfd.statSize}")
            pfd.use { descriptor ->
                val fd = descriptor.fd
                Log.i(TAG, "pkg nativeProbe start fd=$fd")
                val probe = withContext(Dispatchers.IO) { PkgExtractor.nativeProbe(fd) }
                Log.i(
                    TAG,
                    "pkg nativeProbe done status=${probe.status} contentId=${probe.contentId} " +
                        "pkgSize=${probe.packageSize} pfsSize=${probe.pfsImageSize} " +
                        "hint=${probe.titleHint} msg=${probe.message}",
                )
                val displayName = probe.titleHint?.ifBlank { null }
                    ?: probe.contentId.ifBlank { "PKG" }
                ImportManager.update(ImportProgress.Scanning(displayName))
                updateNotification("Identifying $displayName…", indeterminate = true)

                if (probe.status == PkgStatus.ERROR) {
                    error(probe.message ?: "Invalid package")
                }

                val gamesDir = File(filesDir, "games").canonicalFile
                gamesDir.mkdirs()
                val required = probe.pfsImageSize.takeIf { it > 0 } ?: probe.packageSize
                val free = gamesDir.usableSpace
                Log.i(TAG, "pkg space required=$required free=$free")
                if (required > 0 && required > free) {
                    error(
                        "Import requires ${formatBytes(required)} but only " +
                            "${formatBytes(free)} is available",
                    )
                }

                staging = File(gamesDir, ".import-${UUID.randomUUID()}").canonicalFile
                staging!!.mkdirs()
                Log.i(TAG, "pkg staging=${staging!!.absolutePath}")

                var usedPasscode: String? = null
                var extractStatus = PkgStatus.ERROR
                var extractMessage: String? = null

                val candidates = buildList {
                    add(null) // try embedded / zero first via native
                    pkgKeyStore.getPasscode(probe.contentId)?.let { add(it) }
                    add("00000000000000000000000000000000")
                }.distinct()
                Log.i(TAG, "pkg extract candidates=${candidates.size} (passcodes redacted)")

                for ((index, candidate) in candidates.withIndex()) {
                    Log.i(
                        TAG,
                        "pkg extract attempt #$index hasPasscode=${candidate != null} " +
                            "staging=${staging!!.absolutePath}",
                    )
                    val result = withContext(Dispatchers.IO) {
                        extractWithProgress(fd, staging!!, candidate, displayName)
                    }
                    extractStatus = result.status
                    extractMessage = result.message
                    Log.i(TAG, "pkg extract attempt #$index result=${result.status} msg=${result.message}")
                    if (result.status == PkgStatus.OK) {
                        usedPasscode = candidate
                        break
                    }
                    if (result.status == PkgStatus.CANCELLED) throw CancellationException("cancelled")
                    if (result.status != PkgStatus.NEED_PASSCODE) {
                        error(result.message ?: "PKG extract failed")
                    }
                }

                if (extractStatus != PkgStatus.OK) {
                    Log.i(TAG, "pkg need user passcode contentId=${probe.contentId}")
                    ImportManager.update(
                        ImportProgress.NeedPasscode(probe.contentId, probe.titleHint),
                    )
                    updateNotification("Passcode required", indeterminate = true)
                    val waiter = CompletableDeferred<String?>()
                    passcodeWaiter = waiter
                    val userCode = waiter.await()
                    passcodeWaiter = null
                    if (userCode.isNullOrBlank()) {
                        throw CancellationException("cancelled")
                    }
                    Log.i(TAG, "pkg user passcode received len=${userCode.length}")
                    val result = withContext(Dispatchers.IO) {
                        extractWithProgress(fd, staging!!, userCode, displayName)
                    }
                    Log.i(TAG, "pkg extract with user passcode result=${result.status} msg=${result.message}")
                    if (result.status == PkgStatus.CANCELLED) throw CancellationException("cancelled")
                    if (result.status != PkgStatus.OK) {
                        error(result.message ?: "Wrong passcode or extract failed")
                    }
                    usedPasscode = userCode
                }

                ImportManager.update(ImportProgress.Finalizing(displayName))
                updateNotification("Registering game…", indeterminate = true)
                Log.i(TAG, "pkg finalize start")

                val sfoFile = File(staging, "sce_sys/param.sfo")
                val sfo = if (sfoFile.isFile) {
                    runCatching { ParamSfoReader.parse(sfoFile.readBytes()) }.getOrNull()
                } else {
                    null
                }
                val resolved = GameMetadataResolver.resolve(
                    folderName = displayName,
                    sfo = sfo,
                )
                Log.i(TAG, "pkg metadata id=${resolved.id} title=${resolved.title}")

                val result = contentImporter.finalizeStagingTree(
                    ContentImportRequest(
                        id = resolved.id,
                        title = resolved.title,
                        sourceUri = uriString,
                        subtitle = resolved.subtitle,
                        detail = resolved.detail,
                    ),
                    staging!!,
                )
                // finalize moves staging; clear local ref so finally does not delete destination
                staging = null
                completed = true

                if (!usedPasscode.isNullOrBlank() &&
                    usedPasscode != "00000000000000000000000000000000" &&
                    probe.contentId.isNotBlank()
                ) {
                    pkgKeyStore.putPasscode(probe.contentId, usedPasscode)
                    Log.i(TAG, "pkg keydb saved for contentId=${probe.contentId}")
                }

                gameRepository.addImportedGame(result, uriString, System.currentTimeMillis())
                ImportManager.update(ImportProgress.Success(resolved.id, resolved.title))
                Log.i(TAG, "pkg import success id=${resolved.id} bytes=${result.bytesCopied}")
                notifyDone("${resolved.title} imported")
            }
        } catch (failure: Throwable) {
            handleFailure(failure)
        } finally {
            if (!completed) {
                Log.w(TAG, "pkg import cleanup staging=${staging?.absolutePath}")
                staging?.deleteRecursively()
            }
            passcodeWaiter = null
            if (ImportManager.isBusy()) ImportManager.reset()
            Log.i(TAG, "pkg import finished completed=$completed (stopSelf)")
            stopSelf()
        }
    }

    private fun extractWithProgress(
        fd: Int,
        staging: File,
        passcode: String?,
        displayName: String,
    ): com.bachatas4.android.runtime.pkg.PkgExtractResult {
        var lastLogAt = 0L
        Log.i(TAG, "nativeExtract enter fd=$fd out=${staging.absolutePath}")
        val result = PkgExtractor.nativeExtract(
            fd = fd,
            outPath = staging.absolutePath,
            passcode = passcode,
            listener = { bytesDone, totalHint, currentFile ->
                ImportManager.update(
                    ImportProgress.Extracting(
                        bytesCopied = bytesDone,
                        totalBytes = totalHint,
                        currentFile = currentFile,
                        gameTitle = displayName,
                    ),
                )
                val (max, progress) = scaledProgress(bytesDone, totalHint)
                val sizeText = if (totalHint > 0) {
                    "${formatBytes(bytesDone)} / ${formatBytes(totalHint)}"
                } else {
                    formatBytes(bytesDone)
                }
                updateNotification(
                    "Extracting PKG · $sizeText · $currentFile",
                    maxProgress = max,
                    progress = progress,
                    indeterminate = max == 0,
                )
                val now = System.currentTimeMillis()
                if (now - lastLogAt >= 5_000L) {
                    lastLogAt = now
                    Log.i(TAG, "nativeExtract progress $sizeText file=$currentFile")
                }
            },
        )
        Log.i(TAG, "nativeExtract exit status=${result.status} msg=${result.message}")
        return result
    }

    private fun handleFailure(failure: Throwable) {
        if (failure is CancellationException) {
            Log.i(TAG, "import cancelled")
            ImportManager.reset()
            getSystemService(NotificationManager::class.java).cancel(NOTIFICATION_ID)
        } else {
            val message = failure.message ?: failure.javaClass.simpleName
            Log.e(TAG, "import failed: $message", failure)
            ImportManager.update(ImportProgress.Failed(message))
            notifyDone("Import failed: $message", ongoing = false)
        }
    }

    private fun notifyDone(text: String, ongoing: Boolean = false) {
        getSystemService(NotificationManager::class.java).notify(
            NOTIFICATION_ID,
            buildNotification(text, 0, 0, ongoing = ongoing),
        )
    }

    private fun createNotificationChannel() {
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Game Imports", NotificationManager.IMPORTANCE_LOW),
        )
    }

    private fun buildNotification(
        text: String,
        maxProgress: Int,
        progress: Int,
        ongoing: Boolean,
        indeterminate: Boolean = false,
    ): Notification {
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val cancel = PendingIntent.getService(
            this,
            1,
            Intent(ImportManager.ACTION_CANCEL).setClassName(packageName, ImportManager.SERVICE_CLASS),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle("Bachata S4")
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(ongoing)
            .setOnlyAlertOnce(true)
        if (ongoing) {
            builder.addAction(0, "Cancel", cancel)
            if (indeterminate || maxProgress <= 0) {
                builder.setProgress(0, 0, true)
            } else {
                builder.setProgress(maxProgress, progress.coerceIn(0, maxProgress), false)
            }
        }
        return builder.build()
    }

    private fun updateNotification(
        text: String,
        maxProgress: Int = 0,
        progress: Int = 0,
        indeterminate: Boolean = false,
    ) {
        getSystemService(NotificationManager::class.java).notify(
            NOTIFICATION_ID,
            buildNotification(
                text = text,
                maxProgress = maxProgress,
                progress = progress,
                ongoing = true,
                indeterminate = indeterminate,
            ),
        )
    }

    private companion object {
        const val TAG = "BachataImport"
        const val CHANNEL_ID = "import"
        const val NOTIFICATION_ID = 42
        const val PROGRESS_SCALE = 1000

        fun scaledProgress(bytesCopied: Long, totalBytes: Long): Pair<Int, Int> {
            if (totalBytes <= 0L) return 0 to 0
            val progress = ((bytesCopied.toDouble() / totalBytes.toDouble()) * PROGRESS_SCALE)
                .toInt()
                .coerceIn(0, PROGRESS_SCALE)
            return PROGRESS_SCALE to progress
        }

        fun formatBytes(bytes: Long): String {
            if (bytes < 1024) return "$bytes B"
            val kib = bytes / 1024.0
            if (kib < 1024) return "%.1f KB".format(kib)
            val mib = kib / 1024.0
            if (mib < 1024) return "%.1f MB".format(mib)
            return "%.2f GB".format(mib / 1024.0)
        }
    }
}

private fun DocumentFile.toImportEntries(): List<ContentTreeEntry> {
    val entries = mutableListOf<ContentTreeEntry>()
    val pending = ArrayDeque<Pair<DocumentFile, String>>()
    pending.add(this to "")
    while (pending.isNotEmpty()) {
        val (directory, prefix) = pending.removeLast()
        directory.listFiles().forEach { child ->
            val name = child.name ?: return@forEach
            val relativePath = if (prefix.isEmpty()) name else "$prefix/$name"
            when {
                child.isDirectory -> pending.add(child to relativePath)
                child.isFile -> entries.add(
                    ContentTreeEntry(relativePath, child.uri.toString(), child.length().coerceAtLeast(0L)),
                )
            }
        }
    }
    return entries
}
