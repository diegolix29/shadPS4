package com.bachatas4.android.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.os.IBinder
import androidx.core.app.NotificationCompat
import android.os.Build
import com.bachatas4.android.BuildConfig
import com.bachatas4.android.MainActivity
import com.bachatas4.android.RuntimeLaunchProfileProvider
import com.bachatas4.android.model.RuntimeErrorCode
import com.bachatas4.android.runtime.diagnostics.DiagnosticAppInfo
import com.bachatas4.android.runtime.diagnostics.DiagnosticCheckpoint
import com.bachatas4.android.runtime.diagnostics.DiagnosticCheckpointStore
import com.bachatas4.android.runtime.diagnostics.DiagnosticDeviceInfo
import com.bachatas4.android.runtime.diagnostics.DiagnosticDriverInfo
import com.bachatas4.android.runtime.diagnostics.DiagnosticReportContext
import com.bachatas4.android.runtime.diagnostics.DiagnosticReportIds
import com.bachatas4.android.runtime.diagnostics.ProcessRole
import com.bachatas4.android.runtime.diagnostics.ProcessTerminationClassifier
import com.bachatas4.android.runtime.diagnostics.SessionLog
import com.bachatas4.android.runtime.config.ShadPs4ConfigManager
import com.bachatas4.android.runtime.display.WinlatorEmbeddedXServer
import com.bachatas4.android.runtime.install.RuntimeInstaller
import com.bachatas4.android.runtime.install.RuntimeManifest
import com.bachatas4.android.runtime.input.ControllerFrameEncoder
import com.bachatas4.android.runtime.input.ControllerSnapshot
import com.bachatas4.android.runtime.process.RuntimeProcessHandle
import com.bachatas4.android.runtime.process.RuntimeProcessLauncher
import com.bachatas4.android.runtime.process.RuntimeProcessRequest
import com.bachatas4.android.runtime.settings.RuntimeGuestBackend
import com.bachatas4.android.runtime.process.RuntimeVulkanDriver
import com.bachatas4.android.runtime.process.RuntimeVulkanDriverIds
import com.bachatas4.android.runtime.process.VulkanDriverConfiguration
import com.bachatas4.android.runtime.vortek.VortekSessionSupport
import com.bachatas4.android.runtime.vortek.failureCategory
import dagger.hilt.android.AndroidEntryPoint
import com.bachatas4.android.runtime.session.ManagedSession
import com.bachatas4.android.runtime.session.ManagedSessionState
import com.bachatas4.android.runtime.session.FrameTelemetryReporter
import com.winlator.xconnector.UnixSocketConfig
import java.io.File
import java.nio.file.FileAlreadyExistsException
import java.nio.file.Path
import java.nio.file.Paths
import java.nio.file.StandardCopyOption
import java.time.Instant
import java.util.UUID
import java.util.concurrent.TimeUnit
import java.util.concurrent.Executors
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicBoolean
import com.bachatas4.android.runtime.input.GamepadInputManager
import javax.inject.Inject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.Json

@AndroidEntryPoint
class EmulationService : Service() {
    @Inject lateinit var launchProfileProvider: RuntimeLaunchProfileProvider

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var sessionJob: Job? = null
    @Volatile private var process: RuntimeProcessHandle? = null
    private val userRequestedStop = AtomicBoolean(false)

