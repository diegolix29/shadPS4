// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_blend_rewrite.h"

namespace {

using Factor = AmdGpu::BlendControl::BlendFactor;
using Function = AmdGpu::BlendControl::BlendFunc;
using Vulkan::BlendChannel;
using Vulkan::BlendRewriteResult;

TEST(BlendRewriteTest, KeepsNativeMinMaxWithUnitFactors) {
    Vulkan::BlendEquation equation{Factor::One, Function::Min, Factor::One};

    EXPECT_EQ(Vulkan::RewriteScaledMinMaxBlend(equation, AmdGpu::NumberFormat::Unorm,
                                               BlendChannel::Color),
              BlendRewriteResult::Native);
    EXPECT_EQ(equation.function, Function::Min);
}

TEST(BlendRewriteTest, ReducesUnsignedMinWithZeroContributionToZero) {
    Vulkan::BlendEquation equation{Factor::One, Function::Min, Factor::Zero};

    EXPECT_EQ(
        Vulkan::RewriteScaledMinMaxBlend(equation, AmdGpu::NumberFormat::Srgb, BlendChannel::Alpha),
        BlendRewriteResult::Exact);
    EXPECT_EQ(equation.src_factor, Factor::Zero);
    EXPECT_EQ(equation.dst_factor, Factor::Zero);
    EXPECT_EQ(equation.function, Function::Add);
}

TEST(BlendRewriteTest, ReducesUnsignedMaxWithZeroContributionToOtherTerm) {
    Vulkan::BlendEquation equation{Factor::Zero, Function::Max, Factor::DstColor};

    EXPECT_EQ(Vulkan::RewriteScaledMinMaxBlend(equation, AmdGpu::NumberFormat::Unorm,
                                               BlendChannel::Color),
              BlendRewriteResult::Exact);
    EXPECT_EQ(equation.src_factor, Factor::Zero);
    EXPECT_EQ(equation.dst_factor, Factor::DstColor);
    EXPECT_EQ(equation.function, Function::Add);
}

TEST(BlendRewriteTest, SquaresSelfScaledOperands) {
    Vulkan::BlendEquation color{Factor::SrcColor, Function::Min, Factor::DstColor};
    EXPECT_EQ(
        Vulkan::RewriteScaledMinMaxBlend(color, AmdGpu::NumberFormat::Srgb, BlendChannel::Color),
        BlendRewriteResult::Squared);
    EXPECT_EQ(color.function, Function::Min);

    Vulkan::BlendEquation alpha{Factor::SrcAlpha, Function::Max, Factor::DstAlpha};
    EXPECT_EQ(
        Vulkan::RewriteScaledMinMaxBlend(alpha, AmdGpu::NumberFormat::Unorm, BlendChannel::Alpha),
        BlendRewriteResult::Squared);
    EXPECT_EQ(alpha.function, Function::Max);

    // Alpha reads its own component through color factors.
    Vulkan::BlendEquation mixed{Factor::SrcColor, Function::Min, Factor::DstAlpha};
    EXPECT_EQ(
        Vulkan::RewriteScaledMinMaxBlend(mixed, AmdGpu::NumberFormat::Unorm, BlendChannel::Alpha),
        BlendRewriteResult::Squared);
}

TEST(BlendRewriteTest, RejectsColorScaledByAlpha) {
    Vulkan::BlendEquation equation{Factor::SrcAlpha, Function::Min, Factor::DstAlpha};

    EXPECT_EQ(Vulkan::RewriteScaledMinMaxBlend(equation, AmdGpu::NumberFormat::Unorm,
                                               BlendChannel::Color),
              BlendRewriteResult::Unsupported);
    EXPECT_EQ(equation.function, Function::Min);
}

TEST(BlendRewriteTest, RejectsMixedScaling) {
    Vulkan::BlendEquation equation{Factor::One, Function::Min, Factor::DstColor};

    EXPECT_EQ(Vulkan::RewriteScaledMinMaxBlend(equation, AmdGpu::NumberFormat::Unorm,
                                               BlendChannel::Color),
              BlendRewriteResult::Unsupported);
}

TEST(BlendRewriteTest, DoesNotUseUnsignedIdentitiesForSignedTargets) {
    Vulkan::BlendEquation zero{Factor::One, Function::Min, Factor::Zero};
    EXPECT_EQ(
        Vulkan::RewriteScaledMinMaxBlend(zero, AmdGpu::NumberFormat::Snorm, BlendChannel::Color),
        BlendRewriteResult::Unsupported);

    Vulkan::BlendEquation squared{Factor::SrcColor, Function::Min, Factor::DstColor};
    EXPECT_EQ(
        Vulkan::RewriteScaledMinMaxBlend(squared, AmdGpu::NumberFormat::Float, BlendChannel::Color),
        BlendRewriteResult::Unsupported);
}

} // namespace
