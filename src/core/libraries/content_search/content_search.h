// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include "common/types.h"

namespace Core::Loader {
    class SymbolsResolver;
}

namespace Libraries::ContentSearch {

    // Called by ContentExport when a photo is exported
    void NotifyExportedContentId(const std::string& content_id);

    // Returns the host directory where screenshots are stored
    std::filesystem::path GetScreenshotHostDir();

    // Returns the next available counter value for SCREENSHOT naming
    // (legacy name — now returns the total photo count; see
    // GenerateTimestampContentId for the modern stem generator.)
    u32 GetNextPhotoCounter();

    // GR2FORK v1.0: generate a new "YYYYMMDD_HHMMSS_NNN" content id that
    // is guaranteed unique against the currently-tracked photo catalog.
    // Both jpegenc and content_export call this to produce filenames.
    std::string GenerateTimestampContentId();

    // Store/retrieve the last content ID saved by the encoder.
    void SetLastSavedContentId(const std::string& content_id);
    std::string GetLastSavedContentId();

    // Photo catalog accessors
    u32 GetExportedCount();
    std::string GetContentIdByIndex(u32 index);
    bool DeleteContentById(const std::string& content_id);

    int PS4_SYSV_ABI sceContentSearchInit(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                          u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentSearchSearchContent(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                                   u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentSearchSearchApplication(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0,
                                                       u64 a4 = 0, u64 a5 = 0, u64 a6 = 0,
                                                       u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentSearchGetMyApplicationIndex(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0,
                                                           u64 a4 = 0, u64 a5 = 0, u64 a6 = 0,
                                                           u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentSearchGetNumOfContent(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                                     u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentSearchOpenMetadata(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                                  u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentSearchOpenMetadataByContentId(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0,
                                                             u64 a4 = 0, u64 a5 = 0, u64 a6 = 0,
                                                             u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentSearchGetMetadataValue(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                                      u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentSearchGetMetadataFieldInfo(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0,
                                                          u64 a4 = 0, u64 a5 = 0, u64 a6 = 0,
                                                          u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentSearchCloseMetadata(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                                   u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentSearchTerm(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                          u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);

    void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::ContentSearch
