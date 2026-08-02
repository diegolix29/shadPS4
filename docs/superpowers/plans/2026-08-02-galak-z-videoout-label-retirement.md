# Galak-Z VideoOut Label Retirement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire PS4 VideoOut flip labels at the next scanout vblank with generation safety, eliminating Galak-Z's deterministic same-buffer PM4 deadlock without title-specific behavior.

**Architecture:** A pure `FlipLabelTracker` records generations when Liverpool executes the actual PM4 label-lock write. EOP flip requests carry that generation into the Present thread, which preserves immediate previous-buffer release and schedules current-buffer retirement for the next vblank. A delayed retirement clears guest memory only when its captured generation remains current.

**Tech Stack:** C++23, GoogleTest, CMake/CTest, shadPS4 VideoOut and Liverpool PM4, ARM64 FEX runtime, Android Gradle, ADB.

## Global Constraints

- Preserve upstream hardware-tested previous-buffer clearing.
- Track ownership at actual PM4 `WRITE_DATA label = 1`, never at callback registration.
- No game IDs, guest PCs, absolute addresses, wait timeouts, or forced PM4 completion.
- Use `VideoOutPort::port_mutex` for tracker transitions; release it before `SignalVoLabel()` acquires `vo_mutex`.
- Reset tracker ownership on close, buffer registration, and buffer unregistration.
- Device success requires sustained 60+ FPS in representative gameplay with no visible graphical glitches.
- Before any Android Gradle build, run the repository-mandated runtime build and verifier.
- Before installation, prove the APK contains both `assets/runtime/manifest.json` and `assets/runtime/runtime.zip`.
- Preserve all unrelated dirty-worktree files and submodule changes.

---

## File Map

- Create `src/core/libraries/videoout/flip_label_tracker.h`: pure generation and pending-retirement state machine.
- Create `tests/videoout/test_flip_label_tracker.cpp`: deterministic lifecycle tests independent of Vulkan.
- Modify `tests/CMakeLists.txt`: add lightweight `shadps4_videoout_label_test` target.
- Modify `src/core/libraries/videoout/driver.h`: embed tracker, map label addresses, carry request generation, and declare retirement helper.
- Modify `src/core/libraries/videoout/driver.cpp`: reset state, preserve previous release, schedule/take next-vblank retirement.
- Modify `src/core/libraries/videoout/video_out.cpp`: capture executed lock generation in EOP callback.
- Modify `src/video_core/amdgpu/liverpool.cpp`: record exact label lock before PM4 writes `1`.
- Use `runtime/tests/fex-galak-z-semaphore-signal-source.test.mjs`: regression test for already-fixed CPU signal delivery.
- Produce ignored evidence under `android/BachataS4/session-logs-galakz-*` and `android/BachataS4/device-diagnostics/`.

---

### Task 1: Build the Pure Flip-Label Tracker with TDD

**Files:**
- Create: `tests/videoout/test_flip_label_tracker.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `src/core/libraries/videoout/flip_label_tracker.h`

**Interfaces:**
- Produces: `Libraries::VideoOut::FlipLabelTracker`
- Produces: `Generation RecordLock(u32 index)`
- Produces: `Generation CurrentGeneration(u32 index) const`
- Produces: `void ScheduleRetirement(s32 index, Generation generation, u64 due_vblank)`
- Produces: `void CancelRetirement(u32 index)`
- Produces: `std::optional<DueRetirement> TakeDueRetirement(u64 current_vblank)`
- Produces: `void ResetBuffer(u32 index)` and `void Reset()`

- [ ] **Step 1: Add the failing behavior tests**

Create `tests/videoout/test_flip_label_tracker.cpp`:

```cpp
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/libraries/videoout/flip_label_tracker.h"

