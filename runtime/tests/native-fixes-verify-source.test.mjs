import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const verifierUrl = new URL("verify-native-fixes.mjs", import.meta.url);
const gradleUrl = new URL(
  "../../android/BachataS4/app/build.gradle.kts",
  import.meta.url,
);

test("native-fixes verifier is wired into the APK build as a post-assemble gate", () => {
  const verifier = readFileSync(verifierUrl, "utf8");
  const gradle = readFileSync(gradleUrl, "utf8");

  assert.match(verifier, /Java_com_winlator_renderer_GPUImage_unlockHardwareBuffer/);
  assert.match(verifier, /abstract bind path=/);
  assert.match(verifier, /abstract listen path=/);
  assert.match(verifier, /validFdCount/);
  assert.match(verifier, /unzip/);
  assert.match(verifier, /nm/);
  assert.match(verifier, /strings/);

  assert.match(gradle, /verifyNativeRuntimeFixes/);
  assert.match(gradle, /verify-native-fixes\.mjs/);
  assert.match(gradle, /finalizedBy\("verifyNativeRuntimeFixes"\)/);
  assert.match(gradle, /startsWith\("assemble"\)/);
});
