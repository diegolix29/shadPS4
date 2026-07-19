// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <set>
#include <string>

#include "common/overlay_fs.h"
#include "common/path_util.h"
#include "common/string_util.h"
#include "common/zar_fs.h"

namespace Common::FS {

namespace fs = std::filesystem;

namespace {

bool IsArchiveSource(const fs::path& root) {
    return Zar::IsZarArchive(root) || Zar::IsZarInnerPath(root);
}

bool IsSafeRelativePath(const fs::path& path) {
    if (path.has_root_path()) {
        return false;
    }
    return std::ranges::none_of(
        path, [](const fs::path& component) { return PathToUTF8String(component) == ".."; });
}

std::optional<fs::path> ResolveLoosePath(const fs::path& root, const fs::path& relative_path) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return std::nullopt;
    }

    auto current = root;
    for (const auto& part : relative_path.lexically_normal()) {
        const auto name = PathToUTF8String(part);
        if (name.empty() || name == ".") {
            continue;
        }
        if (name == "..") {
            return std::nullopt;
        }

        ec.clear();
        if (!fs::is_directory(current, ec)) {
            return std::nullopt;
        }

        const auto lower_name = Common::ToLower(name);
        fs::directory_iterator iterator{current, ec};
        const fs::directory_iterator end;
        std::optional<fs::path> case_insensitive_match;
        while (!ec && iterator != end) {
            const auto candidate = iterator->path().filename();
            const auto candidate_name = PathToUTF8String(candidate);
            if (candidate_name == name) {
                current /= candidate;
                case_insensitive_match.reset();
                break;
            }
            if (!case_insensitive_match && Common::ToLower(candidate_name) == lower_name) {
                case_insensitive_match = candidate;
            }
            iterator.increment(ec);
        }
        if (ec) {
            return std::nullopt;
        }
        if (iterator == end) {
            if (!case_insensitive_match) {
                return std::nullopt;
            }
            current /= *case_insensitive_match;
        }
    }
    return current;
}

std::optional<fs::path> ResolveSourcePath(const fs::path& root, const fs::path& relative_path) {
    if (IsArchiveSource(root)) {
        const auto candidate = root / relative_path;
        return Zar::Exists(candidate) ? std::optional{candidate} : std::nullopt;
    }
    return ResolveLoosePath(root, relative_path);
}

} // Anonymous namespace

OverlayView::OverlayView(std::span<const fs::path> sources_)
    : sources{sources_.begin(), sources_.end()} {}

std::optional<fs::path> OverlayView::Resolve(const fs::path& relative_path) const {
    if (!IsSafeRelativePath(relative_path)) {
        return std::nullopt;
    }
    for (const auto& source : sources) {
        if (const auto resolved = ResolveSourcePath(source, relative_path)) {
            return resolved;
        }
    }
    return std::nullopt;
}

bool OverlayView::Exists(const fs::path& relative_path) const {
    return Resolve(relative_path).has_value();
}

bool OverlayView::IsDirectory(const fs::path& relative_path) const {
    const auto resolved = Resolve(relative_path);
    return resolved && Zar::IsDirectory(*resolved);
}

bool OverlayView::IterateDirectory(const fs::path& relative_path,
                                   const DirectoryEntryCallback& callback) const {
    if (!IsDirectory(relative_path)) {
        return false;
    }

    std::set<std::string> visited;
    for (const auto& source : sources) {
        const auto source_directory = ResolveSourcePath(source, relative_path);
        if (!source_directory || !Zar::IsDirectory(*source_directory)) {
            continue;
        }
        Zar::IterateDirectory(*source_directory, [&](const fs::path& entry_path, bool is_file) {
            auto name = Common::ToLower(PathToUTF8String(entry_path.filename()));
            if (visited.emplace(std::move(name)).second) {
                callback(entry_path, is_file);
            }
        });
    }
    return true;
}

} // namespace Common::FS
