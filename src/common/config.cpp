// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <optional>
#include <string>
#include <fmt/core.h>
#include <fmt/xchar.h> // for wstring support
#include <toml.hpp>

#include "common/config.h"
#include "common/logging/formatter.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scm_rev.h"

using std::nullopt;
using std::optional;
using std::string;

namespace toml {
template <typename TC, typename K>
std::filesystem::path find_fs_path_or(const basic_value<TC>& v, const K& ky,
                                      std::filesystem::path opt) {
    try {
        auto str = find<string>(v, ky);
        if (str.empty()) {
            return opt;
        }
        std::u8string u8str(reinterpret_cast<const char8_t*>(str.data()), str.size());
        return std::filesystem::path{u8str};
    } catch (...) {
        return opt;
    }
}

// why is it so hard to avoid exceptions with this library
template <typename T>
std::optional<T> get_optional(const toml::value& v, const std::string& key) {
    if (!v.is_table())
        return std::nullopt;
    const auto& tbl = v.as_table();
    auto it = tbl.find(key);
    if (it == tbl.end())
        return std::nullopt;

    if constexpr (std::is_integral_v<T>) {
        if (it->second.is_integer())
            return static_cast<T>(it->second.as_integer());
    } else if constexpr (std::is_floating_point_v<T>) {
        if (it->second.is_floating())
            return static_cast<T>(it->second.as_floating());
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (it->second.is_string())
            return it->second.as_string();
    } else if constexpr (std::is_same_v<T, bool>) {
        if (it->second.is_boolean())
            return it->second.as_boolean();
    } else {
        static_assert([] { return false; }(), "Unsupported type in get_optional<T>");
    }

    return std::nullopt;
}

} // namespace toml

namespace Config {

template <typename T>
class ConfigEntry {
public:
    T base_value;
    optional<T> game_specific_value;

    ConfigEntry(const T& t = T()) : base_value(t), game_specific_value(nullopt) {}

    ConfigEntry& operator=(const T& t) {
        base_value = t;
        return *this;
    }

    const T& get() const {
        return game_specific_value ? *game_specific_value : base_value;
    }

