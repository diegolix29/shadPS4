// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <future>
#include <variant>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 16;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};

    // Fast lookup for shader permutations by specialization signature.
    // Avoids repeated deep StageSpecialization comparisons on hot paths.
    tsl::robin_map<u64, size_t> perm_index_by_sig{};

    // PERF(GR2 v16): Cache the last GetProgram result per program.
    // When the pipeline stamp changes (e.g. viewport/scissor update) but the shader's
    // user_data, runtime_info, and binding offsets are unchanged, we can skip the expensive
    // StageSpecialization construction (~2.26% of GpuComm) entirely.
    struct LastResultCache {
        u64 ud_hash{};           // stage-aware hash of user_data + runtime_info + binding
        u64 ri_bind_hash{};      // hash of runtime_info + binding only (for stable program shortcut)
        size_t perm_idx{};
        u64 perm_hash{};
        vk::ShaderModule module{};
        bool valid{false};
    } last_result{};

    // PERF(GR2 v17): Stability tracking for single-permutation programs.
    // When a program has had only 1 permutation for many consecutive calls, mark it as "stable".
    // For stable single-permutation programs, skip StageSpecialization construction when only
    // user_data addresses change (stride/format/etc. are extremely unlikely to change).
    u32 stability_counter{};
    static constexpr u32 kStabilityThreshold = 64;
    static constexpr u32 kStabilityRevalidateInterval = 512;

    Program() = default;
    Program(Shader::Stage stage, Shader::LogicalStage l_stage, Shader::ShaderParams params)
    : info{stage, l_stage, params} {
        modules.reserve(MaxPermutations);
        perm_index_by_sig.reserve(MaxPermutations * 2);
    }

        void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
            const u64 sig = spec.sig;
            modules.emplace_back(module, std::move(spec));
            // Only keep the first index for a given sig; multiple serialized permutation indices
            // may map to the same specialization (safe to reuse the same module).
            perm_index_by_sig.try_emplace(sig, modules.size() - 1);
        }

        void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                          size_t perm_idx) {
            modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
            const u64 sig = spec.sig;
            modules[perm_idx] = {module, std::move(spec)};
            perm_index_by_sig.try_emplace(sig, perm_idx);
                          }
};

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    const GraphicsPipeline* GetGraphicsPipeline();

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(Shader::Stage stage, Shader::LogicalStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::Stage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool RefreshGraphicsKey();
    bool RefreshGraphicsStages();
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::Stage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::Stage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(Shader::Stage stage, Shader::LogicalStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

    // =========================================================================
    // OPT(GR2 v78): Async graphics pipeline compile + driver-hang watchdog.
    // =========================================================================
    // shadPS4 has been hanging silently inside `vkCreateGraphicsPipelines` on
    // certain (pipeline, RADV Mesa version) combinations — the synchronous
    // Vulkan call never returns. Since GetGraphicsPipeline runs on the GPU
    // submit thread, a driver hang there freezes the entire emulator.
    //
    // Fix: launch the GraphicsPipeline ctor on a worker via std::async. Wait
    // briefly on the future (most compiles take <50ms — zero behavior change
    // for the common case). If the budget elapses, return nullptr from
    // GetGraphicsPipeline and stash the future in `pending_graphics_pipelines`.
    // Rasterizer::Draw already handles nullptr as "skip this draw" (frame-skip).
    // On every subsequent call for the same key, non-blocking-poll the future.
    // Log loudly once past kHangLogThreshold; mark permafailed + move future
    // to the file-local graveyard past kPermaFailThreshold so we neither block
    // destruction nor join a hung thread.
    struct PendingGraphicsPipeline {
        std::future<std::unique_ptr<GraphicsPipeline>> future;
        std::chrono::steady_clock::time_point started_at;
        u64 pipeline_hash{};
        GraphicsPipeline::SerializationSupport sdata{};
        // Stage-data deep copies. GraphicsPipeline's ctor takes spans into
        // PipelineCache::infos/runtime_infos/modules — those cache members are
        // overwritten on the next RefreshGraphicsStages, so the async task
        // cannot rely on them. Copy once at launch.
        std::array<const Shader::Info*, MaxShaderStages> infos_copy{};
        std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos_copy{};
        std::array<vk::ShaderModule, MaxShaderStages> modules_copy{};
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_copy{};
        bool hang_warned{false};
        bool permafailed{false};
    };

    // Thresholds. Tune via constants here; no config plumbing to avoid scope creep.
    static constexpr std::chrono::milliseconds kInitialSyncBudget{200};
    static constexpr std::chrono::seconds      kHangLogThreshold{5};
    static constexpr std::chrono::seconds      kPermaFailThreshold{30};

    // True if the pending entry's future is ready and the result was moved into
    // graphics_pipelines[key] (caller must then erase from pending map). False
    // if still compiling or permafailed.
    bool TryFinalizePending(PendingGraphicsPipeline& pending,
                            const GraphicsPipelineKey& key);

    std::unique_ptr<PendingGraphicsPipeline> LaunchAsyncPipelineCompile(
        const GraphicsPipelineKey& key, u64 pipeline_hash);

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    // OPT(GR2 v78): In-flight async compiles keyed on graphics_key.
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<PendingGraphicsPipeline>>
        pending_graphics_pipelines;
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    // Fast path: if only shader user data changes, the graphics pipeline key does not.
    u64 last_gfx_stamp{};
    const GraphicsPipeline* last_gfx_pipeline{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    GraphicsPipelineKey prev_graphics_key_{};  // Key-level dedup: skip map lookup when unchanged
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;
};

} // namespace Vulkan
