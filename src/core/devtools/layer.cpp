// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "layer.h"

#include <SDL3/SDL_events.h>
#include <emulator.h>
#include <imgui.h>
#include "SDL3/SDL_events.h"

#include "SDL3/SDL_log.h"
#include "common/config.h"
#include "common/singleton.h"
#include "common/types.h"
#include "core/debug_state.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/videoout/video_out.h"
#include "imgui/imgui_std.h"
#include "imgui_internal.h"
#include "input/input_handler.h"
#include "options.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "widget/frame_dump.h"
#include "widget/frame_graph.h"
#include "widget/memory_map.h"
#include "widget/module_list.h"
#include "widget/shader_list.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
using Btn = Libraries::Pad::OrbisPadButtonDataOffset;

using namespace ImGui;
using namespace ::Core::Devtools;
using L = ::Core::Devtools::Layer;

static bool show_simple_fps = false;
static bool visibility_toggled = false;
static bool show_quit_window = false;

static bool show_fullscreen_tip = true;
static float fullscreen_tip_timer = 10.0f;

static float fps_scale = 1.0f;
static int dump_frame_count = 1;

static Widget::FrameGraph frame_graph;
static std::vector<Widget::FrameDumpViewer> frame_viewers;

static float debug_popup_timing = 3.0f;

static bool just_opened_options = false;

static Widget::MemoryMapViewer memory_map;
static Widget::ShaderList shader_list;
static Widget::ModuleList module_list;

// clang-format off
static std::string help_text =
#include "help.txt"
    ;
// clang-format on

