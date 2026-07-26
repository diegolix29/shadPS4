# Crash Bandicoot Mspace and Turnip Compatibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the FEX `ENOSYS` fallback reached by Crash Bandicoot's libc
mspace calls with a bounded guest-backed allocator, then prove the title reaches
the start screen on the connected Turnip device without exit 133 or visible
corruption.

**Architecture:** Add an allocator-only component with opaque tombstoned handles,
host-side free-list metadata, and guest-region payload pointers. Thin
`libSceLibcInternal` wrappers register the existing PS4 NIDs for both native and
FEX builds. Renderer work is gated on a post-fix screenshot and log evidence,
because the current 1D/2D warning also reproduces on native Vulkan.

**Tech Stack:** C++23, GoogleTest, CMake/Ninja, shadPS4 HLE symbol registration,
FEXCore guest calls, Android Gradle, managed-runtime packaging, adb, Turnip
Vulkan.

## Global Constraints

- Follow
  `docs/superpowers/specs/2026-07-26-crash-bandicoot-mspace-turnip-design.md`.
- Preserve all pre-existing dirty submodules, untracked artifacts, logs, and
  user files.
- Do not change the generic FEX unsupported-HLE fallback.
- Do not hardcode `CUSA07399`, `0x31d000000`,
  `0xffffffffffffffe2`, or the observed arena capacity in production code.
- Do not return host heap memory as an mspace payload pointer.
- Do not reuse destroyed handle tokens during the process lifetime.
- Do not apply a renderer patch unless the post-fix device screenshot proves a
  visible defect and diagnostics identify its resource-shape cause.
- Do not create commits, push, or open a PR unless the user separately requests
  it.
- Process large logs with context-mode; never read a full session log directly.
- Before every APK build, rebuild and verify the managed runtime from the
  repository root.
- Never install an APK unless it contains both
  `assets/runtime/manifest.json` and `assets/runtime/runtime.zip`.
- Never bundle a Turnip driver in the APK or managed runtime.

---

### Task 1: Add the focused test target and first failing regression

**Files:**

- Modify: `tests/CMakeLists.txt:12-17`
- Modify: `tests/CMakeLists.txt:141-196`
- Create: `tests/core/test_libc_mspace.cpp`

**Interfaces:**

- Consumes: planned header
  `core/libraries/libc_internal/mspace.h`.
- Produces: CMake target `shadps4_libc_mspace_test` and the first executable
  behavioral specification for `MspaceCreate` and `MspaceMalloc`.

- [x] **Step 1: Register the focused test target**

Add `shadps4_libc_mspace_test` to `TEST_TARGETS`, define a source list containing
only `core/test_libc_mspace.cpp`, create the executable, and link
`GTest::gtest_main`.

```cmake
set(TEST_TARGETS
    shadps4_settings_test
    shadps4_gcn_test
    shadps4_guest_memory_validation_test
    shadps4_imgui_memory_test
    shadps4_libc_mspace_test
)

set(LIBC_MSPACE_TEST_SOURCES
    core/test_libc_mspace.cpp
)

add_executable(shadps4_libc_mspace_test ${LIBC_MSPACE_TEST_SOURCES})

target_link_libraries(shadps4_libc_mspace_test PRIVATE
    GTest::gtest_main
)
```

The existing `foreach(t ${TEST_TARGETS})` block supplies the include directory,
C++23 feature level, platform flags, and test discovery.

- [x] **Step 2: Write the first regression test**

```cpp
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "core/libraries/libc_internal/mspace.h"

namespace {

using Libraries::LibcInternal::MspaceCreate;
using Libraries::LibcInternal::MspaceMalloc;

TEST(LibcMspace, CreateAndMallocReturnGuestBackedPointer) {
    alignas(64) std::array<std::byte, 4096> storage{};

    void* handle = MspaceCreate("CrashArena", storage.data(), storage.size(), 0);
    ASSERT_NE(handle, nullptr);

    void* allocation = MspaceMalloc(handle, 128);
    ASSERT_NE(allocation, nullptr);

    const auto address = reinterpret_cast<std::uintptr_t>(allocation);
    const auto begin = reinterpret_cast<std::uintptr_t>(storage.data());
    EXPECT_GE(address, begin);
    EXPECT_LE(address + 128, begin + storage.size());
    EXPECT_EQ(address % 16, 0u);
}

} // namespace
```

