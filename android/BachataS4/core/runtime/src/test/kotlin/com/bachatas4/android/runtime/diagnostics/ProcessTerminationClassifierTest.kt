package com.bachatas4.android.runtime.diagnostics

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ProcessTerminationClassifierTest {
    @Test
    fun javaExit133RemainsUnknownWithoutSignalInference() {
        val info = ProcessTerminationClassifier.fromJavaExitValue(exitCode = 133, userRequestedStop = false)
        assertEquals(TerminationKind.UNKNOWN, info.terminationKind)
        assertEquals(133, info.exitCode)
        assertNull(info.rawWaitStatus)
        assertNull(info.signalNumber)
        assertNull(info.signalName)
        assertNull(info.coreDumped)
    }

    @Test
    fun normalZeroExitIsExited() {
        val info = ProcessTerminationClassifier.fromJavaExitValue(exitCode = 0, userRequestedStop = false)
        assertEquals(TerminationKind.EXITED, info.terminationKind)
        assertEquals(0, info.exitCode)
    }

    @Test
    fun userRequestedStopIsCancelled() {
        val info = ProcessTerminationClassifier.fromJavaExitValue(exitCode = 143, userRequestedStop = true)
        assertEquals(TerminationKind.CANCELLED_BY_USER, info.terminationKind)
        assertTrue(info.userRequestedStop)
        assertNull(info.signalName)
    }

    @Test
    fun launchFailureClassified() {
        val info = ProcessTerminationClassifier.fromJavaExitValue(
            exitCode = null,
            userRequestedStop = false,
            launchFailed = true,
        )
        assertEquals(TerminationKind.LAUNCH_FAILED, info.terminationKind)
    }

    @Test
    fun timeoutClassified() {
        val info = ProcessTerminationClassifier.fromJavaExitValue(
            exitCode = null,
            userRequestedStop = false,
            timedOut = true,
        )
        assertEquals(TerminationKind.TIMEOUT, info.terminationKind)
    }

    @Test
    fun nativeSignaledProducesSignalFields() {
        val info = ProcessTerminationClassifier.fromNativeWaitStatus(
            rawWaitStatus = 133,
            exited = false,
            signaled = true,
            exitStatus = null,
            signalNumber = 5,
            coreDumped = false,
        )
        assertEquals(TerminationKind.SIGNALED, info.terminationKind)
        assertEquals(5, info.signalNumber)
        assertEquals("SIGTRAP", info.signalName)
        assertEquals(133, info.rawWaitStatus)
        assertNull(info.exitCode)
    }

    @Test
    fun nativeExitedPreservesExitStatus() {
        val info = ProcessTerminationClassifier.fromNativeWaitStatus(
            rawWaitStatus = 0x7f00,
            exited = true,
            signaled = false,
            exitStatus = 127,
            signalNumber = null,
            coreDumped = false,
        )
        assertEquals(TerminationKind.EXITED, info.terminationKind)
        assertEquals(127, info.exitCode)
        assertNull(info.signalNumber)
    }
}
