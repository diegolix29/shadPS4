// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
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

class Rasterizer {
public:
    explicit Rasterizer(const Instance& instance, Scheduler& scheduler,
                        AmdGpu::Liverpool* liverpool);
    ~Rasterizer();

    [[nodiscard]] Scheduler& GetScheduler() noexcept {
        return scheduler;
    }

    [[nodiscard]] VideoCore::BufferCache& GetBufferCache() noexcept {
        return buffer_cache;
    }

    [[nodiscard]] VideoCore::TextureCache& GetTextureCache() noexcept {
        return texture_cache;
    }

    [[nodiscard]] const Instance& GetInstance() const noexcept {
        return instance;
    }

    void Draw(bool is_indexed, u32 index_offset = 0);
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address);

    void DispatchDirect();
    void DispatchIndirect(VAddr address, u32 offset, u32 size);

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
    bool IsMapped(VAddr addr, u64 size);
    void MapMemory(VAddr addr, u64 size);
    void UnmapMemory(VAddr addr, u64 size);

    void CpSync();
    u64 Flush();
    void Finish();
    void OnSubmit();

    PipelineCache& GetPipelineCache() {
        return pipeline_cache;
    }

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
            // OPT: Use GetImageUntouched to avoid LRU touch overhead.
            // ResetBindings only clears transient binding flags and doesn't constitute
            // a "real" image access that should keep the image alive in cache.
            texture_cache.GetImageUntouched(image_id).binding = {};
        }
        bound_images.clear();
    }

    bool IsComputeMetaClear(const Pipeline* pipeline);
    bool IsComputeImageCopy(const Pipeline* pipeline);
    bool IsComputeImageClear(const Pipeline* pipeline);

    /// Bind pipeline with deduplication — skips vkCmdBindPipeline when already bound.
    void BindPipelineCached(vk::PipelineBindPoint bind_point, vk::Pipeline pipeline);

