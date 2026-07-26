import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const context = readFileSync(
  resolve(
    root,
    "android/BachataS4/core/runtime/src/main/cpp/vortek/bachata_vk_context.c",
  ),
  "utf8",
);

test("Vortek hides device extensions whose commands are not transported", () => {
  assert.match(context, /VK_EXT_vertex_input_dynamic_state/);
  assert.match(
    context,
    /requestCode == REQUEST_CODE_VK_CREATE_INSTANCE[\s\S]*disableUnsupportedDeviceExtensions\(context\)/,
  );
  assert.match(context, /strcmp\(extension, unsupported\[i\]\) == 0/);
});
