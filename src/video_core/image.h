// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include "common/enum.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"

#include <deque>
#include <optional>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaAllocator)

namespace VideoCore {

enum ImageFlagBits : u32 {
    Empty = 0,
    MaybeCpuDirty = 1 << 0, ///< The page this image is in was touched before the image address
    CpuDirty = 1 << 1,      ///< Contents have been modified from the CPU
    GpuDirty = 1 << 2, ///< Contents have been modified from the GPU (valid data in buffer cache)
    Dirty = MaybeCpuDirty | CpuDirty | GpuDirty,
    GpuModified = 1 << 3, ///< Contents have been modified from the GPU
    Registered = 1 << 6,  ///< True when the image is registered
    Picked = 1 << 7,      ///< Temporary flag to mark the image as picked
};
DECLARE_ENUM_FLAG_OPERATORS(ImageFlagBits)

struct UniqueImage {
    explicit UniqueImage() = default;
    explicit UniqueImage(vk::Device device, VmaAllocator allocator)
        : device{device}, allocator{allocator} {}
    ~UniqueImage();

    UniqueImage(const UniqueImage&) = delete;
    UniqueImage& operator=(const UniqueImage&) = delete;

    UniqueImage(UniqueImage&& other)
        : allocator{std::exchange(other.allocator, VK_NULL_HANDLE)},
          allocation{std::exchange(other.allocation, VK_NULL_HANDLE)},
          image{std::exchange(other.image, VK_NULL_HANDLE)}, image_ci{std::move(other.image_ci)} {}
    UniqueImage& operator=(UniqueImage&& other) {
        image = std::exchange(other.image, VK_NULL_HANDLE);
        allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        image_ci = std::move(other.image_ci);
        return *this;
    }

    void Create(const vk::ImageCreateInfo& image_ci);

    operator vk::Image() const {
        return image;
    }

    operator bool() const {
        return image;
    }

public:
    vk::Device device{};
    VmaAllocator allocator{};
    VmaAllocation allocation{};
    vk::Image image{};
    vk::ImageCreateInfo image_ci{};
};

constexpr Common::SlotId NULL_IMAGE_ID{0};

class BlitHelper;

struct Image {
    Image(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, BlitHelper& blit_helper,
          Common::SlotVector<ImageView>& slot_image_views, const ImageInfo& info);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Image(Image&& other) noexcept
        : instance(other.instance), scheduler(other.scheduler),
          blit_helper(other.blit_helper), slot_image_views(other.slot_image_views),
          info(std::move(other.info)), aspect_mask(other.aspect_mask),
          supported_samples(other.supported_samples), flags(other.flags),
          track_addr(other.track_addr), track_addr_end(other.track_addr_end),
          depth_id(other.depth_id), usage_flags(other.usage_flags),
          format_features(other.format_features),
          backing_images(std::move(other.backing_images)), backing(other.backing),
          mip_hashes(std::move(other.mip_hashes)), lru_id(other.lru_id),
          tick_accessed_last(other.tick_accessed_last), hash(other.hash),
          fast_update_state(other.fast_update_state.load(std::memory_order_relaxed)),
          usage(other.usage), binding(other.binding) {}

