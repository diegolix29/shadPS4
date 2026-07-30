// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Focused Vulkan-Hpp dispatch test for vkCmdBindVertexBuffers2.
//
// Validates that the exact Vulkan-Hpp overload used by BufferCache::BindVertexBuffers
// reaches the Vortek client correctly — i.e., the VULKAN_HPP_DEFAULT_DISPATCHER
// vkCmdBindVertexBuffers2 slot is non-null after init(*instance) + init(*device).
//
// This test must use the SAME dispatcher path as the production code:
//   - VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
//   - VULKAN_HPP_DEFAULT_DISPATCHER global object (shared with gcn_test_runner.cpp)
//   - vk::CommandBuffer::bindVertexBuffers2(...) Vulkan-Hpp overload
//   - BachataResolveBindVB2(device, gdpa, &source) resolver
//
// Calling the raw C function directly is NOT sufficient.

#include <gtest/gtest.h>

#include <array>

#include "gcn/gcn_test_runner.hpp"
#include "video_core/buffer_cache/vertex_buffer_bind_path.h"
#include "video_core/buffer_cache/vk_dispatch_reload.h"

// vk_common.h macros must be applied before including vulkan.hpp.
// gcn_test_runner.hpp already pulls in vulkan.hpp with the correct defines.

namespace {

TEST(VertexBufferBindPathTest, UsesCoreBindingWhenVertexInputIsDynamic) {
    EXPECT_EQ(VideoCore::SelectVertexBufferBindPath(true), VideoCore::VertexBufferBindPath::Core);
}

TEST(VertexBufferBindPathTest, UsesExtendedBindingWhenVertexInputIsStatic) {
    EXPECT_EQ(VideoCore::SelectVertexBufferBindPath(false),
              VideoCore::VertexBufferBindPath::Extended);
}

// ----------------------------------------------------------------------------
// VkDispatchBindVb2Test
// ----------------------------------------------------------------------------
class VkDispatchBindVb2Test : public ::testing::Test {
protected:
    void SetUp() override {
        auto r = gcn_test::Runner::instance();
        ASSERT_TRUE(r.has_value()) << "Runner init: " << r.error().message;
        runner_ = *r;
    }

