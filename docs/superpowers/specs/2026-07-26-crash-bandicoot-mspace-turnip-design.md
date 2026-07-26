# Crash Bandicoot Mspace and Turnip Compatibility Design

**Date:** 2026-07-26
**Status:** Design approved; specification review pending
**Title:** Crash Bandicoot N. Sane Trilogy (`CUSA07399`)

## Problem

Crash Bandicoot starts through the Android managed runtime on a OnePlus OPD2403
with the FEX guest backend and bundled Turnip driver, renders frames for several
seconds, and then stops with exit code 133.

Exit code 133 is `128 + SIGTRAP`. The fresh device session proves the trap is
caused by a CPU-side HLE compatibility gap:

1. The game maps a `0x19c8000`-byte flexible-memory region.
2. `sceLibcMspaceCreate` (`-hn1tcVHq5Q`) reaches FEX's unsupported-HLE adapter
   and returns `ENOSYS`.
3. `sceLibcMspaceMalloc` (`OJjm-QOIHlI`) also returns `ENOSYS`.
4. The game treats the result as a pointer and writes to
   `0xffffffffffffffe2`.
5. `SignalHandler` raises the fatal `Unreachable code!` diagnostic and Android
   reports `Stopped: 133`.

The same native x86_64 build continues for at least 30 seconds because its
legacy unresolved stub returns null instead of `ENOSYS`. That behavior avoids
the immediate crash but does not provide the required allocator.

The device and native runs also emit two `Color1D`-view/`Color2D`-image
compatibility warnings. They are not the cause of exit 133 and reproduce away
from Turnip, so graphical validation is a separate, evidence-gated stage.

## Goals

- Implement the PS4 libc mspace operations needed by the title instead of
  special-casing the game or weakening the global FEX unsupported-HLE policy.
- Return allocations from the guest-supplied memory region so both native and
  FEX guests receive valid guest virtual addresses.
- Support allocator reuse and normal lifecycle operations, not only the first
  allocation that happens before the observed crash.
- Make invalid handles, integer overflow, exhaustion, double frees, and
  out-of-arena pointers fail safely.
- Add deterministic unit tests before wiring the HLE symbols.
- Verify that Crash Bandicoot remains running on the connected Turnip device,
  reaches the start screen, and no longer records the mspace `ENOSYS` calls or
  exit 133.
- Inspect actual device output for graphical corruption and fix only a
  demonstrated renderer defect.

## Non-goals

- Do not change the return value of the generic unsupported-HLE fallback.
- Do not return host `malloc` memory as a guest allocation.
- Do not hardcode `CUSA07399`, the observed guest addresses, or allocation
  sizes.
- Do not recreate Sony's private allocator metadata layout unless evidence
  shows that the guest inspects it.
- Do not reapply the previously reverted global “Nx1 render target becomes 1D”
  workaround.
- Do not claim broad game compatibility from one title's launch test.

## Selected Architecture

### Guest-backed arena with opaque host metadata

Add a small mspace component under
`src/core/libraries/libc_internal/`. Each arena owns only metadata; its payload
range is the base address and capacity supplied by the guest.

The arena will maintain:

- guest base address and exclusive end address;
- a free-list ordered by offset;
- an allocation table keyed by guest address;
- allocation sizes and alignments;
- a mutex protecting allocator state;
- a bounded diagnostic name copied during creation.

Allocator metadata stays on the host. Allocated pointers always fall within the
declared guest range. This avoids consuming or exposing implementation-specific
headers inside guest memory while preserving guest pointer validity.

### Handle registry and lifetime

An mspace handle is an opaque pointer-sized token backed by a host arena object.
A registry owns arenas with shared lifetime:

- creation inserts a new arena and returns its token;
- every operation resolves the token through the registry before use;
- unknown or destroyed tokens are rejected without dereferencing them;
- an operation holds a shared arena reference while it runs;
- destroy removes the token, while already-running operations finish safely.

This matches the existing FEX HLE adapter's opaque-handle model. The guest must
not dereference an mspace handle; it may only pass it back to libc mspace
functions.

### Allocation algorithm

Use aligned first-fit allocation:

1. Treat zero-byte malloc/calloc requests as one minimum-aligned allocation;
   `realloc(pointer, 0)` frees the allocation and returns null.
2. Reject size/alignment arithmetic overflow.
3. Find the first free interval that can contain the aligned allocation.
4. Split leading and trailing fragments.
5. Record the allocation and return `base + offset`.

Free validates ownership, returns the exact recorded interval to the free-list,
and coalesces adjacent intervals. Realloc grows in place when the immediately
following free interval is large enough; otherwise it allocates a new block,
copies the smaller of the old and new sizes, and frees the old block. Calloc
checks multiplication overflow and zeroes the returned guest range. Memalign
accepts only power-of-two alignment and uses the same interval splitter.

The implementation never expands beyond the guest-supplied capacity.
Exhaustion returns null.

### HLE ABI surface

Register these existing NIDs for `libSceLibcInternal`:

| NID | Function |
| --- | --- |
| `-hn1tcVHq5Q` | `sceLibcMspaceCreate` |
| `W6SiVSiCDtI` | `sceLibcMspaceDestroy` |
| `OJjm-QOIHlI` | `sceLibcMspaceMalloc` |
| `Vla-Z+eXlxo` | `sceLibcMspaceFree` |
| `LYo3GhIlB38` | `sceLibcMspaceCalloc` |
| `gigoVHZvVPE` | `sceLibcMspaceRealloc` |
| `iF1iQHzxBJU` | `sceLibcMspaceMemalign` |
| `fEoW6BJsPt4` | `sceLibcMspaceMallocUsableSize` |

The wrapper signatures will follow the known PS4 ABI:

- create: `(name, base, capacity, flags) -> handle`;
- destroy: `(handle) -> status`;
- malloc: `(handle, size) -> guest pointer`;
- free: `(handle, guest pointer) -> status`;
- calloc: `(handle, element_count, element_size) -> guest pointer`;
- realloc: `(handle, guest pointer, size) -> guest pointer`;
- memalign: `(handle, alignment, size) -> guest pointer`;
- usable size: `(guest pointer) -> size`.

Allocation failures return null. Status-returning operations return `0` on
success and `1` for an invalid handle, pointer, or argument, matching the
observable mspace convention used by the reference implementation.

### Input and range safety

- Reject null base, unusable capacity, and `base + capacity` overflow.
- Reject unknown handles before accessing allocator state.
- Reject pointers outside the arena or not present in its allocation table.
- Reject non-power-of-two alignment and alignment smaller than the ABI minimum
  after normalization.
- Check `count * size`, alignment rounding, pointer addition, and interval
  splitting for overflow.
- Bound the copied mspace name; do not trust it to be indefinitely terminated.
- Keep returned allocations within the supplied arena even under exhaustion.

## Test Design

Create a focused `shadps4_libc_mspace_test` target using a host byte buffer as a
stand-in for mapped guest memory. Tests must cover:

1. A regression test proving create and malloc return a usable in-range pointer
   instead of an error-valued pointer.
2. Multiple aligned allocations do not overlap.
3. Exhaustion returns null without corrupting existing allocations.
4. Free coalesces adjacent blocks and permits reuse.
5. Invalid handles, foreign pointers, and double frees fail safely.
6. Calloc zeroes memory and rejects multiplication overflow.
7. Realloc preserves data, grows in place when possible, and moves safely when
   necessary.
8. Memalign honors valid power-of-two alignments and rejects invalid values.
9. Usable-size reports only live allocations.
10. Destroy invalidates the handle without invalidating guest payload memory.

Tests will first be run in the red state before implementation, then after each
minimal behavior is added.

## Renderer Validation Stage

After the allocator fix is deployed:

1. Pull the new session log and confirm the exact driver and guest backend.
2. Capture an Android screenshot after the start screen appears.
3. Compare it with the native reference path and inspect for visible corruption.
4. Correlate any corruption with renderer errors or Vulkan validation output.
5. If the `Color1D`/`Color2D` mismatch is implicated, add narrowly scoped
   diagnostics for guest address, dimensions, tile mode, image type, view type,
   and shader resource shape.
6. Change image/view/shader dimensionality only after those values identify the
   correct compatibility rule, with a focused unit test for that rule.

If the mismatch is not visibly harmful, retain it as a separate upstream
renderer issue rather than modifying image semantics speculatively.

## Files

Expected allocator work:

- Create `src/core/libraries/libc_internal/mspace.h`
- Create `src/core/libraries/libc_internal/mspace.cpp`
- Modify `src/core/libraries/libc_internal/libc_internal_memory.cpp`
- Modify `CMakeLists.txt`
- Create `tests/core/test_libc_mspace.cpp`
- Modify `tests/CMakeLists.txt`

Renderer files will be selected only after the post-fix evidence gate.

## Verification

The completion gate is:

1. The new focused allocator test target passes.
2. Existing native tests and compilation pass.
3. The managed runtime is rebuilt and
   `runtime/tests/verify-runtime.mjs` succeeds.
4. Android `test`, `lintDebug`, and `assembleDebug` succeed.
5. The selected APK contains both
   `assets/runtime/manifest.json` and `assets/runtime/runtime.zip`.
6. The APK is installed on the connected device and launched as `CUSA07399`.
7. The new application log confirms FEX plus the selected Turnip driver.
8. The new shadPS4 log contains no mspace fallback/`ENOSYS`, fatal write to
   `0xffffffffffffffe2`, or SIGTRAP.
9. The title reaches the start screen and remains running during an observation
   interval.
10. A captured screenshot is checked for visible graphical corruption.

## Self-review

- The fix targets the failing abstraction, not exit-code handling or the
  observed address.
- Native and FEX paths use the same registered implementation.
- Guest payload pointers never come from host heap allocation.
- Handle forgery and stale handles are rejected through lookup.
- Arithmetic and allocator-boundary failure cases have explicit tests.
- Renderer work is separated from the proven CPU crash and requires visual
  evidence.
- The prior reverted global 1D workaround is explicitly excluded.
- Runtime packaging and on-device evidence are required before any completion
  claim.