private:
    friend class VideoCore::BufferCache;

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::PageManager page_manager;
    VideoCore::BufferCache buffer_cache;
    VideoCore::TextureCache texture_cache;
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
    Pipeline::DescriptorWrites set_writes;
    Pipeline::BufferBarriers buffer_barriers;
    Shader::PushData push_data;

    // OPT#4: Descriptor delta cache (skip redundant push-descriptor writes).
    struct DescCacheEntry {
        u64 a{};           // buffer/sampler handle (packed as u64)
        u64 b{};           // offset/imageView handle
        u64 c{};           // range/layout
        u32 epoch{};       // validity epoch
        vk::DescriptorType type{};
    };

    // Invalidate cache when cmd buffer or pipeline changes.
    mutable vk::CommandBuffer desc_cache_cmdbuf{};
    mutable u64 desc_cache_tick{};
    mutable vk::PipelineLayout desc_cache_layout{};
    mutable const Pipeline* desc_cache_pipeline{};
    mutable u32 desc_cache_epoch{1};
    // PERF(GR2): Cover multi-stage unified descriptor bindings.
    // The previous cap only covered ~single-stage bindings, so higher unified bindings bypassed
    // Rasterizer::ShouldWriteDescriptor() and were re-emitted every draw (extra push-descriptor
    // CPU + __memmove in RADV). This cache is keyed by command buffer tick + pipeline/layout and
    // epoch-invalidated, so enlarging the coverage is safe and avoids those uncached writes.
    static constexpr u32 DescCacheBindingsCap = 1024;

    // Indexed by dstBinding (binding.unified index). Fixed-capacity avoids resize/zero-fill on hot path.
    mutable std::array<DescCacheEntry, DescCacheBindingsCap> desc_cache{};
    // (ULTRA MAX) Removed OPT#5 guest-signature cache. It double-worked the hot path and could be unsafe.

    void PrepareDescriptorDeltaCache(const Pipeline* pipeline);
    bool ShouldWriteDescriptor(u32 binding, vk::DescriptorType type, u64 a, u64 b, u64 c);


    // OPT: buffer_bindings removed — BindBuffers is now single-pass.
    struct ImageBindingInfo {
        VideoCore::ImageId image_id{};
        const VideoCore::TextureCache::ImageDesc* desc{}; // Cached base descriptor (info+view_info+type)
        s16 view_mip{-1};
        s16 view_slice{-1};
        // FIX(GR2FORK): PORT(upstream #4075) mip-fallback descriptor arrays.
        // When the shader's ImageResource has MipStorageFallbackMode::DynamicIndex,
        // the descriptor set layout reserves N slots (one per mip level). The
        // rasterizer must emit N image views + N descriptor array entries
        // here, otherwise Index>=1 stays uninitialized and a compute dispatch
        // that samples lod=1 reads a null descriptor -> RADV device-lost.
        // Validation flags this as VUID-vkCmdDispatch-None-08114 on cs_img16.
        // num_bindings is 1 for the common case; >1 only for DynamicIndex images.
        u8 num_bindings{1};
    };
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;

    // OPT(v14.1 hotfix): BindTextures caches stored on the Rasterizer instance (not TLS).
    struct CachedImageDescEntry {
        u64 key{};
        AmdGpu::Image image{};
        VideoCore::TextureCache::ImageDesc desc{};
        bool valid{false};
    };
    // PERF(GR2): 256 direct-mapped slots collides heavily across pipelines/stages.
    // A larger cache cuts ImageDesc rebuilds (ImageInfo::UpdateSize etc.).
    std::array<CachedImageDescEntry, 4096> image_desc_cache_{};

    struct LocalFindImageCacheEntry {
        const VideoCore::TextureCache::ImageDesc* base{};
        VideoCore::TextureCache::FindImageWithViewResult res{};
        bool valid{false};
    };
    // PERF(GR2): Increase intra-call FindImageWithView de-dup window.
    std::array<LocalFindImageCacheEntry, 512> find_image_cache_{};

    struct PersistentFindImageCacheEntry {
        u64 key{};
        const VideoCore::TextureCache::ImageDesc* base{};
        VideoCore::TextureCache::FindImageWithViewResult res{};
        bool valid{false};
    };
    // Cross-call cache to skip repeated FindImageWithView lock/page-table scans for stable descriptors.
    std::array<PersistentFindImageCacheEntry, 4096> find_image_pcache_{};

    // PERF(GR2 v17): Epoch-based validity for BindTextures caches.
    // Instead of zero-initializing 640+ bytes of stamp arrays on the stack every BindTextures call,
    // use a monotonically increasing epoch counter. Entries are valid iff their stamp matches the
    // current epoch. This eliminates the per-call memset overhead.
    u32 bind_textures_epoch_{};
    std::array<u32, 512> find_image_cache_stamp_{}; // was local u8[512] zeroed every call
    std::array<u32, 128> find_texture_cache_stamp_{}; // was local u8[128] zeroed every call
    struct FindTextureCacheEntry {
        VideoCore::ImageId image_id{};
        VideoCore::TextureCache::BindingType type{};
        VideoCore::ImageView* view{};
    };
    std::array<FindTextureCacheEntry, 128> find_texture_cache_{};

    // =========================================================================
    // Render target identity cache
    // =========================================================================
    // PrepareRenderState calls texture_cache.FindImage() per CB/DB every draw.
    // FindImage involves page table walks + hash lookups. But render targets
    // rarely change between draws (they change on render pass switches).
    // Cache the FindImage results keyed on CB/DB addresses.
    struct RenderTargetCache {
        u64 hash{};
        std::array<VideoCore::ImageId, AmdGpu::NUM_COLOR_BUFFERS> cb_image_ids{};
        VideoCore::ImageId db_image_id{};
        bool valid{};
    } rt_cache_{};

    // =========================================================================
    // Per-stage binding skip cache
    // =========================================================================
    // When the same pipeline draws consecutive geometry with identical user_data
    // (same textures, same buffers — only transforms/push constants differ),
    // we can skip the entire BindBuffers + BindTextures iteration (~500 lines
    // of T# reads, cache lookups, descriptor construction) since
    // ShouldWriteDescriptor would filter out all writes anyway.
    //
    // Keyed on {pipeline pointer, user_data hash, cmdbuf tick} per stage.
    struct StageBindingCacheEntry {
        u64 ud_hash{};
        u64 pgm_hash{};
    };
    struct BindingSkipState {
        const Pipeline* pipeline{};
        u64 cmdbuf_tick{};
        bool uses_dma{};
        std::array<StageBindingCacheEntry, 6> stages{}; // MaxShaderStages
        bool valid{};
    } binding_skip_cache_{};

    bool fault_process_pending{};
    bool attachment_feedback_loop{};

    // PopPendingOperations batching: only check every 16th draw.
    mutable u32 draw_counter_{};

    // FilterDraw cache: skip re-checking when pipeline hasn't changed.
    mutable const GraphicsPipeline* last_filter_pipeline_{};
    mutable bool last_filter_result_{true};

    // OPT: Pipeline bind deduplication. Consecutive draws with the same pipeline
    // skip the redundant vkCmdBindPipeline call (significant driver overhead on RADV/ANV).
    mutable vk::Pipeline last_bound_pipeline_{};
    mutable vk::CommandBuffer last_bound_pipeline_cmdbuf_{};
    mutable u64 last_bound_pipeline_tick_{};

    // PERF(GR2 v17): Persistent UpdateImage de-dup cache.
    // UpdateImage takes a shared_lock even to discover "nothing to do" (most common case).
    // By caching {image_id, tick} across draws within the same command buffer tick,
    // we skip redundant lock acquire/release for images that were already checked this tick.
    struct UpdateImageCacheEntry {
        VideoCore::ImageId image_id{};
        u64 tick{};
    };
    std::array<UpdateImageCacheEntry, 256> update_image_cache_{};

    // =========================================================================
    // Sampler Resolution Fast-Path Cache
    // =========================================================================
    // GetSampler() calls XXH3_64bits on the 64-byte Sampler struct + does an
    // unordered_map lookup every time. GR2 typically has 4-8 unique samplers
    // that repeat across hundreds of draws. A 64-slot direct-mapped cache here
    // avoids both the hash and the map lookup on hit (>90% hit rate in steady
    // state). Samplers are immutable Vulkan objects so caching handles is safe.
    // Cache miss falls through to the existing GetSampler() path.
    struct SamplerCacheEntry {
        u64 hash{};
        vk::Sampler sampler{};
        bool valid{false};
    };
    static constexpr u32 SamplerCacheSize = 64;
    std::array<SamplerCacheEntry, SamplerCacheSize> sampler_cache_{};
};

} // namespace Vulkan
