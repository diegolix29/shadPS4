// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <vector>
#include <boost/container/small_vector.hpp>
#include "common/slot_vector.h"
#include "common/types.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Vulkan {

class Scheduler;

/// Manages GPU→buffer-cache synchronization for compute storage image output.
/// Collects storage images during dispatch binding, records vkCmdCopyImageToBuffer
/// after dispatch, and injects the downloaded data into the buffer cache.
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

} // namespace Vulkan