namespace Libraries::VideoOut {
namespace {

TEST(FlipLabelTracker, RetiresPresentedGenerationAtNextVblank) {
    FlipLabelTracker tracker;
    const auto generation = tracker.RecordLock(0);
    tracker.ScheduleRetirement(0, generation, 11);

    EXPECT_FALSE(tracker.TakeDueRetirement(10).has_value());
    const auto due = tracker.TakeDueRetirement(11);
    ASSERT_TRUE(due.has_value());
    EXPECT_EQ(due->index, 0u);
    EXPECT_EQ(due->generation, generation);
}

TEST(FlipLabelTracker, RelockInvalidatesPendingRetirement) {
    FlipLabelTracker tracker;
    const auto first = tracker.RecordLock(1);
    tracker.ScheduleRetirement(1, first, 20);

    const auto second = tracker.RecordLock(1);
    EXPECT_GT(second, first);
    EXPECT_FALSE(tracker.TakeDueRetirement(20).has_value());
    EXPECT_EQ(tracker.CurrentGeneration(1), second);
}

TEST(FlipLabelTracker, SupersedingBufferCancelsDelayedRetirement) {
    FlipLabelTracker tracker;
    const auto generation = tracker.RecordLock(2);
    tracker.ScheduleRetirement(2, generation, 31);

    tracker.CancelRetirement(2);
    EXPECT_FALSE(tracker.TakeDueRetirement(31).has_value());
}

TEST(FlipLabelTracker, ResetBufferClearsGenerationAndOwnership) {
    FlipLabelTracker tracker;
    const auto generation = tracker.RecordLock(3);
    tracker.ScheduleRetirement(3, generation, 41);

    tracker.ResetBuffer(3);
    EXPECT_EQ(tracker.CurrentGeneration(3), FlipLabelTracker::InvalidGeneration);
    EXPECT_FALSE(tracker.TakeDueRetirement(41).has_value());
}

TEST(FlipLabelTracker, OtherBufferLockDoesNotInvalidateCurrentRetirement) {
    FlipLabelTracker tracker;
    const auto first = tracker.RecordLock(4);
    tracker.ScheduleRetirement(4, first, 51);
    EXPECT_EQ(tracker.RecordLock(5), 1u);

    const auto due = tracker.TakeDueRetirement(51);
    ASSERT_TRUE(due.has_value());
    EXPECT_EQ(due->index, 4u);
}

TEST(FlipLabelTracker, InvalidGenerationNeverSchedulesRetirement) {
    FlipLabelTracker tracker;
    tracker.ScheduleRetirement(0, FlipLabelTracker::InvalidGeneration, 1);
    tracker.ScheduleRetirement(-1, 7, 1);
    tracker.ScheduleRetirement(static_cast<s32>(MaxDisplayBuffers), 7, 1);

    EXPECT_FALSE(tracker.TakeDueRetirement(1).has_value());
}

} // namespace
} // namespace Libraries::VideoOut
```

Update `tests/CMakeLists.txt` before the shared target loops:

```cmake
set(VIDEOOUT_LABEL_TEST_SOURCES
    videoout/test_flip_label_tracker.cpp
)

add_executable(shadps4_videoout_label_test ${VIDEOOUT_LABEL_TEST_SOURCES})
```

Add `shadps4_videoout_label_test` to the initial `TEST_TARGETS` list and link it:

```cmake
target_link_libraries(shadps4_videoout_label_test PRIVATE
    GTest::gtest_main
)
```

- [ ] **Step 2: Run the test target and verify RED**

Run:

```bash
cmake --build build --target shadps4_videoout_label_test -j2
```

Expected: compilation fails because
`core/libraries/videoout/flip_label_tracker.h` does not exist. This proves the
new test target is active.

- [ ] **Step 3: Add the minimal tracker implementation**

Create `src/core/libraries/videoout/flip_label_tracker.h`:

```cpp
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>

#include "core/libraries/videoout/buffer.h"

namespace Libraries::VideoOut {

class FlipLabelTracker {
public:
    using Generation = u64;
    static constexpr Generation InvalidGeneration = 0;

    struct DueRetirement {
        u32 index;
        Generation generation;
    };

    Generation RecordLock(u32 index) {
        if (index >= MaxDisplayBuffers) {
            return InvalidGeneration;
        }
        auto& generation = generations[index];
        if (++generation == InvalidGeneration) {
            ++generation;
        }
        return generation;
    }

    [[nodiscard]] Generation CurrentGeneration(u32 index) const {
        return index < MaxDisplayBuffers ? generations[index] : InvalidGeneration;
    }