    void setFromToml(const toml::value& v, const std::string& key, bool is_game_specific = false) {
        auto val = toml::get_optional<T>(v, key);
        if (!val)
            return; // don’t clear existing values if missing

        if (is_game_specific) {
            game_specific_value = *val;
        } else {
            base_value = *val;
        }
    }
};

// General
static ConfigEntry<bool> isNeo(false);
static ConfigEntry<bool> isDevKit(false);
static ConfigEntry<bool> isPSNSignedIn(false);
static ConfigEntry<bool> isTrophyPopupDisabled(false);
static ConfigEntry<double> trophyNotificationDuration(6.0);
static ConfigEntry<std::string> logFilter("");
static ConfigEntry<std::string> logType("sync");
static ConfigEntry<std::string> userName("shadPS4");
static std::string chooseHomeTab = "General";
static ConfigEntry<bool> isShowSplash(false);
static bool isAutoUpdate = false;
static bool isAlwaysShowChangelog = false;
static ConfigEntry<std::string> isSideTrophy("right");
static ConfigEntry<bool> isConnectedToNetwork(false);
static bool enableDiscordRPC = false;
static bool checkCompatibilityOnStartup = false;
static bool compatibilityData = false;
static ConfigEntry<bool> autoRestartGame(false);
static ConfigEntry<bool> restartWithBaseGame(false);
static ConfigEntry<bool> screenTipDisable(false);
static ConfigEntry<bool> g_fpsLimiterEnabled(false);

// Input
static ConfigEntry<int> cursorState(HideCursorState::Idle);
static ConfigEntry<int> cursorHideTimeout(5);
static ConfigEntry<bool> useSpecialPad(false);
static ConfigEntry<int> specialPadClass(1);
static ConfigEntry<bool> isMotionControlsEnabled(true);
static ConfigEntry<bool> useUnifiedInputConfig(true);
static ConfigEntry<std::string> micDevice("Default Device");
static ConfigEntry<std::string> defaultControllerID("");
static ConfigEntry<bool> backgroundControllerInput(false);

// Non-config runtime-only
static bool overrideControllerColor = false;
static int controllerCustomColorRGB[3] = {0, 0, 255};

// GPU
static ConfigEntry<u32> screenWidth(1280);
static ConfigEntry<u32> screenHeight(720);
static ConfigEntry<u32> windowWidth(1280);
static ConfigEntry<u32> windowHeight(720);
static ConfigEntry<u32> internalScreenWidth(1280);
static ConfigEntry<u32> internalScreenHeight(720);
static ConfigEntry<bool> isNullGpu(false);
static ConfigEntry<bool> shouldCopyGPUBuffers(false);
static ConfigEntry<ReadbackSpeed> readbackSpeedMode(ReadbackSpeed::Default);
static ConfigEntry<bool> readbackLinearImagesEnabled(false);
static ConfigEntry<bool> directMemoryAccessEnabled(false);
static ConfigEntry<bool> shouldDumpShaders(false);
static ConfigEntry<bool> shouldPatchShaders(false);
static ConfigEntry<u32> vblankFrequency(60);
static ConfigEntry<bool> isFullscreen(false);
static ConfigEntry<std::string> fullscreenMode("Windowed");
static ConfigEntry<std::string> presentMode("Mailbox");
static ConfigEntry<bool> isHDRAllowed(false);
static ConfigEntry<bool> fsrEnabled(true);
static ConfigEntry<bool> rcasEnabled(true);
static ConfigEntry<int> rcasAttenuation(250);

// Audio / BGM
static bool playBGM = false;
static ConfigEntry<float> rcas_attenuation(0.25f);
static ConfigEntry<std::string> audioBackend("cubeb");
static ConfigEntry<int> audioVolume(100);
static int BGMvolume = 50;

// Vulkan
static ConfigEntry<s32> gpuId(-1);
static ConfigEntry<bool> vkValidation(false);
static ConfigEntry<bool> vkValidationSync(false);
static ConfigEntry<bool> vkValidationGpu(false);
static ConfigEntry<bool> vkCrashDiagnostic(false);
static ConfigEntry<bool> vkHostMarkers(false);
static ConfigEntry<bool> vkGuestMarkers(false);
static ConfigEntry<bool> rdocEnable(false);

// Debug
static ConfigEntry<bool> isDebugDump(false);
static ConfigEntry<bool> isShaderDebug(false);
static ConfigEntry<bool> isSeparateLogFilesEnabled(false);
static ConfigEntry<bool> shaderSkipsEnabled(false);
static ConfigEntry<std::string> memoryAlloc("medium");
static ConfigEntry<bool> isFpsColor(true);
static ConfigEntry<bool> fpsColorState(false);
static ConfigEntry<bool> logEnabled(true);

// Shader skips (runtime, not saved)
std::unordered_map<std::string, std::vector<std::string>> all_skipped_shader_hashes = {
    {"CUSA00018",
     {"f5874f2a8d7f2037", "f5874f2a65f418f9", "25593f798d7f2037", "25593f7965f418f9",
      "2537adba98213a66", "fe36adba8c8b5626"}},
    {"CUSA00093", {"b5a945a8"}},
    {"Default", {"7ee03d3f", "1635154C", "43e07e56", "c7e25f41"}},
    {"CUSA07478", {"3ae1c2c7"}},
    {"CUSA00605", {"27c81bac", "c31d0698", "c7e25f41", "43e07e56"}},
    {"CUSA08809",
     {"9be5b74e", "61a44417", "2a8576db", "b33e9db6", "d0019dd9", "d94ec720", "8fb484ae", "2e27c82",
      "2a6e88d3", "f11eae1f", "baabdd0c", "61c26b46", "b6fee93e", "911e3823", "a0acfa89"}},
    {"CUSA00004", {"586682de"}}};
std::vector<u64> current_skipped_shader_hashes = {};

// GUI
static bool load_game_size = true;
static std::vector<GameInstallDir> settings_install_dirs = {};
std::vector<bool> install_dirs_enabled = {};
std::filesystem::path settings_addon_install_dir = {};
std::filesystem::path save_data_path = {};
u32 main_window_geometry_x = 400;
u32 main_window_geometry_y = 400;
u32 main_window_geometry_w = 1280;
u32 main_window_geometry_h = 720;
u32 mw_themes = 0;
u32 m_icon_size = 36;
u32 m_icon_size_grid = 69;
u32 m_slider_pos = 0;
u32 m_slider_pos_grid = 0;
u32 m_table_mode = 0;
u32 m_window_size_W = 1280;
u32 m_window_size_H = 720;
std::vector<std::string> m_elf_viewer;
std::vector<std::string> m_recent_files;
std::string emulator_language = "en_US";
static int backgroundImageOpacity = 50;
static bool showBackgroundImage = true;
static ConfigEntry<bool> enableAutoBackup(false);
static bool showLabelsUnderIcons = true;
static std::string updateChannel;
static ConfigEntry<int> volumeSlider(100);
static ConfigEntry<bool> muteEnabled(false);
static ConfigEntry<u32> fpsLimit(60);

// Settings
u32 m_language = 1; // english

// Keys
static string trophyKey = "";

bool allowHDR() {
    return isHDRAllowed.get();
}

bool getEnableAutoBackup() {
    return enableAutoBackup.get();
}

bool GetUseUnifiedInputConfig() {
    return useUnifiedInputConfig.get();
}

void SetUseUnifiedInputConfig(bool use) {
    useUnifiedInputConfig.base_value = use;
}

bool GetOverrideControllerColor() {
    return overrideControllerColor;
}

void SetOverrideControllerColor(bool enable) {
    overrideControllerColor = enable;
}

int* GetControllerCustomColor() {
    return controllerCustomColorRGB;
}

bool getLoggingEnabled() {
    return logEnabled.get();
}

void SetControllerCustomColor(int r, int b, int g) {
    controllerCustomColorRGB[0] = r;
    controllerCustomColorRGB[1] = b;
    controllerCustomColorRGB[2] = g;
}

u32 getFpsLimit() {
    return fpsLimit.get();
}

void setFpsLimit(u32 fpsValue) {
    fpsLimit.base_value = fpsValue;
}

bool fpsLimiterEnabled() {
    return g_fpsLimiterEnabled.get();
}

void setFpsLimiterEnabled(bool enabled) {
    g_fpsLimiterEnabled.base_value = enabled;
}

bool getAutoRestartGame() {
    return autoRestartGame.get();
}

void setAutoRestartGame(bool enable) {
    autoRestartGame.base_value = enable;
}

bool getRestartWithBaseGame() {
    return restartWithBaseGame.get();
}
void setRestartWithBaseGame(bool enable) {
    restartWithBaseGame.base_value = enable;
}

string getTrophyKey() {
    return trophyKey;
}

void setTrophyKey(string key) {
    trophyKey = key;
}

bool GetLoadGameSizeEnabled() {
    return load_game_size;
}

std::filesystem::path GetSaveDataPath() {
    if (save_data_path.empty()) {
        return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "savedata";
    }
    return save_data_path;
}

void setLoadGameSizeEnabled(bool enable) {
    load_game_size = enable;
}

bool isNeoModeConsole() {
    return isNeo.get();
}

bool isDevKitConsole() {
    return isDevKit.get();
}

bool getIsFullscreen() {
    return isFullscreen.get();
}

bool getShowLabelsUnderIcons() {
    return showLabelsUnderIcons;
}

bool setShowLabelsUnderIcons() {
    return false;
}

string getFullscreenMode() {
    return fullscreenMode.get();
}

string getPresentMode() {
    return presentMode.get();
}

bool getisTrophyPopupDisabled() {
    return isTrophyPopupDisabled.get();
}

bool getPlayBGM() {
    return playBGM;
}

int getBGMvolume() {
    return BGMvolume;
}

bool getEnableDiscordRPC() {
    return enableDiscordRPC;
}

s16 getCursorState() {
    return cursorState.get();
}

int getCursorHideTimeout() {
    return cursorHideTimeout.get();
}

string getMicDevice() {
    return micDevice.get();
}

double getTrophyNotificationDuration() {
    return trophyNotificationDuration.get();
}

u32 getWindowWidth() {
    return windowWidth.get();
}

u32 getWindowHeight() {
    return windowHeight.get();
}

u32 getInternalScreenWidth() {
    return internalScreenHeight.get();
}

u32 getInternalScreenHeight() {
    return internalScreenHeight.get();
}

s32 getGpuId() {
    return gpuId.get();
}

bool getFsrEnabled() {
    return fsrEnabled.get();
}

void setFsrEnabled(bool enable) {
    fsrEnabled.base_value = enable;
}

bool getRcasEnabled() {
    return rcasEnabled.get();
}

void setRcasEnabled(bool enable) {
    rcasEnabled.base_value = enable;
}

float getRcasAttenuation() {
    return rcas_attenuation.get();
}

void setRcasAttenuation(float value) {
    rcas_attenuation.base_value = value;
}

string getLogFilter() {
    return logFilter.get();
}

string getLogType() {
    return logType.get();
}

string getUserName() {
    return userName.get();
}

string getUpdateChannel() {
    return updateChannel;
}

string getChooseHomeTab() {
    return chooseHomeTab;
}

int getVolumeSlider() {
    return volumeSlider.get();
}

void setVolumeSlider(int volumeValue) {
    volumeSlider.base_value = volumeValue;
}

bool isMuteEnabled() {
    return muteEnabled.get();
}

void setMuteEnabled(bool enabled) {
    muteEnabled.base_value = enabled;
}

bool getUseSpecialPad() {
    return useSpecialPad.get();
}

int getSpecialPadClass() {
    return specialPadClass.get();
}

bool getIsMotionControlsEnabled() {
    return isMotionControlsEnabled.get();
}

bool debugDump() {
    return isDebugDump.get();
}

bool collectShadersForDebug() {
    return isShaderDebug.get();
}

bool showSplash() {
    return isShowSplash.get();
}

bool autoUpdate() {
    return isAutoUpdate;
}

bool alwaysShowChangelog() {
    return isAlwaysShowChangelog;
}

string sideTrophy() {
    return isSideTrophy.get();
}

bool nullGpu() {
    return isNullGpu.get();
}

bool copyGPUCmdBuffers() {
    return shouldCopyGPUBuffers.get();
}

ReadbackSpeed readbackSpeed() {
    return readbackSpeedMode.get();
}

void setReadbackSpeed(ReadbackSpeed mode) {
    readbackSpeedMode.base_value = mode;
}

bool setReadbackLinearImages(bool enable) {
    return readbackLinearImagesEnabled.base_value = enable;
}

bool getReadbackLinearImages() {
    return readbackLinearImagesEnabled.get();
}

bool isScreenTipDisable(bool enable) {
    return screenTipDisable.base_value = enable;
}

bool getScreenTipDisable() {
    return screenTipDisable.get();
}

bool directMemoryAccess() {
    return directMemoryAccessEnabled.get();
}

bool dumpShaders() {
    return shouldDumpShaders.get();
}

bool patchShaders() {
    return shouldPatchShaders.get();
}

bool isRdocEnabled() {
    return rdocEnable.get();
}

bool fpsColor() {
    return isFpsColor.get();
}

bool isLoggingEnabled() {
    return logEnabled.get();
}

u32 vblankFreq() {
    return vblankFrequency.get();
}

bool vkValidationEnabled() {
    return vkValidation.get();
}

bool vkValidationSyncEnabled() {
    return vkValidationSync.get();
}

bool vkValidationGpuEnabled() {
    return vkValidationGpu.get();
}

bool getVkCrashDiagnosticEnabled() {
    return vkCrashDiagnostic.get();
}

bool getVkHostMarkersEnabled() {
    return vkHostMarkers.get();
}

bool getVkGuestMarkersEnabled() {
    return vkGuestMarkers.get();
}

void setVkCrashDiagnosticEnabled(bool enable) {
    vkCrashDiagnostic.base_value = enable;
}

void setVkHostMarkersEnabled(bool enable) {
    vkHostMarkers.base_value = enable;
}

void setVkGuestMarkersEnabled(bool enable) {
    vkGuestMarkers.base_value = enable;
}

bool getCompatibilityEnabled() {
    return compatibilityData;
}

bool getCheckCompatibilityOnStartup() {
    return checkCompatibilityOnStartup;
}

string getAudioBackend() {
    return audioBackend.get();
}

int getAudioVolume() {
    return audioVolume.get();
}

void setfpsColor(bool enable) {
    fpsColorState = enable;
}

bool getIsConnectedToNetwork() {
    return isConnectedToNetwork.get();
}

void setIsConnectedToNetwork(bool connected) {
    isConnectedToNetwork.base_value = connected;
}

void setGpuId(s32 selectedGpuId) {
    gpuId.base_value = selectedGpuId;
}

void setWindowWidth(u32 width) {
    windowWidth.base_value = width;
}

void setWindowHeight(u32 height) {
    windowHeight.base_value = height;
}

void setInternalScreenWidth(u32 width) {
    internalScreenWidth.base_value = width;
}

void setInternalScreenHeight(u32 height) {
    internalScreenHeight.base_value = height;
}

void setDebugDump(bool enable) {
    isDebugDump.base_value = enable;
}

void setLoggingEnabled(bool enable) {
    logEnabled.base_value = enable;
}

void setCollectShaderForDebug(bool enable) {
    isShaderDebug.base_value = enable;
}

bool ShouldSkipShader(const u64& hash) {
    if (!getShaderSkipsEnabled())
        return false;

    return std::find(current_skipped_shader_hashes.begin(), current_skipped_shader_hashes.end(),
                     hash) != current_skipped_shader_hashes.end();
}

void SetSkippedShaderHashes(const string& game_id) {
    current_skipped_shader_hashes.clear();

    auto it = all_skipped_shader_hashes.find(game_id);
    if (it != all_skipped_shader_hashes.end()) {
        const auto& hashes = it->second;
        current_skipped_shader_hashes.reserve(hashes.size());
        for (const auto& hash : hashes) {
            try {
                current_skipped_shader_hashes.push_back((u64)std::stoull(hash, nullptr, 16));
            } catch (const std::invalid_argument& ex) {
                LOG_ERROR(Config, "Invalid shader hash format: {}", hash);
            } catch (const std::out_of_range& ex) {
                LOG_ERROR(Config, "Shader hash out of range: {}", hash);
            }
        }
    }
}

void setShowSplash(bool enable) {
    isShowSplash.base_value = enable;
}

void setAutoUpdate(bool enable) {
    isAutoUpdate = enable;
}

void setAlwaysShowChangelog(bool enable) {
    isAlwaysShowChangelog = enable;
}

void setSideTrophy(string side) {
    isSideTrophy = side;
}

void setNullGpu(bool enable) {
    isNullGpu.base_value = enable;
}

void setAllowHDR(bool enable) {
    isHDRAllowed.base_value = enable;
}

void setEnableAutoBackup(bool enable) {
    enableAutoBackup.base_value = enable;
}

void setCopyGPUCmdBuffers(bool enable) {
    shouldCopyGPUBuffers.base_value = enable;
}

void setDirectMemoryAccess(bool enable) {
    directMemoryAccessEnabled.base_value = enable;
}

void setDumpShaders(bool enable) {
    shouldDumpShaders.base_value = enable;
}

void setVkValidation(bool enable) {
    vkValidation.base_value = enable;
}

void setVkSyncValidation(bool enable) {
    vkValidationSync.base_value = enable;
}

void setRdocEnabled(bool enable) {
    rdocEnable.base_value = enable;
}

void setVblankFreq(u32 value) {
    vblankFrequency.base_value = value;
}

void setIsFullscreen(bool enable) {
    isFullscreen.base_value = enable;
}
static void setShowLabelsUnderIcons(bool enable) {
    showLabelsUnderIcons = enable;
}

void setFullscreenMode(string mode) {
    fullscreenMode.base_value = mode;
}

void setPresentMode(const string& mode) {
    presentMode.game_specific_value.reset(); // clear stale override
    presentMode.base_value = mode;
}

void setisTrophyPopupDisabled(bool disable) {
    isTrophyPopupDisabled.base_value = disable;
}

void setPlayBGM(bool enable) {
    playBGM = enable;
}

void setBGMvolume(int volume) {
    BGMvolume = volume;
}

void setEnableDiscordRPC(bool enable) {
    enableDiscordRPC = enable;
}

void setCursorState(s16 newCursorState) {
    cursorState.base_value = newCursorState;
}

void setCursorHideTimeout(int newcursorHideTimeout) {
    cursorHideTimeout.base_value = newcursorHideTimeout;
}

void setMicDevice(string device) {
    micDevice.base_value = device;
}

void setTrophyNotificationDuration(double newTrophyNotificationDuration) {
    trophyNotificationDuration.base_value = newTrophyNotificationDuration;
}

void setLanguage(u32 language) {
    m_language = language;
}

void setNeoMode(bool enable) {
    isNeo.base_value = enable;
}

void setDevKitMode(bool enable) {
    isDevKit.base_value = enable;
}

void setLogType(const string& type) {
    logType.game_specific_value.reset();
    logType.base_value = type;
}

void setLogFilter(const string& type) {
    logFilter.base_value = type;
}

void setSeparateLogFilesEnabled(bool enabled) {
    isSeparateLogFilesEnabled.base_value = enabled;
}

void setUserName(const string& type) {
    userName.base_value = type;
}

void setUpdateChannel(const std::string& type) {
    updateChannel = type;
}
void setChooseHomeTab(const std::string& type) {
    chooseHomeTab = type;
}

void setUseSpecialPad(bool use) {
    useSpecialPad.base_value = use;
}

void setSpecialPadClass(int type) {
    specialPadClass.base_value = type;
}

void setIsMotionControlsEnabled(bool use) {
    isMotionControlsEnabled.base_value = use;
}

void setCompatibilityEnabled(bool use) {
    compatibilityData = use;
}

void setCheckCompatibilityOnStartup(bool use) {
    checkCompatibilityOnStartup = use;
}

void setMainWindowGeometry(u32 x, u32 y, u32 w, u32 h) {
    main_window_geometry_x = x;
    main_window_geometry_y = y;
    main_window_geometry_w = w;
    main_window_geometry_h = h;
}

void setAudioVolume(int volume) {
    audioVolume.base_value = volume;
}

bool addGameInstallDir(const std::filesystem::path& dir, bool enabled) {
    for (const auto& install_dir : settings_install_dirs) {
        if (install_dir.path == dir) {
            return false;
        }
    }
    settings_install_dirs.push_back({dir, enabled});
    return true;
}

void removeGameInstallDir(const std::filesystem::path& dir) {
    auto iterator =
        std::find_if(settings_install_dirs.begin(), settings_install_dirs.end(),
                     [&dir](const GameInstallDir& install_dir) { return install_dir.path == dir; });
    if (iterator != settings_install_dirs.end()) {
        settings_install_dirs.erase(iterator);
    }
}

void setGameInstallDirEnabled(const std::filesystem::path& dir, bool enabled) {
    auto iterator =
        std::find_if(settings_install_dirs.begin(), settings_install_dirs.end(),
                     [&dir](const GameInstallDir& install_dir) { return install_dir.path == dir; });
    if (iterator != settings_install_dirs.end()) {
        iterator->enabled = enabled;
    }
}

void setAddonInstallDir(const std::filesystem::path& dir) {
    settings_addon_install_dir = dir;
}

void setMainWindowTheme(u32 theme) {
    mw_themes = theme;
}

void setIconSize(u32 size) {
    m_icon_size = size;
}

void setIconSizeGrid(u32 size) {
    m_icon_size_grid = size;
}

void setSliderPosition(u32 pos) {
    m_slider_pos = pos;
}

void setSliderPositionGrid(u32 pos) {
    m_slider_pos_grid = pos;
}

void setTableMode(u32 mode) {
    m_table_mode = mode;
}

void setMainWindowWidth(u32 width) {
    m_window_size_W = width;
}

void setMainWindowHeight(u32 height) {
    m_window_size_H = height;
}

void setElfViewer(const std::vector<std::string>& elfList) {
    m_elf_viewer.resize(elfList.size());
    m_elf_viewer = elfList;
}

void setRecentFiles(const std::vector<std::string>& recentFiles) {
    m_recent_files.resize(recentFiles.size());
    m_recent_files = recentFiles;
}

void setEmulatorLanguage(std::string language) {
    emulator_language = language;
}

void setGameInstallDirs(const std::vector<std::filesystem::path>& dirs_config) {
    settings_install_dirs.clear();
    for (const auto& dir : dirs_config) {
        settings_install_dirs.push_back({dir, true});
    }
}

void setAllGameInstallDirs(const std::vector<GameInstallDir>& dirs_config) {
    settings_install_dirs = dirs_config;
}

void setSaveDataPath(const std::filesystem::path& path) {
    save_data_path = path;
}

u32 getMainWindowGeometryX() {
    return main_window_geometry_x;
}

u32 getMainWindowGeometryY() {
    return main_window_geometry_y;
}

u32 getMainWindowGeometryW() {
    return main_window_geometry_w;
}

u32 getMainWindowGeometryH() {
    return main_window_geometry_h;
}

const std::vector<std::filesystem::path> getGameInstallDirs() {
    std::vector<std::filesystem::path> enabled_dirs;
    for (const auto& dir : settings_install_dirs) {
        if (dir.enabled) {
            enabled_dirs.push_back(dir.path);
        }
    }
    return enabled_dirs;
}

const std::vector<bool> getGameInstallDirsEnabled() {
    std::vector<bool> enabled_dirs;
    for (const auto& dir : settings_install_dirs) {
        enabled_dirs.push_back(dir.enabled);
    }
    return enabled_dirs;
}

std::filesystem::path getAddonInstallDir() {
    if (settings_addon_install_dir.empty()) {
        // Default for users without a config file or a config file from before this option existed
        return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "addcont";
    }
    return settings_addon_install_dir;
}

u32 getMainWindowTheme() {
    return mw_themes;
}

u32 getIconSize() {
    return m_icon_size;
}

u32 getIconSizeGrid() {
    return m_icon_size_grid;
}

u32 getSliderPosition() {
    return m_slider_pos;
}

u32 getSliderPositionGrid() {
    return m_slider_pos_grid;
}

u32 getTableMode() {
    return m_table_mode;
}

u32 getMainWindowWidth() {
    return m_window_size_W;
}

u32 getMainWindowHeight() {
    return m_window_size_H;
}

std::vector<std::string> getElfViewer() {
    return m_elf_viewer;
}

std::vector<std::string> getRecentFiles() {
    return m_recent_files;
}

string getEmulatorLanguage() {
    return emulator_language;
}

u32 GetLanguage() {
    return m_language;
}

bool getSeparateLogFilesEnabled() {
    return isSeparateLogFilesEnabled.get();
}

int getBackgroundImageOpacity() {
    return backgroundImageOpacity;
}

void setBackgroundImageOpacity(int opacity) {
    backgroundImageOpacity = std::clamp(opacity, 0, 100);
}

bool getShowBackgroundImage() {
    return showBackgroundImage;
}

void setShowBackgroundImage(bool show) {
    showBackgroundImage = show;
}

bool getPSNSignedIn() {
    return isPSNSignedIn.get();
}

void setPSNSignedIn(bool sign) {
    isPSNSignedIn.base_value = sign;
}

bool getShaderSkipsEnabled() {
    return shaderSkipsEnabled.get();
}

void setShaderSkipsEnabled(bool enable) {
    shaderSkipsEnabled.base_value = enable;
}

string getMemoryAlloc() {
    return memoryAlloc.get();
}

void setMemoryAlloc(string alloc) {
    memoryAlloc = alloc;
}

string getDefaultControllerID() {
    return defaultControllerID.get();
}

void setDefaultControllerID(std::string id) {
    defaultControllerID = id;
}

bool getBackgroundControllerInput() {
    return backgroundControllerInput.get();
}

void setBackgroundControllerInput(bool enable) {
    backgroundControllerInput = enable;
}

void load(const std::filesystem::path& path, bool is_game_specific) {
    // If the configuration file does not exist, create it and return, unless it is game specific
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (!is_game_specific) {
            save(path);
        }
        return;
    }

