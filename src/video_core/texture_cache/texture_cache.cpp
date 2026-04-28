// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later


#include <xxhash.h>
#include <array>
#include <cstring>

#include "common/assert.h"
#include "common/config.h"
#include "common/debug.h"
#include "common/scope_exit.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;

// GR2 Photo Mode: texture survey budget — counts down unique textures to log
// after encode. Accessible from both FindTexture (GPU thread) and
// TriggerPhotoTextureSurvey (game thread).
static std::atomic<int> s_photo_survey_budget{0};

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           AmdGpu::Liverpool* liverpool_, BufferCache& buffer_cache_,
                           PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)} {
    // Create basic null image at fixed image ID.
    const auto null_id = GetNullImage(vk::Format::eR8G8B8A8Unorm);
    ASSERT(null_id.index == NULL_IMAGE_ID.index);

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    pressure_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_PRESSURE_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() {
    // Clean up one-shot photo download Vulkan resources.
    if (photo_resources_init_) {
        const auto device = instance.GetDevice();
        if (photo_fence_) {
            device.destroyFence(photo_fence_);
        }
        if (photo_cmd_pool_) {
            device.freeCommandBuffers(photo_cmd_pool_, photo_cmd_buf_);
            device.destroyCommandPool(photo_cmd_pool_);
        }
        if (photo_staging_buf_) {
            if (photo_staging_ptr_) {
                device.unmapMemory(photo_staging_mem_);
            }
            device.destroyBuffer(photo_staging_buf_);
            device.freeMemory(photo_staging_mem_);
        }
    }
}

ImageId TextureCache::GetNullImage(const vk::Format format) {
    const auto existing_image = null_images.find(format);
    if (existing_image != null_images.end()) {
        return existing_image->second;
    }

    ImageInfo info{};
    info.pixel_format = format;
    info.type = AmdGpu::ImageType::Color2D;
    info.tile_mode = AmdGpu::TileMode::Thin1DThin;
    info.num_bits = 32;
    info.UpdateSize();

    const ImageId null_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
    auto& image = slot_images[null_id];
    Vulkan::SetObjectName(instance.GetDevice(), image.GetImage(),
                          fmt::format("Null Image ({})", vk::to_string(format)));

    image.flags = ImageFlagBits::Empty;
    image.track_addr = image.info.guest_address;
    image.track_addr_end = image.info.guest_address + image.info.guest_size;

    null_images.emplace(format, null_id);
    return null_id;
}

void TextureCache::ProcessDownloadImages() {
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id);
    }
    download_images.clear();
}

void TextureCache::DownloadImageMemory(ImageId image_id) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }

    // FIX(GR2FORK): defensive guards. The previous version of this function
    // happily called vkCmdCopyImageToBuffer on any image with the GpuModified
    // flag set, which crashes the NVIDIA driver (read at offset 0x108 inside
    // nvoglv64) for several legitimate-but-unsupported source formats:
    //
    //   - Multisampled images: vkCmdCopyImageToBuffer requires samples=1.
    //     Driver may AV instead of returning an error in non-validating modes.
    //   - 3D images: this code path only sets imageExtent.depth=1 and uses
    //     layerCount=resources.layers, which is wrong for 3D — depth is the
    //     extent dimension, not a layer count. Driver behavior on mismatched
    //     extents/layers for 3D is implementation-defined.
    //   - Depth/stencil images with both aspects: aspectMask hardcoded to
    //     either eDepth or eColor, never eStencil; copying a combined D/S
    //     image with only eDepth set may AV depending on driver.
    //   - Null backing: if `backing` is somehow null, GetImage() dereferences
    //     a null pointer. This shouldn't happen given normal flow but a
    //     belt-and-braces check costs nothing on the hot path.
    //
    // Just skip the download for these cases. Skipping is safe — the data
    // simply doesn't make it back to guest memory, which is the same outcome
    // as the previous tiled+download workaround at the GC call site.
    if (image.backing == nullptr) [[unlikely]] {
        LOG_WARNING(Render_Vulkan,
                    "DownloadImageMemory: image {} has null backing, skipping",
                    image_id.index);
        return;
    }
    if (image.info.num_samples > 1) [[unlikely]] {
        // MSAA — vkCmdCopyImageToBuffer requires samples=1.
        return;
    }
    if (image.info.size.depth > 1) [[unlikely]] {
        // 3D image — this path's BufferImageCopy descriptor is shaped for 2D.
        return;
    }
    if (image.info.props.is_depth &&
        (image.aspect_mask & vk::ImageAspectFlagBits::eStencil)) [[unlikely]] {
        // Combined depth+stencil — needs separate copies per aspect, which
        // this code path doesn't do.
        return;
    }

    auto& download_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
    const u32 download_size = image.info.pitch * image.info.size.height *
                              image.info.resources.layers * (image.info.num_bits / 8);
    if (download_size == 0) [[unlikely]] {
        // Pitch / num_bits not populated — would record a copy with zero rows
        // that some drivers reject.
        return;
    }
    ASSERT(download_size <= image.info.guest_size);
    const auto [download, offset] = download_buffer.Map(download_size);
    download_buffer.Commit();
    const vk::BufferImageCopy image_download = {
        .bufferOffset = offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, 1},
    };
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    cmdbuf.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                             download_buffer.Handle(), image_download);

    scheduler.DeferPriorityOperation(
        [this, device_addr = image.info.guest_address, download, download_size] {
            Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr), download,
                                                      download_size);
        });
}

