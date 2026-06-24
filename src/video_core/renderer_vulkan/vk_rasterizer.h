// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <vector>
#include <boost/container/small_vector.hpp>
#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/texture_cache.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {

class Scheduler;
class RenderState;
class GraphicsPipeline;

struct Gow3Features {
    static bool storage_image_sync;
    static bool async_storage_download;
    static bool rt_alias_copy;
    static bool _1x1_readback;
    static bool depth_clear_skip;

    static void Init();
};

class StorageImageSync {
public:
    StorageImageSync(Scheduler& scheduler, VideoCore::BufferCache& buffer_cache,
                     VideoCore::TextureCache& texture_cache);
    ~StorageImageSync();

    /// Clear the per-dispatch collection. Call before each BindResources.
    void Clear();

    /// Collect a storage image for post-dispatch sync. Deduplicates by guest_addr.
    void Collect(VideoCore::ImageId image_id, const VideoCore::Image& image);

    /// Record vkCmdCopyImageToBuffer for collected storage images.
    /// @param async  If true, submit without waiting (defer to ProcessPending).
    ///               If false, wait immediately and inject data into buffer cache.
    void RecordDownload(bool async);

    /// Process all pending async downloads: Wait → InsertGpuData → InvalidateMemoryRange.
    void ProcessPending();

    /// Discard collected syncs without processing (for HLE interception).
    void DiscardPending();

    /// True if there are collected syncs not yet recorded.
    bool HasPending() const;

    /// Number of collected syncs.
    u32 PendingCount() const;

    /// Register the force-sync callback on the buffer cache.
    /// Only call when async mode is enabled.
    void InstallPreAccessCallback();

private:
    struct PendingSync {
        VideoCore::ImageId image_id;
        VAddr guest_addr;
        u64 guest_size;
        u32 width, height;
        u32 layers;
        u32 num_samples, num_mips;
        u32 pitch;
        vk::Format pixel_format;
    };

    struct StagingRef {
        const u8* data;
        u32 size;
    };

    struct PendingDownload {
        u64 tick;
        boost::container::small_vector<StagingRef, 4> staging_refs;
        std::vector<PendingSync> syncs;
    };

    Scheduler& scheduler;
    VideoCore::BufferCache& buffer_cache;
    VideoCore::TextureCache& texture_cache;

    std::vector<PendingSync> pending_syncs;
    std::vector<PendingDownload> pending_downloads;
    std::mutex pending_mutex;
    VideoCore::RangeSet pending_ranges;
};

class Rasterizer {
public:
    explicit Rasterizer(const Instance& instance, Scheduler& scheduler,
                        AmdGpu::Liverpool* liverpool);
    ~Rasterizer();

    [[nodiscard]] Scheduler& GetScheduler() noexcept {
        return scheduler;
    }

    [[nodiscard]] const Instance& GetInstance() const noexcept {
        return instance;
    }

    [[nodiscard]] VideoCore::BufferCache& GetBufferCache() noexcept {
        return buffer_cache;
    }

    [[nodiscard]] VideoCore::TextureCache& GetTextureCache() noexcept {
        return texture_cache;
    }

    void Draw(bool is_indexed, u32 index_offset = 0);
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address);

    void DispatchDirect();
    void DispatchIndirect(VAddr address, u32 offset, u32 size, bool on_gpu);

    void ScopeMarkerBegin(const std::string_view& str, bool from_guest = false);
    void ScopeMarkerEnd(bool from_guest = false);
    void ScopedMarkerInsert(const std::string_view& str, bool from_guest = false);
    void ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                 bool from_guest = false);

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);
    u32 ReadDataFromGds(u32 gsd_offset);
    bool InvalidateMemory(VAddr addr, u64 size);
    bool ReadMemory(VAddr addr, u64 size);
    void ProcessDownloadImages();
    bool IsMapped(VAddr addr, u64 size);
    void MapMemory(VAddr addr, u64 size);
    void UnmapMemory(VAddr addr, u64 size);

    void CpSync();
    u64 Flush();
    void Finish();

    void OnSubmit();
    void CommitPendingGpuRanges();

    PipelineCache& GetPipelineCache() {
        return pipeline_cache;
    }
    VideoCore::ImageId GetCurrentColorBuffer(u32 index = 0) const;

    template <typename Func>
    void ForEachMappedRangeInRange(VAddr addr, u64 size, Func&& func) {
        const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (const auto& mapped_range : (mapped_ranges & range)) {
            func(mapped_range);
        }
    }

private:
    void PrepareRenderState(const GraphicsPipeline* pipeline);
    RenderState BeginRendering(const GraphicsPipeline* pipeline);
    void Resolve();
    void DepthStencilCopy(bool is_depth, bool is_stencil);
    void EliminateFastClear();

    void UpdateDynamicState(const GraphicsPipeline* pipeline, bool is_indexed) const;
    void UpdateViewportScissorState() const;
    void UpdateDepthStencilState() const;
    void UpdatePrimitiveState(bool is_indexed) const;
    void UpdateRasterizationState() const;
    void UpdateColorBlendingState(const GraphicsPipeline* pipeline) const;

    bool FilterDraw();

    void BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                     Shader::PushData& push_data);
    void BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding);
    bool BindResources(const Pipeline* pipeline);

    void ResetBindings() {
        for (auto& image_id : bound_images) {
            texture_cache.GetImage(image_id).binding = {};
        }
        bound_images.clear();
    }

    bool IsComputeMetaClear(const Pipeline* pipeline);
    bool IsComputeImageCopy(const Pipeline* pipeline);
    bool IsComputeImageClear(const Pipeline* pipeline);

private:
    friend class VideoCore::BufferCache;

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::PageManager page_manager;
    VideoCore::BufferCache buffer_cache;
    VideoCore::TextureCache texture_cache;
    StorageImageSync storage_sync_;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    boost::icl::interval_set<VAddr> mapped_ranges;
    Common::SharedFirstMutex mapped_ranges_mutex;
    PipelineCache pipeline_cache;

    using RenderTargetInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    std::array<RenderTargetInfo, AmdGpu::NUM_COLOR_BUFFERS> cb_descs;
    std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc> db_desc;
    boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> image_infos;
    boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS> buffer_infos;
    boost::container::static_vector<VideoCore::ImageId, Shader::NUM_IMAGES> bound_images;

    u32 set_write_index{};
    Pipeline::DescriptorWrites set_writes;
    Pipeline::BufferBarriers buffer_barriers;
    Shader::PushData push_data;

    using BufferBindingInfo = std::tuple<VideoCore::BufferId, AmdGpu::Buffer, u64>;
    boost::container::static_vector<BufferBindingInfo, Shader::NUM_BUFFERS> buffer_bindings;
    using ImageBindingInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;
    bool fault_process_pending{};
    bool attachment_feedback_loop{};
};

} // namespace Vulkan