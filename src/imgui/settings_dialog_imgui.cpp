//  SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include <map>
#include <ranges>
#include <cmrc/cmrc.hpp>
#include <stb_image.h>

#include "common/config.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "imgui/imgui_std.h"
#include "settings_dialog_imgui.h"

#include "imgui_fonts/notosansjp_regular.ttf.g.cpp"
#include "imgui_fonts/proggyvector_regular.ttf.g.cpp"

CMRC_DECLARE(res);
namespace BigPictureMode {

//////////////////// options for comboboxes
const std::map<std::string, int> languageMap = {{"Arabic", 21},
                                                {"Czech", 23},
                                                {"Danish", 14},
                                                {"Dutch", 6},
                                                {"English (United Kingdom)", 18},
                                                {"English (United States)", 1},
                                                {"Finnish", 12},
                                                {"French (Canada)", 22},
                                                {"French (France)", 2},
                                                {"German", 4},
                                                {"Greek", 25},
                                                {"Hungarian", 24},
                                                {"Indonesian", 29},
                                                {"Italian", 5},
                                                {"Japanese", 0},
                                                {"Korean", 9},
                                                {"Norwegian (Bokmaal)", 15},
                                                {"Polish", 16},
                                                {"Portuguese (Brazil)", 17},
                                                {"Portuguese (Portugal)", 7},
                                                {"Romanian", 26},
                                                {"Russian", 8},
                                                {"Simplified Chinese", 11},
                                                {"Spanish (Latin America)", 20},
                                                {"Spanish (Spain)", 3},
                                                {"Swedish", 13},
                                                {"Thai", 27},
                                                {"Traditional Chinese", 10},
                                                {"Turkish", 19},
                                                {"Ukrainian", 30},
                                                {"Vietnamese", 28}};
std::vector<std::string> languageOptions; // assigned from keys above
const std::vector<std::string> logTypeOptions = {"sync", "async"};
const std::vector<std::string> fullscreenModeOptions = {"Windowed", "Fullscreen",
                                                        "Fullscreen (Borderless)"};
const std::vector<std::string> audioBackendOptions = {"SDL", "OpenAL"};
const std::vector<std::string> presentModeOptions = {"Mailbox", "Fifo", "Immediate"};
const std::vector<std::string> hideCursorOptions = {"Never", "Idle", "Always"};
const std::vector<std::string> trophySideOptions = {"left", "right", "top", "bottom"};
const std::vector<std::string> readbacksModeOptions = {"Disabled", "Relaxed", "Precise"};

//////////////// Setting Variables
//////////////// Note:: Use int for all comboboxes as needed by ImGui

// General tab
int consoleLanguageSetting;
int volumeSetting;
bool showSplashSetting;
int audioBackendSetting;

// Graphics tab
int fullscreenModeSetting;
int presentModeSetting;
int windowWidthSetting;
int windowHeightSetting;
bool hdrAllowedSetting;
bool fsrEnabledSetting;
bool rcasEnabledSetting;
float rcasAttenuationSetting;

// Input tab
bool motionControlsSetting;
bool backgroundControllerSetting;
int cursorStateSetting;
int cursorTimeoutSetting;

// Trophy tab
bool trophyPopupDisabledSetting;
int trophySideSetting;
float trophyDurationSetting;

// Log tab
bool logEnabledSetting;
bool separateLogSetting;
int logTypeSetting;

// Experimental tab
int readbacksModeSetting;
bool readbackLinearImagesSetting;
bool directMemoryAccessSetting;
bool devkitConsoleSetting;
bool neoModeSetting;
bool psnSignedInSetting;
bool connectedNetworkSetting;
bool pipelineCacheEnabledSetting;
bool pipelineCacheArchiveSetting;
int extraDmemSetting;
int vblankFrequencySetting;

//////////////// Texture data
SDL_Texture* profilesTexture;
SDL_Texture* generalTexture;
SDL_Texture* globalSettingsTexture;
SDL_Texture* experimentalTexture;
SDL_Texture* graphicsTexture;
SDL_Texture* inputTexture;
SDL_Texture* trophyTexture;
SDL_Texture* logTexture;

//////////////// Gui variable
const float gameImageSize = 200.f;
const float settingsIconSize = 125.f;
std::vector<Game> settingsProfileVec = {};

float uiScale = 1.0f;
SDL_Renderer* renderer;

SettingsCategory currentCategory = SettingsCategory::Profiles;
std::string currentProfile = "Global";
bool closeOnSave = false;

void Init() {
    auto languageKeys = std::views::keys(languageMap);
    languageOptions.assign(languageKeys.begin(), languageKeys.end());

    currentProfile = "Global";
    currentCategory = SettingsCategory::Profiles;
    LoadSettings("Global");

    SDL_Window* window = SDL_GetKeyboardFocus();
    renderer = SDL_GetRenderer(window);

    LoadEmbeddedTexture("src/images/big_picture/settings.png", generalTexture);
    LoadEmbeddedTexture("src/images/big_picture/folder.png", profilesTexture);
    LoadEmbeddedTexture("src/images/big_picture/global-settings.png", globalSettingsTexture);
    LoadEmbeddedTexture("src/images/big_picture/experimental.png", experimentalTexture);
    LoadEmbeddedTexture("src/images/big_picture/graphics.png", graphicsTexture);
    LoadEmbeddedTexture("src/images/big_picture/controller.png", inputTexture);
    LoadEmbeddedTexture("src/images/big_picture/trophy.png", trophyTexture);
    LoadEmbeddedTexture("src/images/big_picture/log.png", logTexture);

    GetGameInfo(settingsProfileVec, true, globalSettingsTexture);
    uiScale = static_cast<float>(Config::getBigPictureScale() / 1000.f);
}

void DeInit() {
    Config::load(std::filesystem::path{}, false);
    Config::setBigPictureScale(static_cast<int>(uiScale * 1000));
    Config::save(std::filesystem::path{}, false);
}

void DrawSettings(bool* open) {
    if (!*open)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    if (ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoDecoration)) {
        if (ImGui::IsWindowAppearing()) {
            Init();
            closeOnSave = false;
        }

        ImGui::DrawPrettyBackground();
        ImGui::SetWindowFontScale(uiScale);
        ImGuiWindowFlags child_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NavFlattened;

        ImVec4 settingsColor = ImVec4(0.1f, 0.1f, 0.12f, 0.8f); // Darker gray
        ImGui::PushStyleColor(ImGuiCol_ChildBg, settingsColor);
        ImGui::BeginChild("Categories", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY,
                          child_flags | ImGuiWindowFlags_HorizontalScrollbar);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(30.0f * uiScale, 0.0f));

