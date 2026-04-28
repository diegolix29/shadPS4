// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/path_util.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "jpeg_error.h"
#include "jpegenc.h"

#include "core/libraries/content_search/content_search.h"

// GR2 Photo Mode: Context finder for post-encode state manipulation.
// After HLE encode, the game's internal photo state machine needs its scene
// node version bumped and result-ready flag set — normally done by the PS4
// hardware JPEG encoder as a side-effect.
#include "gr2_photo_context_finder.h"

// For GR2 photo capture: force GPU buffer readback before CPU reads the image pointer.
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/texture_cache.h"

// stb (JPEG encode to memory via callback)
#include "core/libraries/jpeg/third_party/stb_image_write.h"

// NOTE: This is defined in core/libraries/gnmdriver/gnmdriver.cpp
extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace Libraries::JpegEnc {

constexpr s32 ORBIS_JPEG_ENC_MINIMUM_MEMORY_SIZE = 0x800;
constexpr u32 ORBIS_JPEG_ENC_MAX_IMAGE_DIMENSION = 0xFFFF;
constexpr u32 ORBIS_JPEG_ENC_MAX_IMAGE_PITCH = 0xFFFFFFF;
constexpr u32 ORBIS_JPEG_ENC_MAX_IMAGE_SIZE = 0x7FFFFFFF;
namespace {

    // GR2 PhotoMode path: the game renders the photo to a GPU render target, then calls
    // sceJpegEncEncode with a CPU pointer. With readbackLinearImages=true, the texture cache
    // schedules an async download of the render target to CPU memory, but the async writeback
    // may not have completed by the time we read the pixels here.
    //
    // This function forces a SYNCHRONOUS download of the GPU image through the texture cache
    // using a dedicated one-shot Vulkan command buffer that bypasses the scheduler and
    // Liverpool GPU thread entirely, avoiding deadlocks.
    static bool GR2PhotoForceReadbackIfPossible(const OrbisJpegEncEncodeParam* param) {
        if (!param || !param->image) {
            return false;
        }
        if (!presenter) {
            LOG_WARNING(Lib_Jpeg, "[GR2PhotoHLE] No presenter — cannot force readback");
            return false;
        }

        auto& rasterizer = presenter->GetRasterizer();
        auto& tex_cache  = rasterizer.GetTextureCache();

        const VAddr addr = reinterpret_cast<VAddr>(param->image);
        const u64   size = static_cast<u64>(param->image_size);

        const bool ok = tex_cache.ForceDownloadByAddress(addr, size);
        if (ok) {
            LOG_INFO(Lib_Jpeg,
                     "[GR2PhotoHLE] Synchronous GPU readback succeeded for addr={:#x} size={}",
                     addr, size);
        } else {
            LOG_WARNING(Lib_Jpeg,
                        "[GR2PhotoHLE] ForceDownloadByAddress returned false — "
                        "falling back to existing buffer contents");
        }
        return ok;
    }

    // NOTE: The old ScopedGR2PhotoTempGpuFlags that temporarily enabled
    // readbackLinearImages during encode has been removed. The one-shot Vulkan
    // download path (ForceDownloadByAddress) works independently of that config
    // flag, and leaving readbackLinearImages enabled actually causes problems
    // (the async pipeline races with our synchronous copy, overwriting clean data).

    // --- GR2 Photo Mode: host autosave + debug --------------------------------
    // Why this exists:
    //  - Your logs show sceJpegEncEncode runs, but ContentExport is never called.
    //  - ContentSearchGetNumOfContent stays 0, so GR2's gallery stays empty / placeholder.
    //  - This writes the encoded JPEG bytes to the same folder ContentSearch already scans:
    //        <ScreenshotsDir>/GR2_PhotoApp_HLE/
    //
    // Disable autosave (optional):
    //     export SHADPS4_GR2_PHOTO_AUTOSAVE=0

    static bool GR2PhotoAutoSaveEnabled() {
        const char* v = std::getenv("SHADPS4_GR2_PHOTO_AUTOSAVE");
        if (v == nullptr || *v == '\0') {
            return true; // default: ON
        }
        if (v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F') {
            return false;
        }
        // treat "1", "true", "yes" as enabled
        if (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T') {
            return true;
        }
        return false;
    }

    static std::filesystem::path GetGR2PhotoDir() {
        std::error_code ec;
        // RENAME(GR2FORK v1.0): "GR2_PhotoApp_HLE" -> "Gravity Rush 2" for
        // a user-facing gallery subdir name. Must stay in sync with
        // content_search.cpp's GetScreenshotDir().
        const auto dir =
            Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir) / "Gravity Rush 2";
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            LOG_ERROR(Core, "[GR2PhotoHLE] create_directories failed for {}: {}",
                      Common::FS::PathToUTF8String(dir), ec.message());
        }
        return dir;
    }

    static void SaveGR2PhotoJpegToHost(const u8* data, size_t size, u32 width, u32 height) {
        if (!GR2PhotoAutoSaveEnabled()) {
            return;
        }
        if (data == nullptr || size < 4) {
            return;
        }
        // Only save if it looks like a JPEG stream.
        if (!(data[0] == 0xFF && data[1] == 0xD8)) {
            LOG_ERROR(Lib_Jpeg, "[GR2PhotoHLE] Not saving: output does not start with JPEG SOI ({} bytes)",
                      static_cast<u64>(size));
            return;
        }

        const auto dir = GetGR2PhotoDir();
        const auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();

        std::filesystem::path out_path;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            char name[128];
            std::snprintf(name, sizeof(name), "gr2_photo_%lld_%ux%u_%03d.jpg",
                          static_cast<long long>(now_ms),
                          static_cast<unsigned>(width),
                          static_cast<unsigned>(height),
                          attempt);
            out_path = dir / name;
            std::error_code ec;
            if (!std::filesystem::exists(out_path, ec)) {
                break;
            }
        }

        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            LOG_ERROR(Lib_Jpeg, "[GR2PhotoHLE] Failed to open {} for write",
                      Common::FS::PathToUTF8String(out_path));
            return;
        }
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out.good()) {
            LOG_ERROR(Lib_Jpeg, "[GR2PhotoHLE] Failed writing {} ({} bytes)",
                      Common::FS::PathToUTF8String(out_path), static_cast<u64>(size));
            return;
        }

        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Autosaved JPEG ({} bytes) -> {}",
                 static_cast<u64>(size), Common::FS::PathToUTF8String(out_path));
    }

    // --------------------------------------------------------------------------

} // namespace


