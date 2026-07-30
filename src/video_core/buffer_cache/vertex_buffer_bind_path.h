// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace VideoCore {

enum class VertexBufferBindPath {
    Core,
    Extended,
};

[[nodiscard]] constexpr VertexBufferBindPath
SelectVertexBufferBindPath(bool vertex_input_dynamic_state) {
    return vertex_input_dynamic_state ? VertexBufferBindPath::Core
                                      : VertexBufferBindPath::Extended;
}

} // namespace VideoCore
