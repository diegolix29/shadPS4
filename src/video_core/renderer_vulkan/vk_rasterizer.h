// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bit>
#include <limits>
#include <memory>

#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/pm4_cmds.h"
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
    /// Called after InvalidateMemory handled a CPU write fault.
    void OnCpuWriteFault(VAddr addr);
    bool ReadMemory(VAddr addr, u64 size, void* context = nullptr);
    bool HandleWriteFaultOnReadWatchedPage(VAddr addr, u64 size, void* context);
    void ArmSemanticReadWatch(VAddr addr, u64 size);
    void DisarmSemanticReadWatch(VAddr addr, u64 size);
    VideoCore::MemoryWriteNotifyResult NotifyMemoryWrite(VAddr addr, u64 size,
                                                         VideoCore::MemoryWriteSource source);
    [[nodiscard]] VideoCore::MemoryWriteWatch ArmMemoryWriteWatch(
        VAddr addr, VideoCore::MemoryWriteCallback callback, void* user_data) {
        return page_manager.ArmWriteWatch(addr, callback, user_data);
    }
    bool CancelMemoryWriteWatch(VideoCore::MemoryWriteWatch watch) {
        return page_manager.CancelWriteWatch(watch);
    }
    bool ProcessDownloadImages(const VideoCore::TextureCache::DownloadContext& context,
                               bool* gpu_resident = nullptr);
    void WaitTick(u64 tick);
    void DeferGpuCompletion(Common::UniqueFunction<void>&& callback);
    /// Runs callback once the GPU completes the work recorded up to gpu_tick.
    void DeferGpuCompletionAt(u64 gpu_tick, Common::UniqueFunction<void>&& callback);
    bool IsMapped(VAddr addr, u64 size);
    void MapMemory(VAddr addr, u64 size);
    void UnmapMemory(VAddr addr, u64 size);

    void AcquireMemory(u32 cp_coher_cntl, VAddr base_address, u64 size);
    void FlushCaches(AmdGpu::EventType event_type);

    void CpSync();
    void GpuFenceWait();
    void FullGpuBarrier();
    [[nodiscard]] u64 CurrentTick() const noexcept;
    [[nodiscard]] u64 KnownGpuTick() const noexcept;
    /// Returns true on the command processor thread, the only one that records GPU work.
    [[nodiscard]] bool IsGpuThread() const noexcept;
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

    void StartOcclusionQuery(VAddr addr, s32 num_counter_pairs);
    void EndOcclusionQuery(VAddr addr, s32 num_counter_pairs);

