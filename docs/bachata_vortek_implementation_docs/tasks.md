# Bachata S4 — Vortek System Driver Tasks

## Execution policy

Complete tasks in order. Do not skip a gate because later code compiles.

The shortest efficient path is to reuse a matched Winlator Vortek client/server revision, prove it with a small x86-64 Vulkan program, and only then extend the Vulkan request set needed by shadPS4 and Sonic Mania.

---

## Task 0 — Capture a reproducible baseline

### Goal

Establish the exact starting state and prove that Sonic Mania still works with the currently selected glibc Turnip driver.

### Steps

1. Record:
   - Bachata S4 Git commit.
   - shadPS4 revision.
   - Box64 revisions for `APK_NATIVE` and `HOST_GLIBC`.
   - runtime component lock file contents.
   - Android build flavor and version.
   - test-device model, SoC, GPU, Android version, page size.
2. Launch Sonic Mania with the known-working glibc Turnip driver.
3. Save:
   - full runtime log
   - selected Vulkan backend
   - GPU name
   - Vulkan API version
   - first in-game frame evidence
   - approximate FPS in the starting room
4. Launch with the current system option and capture the failure.
5. Note the first failing boundary:
   - loader
   - instance
   - physical device
   - logical device
   - surface
   - swapchain
   - shader/pipeline
   - game-specific rendering

### Acceptance criteria

- Baseline Turnip result is reproducible.
- Current system result is reproducible.
- Logs are saved under an app-owned diagnostic directory.
- No source changes have been made yet.

---

## Task 1 — Audit and pin a matched Vortek stack

### Goal

Identify one Vortek client revision and the exact matching Winlator server/WSI revision.

### Steps

1. Inspect:
   - `https://github.com/brunodev85/vortek`
   - `https://github.com/brunodev85/winlator`
2. Locate the server implementation that handles:
   - `REQUEST_CODE_CREATE_CONTEXT`
   - Unix-domain socket accept/connect
   - shared-memory FD creation and transfer
   - server/client ring buffers
   - Vulkan request dispatch
   - Android host Vulkan loading
   - WSI or surface integration
3. Locate any Vortek-specific Java/Kotlin/JNI lifecycle code in Winlator.
4. Select a matched revision pair.
5. Document:
   - client commit
   - server commit
   - protocol assumptions
   - socket-path assumptions
   - app-package assumptions
   - build commands
   - license
6. Add a lock entry, for example:
   - component name: `vortek-client`
   - component name: `vortek-server`
   - source URL
   - pinned commit
   - checksum where applicable
7. Update the runtime notice plan.

### Important constraint

Do not proceed with only the public client repository. The client connects to a separate server and cannot use the system driver by itself.

### Acceptance criteria

- The exact client and server source locations are known.
- Both revisions are pinned.
- The server build dependencies are known.
- No hard-coded Winlator package path remains unidentified.

---

## Task 2 — Vendor the sources reproducibly

### Goal

Integrate Vortek source retrieval into Bachata's existing vendoring and runtime-lock system.

### Suggested locations

Use repository conventions. A reasonable layout is:

```text
externals/vortek-client/
externals/winlator-app/...matching server sources...
android/BachataS4/core/runtime/src/main/cpp/vortek/
runtime/locks/components.lock.json
runtime/scripts/vendor-vortek.sh
```

Do not duplicate source unnecessarily if the current Winlator vendor tree already contains the matched server.

### Steps

1. Extend `runtime/scripts/vendor-winlator.sh` or add a focused `vendor-vortek.sh`.
2. Pin source revisions.
3. Apply Bachata patches as separate patch files where practical.
4. Make the vendoring command idempotent.
5. Update:
   - `NOTICE.android-runtime.md`
   - source-lock metadata
   - verification script
6. Ensure Play Store and F-Droid builds can reproduce the same sources.

### Acceptance criteria

- A clean checkout can fetch/vendor the same source revisions.
- Runtime verification detects a missing or altered Vortek component.
- License files and notices are preserved.
- No prebuilt-only dependency is introduced.

