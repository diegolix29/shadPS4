// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <fmt/format.h>

#include "common/assert.h"
#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_record_audit.h"
#include "video_core/renderer_vulkan/vk_record_selftest.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

namespace {

constexpr u32 WordCount = 64 * 1024;
constexpr u64 WordBytes = u64{WordCount} * sizeof(u32);
constexpr u32 SlotWords = 64;
constexpr u32 NumSlots = WordCount / SlotWords;
constexpr u32 ImageExtent = 64;
constexpr u32 ImageWords = ImageExtent * ImageExtent;
constexpr u64 ImageBytes = u64{ImageWords} * sizeof(u32);
constexpr vk::Format ColorFormat = vk::Format::eR32Uint;
/// Storage buffer bindings of the layout used to time wide descriptor pushes.
constexpr u32 WideBindings = 16;

constexpr std::string_view ComputeSource = R"(#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) readonly buffer Src { uint src[]; };
layout(set = 0, binding = 1, std430) writeonly buffer Dst { uint dst[]; };
layout(push_constant) uniform PushData {
    uint mul;
    uint add;
    uint count;
} pc;
void main() {
    const uint i = gl_GlobalInvocationID.x;
    if (i < pc.count) {
        dst[i] = src[i] * pc.mul + pc.add;
    }
}
)";

constexpr std::string_view VertexSource = R"(#version 450
void main() {
    const vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr std::string_view FragmentSource = R"(#version 450
layout(location = 0) out uint out_value;
layout(push_constant) uniform PushData {
    uint value;
} pc;
void main() {
    out_value = pc.value;
}
)";

class Rng {
public:
    explicit Rng(u64 seed) : state{seed | 1} {}

    u32 Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<u32>(state >> 32);
    }

    u32 Below(u32 bound) {
        return Next() % bound;
    }

private:
    u64 state;
};

/// Overwrites recorded arrays right after recording, so a command that kept a pointer to its
/// arguments instead of copying them replays garbage.
template <typename Container>
void Garble(Container& container) {
    std::memset(static_cast<void*>(container.data()), 0xCD,
                container.size() * sizeof(*container.data()));
}

