// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>

#include <immintrin.h>

#include "common/assert.h"
#include "common/debug.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/signal_context.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/flush_epoch.h"
#include "video_core/gpu_authority_tracker.h"
#include "video_core/guest_copy_engine.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

namespace Vulkan {

void Rasterizer::InitializeQueryPool() {
    vk::Device device = instance.GetDevice();

    vk::QueryPoolCreateInfo pool_info{};
    pool_info.queryType = vk::QueryType::eOcclusion;
    pool_info.queryCount = MAX_OCCLUSION_QUERIES;

    auto result = device.createQueryPool(pool_info);
    if (result.result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Failed to create occlusion query pool");
        return;
    }
    occlusion_query_pool = result.value;

    auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.resetQueryPool(occlusion_query_pool, 0, MAX_OCCLUSION_QUERIES);

    occlusion_slot_owner.fill(0);
    occlusion_index_mapping.clear();
    pending_occlusion_readbacks.clear();
    occlusion_current_index = 0;
}

void Rasterizer::DestroyQueryPool() {
    if (occlusion_query_pool) {
        vk::Device device = instance.GetDevice();
        device.destroyQueryPool(occlusion_query_pool);
        occlusion_query_pool = nullptr;
    }
}

void Rasterizer::StartOcclusionQuery(VAddr addr, s32 num_counter_pairs) {
    if (!occlusion_query_pool) {
        return;
    }

    const u32 index = occlusion_current_index;
    const VAddr prev_owner = occlusion_slot_owner[index];
    if (prev_owner != 0 && prev_owner != addr) {
        LOG_ERROR(Render_Vulkan,
                  "Occlusion slot {} reused before addr {:#x} performed EndOcclusionQuery, "
                  "force-clearing previous data",
                  index, prev_owner);

        auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.endQuery(occlusion_query_pool, index);
        occlusion_index_mapping.erase(prev_owner);
    }

    scheduler.EndRendering();
    auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.resetQueryPool(occlusion_query_pool, index, 1);
    cmdbuf.beginQuery(occlusion_query_pool, index, vk::QueryControlFlagBits::ePrecise);

    occlusion_index_mapping.insert_or_assign(addr, index);
    occlusion_slot_owner[index] = addr;

    WriteOcclusionQueryResult(addr, 0, num_counter_pairs);
    occlusion_current_index = (index + 1) % MAX_OCCLUSION_QUERIES;
}

void Rasterizer::WriteOcclusionQueryResult(VAddr address, u64 value, s32 num_counter_pairs) {
    static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
    const u64 result = value | OcclusionCounterValidMask;

    auto* memory = Core::Memory::Instance();
    const u64 last_write_end =
        address + (u64(num_counter_pairs - 1) * sizeof(u64) * 2) + sizeof(u64);

    if (!memory->IsValidMapping(address, last_write_end - address)) {
        LOG_ERROR(Render_Vulkan, "Occlusion query writeback address is invalid: {:#x}", address);
        return;
    }
    for (s32 i = 0; i < num_counter_pairs; ++i) {
        auto* dst = reinterpret_cast<void*>(address + i * sizeof(u64) * 2);
        if (!memory->TryWriteBacking(dst, &result, sizeof(result))) {
            LOG_ERROR(Render_Vulkan, "Occlusion query writeback failed: address {:#x}", address);
        }
    }
}

void Rasterizer::EndOcclusionQuery(VAddr addr, s32 num_counter_pairs) {
    const auto it = occlusion_index_mapping.find(addr);
    if (it == occlusion_index_mapping.end()) {
        LOG_ERROR(Render_Vulkan,
                  "EndOcclusionQuery called for addr {:#x} with no matching StartOcclusionQuery",
                  addr);
        return;
    }
    const u32 index = it->second;

    scheduler.EndRendering();
    auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endQuery(occlusion_query_pool, index);

    pending_occlusion_readbacks.push_back(PendingOcclusionReadback{
        .address = addr + 8,
        .index = index,
        .submit_tick = scheduler.CurrentTick(),
        .counter_pairs = num_counter_pairs,
    });

    occlusion_index_mapping.erase(it);
}

void Rasterizer::ProcessPendingOcclusionReadbacks() {
    if (!occlusion_query_pool) {
        return;
    }
    vk::Device device = instance.GetDevice();

    while (!pending_occlusion_readbacks.empty()) {
        const auto& pending = pending_occlusion_readbacks.front();

        if (!scheduler.IsFree(pending.submit_tick)) {
            break;
        }

        u64 result = 0;
        const vk::Result query_res =
            device.getQueryPoolResults(occlusion_query_pool, pending.index, 1, sizeof(result),
                                       &result, sizeof(result), vk::QueryResultFlagBits::e64);

        if (query_res == vk::Result::eSuccess) {
            WriteOcclusionQueryResult(pending.address, result, pending.counter_pairs);
        } else {
            LOG_WARNING(Render_Vulkan, "Occlusion query {} result not ready after completion",
                        pending.index);
            WriteOcclusionQueryResult(pending.address, 0, pending.counter_pairs);
        }

        occlusion_slot_owner[pending.index] = 0;
        pending_occlusion_readbacks.pop_front();
    }
}

static SHAD_NO_INLINE void ValidateResolvedSharp(const Shader::Info& info, const char* kind,
                                                 size_t index, size_t descriptor_count,
                                                 size_t resolved_count) {
    ASSERT_MSG(index < descriptor_count && resolved_count == descriptor_count,
               "Resolved {} of {} shader {:#x}: index {}, {} descriptors, {} resolved", kind,
               info.stage, info.pgm_hash, index, descriptor_count, resolved_count);
}

#if defined(_MSC_VER)
#define SHAD_RASTERIZER_FORCE_INLINE __forceinline
#else
#define SHAD_RASTERIZER_FORCE_INLINE __attribute__((always_inline)) inline
#endif

[[nodiscard]] static SHAD_RASTERIZER_FORCE_INLINE AmdGpu::Buffer GetResolvedBuffer(
    const Shader::Info& info, u32 index) {
    if (index >= info.buffers.size() || info.resolved_buffers.size() != info.buffers.size())
        [[unlikely]] {
        ValidateResolvedSharp(info, "buffer", index, info.buffers.size(),
                              info.resolved_buffers.size());
    }
    return info.resolved_buffers[index];
}

[[nodiscard]] static SHAD_RASTERIZER_FORCE_INLINE AmdGpu::Image GetResolvedImage(
    const Shader::Info& info, u32 index) {
    if (index >= info.images.size() || info.resolved_images.size() != info.images.size())
        [[unlikely]] {
        ValidateResolvedSharp(info, "image", index, info.images.size(),
                              info.resolved_images.size());
    }
    return info.resolved_images[index];
}

[[nodiscard]] static SHAD_RASTERIZER_FORCE_INLINE AmdGpu::Sampler GetResolvedSampler(
    const Shader::Info& info, u32 index) {
    if (index >= info.samplers.size() || info.resolved_samplers.size() != info.samplers.size())
        [[unlikely]] {
        ValidateResolvedSharp(info, "sampler", index, info.samplers.size(),
                              info.resolved_samplers.size());
    }
    return info.resolved_samplers[index];
}

#undef SHAD_RASTERIZER_FORCE_INLINE

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

static SHAD_NO_INLINE void ReportUnsupportedWindowOffset() {
    LOG_ERROR(Render_Vulkan,
              "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
}

[[nodiscard]] static u64 DescriptorWriteKey0(const vk::WriteDescriptorSet& write) noexcept {
    return static_cast<u64>(write.dstBinding) |
           (static_cast<u64>(write.dstArrayElement) << 32);
}

[[nodiscard]] static u64 DescriptorWriteKey1(const vk::WriteDescriptorSet& write) noexcept {
    return static_cast<u64>(write.descriptorCount) |
           (static_cast<u64>(static_cast<u32>(write.descriptorType)) << 32);
}

[[nodiscard]] static bool DescriptorInfoEqual(const vk::DescriptorBufferInfo& lhs,
                                              const vk::DescriptorBufferInfo& rhs) noexcept {
    static_assert(sizeof(vk::Buffer) == sizeof(u64));
    const u64 different = (std::bit_cast<u64>(lhs.buffer) ^ std::bit_cast<u64>(rhs.buffer)) |
                          (lhs.offset ^ rhs.offset) | (lhs.range ^ rhs.range);
    return different == 0;
}

[[nodiscard]] static bool DescriptorInfoEqual(const vk::DescriptorImageInfo& lhs,
                                              const vk::DescriptorImageInfo& rhs) noexcept {
    static_assert(sizeof(vk::Sampler) == sizeof(u64));
    static_assert(sizeof(vk::ImageView) == sizeof(u64));
    const u64 different =
        (std::bit_cast<u64>(lhs.sampler) ^ std::bit_cast<u64>(rhs.sampler)) |
        (std::bit_cast<u64>(lhs.imageView) ^ std::bit_cast<u64>(rhs.imageView)) |
        (static_cast<u32>(lhs.imageLayout) ^ static_cast<u32>(rhs.imageLayout));
    return different == 0;
}

template <typename Info>
[[nodiscard]] static bool DescriptorInfosEqual(const Info* lhs, const Info* rhs,
                                               const u32 count) noexcept {
    if (count == 1) [[likely]] {
        return DescriptorInfoEqual(*lhs, *rhs);
    }
    for (u32 index = 0; index < count; ++index) {
        if (!DescriptorInfoEqual(lhs[index], rhs[index])) {
            return false;
        }
    }
    return true;
}

template <typename Vector, typename Info>
static void AppendDescriptorInfos(Vector& destination, const Info* source, const u32 count) {
    if (count == 1) [[likely]] {
        destination.push_back(*source);
    } else {
        destination.insert(destination.end(), source, source + count);
    }
}

template <typename Info>
static SHAD_NO_INLINE void CopyDescriptorInfosSlow(Info* destination, const Info* source,
                                                   const u32 count) {
    std::copy_n(source, count, destination);
}

template <typename Info>
static void CopyDescriptorInfos(Info* destination, const Info* source, const u32 count) {
    if (count == 1) [[likely]] {
        *destination = *source;
    } else {
        CopyDescriptorInfosSlow(destination, source, count);
    }
}

static SHAD_NO_INLINE void ReportDescriptorStateOverflow() {
    ASSERT(false);
}

static SHAD_NO_INLINE void ValidateTextureBindingIndex(size_t index, size_t limit) {
    ASSERT(index < limit);
}

static SHAD_NO_INLINE void ValidateSingleImageBinding(u32 count) {
    ASSERT(count == 1);
}

static SHAD_NO_INLINE void ValidateFeedbackLoopBinding(bool force_general) {
    ASSERT_MSG(!force_general, "Having image both as storage and render target is unsupported");
}

static SHAD_NO_INLINE void ValidateDepthTargetView(u32 levels, bool needs_rebind) {
    ASSERT(levels == 1 && !needs_rebind);
}

[[nodiscard]] static vk::Viewport MakeViewport(const Instance& instance, const AmdGpu::Regs& regs,
                                               const u32 index) {
    const auto& vp = regs.viewports[index];
    const auto& vp_ctl = regs.viewport_control;
    const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
    const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

    vk::Viewport viewport{};
    if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
        viewport.minDepth = zoffset - zscale;
        viewport.maxDepth = zoffset + zscale;
    } else {
        viewport.minDepth = zoffset;
        viewport.maxDepth = zoffset + zscale;
    }

    if (!instance.IsDepthRangeUnrestrictedSupported()) {
        viewport.minDepth = std::max(viewport.minDepth, 0.f);
        viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
    }

    if (regs.IsClipDisabled()) {
        viewport.x = 0.f;
        viewport.y = 0.f;
        viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
        viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
    } else {
        const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
        const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
        const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
        const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

        viewport.x = xoffset - xscale;
        viewport.y = yoffset - yscale;
        viewport.width = xscale * 2.0f;
        viewport.height = yscale * 2.0f;
    }
    return viewport;
}

[[nodiscard]] static vk::Rect2D MakeViewportScissor(const AmdGpu::Regs& regs,
                                                    const AmdGpu::Scissor& combined_scissor,
                                                    const u32 index) {
    auto scissor = combined_scissor;
    if (regs.mode_control.vport_scissor_enable) {
        scissor.top_left_x =
            std::max(scissor.top_left_x, s16(regs.viewport_scissors[index].top_left_x));
        scissor.top_left_y =
            std::max(scissor.top_left_y, s16(regs.viewport_scissors[index].top_left_y));
        scissor.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(scissor.bottom_right_x),
                                          regs.viewport_scissors[index].bottom_right_x);
        scissor.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(scissor.bottom_right_y),
                                          regs.viewport_scissors[index].bottom_right_y);
    }
    return {
        .offset = {scissor.top_left_x, scissor.top_left_y},
        .extent = {scissor.GetWidth(), scissor.GetHeight()},
    };
}

