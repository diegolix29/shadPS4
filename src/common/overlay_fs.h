// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace Common::FS {

/// Read-only merged view over loose directories and ZArchive roots. Sources are ordered from
/// highest to lowest priority.
class OverlayView {
public:
    using DirectoryEntryCallback =
        std::function<void(const std::filesystem::path& entry_path, bool is_file)>;

    explicit OverlayView(std::span<const std::filesystem::path> sources);

    /// Resolves a relative path to the first source containing it.
    std::optional<std::filesystem::path> Resolve(const std::filesystem::path& relative_path) const;

    bool Exists(const std::filesystem::path& relative_path) const;
    bool IsDirectory(const std::filesystem::path& relative_path) const;

    /// Iterates the merged directory, suppressing lower-priority entries with the same name.
    bool IterateDirectory(const std::filesystem::path& relative_path,
                          const DirectoryEntryCallback& callback) const;

private:
    std::vector<std::filesystem::path> sources;
};

} // namespace Common::FS
