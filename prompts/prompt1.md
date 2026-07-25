# Goal

Identify and correctly handle the exact syscall causing:

```text
SIGSYS
signal 31
exitCode=159
```

during Sonic Mania startup through the FEX guest-CPU path.

The previous Vulkan issues are fixed:

```text
fence waits pass
vkCmdBindVertexBuffers2 reaches Vortek
RPC serialization passes
host command executes successfully
Tasks 5–8 pass
Turnip Sonic Mania remains working
```

Do not modify Vortek, Vulkan dispatch, fence waits, or `BindVertexBuffers2` unless their regressions fail.

The next milestone is:

```text
identify exact SIGSYS source
→ implement the narrow syscall fix
→ relaunch Sonic Mania
→ reach menu/gameplay or capture the next exact blocker
```

Use **caveman-skill** and **context-mode MCP throughout this task**.

# Critical distinction

Do not assume “FEX unhandled syscall” merely because the crash occurs in FEXCore.

`SIGSYS` can originate from multiple paths:

```text
Android seccomp SECCOMP_RET_TRAP
FEX explicitly raising SIGSYS
a guest signal translated by FEX
syscall-user-dispatch
wrong host syscall ABI/number
another host library making a blocked syscall
```

The first task is to classify the signal using `siginfo_t`.

# Stage 1 — Locate existing SIGSYS ownership

Using context-mode MCP, search the complete runtime and FEX source for:

```text
SIGSYS
SYS_SECCOMP
sigaction
SA_SIGINFO
raise(SIGSYS)
tgkill
rt_tgsigqueueinfo
seccomp
SyscallHandler
UnhandledSyscall
UnsupportedSyscall
HandleSIGSYS
SignalDelegator
GuestSignal
```

Determine:

```text
whether FEX already installs a SIGSYS handler
whether shadPS4 installs one
whether the Android runtime installs one
which handler wins
whether the handler is replaced later
whether FEX intentionally uses SIGSYS internally
```

Do not add a competing signal handler before understanding existing ownership.

Prefer extending the existing FEX signal infrastructure.

# Stage 2 — Add an async-signal-safe SIGSYS trap record

Add diagnostic mode:

```text
BACHATA_FEX_TRACE_SIGSYS=1
```

Add it only to the validated internal environment allow-list.

The signal handler must receive:

```cpp
sigaction(..., SA_SIGINFO, ...)
```

and record the following from `siginfo_t`:

```text
si_signo
si_code
si_errno
si_syscall
si_arch
si_call_addr
```

Also capture host AArch64 register state from `ucontext_t`:

```text
pc
sp
x8
x0
x1
x2
x3
x4
x5
x29
x30
```

On AArch64:

```text
x8 normally contains the host syscall number
x0–x5 contain syscall arguments
```

Also record:

```text
host thread ID
thread name
process ID
FEX thread/context ID
current guest RIP
current guest syscall number, when available
guest syscall arguments, when available
```

Required output:

```text
[Bachata.FEX.SIGSYS] signo=31
[Bachata.FEX.SIGSYS] code=<value>
[Bachata.FEX.SIGSYS] errno=<value>
[Bachata.FEX.SIGSYS] syscall=<number>
[Bachata.FEX.SIGSYS] arch=<audit architecture>
[Bachata.FEX.SIGSYS] call_addr=<address>
[Bachata.FEX.SIGSYS] host_pc=<address>
[Bachata.FEX.SIGSYS] host_x8=<number>
[Bachata.FEX.SIGSYS] guest_rip=<address>
[Bachata.FEX.SIGSYS] guest_syscall=<number or unavailable>
```

Use only async-signal-safe operations:

```text
pre-opened file descriptor
write()
fixed-size structures
fixed-size stack buffer with safe manual formatting
```

Do not use:

```text
malloc
new
iostream
spdlog
Android logging APIs with unknown signal safety
mutexes
condition variables
symbolization inside the handler
```

After writing the record, preserve existing behavior:

* chain to the previous handler when required, or
* restore the default and re-raise.

Do not swallow the signal in diagnostic mode.

# Stage 3 — Classify the signal

