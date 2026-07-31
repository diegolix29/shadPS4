package com.bachatas4.android.designsystem

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.bachatas4.android.designsystem.theme.BachataPalette

@Composable
fun BachataScreenHeader(
    title: String,
    modifier: Modifier = Modifier,
    onBack: (() -> Unit)? = null,
    actions: @Composable RowScope.() -> Unit = {},
) {
    Row(
        modifier = modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (onBack != null) {
            TextButton(onClick = onBack) { Text("‹", style = MaterialTheme.typography.headlineMedium) }
        }
        Text(
            text = title,
            modifier = Modifier.weight(1f),
            style = MaterialTheme.typography.headlineSmall,
            fontWeight = FontWeight.Bold,
            color = BachataPalette.Primary,
        )
        actions()
    }
}

@Composable
fun BachataPanel(
    modifier: Modifier = Modifier,
    color: Color = BachataPalette.Surface,
    content: @Composable () -> Unit,
) {
    Surface(
        modifier = modifier,
        color = color,
        shape = RoundedCornerShape(12.dp),
        content = content,
    )
}

@Composable
fun BachataPrimaryButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    content: @Composable RowScope.() -> Unit,
) {
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier,
        colors = ButtonDefaults.buttonColors(
            containerColor = BachataPalette.Accent,
            contentColor = BachataPalette.OnAccent,
            disabledContainerColor = BachataPalette.RaisedSurface,
            disabledContentColor = BachataPalette.Secondary,
        ),
        shape = RoundedCornerShape(10.dp),
        content = content,
    )
}

@Composable
fun BachataActionBar(
    vararg hints: String,
    modifier: Modifier = Modifier,
) {
    BachataPanel(
        modifier = modifier.fillMaxWidth(),
        color = BachataPalette.Canvas,
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .windowInsetsPadding(WindowInsets.navigationBars)
                .padding(horizontal = 16.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            hints.forEach { hint ->
                Surface(
                    color = BachataPalette.RaisedSurface,
                    shape = RoundedCornerShape(999.dp),
                ) {
                    Text(
                        hint,
                        modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                        style = MaterialTheme.typography.labelMedium,
                        color = BachataPalette.Primary,
                    )
                }
            }
        }
    }
}

@Composable
fun ForwardFab(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    FloatingActionButton(
        onClick = onClick,
        shape = CircleShape,
        containerColor = BachataPalette.Accent,
        contentColor = BachataPalette.OnAccent,
        modifier = modifier.size(64.dp),
    ) {
        Box(
            contentAlignment = Alignment.Center,
            modifier = Modifier.fillMaxSize(),
        ) {
            Text(
                text = "›",
                color = BachataPalette.OnAccent,
                style = TextStyle(
                    fontSize = 40.sp,
                    fontWeight = FontWeight.Bold,
                    textAlign = TextAlign.Center,
                    platformStyle = PlatformTextStyle(includeFontPadding = false),
                ),
            )
        }
    }
}
