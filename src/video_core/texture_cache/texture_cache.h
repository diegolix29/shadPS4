// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <boost/container/small_vector.hpp>
#include <queue>
#include <tsl/robin_map.h>

#include "common/hash.h"
#include "common/lru_cache.h"
#include "common/slot_vector.h"
#include "shader_recompiler/resource.h"
#include "video_core/multi_level_page_table.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/sampler.h"
#include "video_core/texture_cache/tile_manager.h"

namespace AmdGpu {
struct Liverpool;
}

namespace VideoCore {

class BufferCache;
struct GpuAuthorityShadow;
class PageManager;
class ReadbackTracker;

class TextureCache {
    // Default values for garbage collection
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    using ImageIds = boost::container::small_vector<ImageId, 16>;

    struct Traits {
        using Entry = ImageIds;
        static constexpr size_t AddressSpaceBits = 40;
        static constexpr size_t FirstLevelBits = 10;
        static constexpr size_t PageBits = 20;
    };
    using PageTable = MultiLevelPageTable<Traits>;

public:
    enum class BindingType : u32 {
        Texture,
        Storage,
        RenderTarget,
        DepthTarget,
        VideoOut,
    };

    struct ImageDesc {
        ImageInfo info;
        ImageViewInfo view_info;
        BindingType type{BindingType::Texture};

        ImageDesc() = default;
        ImageDesc(const AmdGpu::Image& image, const Shader::ImageResource& desc)
            : info{image, desc}, view_info{image, desc},
              type{desc.is_written ? BindingType::Storage : BindingType::Texture} {}
        ImageDesc(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint)
            : info{buffer, hint}, view_info{buffer}, type{BindingType::RenderTarget} {}
        ImageDesc(const AmdGpu::DepthBuffer& buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl, VAddr htile_address, AmdGpu::CbDbExtent hint,
                  bool write_buffer = false)
            : info{buffer, view.NumSlices(), htile_address, hint, write_buffer},
              view_info{buffer, view, ctl}, type{BindingType::DepthTarget} {}
        ImageDesc(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr cpu_address)
            : info{group, cpu_address}, type{BindingType::VideoOut} {}
    };

    enum class DownloadPolicy : u8 {
        LegacyEager,
        AuthorityManaged,
    };

    enum class DownloadTrigger : u8 {
        EventWriteEos,
        EventWriteEop,
        ReleaseMem,
        ExplicitHostDemand,
        CpuRead,
        Unmap,
        Other,
    };

    struct DownloadContext {
        DownloadTrigger trigger{DownloadTrigger::Other};
        u32 trigger_control{0};
        u32 trigger_data_control{0};
    };

    struct PendingImageDownload {
        ImageId image_id{0};
        u64 image_uid{0};
        u64 resource_version{0};
        VAddr guest_begin{0};
        u32 size{0};
        DownloadPolicy policy{DownloadPolicy::LegacyEager};
    };

    struct PendingFastpathCandidate {
        ImageId image_id{0};
        u64 image_uid{0};
        u64 resource_version{0};
        VAddr guest_addr{0};
        u32 download_size{0};
    };

public:
    TextureCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                 AmdGpu::Liverpool* liverpool, BufferCache& buffer_cache, PageManager& tracker);
    ~TextureCache();

    TileManager& GetTileManager() noexcept {
        return tile_manager;
    }

    /// Invalidates any image in the logical page range.
    void InvalidateMemory(VAddr addr, size_t size);

    /// Marks an image as dirty if it exists at the provided address.
    void InvalidateMemoryFromGPU(VAddr address, size_t max_size);

    /// Evicts any images that overlap the unmapped range.
    void UnmapMemory(VAddr cpu_addr, size_t size);

    /// Schedules a copy of pending images for download back to CPU memory.
    bool ProcessDownloadImages(const DownloadContext& context, bool* gpu_resident = nullptr);