Use `si_code`, `si_arch`, and `si_syscall`.

## Classification A — Android seccomp trap

Evidence:

```text
si_code == SYS_SECCOMP
valid si_syscall
valid si_arch
```

This means the host Android kernel rejected a host syscall.

Record:

```text
host syscall number
host architecture
host instruction address
host library/executable mapping
host arguments
```

Use `/proc/<pid>/maps` captured before the crash to symbolize `si_call_addr`.

Map it to:

```text
FEXCore
glibc
libpthread
shadPS4
another runtime library
generated FEX code
```

## Classification B — FEX-generated SIGSYS

Evidence:

```text
si_code is not SYS_SECCOMP
signal originates from raise/tgkill or FEX signal machinery
```

Locate the exact FEX source line that generated the signal.

Record:

```text
guest syscall number
guest ABI
guest RIP
FEX handler/table entry
reason it was considered unsupported
```

## Classification C — ABI mismatch

Evidence:

```text
host x8 differs from si_syscall
x86 syscall number was issued directly as AArch64 syscall
wrong syscall table selected
```

Fix the ABI translation. Do not add the wrong number to an Android allow-list.

End Stage 3 with exactly one classification.

# Stage 4 — Resolve the syscall name

Resolve the number using the headers and tables used by the actual build.

Do not rely only on a web list or hard-coded table.

Check:

```text
Android NDK/kernel headers for AArch64 host syscall
FEX x86-64 guest syscall table
FEX x86 guest syscall table, if applicable
generated syscall dispatch tables
```

Report:

```text
host syscall number and name
guest syscall number and name
host ABI
guest ABI
calling module
call arguments
```

Add a build-time helper or test that prints the mapping.

# Stage 5 — Controlled rseq A/B test

Only run this stage when:

* the trapped syscall is confirmed as `rseq`, or
* the trace shows glibc/FEX attempting rseq registration immediately before SIGSYS.

Audit the actual environment received by the crashing process:

```text
/proc/<pid>/environ
RuntimeProcessLauncher environment
FEX subprocess environment
HOST_GLIBC loader environment
```

Check whether this is already present:

```text
GLIBC_TUNABLES=glibc.pthread.rseq=0
```

Do not overwrite unrelated glibc tunables.

Merge safely:

```text
existing:
glibc.malloc.foo=...

result:
glibc.malloc.foo=...:glibc.pthread.rseq=0
```

Run two controlled sessions:

```text
A: current environment
B: glibc.pthread.rseq=0
```

Compare:

```text
SIGSYS count
trapped syscall
highest Sonic Mania milestone
FEX thread creation
guest CPU startup
performance
```

Required conclusion:

```text
rseq disable changes failure: yes/no
```

Do not declare rseq fixed merely because the process survives longer.

# If confirmed syscall is rseq

Determine which layer is invoking it:

## Host glibc registration

If the host glibc runtime invokes AArch64 `rseq`:

* keep `glibc.pthread.rseq=0` in the narrowly scoped FEX/shadPS4 environment
* do not apply it globally to the Android application
* add a test proving the variable reaches all relevant FEX-created threads/processes
* document the expected performance tradeoff
* verify thread creation, synchronization, and shutdown

## Guest glibc registration translated by FEX

If guest x86-64 glibc invokes guest `rseq`:

* confirm the guest syscall number and arguments
* inspect FEX’s existing guest `rseq` implementation
* determine whether the Android host syscall is necessary
* prefer a correct FEX fallback that returns a normal Linux error such as the host-equivalent unsupported result when safe
* verify guest glibc falls back without terminating
* do not return success without implementing rseq semantics

## FEX internal registration

If FEX itself uses rseq:

* disable or replace only that optimization on Android
* keep it enabled on normal Linux hosts
* guard using a clear Android/runtime capability decision, not a game check
* add a standalone FEX thread/JIT test

# Stage 6 — Fix non-rseq syscalls narrowly

If the syscall is not rseq, classify it before changing behavior.

For the confirmed syscall, determine:

```text
does Android permit it for ordinary app processes?
does FEX already provide an emulation or fallback?
does guest software handle ENOSYS?
can it be translated to an older permitted syscall?
is the syscall required for correctness?
```