[[nodiscard]] static u32 ActiveViewportMask(const AmdGpu::Regs& regs) noexcept {
    static_assert(AmdGpu::NUM_VIEWPORTS == 16);
    static_assert(sizeof(AmdGpu::ViewportBounds) == 6 * sizeof(u32));
    const auto* base = reinterpret_cast<const int*>(regs.viewports.data());
    const __m256i low_indices = _mm256_setr_epi32(0, 6, 12, 18, 24, 30, 36, 42);
    const __m256i high_indices = _mm256_setr_epi32(48, 54, 60, 66, 72, 78, 84, 90);
    const __m256i magnitude_mask = _mm256_set1_epi32(0x7fffffff);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i low =
        _mm256_and_si256(_mm256_i32gather_epi32(base, low_indices, sizeof(u32)), magnitude_mask);
    const __m256i high =
        _mm256_and_si256(_mm256_i32gather_epi32(base, high_indices, sizeof(u32)), magnitude_mask);
    const u32 inactive_low = static_cast<u32>(
        _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(low, zero))));
    const u32 inactive_high = static_cast<u32>(
        _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(high, zero))));
    return (~inactive_low & 0xffu) | ((~inactive_high & 0xffu) << 8);
}

static SHAD_NO_INLINE void SetMultipleViewportScissorState(
    const Instance& instance, const AmdGpu::Regs& regs, const AmdGpu::Scissor& combined_scissor,
    DynamicState& dynamic_state) {
    Viewports viewports;
    Scissors scissors;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; ++i) {
        if (regs.viewports[i].xscale == 0.f) {
            continue;
        }
        viewports.push_back(MakeViewport(instance, regs, i));
        scissors.push_back(MakeViewportScissor(regs, combined_scissor, i));
    }
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

/// The inputs of each dynamic state group, passed as fields to func.
template <typename Func>
static decltype(auto) ViewportInputs(const AmdGpu::Regs& regs, Func&& func) {
    return func(regs.screen_scissor, regs.window_scissor, regs.generic_scissor, regs.window_offset,
                regs.viewport_scissors, regs.viewports, regs.viewport_control, regs.mode_control,
                regs.clipper_control, regs.primitive_type, regs.polygon_control);
}

template <typename Func>
static decltype(auto) DepthStencilInputs(const AmdGpu::Regs& regs, Func&& func) {
    return func(regs.depth_control, regs.depth_render_control, regs.depth_render_override,
                regs.depth_buffer, regs.depth_bounds_min, regs.depth_bounds_max,
                regs.polygon_control, regs.poly_offset, regs.stencil_control,
                regs.stencil_ref_front, regs.stencil_ref_back);
}

template <typename Func>
static decltype(auto) PrimitiveInputs(const AmdGpu::Regs& regs, Func&& func) {
    return func(regs.enable_primitive_restart, regs.primitive_type, regs.primitive_restart_index,
                regs.polygon_control, regs.clipper_control);
}

template <typename Func>
static decltype(auto) RasterizationInputs(const AmdGpu::Regs& regs, Func&& func) {
    return func(regs.line_control);
}

template <typename Func>
static decltype(auto) BlendInputs(const AmdGpu::Regs& regs, const GraphicsPipeline& pipeline,
                                  Func&& func) {
    return func(regs.blend_constants, pipeline.GetGraphicsKey().write_masks);
}

/// Gives the type of the cached copy of a group of inputs.
struct DynamicInputsCopy {
    template <typename... Fields>
    auto operator()(const Fields&...) const {
        static_assert((std::is_trivially_copyable_v<Fields> && ...));
        static_assert(((sizeof(Fields) % sizeof(u32) == 0) && ...));
        return std::array<u32, (... + sizeof(Fields)) / sizeof(u32)>{};
    }
};

/// Brings the cached copy of a group of inputs up to date field by field, without building a
/// temporary copy. Returns whether any field changed.
template <size_t N>
struct DynamicInputsRefresh {
    std::array<u32, N>& cache;

    template <typename... Fields>
    bool operator()(const Fields&... fields) const {
        static_assert((... + sizeof(Fields)) == sizeof(cache));
        bool changed = false;
        u32* cached = cache.data();
        const auto refresh = [&](const auto& field) {
            if (std::memcmp(cached, &field, sizeof(field)) != 0) {
                std::memcpy(cached, &field, sizeof(field));
                changed = true;
            }
            cached += sizeof(field) / sizeof(u32);
        };
        (refresh(fields), ...);
        return changed;
    }
};

template <size_t N>
[[nodiscard]] static DynamicInputsRefresh<N> RefreshInputs(std::array<u32, N>& cache) {
    return {cache};
}

struct Rasterizer::DynamicStateInputCache {
    decltype(ViewportInputs(std::declval<const AmdGpu::Regs&>(), DynamicInputsCopy{})) viewport{};
    decltype(DepthStencilInputs(std::declval<const AmdGpu::Regs&>(),
                                DynamicInputsCopy{})) depth_stencil{};
    decltype(PrimitiveInputs(std::declval<const AmdGpu::Regs&>(), DynamicInputsCopy{})) primitive{};
    decltype(RasterizationInputs(std::declval<const AmdGpu::Regs&>(),
                                 DynamicInputsCopy{})) rasterization{};
    decltype(BlendInputs(std::declval<const AmdGpu::Regs&>(),
                         std::declval<const GraphicsPipeline&>(), DynamicInputsCopy{})) blend{};
    bool valid{};
};

namespace {

/// Number of guest copy workers. SHADPS4_ASYNC_COPIES=0 keeps copies on the command processor,
/// SHADPS4_COPY_WORKERS=N overrides the worker count.
[[nodiscard]] u32 GuestCopyWorkerCount() {
    if (const char* env = std::getenv("SHADPS4_ASYNC_COPIES"); env != nullptr && env[0] == '0') {
        return 0;
    }
    if (EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise) {
        // Precise readbacks read-protect GPU-written pages; a worker touching one would fault
        // into the caches from outside the command processor thread.
        return 0;
    }
    if (const char* env = std::getenv("SHADPS4_COPY_WORKERS"); env != nullptr) {
        return static_cast<u32>(std::clamp(std::atoi(env), 0, 16));
    }
    // A job takes about two microseconds and arrives once per draw, so two workers keep up.
    // Idle workers spin before parking, and more of them take cores and boost clock from the
    // command processor and the Vulkan recording thread.
    return 2;
}

[[nodiscard]] u64 HashTextureLookup(const AmdGpu::Image& sharp, u64 resource_key,
                                    u32 mip_index) noexcept {
    std::array<u64, sizeof(AmdGpu::Image) / sizeof(u64)> words;
    static_assert(sizeof(words) == sizeof(sharp));
    std::memcpy(words.data(), &sharp, sizeof(sharp));
    u64 value = words[0] * 0x9E3779B97F4A7C15ULL;
    value ^= std::rotl(words[1] * 0xC2B2AE3D27D4EB4FULL, 21);
    value ^= std::rotl(words[2] * 0x165667B19E3779F9ULL, 42);
    value ^= words[3] * 0xD6E8FEB86659FD93ULL;
    value ^= ((resource_key << 5) | mip_index) * 0x94D049BB133111EBULL;
    return value ^ (value >> 29);
}

/// SHADPS4_GPU_SHADOW_SERVE=0 makes uploads of GPU-owned guest ranges materialize guest RAM
/// again instead of copying the authority shadow on the GPU.
[[nodiscard]] bool GpuShadowServeEnabled() {
    const char* env = std::getenv("SHADPS4_GPU_SHADOW_SERVE");
    return env == nullptr || env[0] != '0';
}

} // Anonymous namespace

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, page_manager{this},
      buffer_cache{instance, scheduler, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool} {
    dynamic_state_inputs = std::make_unique<DynamicStateInputCache>();
    texture_lookup = std::make_unique<std::array<TextureLookupEntry, TextureLookupSize>>();
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
        scheduler.GateSubmitsOnGuestCopies();
        auto& copy_engine = VideoCore::GuestCopyEngine::Instance();
        copy_engine.SetReadProtectionProbe(
            [](const void* context, VAddr addr, u64 size) {
                return static_cast<const VideoCore::PageManager*>(context)->HasReadWatchers(addr,
                                                                                            size);
            },
            &page_manager);
        if (GpuShadowServeEnabled()) {
            copy_engine.SetProtectedCopyResolver(
                [](void* context, const VideoCore::GuestCopyEngine::Op& op,
                   std::span<VideoCore::GuestCopyEngine::Op,
                             VideoCore::GuestCopyEngine::MaxResolverRemainder>
                       remainder,
                   u32& remainder_count, u64& gpu_bytes) -> bool {
                    auto& rasterizer = *static_cast<Rasterizer*>(context);
                    return rasterizer.buffer_cache.ServeGuestCopyFromGpuShadows(
                        op, remainder, remainder_count, gpu_bytes, rasterizer.page_manager);
                },
                this);
        } else {
            LOG_INFO(Render_Vulkan, "GPU shadow serving disabled by SHADPS4_GPU_SHADOW_SERVE");
        }
        copy_engine.Start(GuestCopyWorkerCount());
    }
    memory->SetRasterizer(this);
    VideoCore::GpuAuthorityTracker::Instance().SetRasterizer(this);

    InitializeQueryPool();
}

