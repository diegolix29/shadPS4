// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    vk::Pipeline Handle() const noexcept {
        return *pipeline;
    }

    vk::PipelineLayout GetLayout() const noexcept {
        return *pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::LogicalStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::LogicalStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

    // FIX(GR2FORK): the Rasterizer delta-descriptor-write filter needs to
    // distinguish push-descriptor pipelines (stateful, partial writes retain
    // prior values) from descriptor-set pipelines (each commit allocates a
    // fresh set that starts empty). Without this distinction, the filter
    // skips writes that the set-path still needs, producing
    // VUID-vkCmdDrawIndexed-None-08114 and RADV DEVICE_LOST.
    bool UsesPushDescriptors() const noexcept {
        return uses_push_descriptors;
    }

    // PERF: Inline capacities cover the common case. Previous values of 512/384
    // embedded ~56KB in the Rasterizer object, pushing hot member variables out of L1/L2 cache.
    // 128/64 covers ~95% of draws without heap allocation while keeping the object compact.
    using DescriptorWrites = boost::container::small_vector<vk::WriteDescriptorSet, 128>;
    using BufferBarriers = boost::container::small_vector<vk::BufferMemoryBarrier2, 64>;

    void BindResources(DescriptorWrites& set_writes, BufferBarriers& buffer_barriers,
                       const Shader::PushData& push_data) const;

    /// Resolve resources into a DrawBundle for threaded recording.
    /// Does all dedup/caching but does NOT touch cmdbuf.
    void ResolveResources(DescriptorWrites& set_writes, BufferBarriers& buffer_barriers,
                          const Shader::PushData& push_data,
                          struct DrawBundle& bundle) const;

protected:
    [[nodiscard]] std::string GetDebugString() const;

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    bool is_compute;

    // Fast path: many draws re-bind identical push-descriptor writes within the same command buffer.
    // Cache the last signature to avoid expensive vkCmdPushDescriptorSetKHR calls when nothing changed.
    // Push-constant skip cache (per command buffer)
    mutable vk::CommandBuffer cached_push_constants_cmdbuf{};
    mutable u64 cached_push_constants_tick{};
    mutable vk::PipelineLayout cached_push_constants_layout{};
    mutable vk::ShaderStageFlags cached_push_constants_stages{};
    mutable u64 cached_push_constants_sig{};
    mutable bool cached_push_constants_valid{};

    // Push-descriptor skip cache (per command buffer).
    // Avoid redundant vkCmdPushDescriptorSetKHR when the descriptor writes are identical.
    mutable vk::CommandBuffer cached_push_desc_cmdbuf{};
    mutable u64 cached_push_desc_tick{};
    mutable vk::PipelineLayout cached_push_desc_layout{};
    mutable vk::PipelineBindPoint cached_push_desc_bind_point{};
    mutable u64 cached_push_desc_sig{};
    mutable bool cached_push_desc_valid{};
    mutable bool cached_push_desc_is_delta{};

    // Descriptor-set cache (non-push path): cache descriptor sets by signature within the same command buffer.
    // This turns many vkUpdateDescriptorSets calls into simple vkCmdBindDescriptorSets.
    struct CachedDescSetEntry {
        u64 sig{};
        vk::DescriptorSet set{};
    };
    static constexpr u32 DescSetCacheSize = 128;

    mutable vk::CommandBuffer cached_descset_cmdbuf{};
    mutable u64 cached_descset_tick{};
    mutable u32 cached_descset_count{};
    mutable std::array<CachedDescSetEntry, DescSetCacheSize> cached_descsets{};

    // Skip redundant binds when the exact same descriptor set is already bound.
    mutable vk::CommandBuffer cached_bound_desc_cmdbuf{};
    mutable u64 cached_bound_desc_tick{};
    mutable vk::PipelineLayout cached_bound_desc_layout{};
    mutable vk::PipelineBindPoint cached_bound_desc_bind_point{};
    mutable vk::DescriptorSet cached_bound_desc_set{};
    mutable bool cached_bound_desc_valid{};

    // Push-constants cache (skip redundant vkCmdPushConstants).
    mutable vk::PipelineLayout cached_push_const_layout{};
    mutable vk::ShaderStageFlags cached_push_const_stages{};
    mutable Shader::PushData cached_push_const_data{};
    mutable vk::CommandBuffer cached_push_const_cmdbuf{};
    mutable bool cached_push_const_valid{};
};

} // namespace Vulkan
