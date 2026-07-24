#!/usr/bin/env node

/** Fail if an F-Droid / non-Play APK or AAB contains Play-only Turnip assets. */
import { execFileSync } from "node:child_process";
import { existsSync } from "node:fs";
import { resolve } from "node:path";

const target = resolve(process.argv[2] ?? "");
if (!target || !existsSync(target)) {
  console.error("Usage: verify-fdroid-no-bundled-turnip-asset.mjs <apk|aab>");
  process.exit(2);
}

const listing = execFileSync("unzip", ["-Z1", target], { encoding: "utf8" })
  .split("\n")
  .filter(Boolean);

const playAssetPatterns = [
  "assets/drivers/turnip-26.1.0-EMULATOR.zip",
  "assets/drivers/turnip-mojo-25.0-EMULATOR.zip",
  "assets/drivers/turnip-gen8-EMULATOR.zip",
  "assets/licenses/mesa-turnip-26.1.0-NOTICE.txt",
  "assets/licenses/mesa-turnip-mojo-25.0-NOTICE.txt",
  "assets/licenses/mesa-turnip-gen8-NOTICE.txt",
];

const leaks = listing.filter((entry) =>
  playAssetPatterns.some((pattern) => entry.includes(pattern)),
);

if (leaks.length > 0) {
  console.error(`Play-only Turnip assets leaked into non-Play package:\n${leaks.join("\n")}`);
  process.exit(1);
}

console.log(`No Play Turnip assets in ${target}`);
