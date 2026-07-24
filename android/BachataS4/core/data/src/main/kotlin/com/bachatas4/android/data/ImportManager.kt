package com.bachatas4.android.data

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

sealed interface ImportProgress {
    data object Idle : ImportProgress

    /** Tree scan / permission setup before copy starts. */
    data object Preparing : ImportProgress

    data class Scanning(val folderName: String) : ImportProgress

    data class Extracting(
        val bytesCopied: Long,
        val totalBytes: Long,
        val currentFile: String,
        val gameTitle: String,
    ) : ImportProgress

    data class Copying(
        val bytesCopied: Long,
        val totalBytes: Long,
        val currentFile: String,
        val gameTitle: String,
    ) : ImportProgress

    data class Finalizing(val title: String) : ImportProgress

    data class NeedPasscode(val contentId: String, val titleHint: String?) : ImportProgress

    /**
     * PKG import paused before local cache copy so the user can confirm
     * there is enough free storage for package + extract peak usage.
     */
    data class NeedCopyConfirm(
        val contentId: String,
        val titleHint: String?,
        val packageBytes: Long,
        val extractBytes: Long,
        val requiredBytes: Long,
        val freeBytes: Long,
    ) : ImportProgress

    data class Success(val gameId: String, val title: String) : ImportProgress

    data class Failed(val message: String) : ImportProgress
}

object ImportManager {
    const val ACTION_IMPORT = "com.bachatas4.android.action.IMPORT_GAME"
    const val ACTION_CANCEL = "com.bachatas4.android.action.CANCEL_IMPORT"
    const val ACTION_SUBMIT_PASSCODE = "com.bachatas4.android.action.SUBMIT_PASSCODE"
    const val ACTION_CONFIRM_PKG_COPY = "com.bachatas4.android.action.CONFIRM_PKG_COPY"
    const val EXTRA_URI = "source_uri"
    const val EXTRA_MODE = "import_mode"
    const val EXTRA_PASSCODE = "passcode"
    const val MODE_FOLDER = "folder"
    const val MODE_PKG = "pkg"
    const val SERVICE_CLASS = "com.bachatas4.android.service.ImportService"

    private val _progress = MutableStateFlow<ImportProgress>(ImportProgress.Idle)
    val progress: StateFlow<ImportProgress> = _progress

    fun isBusy(state: ImportProgress = _progress.value): Boolean =
        state is ImportProgress.Preparing ||
            state is ImportProgress.Scanning ||
            state is ImportProgress.Extracting ||
            state is ImportProgress.Copying ||
            state is ImportProgress.Finalizing ||
            state is ImportProgress.NeedPasscode ||
            state is ImportProgress.NeedCopyConfirm

    /**
     * Atomically claim the single import slot and enter [ImportProgress.Preparing].
     * Returns false when an import is already in progress.
     */
    fun tryBeginImport(): Boolean {
        while (true) {
            val current = _progress.value
            if (isBusy(current)) return false
            if (_progress.compareAndSet(current, ImportProgress.Preparing)) return true
        }
    }

    fun update(state: ImportProgress) {
        _progress.value = state
    }

    fun reset() {
        _progress.value = ImportProgress.Idle
    }
}
