// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <unordered_map>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scope_exit.h"

#ifdef __APPLE__
#include <CoreFoundation/CFBundle.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <sys/param.h>
#endif

#ifndef MAX_PATH
#ifdef _WIN32
// This is the maximum number of UTF-16 code units permissible in Windows file paths
#define MAX_PATH 260
#include <Shlobj.h>
#include <windows.h>
#else
// This is the maximum number of UTF-8 code units permissible in all other OSes' file paths
#define MAX_PATH 1024
#include <unistd.h>
#endif
#endif

#ifdef ENABLE_QT_GUI
#include <QString>
#endif

namespace Common::FS {

namespace fs = std::filesystem;

#ifdef __APPLE__
using IsTranslocatedURLFunc = Boolean (*)(CFURLRef path, bool* isTranslocated,
                                          CFErrorRef* __nullable error);
using CreateOriginalPathForURLFunc = CFURLRef __nullable (*)(CFURLRef translocatedPath,
                                                             CFErrorRef* __nullable error);

static CFURLRef UntranslocateBundlePath(const CFURLRef bundle_path) {
    if (void* security_handle =
            dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY)) {
        SCOPE_EXIT {
            dlclose(security_handle);
        };

        const auto IsTranslocatedURL = reinterpret_cast<IsTranslocatedURLFunc>(
            dlsym(security_handle, "SecTranslocateIsTranslocatedURL"));
        const auto CreateOriginalPathForURL = reinterpret_cast<CreateOriginalPathForURLFunc>(
            dlsym(security_handle, "SecTranslocateCreateOriginalPathForURL"));

        bool is_translocated = false;
        if (IsTranslocatedURL && CreateOriginalPathForURL &&
            IsTranslocatedURL(bundle_path, &is_translocated, nullptr) && is_translocated) {
            return CreateOriginalPathForURL(bundle_path, nullptr);
        }
    }
    return nullptr;
}

static std::optional<std::filesystem::path> GetBundleParentDirectory() {
    if (CFBundleRef bundle_ref = CFBundleGetMainBundle()) {
        if (CFURLRef bundle_url_ref = CFBundleCopyBundleURL(bundle_ref)) {
            SCOPE_EXIT {
                CFRelease(bundle_url_ref);
            };

            CFURLRef untranslocated_url_ref = UntranslocateBundlePath(bundle_url_ref);
            SCOPE_EXIT {
                if (untranslocated_url_ref) {
                    CFRelease(untranslocated_url_ref);
                }
            };

            char app_bundle_path[MAXPATHLEN];
            if (CFURLGetFileSystemRepresentation(
                    untranslocated_url_ref ? untranslocated_url_ref : bundle_url_ref, true,
                    reinterpret_cast<u8*>(app_bundle_path), sizeof(app_bundle_path))) {
                std::filesystem::path bundle_path{app_bundle_path};
                return bundle_path.parent_path();
            }
        }
    }
    return std::nullopt;
}
#endif

static auto UserPaths = [] {
    std::unordered_map<PathType, std::filesystem::path> paths;

    const auto create_path = [&](PathType shad_path, const std::filesystem::path& new_path) {
        paths.insert_or_assign(shad_path, new_path);
    };

    create_path(PathType::UserDir, "");
    create_path(PathType::LogDir, "");
    create_path(PathType::ScreenshotsDir, "");
    create_path(PathType::ShaderDir, "");
    create_path(PathType::GameDataDir, "");
    create_path(PathType::TempDataDir, "");
    create_path(PathType::SysModuleDir, "");
    create_path(PathType::DownloadDir, "");
    create_path(PathType::CapturesDir, "");
    create_path(PathType::CheatsDir, "");
    create_path(PathType::PatchesDir, "");
    create_path(PathType::MetaDataDir, "");
    create_path(PathType::CustomTrophy, "");
    create_path(PathType::CustomConfigs, "");
    create_path(PathType::CustomThemes, "");
    create_path(PathType::ModsFolder, "");
    create_path(PathType::CacheDir, "");
    create_path(PathType::CustomAudios, "");
    create_path(PathType::FontsDir, "");

    return paths;
}();

static PathInitState current_init_state = PathInitState::Uninitialized;

bool ValidatePath(const fs::path& path) {
    if (path.empty()) {
        LOG_ERROR(Common_Filesystem, "Input path is empty, path={}", PathToUTF8String(path));
        return false;
    }

#ifdef _WIN32
    if (path.u16string().size() >= MAX_PATH) {
        LOG_ERROR(Common_Filesystem, "Input path is too long, path={}", PathToUTF8String(path));
        return false;
    }
#else
    if (path.u8string().size() >= MAX_PATH) {
        LOG_ERROR(Common_Filesystem, "Input path is too long, path={}", PathToUTF8String(path));
        return false;
    }
#endif

    return true;
}

std::filesystem::path GetExecutablePath() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    DWORD size = GetModuleFileNameW(NULL, buffer, MAX_PATH);
    if (size == 0 || size == MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer);
