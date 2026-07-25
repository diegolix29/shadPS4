# Bachata S4 — Vortek System Driver Technical Design

## 1. Overview

This design adds a **Vortek System Driver** backend to Bachata S4.

The backend allows the x86-64 Linux/glibc shadPS4 binary to issue Vulkan calls that are transported to an Android/Bionic server, which then executes them through Android's platform Vulkan loader and the device's installed vendor driver.

The first compatibility target is Sonic Mania.

## 2. Goals

### Primary goal

Run Sonic Mania using the Android device's system Vulkan driver rather than requiring a selected glibc Turnip ICD.

### Secondary goals

- Reuse the Vortek client/server architecture instead of building a new Vulkan bridge.
- Preserve all existing Turnip modes.
- Support a future compatibility path for non-Adreno devices where their system Vulkan driver meets shadPS4 requirements.
- Make unsupported Vulkan functions and structures easy to identify.
- Keep the implementation reproducibly buildable for Play Store and F-Droid variants.

## 3. Non-goals for the first milestone

- Full shadPS4 game compatibility.
- Perfect Vulkan 1.3 conformance beyond the paths required by shadPS4/Sonic Mania.
- Replacing the current Turnip backends.
- Direct loading of vendor HAL files.
- Rewriting the integrated X server.
- Creating a new Vulkan RPC protocol.
- Supporting multiple simultaneous game sessions.
- Hiding unsupported device capabilities through false extension reporting.

## 4. Current state

Bachata S4 currently runs:

```text
x86-64 shadPS4
  -> Box64
  -> managed glibc/rootfs
  -> Vulkan driver path
  -> integrated X11 display
```

Current Vulkan configurations include:

### glibc ICD path

```text
Box64Mode.HOST_GLIBC
SDL_VULKAN_LIBRARY=<runtime>/host/libvulkan.so.1
VK_ICD_FILENAMES=<Turnip manifest>
```

### Android/Bionic custom-driver path

```text
Box64Mode.APK_NATIVE
SDL_VULKAN_LIBRARY=libvulkan.so.1
BACHATA_VULKAN_DRIVER_DIR=...
BACHATA_VULKAN_DRIVER_NAME=...
BACHATA_VULKAN_TMPDIR=...
```

The native `custom_vulkan_bridge.cpp` uses AdrenoTools to open a selected custom Turnip driver.

### Incomplete system path

The current `SYSTEM` configuration selects `APK_NATIVE` and sets only:

```text
SDL_VULKAN_LIBRARY=libvulkan.so.1
```

It does not provide a complete ABI/WSI bridge from the Linux guest to Android's system Vulkan driver.

## 5. Why Vortek

The public Vortek client:

- is a Vulkan wrapper/compatibility client for Winlator
- connects to a separate server over a Unix-domain socket
- receives shared-memory file descriptors
- creates server-to-client and client-to-server ring buffers
- serializes Vulkan calls
- is compiled with Xlib Vulkan platform support
- delegates host-driver compatibility work to the server

This directly matches Bachata's core problem:

```text
Linux/glibc Vulkan application
  -> Android/Bionic system Vulkan driver
```

A Vortek integration is more efficient than creating a new Box64/Bionic Vulkan wrapper because it already provides:

- client/server ABI isolation
- object mapping
- request codes
- serialization
- shared-memory ring buffers
- a Winlator-tested Android host side
- format/shader/texture compatibility mechanisms in the server implementation

## 6. Chosen architecture

```text
┌──────────────────────────────────────────────────────────────┐
│ Android Bachata S4 process/session                           │
│                                                              │
│  SessionController                                           │
│      │                                                       │
│      ├── VortekServerController                              │
│      │      ├── Unix socket                                  │
│      │      ├── shared-memory rings                          │
│      │      ├── Android Surface -> ANativeWindow             │
│      │      └── Android libvulkan.so -> vendor driver        │
│      │                                                       │
│      └── RuntimeProcessLauncher                              │
│             └── host glibc loader + Box64                    │
└───────────────────────┬──────────────────────────────────────┘
                        │ app-owned Unix socket + shared memory
┌───────────────────────▼──────────────────────────────────────┐
│ Guest/runtime side                                            │
│                                                              │
│ x86-64 shadPS4                                                │
│    -> Box64                                                   │
│    -> AArch64 glibc Vulkan loader                             │
│    -> libvulkan_vortek.so                                    │
│    -> serialized Vulkan request stream                       │
└──────────────────────────────────────────────────────────────┘
```