- [x] **Step 3: Configure a dedicated test build**

Run:

```bash
cmake -S . -B runtime/build/shadps4-tests -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
  -DENABLE_BACHATA_RUNTIME=ON \
  -DENABLE_USERFAULTFD=OFF \
  -DENABLE_DISCORD_RPC=OFF \
  -DENABLE_UPDATER=OFF \
  -DENABLE_TESTS=ON
```

- [x] **Step 4: Verify the red state**

Run:

```bash
cmake --build runtime/build/shadps4-tests \
  --target shadps4_libc_mspace_test -j2
```

Expected: compilation fails because
`core/libraries/libc_internal/mspace.h` does not exist. This is the expected
missing-feature failure, not a CMake or dependency failure.

- [x] **Step 5: Check the test-only diff**

Run:

```bash
git diff --check -- tests/CMakeLists.txt tests/core/test_libc_mspace.cpp
```

Expected: no whitespace errors.

---

### Task 2: Implement the smallest guest-backed create/malloc path

**Files:**

- Create: `src/core/libraries/libc_internal/mspace.h`
- Create: `src/core/libraries/libc_internal/mspace.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

- Produces:
  `void* MspaceCreate(const char*, void*, std::size_t, u32)`,
  and `void* MspaceMalloc(void*, std::size_t)`.
- Keeps `MspaceDestroy`, `MspaceFree`, `MspaceCalloc`, `MspaceRealloc`,
  `MspaceMemalign`, and `MspaceMallocUsableSize` declared with temporary
  failure behavior so later tests compile and can drive each implementation.

- [x] **Step 1: Add the public allocator API**

```cpp
#pragma once

#include <cstddef>

#include "common/types.h"

namespace Libraries::LibcInternal {

void* MspaceCreate(const char* name, void* base, std::size_t capacity, u32 flags);
s32 MspaceDestroy(void* handle);
void* MspaceMalloc(void* handle, std::size_t size);
s32 MspaceFree(void* handle, void* pointer);
void* MspaceCalloc(void* handle, std::size_t count, std::size_t size);
void* MspaceRealloc(void* handle, void* pointer, std::size_t size);
void* MspaceMemalign(void* handle, std::size_t alignment, std::size_t size);
std::size_t MspaceMallocUsableSize(void* pointer);

} // namespace Libraries::LibcInternal
```

- [x] **Step 2: Add private arena and handle structures**

In `mspace.cpp`, create:

```cpp
namespace {

constexpr std::size_t DefaultAlignment = 16;
constexpr u32 KnownMspaceFlags = 0x0f;

struct Allocation {
    std::size_t size{};
};

class Arena {
public:
    Arena(std::string name, std::uintptr_t base, std::size_t capacity);

    void* Allocate(std::size_t size, std::size_t alignment);
    s32 Free(void* pointer);
    std::size_t UsableSize(void* pointer) const;

private:
    void InsertFreeBlock(std::size_t offset, std::size_t size);

    std::string name;
    std::uintptr_t base{};
    std::size_t capacity{};
    mutable std::mutex mutex;
    std::map<std::size_t, std::size_t> free_blocks;
    std::unordered_map<std::uintptr_t, Allocation> allocations;
};

struct HandleEntry {
    std::shared_ptr<Arena> arena;
};

std::mutex registry_mutex;
std::unordered_map<void*, std::unique_ptr<HandleEntry>> handles;

} // namespace
```

Destroyed entries stay in `handles` with `arena == nullptr`, so their token
addresses are never reused.

- [x] **Step 3: Implement guarded creation**

Creation must:

- reject null base;
- reject zero/unusable capacity;
- reject `base + capacity` overflow;
- reject unknown flag bits;
- copy at most 63 bytes of the optional name;
- create one initial free interval `[0, capacity)`;
- insert a new `HandleEntry` and return its stable address.

- [x] **Step 4: Implement aligned first-fit allocation**

Use absolute-address alignment so an unaligned guest base is handled correctly.
Normalize `size == 0` to one byte, check every addition for overflow, split
leading/trailing fragments, and store the exact requested size.

Helper shape:

```cpp
std::optional<std::uintptr_t> AlignUp(std::uintptr_t value,
                                      std::size_t alignment);
