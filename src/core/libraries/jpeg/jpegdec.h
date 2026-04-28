// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
    class SymbolsResolver;
}

namespace Libraries::JpegDec {

    // PS4 JPEG Decoder HLE — struct layouts reverse-engineered from eboot + OpenOrbis.

    enum OrbisJpegDecCreateParamAttributes : u32 { ORBIS_JPEG_DEC_ATTRIBUTE_NONE = 0 };

    // Color space values returned in OrbisJpegDecImageInfo::color_space
    enum OrbisJpegDecColorSpace : u16 {
        ORBIS_JPEG_DEC_COLOR_SPACE_UNKNOWN    = 0,
        ORBIS_JPEG_DEC_COLOR_SPACE_YCBCR      = 1,
        ORBIS_JPEG_DEC_COLOR_SPACE_GRAYSCALE   = 2,
        ORBIS_JPEG_DEC_COLOR_SPACE_YCCK        = 3,
    };

    // Chroma subsampling type returned in OrbisJpegDecImageInfo::sampling_type
    enum OrbisJpegDecSamplingType : u16 {
        ORBIS_JPEG_DEC_SAMPLING_TYPE_444  = 0,
        ORBIS_JPEG_DEC_SAMPLING_TYPE_422  = 1,
        ORBIS_JPEG_DEC_SAMPLING_TYPE_420  = 2,
        ORBIS_JPEG_DEC_SAMPLING_TYPE_411  = 3,
    };

    // Pixel format for decoded output (OrbisJpegDecDecodeParam::output_mode)
    enum OrbisJpegDecPixelFormat : u16 {
        ORBIS_JPEG_DEC_PIXEL_FORMAT_B8G8R8A8 = 0,  // BGRA (default)
    };

    struct OrbisJpegDecHandleInternal {
        OrbisJpegDecHandleInternal* handle;
        u32 handle_size;
        u32 max_width;
        u32 reserved;
    };
    static_assert(sizeof(OrbisJpegDecHandleInternal) == 0x18);

    typedef OrbisJpegDecHandleInternal* OrbisJpegDecHandle;

    struct OrbisJpegDecCreateParam {
        u32 size;
        OrbisJpegDecCreateParamAttributes attr;
        u32 max_width;
    };
    static_assert(sizeof(OrbisJpegDecCreateParam) == 0x0C);

    struct OrbisJpegDecParseParam {
        const void* jpeg;
        u32 jpeg_size;
        u16 unk3;
        u16 unk4;
    };
    static_assert(sizeof(OrbisJpegDecParseParam) == 0x10);

    struct OrbisJpegDecDecodeParam {
        const void* jpeg;       // 0x00 JPEG input data
        void* image;            // 0x08 Output pixel buffer
        void* coef_buffer;      // 0x10 DCT coefficient buffer (can be null)
        u32 jpeg_size;          // 0x18 JPEG byte size (0 = auto-detect via EOI)
        u32 image_size;         // 0x1C Output buffer size in bytes
        u32 out_pitch;          // 0x20 Output row pitch in bytes (0 = auto)
        u16 output_mode;        // 0x24 Pixel format (0 = BGRA)
        u16 color_space;        // 0x26 Desired output color space
        u16 down_scale;         // 0x28 Downscale ratio (0 = none)
        u16 alpha;              // 0x2A Constant alpha (0 = opaque 0xFF)
        u32 reserved;           // 0x2C
    };
    static_assert(sizeof(OrbisJpegDecDecodeParam) == 0x30);

    struct OrbisJpegDecImageInfo {
        u32 img_width;          // 0x00 Source image width
        u32 img_height;         // 0x04 Source image height
        u16 color_space;        // 0x08 Color space (OrbisJpegDecColorSpace)
        u16 sampling_type;      // 0x0A Chroma subsampling (OrbisJpegDecSamplingType)
        u32 num_components;     // 0x0C Number of color components (1 or 3)
        u32 output_format;      // 0x10 Output pixel format info
        u32 flags;              // 0x14 Image flags
        u32 reserved;           // 0x18
        u32 out_img_width;      // 0x1C Decoded output width
        u32 out_img_height;     // 0x20 Decoded output height
    };
    static_assert(sizeof(OrbisJpegDecImageInfo) == 0x24);

    s32 PS4_SYSV_ABI sceJpegDecQueryMemorySize(const OrbisJpegDecCreateParam* param);
    s32 PS4_SYSV_ABI sceJpegDecCreate(const OrbisJpegDecCreateParam* param, void* memory, u32 memory_size,
                                      OrbisJpegDecHandle* handle);
    s32 PS4_SYSV_ABI sceJpegDecDelete(OrbisJpegDecHandle handle);
    s32 PS4_SYSV_ABI sceJpegDecParseHeader(const OrbisJpegDecParseParam* param, OrbisJpegDecImageInfo* info);
    s32 PS4_SYSV_ABI sceJpegDecDecode(OrbisJpegDecHandle handle, const OrbisJpegDecDecodeParam* param,
                                      OrbisJpegDecImageInfo* info);

    void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::JpegDec
