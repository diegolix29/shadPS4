package com.bachatas4.android.runtime.pkg

data class PkgProbeResult(
    val contentId: String,
    val packageSize: Long,
    val pfsImageSize: Long,
    val titleHint: String?,
    val status: PkgStatus,
    val message: String? = null,
)

enum class PkgStatus {
    OK,
    NEED_PASSCODE,
    CANCELLED,
    ERROR,
}

data class PkgExtractResult(
    val status: PkgStatus,
    val message: String? = null,
    val contentId: String? = null,
)

fun interface PkgProgressListener {
    fun onProgress(bytesDone: Long, totalHint: Long, currentFile: String)
}

object PkgExtractor {
    init {
        System.loadLibrary("bachata_pkg")
    }

    @JvmStatic
    external fun nativeProbe(fd: Int): PkgProbeResult

    @JvmStatic
    external fun nativeExtract(
        fd: Int,
        outPath: String,
        passcode: String?,
        listener: PkgProgressListener?,
    ): PkgExtractResult

    @JvmStatic
    external fun nativeCancel()
}
