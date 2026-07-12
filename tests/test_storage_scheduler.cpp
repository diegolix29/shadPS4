// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/emulator_settings.h"
#include "core/file_sys/storage_scheduler.h"

namespace {

using Core::FileSys::StorageTimingModel;

TEST(StorageSchedulerTest, AcceptsAndNormalizesOnlyPublicProfiles) {
    constexpr std::array supported{0u, 75u, 100u, 125u};
    for (const u32 profile : supported) {
        EXPECT_TRUE(Core::FileSys::IsSupportedReadBandwidth(profile));
        EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(profile), profile);
    }

    constexpr std::array unsupported{1u, 50u, 74u, 76u, 99u, 101u, 124u, 126u, 1'000u};
    for (const u32 profile : unsupported) {
        EXPECT_FALSE(Core::FileSys::IsSupportedReadBandwidth(profile));
    }
}

TEST(StorageSchedulerTest, UnsupportedValuesClampToNearestProfileNeverToNative) {
    // A hand-edited config asking for throttling must keep throttling.
    EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(1), 75u);
    EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(74), 75u);
    EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(76), 75u);
    EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(87), 75u);
    EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(88), 100u);
    EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(99), 100u);
    EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(101), 100u);
    EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(112), 100u);
    EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(113), 125u);
    EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(126), 125u);
    EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(1'000), 125u);
    EXPECT_EQ(Core::FileSys::NormalizeReadBandwidth(0), 0u);
}

TEST(StorageSchedulerTest, DisabledProfileAddsNoTransferDelay) {
    constexpr StorageTimingModel model{0};
    EXPECT_EQ(model.TransferDuration(0), std::chrono::nanoseconds::zero());
    EXPECT_EQ(model.TransferDuration(1024 * 1024), std::chrono::nanoseconds::zero());
}

TEST(StorageSchedulerTest, ProfilesProduceExactSequentialCeilings) {
    constexpr StorageTimingModel model75{75};
    constexpr StorageTimingModel model100{100};
    constexpr StorageTimingModel model125{125};

    EXPECT_EQ(model75.TransferDuration(75 * 1024 * 1024), std::chrono::seconds{1});
    EXPECT_EQ(model100.TransferDuration(100 * 1024 * 1024), std::chrono::seconds{1});
    EXPECT_EQ(model125.TransferDuration(125 * 1024 * 1024), std::chrono::seconds{1});
    EXPECT_EQ(model100.TransferDuration(512 * 1024), std::chrono::milliseconds{5});
    EXPECT_EQ(model125.TransferDuration(1024 * 1024), std::chrono::milliseconds{8});
}

TEST(StorageSchedulerTest, SmallSequentialReadsAggregateWithoutPerReadLatency) {
    constexpr StorageTimingModel model{75};
    constexpr auto individual = model.ServiceDuration(4 * 1024, true);
    EXPECT_NEAR((individual * 128).count(), model.TransferDuration(512 * 1024).count(), 128);
}

TEST(StorageSchedulerTest, RandomReadAddsMechanicalPositioningOnce) {
    constexpr StorageTimingModel model{100};
    constexpr auto sequential = model.ServiceDuration(4 * 1024, true);
    constexpr auto positioned = model.ServiceDuration(4 * 1024, false);
    EXPECT_EQ(positioned - sequential,
              StorageTimingModel::AverageSeek + StorageTimingModel::AverageRotation);
}

TEST(StorageSchedulerTest, UsesFiosDefaultChunkSize) {
    EXPECT_EQ(StorageTimingModel::MaxChunkSize, 512u * 1024u);
    constexpr StorageTimingModel model{100};
    EXPECT_EQ(model.TransferDuration(StorageTimingModel::MaxChunkSize),
              std::chrono::milliseconds{5});
}

TEST(StorageSchedulerTest, PriorityMappingClampsToSdkRange) {
    EXPECT_EQ(Core::FileSys::StoragePriorityIndex(-1'000), 0u);
    EXPECT_EQ(Core::FileSys::StoragePriorityIndex(-128), 0u);
    EXPECT_EQ(Core::FileSys::StoragePriorityIndex(0), 128u);
    EXPECT_EQ(Core::FileSys::StoragePriorityIndex(127), 255u);
    EXPECT_EQ(Core::FileSys::StoragePriorityIndex(1'000), 255u);
}

TEST(StorageSchedulerTest, BandwidthDefaultsToNativeSpeed) {
    EmulatorSettingsImpl settings;
    EXPECT_EQ(settings.GetApp0ReadBandwidthMiBps(), 0u);
}

TEST(StorageSchedulerTest, BandwidthSupportsPerGameOverride) {
    EmulatorSettingsImpl settings;
    settings.SetApp0ReadBandwidthMiBps(100);
    settings.SetApp0ReadBandwidthMiBps(75, true);

    settings.SetConfigMode(ConfigMode::Global);
    EXPECT_EQ(settings.GetApp0ReadBandwidthMiBps(), 100u);
    settings.SetConfigMode(ConfigMode::Default);
    EXPECT_EQ(settings.GetApp0ReadBandwidthMiBps(), 75u);
    settings.SetConfigMode(ConfigMode::Clean);
    EXPECT_EQ(settings.GetApp0ReadBandwidthMiBps(), 0u);
}

TEST(StorageSchedulerTest, BandwidthIsSerializedAndOverrideable) {
    GeneralSettings settings;
    const nlohmann::json json = settings;
    EXPECT_EQ(json.at("app0_read_bandwidth_mibps"), 0u);

    const auto overrides = settings.GetOverrideableFields();
    EXPECT_NE(std::ranges::find_if(overrides,
                                   [](const OverrideItem& item) {
                                       return std::string{item.key} == "app0_read_bandwidth_mibps";
                                   }),
              overrides.end());
}

} // namespace
