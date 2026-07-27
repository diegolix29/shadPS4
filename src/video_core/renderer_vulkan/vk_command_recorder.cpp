// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_command_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

namespace Vulkan {

namespace {

/// Bump allocator over a recorded payload. The layout is computed once on the recording side,
/// replayed with the same sizes on the replay side.
class PayloadWriter {
public:
    explicit PayloadWriter(std::byte* base_) noexcept : base{base_} {}

    template <typename T>
    T* Copy(const T* source, size_t count) noexcept {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= 8);
        if (count == 0) {
            return nullptr;
        }
        T* const destination = reinterpret_cast<T*>(base + offset);
        std::memcpy(destination, source, count * sizeof(T));
        offset += Size<T>(count);
        return destination;
    }

    template <typename T>
    [[nodiscard]] static constexpr size_t Size(size_t count) noexcept {
        return (count * sizeof(T) + 7) & ~size_t{7};
    }

private:
    std::byte* base;
    size_t offset{};
};

template <typename T>
[[nodiscard]] const T* PayloadAt(const std::byte* payload, size_t offset) noexcept {
    return reinterpret_cast<const T*>(payload + offset);
}

/// Records func(cmdbuf, std::span<const T>) with a copy of items.
template <typename T, typename Func>
void RecordArray(Scheduler& scheduler, const T* items, u32 count, Func&& func) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::byte* const payload = scheduler.RecordWithPayload(
        count * sizeof(T), [count, func](vk::CommandBuffer cmdbuf, const std::byte* data) {
            func(cmdbuf, std::span<const T>{reinterpret_cast<const T*>(data), count});
        });
    if (count != 0) {
        std::memcpy(payload, items, count * sizeof(T));
    }
}

void CheckNoChain(const void* next) {
    ASSERT_MSG(next == nullptr, "Recorded Vulkan structures cannot carry pNext chains");
}

enum class DescriptorPayload : u8 {
    Image,
    Buffer,
    TexelBuffer,
};

/// Push descriptor write as recorded; the descriptors follow all writes in one slot array.
struct PackedWrite {
    u32 binding;
    u32 array_element;
    u32 count;
    vk::DescriptorType type;
};
static_assert(sizeof(PackedWrite) == 16);

/// Room recorded per descriptor: the largest of the descriptor infos.
constexpr size_t DescriptorSlotSize = 24;
static_assert(sizeof(vk::DescriptorImageInfo) == DescriptorSlotSize &&
              sizeof(vk::DescriptorBufferInfo) == DescriptorSlotSize &&
              sizeof(vk::BufferView) <= DescriptorSlotSize);

[[nodiscard]] DescriptorPayload PayloadOf(vk::DescriptorType type) {
    switch (type) {
    case vk::DescriptorType::eSampler:
    case vk::DescriptorType::eCombinedImageSampler:
    case vk::DescriptorType::eSampledImage:
    case vk::DescriptorType::eStorageImage:
    case vk::DescriptorType::eInputAttachment:
        return DescriptorPayload::Image;
    case vk::DescriptorType::eUniformTexelBuffer:
    case vk::DescriptorType::eStorageTexelBuffer:
        return DescriptorPayload::TexelBuffer;
    case vk::DescriptorType::eUniformBuffer:
    case vk::DescriptorType::eStorageBuffer:
    case vk::DescriptorType::eUniformBufferDynamic:
    case vk::DescriptorType::eStorageBufferDynamic:
        return DescriptorPayload::Buffer;
    default:
        UNREACHABLE_MSG("Descriptor type {} cannot be recorded", vk::to_string(type));
    }
}

} // Anonymous namespace