/// Pipelines and render targets shared by every run; none of them depends on a scheduler.
struct TestDevice {
    explicit TestDevice(const Instance& instance) : device{instance.GetDevice()} {
        CreateCompute();
        CreateGraphics(instance);
        for (auto& target : targets) {
            target.image = VideoCore::UniqueImage{device, instance.GetAllocator()};
            target.image.Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = ColorFormat,
                .extent = {ImageExtent, ImageExtent, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage =
                    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
            });
            target.view = Check(device.createImageViewUnique({
                .image = target.image,
                .viewType = vk::ImageViewType::e2D,
                .format = ColorFormat,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            }));
        }
    }

    void CreateCompute() {
        std::array<vk::DescriptorSetLayoutBinding, WideBindings> bindings{};
        for (u32 i = 0; i < WideBindings; ++i) {
            bindings[i] = vk::DescriptorSetLayoutBinding{
                .binding = i,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute,
            };
        }
        const auto make_set_layout = [&](u32 count) {
            return Check(device.createDescriptorSetLayoutUnique({
                .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                .bindingCount = count,
                .pBindings = bindings.data(),
            }));
        };
        compute_set_layout = make_set_layout(2);
        wide_set_layout = make_set_layout(WideBindings);
        const vk::PushConstantRange push_range{
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .offset = 0,
            .size = 3 * sizeof(u32),
        };
        const vk::DescriptorSetLayout compute_handle = *compute_set_layout;
        compute_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &compute_handle,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_range,
        }));
        const vk::DescriptorSetLayout wide_handle = *wide_set_layout;
        wide_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &wide_handle,
        }));
        const vk::ShaderModule module =
            Compile({.name = "vk_record_selftest.comp", .code = ComputeSource},
                    vk::ShaderStageFlagBits::eCompute, device);
        compute_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage =
                        {
                            .stage = vk::ShaderStageFlagBits::eCompute,
                            .module = module,
                            .pName = "main",
                        },
                    .layout = *compute_layout,
                }));
        device.destroyShaderModule(module);
    }

    /// The same dynamic state as the rasterizer's pipelines, so the scheduler's dynamic state
    /// tracking can be driven exactly like during a game.
    void CreateGraphics(const Instance& instance) {
        const vk::PushConstantRange push_range{
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
            .offset = 0,
            .size = sizeof(u32),
        };
        graphics_layout = Check(device.createPipelineLayoutUnique({
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_range,
        }));
        const vk::ShaderModule vertex_module =
            Compile({.name = "vk_record_selftest.vert", .code = VertexSource},
                    vk::ShaderStageFlagBits::eVertex, device);
        const vk::ShaderModule fragment_module =
            Compile({.name = "vk_record_selftest.frag", .code = FragmentSource},
                    vk::ShaderStageFlagBits::eFragment, device);
        const std::array stages{
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = vertex_module,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = fragment_module,
                .pName = "main",
            },
        };
        const vk::PipelineVertexInputStateCreateInfo vertex_input{};
        const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
            .topology = vk::PrimitiveTopology::eTriangleList,
        };
        const vk::PipelineViewportStateCreateInfo viewport_state{};
        const vk::PipelineRasterizationStateCreateInfo rasterization{
            .polygonMode = vk::PolygonMode::eFill,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eCounterClockwise,
            .lineWidth = 1.0f,
        };
        const vk::PipelineMultisampleStateCreateInfo multisample{
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
        };
        const vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
        const vk::PipelineColorBlendAttachmentState blend_attachment{
            .blendEnable = false,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };
        const vk::PipelineColorBlendStateCreateInfo color_blend{
            .attachmentCount = 1,
            .pAttachments = &blend_attachment,
        };
        std::vector<vk::DynamicState> dynamic_states = {
            vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
            vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
            vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
            vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
            vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
            vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
            vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
            vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
            vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
        };
        if (instance.IsDepthBoundsSupported()) {
            dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
            dynamic_states.push_back(vk::DynamicState::eDepthBounds);
        }
        if (instance.IsDynamicColorWriteMaskSupported()) {
            dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
        }
        dynamic_vertex_input = instance.IsVertexInputDynamicState();
        if (dynamic_vertex_input) {
            dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
        }
        const vk::PipelineDynamicStateCreateInfo dynamic_info{
            .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data(),
        };
        const vk::PipelineRenderingCreateInfo rendering_info{
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &ColorFormat,
        };
        const vk::GraphicsPipelineCreateInfo pipeline_info{
            .pNext = &rendering_info,
            .stageCount = static_cast<u32>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport_state,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depth_stencil,
            .pColorBlendState = &color_blend,
            .pDynamicState = &dynamic_info,
            .layout = *graphics_layout,
        };
        for (auto& pipeline : graphics_pipelines) {
            pipeline = Check(device.createGraphicsPipelineUnique({}, pipeline_info));
        }
        device.destroyShaderModule(vertex_module);
        device.destroyShaderModule(fragment_module);
    }

    struct Target {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
    };

    vk::Device device;
    vk::UniqueDescriptorSetLayout compute_set_layout;
    vk::UniqueDescriptorSetLayout wide_set_layout;
    vk::UniquePipelineLayout compute_layout;
    vk::UniquePipelineLayout wide_layout;
    vk::UniquePipelineLayout graphics_layout;
    vk::UniquePipeline compute_pipeline;
    std::array<vk::UniquePipeline, 2> graphics_pipelines;
    std::array<Target, 2> targets;
    bool dynamic_vertex_input{};
};

[[nodiscard]] RenderState MakeRenderState(const TestDevice::Target& target, bool clear,
                                          u32 clear_value) {
    RenderState state{};
    state.width = ImageExtent;
    state.height = ImageExtent;
    state.num_layers = 1;
    state.num_color_attachments = 1;
    state.color_attachments[0].image_view = *target.view;
    state.color_attachments[0].image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    state.color_attachments[0].clear_value = {clear_value, 0, 0, 0};
    state.color_attachments[0].is_clear = clear ? 1 : 0;
    return state;
}

