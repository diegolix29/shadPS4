// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include "video_core/amdgpu/stall_log.h"

TEST(Pm4StallLogTest, ShouldLogStallIteration) {
    EXPECT_FALSE(AmdGpu::ShouldLogStallIteration(0));
    EXPECT_FALSE(AmdGpu::ShouldLogStallIteration(10));
    EXPECT_FALSE(AmdGpu::ShouldLogStallIteration(99'999));
    EXPECT_TRUE(AmdGpu::ShouldLogStallIteration(100'000));
    EXPECT_FALSE(AmdGpu::ShouldLogStallIteration(100'001));
    EXPECT_TRUE(AmdGpu::ShouldLogStallIteration(200'000));
}
