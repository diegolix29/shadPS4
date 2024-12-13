// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform settings {
    float gamma;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 linear_to_srgb(vec3 rgb) {
    return mix(a * pow(rgb, vec3(1.0 / (2.4 - 1.0 + pp.gamma))) - b, d * rgb, step(rgb, vec3(cutoff)));
}

void main()
{
    vec4 color_linear = texture(texSampler, uv);
    color = vec4(linear_to_srgb(color_linear.rgb), color_linear.a);
}