std::shared_ptr<Arena> ResolveArena(void* handle);
```

- [x] **Step 5: Add the implementation source to the test target**

```cmake
set(LIBC_MSPACE_TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/libraries/libc_internal/mspace.cpp
    core/test_libc_mspace.cpp
)
```

- [x] **Step 6: Keep unimplemented operations explicit**

Until later tasks, return:

- `1` from `MspaceDestroy` and `MspaceFree`;
- null from `MspaceCalloc`, `MspaceRealloc`, and `MspaceMemalign`;
- `0` from `MspaceMallocUsableSize`.

Do not fake success.

- [x] **Step 7: Verify the first test turns green**

Run:

```bash
cmake --build runtime/build/shadps4-tests \
  --target shadps4_libc_mspace_test -j2
ctest --test-dir runtime/build/shadps4-tests \
  --output-on-failure -R '^LibcMspace\.CreateAndMallocReturnGuestBackedPointer$'
```

Expected: one test passes.

---

### Task 3: Add free, coalescing, invalid-handle, and destroy behavior

**Files:**

- Modify: `tests/core/test_libc_mspace.cpp`
- Modify: `src/core/libraries/libc_internal/mspace.cpp`

**Interfaces:**

- Consumes: stable handle registry and `Arena::Allocate`.
- Produces: `MspaceFree` with ownership checks/coalescing and
  `MspaceDestroy` with permanent handle invalidation.

- [x] **Step 1: Write the free/reuse test**

Allocate three adjacent blocks, free the first two, and request a block that
fits only if the adjacent free intervals coalesce. Also assert the returned
block is inside the original buffer.

- [x] **Step 2: Verify the new test fails**

Run the focused test binary with:

```bash
runtime/build/shadps4-tests/tests/shadps4_libc_mspace_test \
  --gtest_filter='LibcMspace.FreeCoalescesAndReusesGuestStorage'
```

Expected: failure because `MspaceFree` returns `1`.

- [x] **Step 3: Implement validated free and coalescing**

`Arena::Free` must:

- treat null as success;
- reject pointers absent from the live allocation table;
- remove the live allocation;
- insert its exact interval;
- merge with the preceding and following free blocks;
- return `0` on success and `1` on invalid input.

- [x] **Step 4: Verify the free/reuse test passes**

Run the same filtered command. Expected: pass.

- [x] **Step 5: Write invalid-handle and destroy tests**

Cover:

- null/foreign handles;
- a double free;
- a pointer belonging to a different arena;
- successful destroy;
- calls through a destroyed token returning failure/null;
- repeated destroy returning `1`.

- [x] **Step 6: Verify the new tests fail for the intended cases**

Expected: the placeholder destroy behavior does not invalidate the token.

- [x] **Step 7: Implement tombstoned destroy**

Under `registry_mutex`, find the exact token, reject missing/tombstoned entries,
move the shared arena reference out, set `entry->arena = nullptr`, and return
`0`. Never erase the `HandleEntry`.

- [x] **Step 8: Run all lifecycle tests**

```bash
ctest --test-dir runtime/build/shadps4-tests \
  --output-on-failure -R '^LibcMspace\.'
```

Expected: create/malloc/free/destroy tests pass.

---

### Task 4: Add calloc and exhaustion/overflow behavior

**Files:**

- Modify: `tests/core/test_libc_mspace.cpp`
- Modify: `src/core/libraries/libc_internal/mspace.cpp`

**Interfaces:**

- Produces:
  `void* MspaceCalloc(void*, std::size_t, std::size_t)`.

- [x] **Step 1: Write the calloc test**

Fill the backing buffer with nonzero bytes, call `MspaceCalloc`, and verify only
the returned allocation is zeroed.

- [x] **Step 2: Verify the calloc test fails**

Expected: null from the temporary implementation.

- [x] **Step 3: Implement calloc**

Check `count * size` with `std::numeric_limits<std::size_t>::max() / count`,
normalize a zero product through the allocator's minimum-allocation rule,
allocate, and zero exactly the recorded requested extent.

- [x] **Step 4: Verify calloc passes**

Run the filtered calloc test. Expected: pass.

- [x] **Step 5: Write exhaustion and overflow tests**

Cover:

- an allocation larger than the arena;
- enough smaller allocations to exhaust it;
- `count * size` overflow;
- creation with null base, zero capacity, range overflow, and unknown flags;
- existing live allocations remaining intact after each failed request.

- [x] **Step 6: Verify the tests expose any missing guards**

Because Step 3 already mandates the overflow guard, verify the regression test
passes with the guard and would return a wrapped, non-null allocation if that
guard were absent. A crash is not acceptable evidence.

- [x] **Step 7: Add the missing guards and rerun**

Run:

```bash
ctest --test-dir runtime/build/shadps4-tests \
  --output-on-failure -R '^LibcMspace\.'
