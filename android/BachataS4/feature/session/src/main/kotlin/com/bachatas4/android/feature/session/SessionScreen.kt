package com.bachatas4.android.feature.session

import android.content.Intent
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.background
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.viewinterop.AndroidView
import androidx.activity.compose.BackHandler
import androidx.hilt.navigation.compose.hiltViewModel
import com.bachatas4.android.runtime.session.ManagedSession
import com.bachatas4.android.runtime.session.ManagedSessionState
import com.bachatas4.android.runtime.session.RuntimeSurface
import com.bachatas4.android.feature.session.controller.FixedControllerOverlay
import com.bachatas4.android.feature.session.controller.TouchLayout
import com.bachatas4.android.feature.session.controller.TouchLayoutRepository
import com.bachatas4.android.data.RuntimeProfileStore
import com.bachatas4.android.runtime.settings.ProfileScope
import dagger.hilt.EntryPoint
import dagger.hilt.InstallIn
import dagger.hilt.android.EntryPointAccessors
import dagger.hilt.components.SingletonComponent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.Alignment
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.text.font.FontWeight
import com.bachatas4.android.designsystem.theme.BachataPalette
import com.bachatas4.android.runtime.input.ControllerSnapshot
import com.bachatas4.android.runtime.input.Ps4Button
import com.bachatas4.android.runtime.input.GamepadInputManager
import kotlinx.coroutines.delay
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.ui.platform.LocalDensity