    [[nodiscard]] bool PromotePendingDownloadAuthority(ImageId image_id, u64 image_uid,
                                                        u64 resource_version,
                                                        std::shared_ptr<GpuAuthorityShadow>* shadow);
    void PruneSupersededPendingDownloads(u64 image_uid, u64 superseded_version);
    void ScheduleComputeDownload(ImageId image_id);
    void ScheduleRenderTargetDownload(ImageId image_id);

    [[nodiscard]] std::optional<PendingFastpathCandidate> TakePendingFastpathCandidate();

    [[nodiscard]] bool IsGpuAuthorityImageCurrent(ImageId image_id, u64 image_uid,
                                                  u64 resource_version, VAddr address,
                                                  size_t size);

    void WaitGpuAuthorityShadow(const std::shared_ptr<GpuAuthorityShadow>& shadow);
    bool MaterializeGpuAuthority(const std::shared_ptr<GpuAuthorityShadow>& shadow,
                                 VAddr required_addr, size_t required_size,
                                 s8* out_validation_bytes_equal = nullptr);

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    [[nodiscard]] bool TryReuseImage(ImageId image_id, u64 image_uid, u64 expected_topology_epoch);

    [[nodiscard]] u64 TopologyEpoch() const noexcept {
        return topology_epoch.load(std::memory_order_relaxed);
    }

    /// Retrieves image whose address matches provided
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    /// Retrieves the smallest valid image that fully contains the provided range.
    [[nodiscard]] ImageId FindImageContainingRange(VAddr address, size_t size);

    /// Retrieves an image view with the properties of the specified image id.
    void PrepareTexture(ImageId image_id, BindingType type);

    [[nodiscard]] ImageView& FindTexture(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the render target with specified properties
    void PrepareRenderTarget(ImageId image_id, const ImageDesc& desc);

    [[nodiscard]] ImageView& FindRenderTarget(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the depth target with specified properties
    void PrepareDepthTarget(ImageId image_id, const ImageDesc& desc);

    [[nodiscard]] ImageView& FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    /// Updates image contents if it was modified by CPU.
    void UpdateImage(ImageId image_id);

    /// Resolves overlap between existing cache image and pending merged image
    [[nodiscard]] std::tuple<ImageId, int, int> ResolveOverlap(const ImageInfo& info,
                                                               BindingType binding,
                                                               ImageId cache_img_id,
                                                               ImageId merged_image_id);

    /// Resolves depth overlap and either re-creates the image or returns existing one
    [[nodiscard]] ImageId ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                              ImageId cache_img_id);

    /// Creates a new image with provided image info and copies subresources from image_id
    [[nodiscard]] ImageId ExpandImage(const ImageInfo& info, ImageId image_id);

    /// Reuploads image contents. An image that is about to be overwritten keeps only the
    /// bookkeeping of a refresh.
    void RefreshImage(Image& image, bool overwritten = false);

    /// Retrieves the sampler that matches the provided S# descriptor.
    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base);

    /// Retrieves the image with the specified id.
    [[nodiscard]] Image& GetImage(ImageId id) {
        auto& image = slot_images[id];
        TouchImage(image);
        return image;
    }

    /// Retrieves the image view with the specified id.
    [[nodiscard]] ImageView& GetImageView(ImageId id) {
        return slot_image_views[id];
    }

    /// Get the associated depth stencil image if it is still valid.
    ImageId GetAssociatedDepth(Image& image) {
        if (!image.depth_id) {
            return {};
        }
        if (slot_images.is_allocated(image.depth_id)) {
            auto& depth_image = slot_images[image.depth_id];
            if (depth_image.image_uid == image.depth_uid &&
                depth_image.flags & ImageFlagBits::Registered) {
                return image.depth_id;
            }
        }
        // The linked depth image is no longer valid, disassociate it.
        image.DisassociateDepth();
        return {};
    }

    enum class MetaType {
        CMask,
        FMask,
        HTile,
    };

