// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/storage_image_sync.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

StorageImageSync::StorageImageSync(Scheduler& scheduler_, VideoCore::BufferCache& buffer_cache_,
                                   VideoCore::TextureCache& texture_cache_)
    : scheduler{scheduler_}, buffer_cache{buffer_cache_}, texture_cache{texture_cache_} {}

StorageImageSync::~StorageImageSync() = default;

void StorageImageSync::Clear() {
    pending_syncs.clear();
}

void StorageImageSync::Collect(VideoCore::ImageId image_id, const VideoCore::Image& image) {
    // Deduplicate by guest_addr — the same storage image may be bound to multiple
    // descriptor set slots in a single dispatch.
    const VAddr addr = image.info.guest_address;
    if (addr == 0) return;

    for (const auto& s : pending_syncs) {
        if (s.guest_addr == addr) {
            LOG_DEBUG(Render_Vulkan, "[StorageSync] duplicate binding guest={:#x} skipped", addr);
            return;
        }
    }

    pending_syncs.push_back({
        .image_id = image_id,
        .guest_addr = addr,
        .guest_size = image.info.guest_size,
        .width = image.info.size.width,
        .height = image.info.size.height,
        .layers = image.info.resources.layers,
        .num_samples = image.info.num_samples,
        .num_mips = image.info.resources.levels,
        .pitch = image.info.pitch,
        .pixel_format = image.info.pixel_format,
    });

    LOG_DEBUG(Render_Vulkan,
              "[StorageSync] collected guest={:#x} {}x{} fmt={} layers={} "
              "mips={} samples={} pitch={} size={}",
              image.info.guest_address, image.info.size.width, image.info.size.height,
              vk::to_string(image.info.pixel_format), image.info.resources.layers,
              image.info.resources.levels, image.info.num_samples, image.info.pitch,
              image.info.guest_size);
}

bool StorageImageSync::HasPending() const {
    return !pending_syncs.empty();
}

u32 StorageImageSync::PendingCount() const {
    return static_cast<u32>(pending_syncs.size());
}

void StorageImageSync::DiscardPending() {
    pending_syncs.clear();
}

