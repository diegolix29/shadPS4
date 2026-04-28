// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_set>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <queue>
#include <tsl/robin_map.h>

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
class PageManager;

class TextureCache {
    // Default values for garbage collection
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;


    using ImageIds = boost::container::small_vector<ImageId, 64>;

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
    void ProcessDownloadImages();

    /// Synchronous GPU→CPU download for the photo render target.
    /// Called from the game thread (sceJpegEncEncode) — does NOT use SendCommand or the
    /// scheduler's command buffer, avoiding deadlocks. Instead, it uses a dedicated one-shot
    /// Vulkan command pool/buffer/fence to copy the last known 1024×1024 linear render target
    /// directly to the encoder's CPU buffer at |address|.
    /// Returns true if an image was found and successfully downloaded.
    bool ForceDownloadByAddress(VAddr address, u64 size);

    /// Triggers texture survey logging for the next N texture bindings.
    /// Called after photo encode to discover what the game samples for preview.
    static void TriggerPhotoTextureSurvey(int budget = 200);

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    struct FindImageWithViewResult {
        ImageId image_id{};
        int view_mip{-1};
        int view_slice{-1};
    };

    /// Same as FindImage(), but does not mutate the provided ImageDesc.
    /// Returns the subresource location (mip/slice) when the requested image is a subresource.
    [[nodiscard]] FindImageWithViewResult FindImageWithView(const ImageDesc& desc,
                                                            bool exact_fmt = false,
                                                            bool touch_lru = true);
    [[nodiscard]] bool ValidateCachedFindImage(const ImageDesc& desc, ImageId image_id,
                                               bool exact_fmt = false) const;

    /// Retrieves image whose address matches provided
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    /// Retrieves an image view with the properties of the specified image id.
    /// Retrieves an image view with the properties of the specified image id, using a
    /// lightweight descriptor (binding type + view info).
    [[nodiscard]] ImageView& FindTexture(ImageId image_id, BindingType type,
                                         const ImageViewInfo& view_info);

    [[nodiscard]] ImageView& FindTexture(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the render target with specified properties
    [[nodiscard]] ImageView& FindRenderTarget(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the depth target with specified properties
    [[nodiscard]] ImageView& FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    /// Updates image contents if it was modified by CPU.
    ///
    /// PERF(GR2): This is hit *a lot* in GpuComm via FindTexture/FindRenderTarget.
    /// Use a shared-lock fast path to avoid write-lock churn on already-tracked, clean images.
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

    /// Reuploads image contents.
    void RefreshImage(Image& image);

    /// Retrieves the sampler that matches the provided S# descriptor.
    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base);

    /// Retrieves the image with the specified id.
    [[nodiscard]] Image& GetImage(ImageId id) {
        auto& image = slot_images[id];
        TouchImage(image);
        return image;
    }

    /// Retrieves the image WITHOUT updating LRU cache.
    /// Use only when the caller just needs to clear transient state (e.g. binding flags)
    /// and does not constitute a "real" access that should keep the image alive.
    [[nodiscard]] Image& GetImageUntouched(ImageId id) {
        return slot_images[id];
    }

    /// Retrieves the image view with the specified id.
    [[nodiscard]] ImageView& GetImageView(ImageId id) {
        return slot_image_views[id];
    }

    /// Returns the total VRAM currently used by tracked images, in bytes.
    [[nodiscard]] u64 GetTotalUsedMemory() const noexcept {
        return total_used_memory;
    }

    /// Returns true if the specified address is a metadata surface.
    bool IsMeta(VAddr address) const {
        return surface_metas.contains(address);
    }

    /// Returns true if a slice of the specified metadata surface has been cleared.
    bool IsMetaCleared(VAddr address, u32 slice) const {
        const auto& it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it.value().clear_mask & (1u << slice);
        }
        return false;
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

    /// Gets or creates a null image for a particular format.
    ImageId GetNullImage(vk::Format format);

    /// Copies image memory back to CPU.
    void DownloadImageMemory(ImageId image_id);

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

    /// Removes the image and any views/surface metas that reference it.
    void DeleteImage(ImageId image_id);

    /// Touch the image in the LRU cache.
    void TouchImage(const Image& image);

    void FreeImage(ImageId image_id) {
        Image& image = slot_images[image_id];
        if (False(image.flags & ImageFlagBits::Registered)) {
            // Already freed through another path (e.g., address conflict resolution
            // followed by GC eviction). Skip to avoid double-free.
            return;
        }
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id);
    }

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    BufferCache& buffer_cache;
    PageManager& tracker;
    BlitHelper blit_helper;
    TileManager tile_manager;
    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageView> slot_image_views;
    tsl::robin_map<u64, Sampler> samplers;
    tsl::robin_map<vk::Format, ImageId> null_images;
    std::unordered_set<ImageId> download_images;
    u64 total_used_memory = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 gc_tick = 0;
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    PageTable page_table;
    mutable std::shared_mutex mutex;
    struct MetaDataInfo {
        enum class Type {
            CMask,
            FMask,
            HTile,
        };
        Type type;
        s32 clear_mask = -1;
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;

    // ── GR2 Photo Download (one-shot Vulkan resources, game-thread owned) ──────
    // These are lazily created on first ForceDownloadByAddress call and used
    // exclusively from the game thread, so no locking is needed for them.
    vk::CommandPool photo_cmd_pool_{};
    vk::CommandBuffer photo_cmd_buf_{};
    vk::Fence photo_fence_{};
    vk::Buffer photo_staging_buf_{};
    vk::DeviceMemory photo_staging_mem_{};
    void* photo_staging_ptr_{nullptr};
    u32 photo_staging_size_{0};
    bool photo_resources_init_{false};

    // Tracks the most recently bound 1024×1024 linear render target.
    // Written on the GPU thread (FindRenderTarget), read on the game thread
    // (ForceDownloadByAddress). Using atomic for safe cross-thread access.
    struct PhotoRTInfo {
        ImageId image_id{};
        u64 stamp{0}; // monotonic stamp to detect staleness
    };
    std::mutex photo_rt_mutex_;
    PhotoRTInfo last_photo_rt_{};
    std::atomic<u64> photo_rt_stamp_{0};

    // ── GR2 Photo Snapshot (preserves photo pixels across RT reuse) ──────────
    // After ForceDownloadByAddress copies photo pixels to CPU, we save a copy
    // here. If the game later samples the same address as a texture but the RT
    // has been overwritten (GpuModified), we re-upload the snapshot pixels
    // before the texture is sampled. This mimics real PS4 behavior where the
    // hardware encoder doesn't disturb the render target.
    struct PhotoSnapshot {
        VAddr guest_address{0};       // Guest address of the photo RT
        ImageId image_id{};           // Image ID at time of capture
        std::vector<u8> pixels;       // Saved BGRA pixel data
        u32 width{0};
        u32 height{0};
        u32 pitch{0};
        u32 bpp{0};
        bool valid{false};            // True after successful capture
        u64 capture_stamp{0};         // For staleness detection
        int restore_count{0};         // How many times we've restored
    };
    std::mutex photo_snapshot_mutex_;
    PhotoSnapshot photo_snapshot_{};

    /// Called after ForceDownloadByAddress to save photo pixels
    void SavePhotoSnapshot(VAddr address, ImageId image_id, u32 width, u32 height,
                           u32 pitch, u32 bpp);

    /// Called from FindTexture when the game samples the photo address.
    /// If the RT has been overwritten, restores the snapshot pixels.
    bool MaybeRestorePhotoSnapshot(ImageId image_id, Image& image);
};

} // namespace VideoCore
