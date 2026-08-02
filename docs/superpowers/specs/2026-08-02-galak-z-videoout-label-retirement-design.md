# Galak-Z VideoOut Label Retirement Design

**Date:** 2026-08-02
**Status:** Approved
**Scope:** Generic PS4 VideoOut flip-label lifecycle on all platforms

## Problem

Galak-Z (`CUSA03146`) reaches its animated startup sequence after the existing
FEX guest-signal fix, but then reports 0 FPS. The long startup delay is not an
active shader-compilation phase. The graphics queue is blocked in
`WAIT_REG_MEM`, polling a VideoOut buffer label whose value remains `1`.

shadPS4 currently clears only `prev_index` after a later presentation. That
matches the hardware-tested behavior introduced by upstream PR #2663, but it
does not model the second ownership transition: the current scanout buffer
eventually becomes safe for rendering at a later vblank even when no replacement
flip has executed yet.

Galak-Z waits for that transition before it renders and submits its next flip.
Because the emulator waits for another flip before clearing the label, neither
side can advance.

## Evidence

Fresh Android device runs on OnePlus device `7d6afed8` established all of the
following:

1. FEX delivers guest signal 30 to `GC Finalizer`; the handler returns.
2. Presenter calls complete for the startup frames.
3. `shadPS4:GpuComm` remains active in PM4 opcode `0x3c` (`WAIT_REG_MEM`).
4. The wait address is exactly `VideoOutPort::buffer_labels[index]`.
5. The label value is `1`, reference is `0`, mask is `0xffffffff`, and function
   is equality.
6. `shadPS4:Present` sleeps with no queued request; no later flip can release
   the label.

Read-only process-memory extraction then proved the command-buffer ordering.
Addresses below are from session
`20260802-151504-CUSA03146-af159b16`; native object addresses vary per run.

### Buffer 0 DCB

At `0x300a0041c`:

```text
c0053c00 00000013 6fa44af0 00000055 00000000 ffffffff 0000000a
```

This is `WAIT_REG_MEM` on label 0, waiting for zero.

Its associated patched tail contains:

```text
c0033700 00000500 6fa44af0 00000055 00000001
c0391000 68750776 000000a5
```

This writes `1` to label 0 and then executes `PatchedFlip`.

### Buffer 1 DCB

At `0x301a0df74`:

```text
c0053c00 00000013 6fa44af8 00000055 00000000 ffffffff 0000000a
```

Its associated tail writes `1` to label 1 and executes `PatchedFlip`:

```text
c0033700 00000500 6fa44af8 00000055 00000001
c0391000 68750776 000000a6
```

Each DCB therefore waits for the same surface that it will render and flip.
The wait is correct guest behavior: it is `waitUntilSafeForRendering`.

Pulled evidence is stored under:

```text
android/BachataS4/session-logs-galakz-pm4-proof/
  20260802-151504-CUSA03146-af159b16/
```

## Goals

- Model both VideoOut label ownership transitions:
  - release the previous buffer when a replacement flip becomes active;
  - release the current buffer after its scanout-completion vblank.
- Prevent a delayed retirement from clearing a buffer relocked by newer GPU
  work.
- Unblock Galak-Z without title IDs, guest-address checks, timeouts, forced PM4
  completion, or unconditional label writes.
- Keep the hot path bounded: one small state update per flip-label lock and one
  retirement check per vblank.
- Make the lifecycle independently testable without Vulkan or an Android
  device.

## Non-goals

- Reverting upstream previous-buffer semantics.
- Treating every `WAIT_REG_MEM` as a VideoOut wait.
- Clearing labels merely because a wait has lasted too long.
- Changing flip rate, presentation mode, shader compilation, or frame pacing.
- Claiming 60 FPS from a unit test. Device gameplay remains the final gate.

## Selected Design

### 1. Pure flip-label state tracker

Add a small renderer-independent state tracker under
`src/core/libraries/videoout/`. It owns:

- a monotonically increasing generation for every display-buffer label;
- at most one pending scanout retirement, containing buffer index, captured
  generation, and due vblank count.

The tracker exposes behavior-level operations:

- record an observed GPU lock and return its generation;
- read the current generation for a buffer;
- schedule retirement of the presented buffer for the next vblank;
- cancel retirement when the buffer is superseded or reset;
- return a buffer index only when retirement is due and its generation still
  matches;
- reset one buffer or all state during registration/close.

The tracker does not own guest label memory, condition variables, Vulkan frames,
or threads. This keeps its tests deterministic.

### 2. Observe the actual PM4 lock boundary

The generation must change when the GPU executes the patched
`WRITE_DATA label = 1`, not when the CPU registers an EOP callback.

Incrementing at registration is too early: Galak-Z registers its next flip
before the command reaches `WAIT_REG_MEM`. That would make the current
retirement look stale and recreate the deadlock.

Incrementing only in the EOP callback is too late: the Present thread could
retire the old generation after `WRITE_DATA` has relocked the label but before
`PatchedFlip` raises its IRQ.

Therefore Liverpool's existing `WriteData` handler will identify exact
VideoOut-label writes. Immediately before committing a value of `1` to a label,
it records the new generation through `VideoOutPort`. This is the only boundary
where state and guest-visible ownership change atomically in emulator order.

Generic `WriteData` behavior remains unchanged for non-VideoOut addresses and
for values other than the flip lock value.

### 3. Carry generation with the flip request

When `PatchedFlip` raises the EOP IRQ, the callback reads the current generation
for its buffer. `SubmitFlip` and the Present-thread request carry this generation
alongside index, argument, and EOP state.

