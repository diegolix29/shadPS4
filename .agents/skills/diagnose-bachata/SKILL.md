---
name: diagnose-bachata
description: Diagnose and fix crashes, black screens, and GPU deadlocks in the Bachata-S4 / shadPS4 Android runtime. Use whenever the user reports a game failing to launch, crashing with an exit code (e.g. "Stopped: 133", "exitCode=133"), freezing, or showing a black/blank screen on the Android build — or wants to compare Android vs desktop behavior. Jump-starts the investigation by pointing at the exact logs, code locations, build commands, and adb/device quirks so the agent doesn't scan the whole codebase to get oriented.
---

# Diagnose Bachata-S4

Structured investigation workflow for shadPS4/Bachata-S4 Android runtime failures
(crash-on-launch, exit codes, black screen, GPU hang). Encodes the paths, commands,
and code locations learned from real fixes so each new investigation starts from
the known map, not a blind scan.

## When to use

Trigger when the user reports any of:
- Game "Stopped: NNN" or `exitCode=NNN` on Android
- Crash-to-launcher, app dies within seconds of "Running"
- Black / blank / frozen screen while FPS counter still moves
- A game that works on desktop shadPS4 but fails on the Android build
- "compare Android vs desktop" / "why does it work on PC but not phone"

## Orientation map (read first — don't re-scan)

Repo root: `$HOME/repo/Bachata-S4`. All paths below are relative to it unless noted.

### Where things live

| Concern | Location |
|---|---|
| HLE library implementations | `src/core/libraries/<lib>/` (e.g. `fios2/fios2.cpp`, `playgo/playgo.cpp`) |
| NID → symbol name table | `src/core/aerolib/aerolib.inl` (`STUB("nid", name)` = name known, NO impl) |
| HLE registration in a lib | the lib's `RegisterLib()` — `LIB_FUNCTION("nid", "lib", ver, "mod", fn)` |
| FEX unresolved-HLE fallback | `src/core/linker.cpp:~802` ("temporary ENOSYS fallback") → returns `ENOSYS=38` |
| UnsupportedHleCallAdapter | `src/core/guest_cpu/hle_call_adapter.h` (returns `HleCallFailure{ENOSYS}`) |
| GPU command processor (PM4) | `src/video_core/amdgpu/liverpool.cpp` |
| PM4 opcodes | `src/video_core/amdgpu/pm4_opcodes.h` (`WaitRegMem=0x3c`, `EventWriteEop=0x47`, ...) |
| PM4 packet structs + Test() | `src/video_core/amdgpu/pm4_cmds.h` |
| Guest memory layout | `src/core/address_space.cpp` (SYSTEM_MANAGED `0x400000-0x7FFFFBFFF`, etc.) |
| Memory backing write | `src/core/memory.cpp` `TryWriteBacking()` (~line 155) |
| Vulkan instance / extensions | `src/video_core/renderer_vulkan/vk_instance.cpp` |
| Android runtime launch | `android/BachataS4/core/runtime/.../process/RuntimeProcessLauncher.kt` |
| Session state / exit code | `android/BachataS4/core/runtime/.../session/ManagedSession.kt` (`Stopped(exitCode)`) |

### Exit code cheat sheet

Android `Process.exitCode()`: if killed by signal N, returns `128 + N`.
- **133 = 128 + 5 = SIGTRAP** → most often a guest assert / `UNREACHABLE_MSG` in
  `src/core/signals.cpp:~134` ("Breakpoints almost certainly come from our asserts").
- **134 = SIGABRT**, **139 = SIGSEGV**, **137 = SIGKILL (OOM)**.

For 133: the guest hit an `UNREACHABLE`/`ASSERT`. Pull the session log and find the
`<Critical> SignalHandler: Unreachable code!` line — the lines just above it show
what failed (often a NULL deref from an un-filled HLE handle, or an unimplemented
PM4 opcode).

## Step 1 — Pull the failing session log (do this before reading code)

Session logs live on-device under the app's private storage and are pulled with:

```bash
cd android/BachataS4
ADB="$(wslpath "$USERPROFILE")/AppData/Local/Android/Sdk/platform-tools/adb.exe"
ADB_OVERRIDE="$ADB" ./pull-session-logs.sh --game <CUSAxxxxx> --output session-logs
```

