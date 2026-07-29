import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");
const section = (source, startMarker, endMarker) => {
  const start = source.indexOf(startMarker);
  assert.notEqual(start, -1, `missing start marker: ${startMarker}`);
  const end = source.indexOf(endMarker, start);
  assert.notEqual(end, -1, `missing end marker: ${endMarker}`);
  return source.slice(start, end);
};

test("AvPlayer dispatches game events through the FEX guest bridge", () => {
  const state = read("src/core/libraries/avplayer/avplayer_state.cpp");
  const callback = section(
    state,
    "void AvPlayerState::DefaultEventCallback",
    "// Called inside GAME thread",
  );

  assert.match(state, /#include "core\/guest_cpu\/guest_callback\.h"/);
  assert.match(callback, /#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU/);
  assert.match(
    callback,
    /IsGuestFunctionAddress\(callback_address\)/,
  );
  assert.match(
    callback,
    /RunGuestFunctionOrAbort\([\s\S]*"AvPlayer event"[\s\S]*ptr,[\s\S]*event_id,[\s\S]*source_id,[\s\S]*event_data\)/,
  );
  assert.match(callback, /callback\(ptr, event_id, source_id, event_data\)/);
});

test("AvPlayer dispatches game allocators through the FEX guest bridge", () => {
  const implementation = read("src/core/libraries/avplayer/avplayer_impl.cpp");
  const wrappers = [
    ["void* PS4_SYSV_ABI AvPlayer::Allocate(", "void PS4_SYSV_ABI AvPlayer::Deallocate(", "allocate", "AvPlayer allocate", true],
    ["void PS4_SYSV_ABI AvPlayer::Deallocate(", "void* PS4_SYSV_ABI AvPlayer::AllocateTexture(", "deallocate", "AvPlayer deallocate", false],
    ["void* PS4_SYSV_ABI AvPlayer::AllocateTexture(", "void PS4_SYSV_ABI AvPlayer::DeallocateTexture(", "allocate", "AvPlayer allocate texture", true],
    ["void PS4_SYSV_ABI AvPlayer::DeallocateTexture(", "int PS4_SYSV_ABI AvPlayer::OpenFile(", "deallocate", "AvPlayer deallocate texture", false],
  ];

  assert.match(implementation, /#include "core\/guest_cpu\/guest_callback\.h"/);
  assert.match(
    implementation,
    /const void\* CallbackAddress\(Callback callback\)/,
  );
  assert.match(
    implementation,
    /bool IsGuestCallback\(Callback callback\)/,
  );

  for (const [start, end, variable, label, returnsMemory] of wrappers) {
    const wrapper = section(implementation, start, end);
    assert.match(wrapper, new RegExp(`IsGuestCallback\\(${variable}\\)`));
    assert.match(
      wrapper,
      new RegExp(`RunGuestFunctionOrAbort\\([\\s\\S]*"${label}"`),
    );
    if (returnsMemory) {
      assert.match(
        wrapper,
        new RegExp(`return reinterpret_cast<void\\*>\\([\\s\\S]*RunGuestFunctionOrAbort\\([\\s\\S]*"${label}"`),
      );
      assert.match(wrapper, /return allocate\(ptr, alignment, size\);/);
    } else {
      assert.match(
        wrapper,
        new RegExp(`RunGuestFunctionOrAbort\\([\\s\\S]*"${label}"[\\s\\S]*\\);\\s*return;`),
      );
      assert.match(wrapper, /deallocate\(ptr, memory\);/);
    }
  }
});

test("AvPlayer keeps host FFmpeg buffers out of guest file callbacks", () => {
  const implementation = read("src/core/libraries/avplayer/avplayer_impl.cpp");
  const stub = section(
    implementation,
    "AvPlayerInitData AvPlayer::StubInitData",
    "AvPlayer::AvPlayer(",
  );

  assert.match(stub, /const bool missing_file_callback =/);
  assert.match(stub, /const bool has_guest_file_callback =/);
  for (const callback of ["open", "close", "read_offset", "size"]) {
    assert.match(
      stub,
      new RegExp(`IsGuestCallback\\(data\.file_replacement\.${callback}\\)`),
    );
  }
  assert.match(
    stub,
    /if \(missing_file_callback \|\| has_guest_file_callback\)[\s\S]*result\.file_replacement = \{\};/,
  );
  assert.match(stub, /result\.file_replacement\.open = &AvPlayer::OpenFile/);
  assert.match(stub, /result\.file_replacement\.read_offset = &AvPlayer::ReadOffsetFile/);
});