    toml::value data;

    try {
        std::ifstream ifs;
        ifs.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        ifs.open(path, std::ios_base::binary);
        data = toml::parse(ifs, std::string{fmt::UTF(path.filename().u8string()).data});
    } catch (std::exception& ex) {
        fmt::print("Got exception trying to load config file. Exception: {}\n", ex.what());
        return;
    }

    if (data.contains("General")) {
        const toml::value& general = data.at("General");
        enableAutoBackup.setFromToml(general, "enableAutoBackup", false);
        autoRestartGame.setFromToml(general, "autoRestartGame", false);
        restartWithBaseGame.setFromToml(general, "restartWithBaseGame", false);
        screenTipDisable.setFromToml(general, "screenTipDisable", is_game_specific);
        volumeSlider.setFromToml(general, "volumeSlider", is_game_specific);
        muteEnabled.setFromToml(general, "muteEnabled", is_game_specific);

        isNeo.setFromToml(general, "isPS4Pro", is_game_specific);
        isDevKit.setFromToml(general, "isDevKit", is_game_specific);
        isPSNSignedIn.setFromToml(general, "isPSNSignedIn", is_game_specific);
        playBGM = toml::find_or<bool>(general, "playBGM", false);
        isTrophyPopupDisabled =
            toml::find_or<bool>(general, "isTrophyPopupDisabled", is_game_specific);
        trophyNotificationDuration =
            toml::find_or<double>(general, "trophyNotificationDuration", 5.0);
        BGMvolume = toml::find_or<int>(general, "BGMvolume", 50);
        enableDiscordRPC = toml::find_or<bool>(general, "enableDiscordRPC", true);
        logFilter.setFromToml(general, "logFilter", is_game_specific);
        logType.setFromToml(general, "logType", is_game_specific);
        userName.setFromToml(general, "userName", "shadPS4");
        if (!Common::g_is_release) {
            updateChannel = toml::find_or<std::string>(general, "updateChannel", "BBFork");
        }
        if (updateChannel == "Release") {
            updateChannel = "BBFork";
        }
        if (updateChannel == "Full-Souls") {
            updateChannel = "BBFork";
        }
        if (updateChannel == "Nightly") {
            updateChannel = "BBFork";
        }
        if (updateChannel == "mainBB") {
            updateChannel = "BBFork";
        }
        if (updateChannel == "PartBB") {
            updateChannel = "BBFork";
        }
        if (updateChannel == "Revert") {
            updateChannel = "BBFork";
        }
        isShowSplash.setFromToml(general, "showSplash", is_game_specific);
        isAutoUpdate = toml::find_or<bool>(general, "autoUpdate", false);
        isAlwaysShowChangelog = toml::find_or<bool>(general, "alwaysShowChangelog", false);
        isSideTrophy.setFromToml(general, "sideTrophy", is_game_specific);
        compatibilityData = toml::find_or<bool>(general, "compatibilityEnabled", false);
        checkCompatibilityOnStartup =
            toml::find_or<bool>(general, "checkCompatibilityOnStartup", false);
        isConnectedToNetwork =
            toml::find_or<bool>(general, "isConnectedToNetwork", is_game_specific);
        audioBackend.setFromToml(general, "backend", "cubeb");
        audioVolume.setFromToml(general, "volume", 100);
        chooseHomeTab = toml::find_or<std::string>(general, "chooseHomeTab", chooseHomeTab);
        defaultControllerID.setFromToml(general, "defaultControllerID", "");
    }