    gcn_test::Runner* runner_ = nullptr;
};

// Test 1: vkCmdBindVertexBuffers2 slot is non-null in the default dispatcher
//         immediately after device initialization (same check as init-time audit).
TEST_F(VkDispatchBindVb2Test, SlotNonNullAfterDeviceInit) {
    // VULKAN_HPP_DEFAULT_DISPATCHER was initialized by Runner::initialize():
    //   init(getInstanceProcAddr)  →  init(instance)  →  init(device)
    // The slot must be populated by init(instance) via the EXT alias fallback,
    // or by init(device) via the core VK 1.3 entry.
    EXPECT_NE(VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBindVertexBuffers2, nullptr)
        << "vkCmdBindVertexBuffers2 Vulkan-Hpp dispatcher slot is null after init(*device)";
}

// Test 2: BachataResolveBindVB2 returns non-null when given the dispatcher's gdpa.
//         Regression guard for the PC=0 SIGSEGV crash whose root cause was a null
//         core-1.0 vkCmdBindVertexBuffers dispatcher slot under the Vortek RPC
//         bridge. The production fix (BufferCache::BindVertexBuffers) always
//         prefers the VB2 path resolved here; this asserts the resolver yields a
//         callable function before any indirect call.
TEST_F(VkDispatchBindVb2Test, BachataResolveBindVb2ReturnsNonNull) {
    auto gdpa = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    ASSERT_NE(gdpa, nullptr) << "vkGetDeviceProcAddr slot is null in dispatcher";

    const char* source = nullptr;
    auto fn = BachataResolveBindVB2(
        static_cast<VkDevice>(runner_->device()),
        gdpa,
        &source);
    EXPECT_NE(fn, nullptr)
        << "BachataResolveBindVB2 returned null; gdpa could not resolve vkCmdBindVertexBuffers2";
    EXPECT_NE(source, nullptr) << "resolver did not populate out_source";
}

// Test 2b: Null-input safety. The resolver must never dereference a null
//         device/gdpa (would re-introduce a branch-to-zero crash).
TEST_F(VkDispatchBindVb2Test, BachataResolveBindVb2NullInputsAreSafe) {
    const char* source = nullptr;
    EXPECT_EQ(BachataResolveBindVB2(VK_NULL_HANDLE, nullptr, &source), nullptr);
    EXPECT_EQ(BachataResolveBindVB2(static_cast<VkDevice>(runner_->device()), nullptr, &source),
              nullptr);
    EXPECT_EQ(BachataResolveBindVB2(VK_NULL_HANDLE,
              VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr, nullptr), nullptr);
    // And the core-1.0 resolver used by the fallback path.
    EXPECT_EQ(BachataResolveBindVB(VK_NULL_HANDLE, nullptr), nullptr);
}

// Test 3: Full end-to-end — allocate a command buffer, call bindVertexBuffers2()
//         through the Vulkan-Hpp CommandBuffer wrapper, submit, and wait.
//         This is the same path as BufferCache::BindVertexBuffers.
TEST_F(VkDispatchBindVb2Test, EndToEndBindVertexBuffers2) {
    vk::Device device  = runner_->device();
    vk::PhysicalDevice pd = runner_->physical_device();
    vk::Queue queue = runner_->queue();

    // Create command pool
    auto [pool_result, cmd_pool] = device.createCommandPool(vk::CommandPoolCreateInfo{
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = runner_->queue_family(),
    });
    ASSERT_EQ(pool_result, vk::Result::eSuccess) << "createCommandPool failed";

    // Allocate command buffer
    auto [alloc_result, cmd_bufs] = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
        .commandPool        = cmd_pool,
        .level              = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    });
    ASSERT_EQ(alloc_result, vk::Result::eSuccess) << "allocateCommandBuffers failed";
    vk::CommandBuffer cmdbuf = cmd_bufs[0];

    // Create a small vertex buffer (4 bytes) — content doesn't matter for the dispatch test.
    auto [buf_result, vtx_buf] = device.createBuffer(vk::BufferCreateInfo{
        .size        = 4,
        .usage       = vk::BufferUsageFlagBits::eVertexBuffer,
        .sharingMode = vk::SharingMode::eExclusive,
    });
    ASSERT_EQ(buf_result, vk::Result::eSuccess) << "createBuffer failed";

    // Allocate and bind memory
    auto mem_req = device.getBufferMemoryRequirements(vtx_buf);
    auto mem_props = pd.getMemoryProperties();
    std::uint32_t mem_type_idx = UINT32_MAX;
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req.memoryTypeBits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags &
             vk::MemoryPropertyFlagBits::eHostVisible)) {
            mem_type_idx = i;
            break;
        }
    }
    ASSERT_NE(mem_type_idx, UINT32_MAX) << "No suitable host-visible memory type found";

    auto [mem_result, vtx_mem] = device.allocateMemory(vk::MemoryAllocateInfo{
        .allocationSize  = mem_req.size,
        .memoryTypeIndex = mem_type_idx,
    });
    ASSERT_EQ(mem_result, vk::Result::eSuccess) << "allocateMemory failed";
    ASSERT_EQ(device.bindBufferMemory(vtx_buf, vtx_mem, 0), vk::Result::eSuccess);

    // Create fence for submit synchronization
    auto [fence_result, fence] = device.createFence(vk::FenceCreateInfo{});
    ASSERT_EQ(fence_result, vk::Result::eSuccess) << "createFence failed";

    // Record: begin → bindVertexBuffers2 (Vulkan-Hpp overload) → end
    ASSERT_EQ(cmdbuf.begin(vk::CommandBufferBeginInfo{
                  .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
              }),
              vk::Result::eSuccess);

    // Verify slot non-null before command recording (Task 10 requirement)
    ASSERT_NE(VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBindVertexBuffers2, nullptr)
        << "vkCmdBindVertexBuffers2 slot became null before command recording";

    vk::DeviceSize offset = 0;
    vk::DeviceSize size   = 4;
    vk::DeviceSize stride = 4;

    // ── This is the exact Vulkan-Hpp call path used by BufferCache::BindVertexBuffers ──
    // It goes through VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBindVertexBuffers2.
    // If the slot is null this call will crash at address 0.
    cmdbuf.bindVertexBuffers2(
        /*firstBinding=*/0,
        /*buffers=*/vtx_buf,
        /*offsets=*/offset,
        /*sizes=*/size,
        /*strides=*/stride);

    ASSERT_EQ(cmdbuf.end(), vk::Result::eSuccess);

    // Submit
    vk::SubmitInfo submit{
        .commandBufferCount = 1,
        .pCommandBuffers    = &cmdbuf,
    };
    ASSERT_EQ(queue.submit(1, &submit, fence), vk::Result::eSuccess);
    ASSERT_EQ(device.waitForFences(fence, VK_TRUE, UINT64_MAX), vk::Result::eSuccess);

    // Cleanup
    device.destroyFence(fence);
    device.freeMemory(vtx_mem);
    device.destroyBuffer(vtx_buf);
    device.freeCommandBuffers(cmd_pool, cmdbuf);
    device.destroyCommandPool(cmd_pool);
}

