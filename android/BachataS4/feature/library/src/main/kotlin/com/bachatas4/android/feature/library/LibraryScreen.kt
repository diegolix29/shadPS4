package com.bachatas4.android.feature.library

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.BitmapFactory
import android.os.Build
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.Surface
import androidx.documentfile.provider.DocumentFile
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.hilt.navigation.compose.hiltViewModel
import com.bachatas4.android.data.GameIconPaths
import com.bachatas4.android.data.GameRepository
import com.bachatas4.android.data.ImportManager
import com.bachatas4.android.data.ImportProgress
import com.bachatas4.android.designsystem.BachataActionBar
import com.bachatas4.android.designsystem.BachataPanel
import com.bachatas4.android.designsystem.BachataPrimaryButton
import com.bachatas4.android.designsystem.theme.BachataPalette
import com.bachatas4.android.runtime.input.GamepadInputManager
import dagger.hilt.EntryPoint
import dagger.hilt.InstallIn
import dagger.hilt.android.EntryPointAccessors
import dagger.hilt.components.SingletonComponent
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch

@Composable
fun LibraryScreen(
    onOpenSettings: () -> Unit,
    onOpenGameSettings: (String) -> Unit = {},
    onLaunch: (String) -> Unit,
    viewModel: LibraryViewModel = hiltViewModel(),
) {
    val context = LocalContext.current
    val dependencies = remember {
        EntryPointAccessors.fromApplication(context.applicationContext, LibraryDependencies::class.java)
    }
    val scope = rememberCoroutineScope()
    val importProgress by ImportManager.progress.collectAsState()
    var gameToDelete by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(dependencies) {
        runCatching { dependencies.gameRepository().syncOrphanedFolders() }
        runCatching { dependencies.gameRepository().backfillTitlesFromSfo() }
        dependencies.gameRepository().observeGames().collectLatest(viewModel::setGames)
    }
    // Success / Failed free the import slot for a new pick; clear Success banner after a short delay.
    LaunchedEffect(importProgress) {
        when (importProgress) {
            is ImportProgress.Success -> {
                delay(4_000)
                if (ImportManager.progress.value is ImportProgress.Success) {
                    ImportManager.reset()
                }
            }
            is ImportProgress.Failed -> {
                delay(8_000)
                if (ImportManager.progress.value is ImportProgress.Failed) {
                    ImportManager.reset()
                }
            }
            else -> Unit
        }
    }
    val notificationPermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { /* optional: import progress notification when user grants POST_NOTIFICATIONS */ }
    LaunchedEffect(Unit) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
    }
    var showImportChooser by remember { mutableStateOf(false) }
    var passcodeInput by remember { mutableStateOf("") }
    val folderPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        if (ImportManager.isBusy()) {
            Toast.makeText(context, "Import already in progress", Toast.LENGTH_SHORT).show()
            return@rememberLauncherForActivityResult
        }
        runCatching {
            context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        val intent = Intent(ImportManager.ACTION_IMPORT).apply {
            setClassName(context.packageName, ImportManager.SERVICE_CLASS)
            putExtra(ImportManager.EXTRA_URI, uri.toString())
            putExtra(ImportManager.EXTRA_MODE, ImportManager.MODE_FOLDER)
        }
        context.startService(intent)
    }
    val pkgPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        if (ImportManager.isBusy()) {
            Toast.makeText(context, "Import already in progress", Toast.LENGTH_SHORT).show()
            return@rememberLauncherForActivityResult
        }
        val name = DocumentFile.fromSingleUri(context, uri)?.name.orEmpty()
        if (!name.endsWith(".pkg", ignoreCase = true)) {
            Toast.makeText(context, "Select a .pkg file", Toast.LENGTH_SHORT).show()
            return@rememberLauncherForActivityResult
        }
        runCatching {
            context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        val intent = Intent(ImportManager.ACTION_IMPORT).apply {
            setClassName(context.packageName, ImportManager.SERVICE_CLASS)
            putExtra(ImportManager.EXTRA_URI, uri.toString())
            putExtra(ImportManager.EXTRA_MODE, ImportManager.MODE_PKG)
        }
        context.startService(intent)
    }
    val requestImport: () -> Unit = {
        if (ImportManager.isBusy()) {
            Toast.makeText(context, "Import already in progress", Toast.LENGTH_SHORT).show()
        } else {
            showImportChooser = true
        }
    }
    if (showImportChooser) {
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { showImportChooser = false },
            title = { Text("Import game") },
            text = { Text("Choose a game folder or a PS4 .pkg package.") },
            confirmButton = {
                TextButton(onClick = {
                    showImportChooser = false
                    folderPicker.launch(null)
                }) { Text("Folder") }
            },
            dismissButton = {
                TextButton(onClick = {
                    showImportChooser = false
                    pkgPicker.launch(arrayOf("*/*"))
                }) { Text("PKG") }
            },
        )
    }
    val needCopyConfirm = importProgress as? ImportProgress.NeedCopyConfirm
    if (needCopyConfirm != null) {
        val enoughSpace = needCopyConfirm.freeBytes >= needCopyConfirm.requiredBytes
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { /* wait for confirm/cancel via service */ },
            title = { Text("Confirm PKG import") },
            text = {
                Column {
                    Text(needCopyConfirm.titleHint ?: needCopyConfirm.contentId.ifBlank { "Selected package" })
                    Text(
                        "PKG files are copied into app storage first, then extracted. " +
                            "Peak free space needed is package + extract size.",
                        modifier = Modifier.padding(top = 8.dp),
                    )
                    Text(
                        "Package: ${formatBytes(needCopyConfirm.packageBytes)}\n" +
                            "Extract: ${formatBytes(needCopyConfirm.extractBytes)}\n" +
                            "Needed: ${formatBytes(needCopyConfirm.requiredBytes)}\n" +
                            "Free: ${formatBytes(needCopyConfirm.freeBytes)}",
                        modifier = Modifier.padding(top = 8.dp),
                    )
                    if (!enoughSpace) {
                        Text(
                            "Not enough free storage. Free space or cancel this import.",
                            modifier = Modifier.padding(top = 8.dp),
                            color = Color(0xFFFFB4AB),
                        )
                    }
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        val intent = Intent(ImportManager.ACTION_CONFIRM_PKG_COPY).apply {
                            setClassName(context.packageName, ImportManager.SERVICE_CLASS)
                        }
                        context.startService(intent)
                    },
                    enabled = enoughSpace,
                ) { Text(if (enoughSpace) "Continue" else "Not enough space") }
            },
            dismissButton = {
                TextButton(onClick = {
                    val intent = Intent(ImportManager.ACTION_CANCEL).apply {
                        setClassName(context.packageName, ImportManager.SERVICE_CLASS)
                    }
                    context.startService(intent)
                }) { Text("Cancel") }
            },
        )
    }
    val needPasscode = importProgress as? ImportProgress.NeedPasscode
    if (needPasscode != null) {
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { /* wait for submit/cancel via service */ },
            title = { Text("PKG passcode") },
            text = {
                Column {
                    Text(needPasscode.titleHint ?: needPasscode.contentId)
                    androidx.compose.material3.OutlinedTextField(
                        value = passcodeInput,
                        onValueChange = { passcodeInput = it.take(32) },
                        singleLine = true,
                        label = { Text("32-character passcode") },
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    val intent = Intent(ImportManager.ACTION_SUBMIT_PASSCODE).apply {
                        setClassName(context.packageName, ImportManager.SERVICE_CLASS)
                        putExtra(ImportManager.EXTRA_PASSCODE, passcodeInput)
                    }
                    context.startService(intent)
                    passcodeInput = ""
                }) { Text("Submit") }
            },
            dismissButton = {
                TextButton(onClick = {
                    val intent = Intent(ImportManager.ACTION_CANCEL).apply {
                        setClassName(context.packageName, ImportManager.SERVICE_CLASS)
                    }
                    context.startService(intent)
                    passcodeInput = ""
                }) { Text("Cancel") }
            },
        )
    }
    val onLaunchWithTracking: (String) -> Unit = { id ->
        if (id == "__import_card__") {
            requestImport()
        } else {
            scope.launch {
                runCatching { dependencies.gameRepository().updateLastLaunched(id) }
                onLaunch(id)
            }
        }
    }
    DisposableEffect(viewModel) {
        viewModel.attachNavListener()
        onDispose { GamepadInputManager.unregisterNavListener() }
    }
    LaunchedEffect(viewModel) {
        viewModel.launch.collect { id ->
            if (id == "__import_card__") {
                requestImport()
            } else {
                runCatching { dependencies.gameRepository().updateLastLaunched(id) }
                onLaunch(id)
            }
        }
    }
    LaunchedEffect(viewModel) {
        viewModel.openSettings.collect { id ->
            onOpenGameSettings(id)
        }
    }
    val state by viewModel.state.collectAsState()
    LibraryContent(
        state = state,
        importProgress = importProgress,
        gameToDelete = gameToDelete,
        onOpenSettings = onOpenSettings,
        onOpenGameSettings = onOpenGameSettings,
        onSelectGame = viewModel::selectGame,
        onImport = requestImport,
        onLaunch = onLaunchWithTracking,
        onRequestDelete = { gameToDelete = it },
        onConfirmDelete = { id ->
            scope.launch {
                runCatching { dependencies.gameRepository().deleteGame(id) }
                gameToDelete = null
            }
        },
        onDismissDelete = { gameToDelete = null },
        onShowDetails = viewModel::showDetails,
        onSetNumColumns = viewModel::setNumColumns,
    )
}

