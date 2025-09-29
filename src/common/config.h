// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <vector>
#include "common/types.h"

namespace Config {

struct GameInstallDir {
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
std::string getUserName();
std::string getUpdateChannel();
std::string getChooseHomeTab();

u16 leftDeadZone();
u16 rightDeadZone();
s16 getCursorState();
int getCursorHideTimeout();
double getTrophyNotificationDuration();
bool getUseSpecialPad();
int getSpecialPadClass();
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
void setUseSpecialPad(bool use);
bool getUseSpecialPad();
void setSpecialPadClass(int type);
int getSpecialPadClass();
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
void setUserName(const std::string& type);
void setUpdateChannel(const std::string& type);
void setChooseHomeTab(const std::string& type);
void setGameInstallDirs(const std::vector<std::filesystem::path>& dirs_config);
void setAllGameInstallDirs(const std::vector<GameInstallDir>& dirs_config);
void setSaveDataPath(const std::filesystem::path& path);
void setCompatibilityEnabled(bool use);
void setCheckCompatibilityOnStartup(bool use);
void setBackgroundImageOpacity(int opacity);
void setShowBackgroundImage(bool show);
void setPSNSignedIn(bool sign);
void setShaderSkipsEnabled(bool enable);

void setAudioVolume(int volume);
void setSeparateUpdateEnabled(bool use);

std::string getMainOutputDevice();
void setMainOutputDevice(std::string device);
std::string getPadSpkOutputDevice();
void setPadSpkOutputDevice(std::string device);

void setCursorState(s16 cursorState);
void setCursorHideTimeout(int newcursorHideTimeout);
void setTrophyNotificationDuration(double newTrophyNotificationDuration);
void setUseSpecialPad(bool use);
void setSpecialPadClass(int type);
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
bool addGameInstallDir(const std::filesystem::path& dir, bool enabled = true);
void removeGameInstallDir(const std::filesystem::path& dir);
void setGameInstallDirEnabled(const std::filesystem::path& dir, bool enabled);
void setAddonInstallDir(const std::filesystem::path& dir);
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
const std::vector<std::filesystem::path> getGameInstallDirs();
const std::vector<bool> getGameInstallDirsEnabled();
std::filesystem::path getAddonInstallDir();
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
void setVolumeSlider(int volumeValue);
bool isMuteEnabled();
void setMuteEnabled(bool enabled);

bool getAutoRestartGame();
void setAutoRestartGame(bool enable);
bool getRestartWithBaseGame();
void setRestartWithBaseGame(bool enable);

void setDefaultValues();

constexpr std::string_view GetDefaultGlobalConfig();
std::filesystem::path GetFoolproofInputConfigFile(const std::string& game_id = "");

// settings
u32 GetLanguage();
}; // namespace Config