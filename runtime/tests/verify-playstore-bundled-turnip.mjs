#!/usr/bin/env node

/**
 * Assert a Play Store APK/AAB packages all bundled Turnip assets
 * (mojo-26.1, mojo-25.0, gen8) and does not embed them under the managed runtime.
 */
import { execFileSync } from "node:child_process";
import { readFileSync, existsSync } from "node:fs";
import { resolve } from "node:path";

const target = resolve(process.argv[2] ?? "");
if (!target || !existsSync(target)) {
  console.error("Usage: verify-playstore-bundled-turnip.mjs <playstore.apk|aab>");
  process.exit(2);
}

const packages = [
  "drivers/turnip-26.1.0-EMULATOR.zip",
  "drivers/turnip-mojo-25.0-EMULATOR.zip",
  "drivers/turnip-gen8-EMULATOR.zip",
];

const listing = execFileSync("unzip", ["-Z1", target], { encoding: "utf8" })
  .split("\n")
  .filter(Boolean);

const missing = [];
for (const rel of packages) {
  const expectedAsset = `assets/${rel}`;
  const present = listing.some(
    (entry) =>
      entry === expectedAsset ||
      entry.endsWith(`/${expectedAsset}`) ||
      entry.endsWith(expectedAsset) ||
      entry.includes(expectedAsset),
  );
  if (!present) missing.push(expectedAsset);
}

if (missing.length > 0) {
  console.error(`Missing bundled Turnip asset(s) in ${target}:\n${missing.join("\n")}`);
  process.exit(1);
}

const runtimeLeak = listing.filter((entry) => {
  const path = entry.toLowerCase();
  return (
    path.includes("assets/runtime/") &&
    (path.includes("libvulkan_freedreno") ||
      path.includes("vulkan.ad07xx") ||
      path.includes("turnip-25") ||
      path.includes("turnip-26") ||
      path.includes("turnip-gen8") ||
      path.includes("turnip-mojo"))
  );
});
if (runtimeLeak.length > 0) {
  console.error(`Turnip leaked into managed runtime packaging:\n${runtimeLeak.join("\n")}`);
  process.exit(1);
}

for (const rel of packages) {
  const expectedShaPath = resolve(
    `android/BachataS4/app/src/playstore/assets/${rel}.sha256`,
  );
  if (!existsSync(expectedShaPath)) continue;
  const expected = readFileSync(expectedShaPath, "utf8").trim().split(/\s+/)[0];
  const assetEntry =
    listing.find((e) => e.endsWith(`assets/${rel}`) || e.endsWith(rel)) ??
    `assets/${rel}`;
  try {
    const actual = execFileSync(
      "bash",
      ["-lc", `unzip -p ${JSON.stringify(target)} ${JSON.stringify(assetEntry)} | sha256sum | awk '{print $1}'`],
      { encoding: "utf8", maxBuffer: 1024 * 1024 },
    ).trim();
    if (actual !== expected) {
      console.error(`Bundled Turnip SHA-256 mismatch for ${rel}: expected ${expected}, got ${actual}`);
      process.exit(1);
    }
  } catch (error) {
    console.error(`Failed to extract ${rel} for checksum: ${error.message ?? error}`);
    process.exit(1);
  }
}

console.log(`Play bundled Turnip packages OK in ${target} (${packages.length} assets)`);