@Composable
fun LibraryContent(
    state: LibraryUiState,
    importProgress: ImportProgress,
    gameToDelete: String?,
    onOpenSettings: () -> Unit,
    onOpenGameSettings: (String) -> Unit,
    onSelectGame: (String) -> Unit,
    onImport: () -> Unit,
    onLaunch: (String) -> Unit,
    onRequestDelete: (String) -> Unit,
    onConfirmDelete: (String) -> Unit,
    onDismissDelete: () -> Unit,
    onShowDetails: (String?) -> Unit,
    onSetNumColumns: (Int) -> Unit,
) {
    val selected = state.games.firstOrNull { it.id == state.selectedGameId }
    val context = LocalContext.current
    val isImporting = ImportManager.isBusy(importProgress)
    val selectedCoverBitmap = remember(selected?.relativePath) {
        if (selected == null) null else {
            val file = GameIconPaths.icon0(context.filesDir, selected.relativePath)
            if (!file.isFile) null else {
                runCatching { BitmapFactory.decodeFile(file.absolutePath) }.getOrNull()
            }
        }
    }
    Scaffold(
        containerColor = BachataPalette.Canvas,
        bottomBar = {
            if (state.showDetailsGameId == null) {
                BachataActionBar(
                    "A  LAUNCH",
                    "X  DETAILS",
                )
            }
        },
    ) { contentPadding ->
        BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
            val localMaxHeight = maxHeight
            val cols = ((maxWidth - 32.dp + 12.dp) / (148.dp + 12.dp)).toInt().coerceAtLeast(1)
            LaunchedEffect(cols) {
                onSetNumColumns(cols)
            }

            if (selectedCoverBitmap != null) {
                Image(
                    bitmap = selectedCoverBitmap.asImageBitmap(),
                    contentDescription = null,
                    modifier = Modifier
                        .fillMaxSize()
                        .drawWithContent {
                            drawContent()
                            drawRect(
                                brush = Brush.verticalGradient(
                                    colors = listOf(
                                        Color.Transparent,
                                        BachataPalette.Canvas
                                    ),
                                    startY = size.height * 0.30f,
                                    endY = size.height * 0.65f
                                )
                            )
                            drawRect(
                                color = BachataPalette.Canvas,
                                topLeft = Offset(0f, size.height * 0.65f),
                                size = Size(size.width, size.height * 0.35f)
                            )
                        },
                    alpha = 0.22f,
                    contentScale = ContentScale.Crop,
                )
            }


            LazyVerticalGrid(
                columns = GridCells.Adaptive(minSize = 148.dp),
                modifier = Modifier
                    .fillMaxSize()
                    .padding(contentPadding),
                contentPadding = PaddingValues(16.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                item(span = { GridItemSpan(maxLineSpan) }) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                    ) {
                        androidx.compose.ui.viewinterop.AndroidView(
                            modifier = Modifier.size(32.dp),
                            factory = { viewContext ->
                                android.widget.ImageView(viewContext).apply {
                                    setImageResource(viewContext.applicationInfo.icon)
                                    contentDescription = "Bachata S4 logo"
                                }
                            }
                        )
                        Text(
                            text = "Library",
                            modifier = Modifier.weight(1f),
                            style = MaterialTheme.typography.headlineSmall,
                            fontWeight = FontWeight.Bold,
                            color = BachataPalette.Primary,
                        )
                        TextButton(onClick = onOpenSettings) {
                            Text("⚙", color = BachataPalette.Primary, style = MaterialTheme.typography.titleLarge)
                        }
                    }
                }

                when (val progress = importProgress) {
                    is ImportProgress.Preparing -> {
                        item(span = { GridItemSpan(maxLineSpan) }) {
                            ImportStatusCard(
                                title = "Preparing import…",
                                subtitle = "Scanning game folder. Large titles can take a while.",
                                indeterminate = true,
                            )
                        }
                    }
                    is ImportProgress.Scanning -> {
                        item(span = { GridItemSpan(maxLineSpan) }) {
                            ImportStatusCard(
                                title = "Identifying ${progress.folderName}…",
                                subtitle = "Reading game metadata",
                                indeterminate = true,
                            )
                        }
                    }
                    is ImportProgress.NeedCopyConfirm -> {
                        item(span = { GridItemSpan(maxLineSpan) }) {
                            val enough = progress.freeBytes >= progress.requiredBytes
                            ImportStatusCard(
                                title = "Confirm storage for ${progress.titleHint ?: "PKG"}",
                                subtitle = "Needed ${formatBytes(progress.requiredBytes)} · " +
                                    "Free ${formatBytes(progress.freeBytes)}" +
                                    if (enough) "" else " · not enough space",
                                detail = "Package ${formatBytes(progress.packageBytes)} + " +
                                    "extract ${formatBytes(progress.extractBytes)}",
                                indeterminate = true,
                            )
                        }
                    }
                    is ImportProgress.Copying -> {
                        item(span = { GridItemSpan(maxLineSpan) }) {
                            val fraction = if (progress.totalBytes > 0) {
                                (progress.bytesCopied.toFloat() / progress.totalBytes.toFloat())
                                    .coerceIn(0f, 1f)
                            } else {
                                0f
                            }
                            val percent = (fraction * 100f).toInt()
                            val isPkgCache = progress.currentFile == "Local PKG cache"
                            ImportStatusCard(
                                title = if (isPkgCache) {
                                    "Copying ${progress.gameTitle} to device"
                                } else {
                                    "Importing ${progress.gameTitle}"
                                },
                                subtitle = if (progress.totalBytes > 0) {
                                    "${formatBytes(progress.bytesCopied)} / ${formatBytes(progress.totalBytes)} · $percent%"
                                } else {
                                    formatBytes(progress.bytesCopied)
                                },
                                detail = progress.currentFile,
                                progress = fraction,
                                indeterminate = progress.totalBytes <= 0,
                            )
                        }
                    }
                    is ImportProgress.Extracting -> {
                        item(span = { GridItemSpan(maxLineSpan) }) {
                            val fraction = if (progress.totalBytes > 0) {
                                (progress.bytesCopied.toFloat() / progress.totalBytes.toFloat())
                                    .coerceIn(0f, 1f)
                            } else {
                                0f
                            }
                            val percent = (fraction * 100f).toInt()
                            ImportStatusCard(
                                title = "Extracting ${progress.gameTitle}",
                                subtitle = if (progress.totalBytes > 0) {
                                    "${formatBytes(progress.bytesCopied)} / ${formatBytes(progress.totalBytes)} · $percent%"
                                } else {
                                    formatBytes(progress.bytesCopied)
                                },
                                detail = progress.currentFile,
                                progress = fraction,
                                indeterminate = progress.totalBytes <= 0,
                            )
                        }
                    }
                    is ImportProgress.Finalizing -> {
                        item(span = { GridItemSpan(maxLineSpan) }) {
                            ImportStatusCard(
                                title = "Registering ${progress.title}…",
                                subtitle = "Writing game library entry",
                                indeterminate = true,
                            )
                        }
                    }
                    is ImportProgress.NeedPasscode -> {
                        item(span = { GridItemSpan(maxLineSpan) }) {
                            ImportStatusCard(
                                title = "Passcode required",
                                subtitle = progress.titleHint ?: progress.contentId,
                                indeterminate = true,
                            )
                        }
                    }
                    is ImportProgress.Success -> {
                        item(span = { GridItemSpan(maxLineSpan) }) {
                            Text(
                                "${progress.title} imported",
                                color = BachataPalette.Accent,
                                style = MaterialTheme.typography.titleMedium,
                            )
                        }
                    }
                    is ImportProgress.Failed -> {
                        item(span = { GridItemSpan(maxLineSpan) }) {
                            Surface(
                                modifier = Modifier.fillMaxWidth(),
                                color = Color(0xFF3A1E21).copy(alpha = 0.5f),
                                shape = RoundedCornerShape(12.dp),
                                border = androidx.compose.foundation.BorderStroke(1.dp, Color(0xFFFFB4AB).copy(alpha = 0.3f)),
                            ) {
                                Text(
                                    "Import failed: ${progress.message}",
                                    modifier = Modifier.padding(16.dp),
                                    color = Color(0xFFFFB4AB),
                                )
                            }
                        }
                    }
                    is ImportProgress.Idle -> { /* nothing */ }
                }

                if (state.games.isNotEmpty()) {
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(
                                "All Games",
                                modifier = Modifier.weight(1f),
                                style = MaterialTheme.typography.headlineSmall,
                                fontWeight = FontWeight.Bold,
                                color = BachataPalette.Primary,
                            )
                        }
                    }
                }

                items(state.games, key = { it.id }) { game ->
                    val isSelected = game.id == selected?.id
                    LibraryGameCard(
                        game = game,
                        selected = isSelected,
                        onClick = {
                            onSelectGame(game.id)
                            onShowDetails(game.id)
                        }
                    )
                }

                item(key = "import_card") {
                    ImportGameCard(
                        onClick = {
                            if (isImporting) return@ImportGameCard
                            onSelectGame("__import_card__")
                            onImport()
                        },
                        selected = state.selectedGameId == "__import_card__",
                        enabled = !isImporting,
                    )
                }
            }

            val showDetailsId = state.showDetailsGameId
            val detailsGame = remember(showDetailsId, state.games) {
                state.games.firstOrNull { it.id == showDetailsId }
            }

            AnimatedVisibility(
                visible = detailsGame != null,
                enter = fadeIn(),
                exit = fadeOut(),
                modifier = Modifier.fillMaxSize()
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .background(Color.Black.copy(alpha = 0.6f))
                        .clickable(
                            interactionSource = remember { androidx.compose.foundation.interaction.MutableInteractionSource() },
                            indication = null
                        ) { onShowDetails(null) }
                )
            }

            AnimatedVisibility(
                visible = detailsGame != null,
                enter = slideInVertically(initialOffsetY = { it }),
                exit = slideOutVertically(targetOffsetY = { it }),
                modifier = Modifier.fillMaxSize()
            ) {
                Box(modifier = Modifier.fillMaxSize()) {
                    if (detailsGame != null) {
                        GlassBottomSheet(
                            game = detailsGame,
                            onLaunch = {
                                onShowDetails(null)
                                onLaunch(detailsGame.id)
                            },
                            onCancel = { onShowDetails(null) },
                            onOpenGameSettings = {
                                onShowDetails(null)
                                onOpenGameSettings(detailsGame.id)
                            },
                            onRequestDelete = {
                                onShowDetails(null)
                                onRequestDelete(detailsGame.id)
                            },
                            maxHeight = localMaxHeight,
                            modifier = Modifier.align(Alignment.BottomCenter)
                        )
                    }
                }
            }
        }
    }

    gameToDelete?.let { id ->
        val game = state.games.firstOrNull { it.id == id }
        AlertDialog(
            onDismissRequest = onDismissDelete,
            title = { Text("Remove game") },
            text = { Text("Delete \"${game?.title ?: id}\" and all of its files? This cannot be undone.") },
            confirmButton = {
                Button(onClick = { onConfirmDelete(id) }) {
                    Text("Remove")
                }
            },
            dismissButton = {
                TextButton(onClick = onDismissDelete) {
                    Text("Cancel")
                }
            },
        )
    }
}