    if (data.contains("Input")) {
        const toml::value& input = data.at("Input");

        cursorState.setFromToml(input, "cursorState", is_game_specific);
        cursorHideTimeout.setFromToml(input, "cursorHideTimeout", is_game_specific);
        useSpecialPad.setFromToml(input, "useSpecialPad", is_game_specific);
        specialPadClass.setFromToml(input, "specialPadClass", is_game_specific);
        isMotionControlsEnabled =
            toml::find_or<bool>(input, "isMotionControlsEnabled", is_game_specific);
        useUnifiedInputConfig =
            toml::find_or<bool>(input, "useUnifiedInputConfig", is_game_specific);
        micDevice.setFromToml(input, "micDevice", is_game_specific);
        backgroundControllerInput =
            toml::find_or<bool>(input, "backgroundControllerInput", is_game_specific);
    }

    if (data.contains("GPU")) {
        const toml::value& gpu = data.at("GPU");

        screenWidth.setFromToml(gpu, "screenWidth", is_game_specific);
        screenHeight.setFromToml(gpu, "screenHeight", is_game_specific);
        fsrEnabled.setFromToml(gpu, "fsrEnabled", is_game_specific);
        rcasEnabled.setFromToml(gpu, "rcasEnabled", is_game_specific);
        rcas_attenuation = toml::find_or<float>(gpu, "rcas_attenuation", is_game_specific);
        isNullGpu.setFromToml(gpu, "nullGpu", false);
        shouldCopyGPUBuffers.setFromToml(gpu, "copyGPUBuffers", is_game_specific);
        directMemoryAccessEnabled =
            toml::find_or<bool>(gpu, "directMemoryAccess", is_game_specific);
        shouldDumpShaders.setFromToml(gpu, "dumpShaders", is_game_specific);
        shouldPatchShaders.setFromToml(gpu, "patchShaders", is_game_specific);
        vblankFrequency.setFromToml(gpu, "vblankFrequency", is_game_specific);
        isFullscreen.setFromToml(gpu, "Fullscreen", is_game_specific);
        fullscreenMode.setFromToml(gpu, "FullscreenMode", is_game_specific);
        isHDRAllowed.setFromToml(gpu, "allowHDR", is_game_specific);
        shaderSkipsEnabled.setFromToml(gpu, "shaderSkipsEnabled", is_game_specific);
        memoryAlloc.setFromToml(gpu, "memoryAlloc", is_game_specific);
        windowWidth.setFromToml(gpu, "screenWidth", is_game_specific);
        fpsLimit.setFromToml(gpu, "fpsLimit", is_game_specific);
        g_fpsLimiterEnabled.setFromToml(gpu, "fpsLimiterEnabled", is_game_specific);

        windowHeight.setFromToml(gpu, "screenHeight", is_game_specific);
        internalScreenWidth.setFromToml(gpu, "internalScreenWidth", is_game_specific);
        internalScreenHeight.setFromToml(gpu, "internalScreenHeight", is_game_specific);
        isNullGpu.setFromToml(gpu, "nullGpu", is_game_specific);
        shouldCopyGPUBuffers.setFromToml(gpu, "copyGPUBuffers", is_game_specific);
        readbackSpeedMode = static_cast<ReadbackSpeed>(
            toml::find_or<int>(gpu, "readbackSpeed", static_cast<int>(is_game_specific)));
        readbackLinearImagesEnabled =
            toml::find_or<bool>(gpu, "readbackLinearImages", is_game_specific);
        directMemoryAccessEnabled =
            toml::find_or<bool>(gpu, "directMemoryAccess", is_game_specific);
        shouldDumpShaders.setFromToml(gpu, "dumpShaders", is_game_specific);
        shouldPatchShaders.setFromToml(gpu, "patchShaders", is_game_specific);
        vblankFrequency.setFromToml(gpu, "vblankFrequency", is_game_specific);
        isFullscreen.setFromToml(gpu, "Fullscreen", is_game_specific);
        fullscreenMode.setFromToml(gpu, "FullscreenMode", is_game_specific);
        presentMode.setFromToml(gpu, "presentMode", is_game_specific);
        isHDRAllowed.setFromToml(gpu, "allowHDR", is_game_specific);
    }

