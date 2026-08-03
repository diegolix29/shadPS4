import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("Vortek AHB always includes GPU color-output usage (swapchain import needs it)", () => {
  const gpuImage = read(
    "android/BachataS4/core/runtime/src/main/cpp/winlator/src/gpu_image.c",
  );
  const java = read(
    "android/BachataS4/core/runtime/src/main/java/com/winlator/renderer/GPUImage.java",
  );

  const create = gpuImage.slice(
    gpuImage.indexOf("AHardwareBuffer* createHardwareBuffer"),
  );
  // Must not regress to CPU-only usage when cpuAccess is true — that makes
  // XWindowSwapchain createImage return VK_ERROR_INITIALIZATION_FAILED.
  assert.match(create, /AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT/);
  assert.match(create, /AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE/);
  assert.match(create, /AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN/);
  assert.match(create, /AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN/);
  assert.doesNotMatch(
    create,
    /buffDesc\.usage = cpuAccess \? AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN/,
  );

  assert.match(gpuImage, /Java_com_winlator_renderer_GPUImage_unlockHardwareBuffer/);
  assert.match(java, /releaseCpuLock/);
  assert.match(java, /private native void unlockHardwareBuffer/);
});
