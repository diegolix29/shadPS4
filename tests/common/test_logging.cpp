// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include <gtest/gtest.h>

#include "common/logging/log.h"

namespace Common::Log {
std::array<Level, NUM_LOG_CLASSES> g_class_levels{};

static int g_vlog_calls = 0;

void VLog(Class, Level, const char*, int, const char*, fmt::string_view, fmt::format_args) {
    ++g_vlog_calls;
}
} // namespace Common::Log

namespace {

class LoggingMacroTest : public testing::Test {
protected:
    void SetUp() override {
        Common::Log::g_class_levels.fill(Common::Log::Level::Off);
        Common::Log::g_vlog_calls = 0;
    }

    static void SetCommonLevel(Common::Log::Level level) {
        Common::Log::g_class_levels[static_cast<std::size_t>(Common::Log::Class::Common)] = level;
    }
};

TEST_F(LoggingMacroTest, DisabledClassDoesNotEvaluateArguments) {
    int evaluations = 0;

    LOG_INFO(Common, "value={}", ++evaluations);

    EXPECT_EQ(evaluations, 0);
    EXPECT_EQ(Common::Log::g_vlog_calls, 0);
}

TEST_F(LoggingMacroTest, FilteredLevelDoesNotEvaluateArguments) {
    SetCommonLevel(Common::Log::Level::Error);
    int evaluations = 0;

    LOG_WARNING(Common, "value={}", ++evaluations);

    EXPECT_EQ(evaluations, 0);
    EXPECT_EQ(Common::Log::g_vlog_calls, 0);
}

TEST_F(LoggingMacroTest, EnabledLevelEvaluatesArgumentsOnce) {
    SetCommonLevel(Common::Log::Level::Info);
    int evaluations = 0;

    LOG_INFO(Common, "value={}", ++evaluations);

    EXPECT_EQ(evaluations, 1);
    EXPECT_EQ(Common::Log::g_vlog_calls, 1);
}

TEST_F(LoggingMacroTest, OtherClassesKeepTheirLevel) {
    SetCommonLevel(Common::Log::Level::Trace);
    int evaluations = 0;

    LOG_ERROR(Render_Vulkan, "value={}", ++evaluations);

    EXPECT_EQ(evaluations, 0);
    EXPECT_EQ(Common::Log::g_vlog_calls, 0);
}

} // namespace