        // Must add categories in enum order for L1/R1 to work correctly, with experimental last
        AddCategory("Profiles", profilesTexture, SettingsCategory::Profiles);
        AddCategory("General", generalTexture, SettingsCategory::General);
        AddCategory("Graphics", graphicsTexture, SettingsCategory::Graphics);
        AddCategory("Input", inputTexture, SettingsCategory::Input);
        AddCategory("Trophy", trophyTexture, SettingsCategory::Trophy);
        AddCategory("Log", logTexture, SettingsCategory::Log);

        if (currentProfile != "Global")
            AddCategory("Experimental", experimentalTexture, SettingsCategory::Experimental);

        ImGui::PopStyleVar();
        ImGui::EndChild(); // Categories

        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::BeginChild("ContentRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true,
                          child_flags);
        ImGui::PopStyleColor();

        LoadCategory(currentCategory);

        ImGui::EndChild();
        ImGui::Separator();

        ImGui::SetNextItemWidth(300.0f * uiScale);
        static float sliderScale2 = 1.0f;
        if (ImGui::IsWindowAppearing()) {
            sliderScale2 = uiScale;
        }

        ImGui::SliderFloat("UI Scale", &sliderScale2, 0.25f, 3.0f);
        // Only update when user is not interacting with slider
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            uiScale = sliderScale2;
        }
        ImGui::SameLine();

        // Align buttons right
        float buttonsWidth = ImGui::CalcTextSize("Save").x + ImGui::CalcTextSize("Cancel").x +
                             ImGui::CalcTextSize("Apply").x +
                             ImGui::GetStyle().FramePadding.x * 6.0f +
                             ImGui::GetStyle().ItemSpacing.x * 2;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonsWidth);

        if (ImGui::Button("Save")) {
            closeOnSave = true;
            ImGui::OpenPopup("Save Confirmation");
        }

        ImGui::SameLine();
        if (ImGui::Button("Apply")) {
            ImGui::OpenPopup("Save Confirmation");
        }

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Save Confirmation", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", ("Profile Saved:\n" + currentProfile).c_str());
            ImGui::Separator();

            if (ImGui::Button("OK", ImVec2(250 * uiScale, 0))) {
                std::string profileToSave = currentProfile;
                bool isGameSpecific = (currentProfile != "Global");

                if (isGameSpecific) {
                    // Extract just the 9-digit serial (CUSA00000 format)
                    size_t spacePos = currentProfile.find(' ');
                    if (spacePos != std::string::npos) {
                        profileToSave = currentProfile.substr(0, spacePos);
                    }
                    // Ensure it's exactly 9 characters for CUSA format
                    if (profileToSave.length() > 9) {
                        profileToSave = profileToSave.substr(0, 9);
                    }
                }

                SaveSettings(profileToSave, isGameSpecific);
                if (closeOnSave) {
                    DeInit();
                    *open = false;
                    ImGui::CloseCurrentPopup();
                } else {
                    ImGui::CloseCurrentPopup();
                }
            }

            ImGui::EndPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            DeInit();
            *open = false;
        }

        SettingsCategory lastCategory =
            currentProfile != "Global" ? SettingsCategory::Experimental : SettingsCategory::Log;
        // Navigate categories with Tab / R1 / L1
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_Tab)) {
            int currentIndex = static_cast<int>(currentCategory);
            currentCategory == lastCategory
                ? currentCategory = static_cast<SettingsCategory>(0)
                : currentCategory = static_cast<SettingsCategory>(currentIndex + 1);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) {
            int currentIndex = static_cast<int>(currentCategory);
            currentIndex == 0 ? currentCategory = lastCategory
                              : currentCategory = static_cast<SettingsCategory>(currentIndex - 1);
        }
    }

    ImGui::End();
}

