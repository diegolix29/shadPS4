// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/overlay_fs.h"
#include "core/file_sys/devices/logger.h"
#include "core/file_sys/devices/nop_device.h"
#include "core/file_sys/fs.h"

namespace Core::FileSys {

bool MntPoints::ignore_game_patches = false;

std::string RemoveTrailingSlashes(const std::string& path) {
    // Remove trailing slashes to make comparisons simpler.
    std::string path_sanitized = path;
    while (path_sanitized.ends_with("/")) {
        path_sanitized.pop_back();
    }
    return path_sanitized;
}

bool HasParentTraversal(const std::filesystem::path& path) {
    return std::ranges::any_of(path,
                               [](const std::filesystem::path& part) { return part == ".."; });
}

void MntPoints::Mount(const std::filesystem::path& host_folder, const std::string& guest_folder,
                      bool read_only) {
    std::scoped_lock lock{m_mutex};
    const auto guest_folder_sanitized = RemoveTrailingSlashes(guest_folder);
    const auto existing = std::ranges::find(m_mnt_pairs, guest_folder_sanitized, &MntPair::mount);
    if (existing != m_mnt_pairs.end()) {
        existing->sources.push_back({host_folder, MountLayer::Base});
        existing->read_only = existing->read_only && read_only;
        return;
    }
    m_mnt_pairs.push_back({.sources = {{host_folder, MountLayer::Base}},
                           .mount = guest_folder_sanitized,
                           .read_only = read_only});
}

void MntPoints::MountOverlay(const std::filesystem::path& host_folder,
                             const std::string& guest_folder, MountLayer layer) {
    std::scoped_lock lock{m_mutex};
    const auto guest_folder_sanitized = RemoveTrailingSlashes(guest_folder);
    const auto existing = std::ranges::find(m_mnt_pairs, guest_folder_sanitized, &MntPair::mount);
    if (existing == m_mnt_pairs.end()) {
        m_mnt_pairs.push_back({.sources = {{host_folder, layer}},
                               .mount = guest_folder_sanitized,
                               .read_only = true});
        return;
    }
    if (std::ranges::none_of(existing->sources, [&](const MntSource& source) {
            return source.host_path == host_folder && source.layer == layer;
        })) {
        existing->sources.push_back({host_folder, layer});
    }
}

void MntPoints::Unmount(const std::filesystem::path& host_folder, const std::string& guest_folder) {
    std::scoped_lock lock{m_mutex};
    const auto guest_folder_sanitized = RemoveTrailingSlashes(guest_folder);
    auto it = std::remove_if(m_mnt_pairs.begin(), m_mnt_pairs.end(), [&](const MntPair& pair) {
        return pair.mount == guest_folder_sanitized;
    });
    m_mnt_pairs.erase(it, m_mnt_pairs.end());
}

void MntPoints::UnmountAll() {
    std::scoped_lock lock{m_mutex};
    m_mnt_pairs.clear();
}

std::filesystem::path MntPoints::GetHostPath(std::string_view path, bool* is_read_only,
                                             HostPathType path_type) {
    // Evil games like Turok2 pass double slashes e.g /app0//game.kpf
    std::string corrected_path(path);
    size_t pos = corrected_path.find("//");
    while (pos != std::string::npos) {
        corrected_path.replace(pos, 2, "/");
        pos = corrected_path.find("//", pos + 1);
    }

    if (path.length() > 255)
        return "";

    const std::optional<MntPair> mount = GetMount(corrected_path);
    if (!mount) {
        return "";
    }

    if (is_read_only) {
        *is_read_only = mount->read_only;
    }

    std::vector<std::filesystem::path> sources;
    const auto add_sources = [&](MountLayer layer) {
        for (const auto& source : mount->sources) {
            if (source.layer == layer) {
                sources.push_back(source.host_path);
            }
        }
    };

    if (path_type == HostPathType::Base) {
        add_sources(MountLayer::Base);
    } else if (path_type == HostPathType::Patch) {
        add_sources(MountLayer::Patch);
    } else if (path_type == HostPathType::Mod) {
        add_sources(MountLayer::Mod);
    } else {
        add_sources(MountLayer::Mod);
        if (!ignore_game_patches) {
            add_sources(MountLayer::Patch);
        }
        add_sources(MountLayer::Base);
    }
    if (sources.empty()) {
        return {};
    }

    std::filesystem::path relative_path;
    if (RemoveTrailingSlashes(corrected_path) != mount->mount) {
        relative_path = std::string_view{corrected_path}.substr(mount->mount.size() + 1);
    }
    if (HasParentTraversal(relative_path)) {
        return {};
    }

    if (const auto resolved = Common::FS::OverlayView{sources}.Resolve(relative_path)) {
        return *resolved;
    }

    // Opening the guest path will fail, but the lowest-priority path gives a useful error.
    return sources.back() / relative_path;
}

// TODO: Does not handle mount points inside mount points.
void MntPoints::IterateDirectory(std::string_view guest_directory,
                                 const IterateDirectoryCallback& callback) {
    std::string corrected_path{guest_directory};
    size_t pos = corrected_path.find("//");
    while (pos != std::string::npos) {
        corrected_path.replace(pos, 2, "/");
        pos = corrected_path.find("//", pos + 1);
    }

    const auto mount = GetMount(corrected_path);
    if (!mount) {
        return;
    }

    std::filesystem::path relative_path;
    if (RemoveTrailingSlashes(corrected_path) != mount->mount) {
        relative_path = std::string_view{corrected_path}.substr(mount->mount.size() + 1);
    }
    if (HasParentTraversal(relative_path)) {
        return;
    }

    std::vector<std::filesystem::path> sources;
    const auto append = [&](MountLayer layer) {
        for (const auto& source : mount->sources) {
            if (source.layer == layer) {
                sources.push_back(source.host_path);
            }
        }
    };
    append(MountLayer::Mod);
    if (!ignore_game_patches) {
        append(MountLayer::Patch);
    }
    append(MountLayer::Base);
    if (sources.empty()) {
        return;
    }

    const Common::FS::OverlayView overlay{sources};
    const auto resolved = overlay.Resolve(relative_path).value_or(sources.back() / relative_path);
    callback(resolved / ".", false);
    callback(resolved / "..", false);
    overlay.IterateDirectory(relative_path, callback);
}

int HandleTable::CreateHandle() {
    std::scoped_lock lock{m_mutex};

    auto* file = new File{};
    file->is_opened = false;

    int existingFilesNum = m_files.size();

    for (int index = 0; index < existingFilesNum; index++) {
        if (m_files.at(index) == nullptr) {
            m_files[index] = file;
            return index;
        }
    }

    m_files.push_back(file);
    return m_files.size() - 1;
}

void HandleTable::DeleteHandle(int d) {
    std::scoped_lock lock{m_mutex};
    delete m_files.at(d);
    m_files[d] = nullptr;
}

File* HandleTable::GetFile(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    return m_files.at(d);
}

File* HandleTable::GetSocket(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    auto file = m_files.at(d);
    if (!file) {
        return nullptr;
    }
    if (file->type != Core::FileSys::FileType::Socket) {
        return nullptr;
    }
    return file;
}

File* HandleTable::GetEpoll(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    auto file = m_files.at(d);
    if (file->type != Core::FileSys::FileType::Epoll) {
        return nullptr;
    }
    return file;
}

File* HandleTable::GetResolver(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    auto file = m_files.at(d);
    if (file->type != Core::FileSys::FileType::Resolver) {
        return nullptr;
    }
    return file;
}

File* HandleTable::GetFile(const std::filesystem::path& host_name) {
    std::scoped_lock lock{m_mutex};
    for (auto* file : m_files) {
        if (file != nullptr && file->m_host_name == host_name) {
            return file;
        }
    }
    return nullptr;
}

void HandleTable::CreateStdHandles() {
    auto setup = [this](const char* path, auto* device) {
        int fd = CreateHandle();
        auto* file = GetFile(fd);
        file->is_opened = true;
        file->type = FileType::Device;
        file->m_guest_name = path;
        file->device =
            std::shared_ptr<Devices::BaseDevice>{reinterpret_cast<Devices::BaseDevice*>(device)};
    };
    // order matters
    setup("/dev/stdin", new Devices::Logger("stdin", false));   // stdin
    setup("/dev/stdout", new Devices::Logger("stdout", false)); // stdout
    setup("/dev/stderr", new Devices::Logger("stderr", true));  // stderr
}

int HandleTable::GetFileDescriptor(File* file) {
    std::scoped_lock lock{m_mutex};
    auto it = std::find(m_files.begin(), m_files.end(), file);

    if (it != m_files.end()) {
        return std::distance(m_files.begin(), it);
    }
    return 0;
}

} // namespace Core::FileSys