---

## Task 3 — Build the Vortek guest/client ICD

### Goal

Build `libvulkan_vortek.so` for the architecture and ABI used by the guest-side Vulkan loader.

### Architecture decision gate

First determine which execution path the Vortek client expects:

- AArch64 glibc client loaded by the host glibc Vulkan loader, or
- library used through the APK-native Box64 wrapper path.

Prefer the architecture used by the matching Winlator Vortek integration. Do not guess.

For Bachata's current glibc runtime, the likely efficient shape is:

```text
x86-64 shadPS4
  -> Box64
  -> AArch64 glibc Vulkan loader/ICD
  -> libvulkan_vortek.so
  -> Vortek socket
```

### Steps

1. Build the unchanged matched client first.
2. Add reproducible compiler flags.
3. Preserve `VK_USE_PLATFORM_XLIB_KHR` if the matched client uses Xlib WSI.
4. Replace hard-coded paths with configuration:
   - `BACHATA_VORTEK_SOCKET`
   - optionally `BACHATA_VORTEK_LOG_LEVEL`
5. Add a protocol hello/handshake before normal request traffic:
   - magic
   - protocol version
   - client commit/build ID
   - pointer size
   - endianness
   - Vulkan header version
6. Package the client under the runtime host tree.
7. Generate a Bachata-specific ICD manifest with an app-owned relative/expanded path.
8. Keep the advertised Vulkan API at the highest actually implemented version.

### Do not

- hard-code `/data/data/com.winlator/...`
- hard-code Bachata's package name into the client if an environment/config path works
- advertise Vulkan 1.3 yet unless verified
- remove the original request serializer/ring-buffer design

### Tests

Add a tiny x86-64 guest test that:

1. Loads the Vulkan loader.
2. Calls `vkEnumerateInstanceVersion`.
3. Enumerates instance extensions.
4. Creates a minimal instance.
5. Enumerates physical devices.
6. Prints device name and API version.

### Acceptance criteria

- `libvulkan_vortek.so` is built reproducibly.
- The ICD manifest resolves to it.
- The guest test reaches the client.
- With no server, it fails quickly with `Vortek server unavailable`, not a hang or crash.

---

## Task 4 — Integrate the Android/Bionic Vortek server

### Goal

Run the matching Vortek server inside Bachata and make it open Android's system Vulkan loader.

### Recommended component model

Create a session-owned native server wrapper, not an Activity-owned singleton.

Suggested abstractions:

```text
VortekServerController
VortekServerState
VortekNativeBridge
VortekSurfaceProvider
VortekCapabilities
```

Suggested lifecycle:

```text
STOPPED
  -> STARTING
  -> SOCKET_READY
  -> SURFACE_READY
  -> RUNNING
  -> STOPPING
  -> STOPPED
```

Any failure transitions to `FAILED` with a diagnostic reason.

### Steps

1. Port/vendor the matching server sources.
2. Build them as an Android ARM64/Bionic native library or executable according to the matched architecture.
3. Load host Vulkan only through:
   ```cpp
   dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL)
   ```
4. Resolve and verify:
   - `vkGetInstanceProcAddr`
   - `vkCreateInstance`
5. Make the server socket path session-specific and app-owned.
6. Remove stale socket files only after verifying ownership and type.
7. Start the server before launching Box64.
8. Wait for an explicit readiness signal.
9. Stop the server after the emulator process exits.
10. Close all shared-memory FDs and ring buffers on disconnect.
11. Reject a mismatched client protocol version before creating a Vulkan context.
12. Make repeated start/stop safe.

### Acceptance criteria

- The server starts without shadPS4.
- It logs `libvulkan.so` and `vkGetInstanceProcAddr` success.
- The client handshake succeeds.
- Two sequential sessions start and stop without a stale socket.
- A mismatched client is rejected clearly.

---

## Task 5 — Add a distinct Bachata driver backend

