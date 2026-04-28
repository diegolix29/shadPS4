// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "screenshot.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;

// stb_image_write (implementation compiled in stb_jpeg_impl.cpp)
#include "core/libraries/jpeg/third_party/stb_image_write.h"

namespace Libraries::ScreenShot {

namespace {

struct MemWriter {
    std::vector<u8>* buf;
};

void StbiWriteCb(void* context, void* data, int size) {
    auto* w = static_cast<MemWriter*>(context);
    const auto* src = static_cast<const u8*>(data);
    w->buf->insert(w->buf->end(), src, src + size);
}

} // namespace

int PS4_SYSV_ABI _Z5dummyv() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotCapture() {
    LOG_INFO(Lib_Screenshot, "sceScreenShotCapture called - capturing presenter frame");

    if (!presenter) {
        LOG_ERROR(Lib_Screenshot, "sceScreenShotCapture: presenter not available");
        return ORBIS_OK;
    }

    std::vector<u8> pixels;
    u32 width = 0, height = 0;

    if (!presenter->CaptureScreenshot(pixels, width, height)) {
        LOG_ERROR(Lib_Screenshot, "sceScreenShotCapture: frame capture failed");
        return ORBIS_OK;
    }

    // Encode as JPEG
    std::vector<u8> jpeg;
    jpeg.reserve(width * height);
    MemWriter writer{&jpeg};

    // The frame is BGRA from the swapchain. Convert to RGB for JPEG encoding.
    std::vector<u8> rgb(static_cast<size_t>(width) * height * 3);
    for (u64 i = 0; i < static_cast<u64>(width) * height; i++) {
        rgb[i * 3 + 0] = pixels[i * 4 + 0]; // R (or B depending on swapchain format)
        rgb[i * 3 + 1] = pixels[i * 4 + 1]; // G
        rgb[i * 3 + 2] = pixels[i * 4 + 2]; // B (or R)
    }

    if (!stbi_write_jpg_to_func(StbiWriteCb, &writer, width, height, 3, rgb.data(), 92)) {
        LOG_ERROR(Lib_Screenshot, "sceScreenShotCapture: JPEG encode failed");
        return ORBIS_OK;
    }

    // Save to screenshots directory
    std::error_code ec;
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir);
    std::filesystem::create_directories(dir, ec);

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    char filename[128];
    std::snprintf(filename, sizeof(filename), "screenshot_%lld.jpg",
                  static_cast<long long>(now_ms));

    const auto path = dir / filename;
    std::ofstream out(path, std::ios::binary);
    if (out) {
        out.write(reinterpret_cast<const char*>(jpeg.data()),
                  static_cast<std::streamsize>(jpeg.size()));
        LOG_INFO(Lib_Screenshot, "sceScreenShotCapture: saved {}x{} screenshot ({} bytes) -> {}",
                 width, height, jpeg.size(), path.string());
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotDisable() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotDisableNotification() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotEnable() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotEnableNotification() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotGetAppInfo() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotGetDrcParam() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotIsDisabled() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotIsVshScreenCaptureDisabled() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotSetOverlayImage() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotSetOverlayImageWithOrigin() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotSetParam() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceScreenShotSetDrcParam() {
    LOG_ERROR(Lib_Screenshot, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("AS45QoYHjc4", "libSceScreenShot", 1, "libSceScreenShot", _Z5dummyv);
    LIB_FUNCTION("JuMLLmmvRgk", "libSceScreenShot", 1, "libSceScreenShot", sceScreenShotCapture);
    LIB_FUNCTION("tIYf0W5VTi8", "libSceScreenShot", 1, "libSceScreenShot", sceScreenShotDisable);
    LIB_FUNCTION("ysfza71rm9M", "libSceScreenShot", 1, "libSceScreenShot",
                 sceScreenShotDisableNotification);
    LIB_FUNCTION("2xxUtuC-RzE", "libSceScreenShot", 1, "libSceScreenShot", sceScreenShotEnable);
    LIB_FUNCTION("BDUaqlVdSAY", "libSceScreenShot", 1, "libSceScreenShot",
                 sceScreenShotEnableNotification);
    LIB_FUNCTION("hNmK4SdhPT0", "libSceScreenShot", 1, "libSceScreenShot", sceScreenShotGetAppInfo);
    LIB_FUNCTION("VlAQIgXa2R0", "libSceScreenShot", 1, "libSceScreenShot",
                 sceScreenShotGetDrcParam);
    LIB_FUNCTION("-SV-oTNGFQk", "libSceScreenShot", 1, "libSceScreenShot", sceScreenShotIsDisabled);
    LIB_FUNCTION("ICNJ-1POs84", "libSceScreenShot", 1, "libSceScreenShot",
                 sceScreenShotIsVshScreenCaptureDisabled);
    LIB_FUNCTION("ahHhOf+QNkQ", "libSceScreenShot", 1, "libSceScreenShot",
                 sceScreenShotSetOverlayImage);
    LIB_FUNCTION("73WQ4Jj0nJI", "libSceScreenShot", 1, "libSceScreenShot",
                 sceScreenShotSetOverlayImageWithOrigin);
    LIB_FUNCTION("G7KlmIYFIZc", "libSceScreenShot", 1, "libSceScreenShot", sceScreenShotSetParam);
    LIB_FUNCTION("itlWFWV3Tzc", "libSceScreenShotDrc", 1, "libSceScreenShot",
                 sceScreenShotSetDrcParam);
};

} // namespace Libraries::ScreenShot