#elif defined(__APPLE__)
    char buffer[PATH_MAX];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        return std::filesystem::path(buffer);
    }
    return {};
#elif defined(__linux__)
    char buffer[PATH_MAX];
    ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (size == -1) {
        return {};
    }
    buffer[size] = '\0';
    return std::filesystem::path(buffer);
#else
    return {};
#endif
}

std::string PathToUTF8String(const std::filesystem::path& path) {
    const auto u8_string = path.u8string();
    return std::string{u8_string.begin(), u8_string.end()};
}

const fs::path& GetUserPath(PathType shad_path) {
    if (!IsUserPathsInitialized()) {
        auto portable_dir = GetPortablePath();
        auto global_dir = GetGlobalPath();

        bool portableExists = std::filesystem::exists(portable_dir);
        bool globalExists = std::filesystem::exists(global_dir);

        PathInitState detected_state;
        if (portableExists) {
            detected_state = PathInitState::Portable;
        } else if (globalExists) {
            detected_state = PathInitState::Global;
        } else {
            detected_state = PathInitState::Portable;
        }

        InitializeUserPaths(detected_state);
    }

    return UserPaths.at(shad_path);
}

std::string GetUserPathString(PathType shad_path) {
    return PathToUTF8String(GetUserPath(shad_path));
}

void SetUserPath(PathType shad_path, const fs::path& new_path) {
    if (!std::filesystem::is_directory(new_path)) {
        LOG_ERROR(Common_Filesystem, "Filesystem object at new_path={} is not a directory",
                  PathToUTF8String(new_path));
        return;
    }

    UserPaths.insert_or_assign(shad_path, new_path);
}

std::optional<fs::path> FindGameByID(const fs::path& dir, const std::string& game_id,
                                     int max_depth) {
    if (max_depth < 0) {
        return std::nullopt;
    }

    // Check if this is the game we're looking for
    if (dir.filename() == game_id && fs::exists(dir / "sce_sys" / "param.sfo")) {
        auto eboot_path = dir / "eboot.bin";
        if (fs::exists(eboot_path)) {
            return eboot_path;
        }
    }

    // Recursively search subdirectories
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        if (auto found = FindGameByID(entry.path(), game_id, max_depth - 1)) {
            return found;
        }
    }

    return std::nullopt;
}

std::filesystem::path GetPortablePath() {
    return std::filesystem::current_path() / PORTABLE_DIR;
}

std::filesystem::path GetGlobalPath() {
#if _WIN32
    TCHAR appdata[MAX_PATH] = {0};
    SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, appdata);
    return std::filesystem::path(appdata) / L"shadPS4";
#elif __APPLE__
    return std::filesystem::path(getenv("HOME")) / "Library" / "Application Support" / "shadPS4";
#elif defined(__linux__)
    const char* xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home && strlen(xdg_data_home) > 0) {
        return std::filesystem::path(xdg_data_home) / "shadPS4";
    } else {
        return std::filesystem::path(getenv("HOME")) / ".local" / "share" / "shadPS4";
    }
#else
    return std::filesystem::current_path() / PORTABLE_DIR;
#endif
}

