package com.bachatas4.android

import com.bachatas4.android.data.RuntimeProfileStore
import com.bachatas4.android.feature.drivers.DriverManagerBackend
import com.bachatas4.android.runtime.settings.Box64EnvironmentCodec
import com.bachatas4.android.runtime.settings.CompatibilityConstraint
import com.bachatas4.android.runtime.settings.ProfileScope
import com.bachatas4.android.runtime.settings.ResolvedRuntimeProfile
import com.bachatas4.android.runtime.settings.RuntimeProfileResolver
import com.bachatas4.android.runtime.settings.RuntimeGuestBackend
import com.bachatas4.android.runtime.settings.RuntimeSettingCatalog
import com.bachatas4.android.runtime.settings.SettingKind
import com.bachatas4.android.runtime.settings.ValueSource
import com.bachatas4.android.runtime.process.VulkanDriverConfiguration
import com.bachatas4.android.runtime.process.VulkanDriverResolveContext
import java.nio.file.Path
import javax.inject.Inject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull

class RuntimeLaunchProfileProvider internal constructor(
    private val store: RuntimeProfileStore,
    private val catalog: RuntimeSettingCatalog,
    constraints: Map<String, CompatibilityConstraint>,
    private val driverBackend: DriverManagerBackend,
) {
    private val specs = catalog.shadPs4 + catalog.box64
    private val compatibilityConstraints = constraints

    @Inject
    constructor(
        store: RuntimeProfileStore,
        driverBackend: DriverManagerBackend,
    ) : this(
        store,
        RuntimeSettingCatalog.loadFromResources(),
        androidCompatibilityConstraints(),
        driverBackend,
    )

    suspend fun resolve(gameId: String): ResolvedRuntimeProfile {
        val global = store.load(ProfileScope.Global)
        val game = store.load(ProfileScope.Game(gameId))
        val guestBackend = RuntimeProfileResolver.resolveGuestBackend(global, game)
        val constraints = if (guestBackend == RuntimeGuestBackend.FEX) {
            compatibilityConstraints + FEX_COMPATIBILITY_CONSTRAINTS
        } else {
            compatibilityConstraints
        }
        return RuntimeProfileResolver(specs, constraints).resolve(
            global,
            game,
        )
    }

    fun box64Environment(profile: ResolvedRuntimeProfile): Map<String, String> {
        val environment = profile.unknownBox64.toMutableMap()
        profile.settings.values
            .filter { it.spec.section == "Box64" && it.source != ValueSource.DEFAULT && it.value != null }
            .forEach { setting ->
                val primitive = setting.value as? JsonPrimitive
                    ?: throw IllegalArgumentException("Invalid Box64 value for ${setting.spec.nativeKey}")
                environment[setting.spec.nativeKey] = when (setting.spec.kind) {
                    SettingKind.BOOLEAN -> if (primitive.booleanOrNull == true) "1" else "0"
                    else -> primitive.content
                }
            }
        val validated = Box64EnvironmentCodec.decode(Box64EnvironmentCodec.encode(environment)).toMutableMap()
        profile.box64Preset.environmentValue?.let { validated["BOX64_PROFILE"] = it }
        return validated
    }

    fun explicitSettingIds(profile: ResolvedRuntimeProfile): List<String> =
        profile.settings.values.filter { it.source != ValueSource.DEFAULT }.map { it.spec.id }.sorted()

    fun vulkanConfiguration(
        profile: ResolvedRuntimeProfile,
        runtimeRoot: Path,
        filesDir: Path,
        vortekSocketPath: Path? = null,
    ): VulkanDriverConfiguration =
        try {
            driverBackend.configurationFor(
                profile.driverId,
                VulkanDriverResolveContext(
                    runtimeRoot = runtimeRoot,
                    vortekSocketPath = vortekSocketPath,
                ),
            )
        } catch (error: IllegalStateException) {
            throw MissingRuntimeDriverException(profile.driverId, error)
        } catch (error: IllegalArgumentException) {
            throw MissingRuntimeDriverException(profile.driverId, error)
        }

    private companion object {
        val FEX_COMPATIBILITY_CONSTRAINTS = mapOf(
            "gpu.copy_gpu_buffers" to CompatibilityConstraint(
                JsonPrimitive(true),
                "FEX guest execution can reuse command buffers before asynchronous GPU parsing completes",
            ),
        )

        fun androidCompatibilityConstraints() = mapOf(
            "general.dev_kit_mode" to CompatibilityConstraint(
                JsonPrimitive(false),
                "Retail memory layout; DevKit expands guest direct memory and regressed Bloodborne on constrained devices",
            ),
            "log.sync" to CompatibilityConstraint(JsonPrimitive(false), "Avoid blocking Android runtime logging"),
            "vulkan.pipeline_cache_enabled" to CompatibilityConstraint(JsonPrimitive(true), "Preserve Android pipeline cache"),
            "vulkan.pipeline_cache_archived" to CompatibilityConstraint(JsonPrimitive(false), "Use live Android pipeline cache"),
            "vulkan.vkvalidation_core_enabled" to CompatibilityConstraint(JsonPrimitive(false), "Validation layers are unavailable"),
        )
    }
}

class MissingRuntimeDriverException(
    val driverId: String,
    cause: Throwable? = null,
) : IllegalStateException(
    "Selected Vulkan driver '$driverId' is not installed; open Turnip drivers and select another driver",
    cause,
)
