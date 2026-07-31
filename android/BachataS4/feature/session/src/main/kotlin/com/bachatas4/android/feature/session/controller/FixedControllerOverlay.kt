package com.bachatas4.android.feature.session.controller

import android.graphics.Paint
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.changedToDown
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.dp
import com.bachatas4.android.designsystem.theme.BachataPalette
import com.bachatas4.android.runtime.input.ControllerSnapshot
import com.bachatas4.android.runtime.input.Ps4Button
import kotlin.math.min
import kotlin.math.roundToInt
import android.graphics.Rect
import android.content.Context
import android.view.View
import android.graphics.Canvas as AndroidCanvas
import androidx.compose.ui.viewinterop.AndroidView

@Composable
fun FixedControllerOverlay(
    modifier: Modifier = Modifier,
    layout: TouchLayout = TouchLayout(),
    faded: Boolean = false,
    onSnapshot: (ControllerSnapshot) -> Unit,
) {
    val state = remember(layout) { TouchControllerState(layout) }
    var snapshot by remember { mutableStateOf(ControllerSnapshot.Neutral) }
    DisposableEffect(state) {
        onDispose { state.cancelAll(); onSnapshot(ControllerSnapshot.Neutral) }
    }
    Box(modifier = modifier.fillMaxSize()) {
        AndroidView(
            modifier = Modifier.fillMaxSize().pointerInput(layout) {
                awaitPointerEventScope {
                    while (true) {
                        val event = awaitPointerEvent()
                        val scaleX = size.width / TouchControllerState.LogicalWidth
                        val scaleY = size.height / TouchControllerState.LogicalHeight
                        event.changes.forEach { change ->
                            val x = change.position.x / scaleX
                            val y = change.position.y / scaleY
                            when {
                                change.changedToDown() -> state.pointerDown(change.id.value, x, y)
                                !change.pressed -> state.pointerUp(change.id.value)
                                else -> state.pointerMove(change.id.value, x, y)
                            }
                            change.consume()
                        }
                        val snap = state.snapshot()
                        snapshot = snap
                        onSnapshot(snap)
                    }
                }
            },
            factory = { ctx ->
                GlassOverlayView(ctx).apply {
                    this.layout = layout
                    this.snapshot = snapshot
                    this.faded = faded
                }
            },
            update = { view ->
                view.layout = layout
                view.snapshot = snapshot
                view.faded = faded
                view.invalidate()
            }
        )
    }
}

private fun visualColor(control: String): Color = when (control) {
    "triangle" -> Color(0xFF5DDE90)
    "circle" -> Color(0xFFF06292)
    "cross" -> Color(0xFF5B9CF6)
    "square" -> Color(0xFFE57EF0)
    "ps" -> Color(0xFF00E5FF)
    else -> Color(0xFFCFD8DC)
}

private fun displayLabel(control: String): String = when (control) {
    "triangle" -> "△"
    "circle" -> "○"
    "cross" -> "✕"
    "square" -> "□"
    "dpad_up" -> "▲"
    "dpad_right" -> "▶"
    "dpad_down" -> "▼"
    "dpad_left" -> "◀"
    "left_stick" -> "L3"
    "right_stick" -> "R3"
    "share" -> "Share"
    "options" -> "Options"
    "ps" -> "PS"
    else -> control.uppercase()
}

private val BUTTONS_MAP = mapOf(
    "triangle" to Ps4Button.TRIANGLE, "circle" to Ps4Button.CIRCLE, "cross" to Ps4Button.CROSS, "square" to Ps4Button.SQUARE,
    "dpad_up" to Ps4Button.UP, "dpad_right" to Ps4Button.RIGHT, "dpad_down" to Ps4Button.DOWN, "dpad_left" to Ps4Button.LEFT,
    "l1" to Ps4Button.L1, "l2" to Ps4Button.L2, "r1" to Ps4Button.R1, "r2" to Ps4Button.R2,
    "touchpad" to Ps4Button.TOUCH_PAD, "options" to Ps4Button.OPTIONS,
    "share" to Ps4Button.SHARE, "ps" to Ps4Button.PS,
)

private class GlassOverlayView(context: Context) : View(context) {
    init {
        setLayerType(LAYER_TYPE_SOFTWARE, null)
    }