void CommandRecorder::pipelineBarrier(
    vk::PipelineStageFlags src_stage_mask, vk::PipelineStageFlags dst_stage_mask,
    vk::DependencyFlags flags, vk::ArrayProxy<const vk::MemoryBarrier> const& memory_barriers,
    vk::ArrayProxy<const vk::BufferMemoryBarrier> const& buffer_barriers,
    vk::ArrayProxy<const vk::ImageMemoryBarrier> const& image_barriers) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().pipelineBarrier(src_stage_mask, dst_stage_mask, flags,
                                                      memory_barriers, buffer_barriers,
                                                      image_barriers);
        return;
    }
    const u32 num_memory = memory_barriers.size();
    const u32 num_buffer = buffer_barriers.size();
    const u32 num_image = image_barriers.size();
    for (const auto& barrier : memory_barriers) {
        CheckNoChain(barrier.pNext);
    }
    for (const auto& barrier : buffer_barriers) {
        CheckNoChain(barrier.pNext);
    }
    for (const auto& barrier : image_barriers) {
        CheckNoChain(barrier.pNext);
    }
    const size_t buffer_offset = PayloadWriter::Size<vk::MemoryBarrier>(num_memory);
    const size_t image_offset =
        buffer_offset + PayloadWriter::Size<vk::BufferMemoryBarrier>(num_buffer);
    const size_t size = image_offset + PayloadWriter::Size<vk::ImageMemoryBarrier>(num_image);
    std::byte* const payload = scheduler->RecordWithPayload(
        size, [src_stage_mask, dst_stage_mask, flags, num_memory, num_buffer, num_image,
               buffer_offset, image_offset](vk::CommandBuffer cmdbuf, const std::byte* data) {
            cmdbuf.pipelineBarrier(src_stage_mask, dst_stage_mask, flags, num_memory,
                                   PayloadAt<vk::MemoryBarrier>(data, 0), num_buffer,
                                   PayloadAt<vk::BufferMemoryBarrier>(data, buffer_offset),
                                   num_image,
                                   PayloadAt<vk::ImageMemoryBarrier>(data, image_offset));
        });
    PayloadWriter writer{payload};
    writer.Copy(memory_barriers.data(), num_memory);
    writer.Copy(buffer_barriers.data(), num_buffer);
    writer.Copy(image_barriers.data(), num_image);
}

void CommandRecorder::pipelineBarrier2(const vk::DependencyInfo& info) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().pipelineBarrier2(info);
        return;
    }
    CheckNoChain(info.pNext);
    const u32 num_memory = info.memoryBarrierCount;
    const u32 num_buffer = info.bufferMemoryBarrierCount;
    const u32 num_image = info.imageMemoryBarrierCount;
    for (u32 i = 0; i < num_memory; ++i) {
        CheckNoChain(info.pMemoryBarriers[i].pNext);
    }
    for (u32 i = 0; i < num_buffer; ++i) {
        CheckNoChain(info.pBufferMemoryBarriers[i].pNext);
    }
    for (u32 i = 0; i < num_image; ++i) {
        CheckNoChain(info.pImageMemoryBarriers[i].pNext);
    }
    const size_t buffer_offset = PayloadWriter::Size<vk::MemoryBarrier2>(num_memory);
    const size_t image_offset =
        buffer_offset + PayloadWriter::Size<vk::BufferMemoryBarrier2>(num_buffer);
    const size_t size = image_offset + PayloadWriter::Size<vk::ImageMemoryBarrier2>(num_image);
    const vk::DependencyFlags flags = info.dependencyFlags;
    std::byte* const payload = scheduler->RecordWithPayload(
        size, [flags, num_memory, num_buffer, num_image, buffer_offset,
               image_offset](vk::CommandBuffer cmdbuf, const std::byte* data) {
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = flags,
                .memoryBarrierCount = num_memory,
                .pMemoryBarriers =
                    num_memory != 0 ? PayloadAt<vk::MemoryBarrier2>(data, 0) : nullptr,
                .bufferMemoryBarrierCount = num_buffer,
                .pBufferMemoryBarriers =
                    num_buffer != 0 ? PayloadAt<vk::BufferMemoryBarrier2>(data, buffer_offset)
                                    : nullptr,
                .imageMemoryBarrierCount = num_image,
                .pImageMemoryBarriers = num_image != 0
                                            ? PayloadAt<vk::ImageMemoryBarrier2>(data, image_offset)
                                            : nullptr,
            });
        });
    PayloadWriter writer{payload};
    writer.Copy(info.pMemoryBarriers, num_memory);
    writer.Copy(info.pBufferMemoryBarriers, num_buffer);
    writer.Copy(info.pImageMemoryBarriers, num_image);
}