    void ScheduleRetirement(s32 index, Generation generation, u64 due_vblank) {
        if (index < 0 || static_cast<std::size_t>(index) >= MaxDisplayBuffers ||
            generation == InvalidGeneration) {
            return;
        }
        pending = PendingRetirement{
            .index = static_cast<u32>(index),
            .generation = generation,
            .due_vblank = due_vblank,
        };
    }

    void CancelRetirement(u32 index) {
        if (pending && pending->index == index) {
            pending.reset();
        }
    }

    [[nodiscard]] std::optional<DueRetirement> TakeDueRetirement(u64 current_vblank) {
        if (!pending || current_vblank < pending->due_vblank) {
            return std::nullopt;
        }
        const auto retirement = *pending;
        pending.reset();
        if (CurrentGeneration(retirement.index) != retirement.generation) {
            return std::nullopt;
        }
        return DueRetirement{retirement.index, retirement.generation};
    }

    void ResetBuffer(u32 index) {
        if (index >= MaxDisplayBuffers) {
            return;
        }
        generations[index] = InvalidGeneration;
        CancelRetirement(index);
    }

    void Reset() {
        generations.fill(InvalidGeneration);
        pending.reset();
    }

private:
    struct PendingRetirement {
        u32 index;
        Generation generation;
        u64 due_vblank;
    };

    std::array<Generation, MaxDisplayBuffers> generations{};
    std::optional<PendingRetirement> pending;
};

} // namespace Libraries::VideoOut
```

- [ ] **Step 4: Build and run GREEN tests**

Run:

```bash
cmake --build build --target shadps4_videoout_label_test -j2
ctest --test-dir build --output-on-failure -R FlipLabelTracker
```

Expected: target builds; six `FlipLabelTracker.*` tests pass.

- [ ] **Step 5: Commit the tracker slice**

```bash
git add tests/CMakeLists.txt tests/videoout/test_flip_label_tracker.cpp \
  src/core/libraries/videoout/flip_label_tracker.h
git commit -m "feat(videoout): track flip label ownership"
```

---

### Task 2: Record Generations at the PM4 Lock Boundary

**Files:**
- Modify: `src/core/libraries/videoout/driver.h`
- Modify: `src/video_core/amdgpu/liverpool.cpp`

**Interfaces:**
- Consumes: `FlipLabelTracker::RecordLock`
- Produces: `std::optional<u32> VideoOutPort::VoLabelIndex(const void* address) const`
- Produces: `FlipLabelTracker::Generation VideoOutPort::RecordVoLabelLock(const void* address)`
- Produces: `FlipLabelTracker::Generation VideoOutPort::CurrentVoLabelGeneration(u32 index)`

- [ ] **Step 1: Extend tracker tests for exact invalid-index behavior**

Append to `tests/videoout/test_flip_label_tracker.cpp`:

```cpp
TEST(FlipLabelTracker, InvalidLockDoesNotChangeValidGenerations) {
    FlipLabelTracker tracker;
    EXPECT_EQ(tracker.RecordLock(static_cast<u32>(MaxDisplayBuffers)),
              FlipLabelTracker::InvalidGeneration);
    EXPECT_EQ(tracker.CurrentGeneration(0), FlipLabelTracker::InvalidGeneration);
}
```

- [ ] **Step 2: Add address mapping and synchronized tracker access**

In `driver.h`, include the tracker and add it to `VideoOutPort`:

```cpp
#include "core/libraries/videoout/flip_label_tracker.h"

FlipLabelTracker flip_label_tracker;
```

Replace pointer-order comparison in `IsVoLabel` with exact byte-range mapping:

```cpp
[[nodiscard]] std::optional<u32> VoLabelIndex(const void* address) const {
    const auto value = reinterpret_cast<uintptr_t>(address);
    const auto start = reinterpret_cast<uintptr_t>(buffer_labels.data());
    const auto end = start + sizeof(buffer_labels);
    if (value < start || value >= end || (value - start) % sizeof(u64) != 0) {
        return std::nullopt;
    }
    return static_cast<u32>((value - start) / sizeof(u64));
}

bool IsVoLabel(const u64* address) const {
    return VoLabelIndex(address).has_value();
}