void LoadCategory(SettingsCategory category) {
    ImGui::TextColored(ImVec4(0.00f, 1.00f, 1.00f, 1.00f), "%s",
                       ("Selected Profile: " + currentProfile).c_str()); // Dark Blue
    ImGui::Dummy(ImVec2(0, 20.f * uiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f * uiScale, 10.0f * uiScale));

    if (category == SettingsCategory::General) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingCombo("Console Language", consoleLanguageSetting, languageOptions);
            AddSettingSliderInt("Volume", volumeSetting, 0, 500);
            AddSettingBool("Show Splash Screen When Launching Game", showSplashSetting);
            AddSettingCombo("Audio Backend", audioBackendSetting, audioBackendOptions);

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Graphics) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingCombo("Display Mode", fullscreenModeSetting, fullscreenModeOptions);
            AddSettingCombo("Present Mode", presentModeSetting, presentModeOptions);
            AddSettingSliderInt("Window Width", windowWidthSetting, 0, 8000);
            AddSettingSliderInt("Window Height", windowHeightSetting, 0, 7000);
            AddSettingBool("Enable HDR", hdrAllowedSetting);
            AddSettingBool("Enable FSR", fsrEnabledSetting);

            if (fsrEnabledSetting) {
                AddSettingBool("Enable RCAS", rcasEnabledSetting);
            }

            if (rcasEnabledSetting && fsrEnabledSetting) {
                AddSettingSliderFloat("RCAS Attenuation", rcasAttenuationSetting, 0.0f, 3.0f, 3);
            }

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Input) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingBool("Enable Motion Controls", motionControlsSetting);
            AddSettingBool("Enable Background Controller Input", backgroundControllerSetting);
            AddSettingCombo("Hide Cursor", cursorStateSetting, hideCursorOptions);

            if (cursorStateSetting == 1) {
                AddSettingSliderInt("Hide Cursor Idle Timeout", cursorTimeoutSetting, 1, 10);
            }

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Trophy) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingBool("Disable Trophy Notification", trophyPopupDisabledSetting);
            if (!trophyPopupDisabledSetting) {
                AddSettingCombo("Trophy Notification Position", trophySideSetting,
                                trophySideOptions);
                AddSettingSliderFloat("Trophy Notification Duration", trophyDurationSetting, 0.f,
                                      10.f, 1);
            }

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Log) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingBool("Enable Logging", logEnabledSetting);
            if (logEnabledSetting) {
                AddSettingBool("Separate Log Files", separateLogSetting);
                AddSettingCombo("Log Type", logTypeSetting, logTypeOptions);
            }

            ImGui::EndTable();
        }
    } else if (category == SettingsCategory::Experimental) {
        if (ImGui::BeginTable("SettingsTable", 2)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 500.0f * uiScale);
            ImGui::TableSetupColumn("Value");

            AddSettingSliderInt("Additional DMem Allocation", extraDmemSetting, 0, 20000);
            AddSettingSliderInt("Vblank Frequency", vblankFrequencySetting, 30, 360);
            AddSettingCombo("Readbacks Mode", readbacksModeSetting, readbacksModeOptions);
            AddSettingBool("Enable Readback Linear Images", readbackLinearImagesSetting);
            AddSettingBool("Enable Direct Memory Access", directMemoryAccessSetting);
            AddSettingBool("Enable Devkit Console Mode", devkitConsoleSetting);
            AddSettingBool("Enable PS4 Neo Mode", neoModeSetting);
            AddSettingBool("Set PSN Sign-in to True", psnSignedInSetting);
            AddSettingBool("Set Network Connected to True", connectedNetworkSetting);
            AddSettingBool("Enable Shader Cache", pipelineCacheEnabledSetting);

            if (pipelineCacheEnabledSetting) {
                AddSettingBool("Compress Shader Cache to Zip File", pipelineCacheArchiveSetting);
            }

            ImGui::EndTable();
        }
    }

    ImGui::PopStyleVar();

    // Child Window if Needed
    if (category == SettingsCategory::Profiles) {
        ImGuiWindowFlags child_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NavFlattened;
        ImGui::BeginChild("ProfileSelect", ImVec2(0, 0), true, child_flags);
        Overlay::TextCentered("Select Global or a Game to save Custom Configuration");
        SetProfileIcons(settingsProfileVec);
        ImGui::EndChild();
    }
}