void CommandRecorder::bindPipeline(vk::PipelineBindPoint bind_point, vk::Pipeline pipeline) const {
    scheduler->Record([bind_point, pipeline](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(bind_point, pipeline);
    });
}

void CommandRecorder::resetQueryPool(vk::QueryPool pool, u32 firstQuery, u32 queryCount) {
    scheduler->Record([pool, firstQuery, queryCount](vk::CommandBuffer cmdbuf) {
        cmdbuf.resetQueryPool(pool, firstQuery, queryCount);
    });
}

void CommandRecorder::beginQuery(vk::QueryPool pool, u32 index, vk::QueryControlFlagBits flags) {
    scheduler->Record(
        [pool, index, flags](vk::CommandBuffer cmdbuf) { cmdbuf.beginQuery(pool, index, flags); });
}

void CommandRecorder::endQuery(vk::QueryPool pool, u32 index) {
    scheduler->Record([pool, index](vk::CommandBuffer cmdbuf) { cmdbuf.endQuery(pool, index); });
}

void CommandRecorder::bindDescriptorSets(vk::PipelineBindPoint bind_point,
                                         vk::PipelineLayout layout, u32 first_set,
                                         vk::ArrayProxy<const vk::DescriptorSet> const& sets,
                                         vk::ArrayProxy<const u32> const& dynamic_offsets) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().bindDescriptorSets(bind_point, layout, first_set, sets,
                                                         dynamic_offsets);
        return;
    }
    const u32 num_sets = sets.size();
    const u32 num_offsets = dynamic_offsets.size();
    const size_t offsets_offset = PayloadWriter::Size<vk::DescriptorSet>(num_sets);
    const size_t size = offsets_offset + PayloadWriter::Size<u32>(num_offsets);
    std::byte* const payload = scheduler->RecordWithPayload(
        size, [bind_point, layout, first_set, num_sets, num_offsets,
               offsets_offset](vk::CommandBuffer cmdbuf, const std::byte* data) {
            cmdbuf.bindDescriptorSets(bind_point, layout, first_set, num_sets,
                                      PayloadAt<vk::DescriptorSet>(data, 0), num_offsets,
                                      PayloadAt<u32>(data, offsets_offset));
        });
    PayloadWriter writer{payload};
    writer.Copy(sets.data(), num_sets);
    writer.Copy(dynamic_offsets.data(), num_offsets);
}

