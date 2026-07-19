// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Core::FileSys {

struct AdditionalContentSource {
    std::string entitlement;
    std::vector<std::filesystem::path> roots;
};

/// Discovered base, update, mod, and additional-content sources for one game.
class GameContentCatalog {
public:
    static std::optional<GameContentCatalog> Discover(
        const std::filesystem::path& launch_path,
        std::optional<std::filesystem::path> root_override = std::nullopt);

    static std::optional<std::filesystem::path> FindGameById(const std::filesystem::path& directory,
                                                             const std::string& game_id,
                                                             int max_depth);

    const std::filesystem::path& GetBaseRoot() const {
        return base_root;
    }

    const std::filesystem::path& GetContainerRoot() const {
        return container_root;
    }

    const std::string& GetTitleId() const {
        return title_id;
    }

    const std::vector<std::filesystem::path>& GetPatchRoots() const {
        return patch_roots;
    }

    const std::vector<std::filesystem::path>& GetModRoots() const {
        return mod_roots;
    }

    std::vector<std::filesystem::path> GetAppSources(bool ignore_patches = false) const;
    std::optional<std::filesystem::path> ResolveAppPath(const std::filesystem::path& relative_path,
                                                        bool ignore_patches = false) const;

    /// Combines configured external DLC with DLC embedded in the game container.
    std::vector<AdditionalContentSource> DiscoverAdditionalContent(
        const std::filesystem::path& addon_install_directory) const;

private:
    std::filesystem::path base_root;
    std::filesystem::path container_root;
    std::string title_id;
    std::vector<std::filesystem::path> patch_roots;
    std::vector<std::filesystem::path> mod_roots;
    std::vector<AdditionalContentSource> embedded_additional_content;
};

} // namespace Core::FileSys