void SaveSettings(std::string profile, bool isGameSpecific) {
    /////////// General Tab
    Config::setLanguage(languageMap.at(languageOptions.at(consoleLanguageSetting)));
    Config::setVolumeSlider(volumeSetting, isGameSpecific);
    Config::setShowSplash(showSplashSetting);
    Config::setAudioBackend(static_cast<Config::AudioBackend>(audioBackendSetting));

    /////////// Graphics Tab
    bool isFullscreen = fullscreenModeSetting != 0;
    Config::setIsFullscreen(isFullscreen);
    Config::setFullscreenMode(fullscreenModeOptions.at(fullscreenModeSetting));
    Config::setPresentMode(presentModeOptions.at(presentModeSetting));
    Config::setWindowHeight(windowHeightSetting);
    Config::setWindowWidth(windowWidthSetting);
    Config::setAllowHDR(hdrAllowedSetting);
    Config::setFsrEnabled(fsrEnabledSetting);
    Config::setRcasEnabled(rcasEnabledSetting);
    Config::setRcasAttenuation(static_cast<int>(rcasAttenuationSetting * 1000));

    /////////// Input Tab
    Config::setIsMotionControlsEnabled(motionControlsSetting);
    Config::setBackgroundControllerInput(backgroundControllerSetting);
    Config::setCursorState(cursorStateSetting);
    Config::setCursorHideTimeout(cursorTimeoutSetting);

    /////////// Trophy Tab
    Config::setisTrophyPopupDisabled(trophyPopupDisabledSetting);
    Config::setSideTrophy(trophySideOptions.at(trophySideSetting));
    Config::setTrophyNotificationDuration(static_cast<double>(trophyDurationSetting));

    /////////// Log Tab
    Config::setLoggingEnabled(logEnabledSetting);
    Config::setLogType(logTypeOptions.at(logTypeSetting));
    Config::setSeparateLogFilesEnabled(separateLogSetting);

    /////////// Experimental Tab
    Config::setReadbackSpeed(static_cast<Config::ReadbackSpeed>(readbacksModeSetting));
    Config::setReadbackLinearImages(readbackLinearImagesSetting);
    Config::setDirectMemoryAccess(directMemoryAccessSetting);
    Config::setDevKitMode(devkitConsoleSetting);
    Config::setNeoMode(neoModeSetting);
    Config::setPSNSignedIn(psnSignedInSetting);
    Config::setIsConnectedToNetwork(connectedNetworkSetting);
    Config::setPipelineCacheEnabled(pipelineCacheEnabledSetting);
    Config::setPipelineCacheArchived(pipelineCacheArchiveSetting);
    Config::setExtraDmemInMbytes(extraDmemSetting);
    Config::setVblankFreq(vblankFrequencySetting);

    // Save to appropriate config file
    if (isGameSpecific) {
        // Save to game-specific config file (customConfigs/gameSerial.toml)
        Config::save(profile, true);
    } else {
        // Save to global config file (config.toml)
        Config::save(std::filesystem::path{}, false);
    }
}