void CommandRecorder::pushDescriptorSetKHR(
    vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
    vk::ArrayProxy<const vk::WriteDescriptorSet> const& writes, u32 num_descriptors) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().pushDescriptorSetKHR(bind_point, layout, set, writes);
        return;
    }
    // Draws push descriptors all the time, so the recorded form is compact and the Vulkan
    // structures are rebuilt on the recording thread.
    const u32 num_writes = writes.size();
    if (num_descriptors == ~u32{0}) {
        num_descriptors = 0;
        for (const auto& write : writes) {
            num_descriptors += write.descriptorCount;
        }
    }
    const size_t slots_offset = num_writes * sizeof(PackedWrite);
    std::byte* const payload = scheduler->RecordWithPayload(
        slots_offset + num_descriptors * DescriptorSlotSize,
        [bind_point, layout, set, num_writes, slots_offset](vk::CommandBuffer cmdbuf,
                                                            const std::byte* data) {
            thread_local std::vector<vk::WriteDescriptorSet> unpacked;
            if (unpacked.size() < num_writes) {
                unpacked.resize(num_writes);
            }
            const auto* packed = PayloadAt<PackedWrite>(data, 0);
            const std::byte* slot = data + slots_offset;
            for (u32 i = 0; i < num_writes; ++i) {
                const PackedWrite& write = packed[i];
                auto& out = unpacked[i];
                out = vk::WriteDescriptorSet{
                    .dstBinding = write.binding,
                    .dstArrayElement = write.array_element,
                    .descriptorCount = write.count,
                    .descriptorType = write.type,
                };
                switch (PayloadOf(write.type)) {
                case DescriptorPayload::Image:
                    out.pImageInfo = reinterpret_cast<const vk::DescriptorImageInfo*>(slot);
                    slot += write.count * sizeof(vk::DescriptorImageInfo);
                    break;
                case DescriptorPayload::Buffer:
                    out.pBufferInfo = reinterpret_cast<const vk::DescriptorBufferInfo*>(slot);
                    slot += write.count * sizeof(vk::DescriptorBufferInfo);
                    break;
                case DescriptorPayload::TexelBuffer:
                    out.pTexelBufferView = reinterpret_cast<const vk::BufferView*>(slot);
                    slot += write.count * sizeof(vk::BufferView);
                    break;
                }
            }
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, set, num_writes, unpacked.data());
        });
    auto* packed = reinterpret_cast<PackedWrite*>(payload);
    std::byte* slot = payload + slots_offset;
    for (const auto& write : writes) {
        CheckNoChain(write.pNext);
        const u32 count = write.descriptorCount;
        *packed++ = PackedWrite{
            .binding = write.dstBinding,
            .array_element = write.dstArrayElement,
            .count = count,
            .type = write.descriptorType,
        };
        switch (PayloadOf(write.descriptorType)) {
        case DescriptorPayload::Image:
            if (count == 1) [[likely]] {
                std::memcpy(slot, write.pImageInfo, sizeof(vk::DescriptorImageInfo));
            } else {
                std::memcpy(slot, write.pImageInfo, count * sizeof(vk::DescriptorImageInfo));
            }
            slot += count * sizeof(vk::DescriptorImageInfo);
            break;
        case DescriptorPayload::Buffer:
            if (count == 1) [[likely]] {
                std::memcpy(slot, write.pBufferInfo, sizeof(vk::DescriptorBufferInfo));
            } else {
                std::memcpy(slot, write.pBufferInfo, count * sizeof(vk::DescriptorBufferInfo));
            }
            slot += count * sizeof(vk::DescriptorBufferInfo);
            break;
        case DescriptorPayload::TexelBuffer:
            std::memcpy(slot, write.pTexelBufferView, count * sizeof(vk::BufferView));
            slot += count * sizeof(vk::BufferView);
            break;
        }
    }
}

void CommandRecorder::pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages,
                                    u32 offset, u32 size, const void* values) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().pushConstants(layout, stages, offset, size, values);
        return;
    }
    std::byte* const payload = scheduler->RecordWithPayload(
        size, [layout, stages, offset, size](vk::CommandBuffer cmdbuf, const std::byte* data) {
            cmdbuf.pushConstants(layout, stages, offset, size, data);
        });
    std::memcpy(payload, values, size);
}

void CommandRecorder::bindVertexBuffers(u32 first_binding, u32 binding_count,
                                        const vk::Buffer* buffers,
                                        const vk::DeviceSize* offsets) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().bindVertexBuffers(first_binding, binding_count, buffers,
                                                        offsets);
        return;
    }
    const size_t offsets_offset = PayloadWriter::Size<vk::Buffer>(binding_count);
    const size_t size = offsets_offset + PayloadWriter::Size<vk::DeviceSize>(binding_count);
    std::byte* const payload =
        scheduler->RecordWithPayload(size, [first_binding, binding_count, offsets_offset](
                                               vk::CommandBuffer cmdbuf, const std::byte* data) {
            cmdbuf.bindVertexBuffers(first_binding, binding_count, PayloadAt<vk::Buffer>(data, 0),
                                     PayloadAt<vk::DeviceSize>(data, offsets_offset));
        });
    PayloadWriter writer{payload};
    writer.Copy(buffers, binding_count);
    writer.Copy(offsets, binding_count);
}

