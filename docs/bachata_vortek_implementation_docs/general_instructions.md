# Bachata S4 — Vortek System Driver Implementation Instructions

## Mission

Implement an experimental **Vortek System Driver** backend in Bachata S4 so that the x86-64/glibc shadPS4 runtime can use the Android device's Vulkan driver through a matched Vortek client/server stack.

The first game target is **Sonic Mania**. Optimize the implementation sequence for reaching a rendered, playable Sonic Mania session as quickly as possible without weakening the existing Turnip paths.

## Current repository context

Bachata S4 currently has two execution styles:

- `Box64Mode.HOST_GLIBC`
  - Used by glibc Vulkan ICDs such as imported/bundled Turnip.
  - Uses the host glibc loader and host Box64.
- `Box64Mode.APK_NATIVE`
  - Uses the Android/Bionic Box64 binary from the APK native library directory.
  - Used by the current `SYSTEM` configuration and Android/Bionic custom-driver path.

Relevant current files include:

- `android/BachataS4/core/runtime/src/main/kotlin/com/bachatas4/android/runtime/process/VulkanDriverConfiguration.kt`
- `android/BachataS4/core/runtime/src/main/kotlin/com/bachatas4/android/runtime/process/RuntimeProcessLauncher.kt`
- `android/BachataS4/core/runtime/src/main/cpp/custom_vulkan_bridge.cpp`
- `android/BachataS4/core/runtime/src/main/cpp/CMakeLists.txt`
- `runtime/scripts/vendor-winlator.sh`
- `runtime/scripts/package-runtime.mjs`
- `runtime/locks/`
- `NOTICE.android-runtime.md`

The current `SYSTEM` entry is not a complete system-driver bridge. It selects `APK_NATIVE` and exposes `SDL_VULKAN_LIBRARY=libvulkan.so.1`, but it does not provide a Vortek server, Vortek ICD, protocol lifecycle, capability check, or verified WSI path.

The current custom Android Vulkan bridge is specific to AdrenoTools and custom Turnip. Preserve it unless a refactor is necessary to share small, clearly isolated utilities.

## Source-of-truth repositories

Use only matched, pinned source revisions:

- Bachata S4: `https://github.com/JICA98/Bachata-S4`
- Vortek client: `https://github.com/brunodev85/vortek`
- Winlator source containing the matching Vortek server/integration: `https://github.com/brunodev85/winlator`
- shadPS4 upstream: `https://github.com/shadps4-emu/shadPS4`

The standalone public Vortek repository is the client. Its README explicitly states that a separate server performs host-driver work. Do not implement against the client alone.

## Non-negotiable rules

### Preserve working backends

Do not break or silently alter:

- glibc Turnip ICD mode
- Android/Bionic custom Turnip mode
- imported driver mode
- existing runtime packaging for Play Store and F-Droid
- existing Box64 execution modes

Add Vortek as a distinct backend. Do not initially replace `SYSTEM` globally unless the UI and migration behavior are explicit and tested.

Recommended enum shape:

```kotlin
enum class RuntimeVulkanDriver {
    SYSTEM_VORTEK,
    CUSTOM,
    TURNIP_25_0_0,
    TURNIP_25_3_0_R11,
    TURNIP_26_1_0,
}
```

A temporary migration alias from the old `SYSTEM` value is acceptable, but existing preferences must decode predictably.

### Do not fake Vulkan support

The public Vortek ICD manifest advertises Vulkan `1.1.128`. shadPS4 currently requires Vulkan 1.3 plus `VK_KHR_swapchain` and `VK_KHR_push_descriptor`.

Never change the ICD manifest to `1.3.x` merely to pass startup checks.

Raise the advertised API version only after:

1. The client exports or forwards the required core 1.3 entry points.
2. The server implements or safely maps the required requests.
3. Feature/property chains are serialized correctly.
4. A probe confirms the required capabilities through the Vortek path.
5. shadPS4 can create its instance and device without unsupported-call failures.

### Use a matched client/server protocol

Pin the Vortek client and Winlator server from compatible revisions. Record:

- upstream repository
- commit hash
- retrieval method
- license
- local vendored path
- protocol version or Bachata-added handshake version

Do not mix an arbitrary Vortek client commit with a different server implementation.

### Reuse before rewriting

The efficient route is:

1. Vendor the matching client.
2. Vendor or adapt the matching server and WSI integration.
3. Make the socket path and app package assumptions configurable.
4. Prove the unchanged protocol with a tiny Vulkan probe.
5. Extend only the calls actually reached by shadPS4/Sonic Mania.

Do not create a new Vulkan RPC protocol, a new serializer, or a new X11-to-Android presentation system until the matching Winlator code has been audited and shown unusable.

### Keep the first target narrow

The initial definition of success is not “all PS4 games work with every system driver.”

The first target is:

- Sonic Mania reaches in-game rendering.
- Frames are presented through the device system Vulkan driver.
- Input works.
- The app survives pause/resume or fails with a clear recoverable message.
- Existing Turnip modes remain functional.

Implement only the Vulkan calls, structures, and extensions required to reach this target, while keeping the design extensible.

## Required working method

### Before modifying code

Create a baseline report containing:

- current Git commit
- Android build variant
- runtime lock versions
- Box64 version
- shadPS4 revision
- selected driver mode
- device model, SoC, GPU, Android version
- known working Sonic Mania result with glibc Turnip
- current failure result with the old system option
- complete relevant logs

