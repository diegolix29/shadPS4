# Galak-Z FEX Signal Delivery Design

Date: 2026-07-30

## Problem

Galak-Z (`CUSA03146`) hangs at launch on the Android ARM64 FEX runtime. The Unity
runtime sends Orbis signal 30 (mapped from SIGUSR1) to the GC Finalizer thread as
part of stop-the-world garbage collection. The GC Finalizer is parked inside a host
`futex` via `std::counting_semaphore<…>::acquire()` in
`Libraries::Kernel::Semaphore::acquire()` when the signal arrives.

Three failed approaches clarified why this is hard:

- **fix2 (no-divert):** Queues the signal without redirecting the host PC. The
  process stays alive but the guest handler never runs and GC STW stalls forever.
- **fix3–fix5 (async host-PC divert / setcontext):** The trampoline handler returns,
  then a SIGILL fires exactly at the saved `host_pc`. Diverting a blocked
  `libstdc++` futex proved unrecoverable: the nested signal re-enters an
  async-signal-unsafe code path and crashes.

The root cause is that the GC Finalizer is not executing JIT code when the signal
arrives. `HandleSyscall` (the safe point for running JIT threads) is never reached.
The pending signal therefore never delivers, and GC deadlocks waiting for the
thread to acknowledge stop-the-world.

## Goals

- Deliver Orbis signal 30 to the GC Finalizer thread's registered guest handler
  when that thread is blocked inside a host semaphore wait.
- Keep the existing HLE safe-point path (`HandleSyscall → FlushPendingOrbisSignal`)
  for threads that are running JIT code.
- Preserve the existing non-FEX (x86-64 host) signal path unchanged.
- Preserve the existing Bloodborne / CUSA00900 signal regression contract.
- Require no host-PC injection or `setcontext` calls.

## Non-goals

- Delivering signals to threads blocked in arbitrary non-HLE host waits.
- Changing the JIT or the FEX signal delegator.
- Supporting signal delivery on Windows or macOS hosts.
- Supporting signal delivery outside `SHADPS4_ENABLE_FEX_GUEST_CPU` builds.

## Architecture

### Signal queue

`DeliverGuestOrbisSignal` (called from `SigactionHandler` inside the OS signal
handler) writes into the thread-local `PendingOrbisSignalState`:

```
Handler   – guest VA of the registered Orbis exception handler
OrbisSig  – Orbis signal number (e.g. 30)
Pending   – true
Flushing  – false
HasHostSnapshot – false  (host-PC divert path removed)
```

The write is fire-and-forget. No `setcontext`, no `ucontext` modification, no
trampoline install.

### Safe-point delivery: JIT path

`BridgeSyscallHandler::HandleSyscall` calls `FlushPendingOrbisSignal` at entry and
after each `Bridge.Invoke` return. JIT threads reach this point naturally after
every HLE call.

### Safe-point delivery: blocked-thread path

`Core::Fex::FlushPendingGuestOrbisSignal` (free function, `fex_guest_engine.h`)
calls `ActiveFexExecution.Syscalls->FlushPendingOrbisSignal()` when
`ActiveFexExecution.Syscalls != nullptr`. It is a no-op when no FEX guest thread
is active on the calling host thread.

Linux `libstdc++` `std::counting_semaphore::acquire()` and
`try_acquire_for()` retry `EINTR` internally; callers never observe an EINTR
iteration and cannot use it as a flush trigger. Flush-after-success on a plain
`sem.acquire()` does not help a thread that is blocked indefinitely waiting
for the semaphore to be released.

Under `SHADPS4_ENABLE_FEX_GUEST_CPU`, `Libraries::Kernel::Semaphore::acquire()`
must therefore loop, repeatedly calling `sem.try_acquire_for(25ms)`. After each
failed 25 ms slice it calls `Core::Fex::FlushPendingGuestOrbisSignal()`, giving
the blocked thread a periodic safe-point delivery opportunity. It also flushes
once after a successful acquisition before returning, so any signal that arrived
during the final slice is delivered before control returns to HLE code.

`Semaphore::try_acquire_for` preserves the original caller deadline by
repeatedly waiting `min(remaining, 25ms)`, flushing after each failed slice,
and returning `true` on acquisition or `false` only when the original deadline
expires. `try_acquire_until` already funnels through `try_acquire_for` and
requires no independent change.

The header uses a guarded lightweight forward declaration of
`Core::Fex::FlushPendingGuestOrbisSignal` inside an
`#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU` block rather than including
`fex/fex_guest_engine.h` directly, avoiding a heavyweight dependency in a
widely-included kernel header.

### Flush execution

`BridgeSyscallHandler::FlushPendingOrbisSignal`:

1. Guards against recursion (`Flushing` flag).
2. Clears `PendingOrbisSignal` atomically before running the handler.
3. Determines a valid guest RSP: prefers `CurrentFrame->State.gregs[REG_RSP]`
   inside the PS4 VA window `[0x100000000, 0x900000000)`; falls back to
   `Libraries::Kernel::FexCurrentGuestStackTop() - 0x100` (Orbis pthread guest
   stack top from `exception.cpp`).
4. Aborts flush (restores pending state) if no guest stack is available.
5. Builds a `Libraries::Kernel::Ucontext` on the guest stack so the PS4Util
   soft-handler pattern (Galak-Z) can read `mc_rsp` at `uctx + 0xf8`.