void CommandRecorder::bindVertexBuffers2(u32 first_binding, u32 binding_count,
                                         const vk::Buffer* buffers, const vk::DeviceSize* offsets,
                                         const vk::DeviceSize* sizes,
                                         const vk::DeviceSize* strides) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().bindVertexBuffers2(first_binding, binding_count, buffers,
                                                         offsets, sizes, strides);
        return;
    }
    const bool has_sizes = sizes != nullptr;
    const bool has_strides = strides != nullptr;
    const size_t array_size = PayloadWriter::Size<vk::DeviceSize>(binding_count);
    const size_t offsets_offset = PayloadWriter::Size<vk::Buffer>(binding_count);
    const size_t sizes_offset = offsets_offset + array_size;
    const size_t strides_offset = sizes_offset + (has_sizes ? array_size : 0);
    const size_t size = strides_offset + (has_strides ? array_size : 0);
    std::byte* const payload = scheduler->RecordWithPayload(
        size, [first_binding, binding_count, has_sizes, has_strides, offsets_offset, sizes_offset,
               strides_offset](vk::CommandBuffer cmdbuf, const std::byte* data) {
            cmdbuf.bindVertexBuffers2(
                first_binding, binding_count, PayloadAt<vk::Buffer>(data, 0),
                PayloadAt<vk::DeviceSize>(data, offsets_offset),
                has_sizes ? PayloadAt<vk::DeviceSize>(data, sizes_offset) : nullptr,
                has_strides ? PayloadAt<vk::DeviceSize>(data, strides_offset) : nullptr);
        });
    PayloadWriter writer{payload};
    writer.Copy(buffers, binding_count);
    writer.Copy(offsets, binding_count);
    if (has_sizes) {
        writer.Copy(sizes, binding_count);
    }
    if (has_strides) {
        writer.Copy(strides, binding_count);
    }
}

void CommandRecorder::bindIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset,
                                      vk::IndexType index_type) const {
    scheduler->Record([buffer, offset, index_type](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindIndexBuffer(buffer, offset, index_type);
    });
}

void CommandRecorder::setVertexInputEXT(
    vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> const& bindings,
    vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> const& attributes) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().setVertexInputEXT(bindings, attributes);
        return;
    }
    for (const auto& binding : bindings) {
        CheckNoChain(binding.pNext);
    }
    for (const auto& attribute : attributes) {
        CheckNoChain(attribute.pNext);
    }
    const u32 num_bindings = bindings.size();
    const u32 num_attributes = attributes.size();
    const size_t attributes_offset =
        PayloadWriter::Size<vk::VertexInputBindingDescription2EXT>(num_bindings);
    const size_t size =
        attributes_offset +
        PayloadWriter::Size<vk::VertexInputAttributeDescription2EXT>(num_attributes);
    std::byte* const payload =
        scheduler->RecordWithPayload(size, [num_bindings, num_attributes, attributes_offset](
                                               vk::CommandBuffer cmdbuf, const std::byte* data) {
            cmdbuf.setVertexInputEXT(
                num_bindings, PayloadAt<vk::VertexInputBindingDescription2EXT>(data, 0),
                num_attributes,
                PayloadAt<vk::VertexInputAttributeDescription2EXT>(data, attributes_offset));
        });
    PayloadWriter writer{payload};
    writer.Copy(bindings.data(), num_bindings);
    writer.Copy(attributes.data(), num_attributes);
}

void CommandRecorder::setViewport(u32 first_viewport,
                                  vk::ArrayProxy<const vk::Viewport> const& viewports) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().setViewport(first_viewport, viewports);
        return;
    }
    RecordArray(*scheduler, viewports.data(), viewports.size(),
                [first_viewport](vk::CommandBuffer cmdbuf, std::span<const vk::Viewport> items) {
                    cmdbuf.setViewport(first_viewport, static_cast<u32>(items.size()),
                                       items.data());
                });
}

void CommandRecorder::setScissor(u32 first_scissor,
                                 vk::ArrayProxy<const vk::Rect2D> const& scissors) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().setScissor(first_scissor, scissors);
        return;
    }
    RecordArray(*scheduler, scissors.data(), scissors.size(),
                [first_scissor](vk::CommandBuffer cmdbuf, std::span<const vk::Rect2D> items) {
                    cmdbuf.setScissor(first_scissor, static_cast<u32>(items.size()), items.data());
                });
}