void StorageImageSync::RecordDownload(bool async) {
    if (pending_syncs.empty()) return;

    const u32 sync_count = static_cast<u32>(pending_syncs.size());
    LOG_DEBUG(Render_Vulkan, "[StorageSync] recording {} storage image download(s)", sync_count);

    // Phase 4b: create placeholder buffers to set up read/write protection on the
    // pending ranges before the async window opens. ObtainBuffer(is_written=true)
    // registers the buffer in page_table, sets GPU dirty (read protection), clears
    // CPU dirty (write protection), and adds gpu_modified_ranges. WriteDataBuffer is
    // deferred until force-sync or OnSubmit.
    if (async) {
        std::scoped_lock lk{pending_mutex};
        for (const auto& sync : pending_syncs) {
            std::ignore = buffer_cache.ObtainBuffer(sync.guest_addr,
                                                      static_cast<u32>(sync.guest_size), true);
            pending_ranges.Add(sync.guest_addr, sync.guest_size);
        }
    }

    auto& download_buf = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Download);

    // Transit storage images to transfer-src on the dispatch command buffer.
    // Pipeline barriers ensure CS writes are visible to the subsequent copy.
    for (auto& sync : pending_syncs) {
        auto& img = texture_cache.GetImage(sync.image_id);
        img.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    }

    // Submit dispatch + barriers, get a fresh command buffer.
    scheduler.EndRendering();
    auto cmdbuf = scheduler.CommandBuffer();

    // Record vkCmdCopyImageToBuffer for each storage image on the same cmdbuf.
    // Staging pointers remain valid until insert phase — StreamBuffer watch protects them.
    boost::container::small_vector<StagingRef, 4> staging_refs;

    for (auto& sync : pending_syncs) {
        const auto& img = texture_cache.GetImage(sync.image_id);
        const u32 bpp = img.info.num_bits / 8u;
        const u32 row_length = sync.pitch ? sync.pitch : sync.width;
        const u32 download_size = row_length * sync.height * sync.layers * bpp;

        LOG_DEBUG(Render_Vulkan,
                  "[StorageSync] download guest={:#x} {}x{} layers={} bpp={} row_len={} "
                  "size={}",
                  sync.guest_addr, sync.width, sync.height, sync.layers, bpp, row_length,
                  download_size);

        boost::container::small_vector<vk::BufferImageCopy, 6> regions;
        const u32 layer_size = row_length * sync.height * bpp;
        for (u32 layer = 0; layer < sync.layers; ++layer) {
            regions.push_back({
                .bufferOffset = layer * layer_size,
                .bufferRowLength = row_length,
                .bufferImageHeight = 0,
                .imageSubresource =
                    {
                        .aspectMask = img.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                        .mipLevel = 0,
                        .baseArrayLayer = layer,
                        .layerCount = 1,
                    },
                .imageOffset = {0, 0, 0},
                .imageExtent = {sync.width, sync.height, 1},
            });
        }

        const auto [data, offset] = download_buf.Map(download_size);
        if (!data) {
            LOG_ERROR(Render_Vulkan,
                      "[StorageSync] StreamBuffer Map failed for {}B — download SKIPPED, "
                      "texture corruption likely",
                      download_size);
            continue;
        }
        for (auto& region : regions) {
            region.bufferOffset += offset;
        }
        download_buf.Commit();
        staging_refs.push_back({data, download_size});

        // Record copy directly — image is already in eTransferSrcOptimal from Transit above.
        cmdbuf.copyImageToBuffer(img.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                  download_buf.Handle(), regions);
    }

    if (staging_refs.empty()) {
        LOG_ERROR(Render_Vulkan,
                  "[StorageSync] all {} Map()s failed — nothing to submit, texture corruption "
                  "likely",
                  sync_count);
        pending_syncs.clear();
        return;
    }

    if (async) {
        // Submit copies without waiting — injection deferred to OnSubmit.
        scheduler.EndRendering();
        const u64 tick = scheduler.CurrentTick();
        scheduler.Flush();

        LOG_DEBUG(Render_Vulkan, "[StorageSync] submitted {} downloads at tick {}, pending total={}",
                  staging_refs.size(), tick, pending_downloads.size() + 1);

        pending_downloads.push_back({
            .tick = tick,
            .staging_refs = std::move(staging_refs),
            .syncs = std::move(pending_syncs),
        });
        // pending_syncs is now empty (moved-from)
    } else {
        // Sync mode: submit and wait immediately, then inject data into buffer cache.
        scheduler.EndRendering();
        const u64 tick = scheduler.CurrentTick();
        scheduler.Wait(tick);

        for (size_t i = 0; i < staging_refs.size(); ++i) {
            auto& sync = pending_syncs[i];
            auto& st = staging_refs[i];
            if (!st.data || st.size == 0) continue;
            buffer_cache.InsertGpuData(sync.guest_addr, st.data, st.size);
            texture_cache.InvalidateMemoryRange(sync.guest_addr, sync.guest_size,
                                                /*exclude_producer=*/sync.image_id);
        }
        pending_syncs.clear();
    }
}