/// Puts the state the rasterizer sets for every draw into the scheduler's tracking.
void SetBaseDynamicState(DynamicState& dynamic_state) {
    dynamic_state.SetDepthTestEnabled(false);
    dynamic_state.SetDepthWriteEnabled(false);
    dynamic_state.SetDepthCompareOp(vk::CompareOp::eAlways);
    dynamic_state.SetDepthBoundsTestEnabled(false);
    dynamic_state.SetDepthBiasEnabled(false);
    dynamic_state.SetStencilTestEnabled(false);
    dynamic_state.SetPrimitiveRestartEnabled(false);
    dynamic_state.SetRasterizerDiscardEnabled(false);
    dynamic_state.SetCullMode(vk::CullModeFlagBits::eNone);
    dynamic_state.SetFrontFace(vk::FrontFace::eCounterClockwise);
    dynamic_state.SetBlendConstants({0.f, 0.f, 0.f, 0.f});
    ColorWriteMasks write_masks{};
    write_masks.fill(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                     vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
    dynamic_state.SetColorWriteMasks(write_masks);
    dynamic_state.SetLineWidth(1.0f);
}

[[nodiscard]] vk::Viewport FullViewport() {
    return vk::Viewport{
        .x = 0.f,
        .y = 0.f,
        .width = static_cast<float>(ImageExtent),
        .height = static_cast<float>(ImageExtent),
        .minDepth = 0.f,
        .maxDepth = 1.f,
    };
}

void TransferBarrier(const CommandRecorder& cmdbuf, Rng& rng, vk::Buffer a, vk::Buffer b) {
    constexpr auto AllAccess = vk::AccessFlagBits2::eTransferRead |
                               vk::AccessFlagBits2::eTransferWrite |
                               vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    constexpr auto AllStages =
        vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eComputeShader;
    std::array<vk::MemoryBarrier2, 1> memory_barriers{{
        {
            .srcStageMask = AllStages,
            .srcAccessMask = AllAccess,
            .dstStageMask = AllStages,
            .dstAccessMask = AllAccess,
        },
    }};
    std::vector<vk::BufferMemoryBarrier2> buffer_barriers(1 + rng.Below(48));
    for (size_t i = 0; i < buffer_barriers.size(); ++i) {
        buffer_barriers[i] = vk::BufferMemoryBarrier2{
            .srcStageMask = AllStages,
            .srcAccessMask = AllAccess,
            .dstStageMask = AllStages,
            .dstAccessMask = AllAccess,
            .buffer = (i & 1) ? a : b,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = static_cast<u32>(memory_barriers.size()),
        .pMemoryBarriers = memory_barriers.data(),
        .bufferMemoryBarrierCount = static_cast<u32>(buffer_barriers.size()),
        .pBufferMemoryBarriers = buffer_barriers.data(),
    });
    Garble(memory_barriers);
    Garble(buffer_barriers);
}

void ImageBarrier(const CommandRecorder& cmdbuf, vk::Image image, vk::ImageLayout old_layout,
                  vk::ImageLayout new_layout) {
    std::array<vk::ImageMemoryBarrier2, 1> barriers{{
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        },
    }};
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });
    Garble(barriers);
}

struct Result {
    std::vector<u32> gpu_a;
    std::vector<u32> gpu_b;
    std::vector<u32> gpu_images;
    std::vector<u32> model_a;
    std::vector<u32> model_b;
    std::vector<u32> model_images;
    u32 deferred_ops{};
    u64 command_buffers{};
    std::thread::id replay_thread{};
};

