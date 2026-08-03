#!/usr/bin/env node

import { execFileSync } from "node:child_process";
import { closeSync, mkdtempSync, openSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const apk = resolve(process.argv[2] ?? "android/BachataS4/app/build/outputs/apk/debug/app-debug.apk");
const nativeLibs = [
  { path: "lib/arm64-v8a/libwinlator.so", markers: { exportedSymbol: "Java_com_winlator_renderer_GPUImage_unlockHardwareBuffer", strings: ["abstract bind path=", "abstract listen path="] } },
  { path: "lib/arm64-v8a/libbachata_vortek_server.so", markers: { strings: ["validFdCount"] } },
];

function fail(message) {
  throw new Error(message);
}

function extract(apkPath, entry, destination) {
  const fd = openSync(destination, "w");
  try {
    execFileSync("unzip", ["-p", apkPath, entry], { stdio: ["ignore", fd, "pipe"], maxBuffer: 512 * 1024 * 1024 });
  } finally {
    closeSync(fd);
  }
}

const entries = execFileSync("unzip", ["-Z1", apk], { encoding: "utf8", maxBuffer: 16 * 1024 * 1024 })
  .split("\n").filter(Boolean);

const temporary = mkdtempSync(join(tmpdir(), "bachata-native-fixes-"));
try {
  for (const lib of nativeLibs) {
    if (!entries.includes(lib.path)) fail(`APK is missing native library: ${lib.path}`);
    const local = join(temporary, lib.path.split("/").pop());
    extract(apk, lib.path, local);
    if (lib.markers.exportedSymbol) {
      const symbols = execFileSync("nm", ["-D", local], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
      if (!symbols.includes(lib.markers.exportedSymbol)) {
        fail(`${lib.path} does not export ${lib.markers.exportedSymbol}`);
      }
    }
    if (lib.markers.strings) {
      const strings = execFileSync("strings", [local], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
      for (const marker of lib.markers.strings) {
        if (!strings.includes(marker)) fail(`${lib.path} is missing runtime string "${marker}"`);
      }
    }
    console.log(`native fixes verified: ${lib.path}`);
  }
  console.log(`native fixes verified for ${apk}`);
} finally {
  rmSync(temporary, { recursive: true, force: true });
}
