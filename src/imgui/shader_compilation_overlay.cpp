// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <fmt/format.h>
#include <imgui.h>
#include "shader_compilation_overlay.h"

namespace ImGui {

ShaderCompilationOverlay::ShaderCompilationOverlay() {
    m_start_time = std::chrono::steady_clock::now();
}

ShaderCompilationOverlay::~ShaderCompilationOverlay() = default;

void ShaderCompilationOverlay::DrawSpinningCircle(float radius, float progress) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 center = ImGui::GetCursorScreenPos() + ImVec2(radius, radius);

    draw_list->AddCircle(center, radius, IM_COL32(60, 60, 60, 180), 0, 1.5f);

    const int num_segments = 20;
    const float arc_angle = 2.0f * 3.14159f * 0.75f;
    const float start_angle = m_animation_time * 2.0f;

    ImVec2 arc_points[num_segments + 1];
    for (int i = 0; i <= num_segments; ++i) {
        float angle = start_angle + (float)i / num_segments * arc_angle;
        arc_points[i] = ImVec2(center.x + cos(angle) * radius, center.y + sin(angle) * radius);
    }

    draw_list->AddPolyline(arc_points, num_segments + 1, IM_COL32(100, 200, 255, 255), false, 2.0f);

    draw_list->AddCircleFilled(center, radius * 0.25f, IM_COL32(40, 40, 40, 200));
}

void ShaderCompilationOverlay::SetVisible(bool visible) {
    m_visible = visible;
}

void ShaderCompilationOverlay::SetCompiling(bool compiling) {
    m_compiling = compiling;
    if (compiling) {
        m_start_time = std::chrono::steady_clock::now();
    }
}

void ShaderCompilationOverlay::SetProgress(int current, int total) {
    m_current = current;
    m_total = total;

    std::lock_guard<std::mutex> lock(m_status_mutex);
    if (total > 0) {
        float percentage = (float)current / total * 100.0f;
        m_status_text = fmt::format("Compiling: {}/{} ({:.1f}%)", current, total, percentage);
    } else {
        m_status_text = "Compiling...";
    }
}

void ShaderCompilationOverlay::Draw() {
    if (!m_visible.load() || !m_compiling.load()) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    m_animation_time = std::chrono::duration<float>(now - m_start_time).count();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 work_pos = viewport->WorkPos;
    ImVec2 work_size = viewport->WorkSize;

    std::string status_text_copy;
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        status_text_copy = m_status_text;
    }

    float text_width = ImGui::CalcTextSize(status_text_copy.c_str()).x;
    float content_width = 20.0f + 16.0f + text_width;
    float expected_width = content_width + 20.0f;
    float expected_height = 36.0f;

    ImVec2 window_pos =
        ImVec2(work_pos.x + work_size.x - expected_width - 10.0f, work_pos.y + 10.0f);

    ImGui::SetNextWindowPos(window_pos);
    ImGui::SetNextWindowSize(ImVec2(expected_width, expected_height));

    if (ImGui::Begin("##ShaderCompilationOverlay", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 window_min = ImGui::GetWindowPos();
        ImVec2 window_max = window_min + ImGui::GetWindowSize();
        draw_list->AddRect(window_min + ImVec2(2, 2), window_max - ImVec2(2, 2),
                           IM_COL32(80, 80, 80, 100), 4.0f, 0, 1.0f);

        float window_width = ImGui::GetWindowSize().x;
        float window_height = ImGui::GetWindowSize().y;
        float content_width = 20.0f + 16.0f + ImGui::CalcTextSize(status_text_copy.c_str()).x;
        float start_x = (window_width - content_width) / 2.0f;
        float start_y = (window_height - 20.0f) / 2.0f;

        ImGui::SetCursorPos(ImVec2(start_x, start_y));

        int current = m_current.load();
        int total = m_total.load();
        DrawSpinningCircle(10.0f, total > 0 ? (float)current / total : 0.0f);

        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
        ImGui::Text("%s", status_text_copy.c_str());
    }

    ImGui::End();
}

} // namespace ImGui
