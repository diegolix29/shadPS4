# AvPlayer FEX Guest-Callback Bridge Design

Date: 2026-07-29

## Problem

Bloodborne reaches its title and Controls screens, then becomes black after selecting
Next. The last successful run presents exactly 32 frames. GPU submission continues,
but the game makes no further VideoOut calls.

The transition loads `libSceAvPlayer` and logs `StateReady`, but never `StatePlay`.
`AvPlayerState::DefaultEventCallback` invokes the game's event callback directly from
the AvPlayer controller thread. AvPlayer's memory and file replacement wrappers also
invoke game callbacks directly. That works when host and guest share the native x86-64
ABI, but FEX guest function addresses must run through the existing guest-callback
bridge.

## Goals

- Deliver AvPlayer events to FEX guest callbacks so the game can react to
  `StateReady` and start playback.
- Route AvPlayer allocation and deallocation callbacks through the same bridge.
- Keep FFmpeg file reads in host memory under FEX.
- Preserve the current native x86-64 AvPlayer path.
- Keep the change isolated to AvPlayer and the existing FEX callback facility.

## Non-goals

- Refactoring callbacks in unrelated libraries.
- Skipping, disabling, or faking game movies.
- Changing VideoOut, GPU synchronization, HTTP handle emulation, or texture-cache
  behavior.
- Making host FFmpeg buffers guest-addressable.

## Proposed Design

### Event callback dispatch

AvPlayer will use a small local callback-dispatch helper. In FEX-enabled builds it
will test the saved game callback with
`Core::GuestCpu::IsGuestFunctionAddress`. Guest addresses will run through
`Core::GuestCpu::RunGuestFunctionOrAbort`, passing the original object pointer,
event ID, source ID, and event-data pointer. Host callbacks will retain the current
direct call.

Non-FEX builds will compile to the current direct-call behavior. Null callbacks will
remain no-ops.

### Memory callback dispatch

The allocation, texture-allocation, deallocation, and texture-deallocation wrappers
will apply the same guest-address test. Guest callbacks will run through
`RunGuestFunctionOrAbort`; allocation results will be converted from the bridge's
`u64` return value to the expected pointer. Host callbacks and non-FEX builds will
remain direct calls.

This covers decoded-frame buffers that AvPlayer requests after playback starts.

### File replacement policy

FFmpeg reads into host-owned buffers. Passing those buffer addresses into the game's
guest `read_offset` callback is unsafe and cannot rely on transient HLE frame
publication from the AvPlayer worker thread.

When an FEX build receives guest file-replacement callbacks, `StubInitData` will
clear the replacement table. `AvPlayerSource` will then use its existing native-file
path, including `Core::FileSys::MntPoints::GetHostPath`, so FFmpeg reads the mounted
game file directly in host memory.

Host file callbacks and all non-FEX builds will keep the existing replacement
wrappers. An incomplete callback table will continue to select native file I/O as it
does today.

## Runtime Flow

1. The game initializes AvPlayer with guest event, memory, and file callbacks.
2. AvPlayer preserves the original callbacks.
3. Under FEX, guest file callbacks are replaced by the existing native mounted-file
   source path.
4. The controller reaches `StateReady`.
5. The event helper enters the guest callback through the Linker/FEX bridge.
6. The game starts AvPlayer, producing `StatePlay`.
7. FFmpeg decodes from the host file; decoded buffers use bridged guest allocators.
8. The game consumes frames and resumes VideoOut presentation.

## Error Handling

- Existing null-callback behavior and AvPlayer return codes remain unchanged.
- A guest callback execution failure uses the bridge's existing critical diagnostic
  and abort behavior; it will not silently lose an event or allocation.
- Native source open/read failures continue through existing AvPlayer error handling.
- Callback classification is per function address; host callbacks are never routed
  into FEX merely because FEX support is compiled in.

## Testing

Implementation will follow red-green TDD.

- Add a focused test or source-contract test that fails while AvPlayer directly
  invokes FEX guest event and memory callbacks.
- Verify the FEX policy selects native mounted-file I/O for a complete guest callback
  table, while host and non-FEX behavior remain unchanged.
- Run the relevant AvPlayer, guest-callback, and source-contract tests.
- Rebuild and verify the managed runtime before Gradle, then run
  `test`, `lintDebug`, and `assembleDebug` as required by `AGENTS.md`.
- Verify the APK contains both `assets/runtime/manifest.json` and
  `assets/runtime/runtime.zip`.

Device acceptance requires:

- `StateReady` followed by `StatePlay`;
- VideoOut presentation continuing beyond the previous 32-frame boundary;
- a non-black frame after the Controls-to-gameplay transition; and
- no FEX guest-callback failure.

## Rollback

The change is confined to AvPlayer callback dispatch and FEX file-selection policy.
Reverting those edits restores the previous behavior without changing save data,
runtime packaging, or renderer state.

## Success Criteria

Bloodborne advances beyond the title/menu movie transition into visible gameplay on
the Android FEX runtime, while native x86-64 behavior and existing AvPlayer error
semantics remain intact.
