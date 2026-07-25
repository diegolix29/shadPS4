# Goal

Produce an exactly symbol-matched diagnostic runtime, capture and symbolize the host branch-to-zero crash during Sonic Mania pipeline compilation, and fix only the proven null function or corrupted callback.

Current milestone:

```text
renderer initialized: yes
first visible frame: no
menu/gameplay: no
```

Current failure:

```text
SIGSEGV
PC = 0x0
host render/pipeline thread
during Vulkan pipeline compilation or immediately adjacent work
```

Additional observation:

```text
17 × SIGBUS / BUS_ADRALN
mapping: [anon:FEXMemJIT]
thread: guest/FEX thread
```

Do not treat those SIGBUS events as causal until proving they escaped FEX’s normal signal-emulation path or corrupted state.

Use **caveman-skill** and **context-mode MCP throughout this task**.

# Non-negotiable constraints

Do not:

* rebuild an independently laid-out “debug version” and use its offsets
* patch `signals.cpp`
* suppress the SIGSEGV
* treat `PC=0` as sufficient identification
* guess another Vulkan function
* disable pipeline compilation
* disable LTO only in the diagnostic build and assume addresses still match
* modify Vortek RPC before symbolizing the caller
* modify FEX because handled SIGBUS events appeared
* add a Sonic Mania title-specific workaround
* change Turnip behavior
* revisit the fixed fence or BindVertexBuffers2 paths

# Stage 0 — Correct the device identity

Before further testing, capture:

```bash
adb -s 80605355 shell getprop ro.product.manufacturer
adb -s 80605355 shell getprop ro.product.brand
adb -s 80605355 shell getprop ro.product.model
adb -s 80605355 shell getprop ro.product.device
adb -s 80605355 shell getprop ro.build.version.release
adb -s 80605355 shell getprop ro.build.version.sdk
adb -s 80605355 shell getprop ro.soc.manufacturer
adb -s 80605355 shell getprop ro.soc.model
```

Also capture:

```text
Vulkan physical-device name
vendor ID
device ID
system Vulkan driver path
```

Do not label the device “Realme X2 Pro, Adreno 830.”

Use the actual returned identity consistently in all subsequent reports.

# Stage 1 — Produce symbols from the exact deployed link

The current problem is that the deployed binary and local unstripped binary came from different link outputs.

Replace that process with:

```text
one compile
one link
one original ELF
→ create debug companion from that ELF
→ create stripped deployment copy from that same ELF
```

## Preserve production-relevant build settings

Use the same settings as the failing artifact:

```text
same source commit
same compiler and linker
same target architecture
same optimization
same LTO setting
same feature defines
same linked libraries
same link ordering
same linker script
same FEX configuration
```

Add:

```text
-g
-fno-omit-frame-pointer
-Wl,--build-id=sha1
```

Do not disable LTO for the first diagnostic artifact if the crash currently occurs with LTO.

If frame pointers conflict with the exact production profile, document the change and first try DWARF-based unwinding while retaining the original optimization and LTO setup.

## Build one canonical ELF

Example artifact naming:

```text
runtime/build/rootfs/bin/shadps4-arm64-fex.full
runtime/build/rootfs/bin/shadps4-arm64-fex.debug
runtime/build/rootfs/bin/shadps4-arm64-fex
```

The `.full` file must be the direct linker output.

Never relink between producing `.full`, `.debug`, and the deployed binary.

## Generate the separate debug file

Use the toolchain-matched LLVM utilities:

```bash
cp shadps4-arm64-fex.full shadps4-arm64-fex

llvm-objcopy \
  --only-keep-debug \
  shadps4-arm64-fex.full \
  shadps4-arm64-fex.debug

llvm-strip \
  --strip-debug \
  shadps4-arm64-fex

llvm-objcopy \
  --add-gnu-debuglink=shadps4-arm64-fex.debug \
  shadps4-arm64-fex
```

If the existing production pipeline uses stronger stripping, reproduce it on the copied deployment file only, after creating the debug companion.

Do not strip `.full` or `.debug`.

## Verify correspondence

