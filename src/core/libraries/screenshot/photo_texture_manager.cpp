// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include "common/logging/log.h"
#include "core/libraries/screenshot/photo_pixel_store.h"
#include "core/libraries/screenshot/photo_texture_manager.h"

namespace Libraries::ScreenShot {

PhotoTextureManager& PhotoTextureManager::Instance() {
    static PhotoTextureManager instance;
    return instance;
}

void PhotoTextureManager::InitFakeVtable(uintptr_t eboot_base) {
    std::lock_guard lock(mutex_);
    if (vtable_initialized_) {
        return;
    }

    eboot_base_ = eboot_base;

    // VA 0x374510 is a single `ret` (0xC3) instruction — the same stub
    // already used in the gallery widget's real vtable (slots 4-6, 11).
    // Every vtable dispatch on our fake nodes will call this `ret` and
    // return harmlessly.
    const u64 ret_gadget = eboot_base + 0x374510;
    for (int i = 0; i < 20; i++) {
        fake_vtable_[i] = ret_gadget;
    }

    vtable_initialized_ = true;
    LOG_INFO(Core,
             "[PhotoTextureManager] Fake vtable initialized: 20 slots → ret gadget at {:#x}",
             ret_gadget);
}

void PhotoTextureManager::SetupNode(std::vector<u8>& node) {
    // Node layout: [0x000..0x7FF] main node, [0x800..0x8FF] sub_desc, [0x900..0x91F] status
    u8* base = node.data();

    // node+0x00: vtable pointer → our fake vtable
    u64 vtable_ptr = reinterpret_cast<u64>(fake_vtable_);
    std::memcpy(base + 0x00, &vtable_ptr, 8);

    // node+0x20: refcount = 1000
    u32 refcount = 1000;
    std::memcpy(base + 0x20, &refcount, 4);

    // node+0x83: = 1  (skip cleanup flag in interaction handler)
    base[0x83] = 1;

    // node+0xa0: pointer to sub_desc (at offset 0x800 in our buffer).
    // The per-frame scene graph tick at VA 0xfa5f60 reads:
    //   [node+0xa0]+0xe0 = count1 (animation entries)
    //   [node+0xa0]+0xc8 = count2
    //   [node+0xa0]+0xe8 = count3
    //   [node+0xa0]+0xd8 = count4
    // All zeroed → all four loops skip safely.
    u64 sub_desc_ptr = reinterpret_cast<u64>(base + 0x800);
    std::memcpy(base + 0xa0, &sub_desc_ptr, 8);

    // node+0x150: pointer to status buffer (at offset 0x900 in our buffer).
    // 0xfa6146 writes: byte [node+0x150]+0x10 = 0 (safe with writable memory)
    u64 status_ptr = reinterpret_cast<u64>(base + 0x900);
    std::memcpy(base + 0x150, &status_ptr, 8);
}

u64 PhotoTextureManager::GetOrCreateDummyNode(const std::string& content_id) {
    std::lock_guard lock(mutex_);

    if (!vtable_initialized_) {
        LOG_ERROR(Core, "[PhotoTextureManager] GetOrCreateDummyNode called before "
                        "InitFakeVtable — returning 0");
        return 0;
    }

    // Return existing node if already created.
    auto it = photos_.find(content_id);
    if (it != photos_.end() && it->second.valid) {
        return reinterpret_cast<u64>(it->second.fake_node.data());
    }

    // Create node buffer: 0x800 (node) + 0x100 (sub_desc) + 0x20 (status) = 0x920 bytes
    ManagedPhoto photo;
    photo.fake_node.resize(0x920, 0);
    SetupNode(photo.fake_node);
    photo.valid = true;

    u64 node_addr = reinterpret_cast<u64>(photo.fake_node.data());
    photos_.emplace(content_id, std::move(photo));

    LOG_INFO(Core,
             "[PhotoTextureManager] Created dummy node for '{}' at {:#x} "
             "(vtable={:#x} sub_desc={:#x} status={:#x})",
             content_id, node_addr, reinterpret_cast<u64>(fake_vtable_),
             node_addr + 0x800, node_addr + 0x900);

    return node_addr;
}

u64 PhotoTextureManager::GetOrCreateSceneNode(const std::string& content_id) {
    std::lock_guard lock(mutex_);

    // Check if already created.
    auto it = photos_.find(content_id);
    if (it != photos_.end() && it->second.valid) {
        return reinterpret_cast<u64>(it->second.fake_node.data());
    }

    // Check if pixels are available.
    const auto* stored = PhotoPixelStore::Instance().Get(content_id);
    if (!stored || stored->pixels.empty()) {
        LOG_WARNING(Core, "[PhotoTextureManager] No pixels for '{}' in store", content_id);
        return 0;
    }

    // Create scene node: 0x800 (node) + 0x100 (sub_desc) + 0x20 (status)
    ManagedPhoto photo;
    photo.fake_node.resize(0x920, 0);

    if (vtable_initialized_) {
        SetupNode(photo.fake_node);
    } else {
        // Legacy path: just set refcount
        u32 initial_refcount = 1000;
        std::memcpy(photo.fake_node.data() + 0x20, &initial_refcount, 4);
    }

    photo.valid = true;
    u64 node_addr = reinterpret_cast<u64>(photo.fake_node.data());

    photos_.emplace(content_id, std::move(photo));

    LOG_INFO(Core, "[PhotoTextureManager] Created scene node for '{}' at {:#x} "
             "(pixels: {}x{}, {} bytes)",
             content_id, node_addr, stored->width, stored->height, stored->pixels.size());

    return node_addr;
}

bool PhotoTextureManager::HasNode(const std::string& content_id) const {
    std::lock_guard lock(mutex_);
    auto it = photos_.find(content_id);
    return it != photos_.end() && it->second.valid;
}

void PhotoTextureManager::Clear() {
    std::lock_guard lock(mutex_);
    photos_.clear();
    LOG_INFO(Core, "[PhotoTextureManager] Cleared all nodes");
}

size_t PhotoTextureManager::Count() const {
    std::lock_guard lock(mutex_);
    return photos_.size();
}

} // namespace Libraries::ScreenShot
