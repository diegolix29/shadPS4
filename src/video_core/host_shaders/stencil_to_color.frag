// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

// PACK == 1: one output texel per stencil texel, the stencil value replicated into
// every channel (games reading a packed depth/stencil word expect the stencil in one
// of the channels; the rest carry it harmlessly).
// PACK > 1: packs PACK horizontally adjacent stencil texels into one output texel for
// aliases that see the S8 plane as raw bytes of a wider format (output is 1/PACK the
// stencil width so the byte layout matches).
#ifndef PACK
#define PACK 1
#endif

layout (binding = 0, set = 0) uniform utexture2D in_stencil;

layout (location = 0) in vec2 uv;
layout (location = 0) out uvec4 out_color;

void main()
{
    const ivec2 coord = ivec2(gl_FragCoord.xy);
#if PACK == 1
    out_color = uvec4(texelFetch(in_stencil, coord, 0).r);
#else
    const ivec2 src_size = textureSize(in_stencil, 0);
    uvec4 texels = uvec4(0u);
    for (int i = 0; i < PACK; ++i) {
        const ivec2 src = ivec2(coord.x * PACK + i, coord.y);
        if (src.x < src_size.x) {
            texels[i] = texelFetch(in_stencil, src, 0).r;
        }
    }
    out_color = texels;
#endif
}
