// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

// windows.h defines MemoryBarrier as a macro.
#pragma push_macro("MemoryBarrier")
#undef MemoryBarrier

namespace Vulkan {

class Scheduler;

/// Records commands into the current command buffer of a scheduler. Mirrors the part of
/// vk::CommandBuffer the renderer uses. Every argument is copied, so pointers passed in only
/// have to live for the call. With a recording thread the commands reach the driver later, on
/// that thread; handles passed in must stay alive until the GPU is done with the tick, as for
/// any recorded command.
class CommandRecorder {
public:
    explicit CommandRecorder(Scheduler& scheduler_) noexcept : scheduler{&scheduler_} {}

    void pipelineBarrier(vk::PipelineStageFlags src_stage_mask,
                         vk::PipelineStageFlags dst_stage_mask, vk::DependencyFlags flags,
                         vk::ArrayProxy<const vk::MemoryBarrier> const& memory_barriers,
                         vk::ArrayProxy<const vk::BufferMemoryBarrier> const& buffer_barriers,
                         vk::ArrayProxy<const vk::ImageMemoryBarrier> const& image_barriers) const;
    void pipelineBarrier2(const vk::DependencyInfo& info) const;

    void bindPipeline(vk::PipelineBindPoint bind_point, vk::Pipeline pipeline) const;
    void bindDescriptorSets(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout,
                            u32 first_set, vk::ArrayProxy<const vk::DescriptorSet> const& sets,
                            vk::ArrayProxy<const u32> const& dynamic_offsets) const;
    void pushDescriptorSetKHR(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
                              vk::ArrayProxy<const vk::WriteDescriptorSet> const& writes,
                              u32 num_descriptors = ~u32{0}) const;
    void pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, u32 offset, u32 size,
                       const void* values) const;
    void bindVertexBuffers(u32 first_binding, u32 binding_count, const vk::Buffer* buffers,
                           const vk::DeviceSize* offsets) const;
    void bindVertexBuffers2(u32 first_binding, u32 binding_count, const vk::Buffer* buffers,
                            const vk::DeviceSize* offsets, const vk::DeviceSize* sizes,
                            const vk::DeviceSize* strides) const;
    void bindIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::IndexType index_type) const;
    void setVertexInputEXT(
        vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> const& bindings,
        vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> const& attributes) const;

    void setViewport(u32 first_viewport, vk::ArrayProxy<const vk::Viewport> const& viewports) const;
    void setScissor(u32 first_scissor, vk::ArrayProxy<const vk::Rect2D> const& scissors) const;
    void setViewportWithCount(vk::ArrayProxy<const vk::Viewport> const& viewports) const;
    void setScissorWithCount(vk::ArrayProxy<const vk::Rect2D> const& scissors) const;

    void beginRendering(const vk::RenderingInfo& info) const;
    void endRendering() const;
    void draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) const;
    void drawIndexed(u32 index_count, u32 instance_count, u32 first_index, s32 vertex_offset,
                     u32 first_instance) const;
    void drawIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 draw_count, u32 stride) const;
    void drawIndexedIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 draw_count,
                             u32 stride) const;
    void drawIndirectCount(vk::Buffer buffer, vk::DeviceSize offset, vk::Buffer count_buffer,
                           vk::DeviceSize count_offset, u32 max_draw_count, u32 stride) const;
    void drawIndexedIndirectCount(vk::Buffer buffer, vk::DeviceSize offset, vk::Buffer count_buffer,
                                  vk::DeviceSize count_offset, u32 max_draw_count,
                                  u32 stride) const;
    void dispatch(u32 group_count_x, u32 group_count_y, u32 group_count_z) const;
    void dispatchIndirect(vk::Buffer buffer, vk::DeviceSize offset) const;

    void copyBuffer(vk::Buffer src, vk::Buffer dst,
                    vk::ArrayProxy<const vk::BufferCopy> const& regions) const;
    void copyImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                   vk::ImageLayout dst_layout,
                   vk::ArrayProxy<const vk::ImageCopy> const& regions) const;
    void copyBufferToImage(vk::Buffer src, vk::Image dst, vk::ImageLayout dst_layout,
                           vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const;
    void copyImageToBuffer(vk::Image src, vk::ImageLayout src_layout, vk::Buffer dst,
                           vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const;
    void resolveImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                      vk::ImageLayout dst_layout,
                      vk::ArrayProxy<const vk::ImageResolve> const& regions) const;
    void fillBuffer(vk::Buffer dst, vk::DeviceSize offset, vk::DeviceSize size, u32 data) const;
    void clearColorImage(vk::Image image, vk::ImageLayout layout, const vk::ClearColorValue& color,
                         vk::ArrayProxy<const vk::ImageSubresourceRange> const& ranges) const;

    void beginDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const;
    void insertDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const;
    void endDebugUtilsLabelEXT() const;

    void resetQueryPool(vk::QueryPool pool, u32 firstQuery, u32 queryCount);
    void beginQuery(vk::QueryPool pool, u32 index, vk::QueryControlFlagBits flags);
    void endQuery(vk::QueryPool pool, u32 index);

private:
    Scheduler* scheduler;
};

} // namespace Vulkan

#pragma pop_macro("MemoryBarrier")