```

Expected: all current tests pass.

---

### Task 5: Add realloc semantics

**Files:**

- Modify: `tests/core/test_libc_mspace.cpp`
- Modify: `src/core/libraries/libc_internal/mspace.cpp`

**Interfaces:**

- Produces:
  `void* MspaceRealloc(void*, void*, std::size_t)`.

- [x] **Step 1: Write realloc edge-case tests**

Assert:

- `realloc(handle, nullptr, size)` behaves like malloc;
- `realloc(handle, pointer, 0)` frees and returns null;
- foreign pointers return null without altering either arena.

- [x] **Step 2: Verify the edge-case test fails**

Expected: the temporary null implementation fails the malloc-equivalent case.

- [x] **Step 3: Implement edge cases**

Delegate null pointers to `Allocate`, delegate zero size to `Free`, and validate
ownership before any copy.

- [x] **Step 4: Write shrink and in-place growth tests**

Verify shrinking preserves the prefix and frees the tail. Free a neighboring
block, then verify growth consumes that exact adjacent interval without moving.

- [x] **Step 5: Verify the new growth test fails**

Expected: no working resize path yet.

- [x] **Step 6: Implement shrink and adjacent growth**

Use allocator-internal helpers that run while holding the arena mutex; do not
recursively acquire it through public methods.

- [x] **Step 7: Write the moving-realloc test**

Block in-place growth with a live neighbor, require a move elsewhere in the
same guest arena, and verify `min(old_size, new_size)` bytes are preserved.

- [x] **Step 8: Implement moving realloc**

Allocate the replacement, `std::memcpy` the preserved prefix, then release the
old interval only after allocation succeeds.

- [x] **Step 9: Run all allocator tests**

Expected: all pass under the normal build and with
`--gtest_repeat=50 --gtest_break_on_failure`.

---

### Task 6: Add aligned allocation and usable-size lookup

**Files:**

- Modify: `tests/core/test_libc_mspace.cpp`
- Modify: `src/core/libraries/libc_internal/mspace.cpp`

**Interfaces:**

- Produces:
  `void* MspaceMemalign(void*, std::size_t, std::size_t)` and
  `std::size_t MspaceMallocUsableSize(void*)`.

- [x] **Step 1: Write memalign tests**

Use an intentionally unaligned subspan as the arena base. Test 16-, 64-, and
256-byte alignments and reject zero, non-power-of-two, and overflow-producing
alignments.

- [x] **Step 2: Verify the alignment tests fail**

Expected: null from the placeholder implementation.

- [x] **Step 3: Implement memalign**

Reject invalid alignments, normalize values smaller than 16 to 16, and delegate
to `Arena::Allocate`.

- [x] **Step 4: Write usable-size tests**

Verify exact requested sizes for live malloc/calloc/realloc/memalign blocks and
zero for null, foreign, freed, or destroyed-arena pointers.

- [x] **Step 5: Verify the usable-size tests fail**

Expected: live allocations currently report zero.

- [x] **Step 6: Implement registry-wide owner lookup**

Snapshot live `shared_ptr<Arena>` values under `registry_mutex`, release that
mutex, then query each arena. This avoids registry/arena lock inversion.

- [x] **Step 7: Run focused tests repeatedly**

```bash
cmake --build runtime/build/shadps4-tests \
  --target shadps4_libc_mspace_test -j2
runtime/build/shadps4-tests/tests/shadps4_libc_mspace_test \
  --gtest_repeat=50 --gtest_break_on_failure
