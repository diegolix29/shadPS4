package com.bachatas4.android.feature.session.diagnostics

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.core.content.FileProvider
import java.io.File
import java.nio.file.Path

/**
 * Secure share/save helpers for diagnostic ZIP archives.
 * Authority must be `${applicationId}.diagnostics`.
 */
object DiagnosticShare {
    const val PATH_SEGMENT = "diagnostic-reports"
    const val MIME_ZIP = "application/zip"

    fun authority(packageName: String): String = "$packageName.diagnostics"

    fun reportsDirectory(context: Context): File =
        File(context.cacheDir, PATH_SEGMENT).also { it.mkdirs() }

    fun reportsDirectoryPath(context: Context): Path = reportsDirectory(context).toPath()

    fun contentUri(context: Context, zipFile: File): Uri {
        require(zipFile.isFile) { "Report ZIP missing: ${zipFile.name}" }
        val reports = reportsDirectory(context).canonicalFile
        val canonical = zipFile.canonicalFile
        require(canonical.path.startsWith(reports.path + File.separator) || canonical == reports) {
            "ZIP outside diagnostic-reports cache"
        }
        return FileProvider.getUriForFile(context, authority(context.packageName), canonical)
    }

    fun createShareIntent(context: Context, zipFile: File, subject: String? = null): Intent {
        val uri = contentUri(context, zipFile)
        return Intent(Intent.ACTION_SEND).apply {
            type = MIME_ZIP
            putExtra(Intent.EXTRA_STREAM, uri)
            subject?.let { putExtra(Intent.EXTRA_SUBJECT, it) }
            clipData = ClipData.newUri(context.contentResolver, zipFile.name, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
    }

    fun createChooser(context: Context, zipFile: File, reportId: String): Intent {
        val share = createShareIntent(
            context,
            zipFile,
            subject = "Bachata S4 diagnostic report $reportId",
        )
        return Intent.createChooser(share, "Share diagnostic report")
    }

    fun createSaveDocumentIntent(reportId: String): Intent =
        Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = MIME_ZIP
            putExtra(Intent.EXTRA_TITLE, "bachata-diagnostic-$reportId.zip")
        }
}