void StorageImageSync::ProcessPending() {
    // Take ownership of pending state under the mutex, then release the lock
    // before doing any GPU Wait operations.
    decltype(pending_downloads) downloads;
    {
        std::scoped_lock lk{pending_mutex};
        if (pending_downloads.empty()) return;
        downloads = std::move(pending_downloads);
        pending_ranges.Clear();
    }

    const size_t batch_count = downloads.size();
    u32 total_syncs = 0;
    for (const auto& dl : downloads) {
        total_syncs += static_cast<u32>(dl.syncs.size());
    }

    LOG_DEBUG(Render_Vulkan, "[StorageSync] processing {} batch(es), {} sync(s) total",
             batch_count, total_syncs);

    u64 wait_start = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    u64 total_wait_us = 0;

    for (auto& dl : downloads) {
        if (!scheduler.IsFree(dl.tick)) {
            const u64 single_wait_start =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            scheduler.Wait(dl.tick);
            const u64 single_wait_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count() -
                single_wait_start;
            total_wait_us += single_wait_us;
            LOG_DEBUG(Render_Vulkan, "[StorageSync] waited {} us for tick={}", single_wait_us,
                      dl.tick);
        }

        for (size_t i = 0; i < dl.syncs.size(); ++i) {
            auto& sync = dl.syncs[i];
            auto& st = dl.staging_refs[i];

            if (!st.data || st.size == 0) {
                LOG_ERROR(Render_Vulkan,
                          "[StorageSync] invalid staging ref idx={} guest={:#x} — SKIPPED, "
                          "texture corruption likely",
                          i, sync.guest_addr);
                continue;
            }

            buffer_cache.InsertGpuData(sync.guest_addr, st.data, st.size);

            const u32 count =
                texture_cache.InvalidateMemoryRange(sync.guest_addr, sync.guest_size,
                                                    /*exclude_producer=*/sync.image_id);
            LOG_DEBUG(Render_Vulkan,
                      "[StorageSync] injected guest={:#x} {}B, {} consumer(s) marked",
                      sync.guest_addr, st.size, count);
        }
    }

    const u64 total_wait_end =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    LOG_DEBUG(Render_Vulkan,
             "[StorageSync] batch complete: {} batch(es), total wait {} us, {} sync(s) injected",
             batch_count, total_wait_end - wait_start, total_syncs);
}

void StorageImageSync::InstallPreAccessCallback() {
    buffer_cache.SetPreAccessCallback([this](VAddr addr, u64 size) {
        std::unique_lock lk{pending_mutex};
        if (!pending_ranges.Intersects(addr, size)) {
            return;
        }
        // Force-sync ALL pending downloads — any overlap means the cache is about to
        // access a range whose GPU data is not yet injected. Flushing everything is
        // simpler and safer than trying to match individual ranges.
        LOG_DEBUG(Render_Vulkan,
                 "[StorageSync] force-sync triggered by access to {:#x} ({} pending batch(es))",
                 addr, pending_downloads.size());
        auto all = std::move(pending_downloads);
        pending_ranges.Clear();
        // Transfer ownership out of the lock scope before doing GPU Wait operations.
        lk.unlock();

        int waited = 0;
        u64 total_wait_us = 0;
        for (auto& dl : all) {
            if (!scheduler.IsFree(dl.tick)) {
                const u64 t0 = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
                scheduler.Wait(dl.tick);
                total_wait_us += std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count() -
                                 t0;
                ++waited;
            }
            for (size_t i = 0; i < dl.syncs.size(); ++i) {
                const auto& sync = dl.syncs[i];
                const auto& st = dl.staging_refs[i];
                if (!st.data || st.size == 0) {
                    LOG_ERROR(Render_Vulkan,
                              "[StorageSync] force-sync invalid staging ref idx={} guest={:#x}",
                              i, sync.guest_addr);
                    continue;
                }
                // InsertGpuData internally calls ObtainBuffer (FindBuffer + overlap
                // resolution) + WriteDataBuffer, handling any buffer merges that
                // occurred since the placeholder was created.
                // NOTE: InvalidateMemoryRange skipped here — force-sync may be
                // called from ObtainBufferForImage under texture_cache.mutex.
                // Consumer marking is deferred to OnSubmit batch processing.
                buffer_cache.InsertGpuData(sync.guest_addr, st.data, st.size);
                LOG_DEBUG(Render_Vulkan,
                          "[StorageSync] force-sync injected guest={:#x} {}B", sync.guest_addr,
                          st.size);
            }
        }
        LOG_DEBUG(Render_Vulkan,
                 "[StorageSync] force-sync complete: {} batch(es), {} waited, "
                 "total wait {} us",
                 all.size(), waited, total_wait_us);
    });
}

} // namespace Vulkan
