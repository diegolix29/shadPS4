// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include "common/assert.h"
#include "common/types.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

// =========================================================================
// DrawBundle: self-contained package of pre-resolved Vulkan recording data.
//
// Created by the GpuComm thread (parser/resolver) after all resource
// resolution, descriptor binding, and state computation is done.
// Consumed by the Recorder thread which issues the actual vkCmd* calls.
//
// All Vulkan handles (vk::Buffer, vk::ImageView, vk::Pipeline, vk::Sampler)
// are plain u64 values that remain valid as long as the owning cache objects
// live. Cache eviction does scheduler.Finish() which drains the recorder
// queue, so handle lifetime is guaranteed.
//
// Descriptor write pointers are fixed up to point into bundle-owned storage.
// =========================================================================

struct DrawBundle {
    // --- Draw type ---
    enum class Type : u8 {
        Draw,
        DrawIndexed,
        DrawIndirect,
        DrawIndexedIndirect,
        EndRendering,  // Flush current render pass (for barriers/sync)
    };
    Type type{Type::Draw};

    // --- Pipeline ---
    vk::Pipeline pipeline{};
    vk::PipelineLayout pipeline_layout{};
    vk::PipelineBindPoint bind_point{vk::PipelineBindPoint::eGraphics};

    // --- Render state (for BeginRendering) ---
    RenderState render_state{};

    // --- Dynamic state snapshot ---
    DynamicState dynamic_state{};

    // --- Push constants ---
    Shader::PushData push_data{};
    vk::ShaderStageFlags push_stage_flags{};

    // --- Descriptor writes ---
    // GR2 pipelines with VS+FS+GS can produce 200+ descriptor writes on first bind.
    // Buffer infos: one per buffer descriptor (UBO, SSBO, etc.)
    // Image infos: one per texture/sampler/storage image descriptor
    static constexpr u32 MaxBufferInfos = 96;
    static constexpr u32 MaxImageInfos = 192;
    static constexpr u32 MaxDescWrites = 256;

    u32 num_buffer_infos{};
    u32 num_image_infos{};
    u32 num_desc_writes{};
    std::array<vk::DescriptorBufferInfo, MaxBufferInfos> buffer_infos{};
    std::array<vk::DescriptorImageInfo, MaxImageInfos> image_infos{};
    std::array<vk::WriteDescriptorSet, MaxDescWrites> desc_writes{};

    // --- Buffer barriers ---
    static constexpr u32 MaxBarriers = 32;
    u32 num_barriers{};
    std::array<vk::BufferMemoryBarrier2, MaxBarriers> barriers{};

    // --- Vertex buffers ---
    static constexpr u32 MaxVertexBuffers = 32;
    u32 num_vertex_buffers{};
    std::array<vk::Buffer, MaxVertexBuffers> vb_buffers{};
    std::array<vk::DeviceSize, MaxVertexBuffers> vb_offsets{};
    std::array<vk::DeviceSize, MaxVertexBuffers> vb_sizes{};
    std::array<vk::DeviceSize, MaxVertexBuffers> vb_strides{};

    // --- Index buffer ---
    vk::Buffer index_buffer{};
    vk::DeviceSize index_buffer_offset{};
    vk::IndexType index_type{vk::IndexType::eUint16};

    // --- Draw parameters ---
    u32 num_indices{};
    u32 num_instances{};
    s32 vertex_offset{};
    u32 instance_offset{};

    // --- Flags ---
    bool has_render_state{};
    bool has_dynamic_state{};
    bool has_pipeline_bind{};
    bool has_push_constants{};
    bool has_descriptors{};
    bool has_vertex_buffers{};
    bool has_index_bind{};
    bool has_desc_set_bind{};       // Non-push descriptor path: bindDescriptorSets
    bool end_rendering_before_barriers{};

    // Non-push descriptor path: pre-allocated descriptor set to bind.
    vk::DescriptorSet desc_set{};

    // =========================================================================
    // Methods
    // =========================================================================

    /// Copy descriptor writes from rasterizer vectors and fix up internal pointers.
    void CopyDescriptorWrites(
        const vk::WriteDescriptorSet* src_writes, u32 write_count,
        const vk::DescriptorBufferInfo* src_buf_infos, u32 buf_count,
        const vk::DescriptorImageInfo* src_img_infos, u32 img_count) {

        ASSERT_MSG(buf_count <= MaxBufferInfos,
            "DrawBundle overflow: {} buffer infos > {}", buf_count, MaxBufferInfos);
        ASSERT_MSG(img_count <= MaxImageInfos,
            "DrawBundle overflow: {} image infos > {}", img_count, MaxImageInfos);
        ASSERT_MSG(write_count <= MaxDescWrites,
            "DrawBundle overflow: {} desc writes > {}", write_count, MaxDescWrites);

        num_buffer_infos = buf_count;
        num_image_infos = img_count;
        num_desc_writes = write_count;

        if (num_buffer_infos > 0) {
            std::memcpy(buffer_infos.data(), src_buf_infos,
                        num_buffer_infos * sizeof(vk::DescriptorBufferInfo));
        }
        if (num_image_infos > 0) {
            std::memcpy(image_infos.data(), src_img_infos,
                        num_image_infos * sizeof(vk::DescriptorImageInfo));
        }
        if (num_desc_writes > 0) {
            std::memcpy(desc_writes.data(), src_writes,
                        num_desc_writes * sizeof(vk::WriteDescriptorSet));
            FixupDescriptorPointers(src_buf_infos, src_img_infos);
        }
        has_descriptors = (num_desc_writes > 0);
    }