/// Records a randomized stream exercising every recorder path, keeping a CPU model of what the
/// GPU must produce.
Result RunWorkload(const Instance& instance, Scheduler& scheduler, const TestDevice& test,
                   u32 rounds, u64 seed) {
    using VideoCore::Buffer;
    using VideoCore::MemoryUsage;
    const u64 first_tick = scheduler.CurrentTick();
    constexpr auto StorageFlags = vk::BufferUsageFlagBits::eTransferSrc |
                                  vk::BufferUsageFlagBits::eTransferDst |
                                  vk::BufferUsageFlagBits::eStorageBuffer;
    Buffer buffer_a{instance, scheduler, MemoryUsage::DeviceLocal, 0, StorageFlags, WordBytes};
    Buffer buffer_b{instance, scheduler, MemoryUsage::DeviceLocal, 0, StorageFlags, WordBytes};
    const u64 readback_size = WordBytes * 2 + ImageBytes * rounds;
    Buffer readback{
        instance,     scheduler, MemoryUsage::Download, 0, vk::BufferUsageFlagBits::eTransferDst,
        readback_size};
    const vk::Buffer a = buffer_a.Handle();
    const vk::Buffer b = buffer_b.Handle();
    const auto& target = test.targets[0];

    Result result;
    result.model_a.assign(WordCount, 0);
    result.model_b.assign(WordCount, 0);
    result.model_images.assign(static_cast<size_t>(rounds) * ImageWords, 0);
    auto& model_a = result.model_a;
    auto& model_b = result.model_b;

    Rng rng{seed};
    const u32 align_words =
        std::max<u32>(1, static_cast<u32>(instance.StorageMinAlignment() / sizeof(u32)));
    std::atomic<u32> deferred_ops{0};
    std::vector<u32> slots(NumSlots);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.fillBuffer(a, 0, WordBytes, 0);
    cmdbuf.fillBuffer(b, 0, WordBytes, 0);
    // Where recorded commands run.
    scheduler.Record(
        [seen = &result.replay_thread](vk::CommandBuffer) { *seen = std::this_thread::get_id(); });

    for (u32 round = 0; round < rounds; ++round) {
        // Batches of disjoint fills; later batches overwrite earlier ones after a barrier, which
        // checks the order across chunks and command buffers.
        const u32 batches = 2 + rng.Below(3);
        for (u32 batch = 0; batch < batches; ++batch) {
            TransferBarrier(cmdbuf, rng, a, b);
            std::iota(slots.begin(), slots.end(), 0U);
            const u32 fills = 200 + rng.Below(800);
            for (u32 i = 0; i < fills; ++i) {
                std::swap(slots[i], slots[i + rng.Below(NumSlots - i)]);
                const u32 slot_offset = rng.Below(SlotWords);
                const u32 offset = slots[i] * SlotWords + slot_offset;
                const u32 length = 1 + rng.Below(SlotWords - slot_offset);
                const u32 value = rng.Next();
                cmdbuf.fillBuffer(a, u64{offset} * sizeof(u32), u64{length} * sizeof(u32), value);
                std::fill_n(model_a.begin() + offset, length, value);
            }
        }
        TransferBarrier(cmdbuf, rng, a, b);

        // Copies with disjoint destinations; long region lists take the heap payload path.
        std::iota(slots.begin(), slots.end(), 0U);
        std::vector<vk::BufferCopy> regions(1 + rng.Below(NumSlots / 2));
        for (u32 i = 0; i < regions.size(); ++i) {
            std::swap(slots[i], slots[i + rng.Below(NumSlots - i)]);
            const u32 source = rng.Below(WordCount - SlotWords);
            const u32 length = 1 + rng.Below(SlotWords);
            const u32 destination = slots[i] * SlotWords;
            regions[i] = vk::BufferCopy{
                .srcOffset = u64{source} * sizeof(u32),
                .dstOffset = u64{destination} * sizeof(u32),
                .size = u64{length} * sizeof(u32),
            };
            std::copy_n(model_a.begin() + source, length, model_b.begin() + destination);
        }
        cmdbuf.copyBuffer(a, b, regions);
        Garble(regions);
        TransferBarrier(cmdbuf, rng, a, b);

        // Compute pass through push descriptors and push constants.
        const u32 first = (rng.Below(WordCount / 2) / align_words) * align_words;
        const u32 count = 1 + rng.Below(WordCount / 4);
        std::array<u32, 3> push{rng.Next() | 1, rng.Next(), count};
        std::array<vk::DescriptorBufferInfo, 2> infos{{
            {.buffer = a, .offset = u64{first} * sizeof(u32), .range = u64{count} * sizeof(u32)},
            {.buffer = b, .offset = u64{first} * sizeof(u32), .range = u64{count} * sizeof(u32)},
        }};
        std::array<vk::WriteDescriptorSet, 2> writes{{
            {
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &infos[0],
            },
            {
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &infos[1],
            },
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *test.compute_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *test.compute_layout, 0,
                                    writes);
        Garble(infos);
        Garble(writes);
        cmdbuf.pushConstants(*test.compute_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(push), push.data());
        for (u32 i = 0; i < count; ++i) {
            model_b[first + i] = model_a[first + i] * push[0] + push[1];
        }
        Garble(push);
        cmdbuf.dispatch(Common::DivCeil(count, 64U), 1, 1);
        TransferBarrier(cmdbuf, rng, a, b);

        // Two scissored draws over a cleared target, through the scheduler's rendering scope and
        // dynamic state tracking like the rasterizer.
        ImageBarrier(cmdbuf, target.image, vk::ImageLayout::eUndefined,
                     vk::ImageLayout::eColorAttachmentOptimal);
        const u32 clear_value = rng.Next();
        scheduler.BeginRendering(MakeRenderState(target, true, clear_value));
        u32* const model_image = result.model_images.data() + size_t{round} * ImageWords;
        std::fill_n(model_image, ImageWords, clear_value);

        auto& dynamic_state = scheduler.GetDynamicState();
        SetBaseDynamicState(dynamic_state);
        for (u32 draw = 0; draw < 2; ++draw) {
            const u32 x = rng.Below(ImageExtent);
            const u32 y = rng.Below(ImageExtent);
            const u32 width = 1 + rng.Below(ImageExtent - x);
            const u32 height = 1 + rng.Below(ImageExtent - y);
            const u32 value = rng.Next();
            dynamic_state.SetSingleViewportScissor(
                FullViewport(), vk::Rect2D{
                                    .offset = {static_cast<s32>(x), static_cast<s32>(y)},
                                    .extent = {width, height},
                                });
            dynamic_state.Commit(instance, scheduler);
            if (test.dynamic_vertex_input) {
                cmdbuf.setVertexInputEXT({}, {});
            }
            u32 push_value = value;
            cmdbuf.pushConstants(*test.graphics_layout, vk::ShaderStageFlagBits::eFragment, 0,
                                 sizeof(push_value), &push_value);
            push_value = 0xCDCDCDCD;
            scheduler.BindGraphicsPipeline(*test.graphics_pipelines[draw]);
            cmdbuf.draw(3, 1, 0, 0);
            for (u32 row = y; row < y + height; ++row) {
                std::fill_n(model_image + row * ImageExtent + x, width, value);
            }
        }
        scheduler.EndRendering();
        ImageBarrier(cmdbuf, target.image, vk::ImageLayout::eColorAttachmentOptimal,
                     vk::ImageLayout::eTransferSrcOptimal);
        std::array<vk::BufferImageCopy, 1> image_copy{{
            {
                .bufferOffset = WordBytes * 2 + ImageBytes * round,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .imageOffset = {0, 0, 0},
                .imageExtent = {ImageExtent, ImageExtent, 1},
            },
        }};
        cmdbuf.copyImageToBuffer(target.image, vk::ImageLayout::eTransferSrcOptimal,
                                 readback.Handle(), image_copy);
        Garble(image_copy);

        scheduler.DeferOperation([counter = &deferred_ops] { counter->fetch_add(1); });

        // Vary how command buffers end: kept open across rounds, plain flushes, flushes that wait
        // for the hand-off to the driver, and waits for the GPU.
        switch (rng.Below(5)) {
        case 0:
            break;
        case 1:
            scheduler.Flush();
            break;
        case 2: {
            SubmitInfo info{};
            scheduler.Flush(info);
            scheduler.WaitSubmitted(scheduler.CurrentTick() - 1);
            break;
        }
        case 3:
            scheduler.Wait(scheduler.CurrentTick());
            break;
        default:
            scheduler.PopPendingOperations(true);
            break;
        }
    }

    TransferBarrier(cmdbuf, rng, a, b);
    cmdbuf.copyBuffer(a, readback.Handle(), vk::BufferCopy{.size = WordBytes});
    cmdbuf.copyBuffer(b, readback.Handle(),
                      vk::BufferCopy{.dstOffset = WordBytes, .size = WordBytes});
    scheduler.Finish();
    scheduler.PopPendingOperations(true);

    readback.Invalidate(0, readback_size);
    const u32* const mapped = reinterpret_cast<const u32*>(readback.mapped_data.data());
    result.gpu_a.assign(mapped, mapped + WordCount);
    result.gpu_b.assign(mapped + WordCount, mapped + 2 * WordCount);
    result.gpu_images.assign(mapped + 2 * WordCount,
                             mapped + 2 * WordCount + size_t{rounds} * ImageWords);
    result.deferred_ops = deferred_ops.load();
    result.command_buffers = scheduler.CurrentTick() - first_tick;
    return result;
}

/// Cost, on the thread that records, of each kind of command the rasterizer issues.
struct Costs {
    static constexpr std::array<std::string_view, 6> Names = {
        "draw", "render pass", "pipeline bind", "barrier", "16-descriptor push", "buffer copy",
    };
    std::array<double, Names.size()> ns{};
};

/// Times each command kind. Sizes stay below what the recording thread may queue, so the time
/// is the recording itself and not the recording thread catching up.
Costs RunBenchmark(const Instance& instance, Scheduler& scheduler, const TestDevice& test) {
    using VideoCore::Buffer;
    using VideoCore::MemoryUsage;
    constexpr auto StorageFlags = vk::BufferUsageFlagBits::eTransferSrc |
                                  vk::BufferUsageFlagBits::eTransferDst |
                                  vk::BufferUsageFlagBits::eStorageBuffer;
    Buffer buffer_a{instance, scheduler, MemoryUsage::DeviceLocal, 0, StorageFlags, WordBytes};
    Buffer buffer_b{instance, scheduler, MemoryUsage::DeviceLocal, 0, StorageFlags, WordBytes};
    const vk::Buffer a = buffer_a.Handle();
    const vk::Buffer b = buffer_b.Handle();
    const auto cmdbuf = scheduler.CommandBuffer();
    auto& dynamic_state = scheduler.GetDynamicState();
    const vk::DeviceSize align = instance.StorageMinAlignment();

    std::array<vk::DescriptorBufferInfo, WideBindings> infos{};
    std::array<vk::WriteDescriptorSet, WideBindings> writes{};
    for (u32 i = 0; i < WideBindings; ++i) {
        infos[i] = {.buffer = (i & 1) ? a : b, .offset = i * align, .range = 256};
        writes[i] = {
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &infos[i],
        };
    }

    const auto prepare_targets = [&] {
        for (const auto& target : test.targets) {
            ImageBarrier(cmdbuf, target.image, vk::ImageLayout::eUndefined,
                         vk::ImageLayout::eColorAttachmentOptimal);
        }
    };
    const auto draw_with_state = [&](u32 draw) {
        if (draw % 8 == 0) {
            const s32 offset = static_cast<s32>(draw / 8 % ImageExtent);
            dynamic_state.SetSingleViewportScissor(
                FullViewport(), vk::Rect2D{.offset = {offset, offset}, .extent = {1, 1}});
            dynamic_state.SetBlendConstants({static_cast<float>(draw), 0.f, 0.f, 0.f});
        }
        dynamic_state.Commit(instance, scheduler);
        if (test.dynamic_vertex_input && draw % 8 == 0) {
            cmdbuf.setVertexInputEXT({}, {});
        }
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *test.compute_layout, 0,
                                    std::span{writes}.first(2));
        cmdbuf.pushConstants(*test.graphics_layout, vk::ShaderStageFlagBits::eFragment, 0,
                             sizeof(draw), &draw);
        scheduler.BindGraphicsPipeline(*test.graphics_pipelines[0]);
        cmdbuf.draw(3, 1, 0, 0);
    };

    Costs costs;
    const auto time = [&](size_t index, u32 count, auto&& setup, auto&& body, auto&& teardown) {
        // Start from an idle GPU and recording thread, measure the best of three passes.
        double best = 0.0;
        for (u32 pass = 0; pass < 3; ++pass) {
            scheduler.Finish();
            setup();
            const auto start = std::chrono::steady_clock::now();
            for (u32 i = 0; i < count; ++i) {
                body(i);
            }
            const double elapsed =
                std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                    .count() /
                count;
            teardown();
            best = pass == 0 ? elapsed : std::min(best, elapsed);
        }
        costs.ns[index] = best;
    };
    const auto no_op = [] {};

    time(
        0, 5000,
        [&] {
            prepare_targets();
            scheduler.BeginRendering(MakeRenderState(test.targets[0], false, 0));
            SetBaseDynamicState(dynamic_state);
        },
        draw_with_state, [&] { scheduler.EndRendering(); });
    time(
        1, 2000,
        [&] {
            prepare_targets();
            SetBaseDynamicState(dynamic_state);
        },
        [&](u32 i) {
            scheduler.BeginRendering(MakeRenderState(test.targets[i & 1], false, 0));
            draw_with_state(i);
        },
        [&] { scheduler.EndRendering(); });
    time(
        2, 20000,
        [&] {
            prepare_targets();
            scheduler.BeginRendering(MakeRenderState(test.targets[0], false, 0));
            SetBaseDynamicState(dynamic_state);
            dynamic_state.Commit(instance, scheduler);
        },
        [&](u32 i) {
            scheduler.BindGraphicsPipeline(*test.graphics_pipelines[i & 1]);
            cmdbuf.draw(3, 1, 0, 0);
        },
        [&] { scheduler.EndRendering(); });
    time(
        3, 10000, no_op,
        [&](u32 i) {
            const vk::BufferMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
                .buffer = (i & 1) ? a : b,
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers = &barrier,
            });
        },
        no_op);
    time(
        4, 1000, no_op,
        [&](u32 i) {
            infos[0].offset = (i % 16) * align;
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *test.wide_layout, 0,
                                        writes);
        },
        no_op);
    time(
        5, 20000, no_op,
        [&](u32 i) {
            cmdbuf.copyBuffer(a, b,
                              vk::BufferCopy{
                                  .srcOffset = (i % 1024) * 64ULL,
                                  .dstOffset = (i % 1024) * 64ULL,
                                  .size = 64,
                              });
        },
        no_op);
    scheduler.Finish();
    return costs;
}

