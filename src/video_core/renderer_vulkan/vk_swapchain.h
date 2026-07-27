// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Frontend {
class WindowSDL;
}

namespace Vulkan {

class Instance;
class Scheduler;

class Swapchain {
public:
    explicit Swapchain(const Instance& instance, const Frontend::WindowSDL& window);
    ~Swapchain();

    /// Creates (or recreates) the swapchain with a given size.
    void Create(u32 width, u32 height);

    /// Recreates the swapchain with a given size and current surface.
    void Recreate(u32 width, u32 height);

    /// Acquires the next image in the swapchain.
    bool AcquireNextImage();

    /// Presents the current image and move to the next one. A nonzero present id identifies the
    /// present to WaitForPresent and to the latency markers; ids must increase.
    bool Present(u64 present_id = 0);

    /// Whether the presents of this swapchain carry ids and can be waited for.
    [[nodiscard]] bool HasPresentWait() const {
        return present_wait_active;
    }

    /// Changes whenever the swapchain handle is replaced; present ids belong to one serial.
    [[nodiscard]] u64 GetSerial() const {
        return serial.load(std::memory_order_acquire);
    }

    /// Waits up to timeout_ns until the present with present_id, or a later one, of the given
    /// swapchain serial reached the display. Safe on any thread. Returns eErrorOutOfDateKHR once
    /// the serial is gone.
    [[nodiscard]] vk::Result WaitForPresent(u64 swapchain_serial, u64 present_id,
                                            u64 timeout_ns) const;

    /// Whether NVIDIA Reflex latency reduction runs on this swapchain.
    [[nodiscard]] bool HasLowLatency() const {
        return low_latency_active.load(std::memory_order_acquire);
    }

    /// Asks the driver to signal the timeline semaphore with value when the next frame should
    /// start. Safe on any thread. Returns false when nothing will signal the semaphore.
    bool LatencySleep(vk::Semaphore semaphore, u64 value) const;

    /// Records a Reflex timing marker of the frame presented with present_id. Safe on any thread.
    void SetLatencyMarker(u64 present_id, vk::LatencyMarkerNV marker) const;

    vk::SurfaceKHR GetSurface() const {
        return surface;
    }

    vk::Image Image() const {
        return images[image_index];
    }

    vk::ImageView ImageView() const {
        return images_view[image_index];
    }

    vk::SurfaceFormatKHR GetSurfaceFormat() const {
        return surface_format;
    }

    vk::SwapchainKHR GetHandle() const {
        return swapchain;
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    u32 GetImageCount() const {
        return image_count;
    }

    u32 GetFrameIndex() const {
        return frame_index;
    }

    vk::Extent2D GetExtent() const {
        return extent;
    }

    [[nodiscard]] vk::Semaphore GetImageAcquiredSemaphore() const {
        return image_acquired[frame_index];
    }

    [[nodiscard]] vk::Semaphore GetPresentReadySemaphore() const {
        return present_ready[image_index];
    }

    bool HasHDR() const {
        return supports_hdr;
    }

    void SetHDR(bool hdr);

    bool GetHDR() const {
        return needs_hdr;
    }

    [[nodiscard]] bool IsFIFO() const {
        return present_mode == vk::PresentModeKHR::eFifo;
    }

    [[nodiscard]] bool IsMailbox() const {
        return present_mode == vk::PresentModeKHR::eMailbox;
    }

private:
    /// Selects the best available swapchain image format
    void FindPresentFormat();

    /// Selects the best available present mode
    void FindPresentMode();

    /// Sets the surface properties according to device capabilities
    void SetSurfaceProperties();

    /// Enables Reflex on a newly created swapchain.
    void SetLowLatencyMode();

    /// Destroys current swapchain resources
    void Destroy();

    /// Performs creation of image views and framebuffers from the swapchain images
    void SetupImages();

    /// Creates the image acquired and present ready semaphores
    void RefreshSemaphores();

private:
    const Instance& instance;
    const Frontend::WindowSDL& window;
    /// Held exclusively while the handle is replaced or destroyed, shared by other threads that
    /// use the handle.
    mutable std::shared_mutex handle_mutex;
    vk::SwapchainKHR swapchain{};
    vk::SurfaceKHR surface{};
    vk::SurfaceFormatKHR surface_format;
    vk::PresentModeKHR present_mode;
    vk::Extent2D extent;
    vk::SurfaceTransformFlagBitsKHR transform;
    vk::CompositeAlphaFlagBitsKHR composite_alpha;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> images_view;
    std::vector<vk::Semaphore> image_acquired;
    std::vector<vk::Semaphore> present_ready;
    std::vector<vk::Fence> present_fences;
    std::vector<u8> present_fence_pending;
    u32 width = 0;
    u32 height = 0;
    u32 image_count = 0;
    u32 image_index = 0;
    u32 frame_index = 0;
    bool needs_recreation = true;
    bool needs_hdr = false;    // The game requested HDR swapchain
    bool supports_hdr = false; // SC supports HDR output
    bool present_id2_supported = false; // The surface takes VK_KHR_present_id2 ids
    bool present_wait_active = false;
    bool low_latency_requested = false;
    std::atomic<bool> low_latency_active{false};
    std::atomic<u64> serial{0};
};

} // namespace Vulkan
