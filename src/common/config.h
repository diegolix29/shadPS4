// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <vector>
#include "types.h"

namespace Config {

struct GameInstallDir {
    std::filesystem::path path;
    bool enabled;
};

enum HideCursorState : int { Never, Idle, Always };

void load(const std::filesystem::path& path);
void save(const std::filesystem::path& path);
void saveMainWindow(const std::filesystem::path& path);

float getRcasAttenuation();
void setRcasAttenuation(float value);
float getVolumeLevel();
void setVolumeLevel(float value);
std::string getTrophyKey();
void setTrophyKey(std::string key);
bool GetLoadGameSizeEnabled();
std::filesystem::path GetSaveDataPath();
void setLoadGameSizeEnabled(bool enable);
bool getIsFullscreen();
bool getShowLabelsUnderIcons();
bool setShowLabelsUnderIcons();
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
bool getReadbacksEnabled();
bool getFastReadbacksEnabled();
bool getShaderSkipsEnabled();
std::string getAudioBackend();
int getAudioVolume();
std::string getMemoryAlloc();

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
std::string getBackButtonBehavior();
bool getUseSpecialPad();
int getSpecialPadClass();
bool getIsMotionControlsEnabled();
bool GetUseUnifiedInputConfig();
void SetUseUnifiedInputConfig(bool use);
bool GetOverrideControllerColor();
void SetOverrideControllerColor(bool enable);
int* GetControllerCustomColor();
void SetControllerCustomColor(int r, int b, int g);

u32 getScreenWidth();
u32 getScreenHeight();
void setFullscreenMode(std::string mode);
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

bool debugDump();
bool collectShadersForDebug();
bool showSplash();
bool autoUpdate();
bool alwaysShowChangelog();
std::string sideTrophy();
bool nullGpu();
bool copyGPUCmdBuffers();
void setCopyGPUCmdBuffers(bool enable);
bool setReadbackLinearImages(bool enable);
bool getReadbackLinearImages();
bool directMemoryAccess();
void setDirectMemoryAccess(bool enable);
bool dumpShaders();
bool patchShaders();
bool isRdocEnabled();
bool fpsColor();
u32 vblankDiv();
bool ShouldSkipShader(const u64& hash);
void SetSkippedShaderHashes(const std::string& game_id);
void setMicDevice(std::string device);
std::string getMicDevice();

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
void setVblankDiv(u32 value);
void setGpuId(s32 selectedGpuId);
void setScreenWidth(u32 width);
void setScreenHeight(u32 height);
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
bool isNeoModeConsole();
void setNeoMode(bool enable);  // no ui setting
bool isDevKitConsole();        // no set
bool vkValidationGpuEnabled(); // no set
bool getIsMotionControlsEnabled();
void setIsMotionControlsEnabled(bool use);

// TODO
bool GetLoadGameSizeEnabled();
std::filesystem::path GetSaveDataPath();
void setLoadGameSizeEnabled(bool enable);
bool getCompatibilityEnabled();
bool getCheckCompatibilityOnStartup();
bool getIsConnectedToNetwork();
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
void setReadbacksEnabled(bool enable);
void setFastReadbacksEnabled(bool enable);
void setShaderSkipsEnabled(bool enable);

void setAudioBackend(std::string backend);
void setAudioVolume(int volume);
void setSeparateUpdateEnabled(bool use);
void setMemoryAlloc(std::string alloc);

void setCursorState(s16 cursorState);
void setCursorHideTimeout(int newcursorHideTimeout);
void setTrophyNotificationDuration(double newTrophyNotificationDuration);
void setBackButtonBehavior(const std::string& type);
void setUseSpecialPad(bool use);
void setSpecialPadClass(int type);
void setIsMotionControlsEnabled(bool use);

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

void setDefaultValues();

std::filesystem::path GetFoolproofKbmConfigFile(const std::string& game_id = "");

// settings
u32 GetLanguage();
}; // namespace Config