bool Compare(std::string_view what, const std::vector<u32>& actual,
             const std::vector<u32>& expected) {
    if (actual.size() != expected.size()) {
        LOG_ERROR(Render_Vulkan, "Record self-test: {} has {} words, expected {}", what,
                  actual.size(), expected.size());
        return false;
    }
    size_t mismatches = 0;
    size_t first = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] != expected[i]) {
            if (mismatches++ == 0) {
                first = i;
            }
        }
    }
    if (mismatches != 0) {
        LOG_ERROR(Render_Vulkan,
                  "Record self-test: {} differs in {} of {} words, first at {} ({:#x} != {:#x})",
                  what, mismatches, actual.size(), first, actual[first], expected[first]);
        return false;
    }
    return true;
}

bool CheckRun(std::string_view name, const Result& result, u32 rounds) {
    bool passed = true;
    passed &= Compare(fmt::format("{} buffer A", name), result.gpu_a, result.model_a);
    passed &= Compare(fmt::format("{} buffer B", name), result.gpu_b, result.model_b);
    passed &=
        Compare(fmt::format("{} render targets", name), result.gpu_images, result.model_images);
    if (result.deferred_ops != rounds) {
        LOG_ERROR(Render_Vulkan, "Record self-test: {} ran {} of {} deferred operations", name,
                  result.deferred_ops, rounds);
        passed = false;
    }
    return passed;
}

} // Anonymous namespace