Run:

```bash
llvm-readelf -n shadps4-arm64-fex.full
llvm-readelf -n shadps4-arm64-fex.debug
llvm-readelf -n shadps4-arm64-fex

llvm-readelf -l shadps4-arm64-fex.full
llvm-readelf -l shadps4-arm64-fex

llvm-readelf -S shadps4-arm64-fex.debug
llvm-nm -anC shadps4-arm64-fex.debug | head
```

Required conditions:

```text
full Build ID == debug Build ID == deployed Build ID
PT_LOAD virtual addresses are unchanged
entry point is unchanged
deployed executable code sections correspond to full ELF
debug companion contains DWARF and/or symbol information
```

Record SHA-256 for all three artifacts.

The file hashes do not need to match because the deployed file is stripped. Their Build IDs must match.

## Add a permanent packaging guard

Modify the runtime packaging pipeline so every packaged shadPS4 binary can optionally produce:

```text
debug-symbols/<build-id>/shadps4-arm64-fex.debug
debug-symbols/<build-id>/manifest.json
```

Manifest fields:

```text
source commit
binary Build ID
binary SHA-256
debug-file SHA-256
compiler
linker
optimization flags
LTO setting
target architecture
timestamp
```

Do not package the debug file in the production APK/AAB.

# Stage 2 — Deploy the exact stripped copy

Package and deploy:

```text
shadps4-arm64-fex
```

created from the canonical `.full` binary.

After installation, retrieve or hash the actual executable being launched.

Verify:

```text
deployed SHA-256 == locally generated stripped SHA-256
deployed Build ID == local debug Build ID
```

Add launch logging:

```text
[Bachata.Symbols] binary_path=...
[Bachata.Symbols] build_id=...
[Bachata.Symbols] sha256=...
[Bachata.Symbols] debug_bundle=available
```

Do not proceed if the build IDs differ.

# Stage 3 — Pre-capture module mappings

The crash handler cannot safely perform complex `/proc` parsing.

Before launching the game renderer, capture:

```text
/proc/self/maps
/proc/self/smaps, optional
loaded module list
binary Build IDs
```

Write the map snapshot to the app’s debug directory.

Refresh it after major `dlopen` operations if required.

Required mappings include:

```text
shadps4-arm64-fex
FEXCore
libvulkan_vortek.so
host glibc Vulkan loader
Vortek-related libraries
pipeline-worker libraries
```

# Stage 4 — Harden the SIGSEGV crash record

Add or extend a diagnostic mode:

```text
BACHATA_CRASH_REGISTERS=1
```

The signal handler must use `SA_SIGINFO` and record, using only async-signal-safe operations:

```text
signal number
si_code
fault address
thread ID
thread name captured in advance
PC
SP
x0–x30
PSTATE, when available
```

For this crash, the essential values are:

```text
PC = 0
LR / x30
SP
x0–x29
```

Do not assume `x8` was the indirect branch target merely because it was zero.

Write the record through:

```text
pre-opened FD
write()
fixed-size binary record
```

Do not use:

```text
malloc/new
C++ streams
spdlog
mutexes
symbolizer APIs
complex Android logging
```

Required output:

```text
[Bachata.Crash] signal=SIGSEGV
[Bachata.Crash] thread=<name>
[Bachata.Crash] pc=0x0
[Bachata.Crash] lr=0x...
[Bachata.Crash] sp=0x...
[Bachata.Crash] fault=0x0
[Bachata.Crash] build_id=<exact ID>
```

# Stage 5 — Attach LLDB when possible

The debug app is debuggable. Prefer an LLDB session for one reproduction.

Attach to the actual shadPS4/FEX host process, not only the Android service process.

Configure:

```text
SIGSEGV: stop and do not immediately pass
SIGBUS: initially pass through without stopping
```

The FEX guest path may intentionally handle SIGBUS. Do not let repeated handled SIGBUS stops obscure the fatal SIGSEGV.

At SIGSEGV collect:

```text
process ID
thread list
current thread
thread backtrace all
register read
image list
image lookup for LR
disassembly around LR
stack memory around SP
```