```

Expected: 50 successful iterations.

---

### Task 7: Wire the allocator into libSceLibcInternal

**Files:**

- Modify: `src/core/libraries/libc_internal/libc_internal_memory.cpp:4-48`
- Modify: `CMakeLists.txt:568-583`

**Interfaces:**

- Consumes: all `Mspace*` allocator functions.
- Produces: PS4 SysV ABI wrappers and eight `LIB_FUNCTION` registrations for
  `libSceLibcInternal`.

- [x] **Step 1: Add ABI wrappers**

Add thin functions in `libc_internal_memory.cpp`:

```cpp
void* PS4_SYSV_ABI internal_sceLibcMspaceCreate(const char* name, void* base,
                                                size_t capacity, u32 flags);
s32 PS4_SYSV_ABI internal_sceLibcMspaceDestroy(void* handle);
void* PS4_SYSV_ABI internal_sceLibcMspaceMalloc(void* handle, size_t size);
s32 PS4_SYSV_ABI internal_sceLibcMspaceFree(void* handle, void* pointer);
void* PS4_SYSV_ABI internal_sceLibcMspaceCalloc(void* handle, size_t count,
                                                size_t size);
void* PS4_SYSV_ABI internal_sceLibcMspaceRealloc(void* handle, void* pointer,
                                                 size_t size);
void* PS4_SYSV_ABI internal_sceLibcMspaceMemalign(void* handle,
                                                  size_t alignment,
                                                  size_t size);
size_t PS4_SYSV_ABI internal_sceLibcMspaceMallocUsableSize(void* pointer);
```

Each wrapper delegates directly to the allocator component. Do not add
title-specific branches.

- [x] **Step 2: Register the exact NIDs**

Add to `RegisterlibSceLibcInternalMemory`:

```cpp
LIB_FUNCTION("-hn1tcVHq5Q", "libSceLibcInternal", 1,
             "libSceLibcInternal", internal_sceLibcMspaceCreate);
LIB_FUNCTION("W6SiVSiCDtI", "libSceLibcInternal", 1,
             "libSceLibcInternal", internal_sceLibcMspaceDestroy);
LIB_FUNCTION("OJjm-QOIHlI", "libSceLibcInternal", 1,
             "libSceLibcInternal", internal_sceLibcMspaceMalloc);
LIB_FUNCTION("Vla-Z+eXlxo", "libSceLibcInternal", 1,
             "libSceLibcInternal", internal_sceLibcMspaceFree);
LIB_FUNCTION("LYo3GhIlB38", "libSceLibcInternal", 1,
             "libSceLibcInternal", internal_sceLibcMspaceCalloc);
LIB_FUNCTION("gigoVHZvVPE", "libSceLibcInternal", 1,
             "libSceLibcInternal", internal_sceLibcMspaceRealloc);
LIB_FUNCTION("iF1iQHzxBJU", "libSceLibcInternal", 1,
             "libSceLibcInternal", internal_sceLibcMspaceMemalign);
LIB_FUNCTION("fEoW6BJsPt4", "libSceLibcInternal", 1,
             "libSceLibcInternal", internal_sceLibcMspaceMallocUsableSize);
```

- [x] **Step 3: Add allocator files to the main source list**

Insert `mspace.cpp` and `mspace.h` beside
`libc_internal_memory.cpp/.h` in `HLE_LIBC_INTERNAL_LIB`.

- [x] **Step 4: Rebuild focused tests**

Run:

```bash
cmake --build runtime/build/shadps4-tests \
  --target shadps4_libc_mspace_test -j2
ctest --test-dir runtime/build/shadps4-tests \
  --output-on-failure -R '^LibcMspace\.'
```

Expected: all tests pass.

- [x] **Step 5: Build the native runtime target**

Run:

```bash
cmake --build runtime/build/shadps4-x86_64 --target shadps4 -j2
```

If CMake reports the new source is absent from the generated build graph,
rerun `runtime/scripts/build-shadps4-x86_64.sh` instead of editing generated
Ninja files.

- [x] **Step 6: Check the implementation diff**

Run:

```bash
git diff --check -- \
  CMakeLists.txt \
  tests/CMakeLists.txt \
  tests/core/test_libc_mspace.cpp \
  src/core/libraries/libc_internal/mspace.h \
  src/core/libraries/libc_internal/mspace.cpp \
  src/core/libraries/libc_internal/libc_internal_memory.cpp
