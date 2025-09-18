// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "common/assert.h"
#include "common/types.h"

namespace Libraries::VideoOut {

constexpr std::size_t MaxDisplayBuffers = 16;
constexpr std::size_t MaxDisplayBufferGroups = 4;

enum class PixelFormat : u32 {
    Unknown = 0,
    A8R8G8B8Srgb = 0x80000000,
    A8B8G8R8Srgb = 0x80002200,
    A2R10G10B10 = 0x88060000,
    A2R10G10B10Srgb = 0x88000000,
    A2R10G10B10Bt2020Pq = 0x88740000,
    A16R16G16B16Float = 0xC1060000,

    // 8-bit formats
    R8Unorm = 0x00000001,
    R8Snorm = 0x00000002,
    R8Uint = 0x00000003,
    R8Sint = 0x00000004,

    // 16-bit formats
    R16Unorm = 0x00000005,
    R16Snorm = 0x00000006,
    R16Uint = 0x00000007,
    R16Sint = 0x00000008,
    R16Float = 0x00000009,

    R8G8Unorm = 0x0000000A,
    R8G8Snorm = 0x0000000B,
    R8G8Uint = 0x0000000C,
    R8G8Sint = 0x0000000D,

    // 32-bit formats
    R32Uint = 0x0000000E,
    R32Sint = 0x0000000F,
    R32Float = 0x00000010,

    R16G16Unorm = 0x00000011,
    R16G16Snorm = 0x00000012,
    R16G16Uint = 0x00000013,
    R16G16Sint = 0x00000014,
    R16G16Float = 0x00000015,

    // 64-bit formats
    R32G32Uint = 0x00000016,
    R32G32Sint = 0x00000017,
    R32G32Float = 0x00000018,

    R16G16B16A16Unorm = 0x00000019,
    R16G16B16A16Snorm = 0x0000001A,
    R16G16B16A16Uint = 0x0000001B,
    R16G16B16A16Sint = 0x0000001C,
    R16G16B16A16Float = 0x0000001D,

    // 128-bit formats
    R32G32B32A32Uint = 0x0000001E,
    R32G32B32A32Sint = 0x0000001F,
    R32G32B32A32Float = 0x00000020,

    // Depth / stencil formats
    D16Unorm = 0x00000021,
    D24UnormS8Uint = 0x00000022,
    D32Float = 0x00000023,
    D32FloatS8Uint = 0x00000024,

    // Video / HDR formats
    R10G10B10A2Unorm = 0x00000025,
    R10G10B10A2Uint = 0x00000026,
    B10G11R11Float = 0x00000027,
    E5B9G9R9Float = 0x00000028,
};

enum class TilingMode : s32 {
    Tile = 0,
    Linear = 1,
};

constexpr std::string_view GetPixelFormatString(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
        return "A8R8G8B8Srgb";
    case PixelFormat::A8B8G8R8Srgb:
        return "A8B8G8R8Srgb";
    case PixelFormat::A2R10G10B10:
        return "A2R10G10B10";
    case PixelFormat::A2R10G10B10Srgb:
        return "A2R10G10B10Srgb";
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return "A2R10G10B10Bt2020Pq";
    case PixelFormat::A16R16G16B16Float:
        return "A16R16G16B16Float";
    default:
        UNREACHABLE_MSG("Unknown pixel format {}", static_cast<u32>(format));
        return "";
    }
}

struct BufferAttribute {
    PixelFormat pixel_format;
    TilingMode tiling_mode;
    s32 aspect_ratio;
    u32 width;
    u32 height;
    u32 pitch_in_pixel;
    u32 option;
    u32 reserved0;
    u64 reserved1;
};

struct BufferAttributeGroup {
    bool is_occupied;
    BufferAttribute attrib;
};

struct VideoOutBuffer {
    s32 group_index{-1};
    uintptr_t address_left;
    uintptr_t address_right;
};

} // namespace Libraries::VideoOut