@Composable
private fun LibraryGameCard(
    game: com.bachatas4.android.model.Game,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    GlassMorphicPanel(
        modifier = modifier.clickable { onClick() },
        selected = selected,
    ) {
        Column(modifier = Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            GameCover(
                relativePath = game.relativePath,
                modifier = Modifier.fillMaxWidth().aspectRatio(0.75f),
            )
            Text(
                text = game.title,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                color = BachataPalette.Primary,
                fontWeight = FontWeight.SemiBold,
                style = MaterialTheme.typography.bodyMedium
            )
            val subtitle = game.subtitle
            if (!subtitle.isNullOrBlank()) {
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(4.dp))
                        .background(BachataPalette.Accent.copy(alpha = 0.15f))
                        .padding(horizontal = 6.dp, vertical = 2.dp)
                ) {
                    Text(
                        text = subtitle,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        color = BachataPalette.Accent,
                        style = MaterialTheme.typography.labelSmall,
                        fontWeight = FontWeight.Bold
                    )
                }
            }
        }
    }
}

@Composable
private fun ImportStatusCard(
    title: String,
    subtitle: String,
    detail: String? = null,
    progress: Float = 0f,
    indeterminate: Boolean = false,
) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        color = BachataPalette.Surface.copy(alpha = 0.55f),
        shape = RoundedCornerShape(12.dp),
        border = androidx.compose.foundation.BorderStroke(1.dp, BachataPalette.Accent.copy(alpha = 0.35f)),
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(
                title,
                color = BachataPalette.Primary,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
            )
            if (indeterminate) {
                LinearProgressIndicator(
                    modifier = Modifier.fillMaxWidth(),
                    color = BachataPalette.Accent,
                )
            } else {
                LinearProgressIndicator(
                    progress = { progress.coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth(),
                    color = BachataPalette.Accent,
                )
            }
            Text(
                subtitle,
                color = BachataPalette.Secondary,
                style = MaterialTheme.typography.bodySmall,
            )
            if (!detail.isNullOrBlank()) {
                Text(
                    detail,
                    color = BachataPalette.Secondary,
                    style = MaterialTheme.typography.bodySmall,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

@Composable
private fun ImportGameCard(
    onClick: () -> Unit,
    selected: Boolean,
    enabled: Boolean = true,
    modifier: Modifier = Modifier,
) {
    val accent = if (enabled) BachataPalette.Accent else BachataPalette.Secondary
    val primary = if (enabled) BachataPalette.Primary else BachataPalette.Secondary
    GlassMorphicPanel(
        modifier = modifier
            .fillMaxWidth()
            .then(
                if (enabled) {
                    Modifier.clickable { onClick() }
                } else {
                    Modifier
                },
            ),
        selected = selected && enabled,
    ) {
        Column(
            modifier = Modifier
                .padding(10.dp)
                .fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(8.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .aspectRatio(0.75f)
                    .clip(RoundedCornerShape(8.dp))
                    .background(BachataPalette.Canvas.copy(alpha = 0.3f)),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = if (enabled) "+" else "…",
                    style = MaterialTheme.typography.displayMedium,
                    color = accent,
                    fontWeight = FontWeight.Bold
                )
            }
            Text(
                text = if (enabled) "Import Game" else "Importing…",
                maxLines = 2,
                color = primary,
                fontWeight = FontWeight.SemiBold,
                style = MaterialTheme.typography.bodyMedium,
                textAlign = TextAlign.Center
            )
        }
    }
}

@Composable
private fun GlassMorphicPanel(
    modifier: Modifier = Modifier,
    selected: Boolean = false,
    content: @Composable () -> Unit,
) {
    Surface(
        modifier = modifier,
        color = if (selected) BachataPalette.RaisedSurface.copy(alpha = 0.75f) else BachataPalette.Surface.copy(alpha = 0.40f),
        shape = RoundedCornerShape(12.dp),
        border = androidx.compose.foundation.BorderStroke(
            width = if (selected) 2.dp else 1.dp,
            color = if (selected) BachataPalette.Accent else Color.White.copy(alpha = 0.12f)
        ),
        content = content,
    )
}

@Composable
private fun ControllerKeyIcon(
    key: String,
    backgroundColor: Color,
    modifier: Modifier = Modifier
) {
    Box(
        modifier = modifier
            .size(20.dp)
            .background(color = backgroundColor, shape = CircleShape),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = key,
            color = Color.White,
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center
        )
    }
}

@Composable
private fun GlassBottomSheet(
    game: com.bachatas4.android.model.Game,
    onLaunch: () -> Unit,
    onCancel: () -> Unit,
    onOpenGameSettings: () -> Unit,
    onRequestDelete: () -> Unit,
    maxHeight: androidx.compose.ui.unit.Dp,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier = modifier
            .fillMaxWidth()
            .heightIn(max = maxHeight * 0.75f),
        color = BachataPalette.Surface.copy(alpha = 0.85f),
        shape = RoundedCornerShape(topStart = 24.dp, topEnd = 24.dp),
        border = androidx.compose.foundation.BorderStroke(
            1.dp,
            Color.White.copy(alpha = 0.15f)
        ),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .windowInsetsPadding(WindowInsets.navigationBars)
                .padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Box(
                modifier = Modifier
                    .width(48.dp)
                    .height(4.dp)
                    .clip(RoundedCornerShape(2.dp))
                    .background(Color.White.copy(alpha = 0.3f))
            )

            Column(
                modifier = Modifier
                    .weight(1f, fill = false)
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(16.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                GameCover(
                    relativePath = game.relativePath,
                    modifier = Modifier
                        .width(140.dp)
                        .aspectRatio(0.75f)
                )

                Text(
                    text = game.title,
                    style = MaterialTheme.typography.headlineMedium,
                    fontWeight = FontWeight.Bold,
                    color = BachataPalette.Primary,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    textAlign = TextAlign.Center
                )

                val subtitle = game.subtitle
                if (!subtitle.isNullOrBlank()) {
                    Text(
                        text = subtitle,
                        style = MaterialTheme.typography.titleMedium,
                        color = BachataPalette.Secondary,
                        textAlign = TextAlign.Center
                    )
                }

                val detail = game.detail
                if (!detail.isNullOrBlank()) {
                    Text(
                        text = detail,
                        style = MaterialTheme.typography.bodyMedium,
                        color = BachataPalette.Secondary.copy(alpha = 0.9f),
                        textAlign = TextAlign.Center
                    )
                }
            }

            Column(
                modifier = Modifier.fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                Button(
                    onClick = onLaunch,
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(56.dp),
                    colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                        containerColor = BachataPalette.Accent,
                        contentColor = Color.Black
                    ),
                    shape = RoundedCornerShape(14.dp)
                ) {
                    Row(
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        ControllerKeyIcon(key = "A", backgroundColor = Color(0xFF2E7D32))
                        Text("Launch", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold, color = Color.Black)
                    }
                }

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Button(
                        onClick = onCancel,
                        modifier = Modifier
                            .weight(1f)
                            .height(50.dp),
                        colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                            containerColor = Color.White.copy(alpha = 0.10f),
                            contentColor = BachataPalette.Primary
                        ),
                        shape = RoundedCornerShape(12.dp),
                        contentPadding = PaddingValues(horizontal = 8.dp)
                    ) {
                        Row(
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            ControllerKeyIcon(key = "B", backgroundColor = Color(0xFFC62828))
                            Text("Cancel", style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.SemiBold)
                        }
                    }

                    Button(
                        onClick = onOpenGameSettings,
                        modifier = Modifier
                            .weight(1f)
                            .height(50.dp),
                        colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                            containerColor = Color.White.copy(alpha = 0.05f),
                            contentColor = BachataPalette.Primary
                        ),
                        shape = RoundedCornerShape(12.dp),
                        contentPadding = PaddingValues(horizontal = 8.dp)
                    ) {
                        Row(
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            ControllerKeyIcon(key = "X", backgroundColor = Color(0xFF1565C0))
                            Text("Options", style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.SemiBold)
                        }
                    }

                    Button(
                        onClick = onRequestDelete,
                        modifier = Modifier
                            .weight(1f)
                            .height(50.dp),
                        colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                            containerColor = Color(0x33FFB4AB),
                            contentColor = Color(0xFFFFB4AB)
                        ),
                        shape = RoundedCornerShape(12.dp),
                        contentPadding = PaddingValues(horizontal = 8.dp)
                    ) {
                        Row(
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text("🗑", style = MaterialTheme.typography.bodyMedium)
                            Text("Remove", style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.SemiBold)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun GameCover(
    relativePath: String,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val bitmap = remember(relativePath) {
        val file = GameIconPaths.icon0(context.filesDir, relativePath)
        if (!file.isFile) {
            null
        } else {
            runCatching { BitmapFactory.decodeFile(file.absolutePath) }.getOrNull()
        }
    }
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(8.dp))
            .background(BachataPalette.Canvas),
        contentAlignment = Alignment.Center,
    ) {
        if (bitmap != null) {
            Image(
                bitmap = bitmap.asImageBitmap(),
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop,
            )
        } else {
            Text("GAME", color = BachataPalette.Secondary, style = MaterialTheme.typography.labelSmall)
        }
    }
}

private fun formatBytes(bytes: Long): String {
    if (bytes < 1024) return "$bytes B"
    val kib = bytes / 1024.0
    if (kib < 1024) return "%.1f KB".format(kib)
    val mib = kib / 1024.0
    if (mib < 1024) return "%.1f MB".format(mib)
    return "%.2f GB".format(mib / 1024.0)
}

@EntryPoint
@InstallIn(SingletonComponent::class)
interface LibraryDependencies {
    fun gameRepository(): GameRepository
}
