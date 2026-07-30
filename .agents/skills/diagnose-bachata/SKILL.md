---
name: diagnose-bachata
description: Use when a Bachata-S4 / shadPS4 Android game crashes, reports Stopped or exitCode, freezes, black-screens, GPU-deadlocks, fails after a cutscene or teardown transition, or behaves differently from desktop.
---

# Diagnose Bachata-S4

Structured investigation workflow for shadPS4/Bachata-S4 Android runtime failures
(crash-on-launch, exit codes, black screen, GPU hang). Encodes the paths, commands,
and code locations learned from real fixes so each new investigation starts from
the known map, not a blind scan.

**Core rule:** Treat the exit code and nearest warning as routing hints. Prove the
failure by correlating the terminal error with the lifetime of the exact guest
address, handle, callback, fence, or thread involved.

## Orientation map (read first — don't re-scan)

Resolve the repo root with `git rev-parse --show-toplevel`. All paths below are
relative to it.

### Where things live

| Concern | Location |
|---|---|
| HLE library implementations | `src/core/libraries/<lib>/` (e.g. `fios2/fios2.cpp`, `playgo/playgo.cpp`) |
| NID → symbol name table | `src/core/aerolib/aerolib.inl` (`STUB("nid", name)` = name known, NO impl) |
| HLE registration in a lib | the lib's `RegisterLib()` — `LIB_FUNCTION("nid", "lib", ver, "mod", fn)` |
| FEX unresolved-HLE fallback | `src/core/linker.cpp:~802` ("temporary ENOSYS fallback") → returns `ENOSYS=38` |
| UnsupportedHleCallAdapter | `src/core/guest_cpu/hle_call_adapter.h` (returns `HleCallFailure{ENOSYS}`) |
| FEX guest callback bridge | `src/core/guest_cpu/fex_hle_bridge.*`, `src/core/guest_cpu/hle_call_adapter.*` |
| AvPlayer callback/source lifetime | `src/core/libraries/avplayer/` |
| Pthread mutex lifetime | `src/core/libraries/kernel/threads/mutex.cpp`, `src/core/libraries/kernel/threads/pthread.h` |
| GPU command processor (PM4) | `src/video_core/amdgpu/liverpool.cpp` |
| PM4 opcodes | `src/video_core/amdgpu/pm4_opcodes.h` (`WaitRegMem=0x3c`, `EventWriteEop=0x47`, ...) |
| PM4 packet structs + Test() | `src/video_core/amdgpu/pm4_cmds.h` |
| Guest memory layout | `src/core/address_space.cpp` (SYSTEM_MANAGED `0x400000-0x7FFFFBFFF`, etc.) |
| Memory backing write | `src/core/memory.cpp` `TryWriteBacking()` (~line 155) |
| Vulkan instance / extensions | `src/video_core/renderer_vulkan/vk_instance.cpp` |
| Android runtime launch | `android/BachataS4/core/runtime/.../process/RuntimeProcessLauncher.kt` |
| Session state / exit code | `android/BachataS4/core/runtime/.../session/ManagedSession.kt` (`Stopped(exitCode)`) |

### Exit code cheat sheet

Android `Process.exitCode()` commonly reports `128 + signal`, but the number alone
is not a diagnosis. Guest panic paths may deliberately fault, enter shadPS4's signal
handler, and finish through an assertion/trap path.
- **127 = Shared Library Missing** → `shadPS4 exited before socket connect: 127: .../shadps4-arm64-fex: error while loading shared libraries: <libname>.so.N: cannot open shared object file: No such file or directory` (e.g. `libcap.so.2`). Indicates a host library dependency gap in Debian runtime packaging.
  - **Fix**: Add missing `.so` to `arm64Explicit` in `runtime/scripts/stage-debian-runtime.mjs`, re-stage, and rebuild runtime assets.
- **133** → route immediately to the last guest panic, `EINVAL`, unhandled access,
  `ASSERT`, `UNREACHABLE`, or `SignalHandler` lines. It often ends in a trap/assert,
  but may begin with a deliberate guest null write or another handled fault.
- **134 = SIGABRT**, **139 = SIGSEGV**, **137 = SIGKILL (OOM)**.