bool RunRecordSelfTest(u32 rounds) {
    rounds = std::clamp<u32>(rounds, 1, 512);
    constexpr u64 Seed = 0x5eed'1234'abcd'0042ULL;
#ifdef _WIN32
    constexpr auto WindowType = Frontend::WindowSystemType::Windows;
#else
    constexpr auto WindowType = Frontend::WindowSystemType::Headless;
#endif
    Instance instance{WindowType, -1};
    const TestDevice test{instance};
    const std::thread::id test_thread = std::this_thread::get_id();

    // Recording costs are measured before the audit slows every command down, after a pass that
    // warms up the driver.
    Costs immediate_costs;
    Costs threaded_costs;
    {
        Scheduler scheduler{instance, true, false};
        static_cast<void>(RunBenchmark(instance, scheduler, test));
        immediate_costs = RunBenchmark(instance, scheduler, test);
    }
    {
        Scheduler scheduler{instance, true, true};
        threaded_costs = RunBenchmark(instance, scheduler, test);
    }

    // The calling-thread path first: the audit never sees its command buffers as claimed.
    RecordAudit::Install();
    const RecordAudit::Stats before_immediate = RecordAudit::GetStats();
    Result immediate;
    {
        Scheduler scheduler{instance, true, false};
        immediate = RunWorkload(instance, scheduler, test, rounds, Seed);
    }
    const RecordAudit::Stats before = RecordAudit::GetStats();
    Result threaded;
    bool had_thread = false;
    {
        Scheduler scheduler{instance, true, true};
        had_thread = scheduler.HasRecordingThread();
        threaded = RunWorkload(instance, scheduler, test, rounds, Seed);
    }
    const RecordAudit::Stats after = RecordAudit::GetStats();

    bool passed = true;
    if (!had_thread) {
        LOG_ERROR(Render_Vulkan, "Record self-test: the scheduler did not start a recording "
                                 "thread (GPU profiling active?)");
        passed = false;
    }
    passed &= CheckRun("calling-thread run", immediate, rounds);
    passed &= CheckRun("recording-thread run", threaded, rounds);
    passed &=
        Compare("recording thread vs calling thread, buffer A", threaded.gpu_a, immediate.gpu_a);
    passed &=
        Compare("recording thread vs calling thread, buffer B", threaded.gpu_b, immediate.gpu_b);
    passed &= Compare("recording thread vs calling thread, render targets", threaded.gpu_images,
                      immediate.gpu_images);
    if (immediate.replay_thread != test_thread) {
        LOG_ERROR(Render_Vulkan, "Record self-test: calling-thread run replayed elsewhere");
        passed = false;
    }
    if (threaded.replay_thread == test_thread || threaded.replay_thread == std::thread::id{}) {
        LOG_ERROR(Render_Vulkan,
                  "Record self-test: recorded commands ran on the recording thread's caller");
        passed = false;
    }
    if (after.producers.size() != 1) {
        LOG_ERROR(Render_Vulkan, "Record self-test: {} threads recorded commands, expected 1",
                  after.producers.size());
        passed = false;
    }
    const u64 owner_calls = after.owner_calls - before.owner_calls;
    const u64 foreign_calls = after.foreign_calls - before.foreign_calls;
    const u64 unclaimed_calls = after.unclaimed_calls - before.unclaimed_calls;
    if (owner_calls == 0 || foreign_calls != 0 || unclaimed_calls != 0) {
        LOG_ERROR(Render_Vulkan,
                  "Record self-test: audit saw {} calls on the recording thread, {} from other "
                  "threads (first: {}), {} on command buffers it did not own",
                  owner_calls, foreign_calls, after.first_foreign, unclaimed_calls);
        passed = false;
    }

    // Both runs issue the same commands; the calling-thread run begins one more command buffer
    // because it opens the next one eagerly after its final submit.
    const u64 immediate_calls = before.unclaimed_calls - before_immediate.unclaimed_calls;
    LOG_INFO(Render_Vulkan,
             "Record self-test {}: {} rounds, {} command buffers, {} Vulkan calls on the "
             "recording thread and {} anywhere else ({} calls without the recording thread)",
             passed ? "passed" : "FAILED", rounds, threaded.command_buffers, owner_calls,
             foreign_calls + unclaimed_calls, immediate_calls);
    for (size_t i = 0; i < Costs::Names.size(); ++i) {
        LOG_INFO(Render_Vulkan,
                 "Record self-test: {:<18} costs the recording caller {:6.0f} ns without the "
                 "recording thread, {:6.0f} ns with it",
                 Costs::Names[i], immediate_costs.ns[i], threaded_costs.ns[i]);
    }
    return passed;
}

} // namespace Vulkan
