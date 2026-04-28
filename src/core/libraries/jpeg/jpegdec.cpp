// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/libraries/jpeg/jpegdec.h"

// stb (JPEG decode from memory)
#include "core/libraries/jpeg/third_party/stb_image.h"

namespace Libraries::JpegDec {

// v49d: Capture decoded image buffer address for gallery texture discovery
u64 g_gr2_jpeg_decoded_addr = 0;

namespace {

constexpr size_t kMaxJpegScan = 16 * 1024 * 1024; // 16 MiB safety cap

static inline bool HasJpegSOI(const u8* p, size_t n) {
    return n >= 2 && p[0] == 0xFF && p[1] == 0xD8;
}

static size_t FindJpegSizeByEOI(const u8* p, size_t n) {
    if (!HasJpegSOI(p, n)) {
        return 0;
    }
    const size_t limit = std::min(n, kMaxJpegScan);
    for (size_t i = 2; i + 1 < limit; ++i) {
        if (p[i] == 0xFF && p[i + 1] == 0xD9) {
            return i + 2;
        }
    }
    return 0;
}

static inline u8 AlphaToU8(u16 a) {
    if (a <= 0x00FF) {
        return static_cast<u8>(a);
    }
    return static_cast<u8>(a >> 8);
}

static void ApplyConstantAlphaBGRA(u8* dst, u32 width, u32 height, u32 pitch, u8 alpha) {
    if (!dst || alpha == 0xFF) {
        return;
    }
    for (u32 y = 0; y < height; ++y) {
        u8* row = dst + static_cast<size_t>(y) * pitch;
        for (u32 x = 0; x < width; ++x) {
            row[x * 4 + 3] = alpha;
        }
    }
}

/// Fill OrbisJpegDecImageInfo with information from the decoded JPEG.
/// @param comp  Number of source components from stbi (1=grayscale, 3=YCbCr/RGB)
static void FillImageInfo(OrbisJpegDecImageInfo* info, u32 width, u32 height, int comp) {
    if (!info) {
        return;
    }
    std::memset(info, 0, sizeof(*info));
    info->img_width      = width;
    info->img_height     = height;
    info->out_img_width  = width;
    info->out_img_height = height;

    // Color space: 1=YCbCr for color JPEGs, 2=Grayscale
    if (comp == 1) {
        info->color_space   = ORBIS_JPEG_DEC_COLOR_SPACE_GRAYSCALE;
        info->sampling_type = ORBIS_JPEG_DEC_SAMPLING_TYPE_444;
        info->num_components = 1;
    } else {
        info->color_space   = ORBIS_JPEG_DEC_COLOR_SPACE_YCBCR;
        // stb doesn't expose chroma subsampling; most camera/encoder JPEGs use 4:2:0.
        // Our stb encoder also produces 4:2:0. Safe default.
        info->sampling_type = ORBIS_JPEG_DEC_SAMPLING_TYPE_420;
        info->num_components = 3;
    }
    // output_format: 0 = BGRA (the format we always produce)
    info->output_format = 0;
    info->flags = 0;
    info->reserved = 0;
}

} // namespace

s32 PS4_SYSV_ABI sceJpegDecQueryMemorySize(const OrbisJpegDecCreateParam* param) {
    LOG_INFO(Lib_Jpeg,
             "[GR2PhotoHLE] sceJpegDecQueryMemorySize: param={} size={} attr={} max_width={}",
             fmt::ptr(param),
             param ? param->size : 0,
             param ? static_cast<u32>(param->attr) : 0,
             param ? param->max_width : 0);
    return 0x800;
}

s32 PS4_SYSV_ABI sceJpegDecCreate(const OrbisJpegDecCreateParam* param, void* memory, u32 memory_size,
                                  OrbisJpegDecHandle* handle) {
    LOG_INFO(Lib_Jpeg,
             "[GR2PhotoHLE] sceJpegDecCreate: param={} memory={} memory_size={:#x} handle={}",
             fmt::ptr(param), fmt::ptr(memory), memory_size, fmt::ptr(handle));

    if (!handle || !param || !memory) {
        LOG_ERROR(Lib_Jpeg, "[GR2PhotoHLE] sceJpegDecCreate: invalid params");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (memory_size < sizeof(OrbisJpegDecHandleInternal)) {
        LOG_ERROR(Lib_Jpeg, "[GR2PhotoHLE] sceJpegDecCreate: memory_size too small ({:#x} < {:#x})",
                  memory_size, static_cast<u32>(sizeof(OrbisJpegDecHandleInternal)));
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    auto* h = reinterpret_cast<OrbisJpegDecHandleInternal*>(
        Common::AlignUp(reinterpret_cast<uintptr_t>(memory), 16));
    const uintptr_t mem_end = reinterpret_cast<uintptr_t>(memory) + memory_size;
    if (reinterpret_cast<uintptr_t>(h) + sizeof(*h) > mem_end) {
        LOG_ERROR(Lib_Jpeg, "[GR2PhotoHLE] sceJpegDecCreate: aligned handle doesn't fit");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    h->handle = h;
    h->handle_size = sizeof(*h);
    h->max_width = param->max_width;
    h->reserved = 0;
    *handle = h;

    LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] sceJpegDecCreate: OK handle={} max_width={}",
             fmt::ptr(h), param->max_width);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecDelete(OrbisJpegDecHandle handle) {
    LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] sceJpegDecDelete: handle={}", fmt::ptr(handle));
    if (!handle) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecParseHeader(const OrbisJpegDecParseParam* param, OrbisJpegDecImageInfo* info) {
    LOG_INFO(Lib_Jpeg,
             "[GR2PhotoHLE] sceJpegDecParseHeader: param={} info={} jpeg={} jpeg_size={}",
             fmt::ptr(param), fmt::ptr(info),
             param ? fmt::ptr(param->jpeg) : fmt::ptr(static_cast<const void*>(nullptr)),
             param ? param->jpeg_size : 0);

    if (!param || !info || !param->jpeg) {
        LOG_ERROR(Lib_Jpeg, "[GR2PhotoHLE] sceJpegDecParseHeader: invalid params");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    const auto* bytes = reinterpret_cast<const u8*>(param->jpeg);
    size_t size = static_cast<size_t>(param->jpeg_size);

    // Log first bytes to verify JPEG SOI marker
    if (size >= 4) {
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] ParseHeader: JPEG header bytes: "
                 "{:#04x} {:#04x} {:#04x} {:#04x}",
                 bytes[0], bytes[1], bytes[2], bytes[3]);
    }

    if (size == 0 || size > kMaxJpegScan) {
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] ParseHeader: jpeg_size={} — trying EOI scan", size);
        const size_t detected = FindJpegSizeByEOI(bytes, kMaxJpegScan);
        if (detected == 0) {
            LOG_ERROR(Lib_Jpeg, "[GR2PhotoHLE] ParseHeader: JPEG EOI not found");
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        size = detected;
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] ParseHeader: detected JPEG size={}", size);
    }

    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes),
                               static_cast<int>(size), &w, &h, &comp)) {
        LOG_ERROR(Lib_Jpeg, "[GR2PhotoHLE] ParseHeader: stbi_info_from_memory FAILED");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    FillImageInfo(info, static_cast<u32>(w), static_cast<u32>(h), comp);

    LOG_INFO(Lib_Jpeg,
             "[GR2PhotoHLE] ParseHeader: OK {}x{} comp={} colorSpace={} sampling={} numComp={}",
             w, h, comp, info->color_space, info->sampling_type, info->num_components);

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecDecode(OrbisJpegDecHandle handle, const OrbisJpegDecDecodeParam* param,
                                  OrbisJpegDecImageInfo* info) {
    LOG_INFO(Lib_Jpeg,
             "[GR2PhotoHLE] sceJpegDecDecode: handle={} param={} info={}",
             fmt::ptr(handle), fmt::ptr(param), fmt::ptr(info));

    if (!handle || !param || !param->jpeg || !param->image) {
        LOG_ERROR(Lib_Jpeg, "[GR2PhotoHLE] sceJpegDecDecode: invalid params "
                  "(handle={} param={} jpeg={} image={})",
                  fmt::ptr(handle), fmt::ptr(param),
                  param ? fmt::ptr(param->jpeg) : fmt::ptr(static_cast<const void*>(nullptr)),
                  param ? fmt::ptr(param->image) : fmt::ptr(static_cast<const void*>(nullptr)));
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    // Log ALL decode parameters for diagnostics
    LOG_INFO(Lib_Jpeg,
             "[GR2PhotoHLE] DecDecode params: jpeg={} image={} coef_buf={} "
             "jpeg_size={} image_size={} out_pitch={} "
             "output_mode={} color_space={} down_scale={} alpha={:#x} reserved={}",
             fmt::ptr(param->jpeg), fmt::ptr(param->image), fmt::ptr(param->coef_buffer),
             param->jpeg_size, param->image_size, param->out_pitch,
             param->output_mode, param->color_space, param->down_scale,
             param->alpha, param->reserved);

    const auto* bytes = reinterpret_cast<const u8*>(param->jpeg);
    size_t size = static_cast<size_t>(param->jpeg_size);

    // Log first bytes to verify JPEG header
    if (size >= 4 || (size == 0 && reinterpret_cast<uintptr_t>(bytes) != 0)) {
        const auto* peek = bytes;
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] DecDecode: JPEG header: "
                 "{:#04x} {:#04x} {:#04x} {:#04x}",
                 peek[0], peek[1], peek[2], peek[3]);
    }

    if (size == 0 || size > kMaxJpegScan) {
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] DecDecode: jpeg_size={} — trying EOI scan", size);
        const size_t detected = FindJpegSizeByEOI(bytes, kMaxJpegScan);
        if (detected == 0) {
            LOG_ERROR(Lib_Jpeg, "[GR2PhotoHLE] DecDecode: JPEG SOI/EOI not found");
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        size = detected;
        LOG_INFO(Lib_Jpeg, "[GR2PhotoHLE] DecDecode: detected JPEG size={}", size);
    }

    int w = 0, h = 0, comp = 0;
    stbi_uc* rgba = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes),
                                          static_cast<int>(size), &w, &h, &comp, 4);
    if (!rgba || w <= 0 || h <= 0) {
        LOG_ERROR(Lib_Jpeg,
                  "[GR2PhotoHLE] DecDecode: stbi_load_from_memory FAILED "
                  "(rgba={} w={} h={} comp={} stbi_err='{}')",
                  fmt::ptr(rgba), w, h, comp,
                  stbi_failure_reason() ? stbi_failure_reason() : "none");
        if (rgba) {
            stbi_image_free(rgba);
        }
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    const u32 width = static_cast<u32>(w);
    const u32 height = static_cast<u32>(h);

    u32 pitch = param->out_pitch;
    const u32 min_pitch = width * 4;
    if (pitch == 0 || pitch < min_pitch || pitch > (min_pitch + 0x10000) || (pitch % 4) != 0) {
        LOG_INFO(Lib_Jpeg,
                 "[GR2PhotoHLE] DecDecode: overriding pitch {} -> {} (min_pitch={})",
                 pitch, min_pitch, min_pitch);
        pitch = min_pitch;
    }

    auto* dst = reinterpret_cast<u8*>(param->image);

    // stb gives us RGBA. Write directly as RGBA into the guest buffer.
    // On real PS4, sceJpegDecDecode outputs RGBA byte order in memory
    // (the "B8G8R8A8" format label refers to GPU swizzling, not memory layout).
    const size_t src_pitch = static_cast<size_t>(width) * 4;
    for (u32 y = 0; y < height; ++y) {
        const u8* srow = rgba + static_cast<size_t>(y) * src_pitch;
        u8* drow = dst + static_cast<size_t>(y) * pitch;
        std::memcpy(drow, srow, src_pitch);
    }

    stbi_image_free(rgba);

    // Apply constant alpha if requested.
    // PS4 convention: alpha=0 means "fully opaque" (0xFF), not transparent.
    u8 alpha = AlphaToU8(param->alpha);
    if (alpha == 0) {
        alpha = 0xFF;
    }
    ApplyConstantAlphaBGRA(dst, width, height, pitch, alpha);

    // Fill output image info with all fields properly populated
    FillImageInfo(info, width, height, comp);

    // Log sample pixels for visual verification
    if (width >= 4 && height >= 4) {
        // Center pixel
        const u32 cx = width / 2, cy = height / 2;
        const u8* cpx = dst + static_cast<size_t>(cy) * pitch + cx * 4;
        LOG_INFO(Lib_Jpeg,
                 "[GR2PhotoHLE] DecDecode: center pixel[{},{}] RGBA=({},{},{},{})",
                 cx, cy, cpx[0], cpx[1], cpx[2], cpx[3]);
    }

    LOG_INFO(Lib_Jpeg,
             "[GR2PhotoHLE] sceJpegDecDecode: OK {}x{} comp={} pitch={} jpeg_size={} "
             "output_mode={} alpha_applied={:#x}",
             width, height, comp, pitch, size,
             param->output_mode, alpha);

    // v49d: Capture decoded image buffer address for gallery RE
    g_gr2_jpeg_decoded_addr = reinterpret_cast<u64>(param->image);

    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    // NIDs from aerolib.inl for libSceJpegDec
    LIB_FUNCTION("uNAUmANZMEw", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecQueryMemorySize);
    LIB_FUNCTION("JPh3Zgg0Zwc", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecCreate);
    LIB_FUNCTION("Hwh11+m5KoI", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDelete);
    LIB_FUNCTION("LSinoSQH790", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecParseHeader);
    LIB_FUNCTION("1kzQRoWEgSA", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDecode);
}

} // namespace Libraries::JpegDec