    /// Returns meta type if the specified address is a metadata surface.
    std::optional<MetaType> IsMeta(VAddr address) const {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it->second.type;
        }
        return std::nullopt;
    }

    /// Returns true if a slice of the specified metadata surface has been cleared.
    bool IsMetaCleared(VAddr address, u32 slice) const {
        const auto& it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it.value().clear_mask & (1u << slice);
        }
        return false;
    }

    /// Returns whether a slice of the specified metadata surface has been cleared and marks it
    /// as not cleared, as IsMetaCleared followed by TouchMeta(address, slice, false) would.
    bool TakeMetaCleared(VAddr address, u32 slice) {
        auto it = surface_metas.find(address);
        if (it == surface_metas.end()) {
            return false;
        }
        auto& clear_mask = it.value().clear_mask;
        const bool cleared = clear_mask & (1u << slice);
        clear_mask &= ~(1u << slice);
        return cleared;
    }

    /// Clears all slices of the specified metadata surface.
    bool ClearMeta(VAddr address) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            it.value().clear_mask = u32(-1);
            return true;
        }
        return false;
    }

    /// Updates the state of a slice of the specified metadata surface.
    bool TouchMeta(VAddr address, u32 slice, bool is_clear) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            if (is_clear) {
                it.value().clear_mask |= 1u << slice;
            } else {
                it.value().clear_mask &= ~(1u << slice);
            }
            return true;
        }
        return false;
    }

    /// Runs the garbage collector.
    void RunGarbageCollector();

    template <typename Func>
    void ForEachImageInRegion(VAddr cpu_addr, size_t size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        ImageIds images;
        ForEachPage(cpu_addr, size, [this, &images, cpu_addr, size, func](u64 page) {
            const auto it = page_table.find(page);
            if (it == nullptr) {
                if constexpr (BOOL_BREAK) {
                    return false;
                } else {
                    return;
                }
            }
            for (const ImageId image_id : *it) {
                Image& image = slot_images[image_id];
                if (image.flags & ImageFlagBits::Picked) {
                    continue;
                }
                if (!image.Overlaps(cpu_addr, size)) {
                    continue;
                }
                image.flags |= ImageFlagBits::Picked;
                images.push_back(image_id);
                if constexpr (BOOL_BREAK) {
                    if (func(image_id, image)) {
                        return true;
                    }
                } else {
                    func(image_id, image);
                }
            }
            if constexpr (BOOL_BREAK) {
                return false;
            }
        });
        for (const ImageId image_id : images) {
            slot_images[image_id].flags &= ~ImageFlagBits::Picked;
        }
    }

private:
    struct AliasState;
    enum class AliasAccess {
        Read,
        ReadWrite,
    };

    ImageId CreateStencilImage(const ImageDesc& desc);
    vk::Sampler TouchSampler(Sampler& entry);
    vk::Sampler CreateSampler(u64 hash, const AmdGpu::Sampler& sampler,
                              AmdGpu::BorderColorBuffer border_color_base);

    void PrepareImageAccess(ImageId image_id, AliasAccess access);
    void UpdateImageImpl(ImageId image_id);
    void ScheduleImageDownload(ImageId image_id, bool fastpath_candidate, bool replace_existing);

    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachPage(PAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> Traits::PageBits;
        for (u64 page = addr >> Traits::PageBits; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    /// Copies image memory back to CPU.
    bool DownloadImageMemory(ImageId image_id, bool validate_identity = false,
                             bool track_gpu_source = false, bool* gpu_resident = nullptr);

    /// Thread function for copying downloaded images out to CPU memory.
    void DownloadedImagesThread(const std::stop_token& token);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, VAddr cpu_addr);

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Track CPU reads and writes for image
    void TrackImage(ImageId image_id);
    void TrackImageHead(ImageId image_id);
    void TrackImageTail(ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageId image_id);
    void UntrackImageHead(ImageId image_id);
    void UntrackImageTail(ImageId image_id);

    void MarkAsMaybeDirty(ImageId image_id, Image& image);

    void InvalidateAlias(Image& image);
    void SynchronizeAlias(ImageId image_id);
    [[nodiscard]] std::optional<Extent3D> ResolveAliasCopy(ImageId image_id, AliasState& state);
    [[nodiscard]] bool CommitAliasWriter(AliasState& state);
    void CopyAlias(ImageId src_id, ImageId dst_id, const Extent3D& extent);
    void PublishAliasWrite(ImageId image_id);

    template <typename Func>
    void ForEachAlias(const Image& image, Func&& func);

    [[nodiscard]] bool IsLiveImage(ImageId image_id, u64 image_uid) const {
        return image_id && slot_images.is_allocated(image_id) &&
               slot_images[image_id].image_uid == image_uid;
    }

    /// Removes the image and any views/surface metas that reference it.
    void DeleteImage(ImageId image_id);

    /// Touch the image in the LRU cache at most once per GC tick.
    void TouchImage(Image& image) {
        if (image.lru_tick != gc_tick) [[unlikely]] {
            TouchImageSlow(image);
        }
    }

    void TouchImageSlow(Image& image);

    void FreeImage(ImageId image_id) {
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id);
    }

    void GarbageCollectImages();
    void GarbageCollectSamplers();