6. Calls `ctx->HandleCallback(thread, handler)` — the same nested-callback path
   used by the AVPlayer and guest-entry bridges.
7. Restores the saved GPR frame so the interrupted HLE call or JIT execution can
   resume correctly.

The `HasHostSnapshot` / `HostPc` / `HostGprs` fields in `PendingOrbisSignalState`
exist from a prior iteration. `DeliverGuestOrbisSignal` always sets
`HasHostSnapshot = false`. The SRA-spill branch inside `FlushPendingOrbisSignal`
is therefore dead and must be removed.

## Data Flow

```
pthread_kill(target, SIGUSR1)
  → kernel delivers SIGUSR1 to target host thread
  → SigactionHandler (async-signal context)
      → DeliverGuestOrbisSignal(30, …, guest_handler)
          writes PendingOrbisSignal{Handler, Pending=true}
          returns true
      → SigactionHandler returns
  → Semaphore::acquire() [Linux FEX path]
      loop:
        sem.try_acquire_for(25ms) → false (timeout, semaphore still locked)
        FlushPendingGuestOrbisSignal()          ← safe-point flush
            → BridgeSyscallHandler::FlushPendingOrbisSignal()
                determines guest RSP
                builds Ucontext on guest stack
                ctx->HandleCallback(thread, handler)
                    FEX JIT executes guest handler
                    guest handler returns via CallbackRet thunk
                restores saved GPR frame
                PendingOrbisSignal.Flushing = false
        (GC releases semaphore)
        sem.try_acquire_for(25ms) → true
        FlushPendingGuestOrbisSignal()          ← post-success flush
        return
  → GC STW completes
```

## Error Handling

- If `ActiveFexExecution.Syscalls` is null (no FEX thread active),
  `FlushPendingGuestOrbisSignal` is a no-op. Non-FEX threads are unaffected.
- If no valid guest RSP is found, flush aborts and restores `PendingOrbisSignal`
  so the next opportunity can retry.
- If `HandleCallback` fails or the guest handler crashes, the existing FEX
  diagnostic path reports the fault; it does not silently swallow the signal.
- The `Flushing` re-entrancy guard prevents double-delivery if the guest handler
  itself blocks on a semaphore.
- Non-FEX builds compile the `Semaphore::acquire()` retry without the
  `FlushPendingGuestOrbisSignal` call; the existing Linux `sem.acquire()` path is
  preserved exactly.

## FEX / Linux Guard Behavior

The flush call and the `SA_RESTART` strip are gated on
`SHADPS4_ENABLE_FEX_GUEST_CPU`. On non-FEX Linux builds the semaphore wrapper
compiles to the standard `std::counting_semaphore<max>::acquire()` without
modification. The `posix_sigaction` `SA_RESTART` strip applies only to non-fault
signals (not SIGSEGV, SIGBUS, SIGILL) on Linux FEX builds.

## Testing

Implementation follows red-green TDD.

- A focused source-contract test must fail (RED) before the semaphore wrapper is
  changed. It asserts: (a) `FlushPendingGuestOrbisSignal` is absent from the
  Linux `acquire()` bounded-slice loop; (b) `try_acquire_for` with a 25 ms
  bounded slice is absent; (c) flush is absent from the timed
  `try_acquire_for` path. No claim is made about exposed `EINTR` iterations.
- After Task 2 the acquire-path assertions pass (PARTIAL GREEN); after Task 3
  the engine assertions also pass (full GREEN).
- The existing `bloodborne-fex-signal-source.test.mjs` regression (which
  asserts that `RestoreRIPFromHostPC` is absent from the engine) must continue
  to pass.
- The FEX harness smoke test (`run-fexcore-smoke-source.test.mjs`) must pass if
  available.
- A native CMake build must succeed without errors or new warnings.
- The Gradle sequence `test lintDebug assembleDebug` must succeed.
- Both `assets/runtime/manifest.json` and `assets/runtime/runtime.zip` must be
  present in the debug APK before installation.

Device acceptance for Galak-Z requires:

- process launches without deadlock at the GC-Finalizer stop-the-world boundary;
- `BACHATA_FEX_SIGNAL flush orbis_sig=30` appears in logcat for at least one flush
  cycle; and
- gameplay reaches the title screen or first interactive frame without crash.

Generic `BACHATA_FEX_SIGNAL` trace lines are present in the current implementation
for diagnostic purposes. They must be removed once device evidence confirms the
flush path is working correctly.

## Rollback

The change is confined to:

- `src/core/libraries/kernel/sync/semaphore.h` (bounded 25 ms slice loop with
  flush in Linux `acquire()` and `try_acquire_for()`; forward declaration of
  `Core::Fex::FlushPendingGuestOrbisSignal` under
  `SHADPS4_ENABLE_FEX_GUEST_CPU`),
- `src/core/fex/fex_guest_engine.cpp` (removal of stale SRA-spill diagnostics
  and the host-PC snapshot branch).

Reverting those edits restores the prior behavior without affecting the rest of the
signal infrastructure, the JIT, or the Bloodborne/AvPlayer fix paths.

## Success Criteria

Galak-Z `CUSA03146` advances past the GC stop-the-world hang into visible gameplay
on the Android FEX runtime, while Bloodborne signal delivery, the existing
regression test suite, and non-FEX behavior remain intact.