Rasterizer::~Rasterizer() {
    VideoCore::GuestCopyEngine::Instance().Stop();
    VideoCore::GuestCopyEngine::Instance().SetReadProtectionProbe(nullptr, nullptr);
    VideoCore::GuestCopyEngine::Instance().SetProtectedCopyResolver(nullptr, nullptr);
    VideoCore::GpuAuthorityTracker::Instance().SetRasterizer(nullptr);

    DestroyQueryPool();
}

void Rasterizer::CpSync() {
    scheduler.EndRendering();
    auto cmdbuf = scheduler.CommandBuffer();

    const vk::MemoryBarrier ib_barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eDrawIndirect,
                           vk::DependencyFlagBits::eByRegion, ib_barrier, {}, {});
}

void Rasterizer::AcquireMemory(u32 cp_coher_cntl, VAddr base_address, u64 size) {

    vk::PipelineStageFlags2 src_stages = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2 src_access = vk::AccessFlagBits2::eNone;
    vk::PipelineStageFlags2 dst_stages = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2 dst_access = vk::AccessFlagBits2::eNone;

    if (cp_coher_cntl & (1u << 8)) {
        src_stages |= vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        src_access |= vk::AccessFlagBits2::eColorAttachmentWrite;
    }
    if (cp_coher_cntl & (1u << 9)) {
        src_stages |= vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                      vk::PipelineStageFlagBits2::eLateFragmentTests;
        src_access |= vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    }
    if (cp_coher_cntl & ((1u << 14) | (1u << 15))) {
        src_stages |= vk::PipelineStageFlagBits2::eComputeShader |
                      vk::PipelineStageFlagBits2::eAllGraphics |
                      vk::PipelineStageFlagBits2::eTransfer;
        src_access |= vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite |
                      vk::AccessFlagBits2::eMemoryWrite;
    }

    if (src_stages == vk::PipelineStageFlagBits2::eNone) {
        src_stages = vk::PipelineStageFlagBits2::eAllGraphics |
                     vk::PipelineStageFlagBits2::eComputeShader |
                     vk::PipelineStageFlagBits2::eTransfer;
        src_access = vk::AccessFlagBits2::eColorAttachmentWrite |
                     vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                     vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite |
                     vk::AccessFlagBits2::eMemoryWrite;
    }

    if (cp_coher_cntl & ((1u << 13) | (1u << 14))) {
        dst_stages |= vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eVertexShader |
                      vk::PipelineStageFlagBits2::eComputeShader;
        dst_access |= vk::AccessFlagBits2::eShaderRead;
    }
    if (cp_coher_cntl & (1u << 10)) {
        dst_stages |= vk::PipelineStageFlagBits2::eAllGraphics |
                      vk::PipelineStageFlagBits2::eComputeShader;
        dst_access |= vk::AccessFlagBits2::eUniformRead;
    }

    if (dst_stages == vk::PipelineStageFlagBits2::eNone) {
        dst_stages = vk::PipelineStageFlagBits2::eAllGraphics |
                     vk::PipelineStageFlagBits2::eComputeShader |
                     vk::PipelineStageFlagBits2::eTransfer;
        dst_access = vk::AccessFlagBits2::eShaderRead |
                     vk::AccessFlagBits2::eColorAttachmentRead |
                     vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                     vk::AccessFlagBits2::eUniformRead | vk::AccessFlagBits2::eTransferRead |
                     vk::AccessFlagBits2::eMemoryRead;
    }

    AccumulateFlush(src_stages, src_access, dst_stages, dst_access);
}

void Rasterizer::FlushCaches(AmdGpu::EventType event_type) {

    vk::PipelineStageFlags2 src_stages = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2 src_access = vk::AccessFlagBits2::eNone;

    switch (event_type) {
    case AmdGpu::EventType::CacheFlush:
    case AmdGpu::EventType::CacheFlushTs:
    case AmdGpu::EventType::CacheFlushAndInvEvent:
    case AmdGpu::EventType::CacheFlushAndInvTsEvent:
        src_stages = vk::PipelineStageFlagBits2::eAllGraphics |
                     vk::PipelineStageFlagBits2::eComputeShader |
                     vk::PipelineStageFlagBits2::eTransfer;
        src_access = vk::AccessFlagBits2::eColorAttachmentWrite |
                     vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                     vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite;
        break;
    case AmdGpu::EventType::FlushAndInvCbDataTs:
    case AmdGpu::EventType::FlushAndInvCbMeta:
    case AmdGpu::EventType::FlushAndInvCbPixelData:
        src_stages = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        src_access = vk::AccessFlagBits2::eColorAttachmentWrite;
        break;
    case AmdGpu::EventType::FlushAndInvDbDataTs:
    case AmdGpu::EventType::FlushAndInvDbMeta:
    case AmdGpu::EventType::DbCacheFlushAndInv:
        src_stages = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                     vk::PipelineStageFlagBits2::eLateFragmentTests;
        src_access = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
        break;
    case AmdGpu::EventType::CsPartialFlush:
    case AmdGpu::EventType::CsDone:
        src_stages = vk::PipelineStageFlagBits2::eComputeShader;
        src_access = vk::AccessFlagBits2::eShaderWrite;
        break;
    case AmdGpu::EventType::VsPartialFlush:
    case AmdGpu::EventType::PsPartialFlush:
        src_stages = vk::PipelineStageFlagBits2::eAllGraphics;
        src_access = vk::AccessFlagBits2::eShaderWrite |
                     vk::AccessFlagBits2::eColorAttachmentWrite;
        break;
    default:
        return;
    }

    AccumulateFlush(src_stages, src_access,
                    vk::PipelineStageFlagBits2::eAllGraphics |
                        vk::PipelineStageFlagBits2::eComputeShader |
                        vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eColorAttachmentRead |
                        vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                        vk::AccessFlagBits2::eUniformRead | vk::AccessFlagBits2::eTransferRead |
                        vk::AccessFlagBits2::eMemoryRead);
}

void Rasterizer::AccumulateFlush(vk::PipelineStageFlags2 src_stages, vk::AccessFlags2 src_access,
                                 vk::PipelineStageFlags2 dst_stages,
                                 vk::AccessFlags2 dst_access) {
    // Tracked resources written before this point get a barrier when they are next accessed
    // (FlushEpoch); nothing is recorded for the packet itself.
    VideoCore::FlushEpoch::Advance();
    pending_flush_src_stages |= src_stages;
    pending_flush_src_access |= src_access;
    pending_flush_dst_stages |= dst_stages;
    pending_flush_dst_access |= dst_access;
    if (dma_access_pending) {
        // A pipeline accessed memory through device addresses, which no resource tracks.
        EmitPendingGlobalBarrier();
    }
}

void Rasterizer::EmitPendingGlobalBarrier() {
    dma_access_pending = false;
    if (pending_flush_src_stages == vk::PipelineStageFlagBits2::eNone) {
        return;
    }
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = pending_flush_src_stages,
        .srcAccessMask = pending_flush_src_access,
        .dstStageMask = pending_flush_dst_stages,
        .dstAccessMask = pending_flush_dst_access,
    };
    pending_flush_src_stages = vk::PipelineStageFlagBits2::eNone;
    pending_flush_src_access = vk::AccessFlagBits2::eNone;
    pending_flush_dst_stages = vk::PipelineStageFlagBits2::eNone;
    pending_flush_dst_access = vk::AccessFlagBits2::eNone;
    scheduler.EndRendering();
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

void Rasterizer::FullGpuBarrier() {
    scheduler.EndRendering();
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead |
                         vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eColorAttachmentRead |
                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                         vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                         vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead |
                         vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask =
            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite |
            vk::AccessFlagBits2::eDepthStencilAttachmentRead |
            vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eUniformRead |
            vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
    };
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

void Rasterizer::GpuFenceWait() {
    FullGpuBarrier();
}

u64 Rasterizer::CurrentTick() const noexcept {
    return scheduler.CurrentTick();
}

u64 Rasterizer::KnownGpuTick() const noexcept {
    return scheduler.KnownGpuTick();
}