For 133, find the terminal `<Critical> SignalHandler` or unhandled-access line, then
walk backward to the first subsystem error or guest panic. Never stop at
`Unreachable code!`; it is usually the final consequence.

## Step 1 — Pull the failing session log (do this before reading code)

Session logs live on-device under the app's private storage. Select adb by evidence:

```bash
ADB=adb
"$ADB" devices -l
# On WSL only, if Linux adb sees no device and adb.exe does:
# ADB=/full/path/to/adb.exe

cd android/BachataS4
ADB_OVERRIDE="$ADB" ./pull-session-logs.sh --game <CUSAxxxxx> --output session-logs
```

Each session dir is named `YYYYMMDD-HHMMSS-<CUSAid>-<hash>` and contains:
- `application.log` — app lifecycle, exit code, `guestBackend=fex|box64`, `driver=turnip-...`
- `shadps4.log` — backend stdout/stderr (the real crash evidence; can be millions of lines)
- `shadps4-internal.log` — copied shadPS4 internal log (if present)

On native Linux/PikaOS, `/usr/bin/adb` normally owns the USB device. On WSL there
may be separate Linux and Windows adb servers; use whichever actually lists the
target serial. Use the same adb binary for every command in the session.

List sessions without pulling: `./pull-session-logs.sh --list`
Pull newest: `./pull-session-logs.sh --latest`

A still-running game holds its log open — to flush, `adb.exe -s <serial> shell am
force-stop com.bachatas4.android`, wait 2s, then pull.

## Step 2 — Process big logs in-sandbox (don't Read them raw)

`shadps4.log` routinely hits millions of lines (GPU coroutine spin spam).
**Never `Read` the whole file.** Use `ctx_execute_file` over the pulled log to
filter/aggregate, or `ctx_search` if indexed. Example first pass:

```javascript
const l = FILE_CONTENT.split('\n').filter(Boolean);
const clean = x => x.replace(/\x1b\[[0-9;]*m/g, '').slice(0, 170);
// errors / fatal / critical
console.log(l.filter(x => /<Error>|<Critical>|UNREACHABLE|Unhandled access|SIGTRAP/i.test(x))
             .slice(-30).map(clean).join('\n'));
// last 20 real (non-FEX-trace) lines = crash point
console.log(l.filter(x => x && !/^BACHATA_FEX/.test(x)).slice(-20).map(clean).join('\n'));
```

Patterns to grep for, by failure class:
- **exit 127 / Missing Shared Library:** `error while loading shared libraries` or `cannot open shared object file` (e.g. `libcap.so.2`). Check `runtime/scripts/stage-debian-runtime.mjs`.
- **exit 133 / panic:** `Critical|Unhandled access|ReportGuestHleFailure|UNREACHABLE|DL_PANIC|Invalid mutex|EINVAL`
- **HLE gap (the Fios2 class of bug):** `FEX HLE call <nid>#<lib>.*failed: 38` and
  `unresolved HLE <name> uses temporary ENOSYS fallback`. The failing NID → look up in
  `aerolib.inl`; if it's only a `STUB(...)` with no `LIB_FUNCTION` in the lib's
  `RegisterLib`, that's the gap.
- **Host/guest callback boundary:** callback faults, host pointers in guest code, or
  FEX callback failures around `AvPlayer`. Classify each callback with
  `IsGuestCallback`. Bridge guest event/memory callbacks; keep host-owned FFmpeg
  buffers and native file I/O on the host side.
- **Mutex lifecycle:** `PthreadMutex.cpp|Invalid mutex|EINVAL|pthread_mutex`.
  Trace the exact guest mutex slot through init, destroy, and the failing operation.
- **GPU deadlock / black screen:** `WAIT_REG_MEM stalled` (gives addr/value/ref/mask/function),
  `GPU coroutine active resumes=<huge>` spinning on one `opcode=0xNN submits=1`,
  `EOP fence write`, low `Compiling graphics pipeline` count.
- **Vulkan capability gap:** `Extension VK_<name> unavailable` (cross-check with the
  desktop run — some are benign, some gate features).

### Trace resource identity before forming a fix