void L::DrawMenuBar() {
    const auto& ctx = *GImGui;
    const auto& io = ctx.IO;

    auto isSystemPaused = DebugState.IsGuestThreadsPaused();

    bool open_popup_options = false;
    bool open_popup_help = false;

    if (BeginMainMenuBar()) {
        if (BeginMenu("Options")) {
            if (MenuItemEx("Emulator Paused", nullptr, nullptr, isSystemPaused)) {
                if (isSystemPaused) {
                    DebugState.ResumeGuestThreads();
                } else {
                    DebugState.PauseGuestThreads();
                }
            }
            ImGui::EndMenu();
        }
        if (BeginMenu("GPU Tools")) {
            MenuItem("Show frame info", nullptr, &frame_graph.is_open);
            MenuItem("Show loaded shaders", nullptr, &shader_list.open);
            if (BeginMenu("Dump frames")) {
                SliderInt("Count", &dump_frame_count, 1, 5);
                if (MenuItem("Dump", "Ctrl+Alt+F9", nullptr, !DebugState.DumpingCurrentFrame())) {
                    DebugState.RequestFrameDump(dump_frame_count);
                }
                ImGui::EndMenu();
            }
            open_popup_options = MenuItem("Options");
            open_popup_help = MenuItem("Help & Tips");
            ImGui::EndMenu();
        }
        if (BeginMenu("Display")) {
            auto& pp_settings = presenter->GetPPSettingsRef();
            if (BeginMenu("Brightness")) {
                SliderFloat("Gamma", &pp_settings.gamma, 0.1f, 2.0f);
                ImGui::EndMenu();
            }
            if (BeginMenu("FSR")) {
                auto& fsr = presenter->GetFsrSettingsRef();
                Checkbox("FSR Enabled", &fsr.enable);
                BeginDisabled(!fsr.enable);
                {
                    Checkbox("RCAS", &fsr.use_rcas);
                    BeginDisabled(!fsr.use_rcas);
                    {
                        SliderFloat("RCAS Attenuation", &fsr.rcas_attenuation, 0.0, 3.0);
                    }
                    EndDisabled();
                }
                EndDisabled();
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (BeginMenu("Debug")) {
            if (MenuItem("Memory map")) {
                memory_map.open = true;
            }
            if (MenuItem("Module list")) {
                module_list.open = true;
            }
            ImGui::EndMenu();
        }

        SameLine(ImGui::GetWindowWidth() - 30.0f);
        if (Button("X", ImVec2(25, 25))) {
            DebugState.IsShowingDebugMenuBar() = false;
        }

        EndMainMenuBar();
    }
    if (open_popup_options) {
        OpenPopup("GPU Tools Options");
        just_opened_options = true;
    }
    if (open_popup_help) {
        OpenPopup("HelpTips");
    }
}

void L::DrawAdvanced() {
    DrawMenuBar();

    const auto& ctx = *GImGui;
    const auto& io = ctx.IO;

    frame_graph.Draw();

    if (DebugState.should_show_frame_dump && DebugState.waiting_reg_dumps.empty()) {
        DebugState.should_show_frame_dump = false;
        std::unique_lock lock{DebugState.frame_dump_list_mutex};
        while (!DebugState.frame_dump_list.empty()) {
            const auto& frame_dump = DebugState.frame_dump_list.back();
            frame_viewers.emplace_back(frame_dump);
            DebugState.frame_dump_list.pop_back();
        }
        static bool first_time = true;
        if (first_time) {
            first_time = false;
            DebugState.ShowDebugMessage("Tip: You can shift+click any\n"
                                        "popup to open a new window");
        }
    }

    for (auto it = frame_viewers.begin(); it != frame_viewers.end();) {
        if (it->is_open) {
            it->Draw();
            ++it;
        } else {
            it = frame_viewers.erase(it);
        }
    }

    if (!DebugState.debug_message_popup.empty()) {
        if (debug_popup_timing > 0.0f) {
            debug_popup_timing -= io.DeltaTime;
            if (Begin("##devtools_msg", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove)) {
                BringWindowToDisplayFront(GetCurrentWindow());
                const auto display_size = io.DisplaySize;
                const auto& msg = DebugState.debug_message_popup.front();
                const auto padding = GetStyle().WindowPadding;
                const auto txt_size = CalcTextSize(&msg.front(), &msg.back() + 1, false, 250.0f);
                SetWindowPos({display_size.x - padding.x * 2.0f - txt_size.x, 50.0f});
                SetWindowSize({txt_size.x + padding.x * 2.0f, txt_size.y + padding.y * 2.0f});
                PushTextWrapPos(250.0f);
                TextEx(&msg.front(), &msg.back() + 1);
                PopTextWrapPos();
            }
            End();
        } else {
            DebugState.debug_message_popup.pop();
            debug_popup_timing = 3.0f;
        }
    }

    bool close_popup_options = true;
    if (BeginPopupModal("GPU Tools Options", &close_popup_options,
                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        static char disassembler_cli_isa[512];
        static char disassembler_cli_spv[512];
        static bool frame_dump_render_on_collapse;

        if (just_opened_options) {
            just_opened_options = false;
            auto s = Options.disassembler_cli_isa.copy(disassembler_cli_isa,
                                                       sizeof(disassembler_cli_isa) - 1);
            disassembler_cli_isa[s] = '\0';
            s = Options.disassembler_cli_spv.copy(disassembler_cli_spv,
                                                  sizeof(disassembler_cli_spv) - 1);
            disassembler_cli_spv[s] = '\0';
            frame_dump_render_on_collapse = Options.frame_dump_render_on_collapse;
        }

        InputText("Shader isa disassembler: ", disassembler_cli_isa, sizeof(disassembler_cli_isa));
        if (IsItemHovered()) {
            SetTooltip(R"(Command to disassemble shaders. Example: dis.exe --raw "{src}")");
        }
        InputText("Shader SPIRV disassembler: ", disassembler_cli_spv,
                  sizeof(disassembler_cli_spv));
        if (IsItemHovered()) {
            SetTooltip(R"(Command to disassemble shaders. Example: spirv-cross -V "{src}")");
        }
        Checkbox("Show frame dump popups even when collapsed", &frame_dump_render_on_collapse);
        if (IsItemHovered()) {
            SetTooltip("When a frame dump is collapsed, it will keep\n"
                       "showing all opened popups related to it");
        }

        if (Button("Save")) {
            Options.disassembler_cli_isa = disassembler_cli_isa;
            Options.disassembler_cli_spv = disassembler_cli_spv;
            Options.frame_dump_render_on_collapse = frame_dump_render_on_collapse;
            SaveIniSettingsToDisk(io.IniFilename);
            CloseCurrentPopup();
        }
        EndPopup();
    }

    if (BeginPopup("HelpTips", ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
        CentralizeWindow();

        PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10.0f});
        PushTextWrapPos(600.0f);

        const char* begin = help_text.data();
        TextUnformatted(begin, begin + help_text.size());

        PopTextWrapPos();
        PopStyleVar();

        EndPopup();
    }

    if (memory_map.open) {
        memory_map.Draw();
    }
    if (shader_list.open) {
        shader_list.Draw();
    }
    if (module_list.open) {
        module_list.Draw();
    }
}

void L::DrawSimple() {
    const float frameRate = DebugState.Framerate;
    if (Config::fpsColor()) {
        if (frameRate < 10) {
            PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f)); // Red
        } else if (frameRate >= 10 && frameRate < 20) {
            PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.0f, 1.0f)); // Orange
        } else {
            PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f)); // White
        }
    } else {
        PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f)); // White
    }
    Text("%d FPS (%.1f ms)", static_cast<int>(std::round(frameRate)), 1000.0f / frameRate);
    PopStyleColor();
}