    /// Copy buffer barriers from rasterizer vector.
    void CopyBarriers(const vk::BufferMemoryBarrier2* src, u32 count) {
        ASSERT_MSG(count <= MaxBarriers,
            "DrawBundle overflow: {} barriers > {}", count, MaxBarriers);
        num_barriers = count;
        if (num_barriers > 0) {
            std::memcpy(barriers.data(), src, num_barriers * sizeof(vk::BufferMemoryBarrier2));
        }
    }

    /// Copy vertex buffer bindings.
    void CopyVertexBuffers(const vk::Buffer* buffers, const vk::DeviceSize* offsets,
                           const vk::DeviceSize* sizes, const vk::DeviceSize* strides,
                           u32 count) {
        num_vertex_buffers = std::min(count, MaxVertexBuffers);
        has_vertex_buffers = (num_vertex_buffers > 0);
        if (num_vertex_buffers > 0) {
            std::memcpy(vb_buffers.data(), buffers, num_vertex_buffers * sizeof(vk::Buffer));
            std::memcpy(vb_offsets.data(), offsets, num_vertex_buffers * sizeof(vk::DeviceSize));
            std::memcpy(vb_sizes.data(), sizes, num_vertex_buffers * sizeof(vk::DeviceSize));
            std::memcpy(vb_strides.data(), strides, num_vertex_buffers * sizeof(vk::DeviceSize));
        }
    }

private:
    void FixupDescriptorPointers(const vk::DescriptorBufferInfo* src_buf_base,
                                  const vk::DescriptorImageInfo* src_img_base) {
        for (u32 i = 0; i < num_desc_writes; ++i) {
            auto& w = desc_writes[i];
            if (w.pBufferInfo && src_buf_base) {
                const auto offset = w.pBufferInfo - src_buf_base;
                if (offset >= 0 && static_cast<u32>(offset) < num_buffer_infos) {
                    w.pBufferInfo = &buffer_infos[offset];
                }
            }
            if (w.pImageInfo && src_img_base) {
                const auto offset = w.pImageInfo - src_img_base;
                if (offset >= 0 && static_cast<u32>(offset) < num_image_infos) {
                    w.pImageInfo = &image_infos[offset];
                }
            }
        }
    }
};

static_assert(sizeof(DrawBundle) < 32768, "DrawBundle exceeds expected size");

// =========================================================================
// DrawBundleRing: Lock-free SPSC ring buffer for DrawBundles.
//
// The parser thread writes directly into pre-allocated slots (zero-copy).
// The recorder thread reads and processes them in order.
//
// Usage:
//   Parser:  auto* b = ring.TryStartWrite();
//            if (b) { *b = ...; ring.FinishWrite(); }
//   Recorder: auto* b = ring.TryStartRead();
//             if (b) { Process(*b); ring.FinishRead(); }
// =========================================================================
class DrawBundleRing {
public:
    static constexpr u32 Capacity = 128;
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

    DrawBundleRing() : data_(std::make_unique<DrawBundle[]>(Capacity)) {}

    /// Get a slot to write into. Returns nullptr if ring is full.
    DrawBundle* TryStartWrite() {
        const u32 w = write_pos_.load(std::memory_order_relaxed);
        const u32 r = read_pos_.load(std::memory_order_acquire);
        if (w - r >= Capacity) {
            return nullptr; // Full
        }
        return &data_[w & (Capacity - 1)];
    }

    /// Publish the written bundle. Must follow a successful TryStartWrite.
    void FinishWrite() {
        write_pos_.fetch_add(1, std::memory_order_release);
    }

    /// Get the next bundle to read. Returns nullptr if ring is empty.
    DrawBundle* TryStartRead() {
        const u32 r = read_pos_.load(std::memory_order_relaxed);
        const u32 w = write_pos_.load(std::memory_order_acquire);
        if (r == w) {
            return nullptr; // Empty
        }
        return &data_[r & (Capacity - 1)];
    }

    /// Release the read slot. Must follow a successful TryStartRead.
    void FinishRead() {
        read_pos_.fetch_add(1, std::memory_order_release);
    }

    bool Empty() const {
        return read_pos_.load(std::memory_order_acquire) ==
               write_pos_.load(std::memory_order_acquire);
    }

    u32 Size() const {
        return write_pos_.load(std::memory_order_acquire) -
               read_pos_.load(std::memory_order_acquire);
    }

private:
    std::unique_ptr<DrawBundle[]> data_;
    alignas(64) std::atomic<u32> write_pos_{0};
    alignas(64) std::atomic<u32> read_pos_{0};
};

} // namespace Vulkan
