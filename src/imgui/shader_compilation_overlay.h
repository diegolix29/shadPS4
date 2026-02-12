// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "imgui_layer.h"
#include <string>
#include <chrono>
#include <atomic>
#include <mutex>

namespace ImGui {

class ShaderCompilationOverlay : public Layer {
public:
    ShaderCompilationOverlay();
    ~ShaderCompilationOverlay();

    void Draw() override;
    void SetVisible(bool visible);
    void SetProgress(int current, int total);
    void SetCompiling(bool compiling);

private:
    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_compiling{false};
    std::atomic<int> m_current{0};
    std::atomic<int> m_total{0};
    std::string m_status_text;
    float m_animation_time = 0.0f;
    std::chrono::steady_clock::time_point m_start_time;
    std::mutex m_status_mutex;
    
    void DrawSpinningCircle(float radius, float progress);
};

} // namespace ImGui