void CommandRecorder::setViewportWithCount(
    vk::ArrayProxy<const vk::Viewport> const& viewports) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().setViewportWithCount(viewports);
        return;
    }
    RecordArray(*scheduler, viewports.data(), viewports.size(),
                [](vk::CommandBuffer cmdbuf, std::span<const vk::Viewport> items) {
                    cmdbuf.setViewportWithCount(static_cast<u32>(items.size()), items.data());
                });
}

void CommandRecorder::setScissorWithCount(vk::ArrayProxy<const vk::Rect2D> const& scissors) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().setScissorWithCount(scissors);
        return;
    }
    RecordArray(*scheduler, scissors.data(), scissors.size(),
                [](vk::CommandBuffer cmdbuf, std::span<const vk::Rect2D> items) {
                    cmdbuf.setScissorWithCount(static_cast<u32>(items.size()), items.data());
                });
}

void CommandRecorder::beginRendering(const vk::RenderingInfo& info) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().beginRendering(info);
        return;
    }
    CheckNoChain(info.pNext);
    const u32 num_colors = info.colorAttachmentCount;
    for (u32 i = 0; i < num_colors; ++i) {
        CheckNoChain(info.pColorAttachments[i].pNext);
    }
    if (info.pDepthAttachment != nullptr) {
        CheckNoChain(info.pDepthAttachment->pNext);
    }
    if (info.pStencilAttachment != nullptr) {
        CheckNoChain(info.pStencilAttachment->pNext);
    }
    const bool has_depth = info.pDepthAttachment != nullptr;
    const bool has_stencil = info.pStencilAttachment != nullptr;
    const size_t attachment_size = PayloadWriter::Size<vk::RenderingAttachmentInfo>(1);
    const size_t depth_offset = PayloadWriter::Size<vk::RenderingAttachmentInfo>(num_colors);
    const size_t stencil_offset = depth_offset + (has_depth ? attachment_size : 0);
    const size_t size = stencil_offset + (has_stencil ? attachment_size : 0);
    vk::RenderingInfo copied_info = info;
    copied_info.pColorAttachments = nullptr;
    copied_info.pDepthAttachment = nullptr;
    copied_info.pStencilAttachment = nullptr;
    std::byte* const payload = scheduler->RecordWithPayload(
        size, [copied_info, has_depth, has_stencil, depth_offset,
               stencil_offset](vk::CommandBuffer cmdbuf, const std::byte* data) {
            vk::RenderingInfo rendering_info = copied_info;
            if (rendering_info.colorAttachmentCount != 0) {
                rendering_info.pColorAttachments = PayloadAt<vk::RenderingAttachmentInfo>(data, 0);
            }
            if (has_depth) {
                rendering_info.pDepthAttachment =
                    PayloadAt<vk::RenderingAttachmentInfo>(data, depth_offset);
            }
            if (has_stencil) {
                rendering_info.pStencilAttachment =
                    PayloadAt<vk::RenderingAttachmentInfo>(data, stencil_offset);
            }
            cmdbuf.beginRendering(rendering_info);
        });
    PayloadWriter writer{payload};
    writer.Copy(info.pColorAttachments, num_colors);
    if (has_depth) {
        writer.Copy(info.pDepthAttachment, 1);
    }
    if (has_stencil) {
        writer.Copy(info.pStencilAttachment, 1);
    }
}

void CommandRecorder::endRendering() const {
    scheduler->Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
}

void CommandRecorder::draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
                           u32 first_instance) const {
    scheduler->Record(
        [vertex_count, instance_count, first_vertex, first_instance](vk::CommandBuffer cmdbuf) {
            cmdbuf.draw(vertex_count, instance_count, first_vertex, first_instance);
        });
}