```

Expected: no errors.

---

### Task 8: Verify the native title path and review the change

**Files:**

- Inspect only: implementation diff
- Generated diagnostic:
  `runtime/build/diagnostics/crash-bandicoot-mspace-native.log`

**Interfaces:**

- Consumes: native x86_64 `shadps4` and legally dumped
  `/mnt/c/Users/Admin/Downloads/PS4 Games/crash-bandicoot/eboot.bin`.
- Produces: evidence that the title no longer reaches the unresolved mspace
  stub and does not crash during the 30-second native observation.

- [x] **Step 1: Run the title for 30 seconds**

Run with WSL display variables:

```bash
mkdir -p runtime/build/diagnostics
DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 \
  timeout 30s runtime/build/shadps4-x86_64/shadps4 \
  -g '/mnt/c/Users/Admin/Downloads/PS4 Games/crash-bandicoot/eboot.bin' \
  >runtime/build/diagnostics/crash-bandicoot-mspace-native.log 2>&1
```

Expected process result: timeout while still running, not SIGTRAP.

- [x] **Step 2: Analyze the native log with context-mode**

Count and extract:

- `sceLibcMspaceCreate`/`sceLibcMspaceMalloc` `CommonStub` messages;
- `Critical`, `Unreachable`, and unhandled-access lines;
- compiled graphics pipelines;
- the last non-trace lifecycle lines.

Expected: no unresolved mspace stub and no critical signal.

- [x] **Step 3: Run the full available native test suite**

Run:

```bash
ctest --test-dir runtime/build/shadps4-tests --output-on-failure
```

Expected: zero failed tests.

- [x] **Step 4: Apply the requesting-code-review workflow**

Review the complete diff against:

- guest-pointer bounds;
- arithmetic overflow;
- lock ordering;
- token lifetime and stale handles;
- realloc failure atomicity;
- ABI signatures and NIDs;
- test coverage from the approved design.

Address findings with new failing tests before changing implementation.

---

### Task 9: Rebuild and verify the managed runtime

**Files:**

- Generated: runtime build/stage artifacts
- Generated: `runtime/build/package/runtime.zip`

**Interfaces:**

- Consumes: verified native/FEX source changes and pinned submodules.
- Produces: a fresh verified runtime package containing the new ARM64 FEX HLE
  implementation.

- [x] **Step 1: Synchronize pinned submodules**

Run:

```bash
git submodule update --init --recursive --jobs 8
```

Do not reset or discard pre-existing submodule state.

- [x] **Step 2: Build every managed-runtime component**

Run:

```bash
runtime/scripts/build-runtime-debian.sh
```

Expected: x86_64, Box64 host, ARM64 FEX, staging, and packaging complete.

- [x] **Step 3: Verify the runtime lock manifest**

Run:

```bash
node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json
```

Expected: success and a runtime ZIP hash matching the packaging output.

- [x] **Step 4: Verify no Turnip driver is bundled**

Run:

```bash
node runtime/tests/verify-no-bundled-turnip.mjs runtime/build/rootfs
```

Expected: success.

---

### Task 10: Build and inspect the Android APK

**Files:**

- Generated:
  `android/BachataS4/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk`

**Interfaces:**

- Consumes: freshly verified managed runtime assets.
- Produces: an APK proven to contain both required runtime artifacts and no
  bundled Turnip driver.

- [x] **Step 1: Run Android verification and build tasks**

Run:

```bash
cd android/BachataS4
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export ANDROID_HOME=/mnt/c/Users/Admin/AppData/Local/Android/Sdk
./gradlew test lintDebug assembleDebug
```

Expected: all tasks succeed.

- [x] **Step 2: Verify both managed-runtime assets are present**

Run:

```bash
unzip -l app/build/outputs/apk/playstore/debug/app-playstore-debug.apk \
  | /usr/bin/grep -E 'assets/runtime/(manifest\.json|runtime\.zip)'
```

Expected: exactly both required entries. Stop if either is missing.

- [x] **Step 3: Run the nested runtime verifier**

From the repository root:

```bash
node runtime/tests/verify-apk-runtime.mjs \
  android/BachataS4/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk
```

Expected: runtime manifest/ZIP are valid and no Turnip/Freedreno driver binary is
bundled.

---

### Task 11: Deploy and verify Crash Bandicoot on Turnip

**Files:**

- Generated:
  `android/BachataS4/session-logs-crash-fixed/`
- Generated:
  `android/BachataS4/session-logs-crash-fixed/crash-start-screen.png`

**Interfaces:**

- Consumes: connected device `7d6afed8`, installed user Turnip driver, and
  `CUSA07399`.
- Produces: current application/shadPS4 logs and a screenshot proving the
  on-device result.

- [ ] **Step 1: Install the verified APK**

Run:

```bash
ADB=/mnt/c/Users/Admin/AppData/Local/Android/Sdk/platform-tools/adb.exe
"$ADB" -s 7d6afed8 install -r -d \
  android/BachataS4/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk
```

Expected: `Success`.

- [ ] **Step 2: Clear logcat and launch the title**

Run:

```bash
"$ADB" -s 7d6afed8 logcat -c
"$ADB" -s 7d6afed8 shell am start \
  -n com.bachatas4.android/.DirectLaunchActivity \
  --es game_id CUSA07399
```

Expected: activity starts.

- [ ] **Step 3: Observe the process for at least 120 seconds**

Poll in intervals no longer than 60 seconds. Confirm the app/backend remains
alive and frame telemetry continues beyond the old 13-second crash point.

- [ ] **Step 4: Capture the start-screen screenshot**

Run:

```bash
"$ADB" -s 7d6afed8 shell screencap -p \
  /sdcard/crash-start-screen.png
"$ADB" -s 7d6afed8 pull /sdcard/crash-start-screen.png \
  android/BachataS4/session-logs-crash-fixed/crash-start-screen.png
```

- [ ] **Step 5: Flush and pull the session logs**

Run:

```bash
"$ADB" -s 7d6afed8 shell am force-stop com.bachatas4.android
cd android/BachataS4
ADB_OVERRIDE=/mnt/c/Users/Admin/AppData/Local/Android/Sdk/platform-tools/adb.exe \
  ./pull-session-logs.sh --game CUSA07399 \
  --output session-logs-crash-fixed
```

- [ ] **Step 6: Analyze the application log**

Confirm:

- `guestBackend=fex`;
- `driver=turnip-...`;
- `Running` was reported;
- telemetry continued beyond 120 seconds;
- no spontaneous `exitCode=133` occurred before the deliberate force-stop.

- [ ] **Step 7: Analyze the shadPS4 log with context-mode**

Confirm:

- neither mspace NID used the temporary `ENOSYS` fallback;
- no write to `0xffffffffffffffe2`;
- no `SignalHandler: Unreachable code!`;
- no `WAIT_REG_MEM` stall or GPU coroutine spin;
- graphics pipelines compiled and frame telemetry advanced.

- [ ] **Step 8: Inspect the screenshot**

Use the local image viewer and check for:

- a non-black start screen;
- correct orientation/aspect ratio;
- intact UI/textures;
- missing textures, channel swaps, corruption, or large solid-color regions.

---

### Task 12: Apply the renderer evidence gate

**Files:**

- Inspect: new screenshot and session logs
- No renderer source file is pre-authorized by this plan

**Interfaces:**

- Consumes: actual post-fix device image and renderer diagnostics.
- Produces: either a clean graphical verification result or a concrete,
  separately designed renderer defect with captured resource metadata.

- [ ] **Step 1: Classify the result**

If the screenshot is visually correct, record the two 1D/2D messages as a
non-blocking cross-driver warning and make no renderer change.

- [ ] **Step 2: If visible corruption exists, gather exact resource evidence**

Capture guest address, image size, mip/layer ranges, tile mode, backing
`ImageType`, requested view type, shader resource dimensionality, and Vulkan
validation output for the offending view.

- [ ] **Step 3: Stop before speculative renderer edits**

Use the gathered values to create a focused renderer design and TDD plan. Do not
reuse the reverted global Nx1-to-1D workaround and do not special-case the game
ID.

- [ ] **Step 4: Run the final verification gate**

Before claiming completion, freshly verify:

- focused allocator tests;
- full available native tests;
- runtime lock/package checks;
- Gradle tests/lint/build;
- APK runtime contents;
- current on-device FEX/Turnip log;
- current screenshot.

Only then report whether the crash and visual acceptance criteria are met.