Inspect the repository before assuming paths. Search for:

```text
RuntimeVulkanDriver
VulkanDriverConfiguration
RuntimeProcessRequest
ALLOWED_ENVIRONMENT
custom_vulkan_bridge
bachata_open_custom_vulkan
SDL_VULKAN_LIBRARY
VK_ICD_FILENAMES
SurfaceView
TextureView
ANativeWindow
XServer
Vulkan
vortek
```

### Work in vertical slices

Each slice must produce testable evidence:

1. Source pin and reproducible build.
2. Client can connect to server.
3. Server opens Android `libvulkan.so`.
4. Guest probe enumerates the real device GPU.
5. Guest creates a Vulkan instance.
6. Guest creates a device.
7. Guest creates a surface and swapchain.
8. Guest presents repeated frames.
9. shadPS4 initializes Vulkan.
10. Sonic Mania renders.
11. Sonic Mania is playable.

Do not claim a milestone is complete because code compiles.

### Make failures explicit

Every boundary must have structured logs.

Use tags similar to:

```text
[Bachata.Vortek] <Info> state=starting socket=...
[Bachata.Vortek] <Info> protocol client=... server=...
[Bachata.Vortek] <Info> host_loader=libvulkan.so
[Bachata.Vortek] <Info> gpu="Adreno ..."
[Bachata.Vortek] <Info> api=1.3.x
[Bachata.Vortek] <Info> surface=created
[Bachata.Vortek] <Info> swapchain=created images=...
[Bachata.Vortek] <Error> unsupported_request=vk...
[Bachata.Vortek] <Error> unsupported_structure=VK_STRUCTURE_TYPE_...
[Bachata.Vortek] <Error> missing_extension=VK_KHR_push_descriptor
```

Never silently fall back from Vortek to Turnip. A hidden fallback makes test results meaningless.

### Keep security boundaries

All runtime-created paths must stay inside app-owned storage.

Validate:

- socket path
- shared-memory descriptors
- runtime library path
- ICD manifest path
- server binary/library path
- temporary directory
- log path

Do not accept arbitrary external socket paths or untrusted ICD paths through an unvalidated intent/environment value.

### Keep lifecycle ownership clear

Exactly one component owns each of the following:

- Vortek server process/thread
- Unix socket file
- shared-memory file descriptors
- Android `Surface`/`ANativeWindow`
- host Vulkan instance/device
- guest connection
- shutdown sequence

The session controller should start the server before Box64 and stop it after the emulator process has exited.

### Maintain licenses and notices

Vortek is LGPL-2.1. Bachata S4 is GPL-2.0-or-later. Preserve upstream copyright headers and comply with all applicable source and notice requirements.

Update:

- `NOTICE.android-runtime.md`
- runtime component locks
- source archive/commit metadata
- any F-Droid source-build documentation
- reproducibility checks

Do not bundle a prebuilt binary without a reproducible source path.

## Build and verification expectations

Use the repository's current build pipeline rather than adding an unrelated parallel build system.

Typical repository-level checks:

```bash
git submodule update --init --recursive --jobs 8

runtime/scripts/build-shadps4-x86_64.sh
runtime/scripts/build-box64-host.sh
node runtime/scripts/package-runtime.mjs
node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json

cd android/BachataS4
./gradlew test lintDebug assemblePlaystoreDebug
```

Also run the relevant F-Droid/source-build verification if the modified component is included in that flavor.

Add focused tests for:

- preference decoding and migration
- Vortek driver configuration
- environment allow-listing
- socket path validation
- server start/stop state transitions
- protocol-version rejection
- runtime packaging contents
- manifest API-version gating
- no regression in existing Turnip configurations

## Code quality constraints

- Prefer small classes with explicit ownership.
- Do not place the entire server lifecycle in an Activity.
- Do not use global mutable JNI pointers without synchronization and defined teardown.
- Do not pass `ANativeWindow*` as a string/environment variable.
- Do not hard-code `com.winlator` paths.
- Do not hard-code `/data/data/...` paths.
- Do not load `/vendor/lib64/hw/vulkan.*.so` directly.
- The Android server should load the platform Vulkan loader through `dlopen("libvulkan.so", ...)`.
- Avoid broad shadPS4 renderer changes until the Vortek probe proves the bridge itself.
- Do not remove validation to hide a crash.
- Do not advertise extensions that the client/server cannot honor.
- Keep debug tracing runtime-configurable to avoid production overhead.

## Evidence required with every completed task

For each completed task, provide:

- files changed
- reason for each change
- build command and result
- on-device command or action
- relevant logs
- expected result
- actual result
- remaining blocker
- rollback impact

Screenshots alone are insufficient for Vulkan milestones. Include logs showing the actual selected backend, GPU, API version, surface, and swapchain state.

## Completion criteria

The implementation is complete only when all of the following are true:

- Vortek is a separate selectable driver backend.
- The Vortek client and matching server are pinned and reproducibly built.
- Client/server protocol compatibility is checked.
- The server opens Android's platform Vulkan loader.
- An x86-64/glibc Vulkan probe sees the real device GPU.
- The probe creates a surface and presents frames.
- shadPS4 creates its required Vulkan instance/device through Vortek.
- Sonic Mania reaches in-game and is playable.
- Existing Turnip modes still pass their prior smoke tests.
- Pause/resume and shutdown do not leak a server, socket, or native window.
- Unsupported devices receive a clear capability error.
- Runtime notices, locks, and packaging tests are updated.
