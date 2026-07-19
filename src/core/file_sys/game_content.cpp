// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <charconv>
#include <compare>
#include <map>
#include <span>
#include <string_view>
#include <utility>

#include "common/logging/log.h"
#include "common/overlay_fs.h"
#include "common/path_util.h"
#include "common/string_util.h"
#include "common/zar_fs.h"
#include "core/file_format/psf.h"
#include "core/file_sys/game_content.h"

namespace Core::FileSys {

namespace fs = std::filesystem;
namespace Zar = Common::FS::Zar;

namespace {

constexpr size_t entitlement_label_offset = 20;
constexpr size_t entitlement_label_size = 16;

enum class ContentKind {
    Base,
    Patch,
    Additional,
    Unknown,
};

struct Version {
    std::vector<u32> parts;

    auto operator<=>(const Version& other) const {
        const auto count = std::max(parts.size(), other.parts.size());
        for (size_t i = 0; i < count; ++i) {
            const auto lhs = i < parts.size() ? parts[i] : 0;
            const auto rhs = i < other.parts.size() ? other.parts[i] : 0;
            if (lhs != rhs) {
                return lhs <=> rhs;
            }
        }
        return std::strong_ordering::equal;
    }

    bool operator==(const Version& other) const {
        return (*this <=> other) == 0;
    }
};

struct ContentRoot {
    fs::path root;
    ContentKind kind{};
    std::string title_id;
    std::string content_id;
    std::string app_version;
    std::optional<Version> version;
    int priority{};
};

std::optional<Version> ParseVersion(std::string_view text) {
    Version version;
    while (!text.empty()) {
        const auto separator = text.find('.');
        const auto component = text.substr(0, separator);
        if (component.empty()) {
            return std::nullopt;
        }
        u32 value{};
        const auto [end, error] =
            std::from_chars(component.data(), component.data() + component.size(), value);
        if (error != std::errc{} || end != component.data() + component.size()) {
            return std::nullopt;
        }
        version.parts.push_back(value);
        if (separator == std::string_view::npos) {
            break;
        }
        text.remove_prefix(separator + 1);
    }
    return version.parts.empty() ? std::nullopt : std::optional{std::move(version)};
}

std::string CopyString(const std::optional<std::string_view>& value) {
    return value ? std::string{*value} : std::string{};
}

std::optional<std::string_view> GetContentTitleId(std::string_view content_id) {
    if (content_id.size() != entitlement_label_offset + entitlement_label_size ||
        content_id[6] != '-' || content_id[16] != '_' || content_id[19] != '-') {
        return std::nullopt;
    }
    return content_id.substr(7, 9);
}

bool HasValidIdentity(const ContentRoot& content) {
    if (content.kind != ContentKind::Additional && content.title_id.empty()) {
        LOG_WARNING(Common_Filesystem, "Ignoring content without TITLE_ID at {}",
                    Common::FS::PathToUTF8String(content.root));
        return false;
    }
    if (content.content_id.empty()) {
        if (content.kind == ContentKind::Additional) {
            LOG_WARNING(Common_Filesystem, "Ignoring DLC without CONTENT_ID at {}",
                        Common::FS::PathToUTF8String(content.root));
            return false;
        }
        return true;
    }

    const auto content_title_id = GetContentTitleId(content.content_id);
    if (!content_title_id || (!content.title_id.empty() && *content_title_id != content.title_id)) {
        LOG_WARNING(Common_Filesystem,
                    "Ignoring content with inconsistent TITLE_ID/CONTENT_ID at {}",
                    Common::FS::PathToUTF8String(content.root));
        return false;
    }
    return true;
}

std::optional<ContentRoot> InspectContentRoot(const fs::path& root) {
    const auto param_path = root / "sce_sys" / "param.sfo";
    if (!Zar::IsRegularFile(param_path) || Zar::GetFileSize(param_path) < sizeof(PSFHeader)) {
        return std::nullopt;
    }

    PSF param;
    if (!param.Open(param_path)) {
        LOG_WARNING(Common_Filesystem, "Ignoring content with invalid param.sfo at {}",
                    Common::FS::PathToUTF8String(root));
        return std::nullopt;
    }

    ContentRoot content{.root = root};
    const auto category = Common::ToLower(CopyString(param.GetString("CATEGORY")));
    content.title_id = CopyString(param.GetString("TITLE_ID"));
    content.content_id = CopyString(param.GetString("CONTENT_ID"));
    content.app_version = CopyString(param.GetString("APP_VER"));
    content.version = ParseVersion(content.app_version);

    if (category == "gp") {
        content.kind = ContentKind::Patch;
    } else if (category.starts_with("ac")) {
        content.kind = ContentKind::Additional;
    } else if (Zar::IsRegularFile(root / "eboot.bin")) {
        content.kind = ContentKind::Base;
    } else {
        content.kind = ContentKind::Unknown;
    }
    if (!HasValidIdentity(content)) {
        return std::nullopt;
    }
    return content;
}

void AddCandidate(std::vector<fs::path>& candidates, const fs::path& path) {
    if (std::ranges::find(candidates, path) == candidates.end()) {
        candidates.push_back(path);
    }
}

std::vector<fs::path> GetContainerCandidates(const fs::path& container,
                                             const fs::path& selected_root) {
    std::vector<fs::path> candidates;
    AddCandidate(candidates, container);
    Zar::IterateDirectory(container, [&](const fs::path& entry_path, bool is_file) {
        if (!is_file || Zar::IsZarArchive(entry_path)) {
            AddCandidate(candidates, entry_path);
        }
    });
    AddCandidate(candidates, selected_root);
    return candidates;
}

bool MatchesTitle(const ContentRoot& content, std::string_view title_id) {
    if (!content.title_id.empty() && content.title_id != title_id) {
        return false;
    }
    if (!content.content_id.empty()) {
        const auto content_title_id = GetContentTitleId(content.content_id);
        if (!content_title_id || *content_title_id != title_id) {
            return false;
        }
    }
    return content.kind == ContentKind::Additional || content.title_id == title_id;
}

std::optional<std::string> GetEntitlement(const ContentRoot& content) {
    if (!GetContentTitleId(content.content_id)) {
        return std::nullopt;
    }
    return content.content_id.substr(entitlement_label_offset);
}

std::optional<fs::path> FindSiblingBase(const fs::path& selected_root) {
    auto stem = Zar::GetArchivePath(selected_root).value_or(selected_root);
    if (Zar::IsZarArchive(stem)) {
        stem.replace_extension();
    }

    auto name = Common::FS::PathToUTF8String(stem.filename());
    const auto lower_name = Common::ToLower(name);
    constexpr std::array suffixes{std::string_view{"-update"}, std::string_view{"-patch"},
                                  std::string_view{"-mods"}};
    for (const auto suffix : suffixes) {
        if (!lower_name.ends_with(suffix)) {
            continue;
        }
        stem = stem.parent_path() / name.substr(0, name.size() - suffix.size());
        if (Zar::IsDirectory(stem)) {
            return stem;
        }
        auto archive = stem;
        archive += ".zar";
        if (Zar::IsDirectory(archive)) {
            return archive;
        }
        break;
    }
    return std::nullopt;
}

void AddUniquePath(std::vector<fs::path>& paths, const fs::path& path) {
    if (std::ranges::find(paths, path) == paths.end()) {
        paths.push_back(path);
    }
}

std::vector<fs::path> GetCompanionAnchors(const fs::path& container, const fs::path& base,
                                          std::string_view title_id) {
    std::vector<fs::path> anchors;
    AddUniquePath(anchors, container);
    if (!Zar::GetArchivePath(base)) {
        AddUniquePath(anchors, base);
    }
    AddUniquePath(anchors, container.parent_path() / title_id);
    return anchors;
}

void AddCompanionContent(std::vector<ContentRoot>& contents, std::span<const fs::path> anchors,
                         std::string_view suffix, int priority) {
    for (const auto& anchor : anchors) {
        const auto companions = Zar::FindCompanionPaths(anchor, suffix);
        for (const auto& companion : companions) {
            if (auto content = InspectContentRoot(companion)) {
                content->priority = priority + (Zar::GetArchivePath(companion) ? 1 : 0);
                contents.push_back(std::move(*content));
            }
        }
    }
}

void AddAdditionalContent(std::map<std::string, std::vector<std::pair<int, fs::path>>>& grouped,
                          const ContentRoot& content, int priority, std::string_view title_id) {
    if (content.kind != ContentKind::Additional) {
        return;
    }
    if (!MatchesTitle(content, title_id)) {
        LOG_WARNING(Common_Filesystem, "Ignoring DLC for another title at {}",
                    Common::FS::PathToUTF8String(content.root));
        return;
    }
    const auto entitlement = GetEntitlement(content);
    if (!entitlement) {
        LOG_WARNING(Common_Filesystem, "Ignoring DLC with malformed CONTENT_ID at {}",
                    Common::FS::PathToUTF8String(content.root));
        return;
    }
    grouped[*entitlement].emplace_back(priority, content.root);
}

std::vector<AdditionalContentSource> BuildAdditionalContentList(
    std::map<std::string, std::vector<std::pair<int, fs::path>>> grouped) {
    std::vector<AdditionalContentSource> result;
    result.reserve(grouped.size());
    for (auto& [entitlement, roots] : grouped) {
        std::ranges::sort(roots, [](const auto& lhs, const auto& rhs) {
            if (lhs.first != rhs.first) {
                return lhs.first < rhs.first;
            }
            return lhs.second.generic_string() < rhs.second.generic_string();
        });
        AdditionalContentSource content{.entitlement = std::move(entitlement)};
        for (auto& root : roots) {
            AddUniquePath(content.roots, root.second);
        }
        result.push_back(std::move(content));
    }
    return result;
}

} // Anonymous namespace

std::optional<GameContentCatalog> GameContentCatalog::Discover(
    const fs::path& launch_path, std::optional<fs::path> root_override) {
    auto selected_root = root_override.value_or(launch_path);
    if (!Zar::IsZarArchive(selected_root) && !Zar::IsDirectory(selected_root)) {
        selected_root = selected_root.parent_path();
    }

    auto container = selected_root;
    if (const auto archive = Zar::GetArchivePath(selected_root)) {
        container = *archive;
    }

    std::vector<ContentRoot> contents;
    for (const auto& candidate : GetContainerCandidates(container, selected_root)) {
        if (auto content = InspectContentRoot(candidate)) {
            content->priority = 40;
            contents.push_back(std::move(*content));
        }
    }

    std::vector<ContentRoot*> bases;
    for (auto& content : contents) {
        if (content.kind == ContentKind::Base && !content.title_id.empty()) {
            bases.push_back(&content);
        }
    }

    ContentRoot* base = nullptr;
    if (bases.size() == 1) {
        base = bases.front();
    }
    if (!base) {
        if (bases.size() > 1) {
            LOG_ERROR(Common_Filesystem, "Ambiguous base game roots in {}",
                      Common::FS::PathToUTF8String(container));
            for (const auto* candidate : bases) {
                LOG_ERROR(Common_Filesystem, "  candidate: {}",
                          Common::FS::PathToUTF8String(candidate->root));
            }
        } else if (!root_override) {
            if (const auto sibling_base = FindSiblingBase(selected_root)) {
                return Discover(*sibling_base);
            }
        }
        return std::nullopt;
    }

    GameContentCatalog catalog;
    catalog.base_root = base->root;
    catalog.container_root = container;
    catalog.title_id = base->title_id;

    std::vector<ContentRoot> patches;
    std::map<std::string, std::vector<std::pair<int, fs::path>>> embedded_addons;
    for (auto& content : contents) {
        if (&content == base) {
            continue;
        }
        if (!MatchesTitle(content, catalog.title_id)) {
            if (content.kind == ContentKind::Patch || content.kind == ContentKind::Additional) {
                LOG_WARNING(Common_Filesystem, "Ignoring content for another title at {}",
                            Common::FS::PathToUTF8String(content.root));
            }
            continue;
        }
        if (content.kind == ContentKind::Patch) {
            patches.push_back(content);
        } else if (content.kind == ContentKind::Additional) {
            AddAdditionalContent(embedded_addons, content, 20, catalog.title_id);
        }
    }

    const auto anchors =
        GetCompanionAnchors(catalog.container_root, catalog.base_root, catalog.title_id);
    AddCompanionContent(patches, anchors, "-UPDATE", 0);
    AddCompanionContent(patches, anchors, "-patch", 10);

    patches.erase(std::remove_if(patches.begin(), patches.end(),
                                 [&](const auto& patch) {
                                     if (patch.kind != ContentKind::Patch ||
                                         !MatchesTitle(patch, catalog.title_id) || !patch.version) {
                                         LOG_WARNING(Common_Filesystem,
                                                     "Ignoring invalid game update at {}",
                                                     Common::FS::PathToUTF8String(patch.root));
                                         return true;
                                     }
                                     return false;
                                 }),
                  patches.end());

    if (!patches.empty()) {
        const auto newest =
            std::ranges::max_element(patches, {}, [](const auto& patch) { return *patch.version; });
        const auto active_version = (*newest).version;
        patches.erase(
            std::remove_if(patches.begin(), patches.end(),
                           [&](const auto& patch) { return patch.version != active_version; }),
            patches.end());
        std::ranges::sort(patches, [](const auto& lhs, const auto& rhs) {
            if (lhs.priority != rhs.priority) {
                return lhs.priority < rhs.priority;
            }
            return lhs.root.generic_string() < rhs.root.generic_string();
        });
        for (const auto& patch : patches) {
            AddUniquePath(catalog.patch_roots, patch.root);
        }
        LOG_INFO(Common_Filesystem, "Selected game update version {} from {} layer(s)",
                 patches.front().app_version, catalog.patch_roots.size());
    }

    std::vector<std::pair<int, fs::path>> mods;
    for (const auto& anchor : anchors) {
        for (const auto& mod : Zar::FindCompanionPaths(anchor, "-mods")) {
            mods.emplace_back(Zar::GetArchivePath(mod) ? 1 : 0, mod);
        }
    }
    std::ranges::sort(mods, [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first) {
            return lhs.first < rhs.first;
        }
        return lhs.second.generic_string() < rhs.second.generic_string();
    });
    for (const auto& mod : mods) {
        AddUniquePath(catalog.mod_roots, mod.second);
    }
    catalog.embedded_additional_content = BuildAdditionalContentList(std::move(embedded_addons));
    return catalog;
}