static s32 ValidateJpegEncCreateParam(const OrbisJpegEncCreateParam* param) {
    if (!param) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }
    if (param->size != sizeof(OrbisJpegEncCreateParam)) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_SIZE;
    }
    if (param->attr != ORBIS_JPEG_ENC_ATTRIBUTE_NONE) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    return ORBIS_OK;
}

static s32 ValidateJpegEncMemory(const void* memory, const u32 memory_size) {
    if (!memory) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }
    if (memory_size < ORBIS_JPEG_ENC_MINIMUM_MEMORY_SIZE) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_SIZE;
    }
    return ORBIS_OK;
}

static s32 ValidateJpegEncEncodeParam(const OrbisJpegEncEncodeParam* param) {

    // Validate addresses
    if (!param) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }
    if (!param->image || (param->pixel_format != ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8 &&
                          !Common::IsAligned(reinterpret_cast<VAddr>(param->image), 4))) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }
    if (!param->jpeg) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }

    // Validate sizes
    if (param->image_size == 0 || param->jpeg_size == 0) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_SIZE;
    }

    // Validate parameters
    if (param->image_width > ORBIS_JPEG_ENC_MAX_IMAGE_DIMENSION ||
        param->image_height > ORBIS_JPEG_ENC_MAX_IMAGE_DIMENSION) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->image_pitch == 0 || param->image_pitch > ORBIS_JPEG_ENC_MAX_IMAGE_PITCH ||
        (param->pixel_format != ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8 &&
         !Common::IsAligned(param->image_pitch, 4))) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    const auto calculated_size = param->image_height * param->image_pitch;
    if (calculated_size > ORBIS_JPEG_ENC_MAX_IMAGE_SIZE || calculated_size > param->image_size) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->encode_mode != ORBIS_JPEG_ENC_ENCODE_MODE_NORMAL &&
        param->encode_mode != ORBIS_JPEG_ENC_ENCODE_MODE_MJPEG) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_YCC &&
        param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_GRAYSCALE) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->sampling_type != ORBIS_JPEG_ENC_SAMPLING_TYPE_FULL &&
        param->sampling_type != ORBIS_JPEG_ENC_SAMPLING_TYPE_422 &&
        param->sampling_type != ORBIS_JPEG_ENC_SAMPLING_TYPE_420) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->restart_interval > ORBIS_JPEG_ENC_MAX_IMAGE_DIMENSION) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    switch (param->pixel_format) {
    case ORBIS_JPEG_ENC_PIXEL_FORMAT_R8G8B8A8:
    case ORBIS_JPEG_ENC_PIXEL_FORMAT_B8G8R8A8:
        if (param->image_pitch >> 2 < param->image_width ||
            param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_YCC ||
            param->sampling_type == ORBIS_JPEG_ENC_SAMPLING_TYPE_FULL) {
            return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
        }
        break;
    case ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8U8Y8V8:
        if (param->image_pitch >> 1 < Common::AlignUp(param->image_width, 2) ||
            param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_YCC ||
            param->sampling_type == ORBIS_JPEG_ENC_SAMPLING_TYPE_FULL) {
            return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
        }
        break;
    case ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8:
        if (param->image_pitch < param->image_width ||
            param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_GRAYSCALE ||
            param->sampling_type != ORBIS_JPEG_ENC_SAMPLING_TYPE_FULL) {
            return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
        }
        break;
    default:
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }

    return ORBIS_OK;
}

