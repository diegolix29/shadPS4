// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <type_traits>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_record_audit.h"

namespace Vulkan::RecordAudit {

namespace {

using Dispatcher = std::remove_cvref_t<decltype(VULKAN_HPP_DEFAULT_DISPATCHER)>;

constexpr size_t MaxClaims = 256;

struct Claim {
    std::atomic<u64> handle{};
    std::atomic<u64> owner{};
};

std::array<Claim, MaxClaims> claims;
std::atomic<u32> num_claims{};
std::mutex claim_mutex;

std::atomic<u64> next_thread_token{};
std::atomic<u64> owner_calls{};
std::atomic<u64> foreign_calls{};
std::atomic<u64> unclaimed_calls{};
std::atomic<const char*> first_foreign{};
std::atomic<bool> installed{};

constexpr size_t MaxProducers = 16;
struct Producer {
    std::atomic<u64> token{};
    std::atomic<u64> commands{};
};
std::array<Producer, MaxProducers> producers;
std::atomic<u32> num_producers{};
std::mutex producer_mutex;

u64 ThreadToken() noexcept {
    thread_local const u64 token = next_thread_token.fetch_add(1, std::memory_order_relaxed) + 1;
    return token;
}

void NoteCall(VkCommandBuffer cmdbuf, const char* name) noexcept {
    const u64 handle = reinterpret_cast<u64>(cmdbuf);
    const u32 count = num_claims.load(std::memory_order_acquire);
    for (u32 index = 0; index < count; ++index) {
        const Claim& claim = claims[index];
        if (claim.handle.load(std::memory_order_relaxed) != handle) {
            continue;
        }
        if (claim.owner.load(std::memory_order_relaxed) == ThreadToken()) {
            owner_calls.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (foreign_calls.fetch_add(1, std::memory_order_relaxed) == 0) {
            first_foreign.store(name, std::memory_order_relaxed);
            LOG_ERROR(Render_Vulkan,
                      "Record audit: {} called on a command buffer owned by the "
                      "recording thread from another thread",
                      name);
        }
        return;
    }
    unclaimed_calls.fetch_add(1, std::memory_order_relaxed);
}

template <auto Member, typename Pfn>
struct Hook;

template <auto Member, typename R, typename... Args>
struct Hook<Member, R(VKAPI_PTR*)(Args...)> {
    static inline R(VKAPI_PTR* original)(Args...) = nullptr;
    static inline const char* name = nullptr;

    static R VKAPI_PTR Forward(Args... args) {
        NoteCall(CommandBufferOf(args...), name);
        return original(args...);
    }

    // Every audited entry point takes the command buffer first.
    template <typename... Rest>
    static VkCommandBuffer CommandBufferOf(VkCommandBuffer cmdbuf, Rest...) noexcept {
        return cmdbuf;
    }

    static void Install(Dispatcher& dispatcher, const char* name_) {
        auto& slot = dispatcher.*Member;
        if (slot == nullptr || slot == &Forward) {
            return;
        }
        name = name_;
        original = slot;
        slot = &Forward;
    }
};

#define AUDIT_HOOK(fn) Hook<&Dispatcher::fn, decltype(Dispatcher::fn)>::Install(dispatcher, #fn)

void InstallHooks(Dispatcher& dispatcher) {
    AUDIT_HOOK(vkBeginCommandBuffer);
    AUDIT_HOOK(vkEndCommandBuffer);
    AUDIT_HOOK(vkResetCommandBuffer);
    AUDIT_HOOK(vkCmdPipelineBarrier);
    AUDIT_HOOK(vkCmdPipelineBarrier2);
    AUDIT_HOOK(vkCmdPipelineBarrier2KHR);
    AUDIT_HOOK(vkCmdBindPipeline);
    AUDIT_HOOK(vkCmdBindDescriptorSets);
    AUDIT_HOOK(vkCmdPushDescriptorSetKHR);
    AUDIT_HOOK(vkCmdPushDescriptorSet);
    AUDIT_HOOK(vkCmdPushConstants);
    AUDIT_HOOK(vkCmdBindVertexBuffers);
    AUDIT_HOOK(vkCmdBindVertexBuffers2);
    AUDIT_HOOK(vkCmdBindVertexBuffers2EXT);
    AUDIT_HOOK(vkCmdBindIndexBuffer);
    AUDIT_HOOK(vkCmdSetVertexInputEXT);
    AUDIT_HOOK(vkCmdSetViewport);
    AUDIT_HOOK(vkCmdSetScissor);
    AUDIT_HOOK(vkCmdSetViewportWithCount);
    AUDIT_HOOK(vkCmdSetViewportWithCountEXT);
    AUDIT_HOOK(vkCmdSetScissorWithCount);
    AUDIT_HOOK(vkCmdSetScissorWithCountEXT);
    AUDIT_HOOK(vkCmdSetDepthTestEnable);
    AUDIT_HOOK(vkCmdSetDepthWriteEnable);
    AUDIT_HOOK(vkCmdSetDepthCompareOp);
    AUDIT_HOOK(vkCmdSetDepthBoundsTestEnable);
    AUDIT_HOOK(vkCmdSetDepthBounds);
    AUDIT_HOOK(vkCmdSetDepthBiasEnable);
    AUDIT_HOOK(vkCmdSetDepthBias);
    AUDIT_HOOK(vkCmdSetStencilTestEnable);
    AUDIT_HOOK(vkCmdSetStencilOp);
    AUDIT_HOOK(vkCmdSetStencilReference);
    AUDIT_HOOK(vkCmdSetStencilWriteMask);
    AUDIT_HOOK(vkCmdSetStencilCompareMask);
    AUDIT_HOOK(vkCmdSetPrimitiveRestartEnable);
    AUDIT_HOOK(vkCmdSetRasterizerDiscardEnable);
    AUDIT_HOOK(vkCmdSetCullMode);
    AUDIT_HOOK(vkCmdSetFrontFace);
    AUDIT_HOOK(vkCmdSetBlendConstants);
    AUDIT_HOOK(vkCmdSetColorWriteMaskEXT);
    AUDIT_HOOK(vkCmdSetLineWidth);
    AUDIT_HOOK(vkCmdSetAttachmentFeedbackLoopEnableEXT);
    AUDIT_HOOK(vkCmdBeginRendering);
    AUDIT_HOOK(vkCmdBeginRenderingKHR);
    AUDIT_HOOK(vkCmdEndRendering);
    AUDIT_HOOK(vkCmdEndRenderingKHR);
    AUDIT_HOOK(vkCmdDraw);
    AUDIT_HOOK(vkCmdDrawIndexed);
    AUDIT_HOOK(vkCmdDrawIndirect);
    AUDIT_HOOK(vkCmdDrawIndexedIndirect);
    AUDIT_HOOK(vkCmdDrawIndirectCount);
    AUDIT_HOOK(vkCmdDrawIndexedIndirectCount);
    AUDIT_HOOK(vkCmdDispatch);
    AUDIT_HOOK(vkCmdDispatchIndirect);
    AUDIT_HOOK(vkCmdCopyBuffer);
    AUDIT_HOOK(vkCmdCopyImage);
    AUDIT_HOOK(vkCmdCopyBufferToImage);
    AUDIT_HOOK(vkCmdCopyImageToBuffer);
    AUDIT_HOOK(vkCmdResolveImage);
    AUDIT_HOOK(vkCmdFillBuffer);
    AUDIT_HOOK(vkCmdClearColorImage);
    AUDIT_HOOK(vkCmdUpdateBuffer);
    AUDIT_HOOK(vkCmdBlitImage);
    AUDIT_HOOK(vkCmdClearAttachments);
    AUDIT_HOOK(vkCmdBeginDebugUtilsLabelEXT);
    AUDIT_HOOK(vkCmdInsertDebugUtilsLabelEXT);
    AUDIT_HOOK(vkCmdEndDebugUtilsLabelEXT);
    AUDIT_HOOK(vkCmdResetQueryPool);
    AUDIT_HOOK(vkCmdWriteTimestamp);
    AUDIT_HOOK(vkCmdWriteTimestamp2);
    AUDIT_HOOK(vkCmdBeginQuery);
    AUDIT_HOOK(vkCmdEndQuery);
    AUDIT_HOOK(vkCmdBeginConditionalRenderingEXT);
    AUDIT_HOOK(vkCmdEndConditionalRenderingEXT);
    AUDIT_HOOK(vkCmdSetEvent2);
    AUDIT_HOOK(vkCmdCopyBuffer2);
    AUDIT_HOOK(vkCmdCopyImage2);
}

#undef AUDIT_HOOK

} // Anonymous namespace

std::atomic<bool> watch_producers{};

void NoteProducer() noexcept {
    thread_local Producer* self = nullptr;
    if (self == nullptr) [[unlikely]] {
        std::scoped_lock lock{producer_mutex};
        const u32 count = num_producers.load(std::memory_order_relaxed);
        if (count == MaxProducers) {
            return;
        }
        self = &producers[count];
        self->token.store(ThreadToken(), std::memory_order_relaxed);
        num_producers.store(count + 1, std::memory_order_release);
        LOG_INFO(Render_Vulkan, "Record audit: thread {} records commands", ThreadToken());
    }
    self->commands.fetch_add(1, std::memory_order_relaxed);
}

bool RequestedByEnvironment() {
    const char* env = std::getenv("SHADPS4_VK_RECORD_AUDIT");
    return env != nullptr && env[0] == '1';
}

void Install() {
    std::scoped_lock lock{claim_mutex};
    if (installed.load(std::memory_order_relaxed)) {
        return;
    }
    InstallHooks(VULKAN_HPP_DEFAULT_DISPATCHER);
    installed.store(true, std::memory_order_release);
    watch_producers.store(true, std::memory_order_release);
    LOG_INFO(Render_Vulkan, "Record audit installed: every command buffer call checks its thread");
}

bool IsInstalled() noexcept {
    return installed.load(std::memory_order_acquire);
}

void ClaimCommandBuffer(vk::CommandBuffer cmdbuf) {
    const u64 handle = reinterpret_cast<u64>(static_cast<VkCommandBuffer>(cmdbuf));
    std::scoped_lock lock{claim_mutex};
    const u32 count = num_claims.load(std::memory_order_relaxed);
    for (u32 index = 0; index < count; ++index) {
        if (claims[index].handle.load(std::memory_order_relaxed) == handle) {
            claims[index].owner.store(ThreadToken(), std::memory_order_relaxed);
            return;
        }
    }
    ASSERT_MSG(count < MaxClaims, "Record audit tracks too many command buffers");
    claims[count].owner.store(ThreadToken(), std::memory_order_relaxed);
    claims[count].handle.store(handle, std::memory_order_relaxed);
    num_claims.store(count + 1, std::memory_order_release);
}

Stats GetStats() {
    const char* first = first_foreign.load(std::memory_order_relaxed);
    Stats stats{
        .owner_calls = owner_calls.load(std::memory_order_relaxed),
        .foreign_calls = foreign_calls.load(std::memory_order_relaxed),
        .unclaimed_calls = unclaimed_calls.load(std::memory_order_relaxed),
        .first_foreign = first != nullptr ? first : "",
    };
    const u32 count = num_producers.load(std::memory_order_acquire);
    for (u32 index = 0; index < count; ++index) {
        stats.producers.emplace_back(producers[index].token.load(std::memory_order_relaxed),
                                     producers[index].commands.load(std::memory_order_relaxed));
    }
    return stats;
}

void LogStats(std::string_view context) {
    const Stats stats = GetStats();
    std::string producer_list;
    for (const auto& [token, commands] : stats.producers) {
        producer_list +=
            fmt::format("{}thread {}: {}", producer_list.empty() ? "" : ", ", token, commands);
    }
    LOG_INFO(Render_Vulkan, "Record audit ({}): commands recorded by {}", context,
             producer_list.empty() ? std::string{"nobody"} : producer_list);
    if (stats.foreign_calls != 0) {
        LOG_ERROR(Render_Vulkan,
                  "Record audit ({}): {} calls on the recording thread, {} FROM OTHER THREADS "
                  "(first: {}), {} on unclaimed command buffers",
                  context, stats.owner_calls, stats.foreign_calls, stats.first_foreign,
                  stats.unclaimed_calls);
        return;
    }
    LOG_INFO(Render_Vulkan,
             "Record audit ({}): {} calls on the recording thread, 0 from other threads, {} on "
             "unclaimed command buffers",
             context, stats.owner_calls, stats.unclaimed_calls);
}

} // namespace Vulkan::RecordAudit