bool TextureCache::ForceDownloadByAddress(VAddr address, u64 size) {
    // ── GR2 Photo Mode: synchronous GPU→CPU render target download ─────────
    //
    // Called from the game thread during sceJpegEncEncode.  We must NOT use
    // SendCommand<true> (deadlocks) or the scheduler's command buffer (data race
    // with GPU thread).  Instead we use a dedicated one-shot Vulkan command
    // pool/buffer/fence that is owned entirely by the game thread.
    //
    // Strategy:
    //   1. Retrieve the last tracked 1024×1024 linear render target (set in
    //      FindRenderTarget / FindTexture on the GPU thread).
    //   2. vkDeviceWaitIdle — ensures ALL prior GPU work (including the photo
    //      render) is complete and the image contents are stable.
    //   3. Record an image→buffer copy on our one-shot command buffer.
    //   4. Submit to the graphics queue and wait on a fence.
    //   5. memcpy from the staging buffer to the encoder's CPU address.
    //
    // This is intentionally heavy (WaitIdle + dedicated submit) because it only
    // runs once per photo capture — correctness over performance.
    // ────────────────────────────────────────────────────────────────────────────

    // 1. Retrieve the tracked photo RT.
    ImageId target_id{};
    {
        std::lock_guard lk{photo_rt_mutex_};
        target_id = last_photo_rt_.image_id;
    }
    if (!target_id) {
        LOG_WARNING(Render_Vulkan,
                    "[GR2Photo] ForceDownload: no photo RT tracked yet");
        return false;
    }

    // 2. Validate the image still exists and matches expectations.
    Image* img{nullptr};
    vk::Image vk_image{};
    vk::ImageLayout current_layout{};
    u32 img_width{}, img_height{}, img_pitch{}, img_bpp{};
    u32 img_layers{};
    bool is_depth{};
    {
        std::shared_lock lock{mutex};
        img = &slot_images[target_id];
        if (img->info.size.width != 1024 || img->info.size.height != 1024) {
            LOG_WARNING(Render_Vulkan,
                        "[GR2Photo] ForceDownload: tracked RT is {}x{}, expected 1024x1024",
                        img->info.size.width, img->info.size.height);
            return false;
        }
        if (False(img->flags & ImageFlagBits::GpuModified)) {
            LOG_WARNING(Render_Vulkan,
                        "[GR2Photo] ForceDownload: tracked RT not GPU-modified");
            return false;
        }
        vk_image       = img->GetImage();
        current_layout = img->backing ? img->backing->state.layout
                                      : vk::ImageLayout::eUndefined;
        img_width      = img->info.size.width;
        img_height     = img->info.size.height;
        img_pitch      = img->info.pitch;
        img_bpp        = img->info.num_bits / 8;
        img_layers     = img->info.resources.layers;
        is_depth       = img->info.props.is_depth;
    }

    const u32 download_size = img_pitch * img_height * img_layers * img_bpp;
    if (download_size == 0 || download_size > 16 * 1024 * 1024) {
        LOG_ERROR(Render_Vulkan,
                  "[GR2Photo] ForceDownload: suspicious download_size={}", download_size);
        return false;
    }

    LOG_INFO(Render_Vulkan,
             "[GR2Photo] ForceDownload: RT id={} {}x{} pitch={} bpp={} download={}B target={:#x}",
             target_id.index, img_width, img_height, img_pitch, img_bpp, download_size, address);

    const auto device = instance.GetDevice();

    // 3. Lazy-init one-shot Vulkan resources (command pool, command buffer, fence,
    //    staging buffer).  These persist for the lifetime of the TextureCache and
    //    are reused across photo captures.
    if (!photo_resources_init_) {
        // Command pool — Transient + ResetCommandBuffer for one-shot reuse.
        const vk::CommandPoolCreateInfo pool_ci{
            .flags = vk::CommandPoolCreateFlagBits::eTransient |
                     vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
        };
        photo_cmd_pool_ = Vulkan::Check<"create photo cmd pool">(
            device.createCommandPool(pool_ci));

        const vk::CommandBufferAllocateInfo alloc_ci{
            .commandPool = photo_cmd_pool_,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };
        auto cmd_bufs = Vulkan::Check<"allocate photo cmd buf">(
            device.allocateCommandBuffers(alloc_ci));
        photo_cmd_buf_ = cmd_bufs[0];

        photo_fence_ = Vulkan::Check<"create photo fence">(device.createFence({}));

        // Staging buffer — 4 MiB covers 1024×1024×4 BGRA with room to spare.
        photo_staging_size_ = 4u * 1024u * 1024u;
        const vk::BufferCreateInfo buf_ci{
            .size = photo_staging_size_,
            .usage = vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
        };
        photo_staging_buf_ = Vulkan::Check<"create photo staging buf">(
            device.createBuffer(buf_ci));

        const auto mem_req = device.getBufferMemoryRequirements(photo_staging_buf_);
        const auto mem_props = instance.GetPhysicalDevice().getMemoryProperties();

        // Find host-visible, host-coherent memory type.
        u32 mem_type_idx = UINT32_MAX;
        const auto desired = vk::MemoryPropertyFlagBits::eHostVisible |
                             vk::MemoryPropertyFlagBits::eHostCoherent;
        for (u32 i = 0; i < mem_props.memoryTypeCount; ++i) {
            if ((mem_req.memoryTypeBits & (1u << i)) &&
                (mem_props.memoryTypes[i].propertyFlags & desired) == desired) {
                mem_type_idx = i;
                break;
            }
        }
        ASSERT_MSG(mem_type_idx != UINT32_MAX,
                   "[GR2Photo] No host-visible coherent memory type found");

        const vk::MemoryAllocateInfo alloc_info{
            .allocationSize = mem_req.size,
            .memoryTypeIndex = mem_type_idx,
        };
        photo_staging_mem_ = Vulkan::Check<"allocate photo staging mem">(
            device.allocateMemory(alloc_info));
        Vulkan::Check<"bind photo staging mem">(
            device.bindBufferMemory(photo_staging_buf_, photo_staging_mem_, 0));
        photo_staging_ptr_ = Vulkan::Check<"map photo staging mem">(
            device.mapMemory(photo_staging_mem_, 0, mem_req.size));

        photo_resources_init_ = true;
        LOG_INFO(Render_Vulkan,
                 "[GR2Photo] One-shot Vulkan resources initialized (staging={}B)", photo_staging_size_);
    }

    if (download_size > photo_staging_size_) {
        LOG_ERROR(Render_Vulkan,
                  "[GR2Photo] download_size {} exceeds staging buffer {}", download_size, photo_staging_size_);
        return false;
    }

    // 4. Wait for ALL prior GPU work to finish.  This guarantees the photo render
    //    target contents are fully written and the image layout is stable.
    //    WaitIdle is heavy but this is a one-shot photo path.
    auto idle_result = device.waitIdle();
    ASSERT_MSG(idle_result == vk::Result::eSuccess, "[GR2Photo] waitIdle failed");

    // 5. Record barrier + copy on our one-shot command buffer.
    Vulkan::Check(device.resetFences(photo_fence_));
    Vulkan::Check(photo_cmd_buf_.reset());
    Vulkan::Check(photo_cmd_buf_.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    }));

    // Transition image to TransferSrcOptimal.  After WaitIdle the image is idle;
    // we use a full memory barrier to cover any prior access.
    // current_layout reflects the image's tracked state (from Image::Transit),
    // which may be eColorAttachmentOptimal (render pass end) or eTransferSrcOptimal
    // (if readbackLinearImages already processed it).
    const vk::ImageLayout restore_layout = current_layout;
    if (current_layout == vk::ImageLayout::eUndefined) {
        // Safety: treat Undefined as General to avoid discarding image data.
        current_layout = vk::ImageLayout::eGeneral;
    }
    const vk::ImageMemoryBarrier2 pre_barrier{
        .srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask  = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout     = current_layout,
        .newLayout     = vk::ImageLayout::eTransferSrcOptimal,
        .image         = vk_image,
        .subresourceRange = {
            .aspectMask     = is_depth ? vk::ImageAspectFlagBits::eDepth
                                       : vk::ImageAspectFlagBits::eColor,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = img_layers,
        },
    };
    photo_cmd_buf_.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &pre_barrier,
    });

    const vk::BufferImageCopy copy_region{
        .bufferOffset      = 0,
        .bufferRowLength   = img_pitch,
        .bufferImageHeight = img_height,
        .imageSubresource  = {
            .aspectMask     = is_depth ? vk::ImageAspectFlagBits::eDepth
                                       : vk::ImageAspectFlagBits::eColor,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = img_layers,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {img_width, img_height, 1},
    };
    photo_cmd_buf_.copyImageToBuffer(vk_image, vk::ImageLayout::eTransferSrcOptimal,
                                     photo_staging_buf_, copy_region);

    // Transition back to the original layout so the GPU thread's layout tracking
    // remains valid when it next touches this image.
    const vk::ImageMemoryBarrier2 post_barrier{
        .srcStageMask  = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout     = vk::ImageLayout::eTransferSrcOptimal,
        .newLayout     = (restore_layout == vk::ImageLayout::eUndefined)
                             ? vk::ImageLayout::eGeneral : restore_layout,
        .image         = vk_image,
        .subresourceRange = pre_barrier.subresourceRange,
    };
    photo_cmd_buf_.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &post_barrier,
    });

    Vulkan::Check(photo_cmd_buf_.end());

    // 6. Submit to the graphics queue and wait.
    const auto queue = instance.GetGraphicsQueue();
    const vk::SubmitInfo submit_info{
        .commandBufferCount = 1,
        .pCommandBuffers    = &photo_cmd_buf_,
    };
    auto submit_result = queue.submit(submit_info, photo_fence_);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost,
               "[GR2Photo] Device lost during photo download submit");
    const auto wait_result = device.waitForFences(photo_fence_, VK_TRUE, UINT64_MAX);
    if (wait_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan,
                  "[GR2Photo] Fence wait failed: {}", vk::to_string(wait_result));
        return false;
    }

    // 7. Synchronous memcpy from staging buffer to the encoder's CPU address.
    u8* dst = std::bit_cast<u8*>(address);
    std::memcpy(dst, photo_staging_ptr_, download_size);

    LOG_INFO(Render_Vulkan,
             "[GR2Photo] ForceDownload complete: {}B copied to {:#x}",
             download_size, address);

    // 8. Save a snapshot of the photo pixels so we can restore them later
    //    if the render target gets reused before the preview is displayed.
    SavePhotoSnapshot(address, target_id, img_width, img_height, img_pitch, img_bpp);

    // 9. DO NOT invalidate the texture cache image.
    //
    // Previously this called InvalidateMemory(address, download_size) to mark
    // the image CpuDirty so the texture cache would re-upload from CPU memory.
    // This was intended to help the preview display, but it caused a crash:
    //
    //   The memcpy in step 7 writes raw RGBA pixels to guest memory. When
    //   InvalidateMemory marks the image CpuDirty, the texture cache will
    //   re-upload those raw pixels on next use. But the game's fiber worker
    //   (BGFiberWorkerHi) concurrently reads from the same address expecting
    //   a GNF-format texture → checks "GNF " magic at [r15] → assertion
    //   trap at VA 0x11d4cb1 because raw RGBA pixels don't have GNF headers.
    //
    // Not needed because:
    // - The JPEG encoder reads directly from guest memory (step 7 put pixels there)
    // - The GPU-side image already contains the correct photo render
    // - The preview display samples the GPU image, which is correct as-is
    // - Marking CpuDirty actually corrupts the view by forcing re-upload of
    //   headerless raw pixels
    LOG_INFO(Render_Vulkan,
             "[GR2Photo] ForceDownload: skipping InvalidateMemory "
             "(GPU image already has correct photo, avoids GNF assertion crash)");

    return true;
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash
        const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
        image.hash = XXH3_64bits(addr, image.info.guest_size);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    image.MarkFastStateDirty(); // OPT: Invalidate lock-free fast path.
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            image.MarkFastStateDirty(); // OPT: Invalidate lock-free fast path.
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        // Only consider images that match base address.
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        // Ensure image is reuploaded when accessed again.
        image.flags |= ImageFlagBits::GpuDirty;
        image.MarkFastStateDirty(); // OPT: Invalidate lock-free fast path.
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        const auto new_image_id =
            slot_images.insert(instance, scheduler, blit_helper, slot_image_views, new_info);
        RegisterImage(new_image_id);

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = cache_image.usage;
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;

        if (cache_image.info.num_samples == 1 && new_info.num_samples == 1) {
            // Perform depth<->color copy using the intermediate copy buffer.
            if (instance.IsMaintenance8Supported()) {
                new_image.CopyImage(cache_image);
            } else {
                const auto& copy_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
                new_image.CopyImageWithBuffer(cache_image, copy_buffer.Handle(), 0);
            }
        } else if (cache_image.info.num_samples == 1 && new_info.props.is_depth &&
                   new_info.num_samples > 1) {
            // Perform a rendering pass to transfer the channels of source as samples in dest.
            cache_image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
                                vk::AccessFlagBits2::eShaderRead, {});
            new_image.Transit(vk::ImageLayout::eDepthAttachmentOptimal,
                              vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {});
            blit_helper.ReinterpretColorAsMsDepth(
                new_info.size.width, new_info.size.height, new_info.num_samples,
                cache_image.info.pixel_format, new_info.pixel_format, cache_image.GetImage(),
                new_image.GetImage());
        } else {
            LOG_WARNING(Render_Vulkan, "Unimplemented depth overlap copy");
        }

        // Free the cache image.
        FreeImage(cache_image_id);
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NumFramesBeforeRemoval;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    merged_image.CopyMip(cache_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_image_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    new_image.CopyImage(src_image);

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);

    TrackImage(new_image_id);
    new_image.flags &= ~ImageFlagBits::Dirty;
    return new_image_id;
}