This is the preferred initial shape if it matches the pinned Winlator integration.

An alternative APK-native client arrangement is acceptable only if the matched source demonstrates that architecture and the guest probe confirms it.

## 7. Components

## 7.1 `VortekServerController`

Kotlin/session-layer owner of the server lifecycle.

Responsibilities:

- generate a session-specific app-owned socket path
- start native server
- wait for readiness
- attach/update the Android `Surface`
- expose capability result
- stop server
- remove stale owned socket
- surface diagnostics to UI/session logs

Suggested interface:

```kotlin
interface VortekServerController {
    suspend fun start(config: VortekServerConfig): VortekStartResult
    suspend fun attachSurface(surface: Surface): Result<Unit>
    suspend fun detachSurface(): Result<Unit>
    suspend fun stop(reason: String)
    val state: StateFlow<VortekServerState>
}
```

Suggested state:

```kotlin
sealed interface VortekServerState {
    data object Stopped : VortekServerState
    data object Starting : VortekServerState
    data class SocketReady(val path: Path) : VortekServerState
    data object WaitingForSurface : VortekServerState
    data class Running(val capabilities: VortekCapabilities) : VortekServerState
    data object Stopping : VortekServerState
    data class Failed(val stage: String, val message: String) : VortekServerState
}
```

## 7.2 Native Vortek server wrapper

Android ARM64/Bionic native code.

Responsibilities:

- own listen socket
- authenticate/validate local client protocol
- allocate shared-memory rings
- transfer FDs with `SCM_RIGHTS`
- open Android Vulkan loader
- dispatch request codes
- map guest object IDs to native Vulkan handles
- manage server-side compatibility transformations
- own/refer to `ANativeWindow`
- destroy all Vulkan objects and mappings on disconnect

Host loader:

```cpp
void* loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
auto getInstanceProcAddr =
    reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(loader, "vkGetInstanceProcAddr"));
```

Do not directly open a vendor HAL.

## 7.3 Vortek guest/client ICD

AArch64 glibc shared library loaded by the guest-side Vulkan loader.

Responsibilities:

- export Vulkan ICD entry points expected by the loader
- connect to the configured Unix socket
- perform protocol handshake
- map guest handles/objects
- serialize requests and `pNext` chains
- use shared-memory rings for high-frequency traffic
- return server responses and Vulkan result codes
- expose Xlib WSI expected by shadPS4/SDL

Configuration:

```text
BACHATA_VORTEK_SOCKET
BACHATA_VORTEK_LOG_LEVEL
BACHATA_VORTEK_TRACE
```

The socket path must not remain a compile-time Winlator path.

## 7.4 ICD manifest

Packaged in the runtime host tree, for example:

```text
host/vulkan/icd.d/vortek.json
```

Example shape:

```json
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "/resolved/app/runtime/host/lib/libvulkan_vortek.so",
    "api_version": "1.1.128"
  }
}
```

The final path may be generated during runtime installation/packaging.

The API version must represent implemented behavior. It must not be changed to 1.3 until the compatibility gate passes.

## 7.5 Driver configuration

Recommended new driver identity:

```kotlin
SYSTEM_VORTEK
```

Likely configuration if using a glibc ICD:

```kotlin
RuntimeVulkanDriver.SYSTEM_VORTEK -> VulkanDriverConfiguration(
    box64Mode = Box64Mode.HOST_GLIBC,
    environment = mapOf(
        "SDL_VULKAN_LIBRARY" to runtimeRoot.resolve("host/libvulkan.so.1").toString(),
        "VK_ICD_FILENAMES" to runtimeRoot
            .resolve("host/vulkan/icd.d/vortek.json")
            .toString(),
        "BACHATA_VORTEK_SOCKET" to sessionSocketPath.toString(),
    ),
)
```