void InitializeUserPaths(PathInitState state) {
    if (current_init_state != PathInitState::Uninitialized) {
        return;
    }

    std::filesystem::path user_dir;

    if (state == PathInitState::Portable) {
        user_dir = GetPortablePath();
    } else if (state == PathInitState::Global) {
        user_dir = GetGlobalPath();
    } else {
        return;
    }

    current_init_state = state;

    const auto create_path = [&](PathType shad_path, const std::filesystem::path& new_path) {
        std::filesystem::create_directories(new_path);
        UserPaths.insert_or_assign(shad_path, new_path);
    };

    // Only create directories if they don't already exist
    if (!std::filesystem::exists(user_dir)) {
        create_path(PathType::UserDir, user_dir);
        create_path(PathType::LogDir, user_dir / LOG_DIR);
        create_path(PathType::ScreenshotsDir, user_dir / SCREENSHOTS_DIR);
        create_path(PathType::ShaderDir, user_dir / SHADER_DIR);
        create_path(PathType::GameDataDir, user_dir / GAMEDATA_DIR);
        create_path(PathType::TempDataDir, user_dir / TEMPDATA_DIR);
        create_path(PathType::SysModuleDir, user_dir / SYSMODULES_DIR);
        create_path(PathType::DownloadDir, user_dir / DOWNLOAD_DIR);
        create_path(PathType::CapturesDir, user_dir / CAPTURES_DIR);
        create_path(PathType::CheatsDir, user_dir / CHEATS_DIR);
        create_path(PathType::PatchesDir, user_dir / PATCHES_DIR);
        create_path(PathType::MetaDataDir, user_dir / METADATA_DIR);
        create_path(PathType::CustomTrophy, user_dir / CUSTOM_TROPHY);
        create_path(PathType::CustomConfigs, user_dir / CUSTOM_CONFIGS);
        create_path(PathType::CustomThemes, user_dir / CUSTOM_THEMES);
        create_path(PathType::ModsFolder, user_dir / MODS_FOLDER);
        create_path(PathType::CacheDir, user_dir / CACHE_DIR);
        create_path(PathType::CustomAudios, user_dir / AUDIO_DIR);
        create_path(PathType::FontsDir, user_dir / FONTS_DIR);

        // Only create notice files if they don't exist
        if (!std::filesystem::exists(user_dir / CUSTOM_TROPHY / "Notice.txt")) {
            std::ofstream notice_file(user_dir / CUSTOM_TROPHY / "Notice.txt");
            if (notice_file.is_open()) {
                notice_file
                    // clang-format off
    << "++++++++++++++++++++++++++++++++\n"
    "+ Custom Trophy Images / Sound +\n"
    "++++++++++++++++++++++++++++++++\n\n"

    "You can add custom images to the trophies.\n"
    "*We recommend a square resolution image, for example 200x200, 500x500, same size as the height and width.\n"
    "In this folder ('user\\custom_trophy'), add the files with the following names:\n\n"
    "bronze.png\n"
    "silver.png\n"
    "gold.png\n"
    "platinum.png\n\n"

    "You can add a custom sound for trophy notifications.\n"
    "*By default, no audio is played unless it is in this folder and you are using the QT version.\n"
    "In this folder ('user\\custom_trophy'), add the files with the following names:\n\n"

    "trophy.wav OR trophy.mp3";
                // clang-format on
                notice_file.close();
            }
        }

        if (!std::filesystem::exists(user_dir / AUDIO_DIR / "Notice.txt")) {
            std::ofstream audio_file(user_dir / AUDIO_DIR / "Notice.txt");
            if (audio_file.is_open()) {
                audio_file
                    // clang-format off
    << "++++++++++++++++++++++++++++++++\n"
    "+ Custom Audios / Sounds +\n"
    "++++++++++++++++++++++++++++++++\n\n"

    "You can add custom sounds to the games menu.\n"
    "For the background music / tick movement navigation / start game sound.\n"
    "It has sound built in but if you add.\n"
    "In this folder ('user\\custom_audios'), the files with the following names:\n"
    "bgm.wav/tick.wav - bgm.mp3/tick.mp3 - play.wav/play.mp3.\n"
    "bgm for Background music, tick for movement navigation and play for start game sound.\n"
    "You can use custom audios for the games menu.";
                // clang-format on
                audio_file.close();
            }
        }
    } else {
        // Directory already exists, just set the paths without creating new files
        UserPaths.insert_or_assign(PathType::UserDir, user_dir);
        UserPaths.insert_or_assign(PathType::LogDir, user_dir / LOG_DIR);
        UserPaths.insert_or_assign(PathType::ScreenshotsDir, user_dir / SCREENSHOTS_DIR);
        UserPaths.insert_or_assign(PathType::ShaderDir, user_dir / SHADER_DIR);
        UserPaths.insert_or_assign(PathType::GameDataDir, user_dir / GAMEDATA_DIR);
        UserPaths.insert_or_assign(PathType::TempDataDir, user_dir / TEMPDATA_DIR);
        UserPaths.insert_or_assign(PathType::SysModuleDir, user_dir / SYSMODULES_DIR);
        UserPaths.insert_or_assign(PathType::DownloadDir, user_dir / DOWNLOAD_DIR);
        UserPaths.insert_or_assign(PathType::CapturesDir, user_dir / CAPTURES_DIR);
        UserPaths.insert_or_assign(PathType::CheatsDir, user_dir / CHEATS_DIR);
        UserPaths.insert_or_assign(PathType::PatchesDir, user_dir / PATCHES_DIR);
        UserPaths.insert_or_assign(PathType::MetaDataDir, user_dir / METADATA_DIR);
        UserPaths.insert_or_assign(PathType::CustomTrophy, user_dir / CUSTOM_TROPHY);
        UserPaths.insert_or_assign(PathType::CustomConfigs, user_dir / CUSTOM_CONFIGS);
        UserPaths.insert_or_assign(PathType::CustomThemes, user_dir / CUSTOM_THEMES);
        UserPaths.insert_or_assign(PathType::ModsFolder, user_dir / MODS_FOLDER);
        UserPaths.insert_or_assign(PathType::CacheDir, user_dir / CACHE_DIR);
        UserPaths.insert_or_assign(PathType::CustomAudios, user_dir / AUDIO_DIR);
        UserPaths.insert_or_assign(PathType::FontsDir, user_dir / FONTS_DIR);
    }
}

PathInitState GetUserPathInitState() {
    return current_init_state;
}

bool IsUserPathsInitialized() {
    return current_init_state != PathInitState::Uninitialized;
}

#ifdef ENABLE_QT_GUI
void PathToQString(QString& result, const std::filesystem::path& path) {
#ifdef _WIN32
    result = QString::fromStdWString(path.wstring());
#else
    result = QString::fromStdString(path.string());
#endif
}

std::filesystem::path PathFromQString(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}
#endif

} // namespace Common::FS