bool TextureCache::ValidateCachedFindImage(const ImageDesc& desc, ImageId image_id,
                                           bool exact_fmt) const {
                                               if (!image_id) return false;

                                               // Fast path: if nothing has been registered/unregistered since we cached this,
                                               // the slot_images allocation and Registered flag can't have changed.
                                               // We still need to verify the image fields match (they could have been
                                               // re-registered at the same slot with different params), but we can do
                                               // that WITHOUT the lock by reading the fields atomically.
                                               //
                                               // This is safe because:
                                               // - slot_images[id] memory is stable (pool allocator, never moved)
                                               // - We're only reading, and the fields we check are written under unique_lock
                                               // - Worst case: we read stale data and return false (falls through to full path)
                                               if (!slot_images.is_allocated(image_id)) return false;

                                               const auto& cache_image = slot_images[image_id];
                                               if (!(cache_image.flags & ImageFlagBits::Registered)) return false;

                                               const auto& ci = cache_image.info;
                                               const auto& info = desc.info;
                                               if (ci.guest_address != info.guest_address ||
                                                   ci.guest_size != info.guest_size ||
                                                   ci.size != info.size ||
                                                   (ci.type != info.type && info.size != Extent3D{1, 1, 1}) ||
                                                   !IsVulkanFormatCompatible(ci.pixel_format, info.pixel_format) ||
                                                   (exact_fmt && info.pixel_format != ci.pixel_format)) {
                                                   return false;
                                                   }
                                                   return true;
                                           }

                                           TextureCache::FindImageWithViewResult TextureCache::FindImageWithView(const ImageDesc& desc, bool exact_fmt,
                                                                                                                 bool touch_lru) {
    const auto& info = desc.info;

    if (info.guest_address == 0) [[unlikely]] {
        return {GetNullImage(info.pixel_format), -1, -1};
    }

    // OPT(v8/v4): Tiny per-thread hot cache for perfect-match lookups.
    // GR2 repeatedly queries the same image descriptors; avoid scanning page candidates.
    struct PtrHotEntry {
        const ImageDesc* desc_ptr{};
        ImageId id{};
        VAddr guest_address{};
        u32 guest_size{};
        Extent3D size{};
        AmdGpu::ImageType type{};
        vk::Format pixel_format{};
        bool exact{};
        bool valid{false};
    };
    thread_local PtrHotEntry ptr_hot0{};
    thread_local PtrHotEntry ptr_hot1{};

    auto fast_hot_hash = [&](const ImageInfo& ii, bool exact) noexcept -> u64 {
        // PERF(v4): direct-mapped cache only needs a cheap mixer (we fully validate on hit).
        u64 x = static_cast<u64>(ii.guest_address);
        x ^= (static_cast<u64>(ii.guest_size) << 17);
        x ^= (static_cast<u64>(ii.size.width) << 1);
        x ^= (static_cast<u64>(ii.size.height) << 21);
        x ^= (static_cast<u64>(ii.size.depth) << 37);
        x ^= (static_cast<u64>(static_cast<u32>(ii.type)) << 45);
        x ^= (static_cast<u64>(static_cast<u32>(ii.pixel_format)) << 49);
        x ^= exact ? 0x9e3779b97f4a7c15ULL : 0ULL;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return x;
    };

    struct HotEntry {
        u64 key{};
        ImageId id{};
        bool valid{false};
    };
    static thread_local std::array<HotEntry, 256> hot_cache{};
    u64 hot_key{};
    HotEntry* hot_slot_ptr = nullptr;
    auto ensure_hot_slot = [&]() noexcept -> HotEntry& {
        if (!hot_slot_ptr) {
            hot_key = fast_hot_hash(info, exact_fmt);
            hot_slot_ptr = &hot_cache[hot_key & (hot_cache.size() - 1)];
        }
        return *hot_slot_ptr;
    };

    // ARCH-2: Lock-free fast path for ptr_hot and hot_cache lookups.
    //
    // ptr_hot and hot_cache only read thread_local data + pool-stable Image fields.
    // On x86 (TSO), aligned reads of fundamental types from stable memory are safe
    // without locks. Stale reads cause a cache miss and fall through to the locked path.
    // This eliminates shared_lock acquisition for ~80-90% of lookups.
    const u64 now_tick = scheduler.CurrentTick();
    static constexpr u64 kTouchIntervalTicks = 65536;
    ImageId fast_image_id{};
    bool fast_needs_touch = false;
    {
        // NO lock here — ptr_hot and hot_cache are lock-free safe

        auto try_ptr_hot = [&](const PtrHotEntry& e) -> bool {
            if (!e.valid || e.desc_ptr != &desc || e.exact != exact_fmt) {
                return false;
            }
            if (!slot_images.is_allocated(e.id)) {
                return false;
            }
            auto& cache_image = slot_images[e.id];
            if (!(cache_image.flags & ImageFlagBits::Registered)) {
                return false;
            }
            if (cache_image.info.guest_address != e.guest_address ||
                cache_image.info.guest_size != e.guest_size ||
                cache_image.info.size != e.size ||
                cache_image.info.type != e.type ||
                cache_image.info.pixel_format != e.pixel_format) {
                return false;
                }
                if (cache_image.info.guest_address != info.guest_address ||
                    cache_image.info.guest_size != info.guest_size ||
                    cache_image.info.size != info.size ||
                    !IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
                    (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1}) ||
                    (exact_fmt && info.pixel_format != cache_image.info.pixel_format)) {
                    return false;
                    }
                    fast_image_id = e.id;
                fast_needs_touch = (now_tick - cache_image.tick_accessed_last) > kTouchIntervalTicks;
                return true;
        };

        // Zero-hash pointer MRU fast path (very common with Rasterizer's descriptor cache).
        if (!try_ptr_hot(ptr_hot0)) {
            (void)try_ptr_hot(ptr_hot1);
        }

        // Hot-cache check (O(1) in common case). Lock-free safe.
        if (!fast_image_id) {
            HotEntry& hot_slot = ensure_hot_slot();
            if (hot_slot.valid && hot_slot.key == hot_key) {
                const auto cache_id = hot_slot.id;
                if (slot_images.is_allocated(cache_id)) {
                auto& cache_image = slot_images[cache_id];

                if ((cache_image.flags & ImageFlagBits::Registered) &&
                    cache_image.info.guest_address == info.guest_address &&
                    cache_image.info.guest_size == info.guest_size &&
                    cache_image.info.size == info.size &&
                    IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) &&
                    !(cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1}) &&
                    (!exact_fmt || info.pixel_format == cache_image.info.pixel_format)) {

                    fast_image_id = cache_id;
                fast_needs_touch = (now_tick - cache_image.tick_accessed_last) > kTouchIntervalTicks;
                    }
                }
            }
        }
    }

    // --- Shared-lock: page_table scan (only when hot caches missed) ---
    if (!fast_image_id) {
        std::shared_lock lock{mutex};

        // Fast path (boot-safe): scan only the base page’s candidates for a PERFECT match.
        // Reverse scan preserves the original “last match wins” semantics with an early break.
        const u64 base_page = info.guest_address >> Traits::PageBits;
        if (const auto it = page_table.find(base_page); it != nullptr) {
            for (auto rit = it->rbegin(); rit != it->rend(); ++rit) {
                const auto cache_id = *rit;
                auto& cache_image = slot_images[cache_id];

                if (!(cache_image.flags & ImageFlagBits::Registered)) {
                    continue;
                }
                if (cache_image.info.guest_address != info.guest_address) {
                    continue;
                }
                if (cache_image.info.guest_size != info.guest_size) {
                    continue;
                }
                if (cache_image.info.size != info.size) {
                    continue;
                }
                if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
                    (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
                    continue;
                    }
                    if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
                        continue;
                    }

                    fast_image_id = cache_id;
                // Avoid write-lock churn on hot images: only re-touch every N ticks.
                fast_needs_touch = (now_tick - cache_image.tick_accessed_last) > kTouchIntervalTicks;
                break;
            }
        }
    }

    if (fast_image_id && !fast_needs_touch) {
        HotEntry& hot_slot = ensure_hot_slot();
        hot_slot.key = hot_key;
        hot_slot.id = fast_image_id;
        hot_slot.valid = true;
        ptr_hot1 = ptr_hot0;
        ptr_hot0 = PtrHotEntry{.desc_ptr = &desc,
            .id = fast_image_id,
            .guest_address = info.guest_address,
            .guest_size = info.guest_size,
            .size = info.size,
            .type = info.type,
            .pixel_format = info.pixel_format,
            .exact = exact_fmt,
            .valid = true};
            return {fast_image_id, -1, -1};
    }

    if (fast_image_id) {
        std::unique_lock lock{mutex};
        Image& image = slot_images[fast_image_id];
        if (image.flags & ImageFlagBits::Registered) {
            image.tick_accessed_last = now_tick;
            if (touch_lru) {
                TouchImage(image);
            }
            HotEntry& hot_slot = ensure_hot_slot();
            hot_slot.key = hot_key;
            hot_slot.id = fast_image_id;
            hot_slot.valid = true;
            ptr_hot1 = ptr_hot0;
            ptr_hot0 = PtrHotEntry{.desc_ptr = &desc,
                .id = fast_image_id,
                .guest_address = info.guest_address,
                .guest_size = info.guest_size,
                .size = info.size,
                .type = info.type,
                .pixel_format = info.pixel_format,
                .exact = exact_fmt,
                .valid = true};
                return {fast_image_id, -1, -1};
        }
        // If it got invalidated between locks, fall through to the original slow path.
    }

    std::scoped_lock lock{mutex};

    ImageId image_id{};
    ImageIds image_ids;
    if (!image_id) {
        ForEachImageInRegion(info.guest_address, info.guest_size,
                             [&](ImageId cache_id, Image& cache_image) {
                                 // Preserve existing behavior: collect all candidates.
                                 image_ids.push_back(cache_id);

                                 // Also preserve existing behavior: “last perfect match wins”.
                                 if (cache_image.info.guest_address != info.guest_address) {
                                     return;
                                 }
                                 if (cache_image.info.guest_size != info.guest_size) {
                                     return;
                                 }
                                 if (cache_image.info.size != info.size) {
                                     return;
                                 }
                                 if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
                                     (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
                                     return;
                                     }
                                     if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
                                         return;
                                     }
                                     image_id = cache_id;
                             });
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;

            // Cheap byte-range overlap reject BEFORE ResolveOverlap(). This is semantics-preserving:
            // if the guest byte ranges do not intersect, the candidate cannot affect the merged image.
            const auto& cand_info = slot_images[cache_id].info;
            const u64 merged_begin = merged_info.guest_address;
            u64 merged_end = merged_begin + merged_info.guest_size;
            if (merged_end < merged_begin) merged_end = ~u64{0}; // overflow-safe
            const u64 cand_begin = cand_info.guest_address;
            u64 cand_end = cand_begin + cand_info.guest_size;
            if (cand_end < cand_begin) cand_end = ~u64{0}; // overflow-safe

            if (cand_end <= merged_begin || merged_end <= cand_begin) {
                continue;
            }

            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
            ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        image_id = slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = now_tick;
    if (touch_lru) {
        TouchImage(image);
    }

    HotEntry& hot_slot = ensure_hot_slot();
    hot_slot.key = hot_key;
    hot_slot.id = image_id;
    hot_slot.valid = true;
    if (view_mip < 0 && view_slice < 0) {
        ptr_hot1 = ptr_hot0;
        ptr_hot0 = PtrHotEntry{.desc_ptr = &desc,
            .id = image_id,
            .guest_address = info.guest_address,
            .guest_size = info.guest_size,
            .size = info.size,
            .type = info.type,
            .pixel_format = info.pixel_format,
            .exact = exact_fmt,
            .valid = true};
    }

    return {image_id, view_mip, view_slice};

}



ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto r = FindImageWithView(desc, exact_fmt);
    if (r.view_mip > 0) {
        desc.view_info.range.base.level = r.view_mip;
    }
    if (r.view_slice > 0) {
        desc.view_info.range.base.layer = r.view_slice;
    }
    return r.image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