void LoadSettings(std::string profile) {
    const bool isSpecific = currentProfile != "Global";
    if (!isSpecific) {
        Config::load(std::filesystem::path{}, false);
    } else {
        Config::load(profile, true);
    }

    /////////// General Tab
    int languageIndex = Config::GetLanguage();
    std::string language;
    for (const auto& [key, value] : languageMap) {
        if (value == languageIndex) {
            language = key;
        }
    }
    consoleLanguageSetting = GetComboIndex(language, languageOptions);
    volumeSetting = Config::getVolumeSlider();
    showSplashSetting = Config::showSplash();
    audioBackendSetting = static_cast<int>(Config::getAudioBackend());

    /////////// Graphics Tab
    fullscreenModeSetting = GetComboIndex(Config::getFullscreenMode(), fullscreenModeOptions);
    presentModeSetting = GetComboIndex(Config::getPresentMode(), presentModeOptions);
    windowHeightSetting = Config::getWindowHeight();
    windowWidthSetting = Config::getWindowWidth();
    hdrAllowedSetting = Config::allowHDR();
    fsrEnabledSetting = Config::getFsrEnabled();
    rcasEnabledSetting = Config::getRcasEnabled();
    rcasAttenuationSetting = static_cast<float>(Config::getRcasAttenuation() * 0.001f);

    /////////// Input Tab
    motionControlsSetting = Config::getIsMotionControlsEnabled();
    backgroundControllerSetting = Config::getBackgroundControllerInput();
    cursorStateSetting = Config::getCursorState();
    cursorTimeoutSetting = Config::getCursorHideTimeout();

    /////////// Trophy Tab
    trophyPopupDisabledSetting = Config::getisTrophyPopupDisabled();
    trophySideSetting = GetComboIndex(Config::sideTrophy(), trophySideOptions);
    trophyDurationSetting = static_cast<float>(Config::getTrophyNotificationDuration());

    /////////// Log Tab
    logEnabledSetting = Config::getLoggingEnabled();
    logTypeSetting = GetComboIndex(Config::getLogType(), logTypeOptions);
    separateLogSetting = Config::getSeparateLogFilesEnabled();

    /////////// Experimental Tab
    readbacksModeSetting = static_cast<int>(Config::readbackSpeed());
    readbackLinearImagesSetting = Config::getReadbackLinearImages();
    directMemoryAccessSetting = Config::directMemoryAccess();
    devkitConsoleSetting = Config::isDevKitConsole();
    neoModeSetting = Config::isNeoModeConsole();
    psnSignedInSetting = Config::getPSNSignedIn();
    connectedNetworkSetting = Config::getIsConnectedToNetwork();
    pipelineCacheEnabledSetting = Config::isPipelineCacheEnabled();
    pipelineCacheArchiveSetting = Config::isPipelineCacheArchived();
    extraDmemSetting = Config::getExtraDmemInMbytes();
    vblankFrequencySetting = Config::vblankFreq();
}

void AddCategory(std::string name, SDL_Texture* texture, SettingsCategory category) {
    ImGui::SameLine();
    ImGui::BeginGroup();

    // make button appear hovered as long as category is selected, otherwise dull it's hovered color
    currentCategory == category
        ? ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered])
        : ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.235f, 0.392f, 0.624f, 1.00f));

    if (ImGui::ImageButton(name.c_str(), ImTextureID(texture),
                           ImVec2(settingsIconSize * uiScale, settingsIconSize * uiScale))) {
        currentCategory = category;
    }

    ImGui::PopStyleColor();

    ImGui::SetCursorPosX(
        (ImGui::GetCursorPosX() +
         (settingsIconSize * uiScale - ImGui::CalcTextSize(name.c_str()).x) * 0.5f) +
        ImGui::GetStyle().FramePadding.x);
    ImGui::Text("%s", name.c_str());
    ImGui::EndGroup();
}

