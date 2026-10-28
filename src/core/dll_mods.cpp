// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/dll_mods.h"

namespace Core::DllMods {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct DllMod {
    fs::path folder;
    std::string id;
    std::string name;
    json manifest;
    json choices; // settings.json, or an empty object
    std::vector<fs::path> dlls;
};

json ReadJson(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return json::object();
    }
    try {
        std::ifstream in(path);
        json j = json::parse(in);
        return j.is_object() ? j : json::object();
    } catch (const std::exception& e) {
        LOG_ERROR(Loader, "DLL mods: could not parse {}: {}", path.string(), e.what());
        return json::object();
    }
}

bool AppliesTo(const json& manifest, std::string_view serial) {
    if (!manifest.contains("games") || !manifest["games"].is_array() || manifest["games"].empty()) {
        return true;
    }
    return std::any_of(manifest["games"].begin(), manifest["games"].end(), [serial](const json& g) {
        return g.is_string() && g.get_ref<const std::string&>() == serial;
    });
}

bool IsEnabled(const DllMod& mod) {
    std::error_code ec;
    if (fs::exists(mod.folder / "disabled", ec)) {
        return false;
    }
    if (mod.choices.contains("enabled") && mod.choices["enabled"].is_boolean()) {
        return mod.choices["enabled"].get<bool>();
    }
    return mod.manifest.value("enabled_by_default", true);
}

// The mods for this game that have at least one DLL, in folder name order.
std::vector<DllMod> ModsFor(std::string_view serial) {
    std::vector<DllMod> mods;
    if (serial.empty()) {
        return mods;
    }
    const fs::path root = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "mods";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return mods;
    }
    // Both layouts: mods/<mod>/mod.json and mods/<serial>/<mod>/mod.json (BBSC Play package).
    std::vector<fs::path> folders;
    auto collect = [&folders](const fs::path& dir) {
        std::vector<fs::path> found;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_directory() && fs::exists(entry.path() / "mod.json")) {
                found.push_back(entry.path());
            }
        }
        std::sort(found.begin(), found.end());
        folders.insert(folders.end(), found.begin(), found.end());
    };
    collect(root);
    if (fs::is_directory(root / std::string(serial), ec)) {
        collect(root / std::string(serial));
    }

    for (const fs::path& folder : folders) {
        DllMod mod;
        mod.folder = folder;
        mod.manifest = ReadJson(folder / "mod.json");
        if (!AppliesTo(mod.manifest, serial)) {
            continue;
        }
        mod.id = mod.manifest.value("id", folder.filename().string());
        mod.name = mod.manifest.value("name", mod.id);
        mod.choices = ReadJson(folder / "settings.json");

        auto add_dll = [&](const json& value) {
            if (value.is_string()) {
                mod.dlls.push_back(folder / value.get<std::string>());
            }
        };
        if (mod.manifest.contains("dll")) {
            add_dll(mod.manifest["dll"]);
        }
        if (mod.manifest.contains("dlls") && mod.manifest["dlls"].is_array()) {
            for (const auto& d : mod.manifest["dlls"]) {
                add_dll(d);
            }
        }
        if (!mod.dlls.empty()) {
            mods.push_back(std::move(mod));
        }
    }
    return mods;
}

// A setting's value as an environment string: bool "1"/"0", numbers as digits, text as is.
std::string EnvValue(const json& value) {
    if (value.is_boolean()) {
        return value.get<bool>() ? "1" : "0";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number()) {
        return std::to_string(value.get<double>());
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return {};
}

void SetEnv(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

} // namespace

void ApplyEnvironment(std::string_view serial) {
    for (const DllMod& mod : ModsFor(serial)) {
        const bool on = IsEnabled(mod);
        LOG_INFO(Loader, "DLL mods: {} {} ({})", on ? "ON " : "off", mod.name, mod.folder.string());
        if (!on || !mod.manifest.contains("settings") || !mod.manifest["settings"].is_array()) {
            continue;
        }
        for (const json& def : mod.manifest["settings"]) {
            if (!def.is_object() || !def.contains("env")) {
                continue;
            }
            const std::string id = def.value("id", "");
            const json& value = (!id.empty() && mod.choices.contains(id))
                                    ? mod.choices[id]
                                    : def.value("default", json{});
            const std::string text = EnvValue(value);
            if (def.value("type", "") == "text" && text.empty()) {
                continue; // an empty text setting leaves the mod's own default
            }
            std::vector<std::string> names;
            if (def["env"].is_string()) {
                names.push_back(def["env"].get<std::string>());
            } else if (def["env"].is_array()) {
                for (const auto& n : def["env"]) {
                    if (n.is_string()) {
                        names.push_back(n.get<std::string>());
                    }
                }
            }
            for (const std::string& name : names) {
                if (const char* existing = std::getenv(name.c_str()); existing != nullptr) {
                    LOG_INFO(Loader, "DLL mods: {} kept at {} (set before start)", name, existing);
                    continue;
                }
                SetEnv(name, text);
            }
        }
    }
}

void LoadEnabledDlls(std::string_view serial) {
    for (const DllMod& mod : ModsFor(serial)) {
        if (!IsEnabled(mod)) {
            continue;
        }
        for (const fs::path& dll : mod.dlls) {
            std::error_code ec;
            if (!fs::exists(dll, ec)) {
                LOG_ERROR(Loader, "DLL mods: {} not found ({})", dll.string(), mod.name);
                continue;
            }
#ifdef _WIN32
            // The mod's own folder is searched for the DLL's dependencies too.
            const HMODULE handle =
                LoadLibraryExW(dll.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (handle == nullptr) {
                LOG_ERROR(Loader, "DLL mods: could not load {} (error {})", dll.string(),
                          GetLastError());
                continue;
            }
#else
            void* handle = dlopen(dll.c_str(), RTLD_NOW);
            if (handle == nullptr) {
                LOG_ERROR(Loader, "DLL mods: could not load {}: {}", dll.string(), dlerror());
                continue;
            }
#endif
            LOG_INFO(Loader, "DLL mods: loaded {} ({})", dll.string(), mod.name);
        }
    }
}

namespace {
const DllMod* FindMod(const std::vector<DllMod>& mods, std::string_view id) {
    for (const DllMod& mod : mods) {
        if (mod.id == id) {
            return &mod;
        }
    }
    return nullptr;
}
} // namespace

bool IsModInstalled(std::string_view serial, std::string_view id) {
    const auto mods = ModsFor(serial);
    return FindMod(mods, id) != nullptr;
}

bool IsModEnabled(std::string_view serial, std::string_view id) {
    const auto mods = ModsFor(serial);
    const DllMod* mod = FindMod(mods, id);
    return mod != nullptr && IsEnabled(*mod);
}

void SetModEnabled(std::string_view serial, std::string_view id, bool enabled) {
    const auto mods = ModsFor(serial);
    const DllMod* mod = FindMod(mods, id);
    if (mod == nullptr) {
        return;
    }
    std::error_code ec;
    const fs::path disabled_file = mod->folder / "disabled";
    if (enabled) {
        fs::remove(disabled_file, ec);
    } else if (!fs::exists(disabled_file, ec)) {
        std::ofstream(disabled_file).close();
    }
    // settings.json "enabled" wins over the default, so keep it in step when it exists.
    if (mod->choices.contains("enabled")) {
        json choices = mod->choices;
        choices["enabled"] = enabled;
        std::ofstream out(mod->folder / "settings.json", std::ios::binary);
        out << choices.dump(2);
    }
}

} // namespace Core::DllMods