private:
    static constexpr u32 MAX_OCCLUSION_QUERIES = 2048;

    struct PendingOcclusionReadback {
        VAddr address;
        u32 index;
        u64 submit_tick;
        s32 counter_pairs;
    };

    vk::QueryPool occlusion_query_pool{};
    std::unordered_map<VAddr, u32> occlusion_index_mapping;
    std::array<VAddr, MAX_OCCLUSION_QUERIES> occlusion_slot_owner{};
    std::deque<PendingOcclusionReadback> pending_occlusion_readbacks;
    u32 occlusion_current_index = 0;

    void InitializeQueryPool();
    void DestroyQueryPool();
    void WriteOcclusionQueryResult(VAddr address, u64 value, s32 num_counter_pairs);
    void ProcessPendingOcclusionReadbacks();

    void PrepareRenderState(const GraphicsPipeline* pipeline);
    /// Finds the image of a render target whose cached image cannot be reused, rebuilding the
    /// description when the registers changed.
    VideoCore::ImageId FindColorTarget(u32 cb, bool same_desc);
    VideoCore::ImageId FindDepthTarget(VAddr htile_address, bool same_desc);
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
    bool BindSquarePass(const GraphicsPipeline& pipeline);

    void PrepareBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding);
    void FinalizeBuffers(Shader::PushData& push_data, bool stream_only, u32 first_binding = 0);
    void BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding);
    bool BindResources(const Pipeline* pipeline);
    void SynchronizeDmaBuffers();
    /// Records a guest cache flush without a barrier (see FlushEpoch).
    void AccumulateFlush(vk::PipelineStageFlags2 src_stages, vk::AccessFlags2 src_access,
                         vk::PipelineStageFlags2 dst_stages, vk::AccessFlags2 dst_access);
    /// Global barrier for the accesses resource tracking cannot see, through device addresses.
    void EmitPendingGlobalBarrier();
    void BindPipelineResources(const Pipeline* pipeline);
    void CaptureDescriptorState(const Pipeline* pipeline);
    void MarkImageWrites(bool is_compute);

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
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    boost::icl::interval_set<VAddr> mapped_ranges;
    Common::SharedFirstMutex mapped_ranges_mutex;
    PipelineCache pipeline_cache;

    using RenderTargetInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    std::array<RenderTargetInfo, AmdGpu::NUM_COLOR_BUFFERS> cb_descs;
    std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc> db_desc;
    struct CachedColorTarget {
        AmdGpu::ColorBuffer buffer{};
        u32 hint{};
        VideoCore::ImageId image_id{};
        u64 image_uid{};
        u64 topology_epoch{};
    };
    std::array<CachedColorTarget, AmdGpu::NUM_COLOR_BUFFERS> cached_color_targets{};
    struct CachedDepthTarget {
        AmdGpu::DepthBuffer buffer{};
        AmdGpu::DepthView view{};
        AmdGpu::DepthControl control{};
        VAddr htile_address{};
        u32 hint{};
        VideoCore::ImageId image_id{};
        u64 image_uid{};
        u64 topology_epoch{};
    } cached_depth_target{};
    boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> image_infos;
    boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS> buffer_infos;
    boost::container::static_vector<VideoCore::ImageId, Shader::NUM_IMAGES> bound_images;
    boost::container::static_vector<VideoCore::ImageId, Shader::NUM_IMAGES>
        potential_write_images;

    u32 set_write_index{};
    Pipeline::DescriptorWrites set_writes;
    Pipeline::DescriptorWrites partial_set_writes;
    Pipeline::BufferBarriers buffer_barriers;
    Shader::PushData push_data;

    struct PendingBufferBinding {
        const Shader::BufferResource* desc{};
        VideoCore::BufferId buffer_id{};
        AmdGpu::Buffer sharp{};
        u64 size{};
        u64 alignment{};
        u32 unified_binding{};
        u32 buffer_binding{};
        u32 set_write_index{};
        u16 stream_index{std::numeric_limits<u16>::max()};
        VideoCore::BufferCache::StreamCopySource stream_source{};
    };
    boost::container::static_vector<PendingBufferBinding, Shader::NUM_BUFFERS>
        pending_buffer_bindings;
    boost::container::static_vector<u8, Shader::NUM_BUFFERS> stream_buffer_bindings;
    /// FinalizeBuffers of a binding that no stream copy serves: special buffers and buffers of
    /// the cache.
    void FinalizeCachedBuffer(Shader::PushData& push_data, PendingBufferBinding& pending);
    void WriteBufferDescriptor(const PendingBufferBinding& pending);
    struct ImageBindingInfo {
        VideoCore::ImageId image_id{};
        VideoCore::ImageViewInfo view_info{};
        u8 source_index{};
        u8 mip_index{};
        bool is_storage{};
    };
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;

    struct CachedBufferBinding {
        const Shader::Info* owner{};
        VAddr address{};
        VideoCore::BufferId buffer_id{};
        u64 buffer_uid{};
        u64 topology_epoch{};
        u64 size{};
        bool valid{};
    };
    std::array<std::array<CachedBufferBinding, Shader::NUM_BUFFERS>, MaxShaderStages>
        cached_buffer_bindings{};

    struct CachedImageBinding {
        const Shader::Info* owner{};
        AmdGpu::Image sharp{};
        u64 resource_key{};
        u8 source_index{};
        u8 mip_index{};
        VideoCore::ImageId image_id{};
        u64 image_uid{};
        u64 topology_epoch{};
        VideoCore::ImageViewInfo view_info{};
        bool valid{};
    };
    std::array<std::array<CachedImageBinding, Shader::NUM_IMAGES>, MaxShaderStages>
        cached_image_bindings{};

    /// Image lookups by T#, shared by every program. A program switch invalidates the tokens
    /// above, which belong to a stage slot, but the same T# still resolves to the same image.
    struct TextureLookupEntry {
        AmdGpu::Image sharp{};
        u64 resource_key{};
        VideoCore::ImageId image_id{};
        u32 mip_index{};
        u64 image_uid{};
        u64 topology_epoch{};
        VideoCore::ImageViewInfo view_info{};
        bool valid{};
    };
    static constexpr size_t TextureLookupSize = 2048;
    static_assert(std::has_single_bit(TextureLookupSize));
    std::unique_ptr<std::array<TextureLookupEntry, TextureLookupSize>> texture_lookup;

    struct CachedImageDescription {
        const Shader::Info* owner{};
        AmdGpu::Image sharp{};
        u8 geometry_key{};
        std::unique_ptr<VideoCore::TextureCache::ImageDesc> base_desc;
    };
    std::array<std::array<CachedImageDescription, Shader::NUM_IMAGES>, MaxShaderStages>
        cached_image_descriptions{};

    struct CachedImageView {
        VideoCore::ImageId image_id{};
        u64 image_uid{};
        u64 topology_epoch{};
        vk::Image backing_image{};
        vk::ImageView image_view{};
        VideoCore::ImageViewInfo info{};
        bool valid{};
    };
    std::array<std::array<CachedImageView, Shader::NUM_IMAGES>, MaxShaderStages>
        cached_texture_views{};
    /// Description of the image BindTextureMiss resolves, kept across its mip bindings.
    VideoCore::TextureCache::ImageDesc texture_miss_desc;
    VideoCore::ImageViewInfo texture_miss_base_view;

    /// Binds a texture the stage slot cache missed, through the shared lookup or the texture
    /// cache, and refreshes both caches.
    void BindTextureMiss(const Shader::Info& stage, u32 stage_index, u32 image_index, u32 mip_index,
                         u32 num_bindings, const AmdGpu::Image& tsharp, u8 geometry_key,
                         u64 resource_key, CachedImageBinding& cached, bool& miss_desc_ready);
    /// Finds the image of a binding again after its image was marked for rebind.
    void RebindTexture(const Shader::Info& stage, ImageBindingInfo& image_binding);
    std::array<CachedImageView, AmdGpu::NUM_COLOR_BUFFERS> cached_color_target_views{};
    CachedImageView cached_depth_target_view{};
    /// Whether the cached view still describes view_info of the current backing of image.
    [[nodiscard]] static bool IsCachedViewCurrent(const CachedImageView& cached_view,
                                                  VideoCore::ImageId image_id,
                                                  const VideoCore::Image& image, u64 topology_epoch,
                                                  const VideoCore::ImageViewInfo& view_info);
    static void RefreshCachedView(CachedImageView& cached_view, VideoCore::ImageId image_id,
                                  VideoCore::Image& image, u64 topology_epoch,
                                  const VideoCore::ImageViewInfo& view_info,
                                  bool ensure_guest_samples);
    VideoCore::ImageId RebindColorTarget(u32 cb);

    struct DescriptorWriteState {
        u64 key0{};
        u64 key1{};
        u32 first_info{};
        bool is_buffer{};
    };

    struct DescriptorState {
        const Pipeline* pipeline{};
        /// Tick of the command buffer the pushed descriptors belong to.
        u64 command_buffer_tick{};
        u64 push_descriptor_epoch{};
        boost::container::static_vector<DescriptorWriteState,
                                        Shader::NUM_BUFFERS + Shader::NUM_IMAGES +
                                            Shader::NUM_SAMPLERS>
            writes;
        boost::container::static_vector<vk::DescriptorImageInfo,
                                        Shader::NUM_IMAGES + Shader::NUM_SAMPLERS>
            image_infos;
        boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS> buffer_infos;
        bool valid{};
    } descriptor_state;

    struct DynamicStateInputCache;
    mutable std::unique_ptr<DynamicStateInputCache> dynamic_state_inputs;
    mutable u64 dynamic_state_generation{};
    mutable const GraphicsPipeline* dynamic_state_pipeline{};
    mutable bool dynamic_state_indexed{};
    mutable bool dynamic_state_feedback_loop{};
    bool fault_process_pending{};
    bool attachment_feedback_loop{};

    /// Guest flushes since the last global barrier.
    vk::PipelineStageFlags2 pending_flush_src_stages{};
    vk::AccessFlags2 pending_flush_src_access{};
    vk::PipelineStageFlags2 pending_flush_dst_stages{};
    vk::AccessFlags2 pending_flush_dst_access{};
    /// A pipeline that accesses memory through device addresses ran since the last global
    /// barrier.
    bool dma_access_pending{};
};

} // namespace Vulkan