FlipLabelTracker::Generation RecordVoLabelLock(const void* address) {
    const auto index = VoLabelIndex(address);
    if (!index) {
        return FlipLabelTracker::InvalidGeneration;
    }
    std::scoped_lock lock{port_mutex};
    return flip_label_tracker.RecordLock(*index);
}

FlipLabelTracker::Generation CurrentVoLabelGeneration(u32 index) {
    std::scoped_lock lock{port_mutex};
    return flip_label_tracker.CurrentGeneration(index);
}
```

- [ ] **Step 3: Record the generation immediately before the PM4 lock write**

In Liverpool's graphics `PM4ItOpcode::WriteData` handler, insert this before
`std::memcpy`:

```cpp
if (vo_port != nullptr && data_size >= sizeof(u32) && write_data->data[0] == 1) {
    vo_port->RecordVoLabelLock(address);
}
```

Keep generic `WriteData` behavior unchanged after this call.

- [ ] **Step 4: Compile focused and production targets**

Run:

```bash
cmake --build build --target shadps4_videoout_label_test -j2
cmake --build build --target shadps4 -j2
```

Expected: both targets build without warnings promoted to errors.

- [ ] **Step 5: Commit PM4 ownership observation**

```bash
git add src/core/libraries/videoout/driver.h \
  src/video_core/amdgpu/liverpool.cpp \
  tests/videoout/test_flip_label_tracker.cpp
git commit -m "fix(videoout): observe PM4 label locks"
```

---

### Task 3: Carry Generation and Retire at Next Vblank

**Files:**
- Modify: `src/core/libraries/videoout/driver.h`
- Modify: `src/core/libraries/videoout/driver.cpp`
- Modify: `src/core/libraries/videoout/video_out.cpp`

**Interfaces:**
- Consumes: `VideoOutPort::CurrentVoLabelGeneration`
- Produces: `SubmitFlip(..., bool is_eop, FlipLabelTracker::Generation generation)`
- Produces: `Request::label_generation`
- Produces: `RetireScanoutLabel(VideoOutPort* port, u64 current_vblank)`

- [ ] **Step 1: Add a failing overwrite test for stale pending ownership**

Append:

```cpp
TEST(FlipLabelTracker, NewPresentationReplacesOlderPendingRetirement) {
    FlipLabelTracker tracker;
    const auto first = tracker.RecordLock(0);
    tracker.ScheduleRetirement(0, first, 5);
    const auto second = tracker.RecordLock(1);
    tracker.ScheduleRetirement(1, second, 6);

    EXPECT_FALSE(tracker.TakeDueRetirement(5).has_value());
    const auto due = tracker.TakeDueRetirement(6);
    ASSERT_TRUE(due.has_value());
    EXPECT_EQ(due->index, 1u);
}
```

- [ ] **Step 2: Carry generation through EOP request creation**

In `driver.h`, add a defaulted generation parameter to `SubmitFlip` and
`SubmitFlipInternal`, add `label_generation` to `Request`, and change `Flip` to
receive the presenting vblank:

```cpp
bool SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop = false,
                FlipLabelTracker::Generation label_generation =
                    FlipLabelTracker::InvalidGeneration);

struct Request {
    Vulkan::Frame* frame;
    VideoOutPort* port;
    s64 flip_arg;
    s32 index;
    bool eop;
    FlipLabelTracker::Generation label_generation;
    // Existing operator bool remains unchanged.
};

void Flip(const Request& req, u64 current_vblank);
void SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop,
                        FlipLabelTracker::Generation label_generation);
void RetireScanoutLabel(VideoOutPort* port, u64 current_vblank);
```

In `video_out.cpp`, capture the generation after `PatchedFlip` proves the label
lock executed:

```cpp
const auto generation = port->CurrentVoLabelGeneration(buf_id);
ASSERT_MSG(generation != FlipLabelTracker::InvalidGeneration,
           "Missing VideoOut label generation for buffer {}", buf_id);
