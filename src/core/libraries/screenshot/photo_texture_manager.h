// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/types.h"

namespace Libraries::ScreenShot {

/// Manages fake scene graph nodes for gallery photo items.
///
/// The game's gallery interaction handler (VA 0xc24d20) gates on item+0x968
/// being non-NULL. Without a valid node pointer, gallery items are not
/// selectable. On real PS4 the screenshot service provides texture nodes;
/// in shadPS4 we create minimal "dummy" nodes that satisfy all safety checks:
///
/// - node+0x00: vtable pointer → static fake vtable where every slot points
///   to a `ret` gadget (VA 0x374510), making all vtable dispatches harmless.
/// - node+0x20: high refcount (1000) to prevent release-on-zero paths.
/// - node+0x70: zeroed — atomic CAS in render callback operates on zero safely.
/// - node+0x83: set to 1 — skips the dangerous cleanup path in the interaction
///   handler at 0xc24f77 (cmp byte [node+0x83], 0 / jne skip_cleanup).
/// - node+0x2A0, +0x328: writable — renderer writes position/dirty flags here.
class PhotoTextureManager {
public:
    static PhotoTextureManager& Instance();

    /// Initialize the fake vtable using the eboot runtime base address.
    /// Must be called once before any GetOrCreateDummyNode calls.
    /// All 20 vtable slots point to the `ret` instruction at base+0x374510.
    void InitFakeVtable(uintptr_t eboot_base);

    /// Create or retrieve a dummy scene node for the given content_id.
    /// Does NOT require pixel data — creates a minimal node that passes
    /// all renderer/interaction handler safety checks.
    /// Returns the node pointer (host address), or 0 if vtable not initialized.
    u64 GetOrCreateDummyNode(const std::string& content_id);

    /// Original method: creates node backed by pixel data from PhotoPixelStore.
    /// Returns 0 if pixels aren't available.
    u64 GetOrCreateSceneNode(const std::string& content_id);

    /// Check if a node exists for this content_id.
    bool HasNode(const std::string& content_id) const;

    /// Release all nodes.
    void Clear();

    /// Number of nodes currently managed.
    size_t Count() const;

private:
    PhotoTextureManager() = default;

    void SetupNode(std::vector<u8>& node);

    struct ManagedPhoto {
        std::vector<u8> fake_node;  // 0x800 bytes
        bool valid{false};
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ManagedPhoto> photos_;

    /// Static fake vtable: 20 slots, each pointing to a `ret` gadget.
    /// Initialized once via InitFakeVtable().
    u64 fake_vtable_[20] = {};
    bool vtable_initialized_ = false;
    uintptr_t eboot_base_ = 0;
};

} // namespace Libraries::ScreenShot