namespace {

static inline int ClampQuality(u8 compression_ratio) {
    // GR2 passes compression_ratio=50; treat it as a JPEG quality request.
    const int q = static_cast<int>(compression_ratio);
    if (q < 1) return 1;
    if (q > 100) return 100;
    return q;
}

static inline u8 ClampU8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<u8>(v);
}

struct MemWriter {
    std::vector<u8>* out{};
};

static void StbiWriteToVector(void* context, void* data, int size) {
    auto* w = static_cast<MemWriter*>(context);
    auto* p = static_cast<u8*>(data);
    w->out->insert(w->out->end(), p, p + size);
}

static void YUVToRGB(u8 Y, u8 U, u8 V, u8& R, u8& G, u8& B) {
    const int c = static_cast<int>(Y) - 16;
    const int d = static_cast<int>(U) - 128;
    const int e = static_cast<int>(V) - 128;

    const int r = (298 * c + 409 * e + 128) >> 8;
    const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    const int b = (298 * c + 516 * d + 128) >> 8;

    R = ClampU8(r);
    G = ClampU8(g);
    B = ClampU8(b);
}

} // namespace

static s32 ValidateJpecEngHandle(OrbisJpegEncHandle handle) {
    if (!handle || !Common::IsAligned(reinterpret_cast<VAddr>(handle), 0x20) ||
        handle->handle != handle) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegEncCreate(const OrbisJpegEncCreateParam* param, void* memory,
                                  const u32 memory_size, OrbisJpegEncHandle* handle) {
    if (auto param_ret = ValidateJpegEncCreateParam(param); param_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid create param");
        return param_ret;
    }
    if (auto memory_ret = ValidateJpegEncMemory(memory, memory_size); memory_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid memory");
        return memory_ret;
    }
    if (!handle) {
        LOG_ERROR(Lib_Jpeg, "Invalid handle output");
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }

    auto* handle_internal = reinterpret_cast<OrbisJpegEncHandleInternal*>(
        Common::AlignUp(reinterpret_cast<VAddr>(memory), 0x20));

    // Zero-fill the entire memory buffer FIRST — the game may read internal
    // state from this buffer after encode.  The real PS4 encoder populates
    // encoding tables and metadata here.  Zero-fill ensures the game doesn't
    // read garbage if it accesses fields we don't explicitly set.
    std::memset(memory, 0, memory_size);

    handle_internal->handle = handle_internal;
    handle_internal->handle_size = sizeof(OrbisJpegEncHandleInternal*);
    *handle = handle_internal;

    // GR2: Record handle location so the context finder can scan nearby memory
    // for the game's photo context object after encode completes.
    GR2PhotoContextFinder::Instance().OnCreate(
        reinterpret_cast<uintptr_t>(handle_internal),
        reinterpret_cast<uintptr_t>(memory),
        memory_size);

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegEncDelete(OrbisJpegEncHandle handle) {
    if (auto handle_ret = ValidateJpecEngHandle(handle); handle_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid handle");
        return handle_ret;
    }
    handle->handle = nullptr;
    return ORBIS_OK;
}


s32 PS4_SYSV_ABI sceJpegEncEncode(OrbisJpegEncHandle handle, const OrbisJpegEncEncodeParam* param,
                                  OrbisJpegEncOutputInfo* output_info) {
    if (auto handle_ret = ValidateJpecEngHandle(handle); handle_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid handle");
        return handle_ret;
    }
    if (auto param_ret = ValidateJpegEncEncodeParam(param); param_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid encode param");
        return param_ret;
    }

    const u32 width = param->image_width;
    const u32 height = param->image_height;
    const u32 pitch = param->image_pitch;
    const int quality = ClampQuality(param->compression_ratio);

    // For photo-sized encodes: ensure GPU data is flushed to CPU memory.
    const bool is_photo_encode = (width == 1024 && height == 1024);
    bool readback_ok = false;
    if (is_photo_encode) {
        readback_ok = GR2PhotoForceReadbackIfPossible(param);

        // Diagnostic: dump handle memory to understand what the game expects
        const auto* hmem = reinterpret_cast<const u8*>(handle);
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] sceJpegEncEncode called: handle={:#x} "
                 "param={:#x} output_info={:#x}",
                 reinterpret_cast<uintptr_t>(handle),
                 reinterpret_cast<uintptr_t>(param),
                 reinterpret_cast<uintptr_t>(output_info));
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Handle[0..31]: "
                 "{:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x} "
                 "{:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x} "
                 "{:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x} "
                 "{:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x}",
                 hmem[0],hmem[1],hmem[2],hmem[3],hmem[4],hmem[5],hmem[6],hmem[7],
                 hmem[8],hmem[9],hmem[10],hmem[11],hmem[12],hmem[13],hmem[14],hmem[15],
                 hmem[16],hmem[17],hmem[18],hmem[19],hmem[20],hmem[21],hmem[22],hmem[23],
                 hmem[24],hmem[25],hmem[26],hmem[27],hmem[28],hmem[29],hmem[30],hmem[31]);
        // Check if the game wrote anything to the handle memory beyond our header
        bool has_game_data = false;
        for (int i = 0x10; i < 0x100; ++i) {
            if (hmem[i] != 0) {
                has_game_data = true;
                LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] First non-zero handle byte at offset 0x{:x}: 0x{:02x}",
                         i, hmem[i]);
                break;
            }
        }
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Handle memory beyond header has game data: {}",
                 has_game_data ? "YES" : "NO (all zero)");
    }

    // Snapshot the pixel data.  If ForceDownloadByAddress succeeded, the data
    // was written synchronously to param->image and is stable — a single memcpy
    // is enough.  If it failed (no tracked RT, fallback path), we use the
    // retry-based torn-read protection from the async readbackLinearImages era.
    std::vector<u8> snapshot;
    const u8* src = static_cast<const u8*>(param->image);
    if (is_photo_encode && src && pitch > 0 && height > 0) {
        const u64 buf_size = static_cast<u64>(pitch) * height;
        snapshot.resize(buf_size);

        if (readback_ok) {
            // Synchronous readback completed — data is stable, single copy is sufficient.
            std::memcpy(snapshot.data(), src, buf_size);
            LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Clean snapshot (sync readback)");
        } else {
            // Fallback: async pipeline may still be writing — use retry loop.
            const u64 sample_off = (buf_size / 2) & ~63ULL;
            const u64 sample_len = std::min<u64>(256, buf_size - sample_off);

            for (int retry = 0; retry < 10; ++retry) {
                std::memcpy(snapshot.data(), src, buf_size);
                bool clean = true;
                for (u64 i = 0; i < sample_len; ++i) {
                    if (snapshot[sample_off + i] != src[sample_off + i]) {
                        clean = false;
                        break;
                    }
                }
                if (clean) {
                    LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Clean snapshot on attempt {} (fallback)",
                             retry + 1);
                    break;
                }
                LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Torn snapshot on attempt {}, retrying",
                         retry + 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        src = snapshot.data();
    }

    // Diagnostic: log pitch details for photo-sized encodes
    if (width == 1024 && height == 1024) {
        const u32 expected_pitch = width * 4; // BGRA: 4 bytes per pixel
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Encode diagnostics: width={} height={} pitch={} "
                 "expected_pitch={} pitch_diff={} pixel_format={} image_size={}",
                 width, height, pitch, expected_pitch,
                 static_cast<s32>(pitch) - static_cast<s32>(expected_pitch),
                 static_cast<u32>(param->pixel_format), param->image_size);

        // Log a few sample pixels to detect tiling or corruption patterns
        if (src) {
            // Check row 0 and row 1 to see if data looks linear or tiled
            const u8* r0 = src;
            const u8* r1 = src + pitch;
            LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Row0[0..7]: {:02x} {:02x} {:02x} {:02x}  "
                     "{:02x} {:02x} {:02x} {:02x}",
                     r0[0], r0[1], r0[2], r0[3], r0[4], r0[5], r0[6], r0[7]);
            LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Row1[0..7]: {:02x} {:02x} {:02x} {:02x}  "
                     "{:02x} {:02x} {:02x} {:02x}",
                     r1[0], r1[1], r1[2], r1[3], r1[4], r1[5], r1[6], r1[7]);

            // Check if row 8 matches expectations (detects micro-tiling: 8x8 blocks)
            const u8* r8 = src + 8 * pitch;
            LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Row8[0..7]: {:02x} {:02x} {:02x} {:02x}  "
                     "{:02x} {:02x} {:02x} {:02x}",
                     r8[0], r8[1], r8[2], r8[3], r8[4], r8[5], r8[6], r8[7]);

            // Check for all-zero (readback failed entirely)
            bool all_zero = true;
            for (int i = 0; i < 256 && all_zero; ++i) {
                if (src[i] != 0) all_zero = false;
            }
            LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] First 256 bytes all-zero: {}",
                     all_zero ? "YES (readback may have failed)" : "NO (has pixel data)");
        }
    }

    int comp = 3;
    std::vector<u8> packed;

    if (param->pixel_format == ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8 ||
        param->color_space == ORBIS_JPEG_ENC_COLOR_SPACE_GRAYSCALE) {
        comp = 1;
        packed.resize(static_cast<size_t>(width) * height);
        for (u32 y = 0; y < height; ++y) {
            const u8* row = src + static_cast<size_t>(y) * pitch;
            std::memcpy(packed.data() + static_cast<size_t>(y) * width, row, width);
        }
    } else if (param->pixel_format == ORBIS_JPEG_ENC_PIXEL_FORMAT_B8G8R8A8) {
        comp = 3;
        packed.resize(static_cast<size_t>(width) * height * 3);
        for (u32 y = 0; y < height; ++y) {
            const u8* row = src + static_cast<size_t>(y) * pitch;
            u8* out = packed.data() + static_cast<size_t>(y) * width * 3;
            for (u32 x = 0; x < width; ++x) {
                const u8 b = row[x * 4 + 0];
                const u8 g = row[x * 4 + 1];
                const u8 r = row[x * 4 + 2];
                out[x * 3 + 0] = r;
                out[x * 3 + 1] = g;
                out[x * 3 + 2] = b;
            }
        }
    } else if (param->pixel_format == ORBIS_JPEG_ENC_PIXEL_FORMAT_R8G8B8A8) {
        comp = 3;
        packed.resize(static_cast<size_t>(width) * height * 3);
        for (u32 y = 0; y < height; ++y) {
            const u8* row = src + static_cast<size_t>(y) * pitch;
            u8* out = packed.data() + static_cast<size_t>(y) * width * 3;
            for (u32 x = 0; x < width; ++x) {
                const u8 r = row[x * 4 + 0];
                const u8 g = row[x * 4 + 1];
                const u8 b = row[x * 4 + 2];
                out[x * 3 + 0] = r;
                out[x * 3 + 1] = g;
                out[x * 3 + 2] = b;
            }
        }
    } else if (param->pixel_format == ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8U8Y8V8) {
        // YUYV422: [Y0 U Y1 V] per 2 pixels
        comp = 3;
        packed.resize(static_cast<size_t>(width) * height * 3);
        for (u32 y = 0; y < height; ++y) {
            const u8* row = src + static_cast<size_t>(y) * pitch;
            u8* out = packed.data() + static_cast<size_t>(y) * width * 3;

            for (u32 x = 0; x + 1 < width; x += 2) {
                const u8 y0 = row[x * 2 + 0];
                const u8 u  = row[x * 2 + 1];
                const u8 y1 = row[x * 2 + 2];
                const u8 v  = row[x * 2 + 3];

                u8 r0, g0, b0;
                u8 r1, g1, b1;
                YUVToRGB(y0, u, v, r0, g0, b0);
                YUVToRGB(y1, u, v, r1, g1, b1);

                out[(x + 0) * 3 + 0] = r0;
                out[(x + 0) * 3 + 1] = g0;
                out[(x + 0) * 3 + 2] = b0;

                out[(x + 1) * 3 + 0] = r1;
                out[(x + 1) * 3 + 1] = g1;
                out[(x + 1) * 3 + 2] = b1;
            }
        }
    } else {
        LOG_ERROR(Lib_Jpeg, "sceJpegEncEncode: unsupported pixel_format={}",
                  static_cast<u32>(param->pixel_format));
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }

    std::vector<u8> jpg;
    jpg.reserve(256 * 1024);

    MemWriter w{&jpg};
    const int ok = stbi_write_jpg_to_func(
        &StbiWriteToVector, &w,
        static_cast<int>(width), static_cast<int>(height),
        comp, packed.data(), quality);

    if (!ok || jpg.empty()) {
        LOG_ERROR(Lib_Jpeg, "sceJpegEncEncode: stb failed to encode (w={}, h={}, comp={}, q={})",
                  width, height, comp, quality);
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }

    if (jpg.size() > static_cast<size_t>(param->jpeg_size)) {
        LOG_ERROR(Lib_Jpeg, "JPEG output buffer too small: need {} bytes, have {} bytes",
                  static_cast<u64>(jpg.size()), param->jpeg_size);
        return ORBIS_JPEG_ENC_ERROR_INVALID_SIZE;
    }

    std::memcpy(param->jpeg, jpg.data(), jpg.size());

    // CRITICAL: On real PS4, the hardware encoder writes the actual JPEG output
    // size back to param->jpeg_size. The game passes output_info=NULL and reads
    // the output size from param->jpeg_size after encode returns. Without this,
    // the game sees the original buffer size (not actual JPEG size) and may
    // interpret it as an encode failure.
    {
        auto* mutable_param = const_cast<OrbisJpegEncEncodeParam*>(param);
        const u32 old_jpeg_size = mutable_param->jpeg_size;
        mutable_param->jpeg_size = static_cast<u32>(jpg.size());
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Updated param->jpeg_size: {} -> {} (actual JPEG bytes)",
                 old_jpeg_size, mutable_param->jpeg_size);
    }

    // Also write encode results to the handle internal memory. On real PS4,
    // the hardware encoder stores output metadata in the handle structure.
    // The game may read fields from here rather than from output_info (which is NULL).
    {
        auto* hmem = reinterpret_cast<u8*>(handle);
        // Write an OrbisJpegEncOutputInfo-like struct starting at handle+0x10
        // (right after the 16-byte handle header)
        struct HandleEncodeResult {
            u32 jpeg_size;       // +0x10: actual JPEG byte count
            u32 status;          // +0x14: 0 = success
            u32 width;           // +0x18: image width
            u32 height;          // +0x1C: image height
            u32 color_space;     // +0x20: color space
            u32 sampling_type;   // +0x24: sampling type
            u32 comp_ratio;      // +0x28: compression ratio
            u32 pixel_format;    // +0x2C: pixel format
        };
        auto* result = reinterpret_cast<HandleEncodeResult*>(hmem + 0x10);
        result->jpeg_size = static_cast<u32>(jpg.size());
        result->status = 0; // success
        result->width = param->image_width;
        result->height = param->image_height;
        result->color_space = static_cast<u32>(param->color_space);
        result->sampling_type = static_cast<u32>(param->sampling_type);
        const u32 raw_size = param->image_width * param->image_height * (comp == 1 ? 1 : 3);
        result->comp_ratio = raw_size > 0
            ? static_cast<u32>((static_cast<u64>(jpg.size()) * 100) / raw_size)
            : 0;
        result->pixel_format = static_cast<u32>(param->pixel_format);
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Wrote encode result to handle+0x10: "
                 "size={} status=0 {}x{} cs={} st={} ratio={} fmt={}",
                 result->jpeg_size, result->width, result->height,
                 result->color_space, result->sampling_type,
                 result->comp_ratio, result->pixel_format);
    }

    if (output_info) {
        // Zero-fill first in case the game reads fields beyond what we set
        std::memset(output_info, 0, sizeof(OrbisJpegEncOutputInfo));
        output_info->size = static_cast<u32>(jpg.size());
        output_info->height = param->image_height;
        output_info->width = param->image_width;
        output_info->color_space = static_cast<u32>(param->color_space);
        output_info->sampling_type = static_cast<u32>(param->sampling_type);
        // Compute actual compression ratio: (compressed / uncompressed) * 100
        const u32 raw_size = param->image_width * param->image_height * (comp == 1 ? 1 : 3);
        output_info->comp_ratio = raw_size > 0
            ? static_cast<u32>((static_cast<u64>(jpg.size()) * 100) / raw_size)
            : 0;
    }

    // Quick sanity: log first few bytes so we can tell if GR2 is receiving a valid JPEG header.
    if (jpg.size() >= 4) {
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] JPEG header bytes: {:#x} {:#x} {:#x} {:#x}",
                 static_cast<u32>(jpg[0]), static_cast<u32>(jpg[1]),
                 static_cast<u32>(jpg[2]), static_cast<u32>(jpg[3]));
    }

    // For 1024x1024 photos (GR2 photo mode): save as viewable JPEG.
    // Registration with ContentSearch is deferred to sceContentExportFromData,
    // which the game calls after encode succeeds.
    if (width == 1024 && height == 1024) {
        // Skip saving if the JPEG is suspiciously small — indicates black/empty frame
        // (first photo per session). Valid photos are typically 70-90KB.
        if (jpg.size() < 30000) {
            LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Skipping save — JPEG too small ({} bytes, likely black frame)",
                     jpg.size());
        } else {
            // NAMING(GR2FORK v1.0): was "UP9000-CUSA04943_00-SCREENSHOT%05u".
            // New format is "YYYYMMDD_HHMMSS_NNN" — readable, sortable, short.
            const std::string cid = Libraries::ContentSearch::GenerateTimestampContentId();

            const auto dir = Libraries::ContentSearch::GetScreenshotHostDir();
            const auto path = dir / (cid + ".jpg");

            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(reinterpret_cast<const char*>(jpg.data()),
                          static_cast<std::streamsize>(jpg.size()));
                LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Saved JPEG ({} bytes) -> {}",
                         jpg.size(), Common::FS::PathToUTF8String(path));
            }
            // Store content ID for ContentExport to pick up (avoids duplicate saves)
            Libraries::ContentSearch::SetLastSavedContentId(cid);
        }
    } else {
        SaveGR2PhotoJpegToHost(jpg.data(), jpg.size(), width, height);
    }

    // GR2: After successful photo encode, find and update the game's internal
    // photo context.  On real PS4, the hardware JPEG encoder bumps the scene
    // node version counter and sets the result-ready flag as side-effects.
    // Without these updates the game's state machine stalls at state=1 and
    // the preview shows a red square.
    if (width == 1024 && height == 1024 && jpg.size() >= 30000) {
        const bool ctx_ok = GR2PhotoContextFinder::Instance().OnEncodeComplete(
            static_cast<u32>(jpg.size()), width, height);
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Context finder post-encode: {}",
                 ctx_ok ? "SUCCESS — game state updated"
                        : "FAILED — could not locate photo context");
        GR2PhotoContextFinder::Instance().DumpState();
    }

    LOG_INFO(Lib_Jpeg, "sceJpegEncEncode: [GR2PhotoHLE] stb encoded {} bytes ({}x{}, fmt={}, sampling={})",
             static_cast<u32>(jpg.size()), width, height,
             static_cast<u32>(param->pixel_format), static_cast<u32>(param->sampling_type));

    // GR2: Trigger texture survey — logs the next ~200 unique texture bindings
    // on the GPU thread so we can see what the game samples for the preview.
    if (width == 1024 && height == 1024 && jpg.size() >= 30000) {
        VideoCore::TextureCache::TriggerPhotoTextureSurvey(200);
    }

    // On real PS4, sceJpegEncEncode returns the JPEG output size (positive s32)
    // on success, and a negative error code on failure. The game likely checks:
    //   if (ret <= 0) → encode failed → show red placeholder
    // Returning ORBIS_OK (0) looks like "zero bytes encoded" = failure.
    const s32 result = static_cast<s32>(jpg.size());
    LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] Returning JPEG size as result: {}", result);
    return result;
}

s32 PS4_SYSV_ABI sceJpegEncQueryMemorySize(const OrbisJpegEncCreateParam* param) {
    if (auto param_ret = ValidateJpegEncCreateParam(param); param_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid create param");
        return param_ret;
    }
    return ORBIS_JPEG_ENC_MINIMUM_MEMORY_SIZE;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("K+rocojkr-I", "libSceJpegEnc", 1, "libSceJpegEnc", sceJpegEncCreate);
    LIB_FUNCTION("j1LyMdaM+C0", "libSceJpegEnc", 1, "libSceJpegEnc", sceJpegEncDelete);
    LIB_FUNCTION("QbrU0cUghEM", "libSceJpegEnc", 1, "libSceJpegEnc", sceJpegEncEncode);
    LIB_FUNCTION("o6ZgXfFdWXQ", "libSceJpegEnc", 1, "libSceJpegEnc", sceJpegEncQueryMemorySize);
};

} // namespace Libraries::JpegEnc