Because `VulkanDriverConfiguration.resolve` currently does not receive a session socket path, refactor its input into a context object rather than introducing a global.

Example:

```kotlin
data class VulkanDriverResolveContext(
    val runtimeRoot: Path,
    val customDriverRoot: Path?,
    val vortekSocketPath: Path?,
)
```

## 7.6 Capability probe

A probe is required before shadPS4 launch.

Data model:

```kotlin
data class VortekCapabilities(
    val loaderAvailable: Boolean,
    val instanceVersion: UInt,
    val physicalDeviceName: String?,
    val deviceApiVersion: UInt?,
    val instanceExtensions: Set<String>,
    val deviceExtensions: Set<String>,
    val graphicsQueueAvailable: Boolean,
    val presentQueueAvailable: Boolean,
    val supportsSwapchain: Boolean,
    val supportsPushDescriptor: Boolean,
)
```

Minimum shadPS4 gate:

```text
Vulkan API >= 1.3
VK_KHR_swapchain
VK_KHR_push_descriptor
graphics queue
present support
```

The probe must run through Vortek, not directly in a native Android sample, because the client/server path may expose less than the host driver.

## 8. Process and lifecycle sequence

## 8.1 Startup

```text
User selects System Driver (Vortek)
  -> create session ID
  -> create app-owned socket path
  -> acquire/display Surface
  -> start Vortek server
  -> server opens libvulkan.so
  -> server reports socket ready
  -> attach Surface / ANativeWindow
  -> run Vortek capability probe
  -> reject unsupported device, or continue
  -> build Vortek runtime environment
  -> launch Box64 + shadPS4
  -> client connects and negotiates protocol
  -> game session runs
```

## 8.2 Shutdown

```text
shadPS4 exits or session stops
  -> stop accepting new requests
  -> client disconnect
  -> wait/cancel server workers
  -> vkDeviceWaitIdle where safe
  -> destroy swapchains/surfaces/device/instance
  -> release ANativeWindow
  -> unmap ring buffers
  -> close shared-memory FDs
  -> close socket
  -> unlink owned socket file
  -> transition to STOPPED
```

Shutdown must have a bounded fallback path. A stuck Vulkan call must not leave the app permanently unable to launch another session.

## 8.3 Surface loss

On backgrounding or display replacement:

```text
Surface destroyed
  -> mark WSI unavailable
  -> stop new present operations
  -> destroy/recreate swapchain as required
  -> release old ANativeWindow
  -> attach new Surface
  -> recreate native surface/swapchain
```

For the first milestone, a controlled session restart is acceptable if live recreation is not yet reliable, but it must be explicit and leak-free.

## 9. Protocol design

## 9.1 Preserve upstream request protocol

Use upstream request codes and serializers. Bachata-specific protocol changes should be minimal.

## 9.2 Add a handshake

Before `CREATE_CONTEXT`, exchange:

```c
struct BachataVortekHello {
    uint32_t magic;
    uint16_t protocol_major;
    uint16_t protocol_minor;
    uint32_t pointer_size;
    uint32_t endianness;
    uint32_t vulkan_header_version;
    uint8_t client_build_id[20];
};
```

Server response:

```c
struct BachataVortekHelloReply {
    uint32_t magic;
    uint16_t accepted_major;
    uint16_t accepted_minor;
    int32_t status;
    uint8_t server_build_id[20];
};
```

Reject incompatible major versions.

## 9.3 Transport

The upstream client uses:

- Unix-domain stream socket for setup/control
- FD passing for two shared-memory regions
- a server ring
- a client ring

Preserve this pattern.

Control-plane traffic can remain socket-based. High-frequency Vulkan calls should use the existing ring buffers.

## 9.4 Threading

Initial safe model:

- one accept/control thread
- one ordered Vulkan-dispatch thread per client/context
- optional upstream worker/thread-pool behavior only where already proven
- UI thread never blocks on Vulkan request dispatch

Vulkan command ordering must remain correct. Do not parallelize request handling speculatively.

## 10. Vulkan object model

The client must not send native pointer values as authoritative object handles.

Maintain explicit object IDs/mappings for:

- `VkInstance`
- `VkPhysicalDevice`
- `VkDevice`
- `VkQueue`
- `VkCommandPool`
- `VkCommandBuffer`
- `VkBuffer`
- `VkImage`
- `VkDeviceMemory`
- `VkImageView`
- `VkSampler`
- `VkDescriptorSetLayout`
- `VkPipelineLayout`
- `VkPipeline`
- `VkShaderModule`
- `VkSemaphore`
- `VkFence`
- `VkSurfaceKHR`
- `VkSwapchainKHR`
- other reached object types

Destroy operations must remove mappings and reject stale IDs in debug builds.

## 11. `pNext` serialization

shadPS4 makes extensive feature/property queries. `pNext` support is a critical risk.

Required behavior:

1. Walk the guest chain.
2. Validate each `sType`.
3. Serialize supported structure fields.
4. Rebuild a native server-side chain.
5. Call host Vulkan.
6. Serialize output structures back.
7. Preserve ordering where required.
8. Log unsupported `sType` with the calling function.

Never copy raw pointer-bearing structures across the ABI boundary.

Maintain a central registry:

```text
VkStructureType
  -> structure size
  -> direction: input/output/inout
  -> serializer
  -> deserializer
  -> nested-array handling
```

## 12. WSI design

## 12.1 Guest-facing interface

The Vortek client is built with Xlib platform support. shadPS4/SDL can continue to see:

```text
VK_KHR_surface
VK_KHR_xlib_surface
vkCreateXlibSurfaceKHR
vkGetPhysicalDeviceXlibPresentationSupportKHR
```

## 12.2 Host-facing interface

The Android server ultimately creates/presents against an Android-native surface:

```text
VK_KHR_surface
VK_KHR_android_surface
ANativeWindow
```

Reuse Winlator's matched Vortek/XServer path to associate the guest X window with the Android surface.

Do not assume that simply translating `vkCreateXlibSurfaceKHR` to one global `ANativeWindow` is sufficient. Audit upstream handling for:

- window identity
- dimensions
- fullscreen changes
- surface replacement
- swapchain extent
- orientation
- synchronization with the X server

## 12.3 Surface provider

Suggested JNI interface:

```cpp
extern "C" JNIEXPORT jboolean JNICALL
Java_com_bachatas4_android_runtime_vortek_VortekNativeBridge_nativeAttachSurface(
    JNIEnv* env,
    jclass,
    jobject surface);

extern "C" JNIEXPORT void JNICALL
Java_com_bachatas4_android_runtime_vortek_VortekNativeBridge_nativeDetachSurface(
    JNIEnv* env,
    jclass);
```

Use `ANativeWindow_acquire`/`ANativeWindow_release` according to ownership.

## 13. Vulkan version and extension policy

## 13.1 Host capability

The Android vendor driver may expose Vulkan 1.3 and required extensions.

## 13.2 Bridge capability

The Vortek bridge may expose less than the host.

The reported capability is:

```text
min(host capability, client implementation, server implementation, safe emulation)
```

## 13.3 Required shadPS4 gate

Current shadPS4 minimum:

```text
Vulkan 1.3
VK_KHR_swapchain
VK_KHR_push_descriptor
```

## 13.4 Truthful reporting

For every reported extension:

- enumeration must include it
- its required entry points must resolve
- relevant structures must serialize
- behavior must be implemented or correctly emulated
- unsupported use must return a valid Vulkan error, not crash

Do not list extensions just to pass shadPS4 startup.

## 14. Runtime layout

Suggested runtime files:

```text
runtime-root/
  host/
    lib/
      libvulkan.so.1
      libvulkan_vortek.so
    vulkan/
      icd.d/
        vortek.json
  diagnostics/
    vortek/
  tmp/
    vortek/
```

