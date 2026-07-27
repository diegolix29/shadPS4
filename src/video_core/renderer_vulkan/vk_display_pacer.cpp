// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cstdlib>

#include "video_core/renderer_vulkan/vk_display_pacer.h"

namespace Vulkan {

namespace {

/// Two display times more than this apart do not tell anything about the grid.
constexpr s64 MaxIntervalNs = 250'000'000;
/// A display time this close to the grid of refreshes lies on it.
constexpr s64 CoherenceToleranceNs = 300'000;
/// The refined refresh period stays this close to the reported one, in parts per thousand.
constexpr s64 MaxPeriodDeviationPermille = 10;
constexpr s64 PeriodFilter = 32;
/// A finished frame that waits this long for the display was held back by its cadence. A
/// display that follows the frames takes them within a fraction of this.
constexpr s64 HeldSlackNs = 1'000'000;
/// Time a frame should be ready before the display takes it.
constexpr s64 MarginNs = 1'500'000;
constexpr s64 MinTargetNs = 2'000'000;
/// A frame that finished after its target missed its scanout; the target backs off.
constexpr s64 MissBoostStepNs = 500'000;
constexpr s64 MaxMissBoostNs = 3'000'000;
constexpr s64 MissBoostDecayNs = 1'000;
constexpr s64 MaxSlewNs = 500'000;
constexpr s64 DeadbandNs = 50'000;
constexpr u64 HistoryMask = (u64{1} << 32) - 1;
/// The guest vblank can only lock to a display whose period is within this many parts per
/// thousand of a multiple or a divisor of its own.
constexpr s64 MaxRatioErrorPermille = 6;
constexpr s64 MaxRatio = 4;

} // Anonymous namespace

DisplayPacer::DisplayPacer(const s64 vblank_period_ns_) : vblank_period_ns{vblank_period_ns_} {}

void DisplayPacer::SetNominalDisplayPeriod(const s64 period_ns) {
    // The reported rate may be rounded differently between queries of the same mode.
    const s64 difference = std::abs(period_ns - nominal_display_period_ns);
    if (period_ns == nominal_display_period_ns ||
        (period_ns != 0 && nominal_display_period_ns != 0 &&
         difference * 1000 <= nominal_display_period_ns * 5)) {
        return;
    }
    nominal_display_period_ns = period_ns;
    Reset();
}

void DisplayPacer::Reset() {
    display_period_ns = nominal_display_period_ns;
    last_display_ns = 0;
    count = 0;
    index = 0;
    coherent_history = 0;
    held_history = 0;
    history_count = 0;
    locked = false;
    target_ns = 0;
    miss_boost_ns = 0;
    budget_ns = 0;
}

s64 DisplayPacer::ProductionPercentile95() const {
    if (count == 0) {
        return 0;
    }
    std::array<s64, Window> sorted;
    std::copy_n(production.begin(), count, sorted.begin());
    const u32 k = (count * 95) / 100;
    std::nth_element(sorted.begin(), sorted.begin() + k, sorted.begin() + count);
    return sorted[k];
}

bool DisplayPacer::IsCommensurate() const {
    if (display_period_ns <= 0 || vblank_period_ns <= 0) {
        return false;
    }
    const s64 longer = std::max(display_period_ns, vblank_period_ns);
    const s64 shorter = std::min(display_period_ns, vblank_period_ns);
    const s64 ratio = (longer + shorter / 2) / shorter;
    return ratio >= 1 && ratio <= MaxRatio &&
           std::abs(longer - ratio * shorter) * 1000 <= longer * MaxRatioErrorPermille;
}

DisplayPacer::Stats DisplayPacer::TakeStats() {
    const Stats stats{
        .samples = stat_samples,
        .coherent = static_cast<u32>(std::popcount(coherent_history)),
        .held = static_cast<u32>(std::popcount(held_history)),
        .history = history_count,
        .production_p95_ns = ProductionPercentile95(),
        .corrections_ns = stat_corrections_ns,
    };
    stat_samples = 0;
    stat_corrections_ns = 0;
    return stats;
}

DisplayPacer::Result DisplayPacer::AddSample(const s64 latch_ns, const s64 ready_ns,
                                             const s64 display_ns) {
    ++stat_samples;
    const s64 frame_production = std::max<s64>(ready_ns - latch_ns, 0);
    const s64 frame_slack = std::max<s64>(display_ns - std::max(ready_ns, latch_ns), 0);
    const s64 latency = display_ns - latch_ns;
    production[index] = frame_production;
    index = (index + 1) % Window;
    count = std::min(count + 1, Window);

    // A display with a cadence of its own takes frames on a grid of refreshes, whatever the
    // frames do. One that follows the frames takes them at arbitrary times.
    bool coherent = false;
    if (display_period_ns > 0 && last_display_ns != 0) {
        const s64 interval = display_ns - last_display_ns;
        if (interval > 0 && interval < MaxIntervalNs) {
            const s64 refreshes = (interval + display_period_ns / 2) / display_period_ns;
            const s64 residual = interval - refreshes * display_period_ns;
            coherent = refreshes >= 1 && std::abs(residual) <=
                                             std::max(CoherenceToleranceNs, display_period_ns / 25);
            if (coherent) {
                display_period_ns += (interval / refreshes - display_period_ns) / PeriodFilter;
                const s64 deviation =
                    nominal_display_period_ns * MaxPeriodDeviationPermille / 1000;
                display_period_ns =
                    std::clamp(display_period_ns, nominal_display_period_ns - deviation,
                               nominal_display_period_ns + deviation);
            }
        }
    }
    last_display_ns = display_ns;

    const bool held = frame_slack > HeldSlackNs;
    coherent_history = ((coherent_history << 1) | (coherent ? 1U : 0U)) & HistoryMask;
    held_history = ((held_history << 1) | (held ? 1U : 0U)) & HistoryMask;
    history_count = std::min(history_count + 1, History);
    const u32 coherent_count = static_cast<u32>(std::popcount(coherent_history));
    const u32 held_count = static_cast<u32>(std::popcount(held_history));

    // The guest vblank may drift from nominal by at most 0.5% on average, plus one initial
    // half-period phase move.
    const s64 budget_cap = vblank_period_ns / 2 + 1'000'000;
    const bool commensurate = IsCommensurate();
    if (!locked) {
        if (commensurate && history_count >= History / 2 &&
            coherent_count * 4 >= history_count * 3 && held_count * 3 >= history_count) {
            locked = true;
            budget_ns = budget_cap;
        }
    } else if (!commensurate ||
               (history_count >= History && (coherent_count < History / 2 || held_count < 4))) {
        locked = false;
    }
    budget_ns = std::min(budget_ns + vblank_period_ns / 200, budget_cap);
    if (!locked) {
        target_ns = 0;
        miss_boost_ns = 0;
        return {.locked = false, .correction_ns = 0};
    }

    if (target_ns != 0 && frame_production > target_ns) {
        miss_boost_ns = std::min(miss_boost_ns + MissBoostStepNs, MaxMissBoostNs);
    } else {
        miss_boost_ns = std::max<s64>(miss_boost_ns - MissBoostDecayNs, 0);
    }
    const s64 needed = ProductionPercentile95() + MarginNs + miss_boost_ns;
    const s64 max_target = display_period_ns * 9 / 10;
    target_ns = std::clamp(needed, MinTargetNs, max_target);
    if (needed > max_target) {
        // The GPU cannot finish frames within a refresh, so the phase makes no difference.
        return {.locked = true, .correction_ns = 0};
    }

    // Only the phase within the shorter period is steered: a frame shown whole refreshes later
    // sits behind another frame, which the vblank thread resolves by holding a flip.
    const s64 phase_period = std::min(display_period_ns, vblank_period_ns);
    s64 error = (latency - target_ns) % phase_period;
    if (error > phase_period / 2) {
        error -= phase_period;
    } else if (error < -phase_period / 2) {
        error += phase_period;
    }
    if (std::abs(error) < DeadbandNs) {
        return {.locked = true, .correction_ns = 0};
    }
    s64 correction = std::clamp(error / 8, -MaxSlewNs, MaxSlewNs);
    correction = std::clamp(correction, -budget_ns, budget_ns);
    budget_ns -= std::abs(correction);
    stat_corrections_ns += correction;
    return {.locked = true, .correction_ns = correction};
}

} // namespace Vulkan
