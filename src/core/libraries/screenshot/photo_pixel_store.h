// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/types.h"

namespace Libraries::ScreenShot {

/// Stored photo pixel data — BGRA8 format, ready for GPU upload.
struct StoredPhoto {
    std::vector<u8> pixels; // BGRA8
    u32 width{};
    u32 height{};
};

/// Thread-safe key-value store mapping content_id → pixel data.
/// Used to hold captured frame pixels in memory so they can be
/// uploaded to GPU textures when the gallery needs them.
///
/// Photos are stored in BGRA8 format (matching the presenter's
/// swapchain surface format) to avoid redundant conversions.
class PhotoPixelStore {
public:
    static PhotoPixelStore& Instance();

    /// Store BGRA8 pixel data for a content_id. Evicts oldest if over capacity.
    void Store(const std::string& content_id, std::vector<u8>&& pixels, u32 w, u32 h);

    /// Retrieve stored photo. Returns nullptr if not found.
    const StoredPhoto* Get(const std::string& content_id) const;

    /// Check if a content_id has stored pixels.
    bool Has(const std::string& content_id) const;

    /// Remove a specific content_id.
    void Remove(const std::string& content_id);

    /// Remove all stored photos.
    void Clear();

    /// Number of photos currently stored.
    size_t Count() const;

private:
    PhotoPixelStore() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, StoredPhoto> store_;

    // LRU tracking: most recently used at the back.
    std::vector<std::string> lru_order_;

    // Memory budget: ~200MB at 1080p BGRA (1920*1080*4 = 8.3MB per photo)
    static constexpr size_t kMaxPhotos = 50;

    void EvictIfNeeded();
    void TouchLru(const std::string& content_id);
};

} // namespace Libraries::ScreenShot