### Goal

Expose Vortek as a real selectable runtime backend without breaking Turnip modes.

### Kotlin changes

Update:

- `RuntimeVulkanDriver`
- `RuntimeVulkanDriverPreference.decode`
- `VulkanDriverConfiguration.resolve`
- settings UI/catalog
- session setup
- environment allow list
- tests

### Required preference fix

The current decoder only recognizes `SYSTEM` and otherwise returns the default, which is also `SYSTEM`. Replace it with complete enum decoding plus explicit migration.

Example:

```kotlin
fun decode(value: String?): RuntimeVulkanDriver =
    when (value) {
        "SYSTEM" -> RuntimeVulkanDriver.SYSTEM_VORTEK
        else -> RuntimeVulkanDriver.entries.firstOrNull { it.name == value } ?: DEFAULT
    }
```

### Recommended Vortek configuration

Use the execution mode proven by Task 3. If the client is a glibc ICD, a likely configuration is:

```kotlin
RuntimeVulkanDriver.SYSTEM_VORTEK -> VulkanDriverConfiguration(
    box64Mode = Box64Mode.HOST_GLIBC,
    environment = mapOf(
        "SDL_VULKAN_LIBRARY" to runtimeRoot.resolve("host/libvulkan.so.1").toString(),
        "VK_ICD_FILENAMES" to runtimeRoot.resolve("host/vulkan/icd.d/vortek.json").toString(),
        "BACHATA_VORTEK_SOCKET" to sessionSocketPath,
    ),
)
```

Do not copy this blindly if the matched Winlator client uses another loader arrangement. Confirm with the guest probe.

### Environment allow list

Add only required variables, such as:

```text
BACHATA_VORTEK_SOCKET
BACHATA_VORTEK_LOG_LEVEL
BACHATA_VORTEK_TRACE
```

Continue validating that all paths are app-owned.

### UI label

Use an honest label:

```text
System Driver (Vortek, Experimental)
```

Description:

```text
Uses the device's Android Vulkan driver through the Vortek compatibility layer.
May work on Adreno, Mali, or other Vulkan-capable GPUs, but compatibility is experimental.
```

### Acceptance criteria

- Selecting Vortek produces Vortek-specific environment and lifecycle.
- Selecting Turnip does not start Vortek.
- No hidden fallback occurs.
- Preference migration works.
- Unit tests cover every enum entry.

---

## Task 6 — Prove non-WSI Vulkan operation

### Goal

Get the x86-64 guest probe through instance and device creation using the device system driver.

### Probe stages

1. Client/server handshake.
2. `vkEnumerateInstanceVersion`.
3. `vkEnumerateInstanceExtensionProperties`.
4. `vkCreateInstance`.
5. `vkEnumeratePhysicalDevices`.
6. `vkGetPhysicalDeviceProperties2`.
7. `vkGetPhysicalDeviceFeatures2`.
8. `vkEnumerateDeviceExtensionProperties`.
9. queue-family enumeration.
10. `vkCreateDevice`.
11. `vkGetDeviceQueue`.
12. command pool/buffer creation.
13. basic buffer allocation.
14. submit and wait idle.

### Instrumentation

On an unsupported request, log:

```text
request code
function name
sequence number
thread ID
serialized payload size
last successful function
```

On an unsupported `pNext` structure, log the exact `VkStructureType`.

### Acceptance criteria

- The guest sees the real vendor GPU name.
- The guest creates a logical device.
- A command buffer is submitted successfully.
- No fake extension/API claims are used.
- The first unsupported call, if any, is deterministic and logged.

---

## Task 7 — Integrate WSI and Android Surface ownership

### Goal

Create and present a swapchain through the Android device driver.

### Efficient implementation rule

Reuse the matched Winlator Vortek/XServer WSI implementation. Do not invent a new Xlib-to-Android mapping before auditing it.