Each session dir is named `YYYYMMDD-HHMMSS-<CUSAid>-<hash>` and contains:
- `application.log` — app lifecycle, exit code, `guestBackend=fex|box64`, `driver=turnip-...`
- `shadps4.log` — backend stdout/stderr (the real crash evidence; can be millions of lines)
- `shadps4-internal.log` — copied shadPS4 internal log (if present)

**adb quirk — read this:** there are TWO adb servers. `/usr/bin/adb` (Linux) sees no
device; the device is reachable only through `adb.exe` (Windows). The pull script
auto-detects `adb.exe` if on PATH, but to be explicit pass `ADB_OVERRIDE=<path>`.
All manual `adb shell` calls must use the full `adb.exe` path too.

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
- **exit 133 / SIGTRAP:** `Critical|Unhandled access|ReportGuestHleFailure|UNREACHABLE`
- **HLE gap (the Fios2 class of bug):** `FEX HLE call <nid>#<lib>.*failed: 38` and
  `unresolved HLE <name> uses temporary ENOSYS fallback`. The failing NID → look up in
  `aerolib.inl`; if it's only a `STUB(...)` with no `LIB_FUNCTION` in the lib's
  `RegisterLib`, that's the gap.
- **GPU deadlock / black screen:** `WAIT_REG_MEM stalled` (gives addr/value/ref/mask/function),
  `GPU coroutine active resumes=<huge>` spinning on one `opcode=0xNN submits=1`,
  `EOP fence write`, low `Compiling graphics pipeline` count.
- **Vulkan capability gap:** `Extension VK_<name> unavailable` (cross-check with the
  desktop run — some are benign, some gate features).

## Step 3 — Reproduce / compare on native x86_64 desktop build

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

## Step 4 — Classify CPU-side vs GPU-side writers (gdb watchpoint protocol)

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

## Step 5 — Rebuild + redeploy after a fix

Per `AGENTS.md`, the Gradle build packages existing runtime assets but does NOT
generate them. Before `assembleDebug`, always rebuild the runtime from repo root:

```bash
git submodule update --init --recursive --jobs 8   # only if submodules changed
runtime/scripts/build-runtime-debian.sh
node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json
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
# Do NOT install if either is missing — a green Gradle build alone proves nothing.
```

Install + launch via DirectLaunchActivity (debug-only activity, takes `--es game_id <CUSAid>`):

```bash
ADB="$(wslpath "$USERPROFILE")/AppData/Local/Android/Sdk/platform-tools/adb.exe"
SERIAL=<serial-id>   # adjust per device
"$ADB" -s $SERIAL install -r -d app/build/outputs/apk/fdroid/debug/app-fdroid-debug.apk
"$ADB" -s $SERIAL logcat -c
"$ADB" -s $SERIAL shell am start -n com.bachatas4.android/.DirectLaunchActivity --es game_id CUSA01623
```

Then re-pull the session log (Step 1) and confirm the failure class is gone before
claiming the fix works.

## Conventions / gotchas

- **Two adb servers.** `/usr/bin/adb` is useless here; always use `adb.exe` (full path)
  or `ADB_OVERRIDE` for the pull script.
- **`grep` shell alias breaks `-E -i` together** (`conflicting matchers specified`).
  Use `/usr/bin/grep` explicitly or one flag at a time.
- **Exit code 133 ≠ crash bug necessarily** — it's a guest assert. The real bug is
  whatever made the guest reach the `UNREACHABLE`; the assert is the symptom.
- **`ENABLE_BACHATA_RUNTIME` is ON in both the arm64-FEX and x86_64 native builds**,
  so the `WAIT_REG_MEM stalled` / `EOP fence write` / `GPU coroutine active`
  diagnostics are present in the native build too — use them as the reference.
- **Stale APK is the #1 false-negative.** If a "fix" doesn't change behavior, check
  `ls -la --time=mtime` on the APK vs your last source edit; the runtime rebuild must
  finish (sha256 line printed) before `assembleDebug`.
- **Don't Read multi-million-line `shadps4.log` files.** Always filter in-sandbox
  (`ctx_execute_file`) or via grep — raw reads burn the whole context budget on spin spam.