bool Rasterizer::IsGpuThread() const noexcept {
    return liverpool->IsGpuThread();
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

SHAD_NO_INLINE VideoCore::ImageId Rasterizer::FindColorTarget(u32 cb, bool same_desc) {
    const auto& col_buf = liverpool->regs.color_buffers[cb];
    auto& desc = cb_descs[cb].second;
    auto& cached = cached_color_targets[cb];
    if (!same_desc) {
        const auto& hint = liverpool->last_cb_extent[cb];
        std::construct_at(&desc, col_buf, hint);
        std::memcpy(&cached.buffer, &col_buf, sizeof(col_buf));
        cached.hint = hint.raw;
    } else {
        desc.view_info = VideoCore::ImageViewInfo{col_buf};
    }
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image = texture_cache.GetImage(image_id);
    cached.image_id = image_id;
    cached.image_uid = image.image_uid;
    cached.topology_epoch = texture_cache.TopologyEpoch();
    return image_id;
}

SHAD_NO_INLINE VideoCore::ImageId Rasterizer::FindDepthTarget(VAddr htile_address, bool same_desc) {
    const auto& regs = liverpool->regs;
    auto& desc = db_desc.second;
    auto& cached = cached_depth_target;
    if (!same_desc) {
        const auto& hint = liverpool->last_db_extent;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        std::memcpy(&cached.buffer, &regs.depth_buffer, sizeof(cached.buffer));
        std::memcpy(&cached.view, &regs.depth_view, sizeof(cached.view));
        std::memcpy(&cached.control, &regs.depth_control, sizeof(cached.control));
        cached.htile_address = htile_address;
        cached.hint = hint.raw;
    } else {
        desc.view_info =
            VideoCore::ImageViewInfo{regs.depth_buffer, regs.depth_view, regs.depth_control};
    }
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image = texture_cache.GetImage(image_id);
    cached.image_id = image_id;
    cached.image_uid = image.image_uid;
    cached.topology_epoch = texture_cache.TopologyEpoch();
    return image_id;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    static_assert(std::is_trivially_copyable_v<AmdGpu::ColorBuffer>);
    static_assert(std::is_trivially_copyable_v<AmdGpu::DepthBuffer>);
    static_assert(std::is_trivially_copyable_v<AmdGpu::DepthView>);
    static_assert(std::is_trivially_copyable_v<AmdGpu::DepthControl>);
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->regs;
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = liverpool->last_cb_extent[cb];
        auto& cached = cached_color_targets[cb];
        const bool same_desc = cached.image_id && cached.hint == hint.raw &&
                               std::memcmp(&cached.buffer, &col_buf, sizeof(col_buf)) == 0;
        if (!same_desc || !texture_cache.TryReuseImage(cached.image_id, cached.image_uid,
                                                       cached.topology_epoch)) [[unlikely]] {
            image_id = FindColorTarget(cb, same_desc);
        } else {
            image_id = cached.image_id;
        }
        bound_images.emplace_back(image_id);
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    const auto depth_stencil = GetEffectiveDepthStencilState(regs);
    if (depth_stencil.needs_attachment) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->last_db_extent;
        auto& [image_id, desc] = db_desc;
        auto& cached = cached_depth_target;
        const bool same_desc =
            cached.image_id && cached.hint == hint.raw && cached.htile_address == htile_address &&
            std::memcmp(&cached.buffer, &regs.depth_buffer, sizeof(cached.buffer)) == 0 &&
            std::memcmp(&cached.view, &regs.depth_view, sizeof(cached.view)) == 0 &&
            std::memcmp(&cached.control, &regs.depth_control, sizeof(cached.control)) == 0;
        if (!same_desc || !texture_cache.TryReuseImage(cached.image_id, cached.image_uid,
                                                       cached.topology_epoch)) [[unlikely]] {
            image_id = FindDepthTarget(htile_address, same_desc);
        } else {
            image_id = cached.image_id;
        }
        bound_images.emplace_back(image_id);
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<const Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.user_data[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.user_data[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->regs.color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->last_cb_extent[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    image.Clear(clear_value, desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    ProcessPendingOcclusionReadbacks();

    if (!FilterDraw()) {
        return;
    }

    const auto& regs = liverpool->regs;
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    buffer_cache.PrepareVertexIndexBuffers(*pipeline, is_indexed, index_offset);
    const auto state = BeginRendering(pipeline);
    buffer_cache.FinalizeStreamCopyBatch();
    FinalizeBuffers(push_data, true);
    buffer_cache.FinalizeVertexIndexBuffers(buffer_barriers);

    BindPipelineResources(pipeline);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    const auto& vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const auto cmdbuf = scheduler.CommandBuffer();
    scheduler.BindGraphicsPipeline(pipeline->Handle());

    const auto draw = [&] {
        if (is_indexed) {
            cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                               s32(vertex_offset), instance_offset);
        } else {
            cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                        instance_offset);
        }
    };
    draw();
    if (BindSquarePass(*pipeline)) [[unlikely]] {
        draw();
    }
    DebugState.IncDrawCall();
    MarkImageWrites(false);

    ResetBindings();
}

bool Rasterizer::BindSquarePass(const GraphicsPipeline& pipeline) {
    const vk::Pipeline square_pipeline = pipeline.SquarePassHandle();
    if (!square_pipeline) [[likely]] {
        return false;
    }
    // GNM scales the MIN/MAX operands, MIN(s * s, d * d) = MIN(s, d)^2, so a second draw with the
    // same coverage squares what the first one blended. A depth or stencil write of the first
    // draw would change the coverage of the second one.
    const auto depth_stencil = GetEffectiveDepthStencilState(liverpool->regs);
    if (depth_stencil.depth_write_enable || depth_stencil.stencil_write_enable) {
        if (pipeline.ReportSquarePassSkipped()) {
            LOG_WARNING(Render_Vulkan,
                        "Scaled MIN/MAX blend is not squared for a draw that writes depth or "
                        "stencil");
        }
        return false;
    }
    scheduler.BindGraphicsPipeline(square_pipeline);
    return true;
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    ProcessPendingOcclusionReadbacks();

    if (!FilterDraw()) {
        return;
    }

    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    buffer_cache.PrepareVertexIndexBuffers(*pipeline, is_indexed, 0);
    const auto state = BeginRendering(pipeline);
    buffer_cache.FinalizeStreamCopyBatch();
    FinalizeBuffers(push_data, true);
    buffer_cache.FinalizeVertexIndexBuffers(buffer_barriers);

    const auto& [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);

    VideoCore::Buffer* count_buffer{};
    u32 count_base{};
    if (count_address != 0) {
        std::tie(count_buffer, count_base) = buffer_cache.ObtainBuffer(count_address, 4, false);
    }

    if (auto barrier = buffer->GetBarrier(vk::AccessFlagBits2::eIndirectCommandRead,
                                          vk::PipelineStageFlagBits2::eDrawIndirect)) {
        buffer_barriers.emplace_back(*barrier);
    }
    if (count_buffer) {
        if (auto barrier = count_buffer->GetBarrier(vk::AccessFlagBits2::eIndirectCommandRead,
                                                    vk::PipelineStageFlagBits2::eDrawIndirect)) {
            buffer_barriers.emplace_back(*barrier);
        }
    }

    BindPipelineResources(pipeline);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    // We can safely ignore both SGPR UD indices and results of fetch shader parsing, as vertex and
    // instance offsets will be automatically applied by Vulkan from indirect args buffer.

    const auto cmdbuf = scheduler.CommandBuffer();
    scheduler.BindGraphicsPipeline(pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);
    }
    const auto draw = [&] {
        if (is_indexed) {
            if (count_address != 0) {
                cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                                count_base, max_count, stride);
            } else {
                cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
            }
        } else {
            if (count_address != 0) {
                cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                         count_base, max_count, stride);
            } else {
                cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
            }
        }
    };
    draw();
    if (BindSquarePass(*pipeline)) [[unlikely]] {
        draw();
    }
    DebugState.IncDrawCall();
    MarkImageWrites(false);

    ResetBindings();
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    ProcessPendingOcclusionReadbacks();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    scheduler.EndRendering();
    BindPipelineResources(pipeline);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    DebugState.IncDispatch();
    MarkImageWrites(true);

    ResetBindings();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();
    ProcessPendingOcclusionReadbacks();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);

    if (auto barrier = buffer->GetBarrier(vk::AccessFlagBits2::eIndirectCommandRead,
                                          vk::PipelineStageFlagBits2::eDrawIndirect)) {
        buffer_barriers.emplace_back(*barrier);
    }

    scheduler.EndRendering();
    BindPipelineResources(pipeline);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatchIndirect(buffer->Handle(), base);
    DebugState.IncDispatch();
    MarkImageWrites(true);

    ResetBindings();
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
    texture_cache.ProcessDownloadImages(VideoCore::TextureCache::DownloadContext{});
    texture_cache.RunGarbageCollector();
    buffer_cache.RunGarbageCollector();
    Flush();
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (pipeline->IsCompute() &&
        (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
         IsComputeImageClear(pipeline))) [[unlikely]] {
        return false;
    }

    buffer_cache.BeginStreamCopyBatch();
    set_write_index = 0;
    set_writes.clear();
    buffer_barriers.clear();
    buffer_infos.clear();
    image_infos.clear();
    pending_buffer_bindings.clear();
    stream_buffer_bindings.clear();
    potential_write_images.clear();

    bool uses_dma = false;

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    {
        push_data = MakeUserData(liverpool->regs);
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        {
            stage->PushUd(binding, push_data);
        }
        {
            const u32 first_binding = static_cast<u32>(pending_buffer_bindings.size());
            PrepareBuffers(*stage, binding);
            FinalizeBuffers(push_data, false, first_binding);
        }
        {
            BindTextures(*stage, binding);
        }
        uses_dma |= stage->uses_dma;
    }

    if (pipeline->IsCompute()) {
        buffer_cache.FinalizeStreamCopyBatch();
        FinalizeBuffers(push_data, true);
    }

    if (uses_dma) {
        // Memory reached through device addresses escapes resource tracking: the flushes the
        // guest issued since the last global barrier apply to all of it, and whatever the
        // pipeline writes gets a global barrier at the next flush.
        EmitPendingGlobalBarrier();
        dma_access_pending = true;
        SynchronizeDmaBuffers();
    }

    return true;
}

SHAD_NO_INLINE void Rasterizer::SynchronizeDmaBuffers() {
    // We only use fault buffer for DMA right now.
    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    for (auto& range : mapped_ranges) {
        buffer_cache.SynchronizeBuffersInRange(range.lower(), range.upper() - range.lower());
    }
    fault_process_pending = true;
}

