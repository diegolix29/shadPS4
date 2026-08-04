package com.bachatas4.android.runtime.diagnostics

/**
 * Classifies process termination honestly.
 *
 * Java [Process.exitValue] only yields a flattened exit value; it does not preserve
 * raw waitpid status, signal number, or core-dump bits. Do not infer SIGTRAP from 133.
 */
object ProcessTerminationClassifier {
    private val SIGNAL_NAMES = mapOf(
        1 to "SIGHUP",
        2 to "SIGINT",
        3 to "SIGQUIT",
        4 to "SIGILL",
        5 to "SIGTRAP",
        6 to "SIGABRT",
        7 to "SIGBUS",
        8 to "SIGFPE",
        9 to "SIGKILL",
        11 to "SIGSEGV",
        13 to "SIGPIPE",
        14 to "SIGALRM",
        15 to "SIGTERM",
    )

    fun fromJavaExitValue(
        exitCode: Int?,
        userRequestedStop: Boolean,
        processRole: ProcessRole = ProcessRole.BACKEND,
        processStartUtc: String? = null,
        processEndUtc: String? = null,
        runtimeErrorCode: String? = null,
        launchFailed: Boolean = false,
        timedOut: Boolean = false,
    ): ProcessTerminationInfo {
        if (userRequestedStop) {
            return ProcessTerminationInfo(
                processRole = processRole,
                terminationKind = TerminationKind.CANCELLED_BY_USER,
                exitCode = exitCode,
                rawWaitStatus = null,
                signalNumber = null,
                signalName = null,
                coreDumped = null,
                userRequestedStop = true,
                runtimeErrorCode = runtimeErrorCode,
                processStartUtc = processStartUtc,
                processEndUtc = processEndUtc,
            )
        }
        if (launchFailed) {
            return ProcessTerminationInfo(
                processRole = processRole,
                terminationKind = TerminationKind.LAUNCH_FAILED,
                exitCode = exitCode,
                userRequestedStop = false,
                runtimeErrorCode = runtimeErrorCode,
                processStartUtc = processStartUtc,
                processEndUtc = processEndUtc,
            )
        }
        if (timedOut) {
            return ProcessTerminationInfo(
                processRole = processRole,
                terminationKind = TerminationKind.TIMEOUT,
                exitCode = exitCode,
                userRequestedStop = false,
                runtimeErrorCode = runtimeErrorCode,
                processStartUtc = processStartUtc,
                processEndUtc = processEndUtc,
            )
        }
        if (exitCode == null) {
            return ProcessTerminationInfo(
                processRole = processRole,
                terminationKind = TerminationKind.UNKNOWN,
                exitCode = null,
                userRequestedStop = false,
                runtimeErrorCode = runtimeErrorCode,
                processStartUtc = processStartUtc,
                processEndUtc = processEndUtc,
            )
        }
        // Only Java exitValue is available — do not invent signal fields.
        return ProcessTerminationInfo(
            processRole = processRole,
            terminationKind = if (exitCode == 0) TerminationKind.EXITED else TerminationKind.UNKNOWN,
            exitCode = exitCode,
            rawWaitStatus = null,
            signalNumber = null,
            signalName = null,
            coreDumped = null,
            userRequestedStop = false,
            runtimeErrorCode = runtimeErrorCode,
            processStartUtc = processStartUtc,
            processEndUtc = processEndUtc,
        )
    }

    /**
     * Use only when native waitpid status was actually captured.
     * @param rawWaitStatus raw status word from waitpid
     * @param exited true when WIFEXITED
     * @param signaled true when WIFSIGNALED
     * @param exitStatus WEXITSTATUS when exited
     * @param signalNumber WTERMSIG when signaled
     * @param coreDumped WCOREDUMP when available
     */
    fun fromNativeWaitStatus(
        rawWaitStatus: Int,
        exited: Boolean,
        signaled: Boolean,
        exitStatus: Int?,
        signalNumber: Int?,
        coreDumped: Boolean?,
        userRequestedStop: Boolean = false,
        processRole: ProcessRole = ProcessRole.BACKEND,
        processStartUtc: String? = null,
        processEndUtc: String? = null,
        runtimeErrorCode: String? = null,
    ): ProcessTerminationInfo {
        if (userRequestedStop) {
            return fromJavaExitValue(
                exitCode = exitStatus,
                userRequestedStop = true,
                processRole = processRole,
                processStartUtc = processStartUtc,
                processEndUtc = processEndUtc,
                runtimeErrorCode = runtimeErrorCode,
            ).copy(rawWaitStatus = rawWaitStatus)
        }
        return when {
            signaled && signalNumber != null -> ProcessTerminationInfo(
                processRole = processRole,
                terminationKind = TerminationKind.SIGNALED,
                exitCode = null,
                rawWaitStatus = rawWaitStatus,
                signalNumber = signalNumber,
                signalName = signalName(signalNumber),
                coreDumped = coreDumped,
                userRequestedStop = false,
                runtimeErrorCode = runtimeErrorCode,
                processStartUtc = processStartUtc,
                processEndUtc = processEndUtc,
            )
            exited -> ProcessTerminationInfo(
                processRole = processRole,
                terminationKind = TerminationKind.EXITED,
                exitCode = exitStatus,
                rawWaitStatus = rawWaitStatus,
                signalNumber = null,
                signalName = null,
                coreDumped = coreDumped,
                userRequestedStop = false,
                runtimeErrorCode = runtimeErrorCode,
                processStartUtc = processStartUtc,
                processEndUtc = processEndUtc,
            )
            else -> ProcessTerminationInfo(
                processRole = processRole,
                terminationKind = TerminationKind.UNKNOWN,
                exitCode = exitStatus,
                rawWaitStatus = rawWaitStatus,
                signalNumber = null,
                signalName = null,
                coreDumped = coreDumped,
                userRequestedStop = false,
                runtimeErrorCode = runtimeErrorCode,
                processStartUtc = processStartUtc,
                processEndUtc = processEndUtc,
            )
        }
    }

    fun signalName(signalNumber: Int): String =
        SIGNAL_NAMES[signalNumber] ?: "SIG$signalNumber"
}