    var layout: TouchLayout = TouchLayout()
    var snapshot: ControllerSnapshot = ControllerSnapshot.Neutral
    var faded: Boolean = false

    override fun onDraw(canvas: AndroidCanvas) {
        super.onDraw(canvas)
        val densityScale = min(width / 1920f, height / 1080f)
        val renderAlpha = if (faded) 0f else layout.opacity
        val overlayAlphaInt = (renderAlpha * 255).roundToInt()

        GlassButtonRenderer.scale = densityScale

        layout.controls.filter(TouchControlPlacement::visible).forEach { control ->
            val w = control.width * width * layout.scale
            val h = control.height * height * layout.scale
            val topLeftX = control.centerX * width - w / 2f
            val topLeftY = control.centerY * height - h / 2f
            
            val rect = Rect(
                topLeftX.roundToInt(),
                topLeftY.roundToInt(),
                (topLeftX + w).roundToInt(),
                (topLeftY + h).roundToInt()
            )

            val buttonMask = BUTTONS_MAP[control.control] ?: 0L
            val isPressed = buttonMask != 0L && (snapshot.buttons and buttonMask) != 0L

            val style = TouchControlVisualStyle.forControl(control.control)
            when (style) {
                TouchControlVisualStyle.Stick -> {
                    GlassButtonRenderer.drawStickRing(canvas, rect, overlayAlphaInt)

                    val stickX = if (control.control == "right_stick") snapshot.rightX else snapshot.leftX
                    val stickY = if (control.control == "right_stick") snapshot.rightY else snapshot.leftY
                    val bgR = rect.width() / 2f
                    val nr = (rect.width() * 0.48f / 2f).roundToInt()
                    
                    val cx = rect.exactCenterX()
                    val cy = rect.exactCenterY()
                    
                    val dx = stickX * bgR
                    val dy = stickY * bgR
                    
                    val ncx = cx + dx
                    val ncy = cy + dy
                    
                    val nubRect = Rect(
                        (ncx - nr).roundToInt(),
                        (ncy - nr).roundToInt(),
                        (ncx + nr).roundToInt(),
                        (ncy + nr).roundToInt()
                    )
                    GlassButtonRenderer.drawStickNub(canvas, nubRect, if (control.control == "left_stick") "L3" else "R3", overlayAlphaInt)
                }
                TouchControlVisualStyle.Face -> {
                    GlassButtonRenderer.drawFaceButton(
                        canvas, rect, isPressed,
                        displayLabel(control.control), control.control, overlayAlphaInt
                    )
                }
                TouchControlVisualStyle.Dpad -> {
                    val tl = control.control == "dpad_up" || control.control == "dpad_left"
                    val tr = control.control == "dpad_up" || control.control == "dpad_right"
                    val bl = control.control == "dpad_down" || control.control == "dpad_left"
                    val br = control.control == "dpad_down" || control.control == "dpad_right"
                    GlassButtonRenderer.drawDpadArm(
                        canvas, rect, isPressed, displayLabel(control.control),
                        tl, tr, bl, br, overlayAlphaInt
                    )
                }
                TouchControlVisualStyle.Touchpad -> {
                    GlassButtonRenderer.drawTouchpad(canvas, rect, isPressed, overlayAlphaInt)
                }
                TouchControlVisualStyle.Center -> {
                    when (control.control) {
                        "ps" -> GlassButtonRenderer.drawPsButton(canvas, rect, isPressed, overlayAlphaInt)
                        "share" -> GlassButtonRenderer.drawIconCenterButton(canvas, rect, isPressed, isShare = true, overlayAlpha = overlayAlphaInt)
                        "options" -> GlassButtonRenderer.drawIconCenterButton(canvas, rect, isPressed, isShare = false, overlayAlpha = overlayAlphaInt)
                        else -> GlassButtonRenderer.drawCenterButton(canvas, rect, isPressed, displayLabel(control.control), overlayAlphaInt)
                    }
                }
                else -> {
                    GlassButtonRenderer.drawShoulderButton(
                        canvas, rect, isPressed, displayLabel(control.control), overlayAlphaInt
                    )
                }
            }
        }
    }
}