The public Vortek client is compiled with `VK_USE_PLATFORM_XLIB_KHR`, so the guest-facing path is expected to expose Xlib WSI. The server side must ultimately bind presentation to Bachata's Android surface.

### Surface ownership

1. Identify the existing Bachata display surface owner.
2. Pass the Java `Surface` through JNI.
3. Convert it with:
   ```cpp
   ANativeWindow_fromSurface(env, surface)
   ```
4. Hold an acquired reference while the Vortek session is active.
5. Release it exactly once during shutdown or replacement.
6. Never serialize or expose the pointer to the guest process.

### Required WSI probe

Create an x86-64 Xlib Vulkan test that:

1. Opens the existing Bachata X display.
2. Creates a small X window.
3. Requests the Vortek ICD.
4. Creates a `VkSurfaceKHR`.
5. Chooses a present-capable queue.
6. Creates a swapchain.
7. Clears/presents alternating frames for at least 60 seconds.
8. Handles surface-size changes.
9. Exits cleanly.

### Acceptance criteria

- The screen shows the probe frames.
- Logs identify the real system GPU.
- Surface and swapchain are created through Vortek.
- No frame-copy fallback is silently used unless explicitly designed and measured.
- Background/foreground either recovers or reports `surface recreation required` cleanly.

---

## Task 8 — Add shadPS4-required Vulkan 1.3 coverage

### Goal

Implement the minimum Vulkan surface, core, extension, and `pNext` coverage required for current shadPS4 startup.

### Mandatory capability gate

At minimum, verify:

```text
Vulkan 1.3
VK_KHR_swapchain
VK_KHR_push_descriptor
```

Do not assume these are the only calls needed. Use logs from the actual shadPS4 build.

### Implementation order

1. Run shadPS4 without launching a game.
2. Record the first unsupported function/structure.
3. Implement it on both client and server.
4. Add a focused unit or protocol test.
5. Repeat.
6. Stop after shadPS4 creates its instance/device and reaches its idle UI or game-launch boundary.

Likely high-priority areas include:

- core 1.1/1.2/1.3 promoted entry points
- feature/property `pNext` chains
- `VK_KHR_push_descriptor`
- `VK_KHR_synchronization2`
- dynamic rendering
- descriptor update templates
- timeline semaphores
- memory requirements 2
- dedicated allocation
- image-format queries
- pipeline cache and pipeline creation feedback
- shader module creation
- swapchain functions

Treat this list as a hypothesis, not permission to implement unused APIs blindly.

### Manifest gate

Only after the Vortek path truthfully supports the required version:

- update the ICD manifest API version
- update capability-probe expectations
- add a regression test preventing accidental over-advertisement

### Acceptance criteria

- shadPS4 no longer rejects the Vulkan API version.
- It sees `VK_KHR_swapchain` and `VK_KHR_push_descriptor`.
- Instance and device creation succeed.
- Unsupported calls fail with names, not numeric-only request codes.

---

## Task 9 — Target Sonic Mania

### Goal

Reach a stable, playable Sonic Mania session through Vortek.

### Test loop

1. Launch only Sonic Mania.
2. Record:
   - last 100 Vulkan calls before failure
   - first unsupported call/structure
   - GPU/driver properties
   - memory allocation failures
   - shader/pipeline errors
   - surface/swapchain state
3. Implement the smallest correct missing behavior.
4. Add a regression probe where possible.
5. Repeat until:
   - boot logo/menu renders
   - gameplay renders
   - input works
   - audio remains unaffected
   - frame presentation is stable

### Do not

- add game-title hard-coded Vulkan behavior unless it is a temporary diagnostic patch
- suppress failed Vulkan return codes
- turn off required synchronization globally
- report an extension without implementing it
- modify shadPS4 broadly to avoid fixing the bridge

### Performance measurements

Capture:

- FPS in the starting room
- frame-time median, p95, and p99 if available
- CPU usage by:
  - shadPS4/Box64
  - Vortek client
  - Vortek server