@Composable
fun SessionScreen(
    gameId: String,
    onOpenDrivers: () -> Unit = {},
    /** Play builds omit Turnip recovery UI (driver is fixed to the bundled package). */
    showDriverActions: Boolean = true,
    viewModel: SessionViewModel = hiltViewModel(),
) {
    SessionWindowModeEffect()
    val context = LocalContext.current
    val dependencies = remember { EntryPointAccessors.fromApplication(context.applicationContext, TouchLayoutDependencies::class.java) }
    var touchLayout by remember { mutableStateOf(TouchLayout()) }
    val state by viewModel.state.collectAsState()
    val frames by viewModel.frameTelemetry.collectAsState()
    val device by viewModel.deviceTelemetry.collectAsState()

    var faded by remember { mutableStateOf(false) }
    var showFps by remember { mutableStateOf(true) }
    var showStopOverlay by remember { mutableStateOf(false) }
    var notificationMessage by remember { mutableStateOf<String?>(null) }
    var notificationVisible by remember { mutableStateOf(false) }

    LaunchedEffect(gameId) {
        val global = dependencies.runtimeProfileStore().load(ProfileScope.Global)
        val game = dependencies.runtimeProfileStore().load(ProfileScope.Game(gameId))
        touchLayout = dependencies.touchLayoutRepository().load(game.touchLayoutId ?: global.touchLayoutId)
        viewModel.launch(gameId)
    }

    LaunchedEffect(state) {
        if (state != ManagedSessionState.Idle) {
            notificationMessage = state.label()
            notificationVisible = true
            delay(5000)
            notificationVisible = false
        }
    }

    BackHandler(enabled = state is ManagedSessionState.Running) {
        showStopOverlay = !showStopOverlay
    }

    BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
        val density = LocalDensity.current
        val topOffset = maxHeight * 0.3f
        val topOffsetPx = with(density) { topOffset.toPx() }

        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { currentContext ->
                SurfaceView(currentContext).also { view ->
                    view.holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) = Unit
                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                            if (width > 0 && height > 0) {
                                ManagedSession.attachSurface(RuntimeSurface(holder.surface, width, height))
                            }
                        }
                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            ManagedSession.detachSurface(holder.surface)
                        }
                    })
                }
            },
        )

        FixedControllerOverlay(
            layout = touchLayout,
            faded = faded,
            onSnapshot = { snapshot ->
                if ((snapshot.buttons and Ps4Button.PS) != 0L) {
                    showStopOverlay = true
                }
                if (showStopOverlay) {
                    ManagedSession.submitController(ControllerSnapshot.Neutral)
                } else if (!GamepadInputManager.hasPhysicalController) {
                    ManagedSession.submitController(snapshot)
                }
            }
        )

        if (showFps) {
            Text(
                text = "FPS %.1f (%.1f ms)  RAM %d/%d MB".format(
                    frames.fps, frames.frameTimeMs, device.ramUsedMb, device.ramTotalMb,
                ),
                color = Color.White,
                modifier = Modifier
                    .align(Alignment.BottomStart)
                    .padding(12.dp)
                    .clip(RoundedCornerShape(24.dp))
                    .background(Color.Black.copy(alpha = 0.65f))
                    .padding(horizontal = 14.dp, vertical = 8.dp),
            )
        }

        if (
            showDriverActions &&
            (state as? ManagedSessionState.Failed)?.detail?.contains("not installed") == true
        ) {
            Column(
                modifier = Modifier.align(androidx.compose.ui.Alignment.BottomEnd).padding(12.dp),
                horizontalAlignment = androidx.compose.ui.Alignment.End,
            ) {
                Button(onClick = onOpenDrivers) { Text("Open Turnip drivers") }
            }
        }

        // Notification pill sliding down from top, staying 5s, vanishing moving down
        AnimatedVisibility(
            visible = notificationVisible && notificationMessage != null,
            enter = slideInVertically(initialOffsetY = { -it - topOffsetPx.toInt() }) + fadeIn(),
            exit = slideOutVertically(targetOffsetY = { it * 3 }) + fadeOut(),
            modifier = Modifier.align(Alignment.TopCenter).padding(top = topOffset)
        ) {
            notificationMessage?.let { msg ->
                NotificationPill(message = msg)
            }
        }

        // Overlay with stop button when clicking PS button
        if (showStopOverlay) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.75f))
                    .clickable { showStopOverlay = false },
                contentAlignment = Alignment.Center
            ) {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    modifier = Modifier
                        .background(BachataPalette.Surface, shape = RoundedCornerShape(16.dp))
                        .border(1.dp, BachataPalette.Secondary.copy(alpha = 0.3f), RoundedCornerShape(16.dp))
                        .padding(28.dp)
                        .clickable(enabled = false) {}
                ) {
                    Text(
                        text = "Emulation Interrupted",
                        color = BachataPalette.Primary,
                        style = androidx.compose.material3.MaterialTheme.typography.headlineSmall,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier.padding(bottom = 8.dp)
                    )
                    Text(
                        text = "Do you want to stop the current session?",
                        color = BachataPalette.Secondary,
                        style = androidx.compose.material3.MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.padding(bottom = 24.dp)
                    )

                    // Overlay Settings (Show/Hide overlay switch)
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.padding(bottom = 12.dp)
                    ) {
                        Text(
                            text = "Show/Hide Controller overlay",
                            color = BachataPalette.Secondary,
                            style = androidx.compose.material3.MaterialTheme.typography.bodyMedium,
                            modifier = Modifier.padding(end = 16.dp)
                        )
                        Button(
                            onClick = { faded = !faded },
                            colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                                containerColor = if (faded) BachataPalette.Accent else BachataPalette.RaisedSurface,
                                contentColor = if (faded) BachataPalette.OnAccent else BachataPalette.Primary
                            )
                        ) {
                            Text(if (faded) "Hidden" else "Shown")
                        }
                    }

                    // Overlay Settings (Show FPS overlay switch)
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.padding(bottom = 24.dp)
                    ) {
                        Text(
                            text = "Show FPS telemetry",
                            color = BachataPalette.Secondary,
                            style = androidx.compose.material3.MaterialTheme.typography.bodyMedium,
                            modifier = Modifier.padding(end = 16.dp)
                        )
                        Button(
                            onClick = { showFps = !showFps },
                            colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                                containerColor = if (showFps) BachataPalette.Accent else BachataPalette.RaisedSurface,
                                contentColor = if (showFps) BachataPalette.OnAccent else BachataPalette.Primary
                            )
                        ) {
                            Text(if (showFps) "Show" else "Hide")
                        }
                    }

                    Row {
                        Button(
                            onClick = { showStopOverlay = false },
                            colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                                containerColor = BachataPalette.RaisedSurface,
                                contentColor = BachataPalette.Primary
                            ),
                            modifier = Modifier.padding(end = 12.dp)
                        ) {
                            Text("Resume")
                        }
                        Button(
                            onClick = {
                                context.startService(
                                    Intent(ManagedSession.ACTION_STOP).setClassName(context.packageName, ManagedSession.SERVICE_CLASS),
                                )
                                showStopOverlay = false
                            },
                            colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                                containerColor = Color(0xFFD32F2F),
                                contentColor = Color.White
                            )
                        ) {
                            Text("Stop")
                        }
                        Button(
                            onClick = {
                                if (state is ManagedSessionState.Running || state is ManagedSessionState.Preparing) {
                                    context.startService(
                                        Intent(ManagedSession.ACTION_STOP).setClassName(context.packageName, ManagedSession.SERVICE_CLASS),
                                    )
                                }
                                (context as? android.app.Activity)?.finish()
                            },
                            colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                                containerColor = BachataPalette.RaisedSurface,
                                contentColor = BachataPalette.Primary
                            ),
                            modifier = Modifier.padding(start = 12.dp)
                        ) {
                            Text("Exit")
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun NotificationPill(message: String) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(24.dp))
            .background(
                Brush.horizontalGradient(
                    colors = listOf(
                        Color(0xFF26262B),
                        Color(0xFF1C1C20)
                    )
                )
            )
            .border(
                1.dp,
                Brush.horizontalGradient(
                    colors = listOf(
                        BachataPalette.Accent,
                        BachataPalette.Secondary
                    )
                ),
                RoundedCornerShape(24.dp)
            )
            .padding(horizontal = 24.dp, vertical = 12.dp)
    ) {
        Text(
            text = message,
            color = BachataPalette.Primary,
            style = androidx.compose.material3.MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.SemiBold
        )
    }
}

@EntryPoint
@InstallIn(SingletonComponent::class)
interface TouchLayoutDependencies {
    fun runtimeProfileStore(): RuntimeProfileStore
    fun touchLayoutRepository(): TouchLayoutRepository
}

private fun ManagedSessionState.label(): String = when (this) {
    ManagedSessionState.Idle -> "Idle"
    is ManagedSessionState.Preparing -> "Preparing: $stage"
    is ManagedSessionState.Running -> "Running: $gameId"
    is ManagedSessionState.Failed -> "Error: $detail"
    is ManagedSessionState.Stopped -> "Stopped: ${exitCode ?: "unknown"}"
}