void TextureCache::UpdateImage(ImageId image_id) {
    // PERF(GR2+OPT): Lock-free atomic fast path.
    //
    // UpdateImage is called for every image binding on every draw call. The majority
    // of calls (~80%+) find the image already clean, tracked, and recently touched.
    // The previous implementation took a shared_lock even for this common case,
    // showing up as 4.27% of L1D cache misses due to rwlock contention.
    //
    // New approach: each Image has an atomic<u64> `fast_update_state` that packs
    // {dirty_bit, tracked_bit, last_touch_tick}. We read it with a single atomic
    // load (no lock at all) and only proceed to the locked path when something changed.

    const u64 now_tick = scheduler.CurrentTick();
    static constexpr u64 kTouchIntervalTicks = 8192;

    // --- Atomic fast path (NO lock acquisition) ---
    if (slot_images.is_allocated(image_id)) {
        const u64 fast_state = slot_images[image_id].ReadFastState();

        const bool is_dirty = (fast_state & Image::kFastStateDirty) != 0;
        const bool is_tracked = (fast_state & Image::kFastStateTracked) != 0;
        const u64 last_tick = fast_state >> Image::kFastStateTouchShift;

        if (!is_dirty && is_tracked && (now_tick - last_tick) <= kTouchIntervalTicks) {
            // Image is clean, properly tracked, and recently touched.
            // No work needed — skip all locks entirely.
            return;
        }
    }

    // --- Shared-lock path: verify conditions under lock ---
    bool needs_refresh = false;
    bool needs_touch = false;
    {
        std::shared_lock lock{mutex};
        Image& image = slot_images[image_id];
        if (!(image.flags & ImageFlagBits::Registered)) {
            return;
        }

        const u64 begin = image.info.guest_address;
        u64 end = begin + image.info.guest_size;
        if (end < begin) {
            end = ~u64{0};
        }

        const bool tracked_ok = image.IsTracked() && begin == image.track_addr && end == image.track_addr_end;
        needs_refresh = (image.flags & ImageFlagBits::Dirty) || !tracked_ok;
        needs_touch = (now_tick - image.tick_accessed_last) > kTouchIntervalTicks;
        if (!needs_refresh && !needs_touch) {
            // Update atomic fast state so future calls skip even the shared_lock.
            image.UpdateFastState(now_tick, tracked_ok);
            return;
        }
    }

    // --- Unique-lock path: perform actual work ---
    std::unique_lock lock{mutex};
    Image& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }

    const u64 begin = image.info.guest_address;
    u64 end = begin + image.info.guest_size;
    if (end < begin) {
        end = ~u64{0};
    }

    const bool tracked_ok = image.IsTracked() && begin == image.track_addr && end == image.track_addr_end;
    const bool do_refresh = (image.flags & ImageFlagBits::Dirty) || !tracked_ok;

    if (do_refresh) {
        TrackImage(image_id);
        TouchImage(image);
        RefreshImage(image);
    } else {
        TouchImage(image);
    }

    image.tick_accessed_last = now_tick;

    // Update atomic fast state for future lock-free checks.
    const bool now_tracked = image.IsTracked() &&
                             begin == image.track_addr &&
                             end == image.track_addr_end;
    const bool now_clean = !(image.flags & ImageFlagBits::Dirty);
    if (now_clean && now_tracked) {
        image.UpdateFastState(now_tick, true);
    }
}