    if (data.contains("Vulkan")) {
        const toml::value& vk = data.at("Vulkan");

        gpuId.setFromToml(vk, "gpuId", is_game_specific);
        vkValidation.setFromToml(vk, "validation", is_game_specific);
        vkValidationSync.setFromToml(vk, "validation_sync", is_game_specific);
        vkValidationGpu.setFromToml(vk, "validation_gpu", is_game_specific);
        vkCrashDiagnostic.setFromToml(vk, "crashDiagnostic", is_game_specific);
        vkHostMarkers.setFromToml(vk, "hostMarkers", is_game_specific);
        vkGuestMarkers.setFromToml(vk, "guestMarkers", is_game_specific);
        rdocEnable.setFromToml(vk, "rdocEnable", is_game_specific);
    }
    string current_version = {};

    if (data.contains("Debug")) {
        const toml::value& debug = data.at("Debug");

        isDebugDump.setFromToml(debug, "DebugDump", is_game_specific);
        isSeparateLogFilesEnabled =
            toml::find_or<bool>(debug, "isSeparateLogFilesEnabled", is_game_specific);
        isShaderDebug.setFromToml(debug, "CollectShader", is_game_specific);
        isFpsColor.setFromToml(debug, "FPSColor", is_game_specific);
        logEnabled.setFromToml(debug, "logEnabled", is_game_specific);
        current_version = toml::find_or<std::string>(debug, "ConfigVersion", current_version);
    }

