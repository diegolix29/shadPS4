# AvPlayer FEX Guest-Callback Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Bloodborne advance from its Controls screen into visible gameplay by delivering AvPlayer callbacks correctly through FEX.

**Architecture:** AvPlayer keeps its host wrappers, but each wrapper classifies the saved callback address. Guest event and memory callbacks enter through `Core::GuestCpu::RunGuestFunctionOrAbort`; host callbacks keep direct invocation. Guest file callbacks are disabled under FEX so the existing mounted native-file path keeps FFmpeg buffers entirely in host memory.

**Tech Stack:** C++23, shadPS4 AvPlayer, FEX guest CPU bridge, Node.js `node:test` source-contract tests, managed Android runtime, Gradle.

## Global Constraints

- Preserve direct callback behavior in non-FEX builds and for host callback addresses.
- Do not pass host-owned FFmpeg buffers to guest file callbacks.
- `StateReady` and `StatePlay` use null `event_data`; guest-safe transport for the
  existing non-null `WarningId` host payload is outside this black-screen fix and
  must not be claimed as solved.
- Do not skip or fake game movies.
- Do not change VideoOut, GPU synchronization, HTTP, or texture-cache code.
- Preserve every unrelated dirty-worktree change; stage only the exact files named by each task.
- Any subagent-produced diff must be reviewed by a fresh reviewer and the main agent before acceptance.
- Before `assembleDebug`, run `git submodule update --init --recursive --jobs 8`, build the managed runtime, and verify it against `runtime/locks/components.lock.json`.
- Do not install an APK until both `assets/runtime/manifest.json` and `assets/runtime/runtime.zip` are verified inside it.

## File Map

- Create `runtime/tests/fex-avplayer-callback-source.test.mjs`: lock the FEX event, memory, and native-file callback policies to source.
- Modify `src/core/libraries/avplayer/avplayer_state.cpp`: dispatch the saved game event callback through FEX when its address belongs to guest code.
- Modify `src/core/libraries/avplayer/avplayer_impl.cpp`: dispatch saved game allocators through FEX and select native mounted-file I/O for guest file callbacks.

---

### Task 1: Bridge AvPlayer Event Callbacks

**Files:**

- Create: `runtime/tests/fex-avplayer-callback-source.test.mjs`
- Modify: `src/core/libraries/avplayer/avplayer_state.cpp:7-9,87-96`

**Interfaces:**

- Consumes: `Core::GuestCpu::IsGuestFunctionAddress(const void*) -> bool`
- Consumes: `Core::GuestCpu::RunGuestFunctionOrAbort(const void*, std::string_view, Args...) -> u64`
- Produces: `AvPlayerState::DefaultEventCallback` dispatches guest callbacks through FEX and host callbacks directly.

- [ ] **Step 1: Write the failing event source-contract test**

Create `runtime/tests/fex-avplayer-callback-source.test.mjs`:

```javascript
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");
const section = (source, startMarker, endMarker) => {
  const start = source.indexOf(startMarker);
  assert.notEqual(start, -1, `missing start marker: ${startMarker}`);
  const end = source.indexOf(endMarker, start);
  assert.notEqual(end, -1, `missing end marker: ${endMarker}`);
  return source.slice(start, end);
};

test("AvPlayer dispatches game events through the FEX guest bridge", () => {
  const state = read("src/core/libraries/avplayer/avplayer_state.cpp");
  const callback = section(
    state,
    "void AvPlayerState::DefaultEventCallback",
    "// Called inside GAME thread",
  );

  assert.match(state, /#include "core\/guest_cpu\/guest_callback\.h"/);
  assert.match(callback, /#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU/);
  assert.match(
    callback,
    /const auto callback_address = reinterpret_cast<const void\*>\(callback\)/,
  );
  assert.match(callback, /IsGuestFunctionAddress\(callback_address\)/);
  assert.match(
    callback,
    /RunGuestFunctionOrAbort\([\s\S]*"AvPlayer event"[\s\S]*ptr,[\s\S]*event_id,[\s\S]*source_id,[\s\S]*event_data\)/,
  );
  assert.match(callback, /callback\(ptr, event_id, source_id, event_data\)/);
});
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
node --test runtime/tests/fex-avplayer-callback-source.test.mjs
```

Expected: FAIL at the missing `core/guest_cpu/guest_callback.h` assertion or missing `IsGuestFunctionAddress` assertion.

- [ ] **Step 3: Add the FEX include and replace the event callback body**

Add after the existing common/core includes in `avplayer_state.cpp`:

```cpp
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#endif
```

Replace `DefaultEventCallback` with:

```cpp
void AvPlayerState::DefaultEventCallback(void* opaque, AvPlayerEvents event_id, s32 source_id,
                                         void* event_data) {
    auto const self = reinterpret_cast<AvPlayerState*>(opaque);
    const auto callback = self->m_event_replacement.event_callback;
    const auto ptr = self->m_event_replacement.object_ptr;
    if (callback == nullptr) {
        return;
    }
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    const auto callback_address = reinterpret_cast<const void*>(callback);
    if (Core::GuestCpu::IsGuestFunctionAddress(callback_address)) {
        Core::GuestCpu::RunGuestFunctionOrAbort(callback_address, "AvPlayer event", ptr, event_id,
                                                source_id, event_data);
        return;
    }
#endif
    callback(ptr, event_id, source_id, event_data);
}
```

- [ ] **Step 4: Run the test and verify GREEN**

Run:

```bash
node --test runtime/tests/fex-avplayer-callback-source.test.mjs
```

Expected: `1` test passes, `0` fail.

- [ ] **Step 5: Review and commit only the event slice**

Run:

```bash
git diff --check -- src/core/libraries/avplayer/avplayer_state.cpp runtime/tests/fex-avplayer-callback-source.test.mjs
git diff -- src/core/libraries/avplayer/avplayer_state.cpp runtime/tests/fex-avplayer-callback-source.test.mjs
git add -- src/core/libraries/avplayer/avplayer_state.cpp runtime/tests/fex-avplayer-callback-source.test.mjs
git commit -m "fix(avplayer): bridge FEX event callbacks"
```

Expected: reviewer confirms null behavior is preserved, the guest path returns after bridge dispatch, and the direct path remains.

---

### Task 2: Bridge AvPlayer Memory Callbacks

**Files:**

- Modify: `runtime/tests/fex-avplayer-callback-source.test.mjs`
- Modify: `src/core/libraries/avplayer/avplayer_impl.cpp:4-40`

**Interfaces:**

- Consumes: the same guest-address classifier and bridge as Task 1.
- Produces: `CallbackAddress(Callback) -> const void*` and `IsGuestCallback(Callback) -> bool`, local to `avplayer_impl.cpp` in FEX builds.
- Produces: bridged `Allocate`, `Deallocate`, `AllocateTexture`, and `DeallocateTexture` wrappers.

- [ ] **Step 1: Append the failing memory callback test**

Append to `runtime/tests/fex-avplayer-callback-source.test.mjs`:

```javascript
test("AvPlayer dispatches game allocators through the FEX guest bridge", () => {
  const implementation = read("src/core/libraries/avplayer/avplayer_impl.cpp");
  const wrappers = [
    ["void* PS4_SYSV_ABI AvPlayer::Allocate(", "void PS4_SYSV_ABI AvPlayer::Deallocate(", "allocate", "AvPlayer allocate"],
    ["void PS4_SYSV_ABI AvPlayer::Deallocate(", "void* PS4_SYSV_ABI AvPlayer::AllocateTexture(", "deallocate", "AvPlayer deallocate"],
    ["void* PS4_SYSV_ABI AvPlayer::AllocateTexture(", "void PS4_SYSV_ABI AvPlayer::DeallocateTexture(", "allocate", "AvPlayer allocate texture"],
    ["void PS4_SYSV_ABI AvPlayer::DeallocateTexture(", "int PS4_SYSV_ABI AvPlayer::OpenFile(", "deallocate", "AvPlayer deallocate texture"],
  ];

  assert.match(implementation, /#include "core\/guest_cpu\/guest_callback\.h"/);
  assert.match(
    implementation,
    /const void\* CallbackAddress\(Callback callback\)/,
  );
  assert.match(
    implementation,
    /bool IsGuestCallback\(Callback callback\)/,
  );

  for (const [start, end, variable, label] of wrappers) {
    const wrapper = section(implementation, start, end);
    assert.match(wrapper, new RegExp(`IsGuestCallback\\(${variable}\\)`));
    assert.match(
      wrapper,
      new RegExp(`RunGuestFunctionOrAbort\\([\\s\\S]*"${label}"`),
    );
  }
});
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
node --test runtime/tests/fex-avplayer-callback-source.test.mjs
```

Expected: event test PASS; allocator test FAIL because `avplayer_impl.cpp` has no FEX guest-callback include/helper.

- [ ] **Step 3: Add local address helpers and bridge all four memory wrappers**

Add the guarded include near the top of `avplayer_impl.cpp`:

```cpp
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#endif
```

Add inside `namespace Libraries::AvPlayer`, before `AvPlayer::Allocate`:

```cpp
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
template <typename Callback>
const void* CallbackAddress(Callback callback) {
    return reinterpret_cast<const void*>(callback);
}

template <typename Callback>
bool IsGuestCallback(Callback callback) {
    return Core::GuestCpu::IsGuestFunctionAddress(CallbackAddress(callback));
}
#endif
```

Replace the four memory wrappers with:

```cpp
void* PS4_SYSV_ABI AvPlayer::Allocate(void* handle, u32 alignment, u32 size) {
    const auto* const self = reinterpret_cast<AvPlayer*>(handle);
    const auto allocate = self->m_init_data_original.memory_replacement.allocate;
    const auto ptr = self->m_init_data_original.memory_replacement.object_ptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCallback(allocate)) {
        return reinterpret_cast<void*>(Core::GuestCpu::RunGuestFunctionOrAbort(
            CallbackAddress(allocate), "AvPlayer allocate", ptr, alignment, size));
    }
#endif
    return allocate(ptr, alignment, size);
}

void PS4_SYSV_ABI AvPlayer::Deallocate(void* handle, void* memory) {
    const auto* const self = reinterpret_cast<AvPlayer*>(handle);
    const auto deallocate = self->m_init_data_original.memory_replacement.deallocate;
    const auto ptr = self->m_init_data_original.memory_replacement.object_ptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCallback(deallocate)) {
        Core::GuestCpu::RunGuestFunctionOrAbort(CallbackAddress(deallocate),
                                                "AvPlayer deallocate", ptr, memory);
        return;
    }
#endif
    deallocate(ptr, memory);
}

void* PS4_SYSV_ABI AvPlayer::AllocateTexture(void* handle, u32 alignment, u32 size) {
    const auto* const self = reinterpret_cast<AvPlayer*>(handle);
    const auto allocate = self->m_init_data_original.memory_replacement.allocate_texture;
    const auto ptr = self->m_init_data_original.memory_replacement.object_ptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCallback(allocate)) {
        return reinterpret_cast<void*>(Core::GuestCpu::RunGuestFunctionOrAbort(
            CallbackAddress(allocate), "AvPlayer allocate texture", ptr, alignment, size));
    }
#endif
    return allocate(ptr, alignment, size);
}

void PS4_SYSV_ABI AvPlayer::DeallocateTexture(void* handle, void* memory) {
    const auto* const self = reinterpret_cast<AvPlayer*>(handle);
    const auto deallocate = self->m_init_data_original.memory_replacement.deallocate_texture;
    const auto ptr = self->m_init_data_original.memory_replacement.object_ptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCallback(deallocate)) {
        Core::GuestCpu::RunGuestFunctionOrAbort(
            CallbackAddress(deallocate), "AvPlayer deallocate texture", ptr, memory);
        return;
    }
#endif
    deallocate(ptr, memory);
}
```

- [ ] **Step 4: Run the source-contract tests and verify GREEN**

Run:

```bash
node --test runtime/tests/fex-avplayer-callback-source.test.mjs
```

Expected: `2` tests pass, `0` fail.

- [ ] **Step 5: Review and commit only the memory slice**

Run:

```bash
git diff --check -- src/core/libraries/avplayer/avplayer_impl.cpp runtime/tests/fex-avplayer-callback-source.test.mjs
git diff -- src/core/libraries/avplayer/avplayer_impl.cpp runtime/tests/fex-avplayer-callback-source.test.mjs
git add -- src/core/libraries/avplayer/avplayer_impl.cpp runtime/tests/fex-avplayer-callback-source.test.mjs
git commit -m "fix(avplayer): bridge FEX memory callbacks"
```

Expected: reviewer confirms all four labels are distinct, pointer returns are preserved, and every bridged void callback returns before direct invocation.

---

### Task 3: Select Native File I/O for Guest File Callbacks

**Files:**

- Modify: `runtime/tests/fex-avplayer-callback-source.test.mjs`
- Modify: `src/core/libraries/avplayer/avplayer_impl.cpp:74-92`

**Interfaces:**

- Consumes: `IsGuestCallback(Callback) -> bool` from Task 2.
- Produces: `AvPlayer::StubInitData` clears `file_replacement` when any complete-table function belongs to guest code.
- Preserves: complete host callback tables still use `OpenFile`, `CloseFile`, `ReadOffsetFile`, and `SizeFile`.

- [ ] **Step 1: Append the failing native-file policy test**

Append to `runtime/tests/fex-avplayer-callback-source.test.mjs`:

```javascript
test("AvPlayer keeps host FFmpeg buffers out of guest file callbacks", () => {
  const implementation = read("src/core/libraries/avplayer/avplayer_impl.cpp");
  const stub = section(
    implementation,
    "AvPlayerInitData AvPlayer::StubInitData",
    "AvPlayer::AvPlayer(",
  );

  assert.match(stub, /const bool missing_file_callback =/);
  assert.match(stub, /const bool has_guest_file_callback =/);
  for (const callback of ["open", "close", "read_offset", "size"]) {
    assert.match(
      stub,
      new RegExp(`IsGuestCallback\\(data\\.file_replacement\\.${callback}\\)`),
    );
  }
  assert.match(
    stub,
    /if \(missing_file_callback \|\| has_guest_file_callback\)[\s\S]*result\.file_replacement = \{\};/,
  );
  assert.match(stub, /result\.file_replacement\.open = &AvPlayer::OpenFile/);
  assert.match(stub, /result\.file_replacement\.read_offset = &AvPlayer::ReadOffsetFile/);
});
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
node --test runtime/tests/fex-avplayer-callback-source.test.mjs
```

Expected: event and allocator tests PASS; file-policy test FAIL because `missing_file_callback` and `has_guest_file_callback` do not exist.

- [ ] **Step 3: Replace the file-selection portion of `StubInitData`**

Keep the memory-wrapper assignments unchanged, then use:

```cpp
    const bool missing_file_callback =
        data.file_replacement.open == nullptr || data.file_replacement.close == nullptr ||
        data.file_replacement.read_offset == nullptr || data.file_replacement.size == nullptr;
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
    const bool has_guest_file_callback =
        IsGuestCallback(data.file_replacement.open) ||
        IsGuestCallback(data.file_replacement.close) ||
        IsGuestCallback(data.file_replacement.read_offset) ||
        IsGuestCallback(data.file_replacement.size);
#else
    constexpr bool has_guest_file_callback = false;
#endif
    if (missing_file_callback || has_guest_file_callback) {
        result.file_replacement = {};
    } else {
        result.file_replacement.object_ptr = this;
        result.file_replacement.open = &AvPlayer::OpenFile;
        result.file_replacement.close = &AvPlayer::CloseFile;
        result.file_replacement.read_offset = &AvPlayer::ReadOffsetFile;
        result.file_replacement.size = &AvPlayer::SizeFile;
    }
```

The empty table activates `AvPlayerSource::Init`'s existing
`Core::FileSys::MntPoints::GetHostPath` branch. Do not change
`avplayer_source.cpp` or publish FFmpeg buffers to guest memory.

- [ ] **Step 4: Run focused and adjacent source-contract tests**

Run:

```bash
node --test runtime/tests/fex-avplayer-callback-source.test.mjs
node --test runtime/tests/fex-guest-entry-source.test.mjs
node --test runtime/tests/fex-sonic-runtime-source.test.mjs
```

Expected: new file reports `3` pass, `0` fail; each adjacent FEX source contract
reports `1` pass, `0` fail.

- [ ] **Step 5: Review and commit only the file-policy slice**

Run:

```bash
git diff --check -- src/core/libraries/avplayer/avplayer_impl.cpp runtime/tests/fex-avplayer-callback-source.test.mjs
git diff -- src/core/libraries/avplayer/avplayer_impl.cpp runtime/tests/fex-avplayer-callback-source.test.mjs
git add -- src/core/libraries/avplayer/avplayer_impl.cpp runtime/tests/fex-avplayer-callback-source.test.mjs
git commit -m "fix(avplayer): use native FEX file I/O"
```

Expected: reviewer confirms any guest function selects one coherent native-file path, incomplete tables retain existing fallback behavior, and complete host tables remain wrapped.

---

### Task 4: Review, Build, Package, and Prove on Device

**Files:**

- Review: `src/core/libraries/avplayer/avplayer_state.cpp`
- Review: `src/core/libraries/avplayer/avplayer_impl.cpp`
- Review: `runtime/tests/fex-avplayer-callback-source.test.mjs`
- Verify: `runtime/locks/components.lock.json`
- Verify APK: `android/BachataS4/app/build/outputs/apk/fdroid/debug/app-fdroid-debug.apk`

**Interfaces:**

- Consumes: all deliverables from Tasks 1-3.
- Produces: verified runtime/APK and Bloodborne device evidence beyond the former 32-present boundary.

- [ ] **Step 1: Perform independent diff review**

Have a fresh reviewer inspect only:

```bash
git diff 8b029603 -- src/core/libraries/avplayer/avplayer_state.cpp src/core/libraries/avplayer/avplayer_impl.cpp runtime/tests/fex-avplayer-callback-source.test.mjs
```

Reviewer must check:

- guest callbacks never receive both bridged and direct invocation;
- event arguments remain `ptr`, `event_id`, `source_id`, `event_data`;
- allocation `u64` results are converted back to pointers;
- all four memory callbacks are covered;
- any guest file callback clears the whole replacement table;
- native/non-FEX paths remain direct;
- no unrelated source was edited.

Main agent re-reads every changed hunk and either fixes each valid finding with a new RED/GREEN cycle or records why it is not applicable.

- [ ] **Step 2: Run source tests and whitespace validation**

Run:

```bash
node --test runtime/tests/fex-avplayer-callback-source.test.mjs \
  runtime/tests/fex-guest-entry-source.test.mjs \
  runtime/tests/fex-sonic-runtime-source.test.mjs
git diff --check 8b029603 -- src/core/libraries/avplayer/avplayer_state.cpp src/core/libraries/avplayer/avplayer_impl.cpp runtime/tests/fex-avplayer-callback-source.test.mjs
```

Expected: `5` tests pass, `0` fail; `git diff --check` prints nothing.

- [ ] **Step 3: Build and verify the managed runtime from repository root**

Run:

```bash
git submodule update --init --recursive --jobs 8
runtime/scripts/build-runtime-debian.sh
node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json
```

Expected: runtime build succeeds; verifier reports all managed files valid and prints the packaged `runtime.zip` SHA-256.

- [ ] **Step 4: Build all debug APK variants**

Run:

```bash
cd android/BachataS4
./gradlew test lintDebug assembleDebug
```

Expected: `BUILD SUCCESSFUL`; unit tests and `lintDebug` succeed; fdroid debug APK exists.

- [ ] **Step 5: Verify both managed-runtime assets before installation**

Run from `android/BachataS4`:

```bash
unzip -l app/build/outputs/apk/fdroid/debug/app-fdroid-debug.apk \
  | grep -E 'assets/runtime/(manifest\.json|runtime\.zip)'
node ../../runtime/tests/verify-apk-runtime.mjs \
  app/build/outputs/apk/fdroid/debug/app-fdroid-debug.apk
```

Expected: unzip lists both `assets/runtime/manifest.json` and `assets/runtime/runtime.zip`; APK runtime verifier passes.

- [ ] **Step 6: Install and launch Bloodborne on the known device**

Run from `android/BachataS4`:

```bash
/home/jica/Android/Sdk/platform-tools/adb -s 7d6afed8 install -r -d \
  app/build/outputs/apk/fdroid/debug/app-fdroid-debug.apk
/home/jica/Android/Sdk/platform-tools/adb -s 7d6afed8 logcat -c
/home/jica/Android/Sdk/platform-tools/adb -s 7d6afed8 shell am start \
  -n com.bachatas4.android/.DirectLaunchActivity --es game_id CUSA00900
```

Expected: install succeeds and Bloodborne reaches the Controls screen. Select Next with the configured game controls and wait until the transition completes.

- [ ] **Step 7: Capture screenshot and flushed session logs**

Run:

```bash
/home/jica/Android/Sdk/platform-tools/adb -s 7d6afed8 shell screencap -p \
  /sdcard/bachata-avplayer-fex.png
/home/jica/Android/Sdk/platform-tools/adb -s 7d6afed8 pull \
  /sdcard/bachata-avplayer-fex.png android/BachataS4/device-diagnostics/
/home/jica/Android/Sdk/platform-tools/adb -s 7d6afed8 shell am force-stop \
  com.bachatas4.android
cd android/BachataS4
ADB_OVERRIDE=/home/jica/Android/Sdk/platform-tools/adb \
  ./pull-session-logs.sh --game CUSA00900 \
  --output device-diagnostics/avplayer-fex-callback-fix
```

Expected: screenshot is pulled and the newest CUSA00900 session contains `shadps4.log`.

- [ ] **Step 8: Verify device acceptance evidence**

Set `LOG` to the exact newest pulled `shadps4.log`, then run:

```bash
LOG="$(find android/BachataS4/device-diagnostics/avplayer-fex-callback-fix \
  -name shadps4.log -printf '%T@ %p\n' | sort -nr | head -n1 | cut -d' ' -f2-)"
rg -n 'Sending event to the game: id = State(Ready|Play)' "$LOG"
rg -c 'present_returned' "$LOG"
! rg -n 'FEX guest callback AvPlayer .* failed' "$LOG"
```

Expected:

- `StateReady` occurs before `StatePlay`;
- `present_returned` count is greater than `32`;
- no AvPlayer guest-callback failure appears;
- visual inspection of `android/BachataS4/device-diagnostics/bachata-avplayer-fex.png` shows non-black gameplay after the transition.