CPU-submitted non-EOP flips use an invalid generation and do not schedule
scanout-label retirement unless they have an observed VideoOut lock.

### 4. Preserve immediate previous-buffer retirement

After presentation, `Flip` retains the current upstream behavior:

- if `prev_index` is valid, clear its guest label;
- wake the VideoOut label waiter;
- cancel any pending delayed retirement for that exact prior ownership;
- set `prev_index` to the new index.

This preserves the hardware-tested change from upstream PR #2663 and avoids
holding a superseded buffer for an extra vblank.

### 5. Schedule current-buffer retirement

After an EOP frame is presented, `Flip` schedules its index and captured
generation for `current_vblank + 1`.

At the start of the next Present-thread vblank iteration, before dequeuing a new
flip, the driver asks the tracker whether retirement is due:

- matching generation: clear that buffer label and wake the waiter;
- changed generation: discard stale retirement without touching guest memory;
- no pending/due retirement: do nothing.

Running this before request dequeue lets a blocked GPU command resume, render,
and queue its flip for the following vblank. It also means a replacement already
queued for the boundary can use the same established vblank timing.

Blank frames, close, unregister, and buffer re-registration clear or cancel
related tracker state so ownership cannot leak across port lifetimes.

## Synchronization

`VideoOutPort::port_mutex` protects generation and pending-retirement state.
The PM4 `WriteData` path and Present thread both use this mutex only for short
state transitions.

Guest label writes occur while the chosen generation is known. The mutex is
released before `SignalVoLabel()` takes `vo_mutex` and notifies the waiter. This
keeps a single lock order and avoids a `port_mutex`/`vo_mutex` cycle.

Only exact label-base addresses are tracked. The contiguous label range remains
available to `IsVoLabel` for existing wait optimization.

## Failure Handling and Invariants

- Buffer indexes are validated against `MaxDisplayBuffers`.
- Generation zero means no observed EOP lock.
- A generation never decrements during a port lifetime.
- Retirement consumes its pending record whether it succeeds or is stale.
- A stale retirement never writes guest memory.
- Clearing an already-zero label is harmless but still bounded.
- Close and register reset affected generations and pending ownership.
- No wait duration, game ID, guest PC, or absolute address influences behavior.

## Test Strategy

### Unit tests

Add a dedicated lightweight GTest target for the pure state tracker. TDD starts
with failing tests for:

1. a presented generation remains locked during its presentation vblank and
   retires at the next vblank;
2. a second GPU lock increments generation and prevents stale retirement from
   clearing it;
3. superseding a buffer cancels its delayed retirement;
4. buffer reset removes generation and pending ownership;
5. alternating buffer generations retire independently without cross-clearing;
6. non-EOP/invalid generations never schedule retirement.

### Integration checks

- Build and run the new GTest target.
- Run existing CMake tests affected by shared headers.
- Run existing runtime source tests, including the Galak-Z FEX signal test.
- Build the ARM64 FEX runtime and verify its locked component manifest.
- Run Android Gradle unit tests and lint.

### Device qualification

Follow `documents/android-building.md` and repository `AGENTS.md` exactly:

```bash
git submodule update --init --recursive --jobs 8
runtime/scripts/build-runtime-debian.sh
node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json
cd android/BachataS4
./gradlew test lintDebug assembleDebug
```

Before installation, verify both runtime assets exist in the APK:

```bash
unzip -l app/build/outputs/apk/debug/app-debug.apk \
  | grep -E 'assets/runtime/(manifest\.json|runtime\.zip)'
```

Then install, launch `CUSA03146`, and capture session logs plus screenshots.
Qualification requires:

- no sustained VideoOut-label `WAIT_REG_MEM` stall;
- startup advances beyond the memory-test/FMV sequence;
- menu and interactive gameplay are reachable;
- no new critical/assert/exit condition;
- no visible corruption, tearing, or graphical glitches;
- overlay reports sustained 60+ FPS during representative gameplay, not only a
  loading screen or menu;
- repeated launch proves the result is not a one-run cache artifact.

If the label fix reveals another independent blocker, diagnosis resumes from
fresh evidence; the goal is not considered complete at first boot or first
rendered frame.

## Alternatives Rejected

### Clear the current label immediately after `Present`

Fast but wrong timing. It reverses hardware-tested previous-buffer semantics and
can permit rendering while the buffer still belongs to scanout.

### Clear a label after a timeout in `WAIT_REG_MEM`

Masks synchronization failures, makes behavior host-speed dependent, and can
corrupt frames. It also applies policy at the symptom rather than ownership
source.

### Ignore VideoOut waits

Breaks guest ordering and allows rendering into an unsafe display surface.

### Galak-Z compatibility check

Title IDs, known guest addresses, and command-buffer fingerprints would fix one
binary while preserving the emulator defect for every other game.

## Risks and Mitigations

- **Risk:** delayed callback clears a newly relocked label.
  **Mitigation:** generation increments at actual PM4 lock execution; retirement
  compares the captured generation.
- **Risk:** regression to games relying on previous-buffer clearing.
  **Mitigation:** preserve immediate previous retirement and add an explicit
  regression test.
- **Risk:** new contention in the GPU hot path.
  **Mitigation:** lock only on exact VideoOut label writes, once per submitted
  EOP frame.
- **Risk:** apparent boot success hides low performance or corruption.
  **Mitigation:** retain full device qualification criteria, including sustained
  gameplay FPS and visual inspection.

## Success Criterion

This design succeeds only when its generic lifecycle tests pass and Galak-Z is
demonstrably playable on the target Android device at sustained 60+ FPS without
graphical glitches. Removing the initial 0-FPS deadlock alone is necessary but
not sufficient.
