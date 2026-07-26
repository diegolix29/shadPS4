import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("Vortek waits for submitted rendering before copying the AHB to the compositor", () => {
  const cmake = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/CMakeLists.txt",
  );
  const swapchain = read(
    "android/BachataS4/core/runtime/src/main/cpp/vortek/bachata_xwindow_swapchain.c",
  );
  const probe = read("runtime/tests/vortek_probe/vortek_probe.c");

  assert.match(cmake, /\bbachata_xwindow_swapchain\.c\b/);
  assert.doesNotMatch(
    cmake,
    /\$\{VORTEK_UPSTREAM\}\/src\/xwindow_swapchain\.c/,
  );

  const present = swapchain.slice(
    swapchain.indexOf("void XWindowSwapchain_presentImage"),
  );
  assert.match(present, /vkQueueWaitIdle\(swapchain->queue\)/);
  assert.match(
    present,
    /vkQueueWaitIdle\(swapchain->queue\)[\s\S]*BachataUpstreamXWindowSwapchain_presentImage/,
  );
  assert.match(
    present,
    /if \(result != VK_SUCCESS\) \{[\s\S]*present_sync_failed[\s\S]*return;[\s\S]*\}/,
  );

  assert.doesNotMatch(
    probe,
    /Ensure GPU done before server-side AHB CPU copy on present/,
  );
  assert.doesNotMatch(
    probe,
    /wait_fences\(device, 1, &in_flight, VK_TRUE,[\s\S]{0,200}VkPresentInfoKHR pi/,
  );
});