When the terminal error names a guest address or handle, build its full ordered
lifecycle across the log. For mutex `EINVAL`, temporarily log only:

```text
operation, guest slot, slot value, host handle, name/type, owner, thread, result
```

Log successful init/destroy plus every `EINVAL` return site. Then group events by
guest slot:

- Destroy succeeds, then a later call first sees the destroyed sentinel: semantic
  guest lifetime race. It is not proof of host use-after-free.
- A call already holds the host pointer while destroy deletes it: host object
  lifetime race.
- Waiter/in-flight tracking protects only calls already entered. It cannot protect
  a future call that begins after the slot becomes destroyed.

Treat nearby stubs, warnings, and module unloads as correlated hypotheses until a
mechanism connects them to the same resource. Treat high-volume `QueryProtection`
messages for one host heap pointer as noise unless they align with the terminal fault.

## Step 3 — Choose the narrowest evidence-backed fix

Fix the violated boundary, not the final assertion:

- Preserve host/guest pointer ownership at callback boundaries.
- For a proven destroy→future-lock scheduling race, prefer an operation-specific
  compatibility path through existing lazy initialization. Keep unrelated
  operations strict when possible (for example, destroyed unlock and repeated
  destroy still return `EINVAL`).
- Separate semantic compatibility from host object lifetime safety; one does not
  automatically solve the other.

Reject title-ID/address checks, sleeps/yields used as timing fixes, blanket success
returns, manual fence writes, and process-lifetime leaks. If upstream/FreeBSD is
strict but hardware timing masks undefined guest behavior, document the deviation
as emulator compatibility and cover its exact scope with a regression test.

## Step 4 — Reproduce / compare on native x86_64 desktop build

The repo ships a native x86_64 build (no FEX, direct execution). Run it headless
against the same game to see if a failure is Android/FEX/Turnip-specific or
reproduces on the reference path:

```bash
# native build (has ENABLE_BACHATA_RUNTIME=ON, so stall/EOP diagnostics are compiled in)
runtime/build/shadps4-x86_64/shadps4 -g "<path-to-game>/eboot.bin"
```

Game files on this host live under `$(wslpath "$USERPROFILE")/Downloads/PS4 Games/<game>/`.
The user's real Windows desktop GPU is the gold reference; WSL2g's D3D12-translated
Vulkan is a *second* data point (it can reproduce GPU-path issues but isn't proof of
"works on desktop" — ask the user for the Windows `shad_log.txt` from
`C:\Users\<u>\AppData\Roaming\shadPS4\log\shad_log.txt` when you need the true oracle).

**Important:** a failure reproducing on native WSL2g does NOT mean it's not a real
bug — it just means it's not FEX/Turnip-specific. The desktop-oracle comparison is
what tells you whether the Android path diverged.

## Step 5 — Classify CPU-side vs GPU-side writers (gdb watchpoint protocol)

When a fence/label at a guest address `A` is never written (classic GPU deadlock:
`WAIT_REG_MEM` on `A` waits forever for nonzero), you must determine whether the
writer is **CPU-side guest code** or a **Vulkan shader** before fixing anything.
Do not guess from extension lists or speculation.

gdb is not installed by default and needs no sudo — extract it to a user prefix:

```bash
cd "$HOME/repo/Bachata-S4"
apt-get download gdb libbabeltrace1 libipt2 libdebuginfod1t64 \
  libsource-highlight4t64 libxxhash0 libmpfr6 libpython3.14 libreadline8t64
mkdir -p "$HOME/gdb-user"
for d in *.deb; do dpkg-deb -x "$d" "$HOME/gdb-user"; done
GDB="$HOME/gdb-user/usr/bin/gdb"
GDBLIB="LD_LIBRARY_PATH=$HOME/gdb-user/usr/lib/x86_64-linux-gnu:$HOME/gdb-user/usr/lib"
```

Run the native build under gdb, break after guest memory is mapped (set a breakpoint
on `Core::MemoryManager::Map` or just let it run a few seconds then interrupt), and
install a hardware write watchpoint on the target address:

```
(gdb) watch *(uint32_t*)0x2b0200028        # repeat as *(uint64_t*) if width uncertain
(gdb) rwatch *(uint32_t*)0x2b0200028       # if write watch never fires, also check reads
(gdb) continue
```

When it triggers, capture all of: guest RIP, `disas $pc-16,$pc+16`, thread id +
name (`info threads`), write width, old/new value, registers used for address calc,
`backtrace`, and whether the instruction is LOCK/atomic (`x/i` shows `lock` prefix).

Then follow the branch the evidence dictates — do **not** shortcut by special-casing
the address or substituting a manual host write:

- **Hardware watchpoint triggers (CPU writer):** the guest instruction is the writer.
  Find the equivalent translated ARM64 block under FEX, instrument it to log
  guest RIP, host address, value before/after, width, atomic semantics, and the FEX
  memory-model/TSO config. Reproduce the exact instruction against the same
  SYSTEM_MANAGED mapping. Test with strict FEX memory-order emulation first; if that
  fixes the fence, narrow the fix to the specific relaxed translation. Never replace
  the store with a manual write.
- **Value changes but watchpoint never fires (GPU writer):** trace the Vulkan
  resource backing the guest address — VkBuffer, VkDeviceMemory, memory type,
  HOST_VISIBLE/COHERENT, imported-host-pointer status, CPU mapped ptr, device
  address, descriptor binding, shader stage/dispatch. After the candidate dispatch,
  insert correct shader-write→host-read sync, wait, `vkInvalidateMappedMemoryRanges`
  if non-coherent, then read both the Vulkan allocation and the guest pointer and
  compare. Vulkan-buffer≠0 & guest==0 → aliasing/shadow-download/sync bug. Both==0 →
  dump the shader SPIR-V + descriptors and verify the atomic/store. Run once with
  `TU_DEBUG=flushall` as a cache/barrier diagnostic (not a production fix).
- **Watchpoint can't be installed:** set a desktop-only page-write trap (mprotect
  PROT_READ on the page, catch SIGSEGV) around the page, or add a targeted
  CPU memory-write tracer in the native guest path. Classify on desktop before
  touching FEX.

Success = WAIT_REG_MEM exits naturally, first real draw completes with non-empty
output, all diagnostic traps/dumps/hardcoded addresses removed.

## Step 6 — Rebuild + redeploy after a fix

Per `AGENTS.md`, the Gradle build packages existing runtime assets but does NOT
generate them. Before `assembleDebug`, always rebuild the runtime from repo root:

First select the build environment:

```bash
if command -v aarch64-linux-gnu-gcc >/dev/null; then
  echo native-builder
elif podman container exists bachata-debian-builder 2>/dev/null; then
  echo podman-builder
else
  echo install-or-create-builder
fi
```

On PikaOS, prefer the documented Debian Podman builder. Do not install cross
packages on the host merely because `build-runtime-debian.sh` reports a missing
compiler.

### Option A: Native Host Build

```bash
git submodule update --init --recursive --jobs 8
runtime/scripts/build-runtime-debian.sh
node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json
```

### Option B: Podman Container Build (refer to `documents/podman-setup.md`)

```bash
podman exec --workdir /workspace bachata-debian-builder bash -c "
  git submodule update --init --recursive --jobs 8
  bash runtime/scripts/build-vortek-client.sh
  runtime/scripts/build-runtime-debian.sh
  node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json
  node runtime/tests/verify-no-bundled-turnip.mjs runtime/build/rootfs
"

podman exec --workdir /workspace/android/BachataS4 bachata-debian-builder bash -c "
  export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
  export ANDROID_HOME=/opt/android-sdk
  ./gradlew test lintDebug assembleDebug
"
```

`build-runtime-debian.sh` chains: `build-shadps4-x86_64.sh` → `build-box64-host.sh`
→ `build-shadps4-arm64.sh` (the FEX path; rebuilds `shadps4-arm64-fex`) → stage →
package into `runtime.zip`. The verify step prints the sha256 — it must match the
`runtime.zip sha256=...` line the build emitted, else the APK will package stale assets.

Then build + verify + install the APK:

```bash
cd android/BachataS4
./gradlew test lintDebug assembleDebug
# APK variants: app-fdroid-debug.apk and app-playstore-debug.apk (under app/build/outputs/apk/<variant>/debug/)

# CRITICAL: confirm BOTH managed-runtime assets are present before installing:
unzip -l app/build/outputs/apk/fdroid/debug/app-fdroid-debug.apk \
  | grep -E 'assets/runtime/(manifest\.json|runtime\.zip)'

# For Play Store variant, verify bundled Turnip driver packages:
node runtime/tests/verify-playstore-bundled-turnip.mjs app/build/outputs/apk/playstore/debug/app-playstore-debug.apk
```

Install + launch via DirectLaunchActivity (debug-only activity, takes `--es game_id <CUSAid>`):

```bash
ADB="adb"  # or full adb.exe path on WSL2
SERIAL=<serial-id>   # adjust per device
"$ADB" -s $SERIAL install -r -d app/build/outputs/apk/playstore/debug/app-playstore-debug.apk
# If install fails with INSTALL_FAILED_UPDATE_INCOMPATIBLE (signature mismatch):
# "$ADB" -s $SERIAL uninstall com.bachatas4.android && "$ADB" -s $SERIAL install app/build/outputs/apk/playstore/debug/app-playstore-debug.apk
"$ADB" -s $SERIAL logcat -c
"$ADB" -s $SERIAL shell am start -n com.bachatas4.android/.DirectLaunchActivity --es game_id CUSA01623
```

Then re-pull the session log (Step 1) and confirm the failure class is gone before
claiming the fix works.

## Step 7 — Verify the exact former failure boundary

A build, title screen, or fixed-duration capture is not gameplay proof.

1. Reproduce the same state transition: first-run setup, brightness selection,
   movie teardown, save load, or gameplay entry. `adb install -r` preserves app
   data and may skip the path; never clear user data without permission.
2. For interactive or long cutscenes, start the session, let the user or test driver
   cross the boundary, then pull the still-current log again. A 60-second capture
   taken before interaction is stale evidence.
3. Require all three gates:
   - screenshot/video shows a complete frame after the former boundary;
   - runtime process remains alive;
   - refreshed log lacks the old panic signature/exit code and contains the expected
     later milestone.
4. Remove all temporary address-specific logging and rebuild/retest the clean source
   before commit.

## Conventions / gotchas

- **Select adb; don't assume it.** On WSL, compare Linux adb and `adb.exe`. On native
  Linux/PikaOS, host adb normally works directly.
- **`INSTALL_FAILED_UPDATE_INCOMPATIBLE` error:** Occurs when switching between signature keys (e.g. release vs debug or fdroid vs playstore). Run `adb uninstall com.bachatas4.android` first.
- **`grep` shell alias breaks `-E -i` together** (`conflicting matchers specified`).
  Use `/usr/bin/grep` explicitly or one flag at a time.
- **Exit code 127 = missing host library in container runtime.** If `shadps4-arm64-fex` fails with `cannot open shared object file` (e.g. `libcap.so.2`), add the `.so` to `arm64Explicit` in `stage-debian-runtime.mjs` and re-stage.
- **Exit code 133 is not the root cause.** The real bug is whatever produced the
  guest panic/fault before `SignalHandler` or `UNREACHABLE`.
- **Repeated errors are not automatically causal.** Aggregate by address and thread,
  then correlate the terminal failure with the exact resource lifecycle.
- **Adjacent unload/stub calls are not proof.** Require a same-resource mechanism or
  an intervention that changes the outcome.
- **`ENABLE_BACHATA_RUNTIME` is ON in both the arm64-FEX and x86_64 native builds**,
  so the `WAIT_REG_MEM stalled` / `EOP fence write` / `GPU coroutine active`
  diagnostics are present in the native build too — use them as the reference.
- **Stale APK is the #1 false-negative.** If a "fix" doesn't change behavior, check
  `ls -la --time=mtime` on the APK vs your last source edit; the runtime rebuild must
  finish (sha256 line printed) before `assembleDebug`.
- **Don't Read multi-million-line `shadps4.log` files.** Always filter in-sandbox
  (`ctx_execute_file`) or via grep — raw reads burn the whole context budget on spin spam.