Android native library:

```text
nativeLibraryDir/
  libbachata_vortek_server.so
```

Session socket:

```text
<app-storage>/sessions/<session-id>/vortek.sock
```

Avoid the Unix socket path-length limit by keeping this path short. Validate its encoded length before bind/connect.

## 15. Environment

Required:

```text
VK_ICD_FILENAMES
SDL_VULKAN_LIBRARY
BACHATA_VORTEK_SOCKET
```

Optional:

```text
BACHATA_VORTEK_LOG_LEVEL
BACHATA_VORTEK_TRACE
BACHATA_VORTEK_DUMP_UNSUPPORTED
```

Add them to the existing environment allow list.

Do not pass:

- raw native pointers
- arbitrary external paths
- vendor HAL paths
- unvalidated package-derived paths

## 16. Diagnostics

## 16.1 Session header

Log once:

```text
backend=SYSTEM_VORTEK
client_commit=...
server_commit=...
protocol=...
box64_mode=...
guest_loader=...
icd_manifest=...
socket=...
android_version=...
device=...
gpu=...
```

## 16.2 Request trace

Debug-only bounded ring log:

```text
sequence
thread
function
request bytes
response bytes
result
duration
```

On crash/failure, dump the latest N entries.

## 16.3 Capability report

Log:

- instance version
- device API version
- vendor/device IDs
- queue families
- memory heaps
- required extensions
- missing extensions
- WSI formats/present modes
- selected surface format
- selected present mode

## 17. Failure model

Typed failures:

```kotlin
sealed interface VortekFailure {
    data class SourceMismatch(...) : VortekFailure
    data class ServerStartFailed(...) : VortekFailure
    data class SocketBindFailed(...) : VortekFailure
    data class ProtocolMismatch(...) : VortekFailure
    data class VulkanLoaderMissing(...) : VortekFailure
    data class VulkanSymbolMissing(...) : VortekFailure
    data class UnsupportedApiVersion(...) : VortekFailure
    data class MissingExtension(...) : VortekFailure
    data class UnsupportedRequest(...) : VortekFailure
    data class UnsupportedStructure(...) : VortekFailure
    data class SurfaceUnavailable(...) : VortekFailure
    data class SwapchainFailed(...) : VortekFailure
    data class DeviceLost(...) : VortekFailure
}
```

Never convert all failures into a generic game crash.

## 18. Security

- Bind only inside app-owned storage.
- Use filesystem permissions preventing other apps from connecting.
- Validate handshake magic/version.
- Validate request sizes before ring-buffer reads.
- Bound arrays and strings.
- Reject integer overflow in serialized lengths.
- Validate object IDs.
- Treat guest process requests as untrusted.
- Never trust guest pointers.
- Close received FDs on all error paths.
- Avoid executable extraction outside approved runtime/native locations.
- Preserve Android linker namespace constraints.

## 19. Performance strategy

Correctness first.

Likely hotspots:

- per-call socket round trips
- ring-buffer contention
- command-buffer serialization
- shader-module payload copies
- pipeline creation
- memory mapping/copying
- queue submit/present synchronization
- logging

Optimization principles:

- keep setup/control on socket
- keep hot calls on shared-memory rings
- batch ordered command traffic where upstream already supports it
- cache immutable capability results
- avoid repeated serialization of unchanged objects
- avoid malloc/free in per-draw paths
- use sequence counters rather than coarse locks where safe
- keep detailed tracing off in normal builds
- measure request count and bytes per frame

## 20. Build integration

Use existing repository mechanisms:

- native CMake under the runtime module
- Winlator vendoring script
- runtime lock files
- runtime packaging script
- runtime verification
- Android Gradle variants

Potential CMake structure:

```cmake
add_subdirectory(vortek/server)

add_library(bachata_vortek_server SHARED
    vortek/bachata_vortek_jni.cpp
    vortek/bachata_vortek_lifecycle.cpp
    # matched upstream server sources
)

target_link_libraries(bachata_vortek_server
    PRIVATE
    android
    dl
    log
)
```