void CommandRecorder::drawIndexed(u32 index_count, u32 instance_count, u32 first_index,
                                  s32 vertex_offset, u32 first_instance) const {
    scheduler->Record([index_count, instance_count, first_index, vertex_offset,
                       first_instance](vk::CommandBuffer cmdbuf) {
        cmdbuf.drawIndexed(index_count, instance_count, first_index, vertex_offset, first_instance);
    });
}

void CommandRecorder::drawIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 draw_count,
                                   u32 stride) const {
    scheduler->Record([buffer, offset, draw_count, stride](vk::CommandBuffer cmdbuf) {
        cmdbuf.drawIndirect(buffer, offset, draw_count, stride);
    });
}

void CommandRecorder::drawIndexedIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 draw_count,
                                          u32 stride) const {
    scheduler->Record([buffer, offset, draw_count, stride](vk::CommandBuffer cmdbuf) {
        cmdbuf.drawIndexedIndirect(buffer, offset, draw_count, stride);
    });
}

void CommandRecorder::drawIndirectCount(vk::Buffer buffer, vk::DeviceSize offset,
                                        vk::Buffer count_buffer, vk::DeviceSize count_offset,
                                        u32 max_draw_count, u32 stride) const {
    scheduler->Record([buffer, offset, count_buffer, count_offset, max_draw_count,
                       stride](vk::CommandBuffer cmdbuf) {
        cmdbuf.drawIndirectCount(buffer, offset, count_buffer, count_offset, max_draw_count,
                                 stride);
    });
}

void CommandRecorder::drawIndexedIndirectCount(vk::Buffer buffer, vk::DeviceSize offset,
                                               vk::Buffer count_buffer, vk::DeviceSize count_offset,
                                               u32 max_draw_count, u32 stride) const {
    scheduler->Record([buffer, offset, count_buffer, count_offset, max_draw_count,
                       stride](vk::CommandBuffer cmdbuf) {
        cmdbuf.drawIndexedIndirectCount(buffer, offset, count_buffer, count_offset, max_draw_count,
                                        stride);
    });
}

void CommandRecorder::dispatch(u32 group_count_x, u32 group_count_y, u32 group_count_z) const {
    scheduler->Record([group_count_x, group_count_y, group_count_z](vk::CommandBuffer cmdbuf) {
        cmdbuf.dispatch(group_count_x, group_count_y, group_count_z);
    });
}

void CommandRecorder::dispatchIndirect(vk::Buffer buffer, vk::DeviceSize offset) const {
    scheduler->Record(
        [buffer, offset](vk::CommandBuffer cmdbuf) { cmdbuf.dispatchIndirect(buffer, offset); });
}

void CommandRecorder::copyBuffer(vk::Buffer src, vk::Buffer dst,
                                 vk::ArrayProxy<const vk::BufferCopy> const& regions) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().copyBuffer(src, dst, regions);
        return;
    }
    RecordArray(*scheduler, regions.data(), regions.size(),
                [src, dst](vk::CommandBuffer cmdbuf, std::span<const vk::BufferCopy> items) {
                    cmdbuf.copyBuffer(src, dst, static_cast<u32>(items.size()), items.data());
                });
}

void CommandRecorder::copyImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                                vk::ImageLayout dst_layout,
                                vk::ArrayProxy<const vk::ImageCopy> const& regions) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().copyImage(src, src_layout, dst, dst_layout, regions);
        return;
    }
    RecordArray(*scheduler, regions.data(), regions.size(),
                [src, src_layout, dst, dst_layout](vk::CommandBuffer cmdbuf,
                                                   std::span<const vk::ImageCopy> items) {
                    cmdbuf.copyImage(src, src_layout, dst, dst_layout,
                                     static_cast<u32>(items.size()), items.data());
                });
}

void CommandRecorder::copyBufferToImage(
    vk::Buffer src, vk::Image dst, vk::ImageLayout dst_layout,
    vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().copyBufferToImage(src, dst, dst_layout, regions);
        return;
    }
    RecordArray(*scheduler, regions.data(), regions.size(),
                [src, dst, dst_layout](vk::CommandBuffer cmdbuf,
                                       std::span<const vk::BufferImageCopy> items) {
                    cmdbuf.copyBufferToImage(src, dst, dst_layout, static_cast<u32>(items.size()),
                                             items.data());
                });
}

