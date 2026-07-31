package com.bachatas4.android

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.platform.LocalContext
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.bachatas4.android.data.RuntimeProfileStore
import com.bachatas4.android.feature.drivers.DriverManagerBackend
import com.bachatas4.android.feature.drivers.DriverManagerScreen
import com.bachatas4.android.feature.library.LibraryScreen
import com.bachatas4.android.feature.settings.SettingsScreen
import com.bachatas4.android.feature.setup.SetupScreen
import com.bachatas4.android.feature.session.SessionScreen
import com.bachatas4.android.runtime.settings.ProfileScope
import dagger.hilt.EntryPoint
import dagger.hilt.InstallIn
import dagger.hilt.android.EntryPointAccessors
import dagger.hilt.components.SingletonComponent
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object BachataRoutes {
    const val Setup = "setup"
    const val SetupDrivers = "setup/drivers"
    const val Library = "library"
    const val Game = "game/{id}"
    const val Session = "session/{id}"
    const val Settings = "settings"
    const val GameSettings = "settings/game/{id}"
    const val Drivers = "drivers"
    fun gameSettings(id: String) = "settings/game/$id"
}

@EntryPoint
@InstallIn(SingletonComponent::class)
interface BachataNavEntryPoint {
    fun driverBackend(): DriverManagerBackend
    fun profileStore(): RuntimeProfileStore
}

@Composable
fun BachataNavHost(startDestination: String = BachataRoutes.Setup) {
    val navController = rememberNavController()
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val graph = remember {
        EntryPointAccessors.fromApplication(
            context.applicationContext,
            BachataNavEntryPoint::class.java,
        )
    }
    val showDriverSelection = BuildConfig.SHOW_DRIVER_SELECTION

    fun goToLibraryClearingSetup() {
        navController.navigate(BachataRoutes.Library) {
            popUpTo(BachataRoutes.Setup) { inclusive = true }
        }
    }

    NavHost(navController = navController, startDestination = startDestination) {
        composable(BachataRoutes.Setup) {
            SetupScreen(
                onContinue = {
                    if (showDriverSelection) {
                        navController.navigate(BachataRoutes.SetupDrivers)
                    } else {
                        scope.launch {
                            withContext(Dispatchers.IO) {
                                val driverId = graph.driverBackend().autoSelectDriverId()
                                    ?: error("Play build must auto-select bundled Turnip")
                                graph.profileStore().update(ProfileScope.Global) {
                                    it.copy(driverId = driverId)
                                }
                            }
                            goToLibraryClearingSetup()
                        }
                    }
                },
            )
        }
        composable(BachataRoutes.SetupDrivers) {
            DriverManagerScreen(
                onBack = { navController.popBackStack() },
                onContinue = { goToLibraryClearingSetup() },
            )
        }
        composable(BachataRoutes.Library) {
            LibraryScreen(
                onOpenSettings = { navController.navigate(BachataRoutes.Settings) },
                onOpenGameSettings = { id -> navController.navigate(BachataRoutes.gameSettings(id)) },
                onLaunch = { id -> navController.navigate("session/$id") },
            )
        }
        composable(BachataRoutes.Settings) {
            SettingsScreen(
                onBack = { navController.popBackStack() },
                onOpenDrivers = { navController.navigate(BachataRoutes.Drivers) },
                // Always show Drivers in Settings (Play: bundled status; F-Droid: full manager).
                // Setup/session driver pick remains gated by SHOW_DRIVER_SELECTION.
                showDriversTab = true,
            )
        }
        composable(BachataRoutes.Drivers) {
            DriverManagerScreen(onBack = { navController.popBackStack() })
        }
        composable(BachataRoutes.GameSettings) { entry ->
            SettingsScreen(
                initialGameId = requireNotNull(entry.arguments?.getString("id")),
                onBack = { navController.popBackStack() },
                onOpenDrivers = { navController.navigate(BachataRoutes.Drivers) },
                showDriversTab = true,
            )
        }
        composable(BachataRoutes.Game) {
            LibraryScreen(
                onOpenSettings = { navController.navigate(BachataRoutes.Settings) },
                onOpenGameSettings = { id -> navController.navigate(BachataRoutes.gameSettings(id)) },
                onLaunch = { id -> navController.navigate("session/$id") },
            )
        }
        composable(BachataRoutes.Session) { entry ->
            SessionScreen(
                gameId = requireNotNull(entry.arguments?.getString("id")),
                onOpenDrivers = { navController.navigate(BachataRoutes.Drivers) },
                showDriverActions = showDriverSelection,
                onExit = { navController.popBackStack() },
            )
        }
    }
}
