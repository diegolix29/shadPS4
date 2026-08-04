#!/usr/bin/env node

import { execFileSync } from "node:child_process";
import { closeSync, mkdtempSync, openSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const apk = resolve(process.argv[2] ?? "android/BachataS4/app/build/outputs/apk/debug/app-debug.apk");
const nativeLibs = [
  { path: "lib/arm64-v8a/libwinlator.so", markers: { exportedSymbol: "Java_com_winlator_renderer_GPUImage_unlockHardwareBuffer", strings: ["abstract bind path=", "abstract listen path="] } },
  { path: "lib/arm64-v8a/libbachata_vortek_server.so", markers: { strings: ["validFdCount"] } },
];

const VORTEK_IN_ZIP = "host/lib/libvulkan_vortek.so";
const VORTEK_SOURCE_IN_ZIP = "usr/share/bachata/vortek/SOURCE.txt";
const RUNTIME_ZIP_IN_APK = "assets/runtime/runtime.zip";

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

  if (!entries.includes(RUNTIME_ZIP_IN_APK)) {
    fail(`APK is missing ${RUNTIME_ZIP_IN_APK}`);
  }
  const runtimeZipLocal = join(temporary, "runtime.zip");
  extract(apk, RUNTIME_ZIP_IN_APK, runtimeZipLocal);

  const zipEntries = execFileSync("unzip", ["-Z1", runtimeZipLocal], { encoding: "utf8", maxBuffer: 16 * 1024 * 1024 })
    .split("\n").filter(Boolean);

  if (!zipEntries.includes(VORTEK_IN_ZIP)) {
    fail(`runtime.zip is missing ${VORTEK_IN_ZIP}`);
  }

  const vortekLocal = join(temporary, "libvulkan_vortek.so");
  const vortekFd = openSync(vortekLocal, "w");
  try {
    execFileSync("unzip", ["-p", runtimeZipLocal, VORTEK_IN_ZIP], { stdio: ["ignore", vortekFd, "pipe"], maxBuffer: 128 * 1024 * 1024 });
  } finally {
    closeSync(vortekFd);
  }

  const vortekSymbols = execFileSync("nm", ["-D", vortekLocal], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
  if (!vortekSymbols.includes("vk_icdGetInstanceProcAddr")) {
    const elfSyms = execFileSync("readelf", ["-Ws", vortekLocal], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
    if (!elfSyms.includes("vk_icdGetInstanceProcAddr")) {
      fail(`${VORTEK_IN_ZIP} is missing ICD export vk_icdGetInstanceProcAddr`);
    }
  }

  const vortekStrings = execFileSync("strings", [vortekLocal], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
  if (!/[0-9a-f]{40}/.test(vortekStrings)) {
    fail(`${VORTEK_IN_ZIP} does not contain a BACHATA_VORTEK_CLIENT_BUILD_ID marker (40-char hex SHA)`);
  }

  const elfDyn = execFileSync("readelf", ["-d", vortekLocal], { encoding: "utf8", maxBuffer: 16 * 1024 * 1024 });
  if (/linker64|bionic/i.test(elfDyn)) {
    fail(`${VORTEK_IN_ZIP} appears to be an Android/Bionic binary (linker64/bionic in dynamic section)`);
  }
  if (!elfDyn.includes("libc.so.6")) {
    fail(`${VORTEK_IN_ZIP} does not link glibc libc.so.6 — expected aarch64-glibc target`);
  }

  let pinnedRevision = null;
  if (zipEntries.includes(VORTEK_SOURCE_IN_ZIP)) {
    const sourceTxtLocal = join(temporary, "vortek_SOURCE.txt");
    const sourceFd = openSync(sourceTxtLocal, "w");
    try {
      execFileSync("unzip", ["-p", runtimeZipLocal, VORTEK_SOURCE_IN_ZIP], { stdio: ["ignore", sourceFd, "pipe"], maxBuffer: 1 * 1024 * 1024 });
    } finally {
      closeSync(sourceFd);
    }
    const sourceText = readFileSync(sourceTxtLocal, "utf8");
    const revMatch = sourceText.match(/^revision=([0-9a-f]{40})$/m);
    if (revMatch) {
      pinnedRevision = revMatch[1];
      if (!vortekStrings.includes(pinnedRevision)) {
        fail(
          `${VORTEK_IN_ZIP} does not contain the pinned JICA98 revision ${pinnedRevision} ` +
          `from ${VORTEK_SOURCE_IN_ZIP} — binary may not be from the pinned fork`
        );
      }
      console.log(`vortek client fix verified: revision=${pinnedRevision}`);
    }
  } else {
    fail(`runtime.zip is missing ${VORTEK_SOURCE_IN_ZIP} — provenance record required`);
  }

  console.log(`vortek client verified: ${VORTEK_IN_ZIP} (ICD export ✓, glibc ✓, no bionic ✓, revision pinned ✓)`);
  console.log(`native fixes verified for ${apk}`);
} finally {
  rmSync(temporary, { recursive: true, force: true });
}

