package com.bachatas4.android.runtime.session

import android.view.Surface
import com.bachatas4.android.model.RuntimeErrorCode
import com.bachatas4.android.runtime.diagnostics.DiagnosticReportContext
import com.bachatas4.android.runtime.diagnostics.ProcessTerminationInfo
import com.bachatas4.android.runtime.input.ControllerSnapshot
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

data class RuntimeSurface(
    val surface: Surface,
    val width: Int,
    val height: Int,
)

data class FrameTelemetry(val fps: Float = 0f, val frameTimeMs: Float = 0f)

sealed interface ManagedSessionState {
    data object Idle : ManagedSessionState
    data class Preparing(val stage: String) : ManagedSessionState
    data class Running(val gameId: String) : ManagedSessionState
    data class Failed(
        val code: RuntimeErrorCode,
        val detail: String,
        val reportContext: DiagnosticReportContext? = null,
    ) : ManagedSessionState
    data class Stopped(
        val exitCode: Int?,
        val termination: ProcessTerminationInfo? = null,
        val reportContext: DiagnosticReportContext? = null,
    ) : ManagedSessionState {
        val userRequestedStop: Boolean
            get() = termination?.userRequestedStop == true || reportContext?.userRequestedStop == true
        val isUnexpected: Boolean
            get() = !userRequestedStop && (
                (exitCode != null && exitCode != 0) ||
                    termination?.terminationKind?.let {
                        it != com.bachatas4.android.runtime.diagnostics.TerminationKind.EXITED &&
                            it != com.bachatas4.android.runtime.diagnostics.TerminationKind.CANCELLED_BY_USER
                    } == true
                )
    }
}

object ManagedSession {
    const val ACTION_START = "com.bachatas4.android.action.START_EMULATION"
    const val ACTION_STOP = "com.bachatas4.android.action.STOP_EMULATION"
    const val EXTRA_GAME_ID = "game_id"
    const val EXTRA_GAME_PATH = "game_path"
    const val EXTRA_VULKAN_DRIVER = "vulkan_driver"
    const val SERVICE_CLASS = "com.bachatas4.android.service.EmulationService"

    private val mutableSurface = MutableStateFlow<RuntimeSurface?>(null)
    private val mutableState = MutableStateFlow<ManagedSessionState>(ManagedSessionState.Idle)
    private val controllerSink = AtomicReference<((ControllerSnapshot) -> Unit)?>(null)
    private val controllerSlotSink = AtomicReference<((Int, ControllerSnapshot) -> Unit)?>(null)
    private val mutableFrameTelemetry = MutableStateFlow(FrameTelemetry())
    private val frameTimes = ArrayDeque<Long>()
    private var lastFrameNanos: Long? = null
    val surface: StateFlow<RuntimeSurface?> = mutableSurface
    val state: StateFlow<ManagedSessionState> = mutableState
    val frameTelemetry: StateFlow<FrameTelemetry> = mutableFrameTelemetry

    fun attachSurface(value: RuntimeSurface) { mutableSurface.value = value }
    fun detachSurface(surface: Surface) {
        if (mutableSurface.value?.surface === surface) mutableSurface.value = null
    }
    fun update(value: ManagedSessionState) { mutableState.value = value }
    fun attachControllerSink(sink: (ControllerSnapshot) -> Unit) { controllerSink.set(sink) }
    fun detachControllerSink(sink: (ControllerSnapshot) -> Unit) { controllerSink.compareAndSet(sink, null) }
    fun submitController(snapshot: ControllerSnapshot) { submitController(0, snapshot) }
    fun attachControllerSlotSink(sink: (Int, ControllerSnapshot) -> Unit) { controllerSlotSink.set(sink) }
    fun detachControllerSlotSink(sink: (Int, ControllerSnapshot) -> Unit) { controllerSlotSink.compareAndSet(sink, null) }
    fun submitController(slot: Int, snapshot: ControllerSnapshot) {
        require(slot in 0..3) { "Controller slot must be 0..3" }
        controllerSlotSink.get()?.invoke(slot, snapshot)
        if (slot == 0) controllerSink.get()?.invoke(snapshot)
    }
    @Synchronized
    fun recordPresentedFrame(nowNanos: Long = System.nanoTime()) {
        if (lastFrameNanos?.let { nowNanos <= it } == true) frameTimes.clear()
        lastFrameNanos = nowNanos
        frameTimes.addLast(nowNanos)
        val cutoff = nowNanos - 1_000_000_000L
        while (frameTimes.firstOrNull()?.let { it < cutoff } == true) frameTimes.removeFirst()
        val intervals = frameTimes.size - 1
        if (intervals > 0) {
            val duration = nowNanos - frameTimes.first()
            val fps = intervals * 1_000_000_000f / duration.coerceAtLeast(1L)
            mutableFrameTelemetry.value = FrameTelemetry(fps, 1000f / fps.coerceAtLeast(0.01f))
        }
    }
    @Synchronized
    fun refreshFrameTelemetry(nowNanos: Long = System.nanoTime()) {
        val last = lastFrameNanos ?: return
        val idleNanos = nowNanos - last
        if (idleNanos >= 1_000_000_000L) {
            mutableFrameTelemetry.value = FrameTelemetry(0f, idleNanos / 1_000_000f)
        }
    }
}
