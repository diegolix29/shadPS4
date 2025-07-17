// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <string>
#include <fmt/core.h>
#include <fmt/xchar.h> // for wstring support
#include <toml.hpp>

#include "common/config.h"
#include "common/logging/formatter.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scm_rev.h"

namespace toml {
template <typename TC, typename K>
std::filesystem::path find_fs_path_or(const basic_value<TC>& v, const K& ky,
                                      std::filesystem::path opt) {
    try {
        auto str = find<std::string>(v, ky);
        if (str.empty()) {
            return opt;
        }
        std::u8string u8str{(char8_t*)&str.front(), (char8_t*)&str.back() + 1};
        return std::filesystem::path{u8str};
    } catch (...) {
        return opt;
    }
}
} // namespace toml

namespace Config {

// General
static bool isNeo = false;
static bool isDevKit = false;
static bool isPSNSignedIn = false;
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

static bool playBGM = false;
static bool isTrophyPopupDisabled = false;
static double trophyNotificationDuration = 6.0;
static int BGMvolume = 50;
static bool enableDiscordRPC = false;
static float rcas_attenuation = 0.25f;
static float gameVolume = 2.0f;
static std::string logFilter = "*:Warning";
static std::string logType = "sync";
static std::string userName = "shadPS4";
static std::string chooseHomeTab = "General";
static bool compatibilityData = false;
static bool checkCompatibilityOnStartup = false;

// Input
static int cursorState = HideCursorState::Idle;
static int cursorHideTimeout = 5; // 5 seconds (default)
static bool useSpecialPad = false;
static int specialPadClass = 1;
static bool isMotionControlsEnabled = true;
static bool useUnifiedInputConfig = true;

// These two entries aren't stored in the config
static bool overrideControllerColor = false;
static int controllerCustomColorRGB[3] = {0, 0, 255};

// GPU
static u32 screenWidth = 1280;
static u32 screenHeight = 720;
static bool isShowSplash = false;
static bool isAutoUpdate = false;
static bool isAlwaysShowChangelog = false;
static std::string isSideTrophy = "right";
static u32 windowWidth = 1280;
static u32 windowHeight = 720;
static u32 internalScreenWidth = 1280;
static u32 internalScreenHeight = 720;
static bool isNullGpu = false;
static bool shouldCopyGPUBuffers = false;
static bool readbackLinearImagesEnabled = false;
static bool directMemoryAccessEnabled = false;
static bool shouldDumpShaders = false;
static bool shouldPatchShaders = false;
static u32 vblankDivider = 1;
static bool fpsColorState = false;

// Vulkan
static s32 gpuId = -1;
static bool vkValidation = false;
static bool vkValidationSync = false;
static bool vkValidationGpu = false;
static bool vkCrashDiagnostic = false;
static bool vkHostMarkers = false;
static bool vkGuestMarkers = false;
static bool rdocEnable = false;

// Debug
static bool isDebugDump = false;
static bool isShaderDebug = false;
static bool isSeparateLogFilesEnabled = false;
static bool readbacksEnabled = false;
static bool fastreadbacksEnabled = false;
static bool shaderSkipsEnabled = false;
static std::string memoryAlloc = "medium";
static std::string audioBackend = "cubeb";
static int audioVolume = 100;

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
static bool isFullscreen = false;
static std::string fullscreenMode = "Windowed";
static bool isHDRAllowed = false;
static bool enableAutoBackup = false;
static bool showLabelsUnderIcons = true;
static std::string updateChannel;

// Settings
u32 m_language = 1; // english

// Keys
static std::string trophyKey = "";

// Expected number of items in the config file
static constexpr u64 total_entries = 54;

bool allowHDR() {
    return isHDRAllowed;
}

bool getEnableAutoBackup() {
    return enableAutoBackup;
}

bool GetUseUnifiedInputConfig() {
    return useUnifiedInputConfig;
}

void SetUseUnifiedInputConfig(bool use) {
    useUnifiedInputConfig = use;
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

void SetControllerCustomColor(int r, int b, int g) {
    controllerCustomColorRGB[0] = r;
    controllerCustomColorRGB[1] = b;
    controllerCustomColorRGB[2] = g;
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
    return isNeo;
}

bool isDevKitConsole() {
    return isDevKit;
}

bool getIsFullscreen() {
    return isFullscreen;
}

bool getShowLabelsUnderIcons() {
    return showLabelsUnderIcons;
}

bool setShowLabelsUnderIcons() {
    return false;
}

std::string getFullscreenMode() {
    return fullscreenMode;
}

bool getisTrophyPopupDisabled() {
    return isTrophyPopupDisabled;
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
    return cursorState;
}

int getCursorHideTimeout() {
    return cursorHideTimeout;
}

double getTrophyNotificationDuration() {
    return trophyNotificationDuration;
}

u32 getWindowWidth() {
    return windowWidth;
}

u32 getWindowHeight() {
    return windowHeight;
}

u32 getInternalScreenWidth() {
    return internalScreenHeight;
}

u32 getInternalScreenHeight() {
    return internalScreenHeight;
}

s32 getGpuId() {
    return gpuId;
}

float getRcasAttenuation() {
    return rcas_attenuation;
}

void setRcasAttenuation(float value) {
    rcas_attenuation = value;
}

std::string getLogFilter() {
    return logFilter;
}

std::string getLogType() {
    return logType;
}

std::string getUserName() {
    return userName;
}

std::string getUpdateChannel() {
    return updateChannel;
}

std::string getChooseHomeTab() {
    return chooseHomeTab;
}

float getVolumeLevel() {
    return gameVolume;
}

void setVolumeLevel(float value) {
    gameVolume = value;
}

bool getUseSpecialPad() {
    return useSpecialPad;
}

int getSpecialPadClass() {
    return specialPadClass;
}

bool getIsMotionControlsEnabled() {
    return isMotionControlsEnabled;
}

bool debugDump() {
    return isDebugDump;
}

bool collectShadersForDebug() {
    return isShaderDebug;
}

bool showSplash() {
    return isShowSplash;
}

bool autoUpdate() {
    return isAutoUpdate;
}

bool alwaysShowChangelog() {
    return isAlwaysShowChangelog;
}

std::string sideTrophy() {
    return isSideTrophy;
}

bool nullGpu() {
    return isNullGpu;
}

bool copyGPUCmdBuffers() {
    return shouldCopyGPUBuffers;
}

void setReadbacksEnabled(bool enable) {
    readbacksEnabled = enable;
}

bool setReadbackLinearImages(bool enable) {
    return readbackLinearImagesEnabled = enable;
}

bool getReadbackLinearImages() {
    return readbackLinearImagesEnabled;
}

bool directMemoryAccess() {
    return directMemoryAccessEnabled;
}

bool dumpShaders() {
    return shouldDumpShaders;
}

bool patchShaders() {
    return shouldPatchShaders;
}

bool isRdocEnabled() {
    return rdocEnable;
}

bool fpsColor() {
    return fpsColorState;
}

u32 vblankDiv() {
    return vblankDivider;
}

bool vkValidationEnabled() {
    return vkValidation;
}

bool vkValidationSyncEnabled() {
    return vkValidationSync;
}

bool vkValidationGpuEnabled() {
    return vkValidationGpu;
}

bool getVkCrashDiagnosticEnabled() {
    return vkCrashDiagnostic;
}

bool getVkHostMarkersEnabled() {
    return vkHostMarkers;
}

bool getVkGuestMarkersEnabled() {
    return vkGuestMarkers;
}

void setVkCrashDiagnosticEnabled(bool enable) {
    vkCrashDiagnostic = enable;
}

void setVkHostMarkersEnabled(bool enable) {
    vkHostMarkers = enable;
}

void setVkGuestMarkersEnabled(bool enable) {
    vkGuestMarkers = enable;
}

bool getCompatibilityEnabled() {
    return compatibilityData;
}

bool getCheckCompatibilityOnStartup() {
    return checkCompatibilityOnStartup;
}

std::string getAudioBackend() {
    return audioBackend;
}

int getAudioVolume() {
    return audioVolume;
}

void setfpsColor(bool enable) {
    fpsColorState = enable;
}
void setGpuId(s32 selectedGpuId) {
    gpuId = selectedGpuId;
}

void setWindowWidth(u32 width) {
    windowWidth = width;
}

void setWindowHeight(u32 height) {
    windowHeight = height;
}

void setInternalScreenWidth(u32 width) {
    internalScreenWidth = width;
}

void setInternalScreenHeight(u32 height) {
    internalScreenHeight = height;
}

void setDebugDump(bool enable) {
    isDebugDump = enable;
}

void setCollectShaderForDebug(bool enable) {
    isShaderDebug = enable;
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
    isShowSplash = enable;
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
    isNullGpu = enable;
}

void setAllowHDR(bool enable) {
    isHDRAllowed = enable;
}

void setEnableAutoBackup(bool enable) {
    enableAutoBackup = enable;
}

void setCopyGPUCmdBuffers(bool enable) {
    shouldCopyGPUBuffers = enable;
}

bool getFastReadbacksEnabled() {
    return fastreadbacksEnabled;
}

void setFastReadbacksEnabled(bool enable) {
    fastreadbacksEnabled = enable;
}

void setDirectMemoryAccess(bool enable) {
    directMemoryAccessEnabled = enable;
}

void setDumpShaders(bool enable) {
    shouldDumpShaders = enable;
}

void setVkValidation(bool enable) {
    vkValidation = enable;
}

void setVkSyncValidation(bool enable) {
    vkValidationSync = enable;
}

void setRdocEnabled(bool enable) {
    rdocEnable = enable;
}

void setVblankDiv(u32 value) {
    vblankDivider = value;
}

void setIsFullscreen(bool enable) {
    isFullscreen = enable;
}
static void setShowLabelsUnderIcons(bool enable) {
    showLabelsUnderIcons = enable;
}

void setFullscreenMode(std::string mode) {
    fullscreenMode = mode;
}

void setisTrophyPopupDisabled(bool disable) {
    isTrophyPopupDisabled = disable;
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
    cursorState = newCursorState;
}

void setCursorHideTimeout(int newcursorHideTimeout) {
    cursorHideTimeout = newcursorHideTimeout;
}

void setTrophyNotificationDuration(double newTrophyNotificationDuration) {
    trophyNotificationDuration = newTrophyNotificationDuration;
}

void setLanguage(u32 language) {
    m_language = language;
}

void setNeoMode(bool enable) {
    isNeo = enable;
}

void setDevKitMode(bool enable) {
    isDevKit = enable;
}

void setLogType(const std::string& type) {
    logType = type;
}

void setLogFilter(const std::string& type) {
    logFilter = type;
}

void setSeparateLogFilesEnabled(bool enabled) {
    isSeparateLogFilesEnabled = enabled;
}

void setUserName(const std::string& type) {
    userName = type;
}

void setUpdateChannel(const std::string& type) {
    updateChannel = type;
}
void setChooseHomeTab(const std::string& type) {
    chooseHomeTab = type;
}

void setUseSpecialPad(bool use) {
    useSpecialPad = use;
}

void setSpecialPadClass(int type) {
    specialPadClass = type;
}

void setIsMotionControlsEnabled(bool use) {
    isMotionControlsEnabled = use;
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
    audioVolume = volume;
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

std::string getEmulatorLanguage() {
    return emulator_language;
}

u32 GetLanguage() {
    return m_language;
}

bool getSeparateLogFilesEnabled() {
    return isSeparateLogFilesEnabled;
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
    return isPSNSignedIn;
}

void setPSNSignedIn(bool sign) {
    isPSNSignedIn = sign;
}

bool getReadbacksEnabled() {
    return readbacksEnabled;
}
bool getShaderSkipsEnabled() {
    return shaderSkipsEnabled;
}

void setShaderSkipsEnabled(bool enable) {
    shaderSkipsEnabled = enable;
}

std::string getMemoryAlloc() {
    return memoryAlloc;
}

void setMemoryAlloc(std::string alloc) {
    memoryAlloc = alloc;
}

void load(const std::filesystem::path& path) {
    // If the configuration file does not exist, create it and return
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        save(path);
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

    u64 entry_count = 0;

    if (data.contains("General")) {
        const toml::value& general = data.at("General");
        enableAutoBackup = toml::find_or<bool>(general, "enableAutoBackup", false);
        isNeo = toml::find_or<bool>(general, "isPS4Pro", false);
        isDevKit = toml::find_or<bool>(general, "isDevKit", false);
        isPSNSignedIn = toml::find_or<bool>(general, "isPSNSignedIn", false);
        playBGM = toml::find_or<bool>(general, "playBGM", false);
        isTrophyPopupDisabled = toml::find_or<bool>(general, "isTrophyPopupDisabled", false);
        trophyNotificationDuration =
            toml::find_or<double>(general, "trophyNotificationDuration", 5.0);
        BGMvolume = toml::find_or<int>(general, "BGMvolume", 50);
        enableDiscordRPC = toml::find_or<bool>(general, "enableDiscordRPC", true);
        logFilter = toml::find_or<std::string>(general, "logFilter", "");
        logType = toml::find_or<std::string>(general, "logType", "sync");
        userName = toml::find_or<std::string>(general, "userName", "shadPS4");
        if (Common::g_is_release) {
            updateChannel = toml::find_or<std::string>(general, "updateChannel", "Full-Souls");
        } else if (!Common::g_is_release) {
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
        isShowSplash = toml::find_or<bool>(general, "showSplash", true);
        isAutoUpdate = toml::find_or<bool>(general, "autoUpdate", false);
        isAlwaysShowChangelog = toml::find_or<bool>(general, "alwaysShowChangelog", false);
        isSideTrophy = toml::find_or<std::string>(general, "sideTrophy", "right");
        compatibilityData = toml::find_or<bool>(general, "compatibilityEnabled", false);
        checkCompatibilityOnStartup =
            toml::find_or<bool>(general, "checkCompatibilityOnStartup", false);
        chooseHomeTab = toml::find_or<std::string>(general, "chooseHomeTab", "Release");
        audioBackend = toml::find_or<std::string>(general, "backend", "cubeb");
        audioVolume = toml::find_or<int>(general, "volume", 100);
        gameVolume = toml::find_or<float>(general, "gameVolume", 2.0f);
    }

    if (data.contains("Input")) {
        const toml::value& input = data.at("Input");

        cursorState = toml::find_or<int>(input, "cursorState", cursorState);
        cursorHideTimeout = toml::find_or<int>(input, "cursorHideTimeout", cursorHideTimeout);
        useSpecialPad = toml::find_or<bool>(input, "useSpecialPad", useSpecialPad);
        specialPadClass = toml::find_or<int>(input, "specialPadClass", specialPadClass);
        isMotionControlsEnabled =
            toml::find_or<bool>(input, "isMotionControlsEnabled", isMotionControlsEnabled);
        useUnifiedInputConfig =
            toml::find_or<bool>(input, "useUnifiedInputConfig", useUnifiedInputConfig);

        entry_count += input.size();
    }

    if (data.contains("GPU")) {
        const toml::value& gpu = data.at("GPU");

        screenWidth = toml::find_or<int>(gpu, "screenWidth", screenWidth);
        screenHeight = toml::find_or<int>(gpu, "screenHeight", screenHeight);
        rcas_attenuation = toml::find_or<float>(gpu, "rcas_attenuation", 0.25f);
        isNullGpu = toml::find_or<bool>(gpu, "nullGpu", false);
        shouldCopyGPUBuffers = toml::find_or<bool>(gpu, "copyGPUBuffers", false);
        directMemoryAccessEnabled = toml::find_or<bool>(gpu, "directMemoryAccess", false);
        shouldDumpShaders = toml::find_or<bool>(gpu, "dumpShaders", false);
        shouldPatchShaders = toml::find_or<bool>(gpu, "patchShaders", true);
        vblankDivider = toml::find_or<int>(gpu, "vblankDivider", 1);
        isFullscreen = toml::find_or<bool>(gpu, "Fullscreen", false);
        fullscreenMode = toml::find_or<std::string>(gpu, "FullscreenMode", "Windowed");
        isHDRAllowed = toml::find_or<bool>(gpu, "allowHDR", false);
        fastreadbacksEnabled = toml::find_or<bool>(gpu, "fastreadbacksEnabled", false);
        shaderSkipsEnabled = toml::find_or<bool>(gpu, "shaderSkipsEnabled", true);
        memoryAlloc = toml::find_or<bool>(gpu, "memoryAlloc", "medium");
        windowWidth = toml::find_or<int>(gpu, "screenWidth", windowWidth);
        windowHeight = toml::find_or<int>(gpu, "screenHeight", windowHeight);
        internalScreenWidth = toml::find_or<int>(gpu, "internalScreenWidth", internalScreenWidth);
        internalScreenHeight =
            toml::find_or<int>(gpu, "internalScreenHeight", internalScreenHeight);
        isNullGpu = toml::find_or<bool>(gpu, "nullGpu", isNullGpu);
        shouldCopyGPUBuffers = toml::find_or<bool>(gpu, "copyGPUBuffers", shouldCopyGPUBuffers);
        readbacksEnabled = toml::find_or<bool>(gpu, "readbacks", readbacksEnabled);
        readbackLinearImagesEnabled =
            toml::find_or<bool>(gpu, "readbackLinearImages", readbackLinearImagesEnabled);
        directMemoryAccessEnabled =
            toml::find_or<bool>(gpu, "directMemoryAccess", directMemoryAccessEnabled);
        shouldDumpShaders = toml::find_or<bool>(gpu, "dumpShaders", shouldDumpShaders);
        shouldPatchShaders = toml::find_or<bool>(gpu, "patchShaders", shouldPatchShaders);
        vblankDivider = toml::find_or<int>(gpu, "vblankDivider", vblankDivider);
        isFullscreen = toml::find_or<bool>(gpu, "Fullscreen", isFullscreen);
        fullscreenMode = toml::find_or<std::string>(gpu, "FullscreenMode", fullscreenMode);
        isHDRAllowed = toml::find_or<bool>(gpu, "allowHDR", isHDRAllowed);
        entry_count += gpu.size();
    }

    if (data.contains("Vulkan")) {
        const toml::value& vk = data.at("Vulkan");

        gpuId = toml::find_or<int>(vk, "gpuId", gpuId);
        vkValidation = toml::find_or<bool>(vk, "validation", vkValidation);
        vkValidationSync = toml::find_or<bool>(vk, "validation_sync", vkValidationSync);
        vkValidationGpu = toml::find_or<bool>(vk, "validation_gpu", vkValidationGpu);
        vkCrashDiagnostic = toml::find_or<bool>(vk, "crashDiagnostic", vkCrashDiagnostic);
        vkHostMarkers = toml::find_or<bool>(vk, "hostMarkers", vkHostMarkers);
        vkGuestMarkers = toml::find_or<bool>(vk, "guestMarkers", vkGuestMarkers);
        rdocEnable = toml::find_or<bool>(vk, "rdocEnable", rdocEnable);

        entry_count += vk.size();
    }

    if (data.contains("Debug")) {
        const toml::value& debug = data.at("Debug");

        isDebugDump = toml::find_or<bool>(debug, "DebugDump", isDebugDump);
        isSeparateLogFilesEnabled =
            toml::find_or<bool>(debug, "isSeparateLogFilesEnabled", isSeparateLogFilesEnabled);
        isShaderDebug = toml::find_or<bool>(debug, "CollectShader", isShaderDebug);
        setfpsColor(toml::find_or<bool>(debug, "FPSColor", fpsColor()));

        entry_count += debug.size();
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
        entry_count += gui.size();
    }

    if (data.contains("Settings")) {
        const toml::value& settings = data.at("Settings");
        m_language = toml::find_or<int>(settings, "consoleLanguage", m_language);

        entry_count += settings.size();
    }

    if (data.contains("Keys")) {
        const toml::value& keys = data.at("Keys");
        trophyKey = toml::find_or<std::string>(keys, "TrophyKey", trophyKey);

        entry_count += keys.size();
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

    data["General"]["isPS4Pro"] = isNeo;
    data["General"]["isDevKit"] = isDevKit;
    data["General"]["isPSNSignedIn"] = isPSNSignedIn;
    data["General"]["isTrophyPopupDisabled"] = isTrophyPopupDisabled;
    data["General"]["trophyNotificationDuration"] = trophyNotificationDuration;
    data["General"]["playBGM"] = playBGM;
    data["General"]["BGMvolume"] = BGMvolume;
    data["General"]["enableDiscordRPC"] = enableDiscordRPC;
    data["General"]["logFilter"] = logFilter;
    data["General"]["logType"] = logType;
    data["General"]["userName"] = userName;
    data["General"]["updateChannel"] = updateChannel;
    data["General"]["chooseHomeTab"] = chooseHomeTab;
    data["General"]["showSplash"] = isShowSplash;
    data["General"]["autoUpdate"] = isAutoUpdate;
    data["General"]["alwaysShowChangelog"] = isAlwaysShowChangelog;
    data["General"]["sideTrophy"] = isSideTrophy;
    data["General"]["compatibilityEnabled"] = compatibilityData;
    data["General"]["checkCompatibilityOnStartup"] = checkCompatibilityOnStartup;
    data["Input"]["cursorState"] = cursorState;
    data["Input"]["cursorHideTimeout"] = cursorHideTimeout;
    data["Input"]["useSpecialPad"] = useSpecialPad;
    data["Input"]["specialPadClass"] = specialPadClass;
    data["Input"]["isMotionControlsEnabled"] = isMotionControlsEnabled;
    data["Input"]["useUnifiedInputConfig"] = useUnifiedInputConfig;
    data["GPU"]["screenWidth"] = screenWidth;
    data["GPU"]["screenHeight"] = screenHeight;
    data["GPU"]["rcas_attenuation"] = rcas_attenuation;
    data["GPU"]["screenWidth"] = windowWidth;
    data["GPU"]["screenHeight"] = windowHeight;
    data["GPU"]["internalScreenWidth"] = internalScreenWidth;
    data["GPU"]["internalScreenHeight"] = internalScreenHeight;
    data["GPU"]["nullGpu"] = isNullGpu;
    data["GPU"]["copyGPUBuffers"] = shouldCopyGPUBuffers;
    data["GPU"]["readbacks"] = readbacksEnabled;
    data["GPU"]["readbackLinearImages"] = readbackLinearImagesEnabled;
    data["GPU"]["directMemoryAccess"] = directMemoryAccessEnabled;
    data["GPU"]["dumpShaders"] = shouldDumpShaders;
    data["GPU"]["patchShaders"] = shouldPatchShaders;
    data["GPU"]["vblankDivider"] = vblankDivider;
    data["GPU"]["Fullscreen"] = isFullscreen;
    data["GPU"]["FullscreenMode"] = fullscreenMode;
    data["GPU"]["allowHDR"] = isHDRAllowed;
    data["General"]["enableAutoBackup"] = enableAutoBackup;
    data["GPU"]["fastreadbacksEnabled"] = fastreadbacksEnabled;
    data["GPU"]["shaderSkipsEnabled"] = shaderSkipsEnabled;
    data["GPU"]["memoryAlloc"] = memoryAlloc;
    data["Vulkan"]["gpuId"] = gpuId;
    data["Vulkan"]["validation"] = vkValidation;
    data["Vulkan"]["validation_sync"] = vkValidationSync;
    data["Vulkan"]["validation_gpu"] = vkValidationGpu;
    data["Vulkan"]["crashDiagnostic"] = vkCrashDiagnostic;
    data["Vulkan"]["hostMarkers"] = vkHostMarkers;
    data["Vulkan"]["guestMarkers"] = vkGuestMarkers;
    data["Vulkan"]["rdocEnable"] = rdocEnable;
    data["General"]["backend"] = audioBackend;
    data["General"]["volume"] = audioVolume;
    data["General"]["gameVolume"] = gameVolume;
    data["Debug"]["DebugDump"] = isDebugDump;
    data["Debug"]["CollectShader"] = isShaderDebug;
    data["Debug"]["isSeparateLogFilesEnabled"] = isSeparateLogFilesEnabled;
    data["Debug"]["FPSColor"] = fpsColor();
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
    readbacksEnabled = false;
    fastreadbacksEnabled = false;
    shaderSkipsEnabled = false;
    readbackLinearImagesEnabled = false;
    directMemoryAccessEnabled = false;
    shouldDumpShaders = false;
    shouldPatchShaders = false;
    vblankDivider = 1;
    isFullscreen = false;
    fullscreenMode = "Windowed";
    isHDRAllowed = false;

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
    setfpsColor(true);

    // GUI
    load_game_size = true;

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

constexpr std::string_view GetDefaultKeyboardConfig() {
    return R"(# Feeling lost? Check out the Help section!

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
std::filesystem::path GetFoolproofKbmConfigFile(const std::string& game_id) {
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
        const auto default_config = GetDefaultKeyboardConfig();
        std::ofstream default_config_stream(default_config_file);
        if (default_config_stream) {
            default_config_stream << default_config;
        }
    }

    // if empty, we only need to execute the function up until this point
    if (game_id.empty()) {
        return default_config_file;
    }

    // If game-specific config doesn't exist, create it from the default config
    if (!std::filesystem::exists(config_file)) {
        std::filesystem::copy(default_config_file, config_file);
    }
    return config_file;
}

} // namespace Config