void AddSettingBool(std::string name, bool& value) {
    std::string label = "##" + name;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    ImGui::TextWrapped("%s", name.c_str());
    ImGui::TableNextColumn();
    ImGui::Checkbox(label.c_str(), &value);
}

void AddSettingSliderInt(std::string name, int& value, int min, int max) {
    std::string label = "##" + name;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", name.c_str());

    ImGui::TableNextColumn();
    ImGui::SliderInt(label.c_str(), &value, min, max);
}

void AddSettingSliderFloat(std::string name, float& value, int min, int max, int precision) {
    std::string label = "##" + name;
    std::string precisionString = "%." + std::to_string(precision) + "f";

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", name.c_str());

    ImGui::TableNextColumn();
    ImGui::SliderFloat(label.c_str(), &value, min, max, precisionString.c_str());
}

void AddSettingCombo(std::string name, int& value, std::vector<std::string> options) {
    std::string label = "##" + name;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", name.c_str());

    ImGui::TableNextColumn();
    const char* combo_value = options[value].c_str();
    if (ImGui::BeginCombo(label.c_str(), combo_value)) {
        for (int i = 0; i < options.size(); i++) {
            const bool selected = (i == value);
            if (ImGui::Selectable(options[i].c_str(), selected))
                value = i;

            // Set the initial focus when opening the combo
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

int GetComboIndex(std::string selection, std::vector<std::string> options) {
    for (int i = 0; i < options.size(); i++) {
        if (selection == options[i])
            return i;
    }

    return 0;
}

void LoadEmbeddedTexture(std::string resourcePath, SDL_Texture*& texture) {
    auto resource = cmrc::res::get_filesystem();
    auto file = resource.open(resourcePath);
    std::vector<char> texData = std::vector<char>(file.begin(), file.end());

    BigPictureMode::LoadTextureData(texData, texture, renderer);
}

void SetProfileIcons(std::vector<Game>& games) {
    ImGuiStyle& style = ImGui::GetStyle();
    const float maxAvailableWidth = ImGui::GetContentRegionAvail().x;
    const float itemSpacing = style.ItemSpacing.x; // already scaled
    const float padding = 10.0f * uiScale;
    float rowContentWidth = gameImageSize * uiScale + itemSpacing;

    for (int i = 0; i < games.size(); i++) {
        ImGui::BeginGroup();
        std::string ButtonName = "Button" + std::to_string(i);
        const char* ButtonNameChar = ButtonName.c_str();

        bool isNextItemFocused = (ImGui::GetID(ButtonNameChar) == ImGui::GetFocusID());
        bool popColor = false;
        if (isNextItemFocused) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
            popColor = true;
        }

        if (ImGui::ImageButton(ButtonNameChar, (ImTextureID)games[i].iconTexture,
                               ImVec2(gameImageSize * uiScale, gameImageSize * uiScale))) {
            currentProfile = i == 0 ? "Global" : games[i].serial + " - " + games[i].title;
            LoadSettings(games[i].serial);
        }

        if (popColor) {
            ImGui::PopStyleColor();
        }

        // Scroll to item only when newly-focused
        if (ImGui::IsItemFocused() && !games[i].focusState) {
            ImGui::SetScrollHereY(0.5f);
        }

        if (ImGui::IsWindowFocused()) {
            games[i].focusState = ImGui::IsItemFocused();
        }

        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + gameImageSize * uiScale);
        ImGui::TextWrapped("%s", games[i].title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        // Use same line if content fits horizontally, move to next line if not
        rowContentWidth += (gameImageSize * uiScale + itemSpacing * 2 + padding);
        if (rowContentWidth < maxAvailableWidth) {
            ImGui::SameLine(0.0f, padding);
        } else {
            ImGui::Dummy(ImVec2(0.0f, padding));
            rowContentWidth = gameImageSize * uiScale + itemSpacing;
        }
    }
}

} // namespace BigPictureMode
