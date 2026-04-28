// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "common/logging/log.h"
#include "core/libraries/screenshot/photo_pixel_store.h"

namespace Libraries::ScreenShot {

PhotoPixelStore& PhotoPixelStore::Instance() {
    static PhotoPixelStore instance;
    return instance;
}

void PhotoPixelStore::Store(const std::string& content_id, std::vector<u8>&& pixels,
                            u32 w, u32 h) {
    std::lock_guard lock(mutex_);

    // If already exists, update in place.
    auto it = store_.find(content_id);
    if (it != store_.end()) {
        it->second.pixels = std::move(pixels);
        it->second.width = w;
        it->second.height = h;
        TouchLru(content_id);
        LOG_INFO(Core, "[PhotoPixelStore] Updated '{}' ({}x{}, {} bytes)",
                 content_id, w, h, it->second.pixels.size());
        return;
    }

    EvictIfNeeded();

    StoredPhoto photo;
    photo.pixels = std::move(pixels);
    photo.width = w;
    photo.height = h;
    const size_t byte_size = photo.pixels.size();
    store_.emplace(content_id, std::move(photo));
    lru_order_.push_back(content_id);

    LOG_INFO(Core, "[PhotoPixelStore] Stored '{}' ({}x{}, {} bytes, total={})",
             content_id, w, h, byte_size, store_.size());
}

const StoredPhoto* PhotoPixelStore::Get(const std::string& content_id) const {
    std::lock_guard lock(mutex_);
    auto it = store_.find(content_id);
    if (it == store_.end()) {
        return nullptr;
    }
    return &it->second;
}

bool PhotoPixelStore::Has(const std::string& content_id) const {
    std::lock_guard lock(mutex_);
    return store_.count(content_id) > 0;
}

void PhotoPixelStore::Remove(const std::string& content_id) {
    std::lock_guard lock(mutex_);
    store_.erase(content_id);
    auto it = std::find(lru_order_.begin(), lru_order_.end(), content_id);
    if (it != lru_order_.end()) {
        lru_order_.erase(it);
    }
    LOG_INFO(Core, "[PhotoPixelStore] Removed '{}' (total={})", content_id, store_.size());
}

void PhotoPixelStore::Clear() {
    std::lock_guard lock(mutex_);
    store_.clear();
    lru_order_.clear();
    LOG_INFO(Core, "[PhotoPixelStore] Cleared all entries");
}

size_t PhotoPixelStore::Count() const {
    std::lock_guard lock(mutex_);
    return store_.size();
}

void PhotoPixelStore::EvictIfNeeded() {
    // Caller must hold mutex_.
    while (store_.size() >= kMaxPhotos && !lru_order_.empty()) {
        const auto& oldest = lru_order_.front();
        LOG_INFO(Core, "[PhotoPixelStore] Evicting '{}' (at capacity {})", oldest, kMaxPhotos);
        store_.erase(oldest);
        lru_order_.erase(lru_order_.begin());
    }
}

void PhotoPixelStore::TouchLru(const std::string& content_id) {
    // Caller must hold mutex_. Move content_id to the back (most recent).
    auto it = std::find(lru_order_.begin(), lru_order_.end(), content_id);
    if (it != lru_order_.end()) {
        lru_order_.erase(it);
    }
    lru_order_.push_back(content_id);
}

} // namespace Libraries::ScreenShot
