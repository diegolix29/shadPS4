// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "common/types.h"

namespace Libraries::ScreenShot {

/// Manages photo-to-GNF conversion for the GR2 gallery.
///
/// The CDtex/FAM system loads film_dummy.gnf as the paw-print placeholder
/// texture for each gallery slot. This class:
///   1. Decodes JPEG screenshots to RGBA pixels
///   2. Builds valid GNF files (256×256 RGBA8 Unorm, linear tiling)
///   3. Writes them to disk as slot-indexed files
///   4. Provides atomic slot counter for per-open file substitution
///
/// The file system hook in sceKernelOpen redirects film_dummy.gnf reads
/// to the appropriate slot GNF file, causing each slot to display a
/// different photo.
class PhotoGnfManager {
public:
    static PhotoGnfManager& Instance();

    static constexpr int MAX_SLOTS = 15;
    static constexpr u32 PHOTO_WIDTH = 512;
    static constexpr u32 PHOTO_HEIGHT = 512;
    static constexpr u32 PIXEL_DATA_SIZE = PHOTO_WIDTH * PHOTO_HEIGHT * 4; // RGBA8

    /// Prepare GNF files for up to 10 photo slots.
    /// Decodes JPEGs from the screenshot directory, builds GNFs, writes to disk.
    /// @param screenshot_dir  Host path to the screenshot directory
    /// @param content_ids     Ordered list of content IDs (max 10 used)
    /// @param page_offset     Index of the first photo on the current page
    void PrepareSlots(const std::filesystem::path& screenshot_dir,
                      const std::vector<std::string>& content_ids,
                      u32 page_offset = 0);

    /// Reset the slot counter (call before PhotoCreator / FAM loading starts).
    void ResetSlotCounter();

    /// Get the next slot GNF file path. Called by the file system hook.
    /// Returns empty path if no photo available for this slot.
    /// Atomically increments the internal counter.
    std::filesystem::path ConsumeNextSlotPath();

    /// Check if photo GNFs are prepared and ready for substitution.
    bool IsReady() const { return ready_.load(); }

    /// Number of photo slots that have valid GNF files.
    int PreparedCount() const { return prepared_count_; }

    /// Get the output directory where GNF files are stored.
    std::filesystem::path GetGnfDir() const;

    /// Build a GNF file buffer from RGBA pixel data.
    /// @param rgba_pixels  Pointer to PHOTO_WIDTH × PHOTO_HEIGHT × 4 bytes (RGBA8)
    /// @return Complete GNF file as byte vector (header + pixels)
    static std::vector<u8> BuildGnf(const u8* rgba_pixels);

    /// Build a solid-color test GNF (for debugging).
    static std::vector<u8> BuildSolidGnf(u8 r, u8 g, u8 b, u8 a = 255);

private:
    PhotoGnfManager() = default;

    bool DecodeAndWriteSlot(int slot_index,
                            const std::filesystem::path& jpeg_path);

    mutable std::mutex mutex_;
    std::atomic<int> slot_counter_{0};
    std::atomic<bool> ready_{false};
    int prepared_count_{0};
    bool slot_valid_[MAX_SLOTS]{};
    std::filesystem::path gnf_dir_;
};

} // namespace Libraries::ScreenShot