- request count per frame
- bytes serialized per frame
- ring-buffer wait time
- host queue-submit/present time

### Acceptance criteria

- Sonic Mania reaches in-game.
- It remains playable for at least 15 minutes.
- No server/client deadlock occurs.
- No continuous unbounded memory growth occurs.
- The selected driver is visibly and log-verifiably Vortek.

---

## Task 10 — Optimize only measured bottlenecks

### Goal

Remove Vortek overhead that materially affects Sonic Mania after correctness is achieved.

### Priorities

1. Eliminate unnecessary synchronous round trips.
2. Batch command-buffer traffic using the existing ring-buffer design.
3. Avoid copying large immutable payloads repeatedly.
4. Cache:
   - function mappings
   - object mappings
   - immutable physical-device properties
   - extension lists
5. Move non-critical server work off the UI thread.
6. Avoid per-call allocations in hot Vulkan paths.
7. Keep tracing disabled by default in release builds.

### Guardrails

- Do not weaken synchronization without proof.
- Do not add speculative multithreading to the protocol.
- Do not optimize before profiling.
- Keep a correctness/debug mode.

### Acceptance criteria

- Performance comparison is documented against the first playable build.
- No regression in the WSI probe.
- No regression in Turnip modes.
- Debug logging can still identify the last request before a failure.

---

## Task 11 — Lifecycle, recovery, and device compatibility

### Goal

Make Vortek safe enough for an experimental user-facing option.

### Scenarios

Test:

- normal launch/exit
- force stop
- game crash
- server crash
- client disconnect
- app background/foreground
- screen rotation if supported
- display surface replacement
- two sequential game launches
- low-memory process pressure
- unsupported Vulkan version
- missing `VK_KHR_push_descriptor`
- no present-capable queue
- Mali device if available
- Adreno system driver
- Android 12 through current supported target where available

### Required user errors

Examples:

```text
System Driver (Vortek) unavailable: device reports Vulkan 1.1; shadPS4 requires Vulkan 1.3.
System Driver (Vortek) unavailable: VK_KHR_push_descriptor is missing.
Vortek server failed to start.
Vortek protocol mismatch. Reinstall/update the runtime.
Android rendering surface was lost. Restart the game session.
```

### Acceptance criteria

- No stale server/socket remains after exit.
- Capability failures are actionable.
- The app does not crash because the device lacks a required extension.
- Existing driver selection remains usable after a Vortek failure.

---

## Task 12 — Packaging, CI, notices, and final documentation

### Goal

Make the implementation reproducible and maintainable.

### Update

- runtime component locks
- source vendoring script
- runtime packaging script
- runtime verification
- Android native CMake
- unit tests
- instrumentation/device tests
- `NOTICE.android-runtime.md`
- build documentation
- driver UI documentation
- troubleshooting guide

### CI checks

Add checks that verify:

- Vortek client exists in the runtime package.
- ICD manifest points to the packaged library.
- manifest API version matches the configured supported level.
- Android server native library is packaged.
- required environment variables are allow-listed.
- old Turnip configurations remain unchanged.
- source and license metadata exist.
- no `/data/data/com.winlator` or `com.winlator` hard-coded runtime path remains in the Bachata Vortek build.

### Final acceptance report

Provide:

1. architecture summary
2. pinned upstream revisions
3. changed files
4. build commands
5. probe results
6. Sonic Mania result
7. performance numbers
8. supported/unsupported devices
9. known missing Vulkan calls
10. rollback plan

---

# Overall Definition of Done

The work is done when:

- Vortek is a distinct experimental backend.
- The client/server pair is pinned and reproducible.
- The guest enumerates the actual Android system GPU.
- WSI presents frames through an Android surface.
- shadPS4 truthfully receives Vulkan 1.3 plus required extensions.
- Sonic Mania is playable for at least 15 minutes.
- existing Turnip backends still work.
- lifecycle cleanup is reliable.
- unsupported devices fail with clear messages.
- licensing, locks, packaging, and tests are complete.