ImageView& TextureCache::FindTexture(ImageId image_id, BindingType type, const ImageViewInfo& view_info) {
    Image& image = slot_images[image_id];
    if (type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (Config::readbackLinearImages() && !image.info.props.is_tiled &&
            image.info.guest_address != 0) {
            download_images.emplace(image_id);
            }

        // GR2 Photo Mode: also track 1024×1024 storage writes (compute path).
        if (image.info.size.width == 1024 && image.info.size.height == 1024 &&
            !image.info.props.is_tiled && image.info.guest_address != 0) {
            std::lock_guard lk{photo_rt_mutex_};
            last_photo_rt_.image_id = image_id;
            last_photo_rt_.stamp = photo_rt_stamp_.fetch_add(1, std::memory_order_relaxed) + 1;
        }
    }

    // GR2 Photo Mode: Broad texture sample diagnostic + snapshot restore.
    // After encode, we need to discover what texture the game samples for preview.
    // Use a time-limited window to log ALL texture samples (rate-limited).
    {
        const int budget = s_photo_survey_budget.load(std::memory_order_relaxed);
        if (budget > 0 && type == BindingType::Texture) {
            // Rate-limit: only log unique (address, size) pairs
            struct SeenEntry { VAddr addr; u32 w; u32 h; };
            static thread_local std::array<SeenEntry, 64> seen_cache{};
            static thread_local int seen_count{0};
            static thread_local int last_seen_budget{0};

            // Reset seen cache when a new survey starts
            if (budget > last_seen_budget) {
                seen_count = 0;
            }
            last_seen_budget = budget;

            const VAddr addr = image.info.guest_address;
            const u32 w = image.info.size.width;
            const u32 h = image.info.size.height;
            bool already_seen = false;
            for (int i = 0; i < seen_count && i < 64; i++) {
                if (seen_cache[i].addr == addr && seen_cache[i].w == w && seen_cache[i].h == h) {
                    already_seen = true;
                    break;
                }
            }
            if (!already_seen) {
                if (seen_count < 64) {
                    seen_cache[seen_count++] = {addr, w, h};
                }
                LOG_INFO(Render_Vulkan,
                         "[GR2Photo] SURVEY tex {}x{} addr={:#x} tiled={} bpp={} "
                         "GpuMod={} id={} type={}",
                         w, h, addr,
                         image.info.props.is_tiled ? 1 : 0,
                         image.info.num_bits / 8,
                         True(image.flags & ImageFlagBits::GpuModified) ? 1 : 0,
                         image_id.index,
                         static_cast<u32>(image.info.type));
                // Also log to Core category (Render_Vulkan may be buffered/filtered)
                LOG_INFO(Core,
                         "[GR2Survey] {}x{} addr={:#x} tiled={} bpp={}",
                         w, h, addr,
                         image.info.props.is_tiled ? 1 : 0,
                         image.info.num_bits / 8);
                s_photo_survey_budget.fetch_sub(1, std::memory_order_relaxed);
            }
        }

        // Check for photo-address match (any tiling mode)
        if (type == BindingType::Texture &&
            image.info.size.width == 1024 && image.info.size.height == 1024) {
            std::lock_guard lk{photo_snapshot_mutex_};
            if (photo_snapshot_.valid &&
                image.info.guest_address == photo_snapshot_.guest_address) {
                LOG_INFO(Render_Vulkan,
                         "[GR2Photo] FindTexture: PHOTO ADDRESS sampled! "
                         "addr={:#x} tiled={} GpuMod={} CpuDirty={} id={}",
                         image.info.guest_address,
                         image.info.props.is_tiled ? 1 : 0,
                         True(image.flags & ImageFlagBits::GpuModified) ? 1 : 0,
                         True(image.flags & ImageFlagBits::CpuDirty) ? 1 : 0,
                         image_id.index);
            }
        }

        // Restore snapshot for matching images (linear or tiled)
        if (type == BindingType::Texture &&
            image.info.size.width == 1024 && image.info.size.height == 1024) {
            MaybeRestorePhotoSnapshot(image_id, image);
        }
    }

    // PERF(GR2): FindTexture() can be called multiple times for the same image within one bind pass
    // (storage aliases, repeated view binds, cross-stage reuse). Skip redundant UpdateImage() calls
    // within the same command buffer tick and rely on the first call to synchronize the image.
    struct UpdateImageOnceCacheEntry {
        vk::CommandBuffer cmdbuf{};
        u64 tick{};
        ImageId image_id{};
        bool valid{false};
    };
    static thread_local std::array<UpdateImageOnceCacheEntry, 128> update_once_cache{};

    const auto cmdbuf = scheduler.CommandBuffer();
    const u64 tick = scheduler.CurrentTick();
    const u64 key = (static_cast<u64>(image_id.index) * 0x9e3779b97f4a7c15ULL) ^ tick;
    auto& e = update_once_cache[key & (update_once_cache.size() - 1)];
    if (!(e.valid && e.cmdbuf == cmdbuf && e.tick == tick && e.image_id == image_id)) {
        UpdateImage(image_id);
        e.cmdbuf = cmdbuf;
        e.tick = tick;
        e.image_id = image_id;
        e.valid = true;
    }

    return image.FindView(view_info);
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc) {
    return FindTexture(image_id, desc.type, desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    if (Config::readbackLinearImages() && !image.info.props.is_tiled) {
        download_images.emplace(image_id);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // GR2 Photo Mode: track the most recently bound 1024×1024 linear render target.
    // ForceDownloadByAddress will use this to find the photo RT from the game thread.
    if (image.info.size.width == 1024 && image.info.size.height == 1024 &&
        !image.info.props.is_tiled && image.info.guest_address != 0) {
        std::lock_guard lk{photo_rt_mutex_};
        last_photo_rt_.image_id = image_id;
        last_photo_rt_.stamp = photo_rt_stamp_.fetch_add(1, std::memory_order_relaxed) + 1;

        // Diagnostic: detect when the photo RT is being reused after a snapshot
        {
            std::lock_guard snap_lk{photo_snapshot_mutex_};
            if (photo_snapshot_.valid &&
                image.info.guest_address == photo_snapshot_.guest_address) {
                LOG_INFO(Render_Vulkan,
                         "[GR2Photo] Photo RT at {:#x} reused as render target "
                         "(snapshot still valid, restore_count={})",
                         image.info.guest_address, photo_snapshot_.restore_count);
            }
        }

        LOG_TRACE(Render_Vulkan,
                  "[GR2Photo] Tracked 1024x1024 RT: id={} addr={:#x} pitch={} fmt={}",
                  image_id.index, image.info.guest_address, image.info.pitch,
                  static_cast<u32>(image.info.num_bits));
    }

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        surface_metas.emplace(desc.info.meta_info.cmask_addr,
                              MetaDataInfo{.type = MetaDataInfo::Type::CMask});
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        surface_metas.emplace(desc.info.meta_info.fmask_addr,
                              MetaDataInfo{.type = MetaDataInfo::Type::FMask});
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        surface_metas.emplace(desc.info.meta_info.htile_addr,
                              MetaDataInfo{.type = MetaDataInfo::Type::HTile,
                                           .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id =
                slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
            RegisterImage(stencil_id);
        }
        Image& image = slot_images[stencil_id];
        TouchImage(image);
        image.AssociateDepth(image_id);
    }

    return image.FindView(desc.view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        // The image size should be less than page size to be considered MaybeCpuDirty
        // So this calculation should be very uncommon and reasonably fast
        // For now we'll just check up to 64 first pixels
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u32 w = std::min(image.info.size.width, u32(8));
        const u32 h = std::min(image.info.size.height, u32(8));
        const u32 size = w * h * image.info.num_bits >> (3 + image.info.props.is_block ? 4 : 0);
        const u64 hash = XXH3_64bits(addr, size);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);

    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        if (is_gpu_modified && !is_gpu_dirty) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    if (auto barrier = in_buffer->GetBarrier(vk::AccessFlagBits2::eTransferRead,
                                             vk::PipelineStageFlagBits2::eTransfer)) {
        scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &barrier.value(),
        });
    }

    const auto [buffer, offset] =
        tile_manager.DetileImage(in_buffer->Handle(), in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    image.Upload(image_copies, buffer, offset);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base) {
    const u64 hash = XXH3_64bits(&sampler, sizeof(sampler));
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base);
    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::Registered)) {
        LOG_WARNING(Render_Vulkan,
                    "UnregisterImage: image {} already unregistered (addr={:#x}), skipping",
                    image_id.index, image.info.guest_address);
        return;
    }
    image.flags &= ~ImageFlagBits::Registered;
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(addr, size);
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        auto& image = slot_images[image_id];
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        if (tiled && download) {
            // This is a workaround for now. We can't handle non-linear image downloads.
            return false;
        }
        if (download && !pressured) {
            return false;
        }
        // FIX(GR2FORK): GC-time download disabled. Even with the per-property
        // guards in DownloadImageMemory (null backing, MSAA, 3D, depth+stencil,
        // zero-size), some image still slips through and crashes the NVIDIA
        // driver inside vkCmdCopyImageToBuffer with a read at offset 0x108.
        // The crash signature is identical across rounds and reproduces
        // reliably during pipeline-compile bursts (shader storms on zone load).
        //
        // The download writeback only matters when guest CPU code is going to
        // *read* what the GPU wrote — primarily photo mode (which has its own
        // synchronous path via ForceDownloadByAddress) and a handful of
        // GR2-internal cases. For GC-time eviction, the data being "lost"
        // would have been overwritten by the next draw anyway in the vast
        // majority of cases. Trading occasional stale guest-memory reads
        // for not-crashing is the right tradeoff right now.
        //
        // To re-enable for diagnosis: define GR2FORK_GC_DOWNLOAD_ENABLE.