static void LoadSettings(const char* line) {
    int i;
    float f;
    if (sscanf(line, "fps_scale=%f", &f) == 1) {
        fps_scale = f;
        return;
    }
    if (sscanf(line, "show_advanced_debug=%d", &i) == 1) {
        DebugState.IsShowingDebugMenuBar() = i != 0;
        return;
    }
    if (sscanf(line, "show_frame_graph=%d", &i) == 1) {
        frame_graph.is_open = i != 0;
        return;
    }
    if (sscanf(line, "dump_frame_count=%d", &i) == 1) {
        dump_frame_count = i;
        return;
    }
}

void L::SetupSettings() {
    frame_graph.is_open = true;

    using SettingLoader = void (*)(const char*);

    ImGuiSettingsHandler handler{};
    handler.TypeName = "DevtoolsLayer";
    handler.TypeHash = ImHashStr(handler.TypeName);
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
        if (std::string_view("Data") == name) {
            static_assert(std::is_same_v<decltype(&LoadSettings), SettingLoader>);
            return (void*)&LoadSettings;
        }
        if (std::string_view("CmdList") == name) {
            static_assert(
                std::is_same_v<decltype(&Widget::CmdListViewer::LoadConfig), SettingLoader>);
            return (void*)&Widget::CmdListViewer::LoadConfig;
        }
        if (std::string_view("Options") == name) {
            static_assert(std::is_same_v<decltype(&LoadOptionsConfig), SettingLoader>);
            return (void*)&LoadOptionsConfig;
        }
        return (void*)nullptr;
    };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* handle, const char* line) {
        if (handle != nullptr) {
            reinterpret_cast<SettingLoader>(handle)(line);
        }
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
        buf->appendf("[%s][Data]\n", handler->TypeName);
        buf->appendf("fps_scale=%f\n", fps_scale);
        buf->appendf("show_advanced_debug=%d\n", DebugState.IsShowingDebugMenuBar());
        buf->appendf("show_frame_graph=%d\n", frame_graph.is_open);
        buf->appendf("dump_frame_count=%d\n", dump_frame_count);
        buf->append("\n");
        buf->appendf("[%s][CmdList]\n", handler->TypeName);
        Widget::CmdListViewer::SerializeConfig(buf);
        buf->append("\n");
        buf->appendf("[%s][Options]\n", handler->TypeName);
        SerializeOptionsConfig(buf);
        buf->append("\n");
    };
    AddSettingsHandler(&handler);

    const ImGuiID dock_id = ImHashStr("FrameDumpDock");
    DockBuilderAddNode(dock_id, 0);
    DockBuilderSetNodePos(dock_id, ImVec2{450.0, 150.0});
    DockBuilderSetNodeSize(dock_id, ImVec2{400.0, 500.0});
    DockBuilderFinish(dock_id);
}