Required disassembly:

```text
at least 32 bytes before LR
at least 16 bytes after LR
```

Because `PC=0`, identify the call instruction immediately before the return address.

Likely AArch64 pattern:

```asm
blr xN
```

or:

```asm
blr x16
blr x17
```

Record:

```text
branch instruction
branch register
branch-register value
instruction(s) loading that register
owning object/dispatch table/callback structure
```

Do not stop after reporting LR’s function name.

# Stage 6 — Offline symbolization

Use the matching `.debug` file.

Determine load bias correctly from:

```text
/proc/<pid>/maps
ELF PT_LOAD headers
mapping file offset
```

For a PIE executable, do not pass the raw runtime address directly to `addr2line`.

Calculate:

```text
object_address = runtime_address - load_bias
```

Then run:

```bash
llvm-symbolizer \
  --obj=shadps4-arm64-fex.debug \
  --functions=linkage \
  --inlines \
  0x<OBJECT_ADDRESS>
```

Also run:

```bash
llvm-addr2line \
  -Cfpie \
  shadps4-arm64-fex.debug \
  0x<OBJECT_ADDRESS>
```

Symbolize:

```text
LR
LR - 4
several return addresses from stack
all debugger backtrace addresses
```

Required deliverable:

```text
source file
function
line
inlined caller chain
instruction that branches to zero
register used by the indirect branch
```

# Stage 7 — Classify the exact null target

Choose exactly one classification after symbolization.

## A. Vulkan procedure pointer

Evidence:

```text
caller invokes Vulkan function pointer
branch register came from Vulkan-Hpp or Vortek dispatch table
```

Then record:

```text
function name
proc-query source
whether direct GDPA is non-null
whether dispatcher slot is non-null
whether pointer changed after initialization
```

Add a focused real-device execution test for that exact Vulkan function before changing production code.

## B. Async pipeline callback

Evidence:

```text
caller is AsyncPipelineCreator worker/completion logic
null target is callback, ThreadPool job, completion handler, or ShaderInspector method
```

Audit:

```text
callback initialization
callback ownership
worker lifetime
context lifetime
ShaderInspector lifetime
pipeline result lifetime
completion-pipe payload
connection shutdown
```

## C. Host Vulkan function inside server

Evidence:

```text
caller is request_handler, async_pipeline_creator, or server Vulkan helper
null target is host function obtained from GIPA/GDPA
```

Audit:

```text
function name
instance/device used to query it
extension/core requirement
host support
server function-table initialization order
```

## D. Corrupted function pointer

Evidence:

```text
pointer was previously valid
memory contents changed unexpectedly
no legitimate reinitialization occurred
```

Set a debugger watchpoint on the field.

Audit likely writers:

```text
large pipeline response
SEND_EXTRA_DATA
variable-length pipeline structures
completion pipe
ThreadPool task object
output VkPipeline array
serializer size arithmetic
```

## E. FEX-generated/native bridge target

Evidence:

```text
caller or branch table resides in FEXCore/FEXMemJIT
```

Capture:

```text
guest RIP
host JIT PC
FEX block metadata
guest instruction represented by block
signal-emulation state
```

Do not treat a generated-code address as a normal ELF symbol.

# Stage 8 — Audit the recent async-pipeline changes first if implicated

The async pipeline creator was recently enabled and is active immediately before the crash.

If the symbolized caller is in or near this subsystem, audit:

```text
AsyncPipelineCreator_create
worker entry
ThreadPool initialization
ThreadPool shutdown
pipeline completion callback
notify FD
completion payload
pipeline array
pipeline cache
ShaderInspector
VkContext
connection/session context
```

Check these concrete hazards:

```text
callback pointer never initialized
callback initialized only in Winlator path
callback cleared during connection transition
worker outlives VkContext
worker outlives ShaderInspector
worker outlives request ring
pipeline result array partially uninitialized
pipelineCount arithmetic overflow
short read from completion pipe
payload count mismatch
error result followed by invalid pipeline handles
```

Initialize output arrays:

```cpp
std::vector<VkPipeline> pipelines(count, VK_NULL_HANDLE);
```

Use checked size calculations:

```text
count × sizeof(VkPipeline)
header size + payload size
```

On host pipeline creation failure:

```text
return exact VkResult
do not invoke null completion callback
do not serialize undefined handles
do not publish partially initialized objects
```

# Stage 9 — Classify the preceding SIGBUS events separately

Add:

```text
BACHATA_FEX_TRACE_SIGBUS=1
```

For each SIGBUS record:

```text
si_code
fault address
host PC
guest RIP
thread
whether FEX handler claimed it
whether execution resumed
handler result
```

Aggregate rather than logging every repeated event indefinitely.

Required conclusion:

```text
handled emulation event
or
unhandled/fatal alignment error
```

If all 17 events are handled and execution resumes, do not modify FEX for them.

If one event escapes or corrupts state, identify:

```text
guest instruction
host JIT block
alignment
memory address
FEX handler decision
```

Do not disable strict alignment globally.

# Stage 10 — Add a reproducible crash test

Once the exact caller is known, create the smallest test that reaches it.

Likely options:

```text
pipeline creation with Sonic-like nested state
async pipeline completion
large SPIR-V payload
large SEND_EXTRA_DATA payload
specific Vulkan command invocation
FEX JIT instruction sequence
```

The focused test must:

```text
fail before the fix
pass after the fix
use real Vortek client/server when applicable
run on CPH2649
clean up successfully
```

Do not use Sonic Mania as the only regression test.

# Stage 11 — Apply only the proven fix

After identifying the exact null target:

1. Explain why it is null.
2. Fix its initialization, lookup, ownership, or protocol.
3. Add an assertion before indirect use.
4. Add a clear diagnostic message.
5. Add the focused regression test.
6. Verify no adjacent pointers share the same problem.

Do not convert the null call to a no-op.

Do not return fake pipeline or Vulkan success.

# Stage 12 — Required regression sequence

Run:

```text
exact focused crash test
BindVertexBuffers2 suite
fence matrix
Task 5 transport
Task 6 headless
Task 7 WSI
Task 8 ShadProbe
Turnip Sonic Mania
```

Then launch Sonic Mania using:

```bash
adb -s 80605355 shell am start \
  -n com.bachatas4.android/.DirectLaunchActivity \
  --es game_id CUSA07023
```

Confirm:

```text
SYSTEM_VORTEK
HOST_GLIBC
Adreno 830 system driver
no branch to PC 0 at old location
```

Continue only until:

```text
first visible frame/menu/gameplay
```

or the next exact blocker.

# Required final report

## Device identity

Report actual:

```text
manufacturer
brand
model
device codename
SoC
GPU
Android version
```

## Artifact correspondence

Report:

```text
source commit
full ELF SHA-256
debug ELF SHA-256
deployed ELF SHA-256
full Build ID
debug Build ID
deployed Build ID
exact match status
```

## Crash symbolization

Report:

```text
PC
LR
load bias
object-relative LR
source file
function
line
inline chain
branch instruction
branch register
branch-register value
pointer origin
```

## SIGBUS classification

Report:

```text
count
si_code
handled by FEX yes/no
execution resumed yes/no
causal yes/no
```

## Proven root cause

Choose exactly one:

```text
missing Vulkan proc
null async callback
null host Vulkan proc
lifetime bug
protocol/memory corruption
FEX JIT issue
other proven cause
```

## Fix and focused test

List changed files and focused before/after results.

## Sonic Mania result

State:

```text
renderer initialized
first visible frame
menu
gameplay
```

Include the next exact blocker when applicable.

# Stop condition

Do not perform further speculative pipeline, Vulkan, or FEX fixes until the exact deployed crash is symbolized using the matching Build ID.

After fixing the exact branch-to-zero cause, continue only until:

```text
first visible frame/menu/gameplay
```

or another precise blocker is captured.

End with exactly:

```text
NULL PIPELINE CALL FIXED — CONTINUE TASK 9
```

or:

```text
BLOCKED
```