#ifdef GR2FORK_GC_DOWNLOAD_ENABLE
        if (download) {
            DownloadImageMemory(image_id);
        }
#endif
        FreeImage(image_id);
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::TouchImage(const Image& image) {
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }

    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    });
}

// ── GR2 Photo Snapshot: Save/Restore photo pixels across RT reuse ────────

void TextureCache::TriggerPhotoTextureSurvey(int budget) {
    s_photo_survey_budget.store(budget, std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan, "[GR2Photo] Texture survey triggered (budget={})", budget);
}

void TextureCache::SavePhotoSnapshot(VAddr address, ImageId image_id,
                                     u32 width, u32 height, u32 pitch, u32 bpp) {
    std::lock_guard lk{photo_snapshot_mutex_};
    const u32 size = pitch * height * bpp;
    photo_snapshot_.guest_address = address;
    photo_snapshot_.image_id = image_id;
    photo_snapshot_.width = width;
    photo_snapshot_.height = height;
    photo_snapshot_.pitch = pitch;
    photo_snapshot_.bpp = bpp;
    photo_snapshot_.pixels.resize(size);
    std::memcpy(photo_snapshot_.pixels.data(), std::bit_cast<const u8*>(address), size);
    photo_snapshot_.valid = true;
    photo_snapshot_.capture_stamp = photo_rt_stamp_.load(std::memory_order_relaxed);
    photo_snapshot_.restore_count = 0;

    LOG_INFO(Render_Vulkan,
             "[GR2Photo] Saved photo snapshot: addr={:#x} {}x{} pitch={} bpp={} size={}B",
             address, width, height, pitch, bpp, size);
}