const auto result = driver->SubmitFlip(port, buf_id, flip_arg, true, generation);
```

Thread `label_generation` through `SubmitFlip`, its GPU-dispatch lambda,
`SubmitFlipInternal`, and the queued `Request` initializer.

- [ ] **Step 3: Implement generation-safe next-vblank retirement**

Add to `driver.cpp`:

```cpp
void VideoOutDriver::RetireScanoutLabel(VideoOutPort* port, u64 current_vblank) {
    std::optional<FlipLabelTracker::DueRetirement> retirement;
    {
        std::scoped_lock lock{port->port_mutex};
        retirement = port->flip_label_tracker.TakeDueRetirement(current_vblank);
        if (retirement) {
            port->buffer_labels[retirement->index] = 0;
        }
    }
    if (retirement) {
        port->SignalVoLabel();
    }
}
```

After flip events in `Flip`, replace the unlocked previous-label block with:

```cpp
bool signal_label = false;
{
    std::scoped_lock lock{port->port_mutex};
    if (port->prev_index != -1) {
        const auto previous = static_cast<u32>(port->prev_index);
        port->buffer_labels[previous] = 0;
        port->flip_label_tracker.CancelRetirement(previous);
        signal_label = true;
    }
    port->prev_index = req.index;
    if (req.eop && req.index >= 0) {
        port->flip_label_tracker.ScheduleRetirement(
            req.index, req.label_generation, current_vblank + 1);
    }
}
if (signal_label) {
    port->SignalVoLabel();
}
```

In `PresentThread`, after the guest-pause branch and before dequeuing a request:

```cpp
auto& vblank_status = main_port.vblank_status;
RetireScanoutLabel(&main_port, vblank_status.count);
```

Change the request call to:

```cpp
Flip(request, vblank_status.count);
```

- [ ] **Step 4: Reset lifecycle state at every port boundary**

In `Close`, protect and reset `prev_index`, labels, `flip_status`, and tracker
under `port_mutex`, then call `SignalVoLabel()` after releasing the mutex.

In each `RegisterBuffers` iteration, protect these two operations together:

```cpp
{
    std::scoped_lock lock{port->port_mutex};
    port->buffer_labels[startIndex + i] = 0;
    port->flip_label_tracker.ResetBuffer(startIndex + i);
}
port->SignalVoLabel();
```

In `UnregisterBuffers`, iterate by index under `port_mutex`; for each matching
slot, set `group_index = -1`, clear its label, and call `ResetBuffer(index)`.
Notify once after releasing the lock when any buffer changed.

- [ ] **Step 5: Run focused, full unit, and production compile checks**

Run:

```bash
cmake --build build --target shadps4_videoout_label_test -j2
ctest --test-dir build --output-on-failure -R FlipLabelTracker
cmake --build build --target shadps4 -j2
git diff --check
```

Expected: all tracker tests pass, production target links, diff has no whitespace
errors.

- [ ] **Step 6: Commit vblank retirement**

```bash
git add src/core/libraries/videoout/driver.h \
  src/core/libraries/videoout/driver.cpp \
  src/core/libraries/videoout/video_out.cpp \
  tests/videoout/test_flip_label_tracker.cpp
git commit -m "fix(videoout): retire labels after scanout"
```

---

### Task 4: Regression and Runtime Verification

**Files:**
- Verify: `runtime/tests/fex-galak-z-semaphore-signal-source.test.mjs`
- Verify: `runtime/locks/components.lock.json`
- Verify: runtime and APK outputs only

**Interfaces:**
- Consumes: completed generic VideoOut fix.
- Produces: test and packaged-runtime evidence suitable for device installation.

- [ ] **Step 1: Run focused source and unit regressions**

```bash
node --test runtime/tests/fex-galak-z-semaphore-signal-source.test.mjs
ctest --test-dir build --output-on-failure -R 'FlipLabelTracker|Pm4Stall'
```

Expected: all selected tests pass.

- [ ] **Step 2: Run full configured CTest suite**

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Expected: zero failed tests. Any unrelated pre-existing failure must be recorded
with its exact test name and reproduced against the pre-fix commit before it can
be classified as unrelated.

- [ ] **Step 3: Build and verify managed runtime from repository root**

```bash
git submodule update --init --recursive --jobs 8
runtime/scripts/build-runtime-debian.sh
node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json
```

Expected: runtime build succeeds and verifier reports every locked component as
valid.

- [ ] **Step 4: Build Android tests, lint, and APK**

```bash
cd android/BachataS4
./gradlew test lintDebug assembleDebug
```

Expected: Gradle `BUILD SUCCESSFUL`.

- [ ] **Step 5: Verify packaged runtime assets before installation**

From `android/BachataS4`, run:

```bash
unzip -l app/build/outputs/apk/playstore/debug/app-playstore-debug.apk \
  | grep -E 'assets/runtime/(manifest\.json|runtime\.zip)'