// Test 4: Serialization test — multiple buffers, nonzero firstBinding, different offsets, sizes, and strides.
TEST_F(VkDispatchBindVb2Test, SerializationMultipleBuffers) {
    vk::Device device  = runner_->device();
    vk::PhysicalDevice pd = runner_->physical_device();
    vk::Queue queue = runner_->queue();

    auto [pool_result, cmd_pool] = device.createCommandPool(vk::CommandPoolCreateInfo{
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = runner_->queue_family(),
    });
    ASSERT_EQ(pool_result, vk::Result::eSuccess);

    auto [alloc_result, cmd_bufs] = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
        .commandPool        = cmd_pool,
        .level              = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    });
    ASSERT_EQ(alloc_result, vk::Result::eSuccess);
    vk::CommandBuffer cmdbuf = cmd_bufs[0];

    // Create 2 buffers
    auto [b1_res, buf1] = device.createBuffer(vk::BufferCreateInfo{.size = 256, .usage = vk::BufferUsageFlagBits::eVertexBuffer});
    auto [b2_res, buf2] = device.createBuffer(vk::BufferCreateInfo{.size = 256, .usage = vk::BufferUsageFlagBits::eVertexBuffer});
    ASSERT_EQ(b1_res, vk::Result::eSuccess);
    ASSERT_EQ(b2_res, vk::Result::eSuccess);

    auto mem_req1 = device.getBufferMemoryRequirements(buf1);
    auto mem_props = pd.getMemoryProperties();
    std::uint32_t mem_type_idx = UINT32_MAX;
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req1.memoryTypeBits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible)) {
            mem_type_idx = i;
            break;
        }
    }
    ASSERT_NE(mem_type_idx, UINT32_MAX);

    auto [m1_res, mem1] = device.allocateMemory(vk::MemoryAllocateInfo{.allocationSize = mem_req1.size, .memoryTypeIndex = mem_type_idx});
    auto [m2_res, mem2] = device.allocateMemory(vk::MemoryAllocateInfo{.allocationSize = mem_req1.size, .memoryTypeIndex = mem_type_idx});
    ASSERT_EQ(m1_res, vk::Result::eSuccess);
    ASSERT_EQ(m2_res, vk::Result::eSuccess);
    ASSERT_EQ(device.bindBufferMemory(buf1, mem1, 0), vk::Result::eSuccess);
    ASSERT_EQ(device.bindBufferMemory(buf2, mem2, 0), vk::Result::eSuccess);

    auto [fence_result, fence] = device.createFence(vk::FenceCreateInfo{});
    ASSERT_EQ(fence_result, vk::Result::eSuccess);

    ASSERT_EQ(cmdbuf.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}), vk::Result::eSuccess);

    std::array<vk::Buffer, 2> buffers = {buf1, buf2};
    std::array<vk::DeviceSize, 2> offsets = {0, 16};
    std::array<vk::DeviceSize, 2> sizes = {64, 128};
    std::array<vk::DeviceSize, 2> strides = {16, 32};

    // firstBinding = 1 (nonzero), 2 buffers with distinct offsets, sizes, strides
    cmdbuf.bindVertexBuffers2(
        /*firstBinding=*/1,
        buffers,
        offsets,
        sizes,
        strides);

    ASSERT_EQ(cmdbuf.end(), vk::Result::eSuccess);

    vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmdbuf};
    ASSERT_EQ(queue.submit(1, &submit, fence), vk::Result::eSuccess);
    ASSERT_EQ(device.waitForFences(fence, VK_TRUE, UINT64_MAX), vk::Result::eSuccess);

    device.destroyFence(fence);
    device.freeMemory(mem1);
    device.freeMemory(mem2);
    device.destroyBuffer(buf1);
    device.destroyBuffer(buf2);
    device.freeCommandBuffers(cmd_pool, cmdbuf);
    device.destroyCommandPool(cmd_pool);
}

} // namespace
