// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "common/types.h"

namespace Vulkan {

/// Places the guest vblank so that each frame is ready shortly before the display takes it,
/// instead of a whole refresh early.
///
/// For every displayed guest frame it gets three host times: the vblank that latched the frame,
/// the moment the GPU finished its presentation work and the moment the display took it. The
/// display runs on its own cadence when it takes frames on a fixed grid of refreshes and holds
/// finished frames until the next one: a fixed refresh rate, or a variable one at its ceiling.
/// The grid is the refresh period the window system reports, refined by the display times.
/// While the display runs on its own cadence and its period is a multiple or a divisor of the
/// guest vblank period, the pacer steers the latch-to-display latency towards the production
/// time of the frames plus a margin. A display that shows frames as soon as they are ready
/// follows the guest instead, so the pacer stays out of the way.
///
/// Only the thread that waits for presents uses it.
class DisplayPacer {
public:
    explicit DisplayPacer(s64 vblank_period_ns);

    struct Result {
        /// The display takes frames on a grid of its own that the guest vblank can lock to.
        bool locked;
        /// Signed delay to add to the next guest vblank. Positive moves it later.
        s64 correction_ns;
    };

    struct Stats {
        u32 samples;
        u32 coherent;
        u32 held;
        u32 history;
        s64 production_p95_ns;
        s64 corrections_ns;
    };

    /// Refresh period the window system reports for the display; 0 when unknown.
    void SetNominalDisplayPeriod(s64 period_ns);

    Result AddSample(s64 latch_ns, s64 ready_ns, s64 display_ns);

    void Reset();

    [[nodiscard]] bool IsLocked() const {
        return locked;
    }

    /// Measured refresh period of the display.
    [[nodiscard]] s64 DisplayPeriod() const {
        return display_period_ns;
    }

    /// Time a frame should be latched before the scanout that shows it.
    [[nodiscard]] s64 Lead() const {
        return target_ns;
    }

    /// Returns the counters gathered since the previous call and clears them.
    [[nodiscard]] Stats TakeStats();

private:
    static constexpr u32 Window = 64;
    static constexpr u32 History = 32;

    [[nodiscard]] s64 ProductionPercentile95() const;
    [[nodiscard]] bool IsCommensurate() const;

    s64 vblank_period_ns;
    s64 nominal_display_period_ns{};
    s64 display_period_ns{};
    s64 last_display_ns{};
    std::array<s64, Window> production{};
    u32 count{};
    u32 index{};
    u64 coherent_history{};
    u64 held_history{};
    u32 history_count{};
    bool locked{};
    s64 target_ns{};
    s64 miss_boost_ns{};
    s64 budget_ns{};
    u32 stat_samples{};
    s64 stat_corrections_ns{};
};

} // namespace Vulkan
