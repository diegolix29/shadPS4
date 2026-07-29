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