    if (data.contains("GUI")) {
        const toml::value& gui = data.at("GUI");

        load_game_size = toml::find_or<bool>(gui, "loadGameSizeEnabled", true);
        m_icon_size = toml::find_or<int>(gui, "iconSize", 0);
        m_icon_size_grid = toml::find_or<int>(gui, "iconSizeGrid", 0);
        m_slider_pos = toml::find_or<int>(gui, "sliderPos", 0);
        m_slider_pos_grid = toml::find_or<int>(gui, "sliderPosGrid", 0);
        mw_themes = toml::find_or<int>(gui, "theme", 0);
        m_window_size_W = toml::find_or<int>(gui, "mw_width", 0);
        m_window_size_H = toml::find_or<int>(gui, "mw_height", 0);
        load_game_size = toml::find_or<bool>(gui, "loadGameSizeEnabled", load_game_size);

        const auto install_dir_array =
            toml::find_or<std::vector<std::u8string>>(gui, "installDirs", {});

        try {
            install_dirs_enabled = toml::find<std::vector<bool>>(gui, "installDirsEnabled");
        } catch (...) {
            // If it does not exist, assume that all are enabled.
            install_dirs_enabled.resize(install_dir_array.size(), true);
        }

        if (install_dirs_enabled.size() < install_dir_array.size()) {
            install_dirs_enabled.resize(install_dir_array.size(), true);
        }

        settings_install_dirs.clear();
        for (size_t i = 0; i < install_dir_array.size(); i++) {
            settings_install_dirs.push_back(
                {std::filesystem::path{install_dir_array[i]}, install_dirs_enabled[i]});
        }

        save_data_path = toml::find_fs_path_or(gui, "saveDataPath", save_data_path);

        settings_addon_install_dir =
            toml::find_fs_path_or(gui, "addonInstallDir", settings_addon_install_dir);

        settings_addon_install_dir = toml::find_fs_path_or(gui, "addonInstallDir", {});
        main_window_geometry_x = toml::find_or<int>(gui, "geometry_x", 0);
        main_window_geometry_y = toml::find_or<int>(gui, "geometry_y", 0);
        main_window_geometry_w = toml::find_or<int>(gui, "geometry_w", 0);
        main_window_geometry_h = toml::find_or<int>(gui, "geometry_h", 0);
        m_elf_viewer = toml::find_or<std::vector<std::string>>(gui, "elfDirs", {});
        m_recent_files = toml::find_or<std::vector<std::string>>(gui, "recentFiles", {});
        m_table_mode = toml::find_or<int>(gui, "gameTableMode", 0);
        emulator_language = toml::find_or<std::string>(gui, "emulatorLanguage", "en_US");
        backgroundImageOpacity = toml::find_or<int>(gui, "backgroundImageOpacity", 50);
        showBackgroundImage = toml::find_or<bool>(gui, "showBackgroundImage", true);
    }

    if (data.contains("Settings")) {
        const toml::value& settings = data.at("Settings");
        m_language = toml::find_or<int>(settings, "consoleLanguage", m_language);
    }

    if (data.contains("Keys")) {
        const toml::value& keys = data.at("Keys");
        trophyKey = toml::find_or<std::string>(keys, "TrophyKey", trophyKey);
    }

    if (data.contains("ShaderSkip")) {
        const toml::value& shader_skip_data = data.at("ShaderSkip");
        for (const auto& [game_id, hash_list] : shader_skip_data.as_table()) {
            std::vector<std::string> hashes;
            for (const auto& hash : hash_list.as_array()) {
                hashes.push_back(hash.as_string());
            }
            all_skipped_shader_hashes[game_id] = std::move(hashes);
        }
    }

    // Check if the loaded language is in the allowed list
    const std::vector<std::string> allowed_languages = {
        "ar_SA", "da_DK", "de_DE", "el_GR", "en_US", "es_ES", "fa_IR", "fi_FI", "fr_FR", "hu_HU",
        "id_ID", "it_IT", "ja_JP", "ko_KR", "lt_LT", "nb_NO", "nl_NL", "pl_PL", "pt_BR", "pt_PT",
        "ro_RO", "ru_RU", "sq_AL", "sv_SE", "tr_TR", "uk_UA", "vi_VN", "zh_CN", "zh_TW"};

    if (std::find(allowed_languages.begin(), allowed_languages.end(), emulator_language) ==
        allowed_languages.end()) {
        emulator_language = "en_US"; // Default to en_US if not in the list
        save(path);
    }
}

void sortTomlSections(toml::ordered_value& data) {
    toml::ordered_value ordered_data;
    std::vector<std::string> section_order = {"General", "Input", "GPU", "Vulkan",
                                              "Debug",   "Keys",  "GUI", "Settings"};
    section_order.insert(section_order.begin() + 8, "ShaderSkip");

    for (const auto& section : section_order) {
        if (data.contains(section)) {
            std::vector<std::string> keys;
            for (const auto& item : data.at(section).as_table()) {
                keys.push_back(item.first);
            }

            std::sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b) {
                return std::lexicographical_compare(
                    a.begin(), a.end(), b.begin(), b.end(), [](char a_char, char b_char) {
                        return std::tolower(a_char) < std::tolower(b_char);
                    });
            });

            toml::ordered_value ordered_section;
            for (const auto& key : keys) {
                ordered_section[key] = data.at(section).at(key);
            }

            ordered_data[section] = ordered_section;
        }
    }

    data = ordered_data;
}