    Image& operator=(Image&& other) noexcept {
        if (this != &other) {
            instance = other.instance;
            scheduler = other.scheduler;
            blit_helper = other.blit_helper;
            slot_image_views = other.slot_image_views;
            info = std::move(other.info);
            aspect_mask = other.aspect_mask;
            supported_samples = other.supported_samples;
            flags = other.flags;
            track_addr = other.track_addr;
            track_addr_end = other.track_addr_end;
            depth_id = other.depth_id;
            usage_flags = other.usage_flags;
            format_features = other.format_features;
            backing_images = std::move(other.backing_images);
            backing = other.backing;
            mip_hashes = std::move(other.mip_hashes);
            lru_id = other.lru_id;
            tick_accessed_last = other.tick_accessed_last;
            hash = other.hash;
            fast_update_state.store(other.fast_update_state.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
            usage = other.usage;
            binding = other.binding;
        }
        return *this;
    }

    bool Overlaps(VAddr overlap_cpu_addr, size_t overlap_size) const noexcept {
        const VAddr overlap_end = overlap_cpu_addr + overlap_size;
        const auto image_addr = info.guest_address;
        const auto image_end = info.guest_address + info.guest_size;
        return image_addr < overlap_end && overlap_cpu_addr < image_end;
    }

    vk::Image GetImage() const {
        return backing->image.image;
    }

    bool IsTracked() {
        return track_addr != 0 && track_addr_end != 0;
    }

    bool SafeToDownload() const {
        return True(flags & ImageFlagBits::GpuModified) && False(flags & (ImageFlagBits::Dirty));
    }

    void AssociateDepth(ImageId image_id) {
        depth_id = image_id;
    }

    ImageView& FindView(const ImageViewInfo& view_info, bool ensure_guest_samples = true);

    using Barriers = boost::container::small_vector<vk::ImageMemoryBarrier2, 32>;
    Barriers GetBarriers(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                         vk::PipelineStageFlags2 dst_stage,
                         std::optional<SubresourceRange> subres_range);
    void Transit(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                 std::optional<SubresourceRange> range, vk::CommandBuffer cmdbuf = {});
    void Upload(std::span<const vk::BufferImageCopy> upload_copies, vk::Buffer buffer, u64 offset);
    void Download(std::span<const vk::BufferImageCopy> download_copies, vk::Buffer buffer,
                  u64 offset, u64 download_size);

    void CopyImage(Image& src_image);
    void CopyImageWithBuffer(Image& src_image, vk::Buffer buffer, u64 offset);
    void CopyMip(Image& src_image, u32 mip, u32 slice);

    void Resolve(Image& src_image, const VideoCore::SubresourceRange& mrt0_range,
                 const VideoCore::SubresourceRange& mrt1_range);
    void Clear(const vk::ClearValue& clear_value, const VideoCore::SubresourceRange& range);

    void SetBackingSamples(u32 num_samples, bool copy_backing = true);

public:
    const Vulkan::Instance* instance;
    Vulkan::Scheduler* scheduler;
    BlitHelper* blit_helper;
    Common::SlotVector<ImageView>* slot_image_views;
    ImageInfo info;
    vk::ImageAspectFlags aspect_mask = vk::ImageAspectFlagBits::eColor;
    vk::SampleCountFlags supported_samples = vk::SampleCountFlagBits::e1;
    ImageFlagBits flags = ImageFlagBits::Dirty;
    VAddr track_addr = 0;
    VAddr track_addr_end = 0;
    ImageId depth_id{};

    // Resource state tracking
    vk::ImageUsageFlags usage_flags;
    vk::FormatFeatureFlags2 format_features;
    struct State {
        vk::PipelineStageFlags2 pl_stage = vk::PipelineStageFlagBits2::eAllCommands;
        vk::AccessFlags2 access_mask = vk::AccessFlagBits2::eNone;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };
    struct BackingImage {
        UniqueImage image;
        State state;
        std::vector<State> subresource_states;
        boost::container::small_vector<ImageViewInfo, 4> image_view_infos;
        boost::container::small_vector<ImageViewId, 4> image_view_ids;

        // OPT#5: Hot-path ImageView lookup cache (last-hit).
        // Most draws reuse the same (view_info) repeatedly, so this avoids a linear scan.
        ImageViewInfo last_view_info{};
        ImageViewId last_view_id{};
        bool last_view_valid{};

        u32 num_samples;
    };
    std::deque<BackingImage> backing_images;
    BackingImage* backing{};
    boost::container::static_vector<u64, 16> mip_hashes{};
    u64 lru_id{};
    u64 tick_accessed_last{};
    u64 hash{};

    // =========================================================================
    // OPT: Atomic fast-state for lock-free UpdateImage() fast path.
    //
    // UpdateImage() is called for every image binding every draw call. The majority
    // of calls (~80%+) find the image already clean, tracked, and recently touched.
    // The previous implementation still took a shared_lock for this common case,
    // which showed up as 4.27% of L1D cache misses due to rwlock contention.
    //
    // This atomic packs {dirty_bit, tracked_bit, last_touch_tick} into a single
    // u64 that can be read with a single atomic load — no lock needed.
    // =========================================================================
    static constexpr u64 kFastStateDirty      = 1ULL << 0;
    static constexpr u64 kFastStateTracked    = 1ULL << 1;
    static constexpr u64 kFastStateTouchShift = 2;

    std::atomic<u64> fast_update_state{kFastStateDirty}; // initially dirty

    /// Mark the image as needing a full UpdateImage pass.
    void MarkFastStateDirty() noexcept {
        fast_update_state.fetch_or(kFastStateDirty, std::memory_order_release);
    }

    /// Update the atomic fast state after a successful UpdateImage pass.
    void UpdateFastState(u64 tick, bool is_tracked) noexcept {
        u64 state = (tick << kFastStateTouchShift);
        if (is_tracked) {
            state |= kFastStateTracked;
        }
        // bit 0 = 0 means clean
        fast_update_state.store(state, std::memory_order_release);
    }

    /// Read the current fast state atomically.
    u64 ReadFastState() const noexcept {
        return fast_update_state.load(std::memory_order_acquire);
    }

    struct {
        u32 texture : 1;
        u32 storage : 1;
        u32 render_target : 1;
        u32 depth_target : 1;
        u32 vo_surface : 1;
    } usage{};

    struct {
        u32 is_bound : 1;
        u32 is_target : 1;
        u32 needs_rebind : 1;
        u32 force_general : 1;
    } binding{};
};

} // namespace VideoCore