std::optional<fs::path> GameContentCatalog::FindGameById(const fs::path& directory,
                                                         const std::string& game_id,
                                                         int max_depth) {
    if (max_depth < 0) {
        return std::nullopt;
    }

    std::error_code ec;
    fs::directory_iterator iterator{directory, ec};
    const fs::directory_iterator end;
    while (!ec && iterator != end) {
        const auto path = iterator->path();
        std::error_code entry_ec;
        const bool is_directory = iterator->is_directory(entry_ec);
        if (!entry_ec && (is_directory || Zar::IsZarArchive(path))) {
            if (const auto catalog = Discover(path); catalog && catalog->GetTitleId() == game_id) {
                return catalog->GetBaseRoot() / "eboot.bin";
            }
            if (is_directory) {
                if (auto found = FindGameById(path, game_id, max_depth - 1)) {
                    return found;
                }
            }
        }
        iterator.increment(ec);
    }
    return std::nullopt;
}

std::vector<fs::path> GameContentCatalog::GetAppSources(bool ignore_patches) const {
    std::vector<fs::path> sources;
    sources.reserve(mod_roots.size() + patch_roots.size() + 1);
    sources.insert(sources.end(), mod_roots.begin(), mod_roots.end());
    if (!ignore_patches) {
        sources.insert(sources.end(), patch_roots.begin(), patch_roots.end());
    }
    sources.push_back(base_root);
    return sources;
}

std::optional<fs::path> GameContentCatalog::ResolveAppPath(const fs::path& relative_path,
                                                           bool ignore_patches) const {
    const auto sources = GetAppSources(ignore_patches);
    return Common::FS::OverlayView{sources}.Resolve(relative_path);
}

std::vector<AdditionalContentSource> GameContentCatalog::DiscoverAdditionalContent(
    const fs::path& addon_install_directory) const {
    std::map<std::string, std::vector<std::pair<int, fs::path>>> grouped;
    for (const auto& content : embedded_additional_content) {
        for (const auto& root : content.roots) {
            grouped[content.entitlement].emplace_back(20, root);
        }
    }

    std::array containers{addon_install_directory / title_id,
                          addon_install_directory / (title_id + ".zar")};
    for (const auto& container : containers) {
        if (!Zar::IsDirectory(container)) {
            continue;
        }
        for (const auto& candidate : GetContainerCandidates(container, container)) {
            if (const auto content = InspectContentRoot(candidate)) {
                const int priority = Zar::GetArchivePath(content->root) ? 10 : 0;
                AddAdditionalContent(grouped, *content, priority, title_id);
            }
        }
    }
    return BuildAdditionalContentList(std::move(grouped));
}

} // namespace Core::FileSys
