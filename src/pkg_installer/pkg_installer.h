// SPDX-FileCopyrightText: Copyright 2025-2026 shadLauncher4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct PkgMetadata {
    std::string title_id;
    uint64_t pkg_size = 0;
    uint32_t flags = 0;
    uint32_t file_count = 0;
};

using ProgressCallback = std::function<void(uint32_t extracted, uint32_t total)>;

// Opens the PKG and fills metadata. Returns error string on failure.
std::optional<std::string> ReadPkgMetadata(const std::filesystem::path& pkg_path, PkgMetadata& out);

// Extracts the PKG. If indices empty -> all files. Reports progress per extracted file.
std::optional<std::string> ExtractPkg(const std::filesystem::path& pkg_path,
                                      const std::filesystem::path& out_dir,
                                      const std::vector<int>& indices,
                                      const ProgressCallback& progress_cb);

// Helper to gather .pkg files in a directory (non-recursive or recursive as chosen).
std::vector<std::filesystem::path> ListPkgFiles(const std::filesystem::path& dir, bool recursive);