SHAD_NO_INLINE void Rasterizer::CaptureDescriptorState(const Pipeline* pipeline) {
    auto& state = descriptor_state;
    state.pipeline = pipeline;
    state.command_buffer_tick = scheduler.CurrentTick();
    state.push_descriptor_epoch = scheduler.GraphicsPushDescriptorEpoch();
    state.writes.clear();
    state.image_infos.clear();
    state.buffer_infos.clear();

    for (const auto& write : set_writes) {
        const bool is_buffer = write.pBufferInfo != nullptr;
        if (write.pImageInfo == nullptr && !is_buffer) {
            continue;
        }
        if (state.writes.size() >= state.writes.capacity()) [[unlikely]] {
            ReportDescriptorStateOverflow();
        }
        const u32 first_info = is_buffer ? static_cast<u32>(state.buffer_infos.size())
                                         : static_cast<u32>(state.image_infos.size());
        state.writes.push_back({
            .key0 = DescriptorWriteKey0(write),
            .key1 = DescriptorWriteKey1(write),
            .first_info = first_info,
            .is_buffer = is_buffer,
        });
        if (is_buffer) {
            if (state.buffer_infos.size() + write.descriptorCount >
                state.buffer_infos.capacity()) [[unlikely]] {
                ReportDescriptorStateOverflow();
            }
            AppendDescriptorInfos(state.buffer_infos, write.pBufferInfo, write.descriptorCount);
        } else {
            if (state.image_infos.size() + write.descriptorCount >
                state.image_infos.capacity()) [[unlikely]] {
                ReportDescriptorStateOverflow();
            }
            AppendDescriptorInfos(state.image_infos, write.pImageInfo, write.descriptorCount);
        }
    }
    state.valid = true;
}

void Rasterizer::MarkImageWrites(bool is_compute) {
    if (!is_compute) {
        for (const auto image_id : bound_images) {
            auto& image = texture_cache.GetImage(image_id);
            if (image.binding.is_target) {
                image.MarkWrite();
                if (!image.info.props.is_depth) {
                    texture_cache.ScheduleRenderTargetDownload(image_id);
                }
            }
        }
    }
    for (const auto image_id : potential_write_images) {
        auto& image = texture_cache.GetImage(image_id);
        if (is_compute || !image.binding.is_target) {
            image.MarkWrite();
            if (is_compute) {
                texture_cache.ScheduleComputeDownload(image_id);
            }
        }
    }
}

void Rasterizer::BindPipelineResources(const Pipeline* pipeline) {
    if (pipeline->IsCompute() || !pipeline->UsesPushDescriptors()) {
        pipeline->BindResources(set_writes, buffer_barriers, push_data);
        return;
    }

    auto& cached = descriptor_state;
    const u64 command_buffer_tick = scheduler.CurrentTick();
    const bool can_reuse = cached.valid && cached.pipeline == pipeline &&
                           cached.command_buffer_tick == command_buffer_tick &&
                           cached.push_descriptor_epoch == scheduler.GraphicsPushDescriptorEpoch();
    bool can_update_cached = can_reuse && cached.writes.size() == set_writes.size();
    partial_set_writes.clear();
    u32 cached_write_index = 0;
    u32 partial_descriptor_count = 0;
    {
        for (const auto& write : set_writes) {
            bool unchanged = false;
            const bool is_buffer = write.pBufferInfo != nullptr;
            const bool is_cacheable = write.pImageInfo != nullptr || is_buffer;
            if (is_cacheable && can_reuse && cached_write_index < cached.writes.size()) {
                const auto& old_write = cached.writes[cached_write_index];
                const u64 metadata_difference =
                    (old_write.key0 ^ DescriptorWriteKey0(write)) |
                    (old_write.key1 ^ DescriptorWriteKey1(write)) |
                    static_cast<u64>(old_write.is_buffer != is_buffer);
                unchanged = metadata_difference == 0;
                can_update_cached &= unchanged;
                if (unchanged && is_buffer) {
                    const auto* lhs = cached.buffer_infos.data() + old_write.first_info;
                    unchanged = DescriptorInfosEqual(lhs, write.pBufferInfo, write.descriptorCount);
                    if (!unchanged && can_update_cached) {
                        CopyDescriptorInfos(cached.buffer_infos.data() + old_write.first_info,
                                            write.pBufferInfo, write.descriptorCount);
                    }
                } else if (unchanged) {
                    const auto* lhs = cached.image_infos.data() + old_write.first_info;
                    unchanged = DescriptorInfosEqual(lhs, write.pImageInfo, write.descriptorCount);
                    if (!unchanged && can_update_cached) {
                        CopyDescriptorInfos(cached.image_infos.data() + old_write.first_info,
                                            write.pImageInfo, write.descriptorCount);
                    }
                }
                ++cached_write_index;
            } else if (is_cacheable) {
                can_update_cached = false;
                ++cached_write_index;
            } else {
                can_update_cached = false;
            }
            if (!unchanged) {
                partial_set_writes.push_back(write);
                partial_descriptor_count += write.descriptorCount;
            }
        }
    }

    // An empty partial_set_writes only flushes barriers and push constants.
    pipeline->BindResources(partial_set_writes, buffer_barriers, push_data,
                            partial_descriptor_count);
    if (!can_reuse || !partial_set_writes.empty() || cached_write_index != cached.writes.size()) {
        if (can_update_cached) {
            cached.command_buffer_tick = scheduler.CurrentTick();
            cached.push_descriptor_epoch = scheduler.GraphicsPushDescriptorEpoch();
        } else {
            CaptureDescriptorState(pipeline);
        }
    }
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (u32 index = 0; index < info.buffers.size(); ++index) {
        const auto& desc = info.buffers[index];
        const VAddr address = GetResolvedBuffer(info, index).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (u32 index = 0; index < info.buffers.size(); ++index) {
            const auto& desc = info.buffers[index];
            const VAddr address = GetResolvedBuffer(info, index).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = GetResolvedBuffer(info, 0);
    const AmdGpu::Buffer buf1 = GetResolvedBuffer(info, 1);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    if (instance.IsMaintenance8Supported() ||
        src_image.info.props.is_depth == dst_image.info.props.is_depth) {
        dst_image.CopyImage(src_image);
    } else {
        const auto& copy_buffer =
            buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::DeviceLocal);
        dst_image.CopyImageWithBuffer(src_image, copy_buffer.Handle(), 0);
    }
    dst_image.flags |= VideoCore::ImageFlagBits::GpuModified;
    dst_image.flags &= ~VideoCore::ImageFlagBits::Dirty;
    // Only the image was written; guest memory is untouched, so the buffers over it are not
    // invalidated. Uploading guest memory would overwrite them with bytes older than the image,
    // and texel buffer consumers resynchronize from the image once its epoch moves.
    page_manager.NotifyWrite(dst_image.info.guest_address, dst_image.info.guest_size,
                             VideoCore::MemoryWriteSource::CommandProcessor);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = GetResolvedBuffer(info, 0);
    const AmdGpu::Buffer buf1 = GetResolvedBuffer(info, 1);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    image1.Clear(clear, range);
    image1.flags |= VideoCore::ImageFlagBits::GpuModified;
    image1.flags &= ~VideoCore::ImageFlagBits::Dirty;
    // As with the image copy above, guest memory is untouched and the buffers stay valid.
    page_manager.NotifyWrite(image1.info.guest_address, image1.info.guest_size,
                             VideoCore::MemoryWriteSource::CommandProcessor);
    return true;
}

void Rasterizer::PrepareBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    const u32 stage_index = static_cast<u32>(stage.l_stage);
    ASSERT(stage_index < MaxShaderStages);
    ASSERT(pending_buffer_bindings.size() + stage.buffers.size() <= Shader::NUM_BUFFERS);

    for (u32 i = 0; i < stage.buffers.size(); ++i) {
        const auto& desc = stage.buffers[i];
        const auto vsharp = GetResolvedBuffer(stage, i);
        const u64 alignment = instance.StorageMinAlignment();

        auto& pending = pending_buffer_bindings.emplace_back(PendingBufferBinding{
            .desc = &desc,
            .sharp = vsharp,
            .alignment = alignment,
            .unified_binding = binding.unified++,
            .buffer_binding = binding.buffer++,
            .set_write_index = set_write_index++,
        });

        if (!desc.IsSpecial() && vsharp.base_address != 0 && vsharp.GetSize() > 0) {
            const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
            pending.size = size;
            if (!desc.is_written && size <= VideoCore::BufferCache::CACHING_PAGESIZE &&
                !buffer_cache.HasGpuReadSource(vsharp.base_address, size)) {
                pending.stream_source = VideoCore::BufferCache::StreamCopySource::Guest;
                pending.stream_index =
                    buffer_cache.QueueStreamCopy(VideoCore::BufferCache::StreamCopyRequest{
                        .source_type = VideoCore::BufferCache::StreamCopySource::Guest,
                        .guest_address = vsharp.base_address,
                        .size = static_cast<u32>(size),
                        .alignment = alignment,
                    });
                stream_buffer_bindings.push_back(static_cast<u8>(pending_buffer_bindings.size() - 1));
                continue;
            }

            auto& cached = cached_buffer_bindings[stage_index][i];
            if (cached.owner != &stage) {
                cached = {};
                cached.owner = &stage;
            }

            const bool cache_hit =
                cached.valid && cached.address == vsharp.base_address && cached.size == size &&
                cached.topology_epoch == buffer_cache.TopologyEpoch() &&
                buffer_cache.IsBufferCacheEntryValid(cached.buffer_id, cached.buffer_uid,
                                                     vsharp.base_address, size);
            if (cache_hit) {
                pending.buffer_id = cached.buffer_id;
                continue;
            }

            pending.buffer_id = buffer_cache.FindBuffer(vsharp.base_address, size);
            cached.topology_epoch = buffer_cache.TopologyEpoch();
            cached.address = vsharp.base_address;
            cached.buffer_id = pending.buffer_id;
            cached.buffer_uid = buffer_cache.GetBufferUid(pending.buffer_id);
            cached.size = size;
            cached.valid = true;
            continue;
        }

        if (desc.buffer_type == Shader::BufferType::Flatbuf) {
            pending.size = stage.flattened_ud_buf.size() * sizeof(u32);
            if (pending.size != 0) {
                pending.stream_source = VideoCore::BufferCache::StreamCopySource::Host;
                pending.stream_index =
                    buffer_cache.QueueStreamCopy(VideoCore::BufferCache::StreamCopyRequest{
                        .source_type = VideoCore::BufferCache::StreamCopySource::Host,
                        .host_address = reinterpret_cast<const u8*>(stage.flattened_ud_buf.data()),
                        .size = static_cast<u32>(pending.size),
                        .alignment = alignment,
                    });
                stream_buffer_bindings.push_back(static_cast<u8>(pending_buffer_bindings.size() - 1));
            }
            continue;
        }

        if (desc.buffer_type == Shader::BufferType::SharedMemory) {
            const auto& cs_program = liverpool->GetCsRegs();
            pending.size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
            if (pending.size != 0) {
                ASSERT(pending.size <= std::numeric_limits<u32>::max());
                pending.stream_source = VideoCore::BufferCache::StreamCopySource::Zero;
                pending.stream_index =
                    buffer_cache.QueueStreamCopy(VideoCore::BufferCache::StreamCopyRequest{
                        .source_type = VideoCore::BufferCache::StreamCopySource::Zero,
                        .size = static_cast<u32>(pending.size),
                        .alignment = alignment,
                        .deduplicate = false,
                    });
                stream_buffer_bindings.push_back(static_cast<u8>(pending_buffer_bindings.size() - 1));
            }
        }
    }
}

