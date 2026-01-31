// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {

void TriggerScreenshot();

bool ConsumeScreenshotRequest();

bool CaptureScreenshot(Vulkan::Rasterizer& rasterizer, const std::filesystem::path& output_dir = {},
                       const std::string& filename = "");

} // namespace VideoCore