private:
    struct ExactImageCacheKey {
        std::array<u64, 6> words{};
    };
    static_assert(sizeof(ExactImageCacheKey) == 48);

    struct ExactImageCacheEntry {
        ExactImageCacheKey key{};
        ImageId image_id{};
        u64 image_uid{};
        u64 topology_epoch{};
        bool valid{};
    };

    ImageId FindImageSlow(ImageDesc& desc, bool exact_fmt, const ExactImageCacheKey& exact_key,
                          ExactImageCacheEntry& exact_entry, size_t cache_index);

    static constexpr size_t ExactImageCacheSize = 256;
    static_assert(std::has_single_bit(ExactImageCacheSize));

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    BufferCache& buffer_cache;
    PageManager& tracker;
    std::shared_ptr<ReadbackTracker> readback_tracker;
    BlitHelper blit_helper;
    TileManager tile_manager;
    /// Outlives the images, which return their Vulkan images to it.
    ImageRecycler image_recycler;
    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageView> slot_image_views;
    tsl::robin_map<u64, Sampler, IntegerKeyHash> samplers;
    std::vector<PendingImageDownload> pending_downloads;
    struct AliasState {
        // Backing contains the complete shared-memory view; writer is an uncommitted write.
        u64 backing_uid{};
        u64 writer_uid{};
        u64 download_uid{};
        ImageId backing{};
        ImageId writer{};
        ImageId download{};
        u32 members{};

        void ResetAuthority() {
            const u32 member_count = members;
            *this = {};
            members = member_count;
        }
    };
    tsl::robin_map<VAddr, AliasState, IntegerKeyHash> alias_states;
    boost::container::small_vector<VAddr, 4> pending_alias_downloads;
    u64 alias_generation{};
    u64 total_used_memory = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 total_used_samplers = 0;
    u64 trigger_gc_samplers = 0;
    u64 pressure_gc_samplers = 0;
    u64 critical_gc_samplers = 0;
    u64 gc_tick = 0;
    std::atomic<u64> topology_epoch{1};
    std::array<ExactImageCacheEntry, ExactImageCacheSize> exact_image_cache{};
    std::array<ExactImageCacheEntry, ExactImageCacheSize> exact_image_cache_victim{};
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru_cache;
    bool readback_linear_images;
    PageTable page_table;
    std::recursive_mutex mutex;
    std::mutex samplers_mutex;
    std::mutex download_images_mutex;
    mutable std::mutex fastpath_candidate_mutex;
    std::optional<PendingFastpathCandidate> pending_fastpath_candidate;
    struct MetaDataInfo {
        MetaType type;
        s32 clear_mask = -1;
    };
    tsl::robin_map<VAddr, MetaDataInfo, IntegerKeyHash> surface_metas;
};

} // namespace VideoCore