void save(const std::filesystem::path& path) {
    toml::ordered_value data;

    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        try {
            std::ifstream ifs;
            ifs.exceptions(std::ifstream::failbit | std::ifstream::badbit);
            ifs.open(path, std::ios_base::binary);
            data = toml::parse<toml::ordered_type_config>(
                ifs, std::string{fmt::UTF(path.filename().u8string()).data});
        } catch (const std::exception& ex) {
            fmt::print("Exception trying to parse config file. Exception: {}\n", ex.what());
            return;
        }
    } else {
        if (error) {
            fmt::print("Filesystem error: {}\n", error.message());
        }
        fmt::print("Saving new configuration file {}\n", fmt::UTF(path.u8string()));
    }

    data["General"]["volumeSlider"] = volumeSlider.base_value;
    data["General"]["muteEnabled"] = muteEnabled.base_value;

    data["General"]["isPS4Pro"] = isNeo.base_value;
    data["General"]["isDevKit"] = isDevKit.base_value;
    data["General"]["isPSNSignedIn"] = isPSNSignedIn.base_value;
    data["General"]["isTrophyPopupDisabled"] = isTrophyPopupDisabled.base_value;
    data["General"]["trophyNotificationDuration"] = trophyNotificationDuration.base_value;
    data["General"]["playBGM"] = playBGM;
    data["General"]["BGMvolume"] = BGMvolume;
    data["General"]["enableDiscordRPC"] = enableDiscordRPC;
    data["General"]["logFilter"] = logFilter.base_value;
    data["General"]["logType"] = logType.base_value;
    data["General"]["userName"] = userName.base_value;
    data["General"]["updateChannel"] = updateChannel;
    data["General"]["chooseHomeTab"] = chooseHomeTab;
    data["General"]["showSplash"] = isShowSplash.base_value;
    data["General"]["isPS4Pro"] = isNeo.base_value;

    data["General"]["sideTrophy"] = isSideTrophy.base_value;
    data["General"]["compatibilityEnabled"] = compatibilityData;
    data["General"]["checkCompatibilityOnStartup"] = checkCompatibilityOnStartup;
    data["General"]["isConnectedToNetwork"] = isConnectedToNetwork.base_value;
    data["General"]["defaultControllerID"] = defaultControllerID.base_value;
    data["Input"]["cursorState"] = cursorState.base_value;
    data["Input"]["cursorHideTimeout"] = cursorHideTimeout.base_value;
    data["Input"]["useSpecialPad"] = useSpecialPad.base_value;
    data["Input"]["specialPadClass"] = specialPadClass.base_value;
    data["Input"]["isMotionControlsEnabled"] = isMotionControlsEnabled.base_value;
    data["Input"]["useUnifiedInputConfig"] = useUnifiedInputConfig.base_value;
    data["GPU"]["screenWidth"] = screenWidth.base_value;
    data["GPU"]["screenHeight"] = screenHeight.base_value;
    data["GPU"]["rcas_attenuation"] = rcas_attenuation.base_value;
    data["GPU"]["fsrEnabled"] = fsrEnabled.base_value;
    data["GPU"]["rcasEnabled"] = rcasEnabled.base_value;
    data["Input"]["micDevice"] = micDevice.base_value;
    data["Input"]["backgroundControllerInput"] = backgroundControllerInput.base_value;
    data["GPU"]["fpsLimit"] = fpsLimit.base_value;
    data["GPU"]["fpsLimiterEnabled"] = g_fpsLimiterEnabled.base_value;

    data["GPU"]["screenWidth"] = windowWidth.base_value;
    data["GPU"]["screenHeight"] = windowHeight.base_value;
    data["GPU"]["internalScreenWidth"] = internalScreenWidth.base_value;
    data["GPU"]["internalScreenHeight"] = internalScreenHeight.base_value;
    data["GPU"]["nullGpu"] = isNullGpu.base_value;
    data["GPU"]["copyGPUBuffers"] = shouldCopyGPUBuffers.base_value;
    data["GPU"]["readbackSpeed"] = static_cast<int>(readbackSpeedMode.base_value);
    data["GPU"]["readbackLinearImages"] = readbackLinearImagesEnabled.base_value;
    data["GPU"]["directMemoryAccess"] = directMemoryAccessEnabled.base_value;
    data["GPU"]["dumpShaders"] = shouldDumpShaders.base_value;
    data["GPU"]["patchShaders"] = shouldPatchShaders.base_value;
    data["GPU"]["vblankFrequency"] = vblankFrequency.base_value;
    data["GPU"]["Fullscreen"] = isFullscreen.base_value;
    data["GPU"]["FullscreenMode"] = fullscreenMode.base_value;
    data["GPU"]["presentMode"] = presentMode.base_value;
    data["GPU"]["allowHDR"] = isHDRAllowed.base_value;
    data["General"]["enableAutoBackup"] = enableAutoBackup.base_value;
    data["General"]["autoRestartGame"] = autoRestartGame.base_value;
    data["General"]["restartWithBaseGame"] = restartWithBaseGame.base_value;
    data["General"]["screenTipDisable"] = screenTipDisable.base_value;
    data["GPU"]["shaderSkipsEnabled"] = shaderSkipsEnabled.base_value;
    data["GPU"]["memoryAlloc"] = memoryAlloc.base_value;
    data["Vulkan"]["gpuId"] = gpuId.base_value;
    data["Vulkan"]["validation"] = vkValidation.base_value;
    data["Vulkan"]["validation_sync"] = vkValidationSync.base_value;
    data["Vulkan"]["validation_gpu"] = vkValidationGpu.base_value;
    data["Vulkan"]["crashDiagnostic"] = vkCrashDiagnostic.base_value;
    data["Vulkan"]["hostMarkers"] = vkHostMarkers.base_value;
    data["Vulkan"]["guestMarkers"] = vkGuestMarkers.base_value;
    data["Vulkan"]["rdocEnable"] = rdocEnable.base_value;
    data["General"]["backend"] = audioBackend.base_value;
    data["General"]["volume"] = audioVolume.base_value;
    data["Debug"]["DebugDump"] = isDebugDump.base_value;
    data["Debug"]["CollectShader"] = isShaderDebug.base_value;
    data["Debug"]["isSeparateLogFilesEnabled"] = isSeparateLogFilesEnabled.base_value;
    data["Debug"]["FPSColor"] = isFpsColor.base_value;
    data["Debug"]["logEnabled"] = logEnabled.base_value;
    data["Keys"]["TrophyKey"] = trophyKey;

    std::vector<std::string> install_dirs;
    std::vector<bool> install_dirs_enabled;

    // temporary structure for ordering
    struct DirEntry {
        std::string path_str;
        bool enabled;
    };

    std::vector<DirEntry> sorted_dirs;
    for (const auto& dirInfo : settings_install_dirs) {
        sorted_dirs.push_back(
            {std::string{fmt::UTF(dirInfo.path.u8string()).data}, dirInfo.enabled});
    }

    // Sort directories alphabetically
    std::sort(sorted_dirs.begin(), sorted_dirs.end(), [](const DirEntry& a, const DirEntry& b) {
        return std::lexicographical_compare(
            a.path_str.begin(), a.path_str.end(), b.path_str.begin(), b.path_str.end(),
            [](char a_char, char b_char) { return std::tolower(a_char) < std::tolower(b_char); });
    });

    for (const auto& entry : sorted_dirs) {
        install_dirs.push_back(entry.path_str);
        install_dirs_enabled.push_back(entry.enabled);
    }

    data["GUI"]["installDirs"] = install_dirs;
    data["GUI"]["installDirsEnabled"] = install_dirs_enabled;
    data["GUI"]["saveDataPath"] = std::string{fmt::UTF(save_data_path.u8string()).data};
    data["GUI"]["loadGameSizeEnabled"] = load_game_size;

    data["GUI"]["addonInstallDir"] =
        std::string{fmt::UTF(settings_addon_install_dir.u8string()).data};
    data["GUI"]["emulatorLanguage"] = emulator_language;
    data["GUI"]["backgroundImageOpacity"] = backgroundImageOpacity;
    data["GUI"]["showBackgroundImage"] = showBackgroundImage;
    data["Settings"]["consoleLanguage"] = m_language;
    toml::value shader_skip_data;
    for (const auto& [game_id, hashes] : all_skipped_shader_hashes) {
        std::vector<toml::value> hash_values;
        for (const auto& hash : hashes) {
            hash_values.emplace_back(hash);
        }
        shader_skip_data[game_id] = hash_values;
    }

    data["ShaderSkip"] = shader_skip_data;

    // Sorting of TOML sections
    sortTomlSections(data);

    std::ofstream file(path, std::ios::binary);
    file << data;
    file.close();

    saveMainWindow(path);
}

void saveMainWindow(const std::filesystem::path& path) {
    toml::ordered_value data;

    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        try {
            std::ifstream ifs;
            ifs.exceptions(std::ifstream::failbit | std::ifstream::badbit);
            ifs.open(path, std::ios_base::binary);
            data = toml::parse<toml::ordered_type_config>(
                ifs, std::string{fmt::UTF(path.filename().u8string()).data});
        } catch (const std::exception& ex) {
            fmt::print("Exception trying to parse config file. Exception: {}\n", ex.what());
            return;
        }
    } else {
        if (error) {
            fmt::print("Filesystem error: {}\n", error.message());
        }
        fmt::print("Saving new configuration file {}\n", fmt::UTF(path.u8string()));
    }

    data["GUI"]["mw_width"] = m_window_size_W;
    data["GUI"]["mw_height"] = m_window_size_H;
    data["GUI"]["theme"] = mw_themes;
    data["GUI"]["iconSize"] = m_icon_size;
    data["GUI"]["sliderPos"] = m_slider_pos;
    data["GUI"]["iconSizeGrid"] = m_icon_size_grid;
    data["GUI"]["sliderPosGrid"] = m_slider_pos_grid;
    data["GUI"]["gameTableMode"] = m_table_mode;
    data["GUI"]["geometry_x"] = main_window_geometry_x;
    data["GUI"]["geometry_y"] = main_window_geometry_y;
    data["GUI"]["geometry_w"] = main_window_geometry_w;
    data["GUI"]["geometry_h"] = main_window_geometry_h;
    data["GUI"]["elfDirs"] = m_elf_viewer;
    data["GUI"]["recentFiles"] = m_recent_files;

    // Sorting of TOML sections
    sortTomlSections(data);

    std::ofstream file(path, std::ios::binary);
    file << data;
    file.close();
}

