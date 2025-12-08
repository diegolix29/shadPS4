// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <filesystem>
#include <string>

#include "imgui/imgui_layer.h"

namespace Core::Devtools {

class Layer final : public ImGui::Layer {

    static void DrawMenuBar();

    static void DrawAdvanced();

    static void DrawSimple();

public:
    static void SetupSettings();
    void SaveConfigWithOverrides(const std::filesystem::path& path, bool perGame,
                                 const std::string& gameSerial);
    void Draw();
    void DrawPauseStatusWindow(bool& is_open);
    bool show_pause_status = false;
    void TextCentered(const std::string& text);
    static void DrawNullGpuNotice();
};

} // namespace Core::Devtools

namespace Overlay {

void ToggleSimpleFps();
void SetSimpleFps(bool enabled);
void ToggleQuitWindow();
void TogglePauseWindow();
} // namespace Overlay