    override fun onCreate() {
        super.onCreate()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ManagedSession.ACTION_STOP -> stopSession()
            ManagedSession.ACTION_START -> {
                if (sessionJob?.isActive == true) {
                    return START_NOT_STICKY
                } else {
                    val gameId = intent.getStringExtra(ManagedSession.EXTRA_GAME_ID).orEmpty()
                    val gamePath = intent.getStringExtra(ManagedSession.EXTRA_GAME_PATH).orEmpty()
                    val driverName = intent.getStringExtra(ManagedSession.EXTRA_VULKAN_DRIVER)
                        ?: RuntimeVulkanDriver.TURNIP_26_1_0.name
                    sessionJob = scope.launch { runSession(gameId, gamePath, RuntimeVulkanDriver.valueOf(driverName)) }
                }
            }
        }
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        stopSession()
        scope.cancel()
        super.onDestroy()
    }

    private suspend fun runSession(gameId: String, relativePath: String, vulkanDriver: RuntimeVulkanDriver) {
        var xServer: WinlatorEmbeddedXServer? = null
        var boundSocket: LocalSocket? = null
        var serverSocket: LocalServerSocket? = null
        var clientSocket: LocalSocket? = null
        var controllerSink: ((Int, ControllerSnapshot) -> Unit)? = null
        var runtimeRoot: Path? = null
        val acceptExecutor = Executors.newSingleThreadExecutor()
        val controlFile = File(filesDir, "runtime-control.sock").apply { delete() }
        val logsRoot = File(filesDir, "logs").toPath()
        SessionLog.prune(logsRoot, keep = MAX_LOG_SESSIONS - 1)
        userRequestedStop.set(false)
        val sessionLog = SessionLog.create(
            logsRoot,
            gameId,
            Instant.now(),
            UUID.randomUUID().toString().substring(0, 8),
        )
        val checkpointStore = DiagnosticCheckpointStore(sessionLog.directory, sessionLog)
        checkpointStore.mark(DiagnosticCheckpoint.SESSION_CREATED)
        checkpointStore.mark(DiagnosticCheckpoint.SESSION_DIRECTORY_READY)
        val reportId = DiagnosticReportIds.generate()
        val outputFile = sessionLog.backendLog.toFile()
        val telemetryReporter = FrameTelemetryReporter()
        var firstFrameReached = false
        var processStartUtc: String? = null
        var processEndUtc: String? = null
        var guestBackendName = "unknown"
        var driverInfo = DiagnosticDriverInfo(type = "unknown", name = "unknown")
        var settingsJson: String? = null
        var gameTitle = gameId
        var launchFailed = false
        var statePublished = false
        sessionLog.info("Session", "start game=$gameId intentDriver=$vulkanDriver reportId=$reportId")
        GamepadInputManager.onSessionStart()
        sessionLog.info(
            "Device",
            "manufacturer=${android.os.Build.MANUFACTURER} model=${android.os.Build.MODEL} sdk=${android.os.Build.VERSION.SDK_INT}",
        )
        var vortekSession: VortekSessionSupport? = null
        try {
            require(gameId.matches(Regex("[A-Za-z0-9._-]+"))) { "Invalid game id" }
            val gamesRoot = File(filesDir, "games").canonicalFile
            val gameRoot = File(filesDir, relativePath).canonicalFile
            require(gameRoot.toPath().startsWith(gamesRoot.toPath())) { "Game path escapes app storage" }
            val eboot = File(gameRoot, "eboot.bin")
            require(eboot.isFile) { "Imported eboot.bin is missing" }
            sessionLog.info("Content", "validated game root and eboot.bin")
            gameTitle = gameId
            val launchProfile = launchProfileProvider.resolve(gameId)
            settingsJson = "{" +
                "\"schemaVersion\":${launchProfile.schemaVersion}," +
                "\"guestBackend\":\"${launchProfile.guestBackend.name}\"," +
                "\"driverId\":\"${launchProfile.driverId}\"" +
                "}"
            sessionLog.info(
                "Config",
                "schema=${launchProfile.schemaVersion} settings=${launchProfileProvider.explicitSettingIds(launchProfile).joinToString(",")}",
            )

            ManagedSession.update(ManagedSessionState.Preparing("runtime"))
            val installedRuntime = installRuntime()
            runtimeRoot = installedRuntime
            sessionLog.info("Runtime", "installed version=${installedRuntime.fileName}")
            checkpointStore.mark(DiagnosticCheckpoint.RUNTIME_VERIFIED)
            installedRuntime.resolve(".local/share").toFile().mkdirs()
            installedRuntime.resolve(".config").toFile().mkdirs()
            ManagedSession.update(ManagedSessionState.Preparing("display"))
            val target = withTimeout(SURFACE_TIMEOUT_MS) { ManagedSession.surface.filterNotNull().first() }
            sessionLog.info("Display", "surface=${target.width}x${target.height}")
            val socketRoot = File(filesDir, "x").apply { mkdirs() }
            // Xlib DISPLAY=:0 resolves @/tmp/.X11-unix/X0 (abstract) or
            // /tmp/.X11-unix/X0 (filesystem). Do not use bare "/X0" — that only
            // works if another process (e.g. Termux X11) owns the canonical path.
            xServer = WinlatorEmbeddedXServer(
                this,
                socketRoot,
                useAbstractXSocket = true,
                xSocketPath = UnixSocketConfig.XSERVER_PATH,
                useSharedMemoryAudio = false,
            )
            xServer.start(target.surface, target.width, target.height)
            sessionLog.info("Display", "embedded X server started display=${xServer.display}")
            checkpointStore.mark(DiagnosticCheckpoint.DISPLAY_READY)

            boundSocket = LocalSocket().also {
                it.bind(LocalSocketAddress(controlFile.path, LocalSocketAddress.Namespace.FILESYSTEM))
            }
            serverSocket = LocalServerSocket(boundSocket.fileDescriptor)
            val nativeLibraryDir = Paths.get(applicationInfo.nativeLibraryDir)

            val useVortek = RuntimeVulkanDriverIds.isVortek(launchProfile.driverId)
            var vortekSocketPath: Path? = null
            if (useVortek) {
                val shortId = VortekSessionSupport.newSessionId()
                val session = VortekSessionSupport(
                    filesDir = filesDir,
                    sessionShortId = shortId,
                    log = { tag, msg ->
                        val section = if (tag == VortekSessionSupport.LOG_TAG) "Vortek" else tag
                        sessionLog.info(section, msg)
                    },
                )
                vortekSession = session
                val start = session.startServer()
                if (!start.ok) {
                    throw IllegalStateException("${start.failureCategory()}: ${start.message}")
                }
                // Wire X window → AHB bridge for Vortek WSI (matched Winlator JMethods path).
                val xs = xServer.xServer
                if (xs != null) {
                    val windowBridge = com.bachatas4.android.runtime.vortek.VortekWindowBridge(xs)
                    val bridgeResult = session.controller.setWindowBridge(windowBridge)
                    if (!bridgeResult.isOk) {
                        throw IllegalStateException("vortek_window_bridge: ${bridgeResult.message}")
                    }
                    sessionLog.info("Vortek", "window_bridge=attached")
                } else {
                    throw IllegalStateException("vortek_surface_unavailable: x_server_missing")
                }
                vortekSocketPath = session.socketAsPath()
                sessionLog.info("Vulkan", "driver=system-vortek box64Mode=HOST_GLIBC")
                checkpointStore.mark(DiagnosticCheckpoint.VORTEK_READY)
            }

            val driverConfiguration = launchProfileProvider.vulkanConfiguration(
                launchProfile,
                installedRuntime,
                filesDir.toPath(),
                vortekSocketPath = vortekSocketPath,
            )
            val guestBackend = launchProfile.guestBackend
            guestBackendName = guestBackend.name.lowercase()
            driverInfo = DiagnosticDriverInfo(
                type = when {
                    useVortek -> "vortek"
                    driverConfiguration.driverProfileId.contains("turnip", ignoreCase = true) -> "turnip"
                    else -> "system"
                },
                name = driverConfiguration.driverProfileId,
                version = null,
                build = driverConfiguration.box64Mode.name,
                source = if (useVortek) "system vortek" else "selected profile",
                profileId = driverConfiguration.driverProfileId,
            )
            checkpointStore.mark(DiagnosticCheckpoint.DRIVER_SELECTED)
            checkpointStore.mark(DiagnosticCheckpoint.DRIVER_LOADED)
            sessionLog.info("Runtime", "guestBackend=${guestBackend.name.lowercase()}")
            sessionLog.info(
                "Vulkan",
                "driver=${driverConfiguration.driverProfileId} box64Mode=${driverConfiguration.box64Mode}",
            )
            if (useVortek) {
                sessionLog.info("Vortek", "guest_launch=begin")
            }
            val runtimeHome = filesDir.toPath().resolve("runtime-home")
            ShadPs4ConfigManager.write(runtimeHome, launchProfile)
            sessionLog.info("Vulkan", "persistent pipeline cache enabled home=$runtimeHome")
            val backendEnvironment = if (guestBackend == RuntimeGuestBackend.BOX64) {
                launchProfileProvider.box64Environment(launchProfile)
            } else {
                emptyMap()
            }
            val environment = runtimeEnvironment(installedRuntime, runtimeHome, socketRoot, xServer.display) +
                driverConfiguration.environment + backendEnvironment
            val shadPs4Executable = if (guestBackend == RuntimeGuestBackend.FEX) {
                installedRuntime.resolve("host/shadps4-arm64-fex")
            } else {
                installedRuntime.resolve("bin/shadps4")
            }
            process = try {
                RuntimeProcessLauncher().launch(
                    RuntimeProcessRequest(
                        nativeLibraryDir = nativeLibraryDir,
                        runtimeRoot = installedRuntime,
                        overrideRoot = gameRoot.toPath(),
                        storageRoot = filesDir.toPath(),
                        shadPs4Executable = shadPs4Executable,
                        socketPath = controlFile.path,
                        environment = environment,
                        arguments = listOf("-g", eboot.path),
                        outputPath = outputFile.toPath(),
                        box64Mode = driverConfiguration.box64Mode,
                        guestBackend = guestBackend,
                    ),
                )
            } catch (launchError: Exception) {
                launchFailed = true
                if (useVortek) {
                    throw IllegalStateException(
                        "vortek_guest_launch_failed: ${launchError.message}",
                        launchError,
                    )
                }
                throw launchError
            }
            processStartUtc = Instant.now().toString()
            sessionLog.info("Runtime", "backend process launched")
            checkpointStore.mark(DiagnosticCheckpoint.BACKEND_LAUNCHED)
            // CONTEXT_READY is observed by the Android server when the real packaged client
            // connects (Task 5 probe / game Vulkan init). Do not block control-socket accept.
            val acceptFuture = acceptExecutor.submit<LocalSocket> { serverSocket.accept() }
            while (clientSocket == null) {
                clientSocket = try {
                    acceptFuture.get(ACCEPT_POLL_MILLIS, TimeUnit.MILLISECONDS)
                } catch (_: TimeoutException) {
                    check(process?.isAlive == true) {
                        "shadPS4 exited before socket connect: ${process?.exitCode}"
                    }
                    null
                }
            }
            checkpointStore.mark(DiagnosticCheckpoint.CONTROL_SOCKET_CONNECTED)
            val encoder = ControllerFrameEncoder()
            val controllerOutput = clientSocket.outputStream
            val writeLock = Any()
            val sink: (Int, ControllerSnapshot) -> Unit = { slot, snapshot ->
                encoder.encode(slot, snapshot)?.let { frame ->
                    runCatching {
                        synchronized(writeLock) {
                            controllerOutput.write(frame)
                            controllerOutput.flush()
                        }
                    }
                }
            }
            controllerSink = sink
            ManagedSession.attachControllerSlotSink(sink)
            sessionLog.info("Input", "controller transport attached")
            clientSocket.inputStream.bufferedReader().forEachLine { frame ->
                when {
                    frame == "BACHATA/1 EVENT Running" -> {
                        sessionLog.info("Session", "backend reported Running")
                        checkpointStore.mark(DiagnosticCheckpoint.SHADPS4_RUNNING)
                        ManagedSession.update(ManagedSessionState.Running(gameId))
                    }
                    frame == "BACHATA/1 EVENT Frame" -> {
                        val nowNanos = System.nanoTime()
                        if (!firstFrameReached) {
                            firstFrameReached = true
                            checkpointStore.mark(DiagnosticCheckpoint.FIRST_FRAME_SUBMITTED)
                            checkpointStore.mark(DiagnosticCheckpoint.FIRST_FRAME_PRESENTED)
                        }
                        ManagedSession.recordPresentedFrame(nowNanos)
                        telemetryReporter.record(nowNanos, ManagedSession.frameTelemetry.value)?.let { sample ->
                            sessionLog.info("Performance", sample.logLine())
                        }
                    }
                    frame.startsWith("BACHATA/1 ERROR code=") -> {
                        sessionLog.error("Backend", frame)
                        val reportContext = buildReportContext(
                            reportId = reportId,
                            sessionLog = sessionLog,
                            checkpointStore = checkpointStore,
                            gameTitle = gameTitle,
                            cusaId = gameId,
                            guestBackend = guestBackendName,
                            firstFrameReached = firstFrameReached,
                            driverInfo = driverInfo,
                            processStartUtc = processStartUtc,
                            processEndUtc = Instant.now().toString(),
                            exitCode = process?.exitCode,
                            launchFailed = false,
                            runtimeErrorCode = frame.substringAfter("code="),
                            settingsJson = settingsJson,
                        )
                        ManagedSession.update(
                            ManagedSessionState.Failed(
                                RuntimeErrorCode.CONTENT_INVALID,
                                frame.substringAfter("code="),
                                reportContext = reportContext,
                            ),
                        )
                        statePublished = true
                    }
                }
            }
            process?.waitFor(PROCESS_EXIT_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            processEndUtc = Instant.now().toString()
            sessionLog.info("Session", "backend stopped exitCode=${process?.exitCode}")
            if (!statePublished) {
                checkpointStore.mark(DiagnosticCheckpoint.SESSION_STOPPING)
                val reportContext = buildReportContext(
                    reportId = reportId,
                    sessionLog = sessionLog,
                    checkpointStore = checkpointStore,
                    gameTitle = gameTitle,
                    cusaId = gameId,
                    guestBackend = guestBackendName,
                    firstFrameReached = firstFrameReached,
                    driverInfo = driverInfo,
                    processStartUtc = processStartUtc,
                    processEndUtc = processEndUtc,
                    exitCode = process?.exitCode,
                    launchFailed = false,
                    settingsJson = settingsJson,
                )
                ManagedSession.update(
                    ManagedSessionState.Stopped(
                        exitCode = process?.exitCode,
                        termination = reportContext.termination,
                        reportContext = reportContext,
                    ),
                )
                statePublished = true
            }
        } catch (_: CancellationException) {
            processEndUtc = Instant.now().toString()
            sessionLog.info("Session", "cancelled exitCode=${process?.exitCode} userStop=${userRequestedStop.get()}")
            if (!statePublished) {
                checkpointStore.mark(DiagnosticCheckpoint.SESSION_STOPPING)
                val reportContext = buildReportContext(
                    reportId = reportId,
                    sessionLog = sessionLog,
                    checkpointStore = checkpointStore,
                    gameTitle = gameTitle,
                    cusaId = gameId,
                    guestBackend = guestBackendName,
                    firstFrameReached = firstFrameReached,
                    driverInfo = driverInfo,
                    processStartUtc = processStartUtc,
                    processEndUtc = processEndUtc,
                    exitCode = process?.exitCode,
                    launchFailed = launchFailed,
                    settingsJson = settingsJson,
                )
                ManagedSession.update(
                    ManagedSessionState.Stopped(
                        exitCode = process?.exitCode,
                        termination = reportContext.termination,
                        reportContext = reportContext,
                    ),
                )
                statePublished = true
            }
        } catch (error: Exception) {
            processEndUtc = Instant.now().toString()
            val childOutput = runCatching { outputFile.readLines().takeLast(MAX_ERROR_LOG_LINES).joinToString(" | ") }
                .getOrDefault("")
            val detail = listOfNotNull(error.message, childOutput.ifBlank { null }).joinToString(": ")
            sessionLog.error("Session", "${error.javaClass.simpleName}: ${error.message.orEmpty()}")
            if (!statePublished) {
                val reportContext = buildReportContext(
                    reportId = reportId,
                    sessionLog = sessionLog,
                    checkpointStore = checkpointStore,
                    gameTitle = gameTitle,
                    cusaId = gameId,
                    guestBackend = guestBackendName,
                    firstFrameReached = firstFrameReached,
                    driverInfo = driverInfo,
                    processStartUtc = processStartUtc,
                    processEndUtc = processEndUtc,
                    exitCode = process?.exitCode,
                    launchFailed = launchFailed || error.message?.contains("exited before socket") == true,
                    runtimeErrorCode = RuntimeErrorCode.BACKEND_CRASHED.name,
                    settingsJson = settingsJson,
                )
                ManagedSession.update(
                    ManagedSessionState.Failed(
                        RuntimeErrorCode.BACKEND_CRASHED,
                        detail.ifBlank { error.javaClass.simpleName },
                        reportContext = reportContext,
                    ),
                )
                statePublished = true
            }
        } finally {
            runtimeRoot?.resolve(".local/share/shadPS4/log/shad_log.txt")?.let { internalLog ->
                runCatching {
                    if (internalLog.toFile().isFile) {
                        java.nio.file.Files.copy(
                            internalLog,
                            sessionLog.directory.resolve("shadps4-internal.log"),
                            StandardCopyOption.REPLACE_EXISTING,
                        )
                    }
                }.onFailure {
                    sessionLog.warning("Logs", "internal backend log copy failed: ${it.message.orEmpty()}")
                }
            }
            controllerSink?.let { sink ->
                repeat(4) { slot -> ManagedSession.submitController(slot, ControllerSnapshot.Neutral) }
                ManagedSession.detachControllerSlotSink(sink)
            }
            GamepadInputManager.onSessionEnd()
            val guestExit = process?.exitCode
            process?.destroyForcibly()
            process = null
            runCatching { clientSocket?.close() }
            runCatching { serverSocket?.close() }
            runCatching { boundSocket?.close() }
            acceptExecutor.shutdownNow()
            runCatching { xServer?.let { runBlocking { it.stop() } } }
            // Always stop Vortek after guest teardown (or if guest never launched).
            vortekSession?.let { session ->
                runCatching {
                    runBlocking { session.stop("session_cleanup", guestExitCode = guestExit) }
                }.onFailure {
                    sessionLog.warning("Vortek", "vortek_server_stop_failed: ${it.message.orEmpty()}")
                }
            }
            controlFile.delete()
            checkpointStore.mark(DiagnosticCheckpoint.SESSION_FINISHED)
            sessionLog.info("Session", "cleanup complete")
            stopSelf()
        }
    }

    private fun buildReportContext(
        reportId: String,
        sessionLog: SessionLog,
        checkpointStore: DiagnosticCheckpointStore,
        gameTitle: String,
        cusaId: String,
        guestBackend: String,
        firstFrameReached: Boolean,
        driverInfo: DiagnosticDriverInfo,
        processStartUtc: String?,
        processEndUtc: String?,
        exitCode: Int?,
        launchFailed: Boolean,
        runtimeErrorCode: String? = null,
        settingsJson: String?,
    ): DiagnosticReportContext {
        val userStop = userRequestedStop.get()
        val termination = ProcessTerminationClassifier.fromJavaExitValue(
            exitCode = exitCode,
            userRequestedStop = userStop,
            processRole = when (guestBackend) {
                "fex" -> ProcessRole.FEX
                "box64" -> ProcessRole.BOX64
                else -> ProcessRole.BACKEND
            },
            processStartUtc = processStartUtc,
            processEndUtc = processEndUtc,
            runtimeErrorCode = runtimeErrorCode,
            launchFailed = launchFailed && !userStop,
        )
        return DiagnosticReportContext(
            reportId = reportId,
            sessionDirectory = sessionLog.directory.toString(),
            gameTitle = gameTitle,
            cusaId = cusaId,
            guestBackend = guestBackend,
            lastCheckpoint = checkpointStore.lastCheckpoint?.name,
            firstFrameReached = firstFrameReached,
            userRequestedStop = userStop,
            termination = termination,
            driver = driverInfo,
            app = diagnosticAppInfo(),
            device = diagnosticDeviceInfo(),
            processStartUtc = processStartUtc,
            processEndUtc = processEndUtc,
            runtimeErrorCode = runtimeErrorCode,
            settingsJson = settingsJson,
        )
    }

    private fun diagnosticAppInfo(): DiagnosticAppInfo =
        DiagnosticAppInfo(
            versionName = BuildConfig.VERSION_NAME,
            versionCode = BuildConfig.VERSION_CODE.toLong(),
            packageName = packageName,
            distribution = BuildConfig.FLAVOR.ifBlank { "unknown" },
            sourceCommit = BuildConfig.SOURCE_COMMIT,
            runtimeRevision = BuildConfig.RUNTIME_REVISION,
            debuggable = BuildConfig.DEBUG,
        )

    private fun diagnosticDeviceInfo(): DiagnosticDeviceInfo =
        DiagnosticDeviceInfo(
            manufacturer = Build.MANUFACTURER.orEmpty(),
            model = Build.MODEL.orEmpty(),
            device = Build.DEVICE.orEmpty(),
            androidRelease = Build.VERSION.RELEASE.orEmpty(),
            sdkInt = Build.VERSION.SDK_INT,
            supportedAbis = Build.SUPPORTED_ABIS?.toList().orEmpty(),
            soc = Build.SOC_MODEL.takeIf { it.isNotBlank() } ?: Build.HARDWARE,
            gpuRenderer = null,
            gpuVendor = null,
            gpuVersion = null,
            ramTotalMb = null,
        )

    private fun installRuntime(): Path {
        val installRoot = File(filesDir, "runtime").toPath()
        if (!com.bachatas4.android.BuildConfig.DOWNLOAD_RUNTIME) {
            val manifest = assets.open("runtime/manifest.json").bufferedReader().use {
                Json { ignoreUnknownKeys = true }.decodeFromString<RuntimeManifest>(it.readText())
            }
            val target = installRoot.resolve(manifest.runtimeVersion)
            if (target.toFile().isDirectory) return target
            return assets.open("runtime/runtime.zip").use { bundle ->
                RuntimeInstaller(installRoot).install(bundle, manifest).getOrElse { error ->
                    if (error is FileAlreadyExistsException && target.toFile().isDirectory) target else throw error
                }
            }
        }
        val installedDir = installRoot.toFile().listFiles()?.firstOrNull { it.isDirectory && it.name.startsWith("box64-") }
        if (installedDir != null) return installedDir.toPath()
        throw IllegalStateException("Runtime not installed")
    }

    private fun runtimeEnvironment(runtimeRoot: Path, runtimeHome: Path, socketRoot: File, display: String) = mapOf(
        "HOME" to runtimeHome.toString(),
        "BOX64_PATH" to runtimeRoot.resolve("bin").toString(),
        "BOX64_LOG" to "1",
        "BOX64_LOAD_ADDR" to "0x6000000000",
        "BOX64_PREFER_WRAPPED" to "1",
        "BOX64_DYNAREC_CALLRET" to "1",
        "BOX64_LD_LIBRARY_PATH" to "${runtimeRoot.resolve("lib/x86_64-linux-gnu")}:${runtimeRoot.resolve("lib64")}",
        "BOX64_EMULATED_LIBS" to EMULATED_LIBRARIES,
        "BACHATA_ALSA_SOCKET" to File(socketRoot, UnixSocketConfig.ALSA_SERVER_PATH).path,
        "DISPLAY" to display,
        "SDL_VIDEODRIVER" to "x11",
        "XKB_CONFIG_ROOT" to runtimeRoot.resolve("usr/share/X11/xkb").toString(),
        "TMPDIR" to cacheDir.path,
        "XDG_CACHE_HOME" to File(cacheDir, "xdg").apply { mkdirs() }.path,
        "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
        "BACHATA_VORTEK_TRACE_BIND_VERTEX_BUFFERS" to "1",
        "BACHATA_FEX_TRACE_SIGSYS" to "1",
    )

    private fun stopSession() {
        userRequestedStop.set(true)
        process?.destroy()
        sessionJob?.cancel()
        sessionJob = null
    }

    private fun createNotificationChannel() {
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Emulation", NotificationManager.IMPORTANCE_LOW),
        )
    }

    private fun notification(): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val stop = PendingIntent.getService(
            this, 1,
            Intent(ManagedSession.ACTION_STOP).setClassName(packageName, ManagedSession.SERVICE_CLASS),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle("Bachata S4 emulation")
            .setContentText("Game session running")
            .setContentIntent(open)
            .addAction(0, "Stop", stop)
            .setOngoing(true)
            .build()
    }

    private companion object {
        const val CHANNEL_ID = "emulation"
        const val NOTIFICATION_ID = 41
        const val SURFACE_TIMEOUT_MS = 30_000L
        const val PROCESS_EXIT_TIMEOUT_SECONDS = 5L
        const val ACCEPT_POLL_MILLIS = 250L
        const val MAX_ERROR_LOG_LINES = 20
        const val MAX_LOG_SESSIONS = 10
        const val EMULATED_LIBRARIES = "libSDL2-2.0.so.0:libudev.so.1:libuuid.so.1"
    }
}
