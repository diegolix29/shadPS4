// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/types.h"
namespace Config {

enum class ConfigMode {
    Default,
    Global,
    Clean,
};
void setConfigMode(ConfigMode mode);

struct GameDirectories {
    std::filesystem::path path;
    bool enabled;
};

enum HideCursorState : u32 {
    Never,
    Idle,
    Always,
};

enum class ReadbackSpeed : u32 {
    Disable,
    Unsafe,
    Low,
    Default,
    Fast,
};

void load(const std::filesystem::path& path, bool is_game_specific = false);
void save(const std::filesystem::path& path);
void saveMainWindow(const std::filesystem::path& path);

bool getSeparateUpdateEnabled();
int getRcasAttenuation();
void setRcasAttenuation(int value);
std::string getTrophyKey();
void setTrophyKey(std::string key);
bool GetLoadGameSizeEnabled();
std::filesystem::path GetSaveDataPath();
void setLoadGameSizeEnabled(bool enable);
bool getIsFullscreen();
bool getShowLabelsUnderIcons();
void setShowLabelsUnderIcons(bool enable);
bool getEnableColorFilter();
void setEnableColorFilter(bool enable);
bool getShowFpsCounter();
void setShowFpsCounter(bool enable, bool is_game_specific = false);
bool getToolbarWidgetVisibility(const std::string& name, bool default_value);
void setToolbarWidgetVisibility(const std::string& name, bool is_visible);
std::string getFullscreenMode();
bool isNeoModeConsole();
bool isDevKitConsole();
bool getPlayBGM();
int getBGMvolume();
bool getisTrophyPopupDisabled();
bool getEnableDiscordRPC();
bool getCompatibilityEnabled();
bool getCheckCompatibilityOnStartup();
int getBackgroundImageOpacity();
bool getShowBackgroundImage();
bool getPSNSignedIn();
bool getShaderSkipsEnabled();
std::string getAudioBackend();
int getAudioVolume();

std::string getLogFilter();
std::string getLogType();
std::string getUserName(int id);
std::array<std::string, 4> const getUserNames();
std::string getUpdateChannel();
std::string getChooseHomeTab();
void setSeparateUpdateEnabled(bool use);

u16 leftDeadZone();
u16 rightDeadZone();
s16 getCursorState();
int getCursorHideTimeout();
double getTrophyNotificationDuration();

bool getIsMotionControlsEnabled();
bool GetUseUnifiedInputConfig();
void SetUseUnifiedInputConfig(bool use);
bool GetOverrideControllerColor();
void SetOverrideControllerColor(bool enable);
int* GetControllerCustomColor();
void SetControllerCustomColor(int r, int b, int g);

void setFullscreenMode(std::string mode);
std::string getPresentMode();
void setPresentMode(std::string mode);
u32 getWindowWidth();
u32 getWindowHeight();
void setWindowWidth(u32 width);
void setWindowHeight(u32 height);
u32 getInternalScreenWidth();
u32 getInternalScreenHeight();
void setInternalScreenWidth(u32 width);
void setInternalScreenHeight(u32 height);
bool debugDump();
void setDebugDump(bool enable);
s32 getGpuId();
bool allowHDR();
bool getEnableAutoBackup();

void setGuiStyle(const std::string& style);
std::string getGuiStyle();
std::string getVersionPath();
void setVersionPath(const std::string& path);
bool getBootLauncher();
void setBootLauncher(bool enabled);
bool getSdlInstalled();
void setSdlInstalled(bool use);
bool getQTInstalled();
void setQTInstalled(bool use);
bool getGameRunning();
void setGameRunning(bool running);
bool debugDump();
bool collectShadersForDebug();
bool showSplash();
bool autoUpdate();
bool alwaysShowChangelog();
std::string sideTrophy();
bool nullGpu();
bool copyGPUCmdBuffers();
void setCopyGPUCmdBuffers(bool enable);
ReadbackSpeed readbackSpeed();
void setReadbackSpeed(ReadbackSpeed mode);
bool setReadbackLinearImages(bool enable);
bool getReadbackLinearImages();
bool setScreenTipDisable(bool enable);
bool getScreenTipDisable();
bool directMemoryAccess();
void setDirectMemoryAccess(bool enable);
bool dumpShaders();
bool patchShaders();
bool isRdocEnabled();
bool fpsColor();
u32 vblankFreq();
void setVblankFreq(u32 value);
bool ShouldSkipShader(const u64& hash);
void SetSkippedShaderHashes(const std::string& game_id);
void setMicDevice(std::string device);
std::string getMicDevice();

std::string getCustomBackgroundImage();
void setCustomBackgroundImage(const std::string& path);

void setDebugDump(bool enable);
void setCollectShaderForDebug(bool enable);
void setShowSplash(bool enable);
void setAutoUpdate(bool enable);
void setAlwaysShowChangelog(bool enable);
void setSideTrophy(std::string side);
void setNullGpu(bool enable);
void setAllowHDR(bool enable);
void setEnableAutoBackup(bool enable);
void setCopyGPUCmdBuffers(bool enable);
void setDumpShaders(bool enable);
void setGpuId(s32 selectedGpuId);
void setIsFullscreen(bool enable);
void setFullscreenMode(std::string mode);
void setisTrophyPopupDisabled(bool disable);
void setPlayBGM(bool enable);
void setBGMvolume(int volume);
void setEnableDiscordRPC(bool enable);
void setLanguage(u32 language);
void setUseSpecialPad(int pad, bool use, bool is_game_specific);
bool getUseSpecialPad(int pad);
void setSpecialPadClass(int pad, int type, bool is_game_specific);
int getSpecialPadClass(int pad);
bool getPSNSignedIn();
void setPSNSignedIn(bool sign); // no ui setting
bool patchShaders();            // no set
void setfpsColor(bool enable);
void setNeoMode(bool enable);  // no ui setting
bool vkValidationGpuEnabled(); // no set
int getExtraDmemInMbytes();
void setExtraDmemInMbytes(int value);
bool getIsMotionControlsEnabled();
void setIsMotionControlsEnabled(bool use);
std::string getDefaultControllerID();
void setDefaultControllerID(std::string id);
bool getBackgroundControllerInput();
void setBackgroundControllerInput(bool enable);
bool getLoggingEnabled();
void setLoggingEnabled(bool enable);
bool getFsrEnabled();
void setFsrEnabled(bool enable);
bool getRcasEnabled();
void setRcasEnabled(bool enable);
bool getLoadAutoPatches();
void setLoadAutoPatches(bool enable);
bool isPipelineCacheArchived();
void setPipelineCacheArchived(bool enable, bool is_game_specific = false);

enum UsbBackendType : int { Real, SkylandersPortal, InfinityBase, DimensionsToypad };
int getUsbDeviceBackend();
void setUsbDeviceBackend(int value, bool is_game_specific = false);

// TODO
bool GetLoadGameSizeEnabled();
std::filesystem::path GetSaveDataPath();
void setLoadGameSizeEnabled(bool enable);
bool getCompatibilityEnabled();
bool getCheckCompatibilityOnStartup();
bool getIsConnectedToNetwork();
void setIsConnectedToNetwork(bool connected);
std::string getUserName();
std::string getChooseHomeTab();
bool GetUseUnifiedInputConfig();
void SetUseUnifiedInputConfig(bool use);
bool GetOverrideControllerColor();
void SetOverrideControllerColor(bool enable);
int* GetControllerCustomColor();
void SetControllerCustomColor(int r, int b, int g);
void setUserName(int id, const std::string& name);

void setUpdateChannel(const std::string& type);
void setChooseHomeTab(const std::string& type);
void setGameDirectories(const std::vector<std::filesystem::path>& dirs_config);
void setAllGameDirectories(const std::vector<GameDirectories>& dirs_config);
void setSaveDataPath(const std::filesystem::path& path);
void setCompatibilityEnabled(bool use);
void setCheckCompatibilityOnStartup(bool use);
void setBackgroundImageOpacity(int opacity);
void setShowBackgroundImage(bool show);
void setPSNSignedIn(bool sign);
void setShaderSkipsEnabled(bool enable);

void setAudioVolume(int volume);

std::string getMainOutputDevice();
void setMainOutputDevice(std::string device);
std::string getPadSpkOutputDevice();
void setPadSpkOutputDevice(std::string device);

void setCursorState(s16 cursorState);
void setCursorHideTimeout(int newcursorHideTimeout);
void setTrophyNotificationDuration(double newTrophyNotificationDuration);
void setIsMotionControlsEnabled(bool use);
std::filesystem::path getSysModulesPath();
void setSysModulesPath(const std::filesystem::path& path);

void setLogType(const std::string& type);
void setLogFilter(const std::string& type);
void setSeparateLogFilesEnabled(bool enabled);
bool getSeparateLogFilesEnabled();
void setVkValidation(bool enable);
void setVkSyncValidation(bool enable);
void setRdocEnabled(bool enable);

bool vkValidationEnabled();
bool vkValidationCoreEnabled();
bool vkValidationSyncEnabled();
bool vkValidationGpuEnabled();
bool getVkCrashDiagnosticEnabled();
bool getVkHostMarkersEnabled();
bool getVkGuestMarkersEnabled();
void setVkCrashDiagnosticEnabled(bool enable);
void setVkHostMarkersEnabled(bool enable);
void setVkGuestMarkersEnabled(bool enable);
void setNeoMode(bool enable);
void setDevKitMode(bool enable);

// Gui
void setMainWindowGeometry(u32 x, u32 y, u32 w, u32 h);
bool addGameDirectories(const std::filesystem::path& dir, bool enabled = true);
void removeGameDirectories(const std::filesystem::path& dir);
void setGameDirectoriesEnabled(const std::filesystem::path& dir, bool enabled);
void setAddonDirectories(const std::filesystem::path& dir);
void setMainWindowTheme(u32 theme);
void setIconSize(u32 size);
void setIconSizeGrid(u32 size);
void setSliderPosition(u32 pos);
void setSliderPositionGrid(u32 pos);
void setTableMode(u32 mode);
void setMainWindowWidth(u32 width);
void setMainWindowHeight(u32 height);
void setElfViewer(const std::vector<std::string>& elfList);
void setRecentFiles(const std::vector<std::string>& recentFiles);
void setEmulatorLanguage(std::string language);
bool getPauseOnUnfocus();
void setPauseOnUnfocus(bool enable);
bool getShowWelcomeDialog();
void setShowWelcomeDialog(bool enable);
u32 getFpsLimit();
void setFpsLimit(u32 fpsValue);
bool isFpsLimiterEnabled();
void setFpsLimiterEnabled(bool enabled);
bool getFirstBootHandled();
void setFirstBootHandled(bool handled);
u32 getMainWindowGeometryX();
u32 getMainWindowGeometryY();
u32 getMainWindowGeometryW();
u32 getMainWindowGeometryH();
bool isPipelineCacheEnabled();
void setPipelineCacheEnabled(bool enable, bool is_game_specific = false);
const std::vector<std::filesystem::path> getGameDirectories();
const std::vector<bool> getGameDirectoriesEnabled();
std::filesystem::path getAddonDirectory();
u32 getMainWindowTheme();
u32 getIconSize();
u32 getIconSizeGrid();
u32 getSliderPosition();
u32 getSliderPositionGrid();
u32 getTableMode();
u32 getMainWindowWidth();
u32 getMainWindowHeight();
std::vector<std::string> getElfViewer();
std::vector<std::string> getRecentFiles();
std::string getEmulatorLanguage();

int getVolumeSlider();
void setVolumeSlider(int volumeValue, bool is_game_specific);
bool isMuteEnabled();
void setMuteEnabled(bool enabled);

bool GamesMenuUI();
void setGamesMenuUI(bool enable);
bool HubMenuUI();
void setHubMenuUI(bool enable);
bool getRestartWithBaseGame();
void setRestartWithBaseGame(bool enable);
bool DisableHardcodedHotkeys();
void setDisableHardcodedHotkeys(bool disable);
bool UseHomeButtonForHotkeys();
void setUseHomeButtonForHotkeys(bool disable);

void setDefaultValues();

constexpr std::string_view GetDefaultGlobalConfig();
std::filesystem::path GetFoolproofInputConfigFile(const std::string& game_id = "");

// settings
u32 GetLanguage();
}; // namespace Config