bool TextureCache::MaybeRestorePhotoSnapshot(ImageId image_id, Image& image) {
    // Quick check without lock: is there a valid snapshot?
    // (This is called on the hot path for EVERY texture binding, so we need it fast)
    {
        std::lock_guard lk{photo_snapshot_mutex_};
        if (!photo_snapshot_.valid) {
            return false;
        }

        // Check if this image matches the saved photo address
        if (image.info.guest_address != photo_snapshot_.guest_address) {
            return false;
        }

        // Check dimensions match
        if (image.info.size.width != photo_snapshot_.width ||
            image.info.size.height != photo_snapshot_.height) {
            return false;
        }

        // The image is at the photo address. Check if the GPU has overwritten it
        // since we saved the snapshot.
        if (True(image.flags & ImageFlagBits::GpuModified) &&
            False(image.flags & ImageFlagBits::CpuDirty)) {
            // The RT was reused after we saved the snapshot. Restore photo pixels
            // to CPU memory so the texture cache re-uploads them.
            const u32 size = photo_snapshot_.pitch * photo_snapshot_.height *
                             photo_snapshot_.bpp;
            u8* dst = std::bit_cast<u8*>(photo_snapshot_.guest_address);
            std::memcpy(dst, photo_snapshot_.pixels.data(), size);

            // Mark as CPU-dirty so UpdateImage → RefreshImage uploads our pixels
            image.flags |= ImageFlagBits::CpuDirty;
            image.flags &= ~ImageFlagBits::GpuModified;
            image.MarkFastStateDirty();

            photo_snapshot_.restore_count++;
            LOG_INFO(Render_Vulkan,
                     "[GR2Photo] Restored photo snapshot to {:#x} ({}x{}, restore #{})",
                     photo_snapshot_.guest_address,
                     photo_snapshot_.width, photo_snapshot_.height,
                     photo_snapshot_.restore_count);

            // Allow up to 60 restores (about 1 second of frames at 60fps)
            // then stop to avoid interfering with normal rendering
            if (photo_snapshot_.restore_count >= 60) {
                LOG_INFO(Render_Vulkan,
                         "[GR2Photo] Photo snapshot expired after {} restores",
                         photo_snapshot_.restore_count);
                photo_snapshot_.valid = false;
                photo_snapshot_.pixels.clear();
            }

            return true;
        }

        // If CpuDirty is already set, the photo pixels are still in CPU memory
        // and will be uploaded automatically by RefreshImage. No action needed.
        return false;
    }
}

} // namespace VideoCore