void Rasterizer::WriteBufferDescriptor(const PendingBufferBinding& pending) {
    auto& set_write = set_writes[pending.set_write_index];
    set_write.dstSet = VK_NULL_HANDLE;
    set_write.dstBinding = pending.unified_binding;
    set_write.dstArrayElement = 0;
    set_write.descriptorCount = 1;
    set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
    set_write.pBufferInfo = &buffer_infos.back();
}

SHAD_NO_INLINE void Rasterizer::FinalizeCachedBuffer(Shader::PushData& push_data,
                                                     PendingBufferBinding& pending) {
    const auto& desc = *pending.desc;
    const auto& vsharp = pending.sharp;
    if (!pending.buffer_id) {
        if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
            const auto* gds_buf = buffer_cache.GetGdsBuffer();
            buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
        } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
            // Permutations compiled without enabled planes never read the buffer, so the
            // declared binding is satisfied with a null descriptor instead of a copy.
            if (liverpool->regs.clipper_control.user_clip_plane_enable == 0) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                    const auto& plane = liverpool->regs.clip_user_data[i];
                    planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                    planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                    planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                    planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                }
                const u32 ubo_size = static_cast<u32>(sizeof(planes));
                const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, pending.alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            }
        } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
            const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
            buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
        } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
            const auto* fault_buffer = buffer_cache.GetFaultBuffer();
            buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
        } else {
            buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
        }
    } else {
        const auto [vk_buffer, offset] =
            buffer_cache.ObtainBuffer(vsharp.base_address, pending.size, desc.is_written,
                                      desc.is_formatted, pending.buffer_id);
        const u64 offset_aligned = Common::AlignDown(offset, u64{pending.alignment});
        const u32 adjust = static_cast<u32>(offset - offset_aligned);
        ASSERT(adjust % 4 == 0);
        push_data.AddOffset(pending.buffer_binding, adjust);
        buffer_infos.emplace_back(vk_buffer->Handle(), offset_aligned, pending.size + adjust);
        const vk::AccessFlags2 shader_access =
            desc.is_written ? vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite
                            : vk::AccessFlagBits2::eShaderRead;
        if (auto barrier =
                vk_buffer->GetBarrier(shader_access, vk::PipelineStageFlagBits2::eAllCommands)) {
            buffer_barriers.emplace_back(*barrier);
        }
        if (desc.is_written) {
            texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, pending.size);
        }
    }
    WriteBufferDescriptor(pending);
}

void Rasterizer::FinalizeBuffers(Shader::PushData& push_data, bool stream_only, u32 first_binding) {
    static constexpr u16 NoStreamCopy = std::numeric_limits<u16>::max();

    // The stream copy batch serves most bindings of a draw; the other kinds of binding are
    // finalized out of line.
    const auto finalize_stream =
        [&](PendingBufferBinding& pending) {
            const auto& result = buffer_cache.GetStreamCopyResult(pending.stream_index);
            const u64 offset_aligned = Common::AlignDown(result.offset, u64{pending.alignment});
            const u32 adjust = static_cast<u32>(result.offset - offset_aligned);
            if (pending.stream_source == VideoCore::BufferCache::StreamCopySource::Guest) {
                ASSERT(adjust % 4 == 0);
                push_data.AddOffset(pending.buffer_binding, adjust);
                buffer_infos.emplace_back(result.buffer->Handle(), offset_aligned,
                                          pending.size + adjust);
                if (auto barrier =
                        result.buffer->GetBarrier(vk::AccessFlagBits2::eShaderRead,
                                                  vk::PipelineStageFlagBits2::eAllCommands)) {
                    buffer_barriers.emplace_back(*barrier);
                }
            } else {
                buffer_infos.emplace_back(result.buffer->Handle(), offset_aligned,
                                          pending.size + adjust);
            }
            WriteBufferDescriptor(pending);
        };
    if (stream_only) {
        for (const u8 index : stream_buffer_bindings) {
            auto& pending = pending_buffer_bindings[index];
            if (pending.stream_index != NoStreamCopy) [[likely]] {
                finalize_stream(pending);
            } else {
                FinalizeCachedBuffer(push_data, pending);
            }
        }
    } else {
        for (u32 index = first_binding; index < pending_buffer_bindings.size(); ++index) {
            auto& pending = pending_buffer_bindings[index];
            if (pending.stream_index == NoStreamCopy) {
                FinalizeCachedBuffer(push_data, pending);
            }
        }
    }
}

static SHAD_NO_INLINE void LogRejectedImage(const AmdGpu::Image& tsharp) {
    LOG_WARNING(Render_Vulkan,
                "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                "data_format={}, num_format={}",
                tsharp.Address(), tsharp.pitch, tsharp.width, static_cast<u32>(tsharp.GetDataFmt()),
                static_cast<u32>(tsharp.GetNumberFmt()));
}

static void SelectImageMip(VideoCore::TextureCache::ImageDesc& desc,
                           const Shader::ImageResource& resource, u32 mip_index, u32 num_bindings) {
    using MipFallback = Shader::MipStorageFallbackMode;
    if (resource.mip_fallback_mode == MipFallback::ConstantIndex) {
        if (num_bindings != 1) [[unlikely]] {
            ValidateSingleImageBinding(num_bindings);
        }
        desc.view_info.range.base.level += resource.constant_mip_index;
        desc.view_info.range.extent.levels = 1;
    } else if (resource.mip_fallback_mode == MipFallback::DynamicIndex) {
        desc.view_info.range.base.level += mip_index;
        desc.view_info.range.extent.levels = 1;
    }
}

SHAD_NO_INLINE void Rasterizer::BindTextureMiss(const Shader::Info& stage, const u32 stage_index,
                                                const u32 image_index, const u32 mip_index,
                                                const u32 num_bindings, const AmdGpu::Image& tsharp,
                                                const u8 geometry_key, const u64 resource_key,
                                                CachedImageBinding& cached, bool& miss_desc_ready) {
    using ImageDesc = VideoCore::TextureCache::ImageDesc;
    const auto& image_desc = stage.images[image_index];
    TextureLookupEntry* lookup_fill{};
    if (auto& lookup = (*texture_lookup)[HashTextureLookup(tsharp, resource_key, mip_index) &
                                         (TextureLookupSize - 1)];
        lookup.valid && lookup.resource_key == resource_key && lookup.mip_index == mip_index &&
        std::memcmp(&lookup.sharp, &tsharp, sizeof(tsharp)) == 0 &&
        lookup.topology_epoch == texture_cache.TopologyEpoch() &&
        texture_cache.TryReuseImage(lookup.image_id, lookup.image_uid, lookup.topology_epoch)) {
        // The image description and FindImage depend only on the T# and the fields in
        // resource_key, so another program's lookup of the same T# is valid here.
        image_bindings.emplace_back(ImageBindingInfo{
            .image_id = lookup.image_id,
            .view_info = lookup.view_info,
            .source_index = static_cast<u8>(image_index),
            .mip_index = static_cast<u8>(mip_index),
            .is_storage = image_desc.is_written,
        });
    } else {
        lookup_fill = &lookup;
        auto& miss_desc = texture_miss_desc;
        if (!miss_desc_ready) {
            auto& description = cached_image_descriptions[stage_index][image_index];
            const bool same_geometry =
                description.base_desc && description.owner == &stage &&
                description.geometry_key == geometry_key &&
                std::memcmp(&description.sharp, &tsharp, sizeof(tsharp)) == 0;
            if (!same_geometry) {
                if (!description.base_desc) {
                    description.base_desc = std::make_unique<ImageDesc>(tsharp, image_desc);
                } else {
                    *description.base_desc = ImageDesc{tsharp, image_desc};
                }
                description.owner = &stage;
                description.sharp = tsharp;
                description.geometry_key = geometry_key;
            }
            miss_desc = *description.base_desc;
            texture_miss_base_view = miss_desc.view_info;
            miss_desc_ready = true;
        }
        miss_desc.view_info = texture_miss_base_view;
        SelectImageMip(miss_desc, image_desc, mip_index, num_bindings);
        const auto image_id = texture_cache.FindImage(miss_desc);
        image_bindings.emplace_back(ImageBindingInfo{
            .image_id = image_id,
            .view_info = miss_desc.view_info,
            .source_index = static_cast<u8>(image_index),
            .mip_index = static_cast<u8>(mip_index),
            .is_storage = image_desc.is_written,
        });
    }

    const auto& image_binding = image_bindings.back();
    const auto& image = texture_cache.GetImage(image_binding.image_id);
    const u64 topology_epoch = texture_cache.TopologyEpoch();
    cached.sharp = tsharp;
    cached.resource_key = resource_key;
    cached.source_index = static_cast<u8>(image_index);
    cached.mip_index = static_cast<u8>(mip_index);
    cached.image_id = image_binding.image_id;
    cached.image_uid = image.image_uid;
    cached.topology_epoch = topology_epoch;
    cached.view_info = image_binding.view_info;
    cached.valid = true;
    if (lookup_fill != nullptr) {
        *lookup_fill = TextureLookupEntry{
            .sharp = tsharp,
            .resource_key = resource_key,
            .image_id = image_binding.image_id,
            .mip_index = mip_index,
            .image_uid = image.image_uid,
            .topology_epoch = topology_epoch,
            .view_info = image_binding.view_info,
            .valid = true,
        };
    }
}