void CommandRecorder::copyImageToBuffer(
    vk::Image src, vk::ImageLayout src_layout, vk::Buffer dst,
    vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().copyImageToBuffer(src, src_layout, dst, regions);
        return;
    }
    RecordArray(*scheduler, regions.data(), regions.size(),
                [src, src_layout, dst](vk::CommandBuffer cmdbuf,
                                       std::span<const vk::BufferImageCopy> items) {
                    cmdbuf.copyImageToBuffer(src, src_layout, dst, static_cast<u32>(items.size()),
                                             items.data());
                });
}

void CommandRecorder::resolveImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                                   vk::ImageLayout dst_layout,
                                   vk::ArrayProxy<const vk::ImageResolve> const& regions) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().resolveImage(src, src_layout, dst, dst_layout, regions);
        return;
    }
    RecordArray(*scheduler, regions.data(), regions.size(),
                [src, src_layout, dst, dst_layout](vk::CommandBuffer cmdbuf,
                                                   std::span<const vk::ImageResolve> items) {
                    cmdbuf.resolveImage(src, src_layout, dst, dst_layout,
                                        static_cast<u32>(items.size()), items.data());
                });
}

void CommandRecorder::fillBuffer(vk::Buffer dst, vk::DeviceSize offset, vk::DeviceSize size,
                                 u32 data) const {
    scheduler->Record([dst, offset, size, data](vk::CommandBuffer cmdbuf) {
        cmdbuf.fillBuffer(dst, offset, size, data);
    });
}

void CommandRecorder::clearColorImage(
    vk::Image image, vk::ImageLayout layout, const vk::ClearColorValue& color,
    vk::ArrayProxy<const vk::ImageSubresourceRange> const& ranges) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().clearColorImage(image, layout, color, ranges);
        return;
    }
    RecordArray(*scheduler, ranges.data(), ranges.size(),
                [image, layout, color](vk::CommandBuffer cmdbuf,
                                       std::span<const vk::ImageSubresourceRange> items) {
                    cmdbuf.clearColorImage(image, layout, &color, static_cast<u32>(items.size()),
                                           items.data());
                });
}

namespace {

/// Records a debug label with its name copied into the payload.
template <typename Func>
void RecordLabel(Scheduler& scheduler, const vk::DebugUtilsLabelEXT& label, Func&& func) {
    CheckNoChain(label.pNext);
    const size_t length = label.pLabelName != nullptr ? std::strlen(label.pLabelName) : 0;
    const std::array<float, 4> color = label.color;
    std::byte* const payload = scheduler.RecordWithPayload(
        length + 1, [color, func](vk::CommandBuffer cmdbuf, const std::byte* data) {
            func(cmdbuf, vk::DebugUtilsLabelEXT{
                             .pLabelName = reinterpret_cast<const char*>(data),
                             .color = color,
                         });
        });
    if (length != 0) {
        std::memcpy(payload, label.pLabelName, length);
    }
    payload[length] = std::byte{0};
}

} // Anonymous namespace

void CommandRecorder::beginDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().beginDebugUtilsLabelEXT(label);
        return;
    }
    RecordLabel(*scheduler, label,
                [](vk::CommandBuffer cmdbuf, const vk::DebugUtilsLabelEXT& copied) {
                    cmdbuf.beginDebugUtilsLabelEXT(copied);
                });
}

void CommandRecorder::insertDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const {
    if (!scheduler->HasRecordingThread()) {
        scheduler->RawCommandBuffer().insertDebugUtilsLabelEXT(label);
        return;
    }
    RecordLabel(*scheduler, label,
                [](vk::CommandBuffer cmdbuf, const vk::DebugUtilsLabelEXT& copied) {
                    cmdbuf.insertDebugUtilsLabelEXT(copied);
                });
}

void CommandRecorder::endDebugUtilsLabelEXT() const {
    scheduler->Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endDebugUtilsLabelEXT(); });
}

} // namespace Vulkan