void DrawFullscreenTipWindow(bool& is_open, float& fullscreen_tip_timer) {
    if (!is_open)
        return;

    constexpr ImVec2 window_pos = {10, 10};
    constexpr ImVec2 window_size = {325, 375};

    ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.9f);

    if (ImGui::Begin("Fullscreen Tip", &is_open,
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavInputs |
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextUnformatted("Pause/Resume the emulator with: F9 or L2+R2+Options\n"
                               "Stop the game with: F4 or L2+L2+Share/Back/Select\n"
                               "Toggle fullscreen: F11 or L2+R2+R3\n"
                               "Developer Tools: Ctrl + F10 or L2+R2+Square\n"
                               "Show FPS: F10 or L2+R2+L3\n");

        ImGui::Spacing();
        ImGui::Separator();

        if (Config::getIsConnectedToNetwork()) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Network: Connected");
        } else {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Network: Disconnected");
        }

        ImGui::TextDisabled("NOTE: CHEATS AREN'T WORKING RIGHT NOW");

        ImGui::Spacing();
        ImGui::SeparatorText("Current Config");

        ImGui::Text("Shader Skips: %s", Config::getShaderSkipsEnabled() ? "On" : "Off");
        ImGui::Text("Linear Readbacks: %s", Config::getReadbackLinearImages() ? "On" : "Off");
        ImGui::Text("DMA Access: %s", Config::directMemoryAccess() ? "On" : "Off");
        ImGui::Text("VBlank Divider: %d", Config::vblankDiv());
        ImGui::Text("HDR Allowed: %s", Config::allowHDR() ? "Yes" : "No");
        ImGui::Text("Auto Backup: %s", Config::getEnableAutoBackup() ? "On" : "Off");
        ImGui::Text("PSN Signed In: %s", Config::getPSNSignedIn() ? "Yes" : "No");
        ImGui::Text("LogType: %s", Config::getLogType().c_str());
        const char* readbackaccuStr = "Unknown";
        switch (Config::readbackSpeed()) {
        case Config::ReadbackSpeed::Unsafe:
            readbackaccuStr = "Unsafe";
            break;
        case Config::ReadbackSpeed::Low:
            readbackaccuStr = "Low";
            break;
        case Config::ReadbackSpeed::Fast:
            readbackaccuStr = "Fast";
            break;
        case Config::ReadbackSpeed::Default:
            readbackaccuStr = "Default";
            break;
        }
        ImGui::Text("Readbacks Speed: %s", readbackaccuStr);
    }
    ImGui::End();
}

void DrawPauseStatusWindow(bool& is_open) {
    if (!is_open)
        return;

    constexpr ImVec2 window_size = {600, 500};
    ImGui::SetNextWindowSize(window_size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.9f);

    if (ImGui::Begin("Pause Menu - Hotkeys", &is_open,
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavInputs)) {
        ImGui::TextUnformatted("Pause/Resume the emulator with: F9 or L2+R2+Options\n"
                               "Stop the game with: F4 or L2+L2+Share/Back/Select\n"
                               "Toggle fullscreen: F11 or L2+R2+R3\n"
                               "Developer Tools: Ctrl + F10 or L2+R2+Square\n"
                               "Show FPS: F10 or L2+R2+L3\n");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Tip: Use the keyboard or controller shortcuts above.");

        ImGui::Spacing();
        ImGui::SeparatorText("Network Status");

        if (Config::getIsConnectedToNetwork()) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Network: Connected");
        } else {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Network: Disconnected");
        }

        static bool network_connected = Config::getIsConnectedToNetwork();
        if (ImGui::Checkbox("Set Network Connected", &network_connected)) {
            Config::setIsConnectedToNetwork(network_connected);
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Edit Configurations");

        if (ImGui::Checkbox("Show Fullscreen Tip", &show_fullscreen_tip)) {
            if (show_fullscreen_tip)
                fullscreen_tip_timer = 10.0f;
        }

        bool ss = Config::getShaderSkipsEnabled();
        if (ImGui::Checkbox("Shader Skips", &ss))
            Config::setShaderSkipsEnabled(ss);

        bool lr = Config::getReadbackLinearImages();
        if (ImGui::Checkbox("Linear Readbacks", &lr))
            Config::setReadbackLinearImages(lr);

        bool dma = Config::directMemoryAccess();
        if (ImGui::Checkbox("DMA Access", &dma))
            Config::setDirectMemoryAccess(dma);

        int vblank = Config::vblankDiv();
        if (ImGui::SliderInt("VBlank Divider", &vblank, 1, 100))
            Config::setVblankDiv(vblank);

        bool hdr = Config::allowHDR();
        if (ImGui::Checkbox("HDR Allowed", &hdr))
            Config::setAllowHDR(hdr);

        bool autobackup = Config::getEnableAutoBackup();
        if (ImGui::Checkbox("Auto Backup", &autobackup))
            Config::setEnableAutoBackup(autobackup);

        bool psn = Config::getPSNSignedIn();
        if (ImGui::Checkbox("PSN Signed In", &psn))
            Config::setPSNSignedIn(psn);

        float volume = Config::getVolumeLevel();
        if (ImGui::SliderFloat("Volume", &volume, 0.0f, 3.0f)) {
            Config::setVolumeLevel(volume);
        }

        static const char* logTypes[] = {"sync", "async"};
        int logTypeIndex = 0;
        for (int i = 0; i < IM_ARRAYSIZE(logTypes); i++) {
            if (Config::getLogType() == logTypes[i]) {
                logTypeIndex = i;
                break;
            }
        }
        if (ImGui::Combo("Log Type", &logTypeIndex, logTypes, IM_ARRAYSIZE(logTypes)))
            Config::setLogType(logTypes[logTypeIndex]);

        static const char* readbackAccuracyStrs[] = {"Unsafe", "Low", "Fast", "Default"};
        int readbackAccIndex = (int)Config::readbackSpeed();
        if (ImGui::Combo("Readbacks Speed", &readbackAccIndex, readbackAccuracyStrs,
                         IM_ARRAYSIZE(readbackAccuracyStrs)))
            Config::setReadbackSpeed((Config::ReadbackSpeed)readbackAccIndex);

        ImGui::Spacing();
        ImGui::Separator();

        if (ImGui::Button("Save")) {
            const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
            Config::save(config_dir / "config.toml");
            DebugState.ResumeGuestThreads();
        };
    }
    ImGui::End();
}