SHAD_NO_INLINE void Rasterizer::RebindTexture(const Shader::Info& stage,
                                              ImageBindingInfo& image_binding) {
    // Built from the T# because a lookup hit leaves the cached description of this slot
    // untouched.
    const u32 source_index = image_binding.source_index;
    VideoCore::TextureCache::ImageDesc desc{GetResolvedImage(stage, source_index),
                                            stage.images[source_index]};
    desc.view_info = image_binding.view_info;
    image_binding.image_id = texture_cache.FindImage(desc);
    image_binding.view_info = desc.view_info;
}

bool Rasterizer::IsCachedViewCurrent(const CachedImageView& cached_view,
                                     VideoCore::ImageId image_id, const VideoCore::Image& image,
                                     u64 topology_epoch,
                                     const VideoCore::ImageViewInfo& view_info) {
    return cached_view.valid && cached_view.image_id == image_id &&
           cached_view.image_uid == image.image_uid &&
           cached_view.topology_epoch == topology_epoch &&
           cached_view.backing_image == image.GetImage() && cached_view.info == view_info;
}

SHAD_NO_INLINE void Rasterizer::RefreshCachedView(CachedImageView& cached_view,
                                                  VideoCore::ImageId image_id,
                                                  VideoCore::Image& image, u64 topology_epoch,
                                                  const VideoCore::ImageViewInfo& view_info,
                                                  bool ensure_guest_samples) {
    auto& image_view = image.FindView(view_info, ensure_guest_samples);
    cached_view = {
        .image_id = image_id,
        .image_uid = image.image_uid,
        .topology_epoch = topology_epoch,
        .backing_image = image.GetImage(),
        .image_view = *image_view.image_view,
        .info = image_view.info,
        .valid = true,
    };
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    image_bindings.clear();
    const u32 first_image_idx = image_infos.size();
    const u32 stage_index = static_cast<u32>(stage.l_stage);
    if (stage_index >= MaxShaderStages) [[unlikely]] {
        ValidateTextureBindingIndex(stage_index, MaxShaderStages);
    }
    // One entry per stage image, and a stage has at most NUM_IMAGES of them.
    std::array<u32, Shader::NUM_IMAGES> image_descriptor_array_sizes;
    u32 num_image_descriptors = 0;

    for (u32 image_index = 0; image_index < stage.images.size(); ++image_index) {
        const auto& image_desc = stage.images[image_index];
        const auto tsharp = GetResolvedImage(stage, image_index);
        const u8 geometry_key = static_cast<u8>(image_desc.is_depth) |
                                (static_cast<u8>(image_desc.is_array) << 1) |
                                (static_cast<u8>(image_desc.is_written) << 2);
        const u64 resource_key = static_cast<u64>(geometry_key) |
                                 (static_cast<u64>(image_desc.mip_fallback_mode) << 3) |
                                 (static_cast<u64>(image_desc.constant_mip_index) << 8);
#ifdef _DEBUG
        if (texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }
#endif

        const auto data_fmt = tsharp.GetDataFmt();
        bool bind_null = tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid;
        if (!bind_null && (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
                           tsharp.pitch < tsharp.width || !magic_enum::enum_contains(data_fmt) ||
                           !magic_enum::enum_contains(tsharp.GetNumberFmt()))) [[unlikely]] {
            LogRejectedImage(tsharp);
            bind_null = true;
        }
        if (bind_null) {
            const u32 cache_index = image_bindings.size();
            image_bindings.emplace_back(ImageBindingInfo{
                .source_index = static_cast<u8>(image_index),
                .is_storage = image_desc.is_written,
            });
            if (cache_index < Shader::NUM_IMAGES) {
                auto& cached = cached_image_bindings[stage_index][cache_index];
                cached.valid = false;
                cached.owner = &stage;
            }
            image_descriptor_array_sizes[num_image_descriptors++] = 1;
            continue;
        }

        const u32 num_bindings = image_desc.NumBindings(tsharp);
        bool miss_desc_ready = false;

        for (u32 i = 0; i < num_bindings; ++i) {
            const u32 cache_index = image_bindings.size();
            if (cache_index >= Shader::NUM_IMAGES) [[unlikely]] {
                ValidateTextureBindingIndex(cache_index, Shader::NUM_IMAGES);
            }

            auto& cached = cached_image_bindings[stage_index][cache_index];
            if (cached.owner != &stage) {
                cached = {};
                cached.owner = &stage;
            }

            const bool cache_hit = cached.valid && cached.resource_key == resource_key &&
                                   cached.source_index == image_index && cached.mip_index == i &&
                                   std::memcmp(&cached.sharp, &tsharp, sizeof(tsharp)) == 0 &&
                                   cached.topology_epoch == texture_cache.TopologyEpoch() &&
                                   texture_cache.TryReuseImage(cached.image_id, cached.image_uid,
                                                               cached.topology_epoch);
            if (cache_hit) [[likely]] {
                image_bindings.emplace_back(ImageBindingInfo{
                    .image_id = cached.image_id,
                    .view_info = cached.view_info,
                    .source_index = static_cast<u8>(image_index),
                    .mip_index = static_cast<u8>(i),
                    .is_storage = image_desc.is_written,
                });
            } else {
                BindTextureMiss(stage, stage_index, image_index, i, num_bindings, tsharp,
                                geometry_key, resource_key, cached, miss_desc_ready);
            }

            auto& image_binding = image_bindings.back();
            auto* image = &texture_cache.GetImage(image_binding.image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                image_binding.image_id = depth_image_id;
                image = &texture_cache.GetImage(depth_image_id);
            }
            if (image->binding.is_bound) {
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes[num_image_descriptors++] = num_bindings;
    }

    u32 texture_binding_index = 0;
    for (auto& image_binding : image_bindings) {
        auto& image_id = image_binding.image_id;
        auto& view_info = image_binding.view_info;
        auto& cached_view = cached_texture_views[stage_index][texture_binding_index++];
        const bool is_storage = image_binding.is_storage;
        if (!image_id) {
            cached_view.valid = false;
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id); old_image.binding.needs_rebind)
                [[unlikely]] {
                old_image.binding = {};
                RebindTexture(stage, image_binding);
                cached_view.valid = false;
            }

            bound_images.emplace_back(image_id);
            if (is_storage &&
                std::ranges::find(potential_write_images, image_id) == potential_write_images.end()) {
                potential_write_images.emplace_back(image_id);
            }

            auto& image = texture_cache.GetImage(image_id);
            texture_cache.PrepareTexture(
                image_id, is_storage ? VideoCore::TextureCache::BindingType::Storage
                                     : VideoCore::TextureCache::BindingType::Texture);
            const u64 topology_epoch = texture_cache.TopologyEpoch();
            if (!IsCachedViewCurrent(cached_view, image_id, image, topology_epoch, view_info))
                [[unlikely]] {
                RefreshCachedView(cached_view, image_id, image, topology_epoch, view_info, true);
            }
            const vk::ImageView image_view_handle = cached_view.image_view;

            if ((image.binding.force_general || image.binding.is_target) &&
                !image.info.props.is_depth) {
                image.Transit(instance.IsAttachmentFeedbackLoopLayoutSupported() &&
                                      image.binding.is_target
                                  ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                  : vk::ImageLayout::eGeneral,
                              vk::AccessFlagBits2::eShaderRead |
                                  (image.info.props.is_depth
                                       ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                                       : vk::AccessFlagBits2::eColorAttachmentWrite |
                                             vk::AccessFlagBits2::eColorAttachmentRead),
                              {});
            } else if (is_storage) {
                image.Transit(vk::ImageLayout::eGeneral,
                              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                              view_info.range);
            } else {
                const auto new_layout = image.info.props.is_depth
                                            ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                            : vk::ImageLayout::eShaderReadOnlyOptimal;
                image.Transit(new_layout, vk::AccessFlagBits2::eShaderRead, view_info.range);
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            image_infos.emplace_back(VK_NULL_HANDLE, image_view_handle,
                                     image.backing->state.layout);
        }
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 descriptor = 0; descriptor < num_image_descriptors; ++descriptor) {
        const u32 array_size = image_descriptor_array_sizes[descriptor];
        const bool is_storage = image_bindings[image_binding_idx].is_storage;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    for (u32 sampler_index = 0; sampler_index < stage.samplers.size(); ++sampler_index) {
        const auto& sampler = stage.samplers[sampler_index];
        auto ssharp = GetResolvedSampler(stage, sampler_index);
        if (sampler.disable_aniso) {
            const auto tsharp = GetResolvedImage(stage, sampler.associated_image);
            if (tsharp.base_level == 0 && tsharp.last_level == 0) {
                ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        }
        const auto vk_sampler = texture_cache.GetSampler(ssharp, liverpool->regs.ta_bc_base);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

SHAD_NO_INLINE VideoCore::ImageId Rasterizer::RebindColorTarget(u32 cb) {
    auto& [image_id, desc] = cb_descs[cb];
    image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
    const auto& image = texture_cache.GetImage(image_id);
    auto& cached = cached_color_targets[cb];
    cached.image_id = image_id;
    cached.image_uid = image.image_uid;
    cached.topology_epoch = texture_cache.TopologyEpoch();
    cached_color_target_views[cb].valid = false;
    return image_id;
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    const auto& regs = liverpool->regs;
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state{};
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) [[unlikely]] {
            image_id = RebindColorTarget(cb);
            image = &texture_cache.GetImage(image_id);
        }
        // PrepareRenderTarget brings the image up to date below. The update here only matters
        // when SetBackingSamples copies the current backing into one with another sample count.
        if (image->backing && image->backing->num_samples != key.color_samples[cb]) {
            texture_cache.UpdateImage(image_id);
            image->SetBackingSamples(key.color_samples[cb]);
        }
        texture_cache.PrepareRenderTarget(image_id, desc);
        auto& cached_view = cached_color_target_views[cb];
        const u64 topology_epoch = texture_cache.TopologyEpoch();
        if (!IsCachedViewCurrent(cached_view, image_id, *image, topology_epoch, desc.view_info))
            [[unlikely]] {
            RefreshCachedView(cached_view, image_id, *image, topology_epoch, desc.view_info, false);
        }
        const auto slice = cached_view.info.range.base.layer;
        const auto mip = cached_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.TakeMetaCleared(col_buf.CmaskAddress(), slice);

        if (image->binding.is_bound) {
            if (image->binding.force_general) [[unlikely]] {
                ValidateFeedbackLoopBinding(image->binding.force_general);
            }
            image->Transit(instance.IsAttachmentFeedbackLoopLayoutSupported()
                               ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                               : vk::ImageLayout::eGeneral,
                           vk::AccessFlagBits2::eColorAttachmentWrite, {});
            attachment_feedback_loop = true;
        } else {
            image->Transit(vk::ImageLayout::eColorAttachmentOptimal,
                           vk::AccessFlagBits2::eColorAttachmentWrite |
                               vk::AccessFlagBits2::eColorAttachmentRead,
                           desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, cached_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = cached_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        image->usage.render_target = 1u;
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        texture_cache.PrepareDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);
        const u64 topology_epoch = texture_cache.TopologyEpoch();
        if (!IsCachedViewCurrent(cached_depth_target_view, image_id, image, topology_epoch,
                                 desc.view_info)) [[unlikely]] {
            RefreshCachedView(cached_depth_target_view, image_id, image, topology_epoch,
                              desc.view_info, false);
        }

        const auto slice = cached_depth_target_view.info.range.base.layer;
        const bool htile_cleared = texture_cache.TakeMetaCleared(htile_address, slice);
        const bool is_depth_clear =
            (regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
             regs.depth_control.depth_write_enable) ||
            htile_cleared;
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        if (desc.view_info.range.extent.levels != 1 || image.binding.needs_rebind) [[unlikely]] {
            ValidateDepthTargetView(desc.view_info.range.extent.levels,
                                    image.binding.needs_rebind);
        }

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto write_layout = has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                              : vk::ImageLayout::eDepthAttachmentOptimal;
        // A draw that writes less than the layout allows keeps it: the read-only layouts are
        // only needed while the image is also sampled, and texture binding moves it there first.
        // Leaving the writable layout would break the rendering scope now and again at the
        // next draw that writes.
        const bool keep_write_layout = image.backing->subresource_states.empty() &&
                                       image.backing->state.layout == write_layout;
        const auto new_layout = desc.view_info.is_storage || keep_write_layout ? write_layout
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        image.Transit(new_layout,
                      vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                          vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                      desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers =
            std::min<u32>(state.num_layers, cached_depth_target_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = cached_depth_target_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->last_cb_extent[0];
    const auto& mrt1_hint = liverpool->last_cb_extent[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->regs.color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->regs.color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->regs.color_buffers[0].Address(),
                                 liverpool->regs.color_buffers[1].Address()));
    mrt1_image.Resolve(mrt0_image, mrt0_desc.view_info.range, mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->regs;

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->regs.depth_view.slice_start;
    sub_range.extent.layers = liverpool->regs.depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    read_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                       sub_range);
    write_image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                        sub_range);

    auto aspect_mask = vk::ImageAspectFlags(0);
    if (is_depth) {
        aspect_mask |= vk::ImageAspectFlagBits::eDepth;
    }
    if (is_stencil) {
        aspect_mask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageCopy region = {
        .srcSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .srcOffset = {0, 0, 0},
        .dstSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .dstOffset = {0, 0, 0},
        .extent = {write_image.info.size.width, write_image.info.size.height, 1},
    };
    scheduler.CommandBuffer().copyImage(read_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                        write_image.GetImage(),
                                        vk::ImageLayout::eTransferDstOptimal, region);
    write_image.MarkWrite();

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    buffer_cache.FillBuffer(address, num_bytes, value, is_gds);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    buffer_cache.CopyBuffer(dst, src, num_bytes, dst_gds, src_gds);
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size);
    buffer_cache.InvalidateTransientReuse(addr, size);
    texture_cache.InvalidateMemory(addr, size);
    page_manager.NotifyWrite(addr, size, VideoCore::MemoryWriteSource::Cpu);
    return true;
}

void Rasterizer::OnCpuWriteFault(VAddr addr) {
    buffer_cache.OnCpuWriteFault(addr);
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, void* context) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size);

    if (page_manager.HasReadWatcher(addr)) {
        DisarmSemanticReadWatch(addr, size);
    }
    return true;
}

void Rasterizer::ArmSemanticReadWatch(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        return;
    }
    page_manager.UpdatePageWatchers<true, true>(addr, size);
}

void Rasterizer::DisarmSemanticReadWatch(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        return;
    }
    page_manager.UpdatePageWatchers<false, true>(addr, size);
}

bool Rasterizer::HandleWriteFaultOnReadWatchedPage(VAddr addr, u64 size, void* context) {
    if (!page_manager.HasReadWatcher(addr)) {
        return false;
    }
    if (page_manager.HasReadWatcher(addr)) {
        page_manager.TemporarilyUnprotect(addr, size);
        Core::Signals::Instance()->RequestSingleStepRearm(context, addr & ~0xFFFULL, 4096);
    }
    return true;
}

VideoCore::MemoryWriteNotifyResult Rasterizer::NotifyMemoryWrite(
    VAddr addr, u64 size, VideoCore::MemoryWriteSource source) {
    buffer_cache.InvalidateTransientReuse(addr, size);
    return page_manager.NotifyWrite(addr, size, source);
}

bool Rasterizer::ProcessDownloadImages(const VideoCore::TextureCache::DownloadContext& context,
                                       bool* gpu_resident) {
    return texture_cache.ProcessDownloadImages(context, gpu_resident);
}

void Rasterizer::WaitTick(u64 tick) {
    scheduler.Wait(tick);
}

void Rasterizer::DeferGpuCompletion(Common::UniqueFunction<void>&& callback) {
    DeferGpuCompletionAt(scheduler.CurrentTick(), std::move(callback));
}

void Rasterizer::DeferGpuCompletionAt(u64 gpu_tick, Common::UniqueFunction<void>&& callback) {
    scheduler.DeferPriorityOperationAt(gpu_tick, std::move(callback));
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    // Workers must not read a range after its guest mapping is torn down.
    VideoCore::GuestCopyEngine::Instance().WaitForGuestWrite(addr, size);
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    page_manager.OnGpuUnmap(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    const u64 generation = liverpool->GraphicsStateGeneration();
    const bool registers_changed = dynamic_state_generation != generation;
    const bool indexed_changed = dynamic_state_indexed != is_indexed;
    const bool feedback_changed = dynamic_state_feedback_loop != attachment_feedback_loop;
    auto& inputs = *dynamic_state_inputs;
    const bool first_draw = !inputs.valid;
    bool primitive_changed = first_draw || indexed_changed;
    const auto& regs = liverpool->regs;
    if (registers_changed || first_draw) {
        // Every group is refreshed, even on the first draw, so the cache ends up current.
        const bool viewport_changed = ViewportInputs(regs, RefreshInputs(inputs.viewport));
        if (first_draw || viewport_changed) {
            UpdateViewportScissorState();
        }

        const bool depth_stencil_changed =
            DepthStencilInputs(regs, RefreshInputs(inputs.depth_stencil));
        if (first_draw || depth_stencil_changed) {
            UpdateDepthStencilState();
        }

        primitive_changed |= PrimitiveInputs(regs, RefreshInputs(inputs.primitive));

        const bool rasterization_changed =
            RasterizationInputs(regs, RefreshInputs(inputs.rasterization));
        if (first_draw || rasterization_changed) {
            UpdateRasterizationState();
        }
        inputs.valid = true;
    }
    if (primitive_changed) {
        UpdatePrimitiveState(is_indexed);
    }

    const bool blend_changed = BlendInputs(regs, *pipeline, RefreshInputs(inputs.blend));
    if (first_draw || blend_changed || feedback_changed) {
        UpdateColorBlendingState(pipeline);
    }
    dynamic_state_generation = generation;
    dynamic_state_pipeline = pipeline;
    dynamic_state_indexed = is_indexed;
    dynamic_state_feedback_loop = attachment_feedback_loop;

    auto& dynamic_state = scheduler.GetDynamicState();
    if (dynamic_state.dirty_bits == 0) {
        return;
    }
    dynamic_state.Commit(instance, scheduler);
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->regs;

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max(scr, std::max(s16(win + win_offset), s16(gen + win_offset)));
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min(scr, std::min(s16(win + win_offset), s16(gen + win_offset)));
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        ReportUnsupportedWindowOffset();
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    const u32 active_viewports = ActiveViewportMask(regs);
    const u32 first = std::countr_zero(active_viewports);

    if (active_viewports == 0) {
        constexpr vk::Viewport empty_viewport{
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor{
            .offset = {0, 0},
            .extent = {1, 1},
        };
        dynamic_state.SetSingleViewportScissor(empty_viewport, empty_scissor);
        return;
    }

    if (!std::has_single_bit(active_viewports)) [[unlikely]] {
        SetMultipleViewportScissorState(instance, regs, scsr, dynamic_state);
        return;
    }

    dynamic_state.SetSingleViewportScissor(MakeViewport(instance, regs, first),
                                           MakeViewportScissor(regs, scsr, first));
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_stencil = GetEffectiveDepthStencilState(regs);
    const bool depth_test_enabled = depth_stencil.depth_test_enable;
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    dynamic_state.SetDepthWriteEnabled(depth_stencil.depth_write_enable &&
                                       !regs.depth_render_control.depth_clear_enable);
    if (depth_test_enabled) {
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const bool depth_bounds_test_enabled = depth_stencil.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const bool stencil_test_enabled = depth_stencil.stencil_test_enable;
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

} // namespace Vulkan