Examples of valid narrow patterns:

```text
clone3 → clone fallback, when semantics match
new time syscall → compatible older syscall, when ranges permit
unsupported optional query → return correct Linux error
guest-only operation → emulate inside FEX
```

Do not apply these examples unless they match the captured syscall.

Never:

```text
disable Android seccomp
attempt to replace the app seccomp policy
ignore every SIGSYS
return success for unknown syscalls
convert every blocked syscall to ENOSYS
run the app as root
add broad syscall passthrough
```

# Stage 7 — Add a focused syscall reproduction test

Create the smallest test that reaches the same FEX syscall path without launching Sonic Mania.

The test must use:

```text
same FEX build
same host glibc
same Android process type
same environment
same guest ABI
```

Required test modes:

```text
baseline reproduces SIGSYS or blocked result
fixed mode returns expected result
no process termination
subsequent guest code executes
```

For rseq, test:

```text
main-thread registration
new pthread registration
thread exit/unregistration if applicable
multiple sequential threads
```

For another syscall, exercise its real argument pattern.

# Stage 8 — Verify FEX syscall dispatch behavior

Audit the exact handler for the confirmed guest syscall.

Report:

```text
source file
handler function
registration in syscall table
guest ABI
host call made
error conversion
signal behavior
```

A guest syscall that is unsupported should normally produce a guest-visible Linux error according to FEX’s design, not kill the host process with an unexplained SIGSYS.

If FEX currently deliberately traps unsupported syscalls, preserve upstream behavior outside the Bachata Android configuration and implement a scoped compatibility path.

# Stage 9 — Relaunch Sonic Mania

After the focused test passes:

1. Run the `BindVertexBuffers2` focused test.
2. Run the fence matrix.
3. Run Task 6 headless.
4. Run Task 7 WSI.
5. Run Task 8 ShadProbe.
6. Launch:

```bash
adb -s 80605355 shell am start \
  -n com.bachatas4.android/.DirectLaunchActivity \
  --es game_id CUSA07023
```

Confirm:

```text
driver=system-vortek
box64Mode=HOST_GLIBC
BindVertexBuffers2 passes
no SIGSYS for the fixed syscall
```

Continue until:

```text
first visible frame
menu
gameplay
```

or a new precise blocker occurs.

# Required regressions

Confirm:

```text
BindVertexBuffers2 tests pass
fence matrix passes
Task 5 transport passes
Task 6 headless passes
Task 7 WSI passes
Task 8 ShadProbe passes
Turnip Sonic Mania remains around 60 FPS
SYSTEM_VORTEK remains opt-in
no silent fallback
ICD remains Vulkan 1.3.0
custom_vulkan_bridge.cpp remains unchanged
Android seccomp remains enabled
```

# Required report

## SIGSYS classification

Report:

```text
si_code
si_errno
si_syscall
si_arch
si_call_addr
host x8
host PC
guest syscall
guest RIP
```

Choose exactly one source:

```text
Android seccomp
FEX-generated signal
guest signal translation
ABI mismatch
other proven source
```

## Syscall identity

Report:

```text
host number/name
guest number/name
calling module
arguments
reason it failed
```

## Root cause

State one proven root cause.

Examples:

```text
host glibc rseq blocked by Android seccomp
guest rseq incorrectly forwarded to blocked host rseq
missing FEX guest syscall handler
wrong ABI syscall number
unsupported Android syscall without fallback
```

## Fix

List:

```text
FEX files
launcher/environment files
tests
configuration guards
```

## Focused test

Include baseline and fixed outcomes.

## Sonic Mania result

State:

```text
renderer initialized
first visible frame
menu
gameplay
```

Include the next blocker when present.

# Stop condition

Do not make a syscall behavior change before recording:

```text
si_code
si_syscall
si_arch
si_call_addr
```

After identifying and fixing the syscall, continue only until:

```text
menu/gameplay is reached
```

or:

```text
a new exact blocker is captured
```

End with exactly:

```text
SIGSYS FIXED — CONTINUE TASK 9
```

or:

```text
BLOCKED
```