The glibc client should be built by the host-runtime toolchain, not accidentally by the Android NDK, unless the matched architecture explicitly requires that.

## 21. Testing strategy

## 21.1 Unit tests

- driver preference decode/migration
- driver environment creation
- environment allow list
- socket-path validation
- protocol hello serialization
- request-size validation
- object-pool lifecycle
- API-version gating
- extension gating
- unsupported-structure reporting

## 21.2 Native host tests

- open `libvulkan.so`
- resolve symbols
- enumerate host Vulkan
- create/destroy native instance
- attach/detach `ANativeWindow`

## 21.3 Guest probes

1. instance/device probe
2. buffer/submit probe
3. Xlib surface probe
4. swapchain present loop
5. stress create/destroy loop
6. pause/resume surface loop

## 21.4 Application tests

- Sonic Mania
- existing Turnip smoke test
- repeated session launch
- force-stop cleanup
- unsupported-device messaging

## 22. Rollout

### Phase A — developer-only

- backend hidden behind developer flag
- verbose logs
- one known device
- Sonic Mania only

### Phase B — experimental UI

- visible as `System Driver (Vortek, Experimental)`
- capability probe
- clear unsupported-device errors
- no automatic selection

### Phase C — broader testing

- Adreno system driver
- Mali system driver
- additional lightweight games
- telemetry only if privacy-compliant and opt-in

Do not make Vortek the default until it is more reliable than existing Turnip for the supported target group.

## 23. Risks and mitigations

### Risk: public Vortek client is older than Winlator's internal server

Mitigation:

- pin a matched client/server revision
- add protocol handshake
- vendor both together

### Risk: Vulkan 1.3 coverage is large

Mitigation:

- implement only calls reached by shadPS4/Sonic Mania
- use deterministic unsupported-call logging
- create focused probes for each added call

### Risk: `pNext` chains cause crashes or false features

Mitigation:

- central structure registry
- strict unsupported-structure errors
- round-trip tests
- truthful feature reporting

### Risk: WSI does not match Bachata's X server

Mitigation:

- reuse matched Winlator Vortek/XServer integration
- first build a 60-second present-loop probe
- keep surface ownership explicit

### Risk: IPC overhead reduces FPS

Mitigation:

- shared-memory rings
- avoid synchronous per-draw socket traffic
- profile before optimizing
- batch where upstream does

### Risk: app lifecycle loses `ANativeWindow`

Mitigation:

- session-owned surface provider
- explicit attach/detach
- controlled restart fallback
- leak tests

### Risk: existing Turnip path regresses

Mitigation:

- separate enum/backend
- regression tests
- no shared global environment
- smoke test every release

## 24. Acceptance criteria

The architecture is accepted when:

1. A matched Vortek client/server pair is pinned.
2. A clean build reproducibly packages both sides.
3. The x86-64 guest probe enumerates the actual Android system GPU.
4. The guest creates a logical device.
5. The Xlib/Android WSI probe presents frames for 60 seconds.
6. The Vortek path truthfully exposes Vulkan 1.3 plus:
   - `VK_KHR_swapchain`
   - `VK_KHR_push_descriptor`
7. shadPS4 creates its Vulkan instance/device.
8. Sonic Mania reaches in-game and remains playable for 15 minutes.
9. Existing Turnip modes still work.
10. Shutdown, crash, and repeat-launch tests leave no stale server/socket/window.
11. Unsupported devices receive actionable messages.
12. Licenses, notices, locks, packaging, and tests are complete.

## 25. Reference links

- Bachata S4: https://github.com/JICA98/Bachata-S4
- Vortek client: https://github.com/brunodev85/vortek
- Winlator: https://github.com/brunodev85/winlator
- shadPS4: https://github.com/shadps4-emu/shadPS4
- shadPS4 quick-start requirements: https://github.com/shadps4-emu/shadPS4/wiki/I.-Quick-start-%5BUsers%5D
