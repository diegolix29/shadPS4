// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <optional>
#include <string>
#include <fmt/core.h>
#include <fmt/xchar.h> // for wstring support
#include <toml.hpp>

#include "common/assert.h"
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
        auto str = find<std::string>(v, ky);
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

    if constexpr (std::is_same_v<T, int>) {
        if (it->second.is_integer()) {
            return static_cast<s32>(toml::get<int>(it->second));
        }
    } else if constexpr (std::is_same_v<T, unsigned int>) {
        if (it->second.is_integer()) {
            return static_cast<u32>(toml::get<unsigned int>(it->second));
        }
    } else if constexpr (std::is_same_v<T, double>) {
        if (it->second.is_floating()) {
            return toml::get<T>(it->second);
        }
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (it->second.is_string()) {
            return toml::get<T>(it->second);
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        if (it->second.is_boolean()) {
            return toml::get<T>(it->second);
        }
    } else if constexpr (std::is_same_v<T, std::array<string, 4>>) {
        if (it->second.is_array()) {
            return toml::get<T>(it->second);
        }
    } else {
        static_assert([] { return false; }(), "Unsupported type in get_optional<T>");
    }

    return std::nullopt;
}

} // namespace toml

namespace Config {

ConfigMode config_mode = ConfigMode::Default;

void setConfigMode(ConfigMode mode) {
    config_mode = mode;
}

template <typename T>
class ConfigEntry {
public:
    const T default_value;
    T base_value;
    optional<T> game_specific_value;
    ConfigEntry(const T& t = T()) : default_value(t), base_value(t), game_specific_value(nullopt) {}
    ConfigEntry operator=(const T& t) {
        base_value = t;
        return *this;
    }
    const T get() const {
        switch (config_mode) {
        case ConfigMode::Default:
            return game_specific_value.value_or(base_value);
        case ConfigMode::Global:
            return base_value;
        case ConfigMode::Clean:
            return default_value;
        default:
            UNREACHABLE();
        }
    }
    void setFromToml(const toml::value& v, const std::string& key, bool is_game_specific = false) {
        if (is_game_specific) {
            game_specific_value = toml::get_optional<T>(v, key);
        } else {
            base_value = toml::get_optional<T>(v, key).value_or(base_value);
        }
    }
    void set(const T value, bool is_game_specific = false) {
        is_game_specific ? game_specific_value = value : base_value = value;
    }
    void setDefault(bool is_game_specific = false) {
        is_game_specific ? game_specific_value = default_value : base_value = default_value;
    }
    void setTomlValue(toml::ordered_value& data, const std::string& header, const std::string& key,
                      bool is_game_specific = false) {
        if (is_game_specific) {
            data[header][key] = game_specific_value.value_or(base_value);
            game_specific_value = std::nullopt;
        } else {
            data[header][key] = base_value;
        }
    }
    // operator T() {
    //     return get();
    // }
};
// General
static ConfigEntry<bool> isNeo(false);
static ConfigEntry<bool> isDevKit(false);
static ConfigEntry<bool> isPSNSignedIn(false);
static ConfigEntry<bool> isTrophyPopupDisabled(false);
static ConfigEntry<double> trophyNotificationDuration(6.0);
static ConfigEntry<std::string> logFilter("");
static ConfigEntry<std::string> logType("sync");
static ConfigEntry<std::array<std::string, 4>> userNames({
    "shadPS4",
    "shadps4-2",
    "shadPS4-3",
    "shadPS4-4",
});
static std::string chooseHomeTab = "General";
static ConfigEntry<bool> isShowSplash(false);
static bool isAutoUpdate = false;
static ConfigEntry<bool> pauseOnUnfocus(false);
static bool showWelcomeDialog = true;
static ConfigEntry<bool> isDisableHardcodedHotkeys(false);
static ConfigEntry<bool> homeButtonHotkey(false);
static bool isAlwaysShowChangelog = false;
static ConfigEntry<std::string> isSideTrophy("right");
static ConfigEntry<bool> isConnectedToNetwork(false);
static bool enableDiscordRPC = false;
static bool checkCompatibilityOnStartup = false;
static bool compatibilityData = false;
static std::filesystem::path sys_modules_path = {};
static bool bootGamesMenu = false;
static bool bootHubMenu = false;
static bool restartWithBaseGame = false;
static bool separateupdatefolder = false;
static ConfigEntry<bool> screenTipDisable(false);
static ConfigEntry<bool> fpsLimiterEnabled(false);
static std::string guiStyle = "Fusion";
static std::string g_customBackgroundImage;
static ConfigEntry<bool> firstBootHandled(false);
static std::string version_path;

// Input
static ConfigEntry<int> cursorState(HideCursorState::Idle);
static ConfigEntry<int> cursorHideTimeout(5);
static ConfigEntry<bool> isMotionControlsEnabled(true);
static ConfigEntry<bool> useUnifiedInputConfig(true);
static ConfigEntry<std::string> micDevice("Default Device");
static ConfigEntry<std::string> defaultControllerID("");
static ConfigEntry<bool> backgroundControllerInput(false);
static ConfigEntry<bool> useSpecialPads[4] = {false, false, false, false};
static ConfigEntry<int> specialPadClasses[4] = {1, 1, 1, 1};
static ConfigEntry<string> mainOutputDevice("Default Device");
static ConfigEntry<string> padSpkOutputDevice("Default Device");
static ConfigEntry<int> extraDmemInMbytes(0);
static ConfigEntry<int> usbDeviceBackend(UsbBackendType::Real);

// Non-config runtime-only
static bool overrideControllerColor = false;
static int controllerCustomColorRGB[3] = {0, 0, 255};

// GPU
static ConfigEntry<u32> windowWidth(1280);
static ConfigEntry<u32> windowHeight(720);
static ConfigEntry<u32> internalScreenWidth(1280);
static ConfigEntry<u32> internalScreenHeight(720);
static ConfigEntry<bool> isNullGpu(false);
static ConfigEntry<bool> shouldCopyGPUBuffers(false);
static ConfigEntry<ReadbackSpeed> readbackSpeedMode(ReadbackSpeed::Disable);
static ConfigEntry<bool> readbackLinearImagesEnabled(false);
static ConfigEntry<bool> directMemoryAccessEnabled(false);
static ConfigEntry<bool> shouldDumpShaders(false);
static ConfigEntry<bool> shouldPatchShaders(false);
static ConfigEntry<u32> vblankFrequency(60);
static ConfigEntry<bool> isFullscreen(false);
static ConfigEntry<std::string> fullscreenMode("Windowed");
static ConfigEntry<string> presentMode("Mailbox");
static ConfigEntry<bool> isHDRAllowed(false);
static ConfigEntry<bool> fsrEnabled(true);
static ConfigEntry<bool> rcasEnabled(true);

// Audio / BGM
static bool playBGM = false;
static ConfigEntry<int> rcasAttenuation(250);
static int BGMvolume = 50;

// Vulkan
static ConfigEntry<s32> gpuId(-1);
static ConfigEntry<bool> vkValidation(false);
static ConfigEntry<bool> vkValidationCore(true);
static ConfigEntry<bool> vkValidationSync(false);
static ConfigEntry<bool> vkValidationGpu(false);
static ConfigEntry<bool> vkCrashDiagnostic(false);
static ConfigEntry<bool> vkHostMarkers(false);
static ConfigEntry<bool> vkGuestMarkers(false);
static ConfigEntry<bool> rdocEnable(false);
static ConfigEntry<bool> pipelineCacheEnable(false);
static ConfigEntry<bool> pipelineCacheArchive(true);

// Debug
static ConfigEntry<bool> isDebugDump(false);
static ConfigEntry<bool> isShaderDebug(false);
static ConfigEntry<bool> isSeparateLogFilesEnabled(false);
static ConfigEntry<bool> shaderSkipsEnabled(false);
static ConfigEntry<bool> isFpsColor(true);
static ConfigEntry<bool> showFpsCounter(false);
static ConfigEntry<bool> fpsColorState(false);
static ConfigEntry<bool> logEnabled(true);

std::unordered_map<std::string, std::vector<std::string>> all_skipped_shader_hashes = {
    {"CUSA16195", {"5f8eaca5", "5469af28"}},
    {"CUSA00018",
     {"7b9fc304d5a8f0de", "f5874f2a8d7f2037", "f5874f2a65f418f9", "25593f798d7f2037",
      "25593f7965f418f9", "2537adba98213a66", "fe36adba8c8b5626"}},
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
static std::vector<GameDirectories> settings_directories = {};
std::vector<bool> directories_enabled = {};
std::filesystem::path settings_addon_directories = {};
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
static bool enableColorFilter = true;
static std::string updateChannel;
static ConfigEntry<int> volumeSlider(100);
static ConfigEntry<bool> muteEnabled(false);
static ConfigEntry<u32> fpsLimit(60);
static bool isGameRunning = false;
static bool isSDL = false;
static bool isQT = false;
static bool load_auto_patches = true;
static bool launcher_boot = false;
std::unordered_map<std::string, bool> toolbar_visibility_settings;

bool getToolbarWidgetVisibility(const std::string& name, bool default_value) {
    if (toolbar_visibility_settings.count(name)) {
        return toolbar_visibility_settings.at(name);
    }
    return default_value;
}

void setToolbarWidgetVisibility(const std::string& name, bool is_visible) {
    toolbar_visibility_settings[name] = is_visible;
}

bool getGameRunning() {
    return isGameRunning;
}

void setGameRunning(bool running) {
    isGameRunning = running;
}

bool getSdlInstalled() {
    return isSDL;
}

void setSdlInstalled(bool use) {
    isSDL = use;
}
bool getQTInstalled() {
    return isQT;
}

void setQTInstalled(bool use) {
    isQT = use;
}

// Settings
u32 m_language = 1; // english

// Keys
static std::string trophyKey = "";

bool getShowWelcomeDialog() {
    return showWelcomeDialog;
}

bool isPipelineCacheArchived() {
    return pipelineCacheArchive.get();
}

void setPipelineCacheArchived(bool enable, bool is_game_specific) {
    pipelineCacheArchive.set(enable, is_game_specific);
}

bool isPipelineCacheEnabled() {
    return pipelineCacheEnable.get();
}

void setPipelineCacheEnabled(bool enable, bool is_game_specific) {
    pipelineCacheEnable.set(enable, is_game_specific);
}

void setShowWelcomeDialog(bool enable) {
    showWelcomeDialog = enable;
}

bool getPauseOnUnfocus() {
    return pauseOnUnfocus.get();
}
void setPauseOnUnfocus(bool enable) {
    pauseOnUnfocus.base_value = enable;
}

bool vkValidationCoreEnabled() {
    return vkValidationCore.get();
}

bool allowHDR() {
    return isHDRAllowed.get();
}

int getExtraDmemInMbytes() {
    return extraDmemInMbytes.get();
}

void setExtraDmemInMbytes(int value) {
    extraDmemInMbytes.base_value = value;
}

bool getLoadAutoPatches() {
    return load_auto_patches;
}
void setLoadAutoPatches(bool enable) {
    load_auto_patches = enable;
}

int getUsbDeviceBackend() {
    return usbDeviceBackend.get();
}

void setUsbDeviceBackend(int value) {
    usbDeviceBackend.base_value = value;
}

bool getFirstBootHandled() {
    return firstBootHandled.get();
}

void setFirstBootHandled(bool handled) {
    firstBootHandled.base_value = handled;
}

std::string getMainOutputDevice() {
    return mainOutputDevice.get();
}

std::string getPadSpkOutputDevice() {
    return padSpkOutputDevice.get();
}

void setMainOutputDevice(std::string device) {
    mainOutputDevice.base_value = device;
}

void setPadSpkOutputDevice(std::string device) {
    padSpkOutputDevice.base_value = device;
}

std::string getCustomBackgroundImage() {
    return g_customBackgroundImage;
}

void setCustomBackgroundImage(const std::string& path) {
    g_customBackgroundImage = path;
}

std::string getGuiStyle() {
    return guiStyle;
}

void setGuiStyle(const std::string& style) {
    guiStyle = style;
}

void setVersionPath(const std::string& path) {
    version_path = path;
}

std::string getVersionPath() {
    return version_path;
}

bool getBootLauncher() {
    return launcher_boot;
}

void setBootLauncher(bool enabled) {
    launcher_boot = enabled;
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

std::filesystem::path getSysModulesPath() {
    if (sys_modules_path.empty()) {
        return Common::FS::GetUserPath(Common::FS::PathType::SysModuleDir);
    }
    return sys_modules_path;
}

void setSysModulesPath(const std::filesystem::path& path) {
    sys_modules_path = path;
}

u32 getFpsLimit() {
    return fpsLimit.get();
}

void setFpsLimit(u32 fpsValue) {
    fpsLimit.base_value = fpsValue;
}

bool isFpsLimiterEnabled() {
    return fpsLimiterEnabled.get();
}

void setFpsLimiterEnabled(bool enabled) {
    fpsLimiterEnabled.base_value = enabled;
}

bool GamesMenuUI() {
    return bootGamesMenu;
}

void setGamesMenuUI(bool enable) {
    bootGamesMenu = enable;
}

bool HubMenuUI() {
    return bootHubMenu;
}

void setHubMenuUI(bool enable) {
    bootHubMenu = enable;
}

bool getRestartWithBaseGame() {
    return restartWithBaseGame;
}
void setRestartWithBaseGame(bool enable) {
    restartWithBaseGame = enable;
}

bool getSeparateUpdateEnabled() {
    return separateupdatefolder;
}

void setSeparateUpdateEnabled(bool use) {
    separateupdatefolder = use;
}

std::string getTrophyKey() {
    return trophyKey;
}

void setTrophyKey(std::string key) {
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

void setShowLabelsUnderIcons(bool enable) {
    showLabelsUnderIcons = enable;
}

bool getEnableColorFilter() {
    return enableColorFilter;
}

void setEnableColorFilter(bool enable) {
    enableColorFilter = enable;
}

string getFullscreenMode() {
    return fullscreenMode.get();
}

std::string getPresentMode() {
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

std::string getMicDevice() {
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
    return internalScreenWidth.get();
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

int getRcasAttenuation() {
    return rcasAttenuation.get();
}

void setRcasAttenuation(int value) {
    rcasAttenuation.base_value = value;
}

std::string getLogFilter() {
    return logFilter.get();
}

std::string getLogType() {
    return logType.get();
}

string getUserName(int id) {
    return userNames.get()[id];
}

void setUserName(int id, string name) {
    auto temp = userNames.get();
    temp[id] = name;
    userNames.set(temp);
}

std::array<string, 4> const getUserNames() {
    return userNames.get();
}

std::string getUpdateChannel() {
    return updateChannel;
}

std::string getChooseHomeTab() {
    return chooseHomeTab;
}

int getVolumeSlider() {
    return volumeSlider.get();
}

void setVolumeSlider(int volumeValue, bool is_game_specific) {
    volumeSlider.set(volumeValue, is_game_specific);
}

bool isMuteEnabled() {
    return muteEnabled.get();
}

void setMuteEnabled(bool enabled) {
    muteEnabled.base_value = enabled;
}

bool getUseSpecialPad(int pad) {
    if (pad < 1 || pad > 4)
        return false;
    return useSpecialPads[pad - 1].get();
}

int getSpecialPadClass(int pad) {
    if (pad < 1 || pad > 4)
        return 1;
    return specialPadClasses[pad - 1].get();
}

void setUseSpecialPad(int pad, bool use, bool is_game_specific) {
    if (pad < 1 || pad > 4)
        return;
    useSpecialPads[pad - 1].set(use, is_game_specific);
}

void setSpecialPadClass(int pad, int type, bool is_game_specific) {
    if (pad < 1 || pad > 4)
        return;
    specialPadClasses[pad - 1].set(type, is_game_specific);
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

std::string sideTrophy() {
    return isSideTrophy.get();
}

bool DisableHardcodedHotkeys() {
    return isDisableHardcodedHotkeys.get();
}

void setDisableHardcodedHotkeys(bool disable) {
    isDisableHardcodedHotkeys.base_value = disable;
}

bool UseHomeButtonForHotkeys() {
    return homeButtonHotkey.get();
}

void setUseHomeButtonForHotkeys(bool disable) {
    homeButtonHotkey.base_value = disable;
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
    readbackSpeedMode.game_specific_value = mode;
}

bool setReadbackLinearImages(bool enable) {
    return readbackLinearImagesEnabled.base_value = enable;
}

bool getReadbackLinearImages() {
    return readbackLinearImagesEnabled.get();
}

bool setScreenTipDisable(bool enable) {
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

bool getShowFpsCounter() {
    return showFpsCounter.get();
}

void setShowFpsCounter(bool enable, bool is_game_specific) {
    showFpsCounter.set(enable, is_game_specific);
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

void SetSkippedShaderHashes(const std::string& game_id) {
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

void setSideTrophy(std::string side) {
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

void setFullscreenMode(string mode) {
    fullscreenMode.base_value = mode;
}

void setPresentMode(std::string mode) {
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

void setMicDevice(std::string device) {
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

void setLogType(const std::string& type) {
    logType.game_specific_value.reset();
    logType.base_value = type;
}

void setLogFilter(const std::string& type) {
    logFilter.base_value = type;
}

void setSeparateLogFilesEnabled(bool enabled) {
    isSeparateLogFilesEnabled.base_value = enabled;
}
void setUserName(int id, const std::string& name) {
    auto arr = userNames.get();
    arr[id] = name;
    userNames.set(arr);
}
void setUpdateChannel(const std::string& type) {
    updateChannel = type;
}
void setChooseHomeTab(const std::string& type) {
    chooseHomeTab = type;
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

bool addGameDirectories(const std::filesystem::path& dir, bool enabled) {
    for (const auto& directories : settings_directories) {
        if (directories.path == dir) {
            return false;
        }
    }
    settings_directories.push_back({dir, enabled});
    return true;
}

void removeGameDirectories(const std::filesystem::path& dir) {
    auto iterator = std::find_if(
        settings_directories.begin(), settings_directories.end(),
        [&dir](const GameDirectories& directories) { return directories.path == dir; });
    if (iterator != settings_directories.end()) {
        settings_directories.erase(iterator);
    }
}

void setGameDirectoriesEnabled(const std::filesystem::path& dir, bool enabled) {
    auto iterator = std::find_if(
        settings_directories.begin(), settings_directories.end(),
        [&dir](const GameDirectories& directories) { return directories.path == dir; });
    if (iterator != settings_directories.end()) {
        iterator->enabled = enabled;
    }
}

void setAddonDirectories(const std::filesystem::path& dir) {
    settings_addon_directories = dir;
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

void setGameDirectories(const std::vector<std::filesystem::path>& dirs_config) {
    settings_directories.clear();
    for (const auto& dir : dirs_config) {
        settings_directories.push_back({dir, true});
    }
}

void setAllGameDirectories(const std::vector<GameDirectories>& dirs_config) {
    settings_directories = dirs_config;
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

const std::vector<std::filesystem::path> getGameDirectories() {
    std::vector<std::filesystem::path> enabled_dirs;
    for (const auto& dir : settings_directories) {
        if (dir.enabled) {
            enabled_dirs.push_back(dir.path);
        }
    }
    return enabled_dirs;
}

const std::vector<bool> getGameDirectoriesEnabled() {
    std::vector<bool> enabled_dirs;
    for (const auto& dir : settings_directories) {
        enabled_dirs.push_back(dir.enabled);
    }
    return enabled_dirs;
}

std::filesystem::path getAddonDirectory() {
    if (settings_addon_directories.empty()) {
        // Default for users without a config file or a config file from before this option existed
        return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "addcont";
    }
    return settings_addon_directories;
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

std::string getEmulatorLanguage() {
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

std::string getDefaultControllerID() {
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
        bootGamesMenu = toml::find_or<bool>(general, "GamesMenuUI", false);
        bootHubMenu = toml::find_or<bool>(general, "HubMenuUI", false);
        restartWithBaseGame = toml::find_or<bool>(general, "restartWithBaseGame", false);
        separateupdatefolder = toml::find_or<bool>(general, "separateUpdateEnabled", false);
        screenTipDisable.setFromToml(general, "screenTipDisable", is_game_specific);
        volumeSlider.setFromToml(general, "volumeSlider", is_game_specific);
        muteEnabled.setFromToml(general, "muteEnabled", is_game_specific);
        extraDmemInMbytes.setFromToml(general, "extraDmemInMbytes", is_game_specific);
        isNeo.setFromToml(general, "isPS4Pro", is_game_specific);
        isDevKit.setFromToml(general, "isDevKit", is_game_specific);
        isPSNSignedIn.setFromToml(general, "isPSNSignedIn", is_game_specific);
        playBGM = toml::find_or<bool>(general, "playBGM", false);
        isTrophyPopupDisabled.setFromToml(general, "isTrophyPopupDisabled", is_game_specific);
        trophyNotificationDuration.setFromToml(general, "trophyNotificationDuration",
                                               is_game_specific);
        BGMvolume = toml::find_or<int>(general, "BGMvolume", 50);
        enableDiscordRPC = toml::find_or<bool>(general, "enableDiscordRPC", true);
        logFilter.setFromToml(general, "logFilter", is_game_specific);
        logType.setFromToml(general, "logType", is_game_specific);
        userNames.setFromToml(general, "userNames", is_game_specific);

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
        pauseOnUnfocus.setFromToml(general, "pauseOnUnfocus", is_game_specific);
        showWelcomeDialog = toml::find_or<bool>(general, "showWelcomeDialog", true);

        isAlwaysShowChangelog = toml::find_or<bool>(general, "alwaysShowChangelog", false);
        isDisableHardcodedHotkeys.setFromToml(general, "DisableHardcodedHotkeys", is_game_specific);
        homeButtonHotkey.setFromToml(general, "UseHomeButtonForHotkeys", is_game_specific);
        isSideTrophy.setFromToml(general, "sideTrophy", is_game_specific);
        compatibilityData = toml::find_or<bool>(general, "compatibilityEnabled", false);
        checkCompatibilityOnStartup =
            toml::find_or<bool>(general, "checkCompatibilityOnStartup", false);
        isConnectedToNetwork.setFromToml(general, "isConnectedToNetwork", is_game_specific);
        firstBootHandled.setFromToml(general, "firstBootHandled", is_game_specific);
        chooseHomeTab = toml::find_or<std::string>(general, "chooseHomeTab", chooseHomeTab);
        defaultControllerID.setFromToml(general, "defaultControllerID", "");
        sys_modules_path = toml::find_fs_path_or(general, "sysModulesPath", sys_modules_path);
    }

    if (data.contains("Input")) {
        const toml::value& input = data.at("Input");

        cursorState.setFromToml(input, "cursorState", is_game_specific);
        cursorHideTimeout.setFromToml(input, "cursorHideTimeout", is_game_specific);
        cursorHideTimeout.setFromToml(input, "cursorHideTimeout", is_game_specific);
        for (int p = 1; p <= 4; p++) {
            std::string useKey = "useSpecialPad" + std::to_string(p);
            std::string classKey = "specialPadClass" + std::to_string(p);

            useSpecialPads[p - 1].setFromToml(input, useKey, false);
            specialPadClasses[p - 1].setFromToml(input, classKey, false);
        }
        isMotionControlsEnabled.setFromToml(input, "isMotionControlsEnabled", is_game_specific);
        useUnifiedInputConfig.setFromToml(input, "useUnifiedInputConfig", is_game_specific);
        backgroundControllerInput.setFromToml(input, "backgroundControllerInput", is_game_specific);
        usbDeviceBackend.setFromToml(input, "usbDeviceBackend", is_game_specific);
    }

    if (data.contains("Audio")) {
        const toml::value& audio = data.at("Audio");

        micDevice.setFromToml(audio, "micDevice", is_game_specific);
        mainOutputDevice.setFromToml(audio, "mainOutputDevice", is_game_specific);
        padSpkOutputDevice.setFromToml(audio, "padSpkOutputDevice", is_game_specific);
    }

    if (data.contains("GPU")) {
        const toml::value& gpu = data.at("GPU");

        fsrEnabled.setFromToml(gpu, "fsrEnabled", is_game_specific);
        rcasEnabled.setFromToml(gpu, "rcasEnabled", is_game_specific);
        if (is_game_specific) {
            if (auto opt = toml::get_optional<int>(gpu, "readbackSpeedMode")) {
                readbackSpeedMode.game_specific_value = static_cast<ReadbackSpeed>(*opt);
            }
        } else {
            if (auto opt = toml::get_optional<int>(gpu, "readbackSpeedMode")) {
                readbackSpeedMode.base_value = static_cast<ReadbackSpeed>(*opt);
            }
        }
        if (is_game_specific) {
            if (auto opt = toml::get_optional<double>(gpu, "rcasAttenuation")) {
                rcasAttenuation.game_specific_value = static_cast<float>(*opt);
            }
        } else {
            if (auto opt = toml::get_optional<double>(gpu, "rcasAttenuation")) {
                rcasAttenuation.base_value = static_cast<float>(*opt);
            }
        }

        isNullGpu.setFromToml(gpu, "nullGpu", false);
        shouldDumpShaders.setFromToml(gpu, "dumpShaders", is_game_specific);
        shouldPatchShaders.setFromToml(gpu, "patchShaders", is_game_specific);
        vblankFrequency.setFromToml(gpu, "vblankFrequency", is_game_specific);
        isHDRAllowed.setFromToml(gpu, "allowHDR", is_game_specific);
        shaderSkipsEnabled.setFromToml(gpu, "shaderSkipsEnabled", is_game_specific);
        fpsLimit.setFromToml(gpu, "fpsLimit", is_game_specific);
        fpsLimiterEnabled.setFromToml(gpu, "fpsLimiterEnabled", is_game_specific);
        windowWidth.setFromToml(gpu, "screenWidth", is_game_specific);
        windowHeight.setFromToml(gpu, "screenHeight", is_game_specific);
        internalScreenWidth.setFromToml(gpu, "internalScreenWidth", is_game_specific);
        internalScreenHeight.setFromToml(gpu, "internalScreenHeight", is_game_specific);
        isNullGpu.setFromToml(gpu, "nullGpu", is_game_specific);
        shouldCopyGPUBuffers.setFromToml(gpu, "copyGPUBuffers", is_game_specific);
        readbackLinearImagesEnabled.setFromToml(gpu, "readbackLinearImages", is_game_specific);
        directMemoryAccessEnabled.setFromToml(gpu, "directMemoryAccess", is_game_specific);
        isFullscreen.setFromToml(gpu, "Fullscreen", is_game_specific);
        fullscreenMode.setFromToml(gpu, "FullscreenMode", is_game_specific);
        if (is_game_specific) {
            if (auto opt = toml::get_optional<std::string>(gpu, "presentMode")) {
                presentMode.game_specific_value = *opt;
            }
        } else {
            if (auto opt = toml::get_optional<std::string>(gpu, "presentMode")) {
                presentMode.base_value = *opt;
            }
        }
    }

    if (data.contains("Vulkan")) {
        const toml::value& vk = data.at("Vulkan");

        gpuId.setFromToml(vk, "gpuId", is_game_specific);
        vkValidationCore.setFromToml(vk, "validation_core", is_game_specific);
        vkValidation.setFromToml(vk, "validation", is_game_specific);
        vkValidationSync.setFromToml(vk, "validation_sync", is_game_specific);
        vkValidationGpu.setFromToml(vk, "validation_gpu", is_game_specific);
        vkCrashDiagnostic.setFromToml(vk, "crashDiagnostic", is_game_specific);
        vkHostMarkers.setFromToml(vk, "hostMarkers", is_game_specific);
        vkGuestMarkers.setFromToml(vk, "guestMarkers", is_game_specific);
        rdocEnable.setFromToml(vk, "rdocEnable", is_game_specific);
        pipelineCacheEnable.setFromToml(vk, "pipelineCacheEnable", is_game_specific);
        pipelineCacheArchive.setFromToml(vk, "pipelineCacheArchive", is_game_specific);
    }
    string current_version = {};

    if (data.contains("Debug")) {
        const toml::value& debug = data.at("Debug");

        isDebugDump.setFromToml(debug, "DebugDump", is_game_specific);
        isSeparateLogFilesEnabled =
            toml::find_or<bool>(debug, "isSeparateLogFilesEnabled", is_game_specific);
        isShaderDebug.setFromToml(debug, "CollectShader", is_game_specific);
        isFpsColor.setFromToml(debug, "FPSColor", is_game_specific);
        showFpsCounter.setFromToml(debug, "showFpsCounter", is_game_specific);
        logEnabled.setFromToml(debug, "logEnabled", is_game_specific);
        current_version = toml::find_or<std::string>(debug, "ConfigVersion", current_version);
    }

    if (data.contains("GUI")) {
        const toml::value& gui = data.at("GUI");

        g_customBackgroundImage = toml::find_or<std::string>(gui, "CustomBackgroundImage", "");
        load_game_size = toml::find_or<bool>(gui, "loadGameSizeEnabled", true);

        m_icon_size = toml::find_or<int>(gui, "iconSize", 36);
        m_icon_size_grid = toml::find_or<int>(gui, "iconSizeGrid", 69);
        m_slider_pos = toml::find_or<int>(gui, "sliderPos", 0);
        m_slider_pos_grid = toml::find_or<int>(gui, "sliderPosGrid", 0);

        mw_themes = toml::find_or<int>(gui, "theme", 0);
        guiStyle = toml::find_or<std::string>(gui, "guiStyle", "");
        version_path = toml::find_or<std::string>(gui, "version_path", "");

        m_window_size_W = toml::find_or<int>(gui, "mw_width", 1280);
        m_window_size_H = toml::find_or<int>(gui, "mw_height", 720);
        main_window_geometry_x = toml::find_or<int>(gui, "geometry_x", 0);
        main_window_geometry_y = toml::find_or<int>(gui, "geometry_y", 0);
        main_window_geometry_w = toml::find_or<int>(gui, "geometry_w", 1280);
        main_window_geometry_h = toml::find_or<int>(gui, "geometry_h", 720);

        m_table_mode = toml::find_or<int>(gui, "gameTableMode", 0);
        emulator_language = toml::find_or<std::string>(gui, "emulatorLanguage", "en_US");

        backgroundImageOpacity = toml::find_or<int>(gui, "backgroundImageOpacity", 50);
        showBackgroundImage = toml::find_or<bool>(gui, "showBackgroundImage", true);
        showLabelsUnderIcons = toml::find_or<bool>(gui, "showLabelsUnderIcons", true);
        enableColorFilter = toml::find_or<bool>(gui, "enableColorFilter", true);
        launcher_boot = toml::find_or<bool>(gui, "launcher_boot", false);
        isQT = toml::find_or<bool>(gui, "isQT", false);
        isSDL = toml::find_or<bool>(gui, "isSDL", false);

        toolbar_visibility_settings.clear();
        if (gui.contains("ToolbarVisibility") && gui.at("ToolbarVisibility").is_table()) {
            for (const auto& [key, value] : gui.at("ToolbarVisibility").as_table()) {
                if (value.is_boolean()) {
                    toolbar_visibility_settings[key] = toml::get<bool>(value);
                }
            }
        }

        const auto directories_array =
            toml::find_or<std::vector<std::u8string>>(gui, "Directories", {});
        try {
            directories_enabled = toml::find<std::vector<bool>>(gui, "DirectoriesEnabled");
        } catch (...) {
            directories_enabled.resize(directories_array.size(), true);
        }
        if (directories_enabled.size() < directories_array.size()) {
            directories_enabled.resize(directories_array.size(), true);
        }
        settings_directories.clear();
        for (size_t i = 0; i < directories_array.size(); i++) {
            settings_directories.push_back(
                {std::filesystem::path{directories_array[i]}, directories_enabled[i]});
        }

        settings_addon_directories =
            toml::find_fs_path_or(gui, "addonDirectories", std::filesystem::path{});
        save_data_path = toml::find_fs_path_or(gui, "saveDataPath", save_data_path);

        m_elf_viewer = toml::find_or<std::vector<std::string>>(gui, "elfDirs", {});
        m_recent_files = toml::find_or<std::vector<std::string>>(gui, "recentFiles", {});
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

    const std::vector<std::string> allowed_languages = {
        "ar_SA", "da_DK", "de_DE", "el_GR", "en_US", "es_ES", "fa_IR", "fi_FI", "fr_FR", "hu_HU",
        "id_ID", "it_IT", "ja_JP", "ko_KR", "lt_LT", "nb_NO", "nl_NL", "pl_PL", "pt_BR", "pt_PT",
        "ro_RO", "ru_RU", "sq_AL", "sv_SE", "tr_TR", "uk_UA", "vi_VN", "zh_CN", "zh_TW"};

    if (std::find(allowed_languages.begin(), allowed_languages.end(), emulator_language) ==
        allowed_languages.end()) {
        emulator_language = "en_US";
        save(path);
    }
}

void sortTomlSections(toml::ordered_value& data) {
    toml::ordered_value ordered_data;
    std::vector<std::string> section_order = {"General", "Input", "Audio", "GPU",     "Vulkan",
                                              "Debug",   "Keys",  "GUI",   "Settings"};
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
    data["General"]["extraDmemInMbytes"] = extraDmemInMbytes.base_value;
    data["General"]["isPSNSignedIn"] = isPSNSignedIn.base_value;
    data["General"]["isTrophyPopupDisabled"] = isTrophyPopupDisabled.base_value;
    data["General"]["trophyNotificationDuration"] = trophyNotificationDuration.base_value;
    data["General"]["playBGM"] = playBGM;
    data["General"]["BGMvolume"] = BGMvolume;
    data["General"]["enableDiscordRPC"] = enableDiscordRPC;
    data["General"]["logFilter"] = logFilter.base_value;
    data["General"]["logType"] = logType.base_value;
    data["General"]["userNames"] = userNames.base_value;
    data["General"]["updateChannel"] = updateChannel;
    data["General"]["chooseHomeTab"] = chooseHomeTab;
    data["General"]["showSplash"] = isShowSplash.base_value;
    data["General"]["autoUpdate"] = isAutoUpdate;
    data["General"]["pauseOnUnfocus"] = pauseOnUnfocus.base_value;
    data["General"]["showWelcomeDialog"] = showWelcomeDialog;
    data["General"]["alwaysShowChangelog"] = isAlwaysShowChangelog;
    data["General"]["DisableHardcodedHotkeys"] = isDisableHardcodedHotkeys.base_value;
    data["General"]["UseHomeButtonForHotkeys"] = homeButtonHotkey.base_value;
    data["General"]["enableAutoBackup"] = enableAutoBackup.base_value;
    data["General"]["GamesMenuUI"] = bootGamesMenu;
    data["General"]["HubMenuUI"] = bootHubMenu;
    data["General"]["restartWithBaseGame"] = restartWithBaseGame;
    data["General"]["separateUpdateEnabled"] = separateupdatefolder;
    data["General"]["screenTipDisable"] = screenTipDisable.base_value;
    data["General"]["sideTrophy"] = isSideTrophy.base_value;
    data["General"]["compatibilityEnabled"] = compatibilityData;
    data["General"]["checkCompatibilityOnStartup"] = checkCompatibilityOnStartup;
    data["General"]["sysModulesPath"] = string{fmt::UTF(sys_modules_path.u8string()).data};
    data["General"]["isConnectedToNetwork"] = isConnectedToNetwork.base_value;
    data["General"]["firstBootHandled"] = firstBootHandled.base_value;
    data["General"]["defaultControllerID"] = defaultControllerID.base_value;

    data["Input"]["cursorState"] = cursorState.base_value;
    data["Input"]["cursorHideTimeout"] = cursorHideTimeout.base_value;
    for (int p = 1; p <= 4; p++) {
        std::string useKey = "useSpecialPad" + std::to_string(p);
        std::string classKey = "specialPadClass" + std::to_string(p);

        useSpecialPads[p - 1].setTomlValue(data, "Input", useKey, false);
        specialPadClasses[p - 1].setTomlValue(data, "Input", classKey, false);
    }
    data["Input"]["isMotionControlsEnabled"] = isMotionControlsEnabled.base_value;
    data["Input"]["useUnifiedInputConfig"] = useUnifiedInputConfig.base_value;
    data["Input"]["backgroundControllerInput"] = backgroundControllerInput.base_value;
    data["Input"]["usbDeviceBackend"] = usbDeviceBackend.base_value;

    data["Audio"]["micDevice"] = micDevice.base_value;
    data["Audio"]["mainOutputDevice"] = mainOutputDevice.base_value;
    data["Audio"]["padSpkOutputDevice"] = padSpkOutputDevice.base_value;

    data["GPU"]["rcasAttenuation"] = rcasAttenuation.base_value;
    data["GPU"]["fsrEnabled"] = fsrEnabled.base_value;
    data["GPU"]["rcasEnabled"] = rcasEnabled.base_value;
    data["GPU"]["fpsLimit"] = fpsLimit.base_value;
    data["GPU"]["fpsLimiterEnabled"] = fpsLimiterEnabled.base_value;
    data["GPU"]["screenWidth"] = windowWidth.base_value;
    data["GPU"]["screenHeight"] = windowHeight.base_value;
    data["GPU"]["internalScreenWidth"] = internalScreenWidth.base_value;
    data["GPU"]["internalScreenHeight"] = internalScreenHeight.base_value;
    data["GPU"]["nullGpu"] = isNullGpu.base_value;
    data["GPU"]["copyGPUBuffers"] = shouldCopyGPUBuffers.base_value;
    data["GPU"]["readbackSpeedMode"] = static_cast<int>(readbackSpeedMode.base_value);
    data["GPU"]["readbackLinearImages"] = readbackLinearImagesEnabled.base_value;
    data["GPU"]["directMemoryAccess"] = directMemoryAccessEnabled.base_value;
    data["GPU"]["dumpShaders"] = shouldDumpShaders.base_value;
    data["GPU"]["patchShaders"] = shouldPatchShaders.base_value;
    data["GPU"]["vblankFrequency"] = vblankFrequency.base_value;
    data["GPU"]["Fullscreen"] = isFullscreen.base_value;
    data["GPU"]["FullscreenMode"] = fullscreenMode.base_value;
    data["GPU"]["presentMode"] = presentMode.base_value;
    data["GPU"]["allowHDR"] = isHDRAllowed.base_value;
    data["GPU"]["shaderSkipsEnabled"] = shaderSkipsEnabled.base_value;
    data["Vulkan"]["gpuId"] = gpuId.base_value;
    data["Vulkan"]["validation"] = vkValidation.base_value;
    data["Vulkan"]["validation_core"] = vkValidationCore.base_value;
    data["Vulkan"]["validation_sync"] = vkValidationSync.base_value;
    data["Vulkan"]["validation_gpu"] = vkValidationGpu.base_value;
    data["Vulkan"]["crashDiagnostic"] = vkCrashDiagnostic.base_value;
    data["Vulkan"]["hostMarkers"] = vkHostMarkers.base_value;
    data["Vulkan"]["guestMarkers"] = vkGuestMarkers.base_value;
    data["Vulkan"]["rdocEnable"] = rdocEnable.base_value;
    data["Vulkan"]["pipelineCacheEnable"] = pipelineCacheEnable.base_value;
    data["Vulkan"]["pipelineCacheArchive"] = pipelineCacheArchive.base_value;

    data["Debug"]["DebugDump"] = isDebugDump.base_value;
    data["Debug"]["CollectShader"] = isShaderDebug.base_value;
    data["Debug"]["isSeparateLogFilesEnabled"] = isSeparateLogFilesEnabled.base_value;
    data["Debug"]["FPSColor"] = isFpsColor.base_value;
    data["Debug"]["showFpsCounter"] = showFpsCounter.base_value;
    data["Debug"]["logEnabled"] = logEnabled.base_value;
    data["Keys"]["TrophyKey"] = trophyKey;

    std::vector<std::string> directories;
    std::vector<bool> directories_enabled;

    struct DirEntry {
        std::string path_str;
        bool enabled;
    };

    std::vector<DirEntry> sorted_dirs;
    for (const auto& dirInfo : settings_directories) {
        sorted_dirs.push_back(
            {std::string{fmt::UTF(dirInfo.path.u8string()).data}, dirInfo.enabled});
    }

    std::sort(sorted_dirs.begin(), sorted_dirs.end(), [](const DirEntry& a, const DirEntry& b) {
        return std::lexicographical_compare(
            a.path_str.begin(), a.path_str.end(), b.path_str.begin(), b.path_str.end(),
            [](char a_char, char b_char) { return std::tolower(a_char) < std::tolower(b_char); });
    });

    for (const auto& entry : sorted_dirs) {
        directories.push_back(entry.path_str);
        directories_enabled.push_back(entry.enabled);
    }

    data["GUI"]["Directories"] = directories;
    data["GUI"]["DirectoriesEnabled"] = directories_enabled;
    data["GUI"]["installDirs"] = std::vector<std::string>(directories);
    data["GUI"]["installDirsEnabled"] = std::vector<bool>(directories_enabled);
    std::string addon_dir_str = std::string{fmt::UTF(settings_addon_directories.u8string()).data};
    data["GUI"]["addonDirectories"] = addon_dir_str;
    data["GUI"]["addonInstallDir"] = std::string(addon_dir_str);
    data["GUI"]["saveDataPath"] = std::string{fmt::UTF(save_data_path.u8string()).data};
    data["GUI"]["loadGameSizeEnabled"] = load_game_size;
    data["GUI"]["CustomBackgroundImage"] = g_customBackgroundImage;
    data["GUI"]["showLabelsUnderIcons"] = showLabelsUnderIcons;
    data["GUI"]["enableColorFilter"] = enableColorFilter;
    data["GUI"]["backgroundImageOpacity"] = backgroundImageOpacity;
    data["GUI"]["showBackgroundImage"] = showBackgroundImage;
    data["GUI"]["emulatorLanguage"] = emulator_language;
    data["GUI"]["isQT"] = isQT;
    data["GUI"]["isSDL"] = isSDL;
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
    data["GUI"]["showLabelsUnderIcons"] = showLabelsUnderIcons;
    data["GUI"]["enableColorFilter"] = enableColorFilter;
    data["GUI"]["launcher_boot"] = launcher_boot;
    data["GUI"]["guiStyle"] = guiStyle;
    data["GUI"]["version_path"] = version_path;
    toml::value toolbar_map = toml::table{};
    for (const auto& [name, visible] : toolbar_visibility_settings) {
        toolbar_map[name] = visible;
    }
    data["GUI"]["ToolbarVisibility"] = toolbar_map;

    sortTomlSections(data);

    std::ofstream file(path, std::ios::binary);
    file << data;
    file.close();
}

void setDefaultValues() {
    // General
    isNeo = false;
    isDevKit = false;
    showWelcomeDialog = true;
    extraDmemInMbytes = 0;
    isPSNSignedIn = false;
    isTrophyPopupDisabled = false;
    trophyNotificationDuration = 6.0;
    enableDiscordRPC = false;
    playBGM = false;
    BGMvolume = 50;
    enableDiscordRPC = true;
    isDisableHardcodedHotkeys = false;
    homeButtonHotkey = false;
    logFilter = "";
    logType = "sync";
    userNames = {"shadPS4", "shadPS4-2", "shadPS4-3", "shadPS4-4"};
    chooseHomeTab = "General";
    isShowSplash = false;
    isSideTrophy = "right";
    compatibilityData = false;
    checkCompatibilityOnStartup = false;
    isConnectedToNetwork = false;
    firstBootHandled = false;
    bootGamesMenu = false;
    bootHubMenu = false;
    restartWithBaseGame = false;
    separateupdatefolder = false;
    screenTipDisable = false;

    // Input
    cursorState = HideCursorState::Idle;
    cursorHideTimeout = 5;
    isMotionControlsEnabled = true;
    useUnifiedInputConfig = true;
    overrideControllerColor = false;
    controllerCustomColorRGB[0] = 0;
    controllerCustomColorRGB[1] = 0;
    controllerCustomColorRGB[2] = 255;
    micDevice = "Default Device";
    backgroundControllerInput = false;
    mainOutputDevice = "Default Device";
    padSpkOutputDevice = "Default Device";

    // GPU
    isDebugDump = false;
    isShaderDebug = false;
    isShowSplash = false;
    isAutoUpdate = false;
    pauseOnUnfocus = false;
    isAlwaysShowChangelog = false;
    windowWidth = 1280;
    windowHeight = 720;
    internalScreenWidth = 1280;
    internalScreenHeight = 720;
    isNullGpu = false;
    shouldCopyGPUBuffers = false;
    readbackSpeedMode = ReadbackSpeed::Disable;
    shaderSkipsEnabled = false;
    readbackLinearImagesEnabled = false;
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
    rcasAttenuation = 250;
    fpsLimit = 60;
    fpsLimiterEnabled = false;

    // Vulkan
    gpuId = -1;
    vkValidation = false;
    vkValidationSync = false;
    vkValidationGpu = false;
    vkCrashDiagnostic = false;
    vkHostMarkers = false;
    vkGuestMarkers = false;
    rdocEnable = false;
    pipelineCacheEnable = false;
    pipelineCacheArchive = true;

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
    isQT = false;
    isSDL = false;

    // Settings
    emulator_language = "en_US";
    m_language = 1;
    gpuId = -1;
    compatibilityData = false;
    checkCompatibilityOnStartup = false;
    backgroundImageOpacity = 50;
    showBackgroundImage = true;
    showLabelsUnderIcons = true;
    enableColorFilter = true;
    launcher_boot = false;
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
hotkey_toggle_mouse_to_touchpad = delete
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
            // std::ofstream global_config_stream(config_file);
            // if (global_config_stream) {
            //     global_config_stream << global_config;
            //  }
        }
    }

    // If game-specific config doesn't exist, create it from the default config
    if (!std::filesystem::exists(config_file)) {
        std::filesystem::copy(default_config_file, config_file);
    }
    return config_file;
}

} // namespace Config