void L::Draw() {
    const auto io = GetIO();
    PushID("DevtoolsLayer");

    if (IsKeyPressed(ImGuiKey_F4, false)) {
        show_quit_window = true;
    }

    if (IsKeyPressed(ImGuiKey_F9, false)) {
        if (io.KeyCtrl && io.KeyAlt) {
            if (!DebugState.ShouldPauseInSubmit()) {
                DebugState.RequestFrameDump(dump_frame_count);
            }
        } else {
            if (DebugState.IsGuestThreadsPaused()) {
                DebugState.ResumeGuestThreads();
                SDL_Log("Game resumed from Keyboard");
                show_pause_status = false;
            } else {
                DebugState.PauseGuestThreads();
                SDL_Log("Game paused from Keyboard");
                show_pause_status = true;
            }
            visibility_toggled = true;
        }
    }

    const bool key_f10 = ImGui::IsKeyPressed(ImGuiKey_F10, false);
    const bool ctrl_held = io.KeyCtrl;

    if ((key_f10 && ctrl_held)) {
        DebugState.IsShowingDebugMenuBar() ^= true;
        visibility_toggled = true;
    } else if (key_f10) {
        show_simple_fps = !show_simple_fps;
        visibility_toggled = true;
    }

    if (!Input::HasUserHotkeyDefined(Input::HotkeyPad::SimpleFpsPad)) {
        if (Input::ControllerComboPressedOnce({Btn::L2, Btn::R2, Btn::L3})) {
            show_simple_fps = !show_simple_fps;
            visibility_toggled = true;
        }
    }

    if (!Input::HasUserHotkeyDefined(Input::HotkeyPad::FullscreenPad)) {
        if (Input::ControllerComboPressedOnce({Btn::L2, Btn::R2, Btn::R3})) {
            SDL_Event toggleFullscreenEvent;
            toggleFullscreenEvent.type = SDL_EVENT_TOGGLE_FULLSCREEN;
            SDL_PushEvent(&toggleFullscreenEvent);
        }
    }

    if (!Input::HasUserHotkeyDefined(Input::HotkeyPad::PausePad)) {
        if (Input::ControllerComboPressedOnce({Btn::L2, Btn::R2, Btn::Options})) {
            if (DebugState.IsGuestThreadsPaused()) {
                DebugState.ResumeGuestThreads();
                SDL_Log("Game resumed from Controller");
                show_pause_status = false;
            } else {
                DebugState.PauseGuestThreads();
                SDL_Log("Game paused from Controller");
                show_pause_status = true;
            }
            visibility_toggled = true;
        }
    }

    if (!Input::HasUserHotkeyDefined(Input::HotkeyPad::QuitPad)) {
        if (Input::ControllerComboPressedOnce({Btn::L2, Btn::R2, Btn::TouchPad})) {
            show_quit_window = true;
        }
    }

    const bool show_debug_menu_combo =
        Input::ControllerComboPressedOnce({Btn::L2, Btn::R2, Btn::Square});

    if (!Input::HasUserHotkeyDefined(Input::HotkeyPad::DebugMenuPad)) {
        if ((key_f10 && ctrl_held) || show_debug_menu_combo) {
            DebugState.IsShowingDebugMenuBar() ^= true;
            visibility_toggled = true;
        }
    }

    if (!DebugState.IsGuestThreadsPaused()) {
        const auto fn = DebugState.flip_frame_count.load();
        frame_graph.AddFrame(fn, DebugState.FrameDeltaTime);
    }

    if (show_fullscreen_tip) {
        fullscreen_tip_timer -= io.DeltaTime;
        if (fullscreen_tip_timer <= 0.0f) {
            show_fullscreen_tip = false;
        } else {
            DrawFullscreenTipWindow(show_fullscreen_tip, fullscreen_tip_timer);
        }
    }

    static bool showPauseHelpWindow = true;

    if (DebugState.IsGuestThreadsPaused()) {
        DrawPauseStatusWindow(showPauseHelpWindow);
    }

    if (show_simple_fps) {
        if (Begin("Video Info", nullptr,
                  ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
            // Set window position to top left if it was toggled on
            if (visibility_toggled) {
                SetWindowPos("Video Info", {999999.0f, 0.0f}, ImGuiCond_Always);
                visibility_toggled = false;
            }
            if (BeginPopupContextWindow()) {
#define M(label, value)                                                                            \
    if (MenuItem(label, nullptr, fps_scale == value))                                              \
    fps_scale = value
                M("0.5x", 0.5f);
                M("1.0x", 1.0f);
                M("1.5x", 1.5f);
                M("2.0x", 2.0f);
                M("2.5x", 2.5f);
                EndPopup();
#undef M
            }
            KeepWindowInside();
            SetWindowFontScale(fps_scale);
            DrawSimple();
        }
        End();
    }

    if (DebugState.IsShowingDebugMenuBar()) {
        PushFont(io.Fonts->Fonts[IMGUI_FONT_MONO]);
        PushID("DevtoolsLayer");
        DrawAdvanced();
        PopID();
        PopFont();
    }

    if (show_quit_window) {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (Begin("Quit Notification", nullptr,
                  ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
            SetWindowFontScale(1.5f);
            TextCentered("Are you sure you want to quit?");
            NewLine();
            Text("Press Escape or Circle/B button to cancel");
            Text("Press Enter or Cross/A button to quit");

            if (IsKeyPressed(ImGuiKey_Escape, false) ||
                (IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
                show_quit_window = false;
            }

            if (IsKeyPressed(ImGuiKey_Enter, false) ||
                (IsKeyPressed(ImGuiKey_GamepadFaceDown, false))) {
                SDL_Event event;
                SDL_memset(&event, 0, sizeof(event));
                event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&event);
            }
        }
        End();
    }

    PopID();
}

void L::TextCentered(const std::string& text) {
    float window_width = ImGui::GetWindowSize().x;
    float text_width = ImGui::CalcTextSize(text.c_str()).x;
    float text_indentation = (window_width - text_width) * 0.5f;

    ImGui::SameLine(text_indentation);
    ImGui::Text("%s", text.c_str());
}

namespace Overlay {

void ToggleSimpleFps() {
    show_simple_fps = !show_simple_fps;
    visibility_toggled = true;
}

void ToggleQuitWindow() {
    show_quit_window = !show_quit_window;
}

} // namespace Overlay