void setDefaultValues() {
    // General
    isNeo = false;
    isDevKit = false;
    isPSNSignedIn = false;
    isTrophyPopupDisabled = false;
    trophyNotificationDuration = 6.0;
    enableDiscordRPC = false;
    playBGM = false;
    BGMvolume = 50;
    enableDiscordRPC = true;
    screenWidth = 1280;
    screenHeight = 720;
    logFilter = "";
    logType = "sync";
    userName = "shadPS4";
    memoryAlloc = "medium";
    chooseHomeTab = "General";
    isShowSplash = false;
    isSideTrophy = "right";
    compatibilityData = false;
    checkCompatibilityOnStartup = false;
    isConnectedToNetwork = false;
    autoRestartGame = false;
    restartWithBaseGame = false;
    screenTipDisable = false;

    // Input
    cursorState = HideCursorState::Idle;
    cursorHideTimeout = 5;
    useSpecialPad = false;
    specialPadClass = 1;
    isMotionControlsEnabled = true;
    useUnifiedInputConfig = true;
    overrideControllerColor = false;
    controllerCustomColorRGB[0] = 0;
    controllerCustomColorRGB[1] = 0;
    controllerCustomColorRGB[2] = 255;
    micDevice = "Default Device";
    backgroundControllerInput = false;

    // GPU
    screenWidth = 1280;
    screenHeight = 720;
    isDebugDump = false;
    isShaderDebug = false;
    isShowSplash = false;
    isAutoUpdate = false;
    isAlwaysShowChangelog = false;
    isSideTrophy = "right";
    windowWidth = 1280;
    windowHeight = 720;
    internalScreenWidth = 1280;
    internalScreenHeight = 720;
    isNullGpu = false;
    shouldCopyGPUBuffers = false;
    readbackSpeedMode = ReadbackSpeed::Default;
    shaderSkipsEnabled = false;
    readbackLinearImagesEnabled = false;
    screenTipDisable = false;
    directMemoryAccessEnabled = false;
    shouldDumpShaders = false;
    shouldPatchShaders = false;
    vblankFrequency = 60;
    isFullscreen = false;
    fullscreenMode = "Windowed";
    presentMode = "Mailbox";
    isHDRAllowed = false;
    fsrEnabled = true;
    rcasEnabled = true;
    rcas_attenuation = 250;
    fpsLimit = 60;
    g_fpsLimiterEnabled = false;

    // Vulkan
    gpuId = -1;
    vkValidation = false;
    vkValidationSync = false;
    vkValidationGpu = false;
    vkCrashDiagnostic = false;
    vkHostMarkers = false;
    vkGuestMarkers = false;
    rdocEnable = false;

    // Debug
    isDebugDump = false;
    isShaderDebug = false;
    isSeparateLogFilesEnabled = false;
    isFpsColor = true;
    logEnabled = true;

    // GUI
    load_game_size = true;
    volumeSlider = 100;
    muteEnabled = false;

    // Settings
    emulator_language = "en_US";
    m_language = 1;
    gpuId = -1;
    compatibilityData = false;
    checkCompatibilityOnStartup = false;
    backgroundImageOpacity = 50;
    showBackgroundImage = true;
    audioBackend = "cubeb";
    audioVolume = 100;
}

constexpr std::string_view GetDefaultGlobalConfig() {
    return R"(# Anything put here will be loaded for all games,
# alongside the game's config or default.ini depending on your preference.

hotkey_renderdoc_capture = f12
hotkey_fullscreen = f11
hotkey_show_fps = f10
hotkey_pause = f9
hotkey_reload_inputs = f8
hotkey_toggle_mouse_to_joystick = f7
hotkey_toggle_mouse_to_gyro = f6
hotkey_quit = lctrl, lshift, end
)";
}

constexpr std::string_view GetDefaultInputConfig() {
    return R"(#Feeling lost? Check out the Help section!

# Keyboard bindings

triangle = f
circle = space
cross = e
square = r

pad_up = w, lalt
pad_up = mousewheelup
pad_down = s, lalt
pad_down = mousewheeldown
pad_left = a, lalt
pad_left = mousewheelleft
pad_right = d, lalt
pad_right = mousewheelright

l1 = rightbutton, lshift
r1 = leftbutton
l2 = rightbutton
r2 = leftbutton, lshift
l3 = x
r3 = q
r3 = middlebutton

options = escape
touchpad = g

key_toggle = i, lalt
mouse_to_joystick = right
mouse_movement_params = 0.5, 1, 0.125
leftjoystick_halfmode = lctrl

axis_left_x_minus = a
axis_left_x_plus = d
axis_left_y_minus = w
axis_left_y_plus = s

# Controller bindings

triangle = triangle
cross = cross
square = square
circle = circle

l1 = l1
l2 = l2
l3 = l3
r1 = r1
r2 = r2
r3 = r3

options = options
touchpad_center = back

pad_up = pad_up
pad_down = pad_down
pad_left = pad_left
pad_right = pad_right

axis_left_x = axis_left_x
axis_left_y = axis_left_y
axis_right_x = axis_right_x
axis_right_y = axis_right_y

# Range of deadzones: 1 (almost none) to 127 (max)
analog_deadzone = leftjoystick, 2, 127
analog_deadzone = rightjoystick, 2, 127

override_controller_color = false, 0, 0, 255
)";
}
std::filesystem::path GetFoolproofInputConfigFile(const std::string& game_id) {
    // Read configuration file of the game, and if it doesn't exist, generate it from default
    // If that doesn't exist either, generate that from getDefaultConfig() and try again
    // If even the folder is missing, we start with that.

    const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "input_config";
    const auto config_file = config_dir / (game_id + ".ini");
    const auto default_config_file = config_dir / "default.ini";

    // Ensure the config directory exists
    if (!std::filesystem::exists(config_dir)) {
        std::filesystem::create_directories(config_dir);
    }

    // Check if the default config exists
    if (!std::filesystem::exists(default_config_file)) {
        // If the default config is also missing, create it from getDefaultConfig()
        const auto default_config = GetDefaultInputConfig();
        std::ofstream default_config_stream(default_config_file);
        if (default_config_stream) {
            default_config_stream << default_config;
        }
    }

    // if empty, we only need to execute the function up until this point
    if (game_id.empty()) {
        return default_config_file;
    }

    // Create global config if it doesn't exist yet
    if (game_id == "global" && !std::filesystem::exists(config_file)) {
        if (!std::filesystem::exists(config_file)) {
            const auto global_config = GetDefaultGlobalConfig();
            std::ofstream global_config_stream(config_file);
            if (global_config_stream) {
                global_config_stream << global_config;
            }
        }
    }

    // If game-specific config doesn't exist, create it from the default config
    if (!std::filesystem::exists(config_file)) {
        std::filesystem::copy(default_config_file, config_file);
    }
    return config_file;
}

} // namespace Config