```

Expected: one `manifest.json` entry and one `runtime.zip` entry. Do not install
if either is absent.

- [ ] **Step 6: Review implementation diff before device mutation**

```bash
git status --short
git diff HEAD~2 -- src/core/libraries/videoout src/video_core/amdgpu/liverpool.cpp \
  tests/videoout tests/CMakeLists.txt
git diff --check HEAD~2
```

Expected: only planned files changed; no title-specific condition, timeout, or
absolute address exists.

---

### Task 5: Device Qualification to Playable 60+ FPS

**Files:**
- Create ignored evidence: `android/BachataS4/session-logs-galakz-fixed/`
- Create ignored screenshots: `android/BachataS4/device-diagnostics/galakz-fixed-*.png`

**Interfaces:**
- Consumes: APK verified to contain both runtime assets.
- Produces: authoritative boot, gameplay, FPS, visual, and repeat-launch proof.

- [ ] **Step 1: Install verified APK and confirm version/runtime identity**

```bash
adb -s 7d6afed8 install -r \
  android/BachataS4/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk
adb -s 7d6afed8 shell dumpsys package com.bachatas4.android \
  | grep -E 'versionName|versionCode|lastUpdateTime'
```

Expected: install succeeds and `lastUpdateTime` matches this build.

- [ ] **Step 2: Launch Galak-Z with clean logcat**

```bash
adb -s 7d6afed8 shell am force-stop com.bachatas4.android
adb -s 7d6afed8 logcat -c
adb -s 7d6afed8 shell am start \
  -n com.bachatas4.android/.DirectLaunchActivity --es game_id CUSA03146
```

Expected: backend reaches Running and remains alive.

- [ ] **Step 3: Capture startup transition evidence**

Capture screenshots at 30, 60, 90, and 120 seconds without blocking longer than
30 seconds per wait. Pull session logs after the startup boundary:

```bash
ADB_OVERRIDE=adb android/BachataS4/pull-session-logs.sh \
  --game CUSA03146 --output android/BachataS4/session-logs-galakz-fixed
```

Expected: startup advances beyond memory-test/FMV sequence; logs contain no
sustained VideoOut-label `WAIT_REG_MEM stalled` series.

- [ ] **Step 4: Reach interactive gameplay and measure sustained FPS**

Use controller input to leave menus and enter a representative combat/flight
scene. Capture at least 60 seconds of overlay samples and screenshots spanning
motion, effects, and camera movement.

Expected: overlay remains at or above 60 FPS for the representative interval,
excluding bounded loading transitions; input, audio, and frame progression stay
responsive.

- [ ] **Step 5: Inspect graphical and runtime health**

Check screenshots for corruption, tearing, missing geometry, black frames, and
shader artifacts. Search pulled logs:

```bash
rg -n '<Critical>|<Error>|ASSERT|SIGILL|SIGSEGV|GPU.*deadlock|WAIT_REG_MEM stalled|exitCode' \
  android/BachataS4/session-logs-galakz-fixed
```

Expected: no new fatal condition or sustained GPU wait. Repeated benign messages
must be understood before completion can be claimed.

- [ ] **Step 6: Repeat launch to exclude cache-only success**

Force-stop and repeat Steps 2-5 using the warmed cache. Capture a second session
directory and at least one gameplay screenshot.

Expected: second launch reaches gameplay and sustains 60+ FPS without visual
glitches.

- [ ] **Step 7: Final completion audit**

Verify every explicit goal item against current evidence:

```text
bootable: two launches reach interactive gameplay
startup: no 0-FPS VideoOut deadlock
performance: representative gameplay sustains 60+ FPS
graphics: screenshots and motion inspection show no glitches
packaging: APK contains both managed-runtime assets
regressions: focused and full tests pass
```

Only after all six lines have direct evidence, mark the goal complete. If a new
independent blocker appears, keep the goal active and return to systematic
diagnosis.
