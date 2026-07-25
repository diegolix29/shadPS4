#!/usr/bin/env node

import { createHash } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const EXPECTED_COMPONENTS = [
  { name: "box64", url: "https://github.com/ptitSeb/box64.git", revision: "50c8b90b09b433ab0767de44af2d0731cb0748b7", license: "MIT" },
  { name: "winlator-app", url: "https://github.com/brunodev85/winlator-app.git", revision: "e113da42beefc39c69c8944b27c19c3703bfa856", license: "LGPL-2.1" },
  { name: "winlator-components", url: "https://github.com/brunodev85/winlator.git", revision: "fb66541b93a4eb3ee585a433b4c7b20544d58e40", license: "MIT" },
  { name: "glibc-packages", url: "https://github.com/termux-pacman/glibc-packages.git", revision: "26d89ba7a1f856b99f0d437bef54f558b2485075", license: "mixed" },
  { name: "mesa", url: "https://gitlab.freedesktop.org/mesa/mesa.git", revision: "6984e91b5fe1d1c204e54954a4282fcdc0c44b78", license: "MIT" },
  {
    name: "fex",
    url: "https://github.com/FEX-Emu/FEX.git",
    revision: "f2b679f6028ce1c38875233aecfcf5d3f8ebecec",
    license: "MIT",
  },
  {
    name: "vortek-client",
    url: "https://github.com/brunodev85/vortek.git",
    revision: "ab7329c4b445a4abd9b9af91b8148e1ca41464fa",
    license: "LGPL-2.1",
    sourceDestination: "runtime/sources/vortek-client",
    buildOutput: "host/lib/libvulkan_vortek.so",
  },
  {
    name: "vortek-server",
    url: "https://github.com/brunodev85/winlator-app.git",
    revision: "e113da42beefc39c69c8944b27c19c3703bfa856",
    license: "LGPL-2.1",
    sourceDestination: "runtime/sources/winlator-app/app/src/main/cpp/vortekrenderer",
    sourcedFrom: "winlator-app",
  },
];
const FEX_REVISION = EXPECTED_COMPONENTS.find(({ name }) => name === "fex").revision;
const EXPECTED_INPUTS = [
  { name: "glibc-2.43+r22+g8362e8ce10b2-2-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/g/glibc/glibc-2.43%2Br22%2Bg8362e8ce10b2-2-x86_64.pkg.tar.zst", sha256: "2c20828b3a571b272697671c90b1e3a8c426d6a7e7fb99a242099373f2710fe1" },
  { name: "glibc-2.43+r22+g8362e8ce10b2-2-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/core/glibc-2.43+r22+g8362e8ce10b2-2-aarch64.pkg.tar.xz", sha256: "8fac217e98c6e4342326726b2640ac254e8c82032f06f30bfa13ebbcc4fcb25b" },
  { name: "libstdc++-16.1.1+r12+g301eb08fa2c5-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libstdc%2B%2B/libstdc%2B%2B-16.1.1%2Br12%2Bg301eb08fa2c5-1-x86_64.pkg.tar.zst", sha256: "5eb8ab787086682875805e7eaad8728e73bad687aba00c9460ecb261c3762aeb" },
  { name: "libgcc-16.1.1+r12+g301eb08fa2c5-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libgcc/libgcc-16.1.1%2Br12%2Bg301eb08fa2c5-1-x86_64.pkg.tar.zst", sha256: "01fbf0e50872c2f4538ea067f97b143124963e9037ca8eb69cdc8b693779c11b" },
  { name: "ca-certificates-mozilla-3.125-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/c/ca-certificates-mozilla/ca-certificates-mozilla-3.125-1-x86_64.pkg.tar.zst", sha256: "0fa76c249c0a6c28963f02ae366730a739121585aa1ffbb09b106b7a4fc8f358" },
  { name: "cacert-2025-02-25.pem", url: "https://curl.se/ca/cacert-2025-02-25.pem", sha256: "50a6277ec69113f00c5fd45f09e8b97a4b3e32daa35d3a95ab30137a55386cef" },
  { name: "sdl2-2.30.11-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/s/sdl2/sdl2-2.30.11-1-x86_64.pkg.tar.zst", sha256: "7f2b1abdd245c83585d2ccf69d86fc596b87c45c54d59984ab195c403d3ae41f" },
  { name: "libx11-1.8.9-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libx11/libx11-1.8.9-1-x86_64.pkg.tar.zst", sha256: "ad425488570b8701a9f0bbb606ec5ba94682648659238c21ca765b534a614add" },
  { name: "libxext-1.3.7-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libxext/libxext-1.3.7-1-x86_64.pkg.tar.zst", sha256: "ac56905dc51bb652eca5f706fd7e7bb7ea81d4e057a236139fc769ce5ea10cf1" },
  { name: "libxcursor-1.2.3-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libxcursor/libxcursor-1.2.3-1-x86_64.pkg.tar.zst", sha256: "2a90267877c2f5ffe6c41a8ef91e6def3b7720de7b4b2628b329373ce20c1b99" },
  { name: "libxrandr-1.5.5-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libxrandr/libxrandr-1.5.5-1-x86_64.pkg.tar.zst", sha256: "49d3a3596311477f8ad2e1735dae612013118802822f21ce22886f65103dd899" },
  { name: "libxrender-0.9.12-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libxrender/libxrender-0.9.12-1-x86_64.pkg.tar.zst", sha256: "fed0389073d5b107074eaab48cefcc2716e607865142cde5b579c8ceeefea142" },
  { name: "libxfixes-6.0.2-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libxfixes/libxfixes-6.0.2-1-x86_64.pkg.tar.zst", sha256: "d58ab2dbf326e36cf84792fe0dc12c34239ea04ac34159006a566103203db272" },
  { name: "libxi-1.8.3-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libxi/libxi-1.8.3-1-x86_64.pkg.tar.zst", sha256: "5ac5541f58978a3a5fab08c24f03da43200a7e9b25098e3e6da7bad7a0892cf0" },
  { name: "libxss-1.2.5-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libxss/libxss-1.2.5-1-x86_64.pkg.tar.zst", sha256: "79fd8fc47e77f479e6e4229a6957f26a6dd549bac7e047bafecdcaf5fde58889" },
  { name: "libxcb-1.17.0-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libxcb/libxcb-1.17.0-1-x86_64.pkg.tar.zst", sha256: "2b2e7ac64b1d56c08a227c10bcab179605f2773f31db0a8c89f49f4e5b2f1292" },
  { name: "libxau-1.0.12-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libxau/libxau-1.0.12-1-x86_64.pkg.tar.zst", sha256: "605c8b059c36792f4e0cc235acadf39d0762df6c7878825a1be01a00ae7ea21e" },
  { name: "libxdmcp-1.1.5-2-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libxdmcp/libxdmcp-1.1.5-2-x86_64.pkg.tar.zst", sha256: "623c957c2fd4b427a0f5a531da44931f9f66521391ee0bd0e635479947036b65" },
  { name: "libxkbcommon-1.11.0-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/l/libxkbcommon/libxkbcommon-1.11.0-1-x86_64.pkg.tar.zst", sha256: "6c7206057e97e08812c429106a7f639411d4919ba15c193f0582743dd5b7cfe6" },
  { name: "libxss-1.2.5-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/extra/libxss-1.2.5-1-aarch64.pkg.tar.xz", sha256: "1ebc34a29420166cb25040bfbff5d6ff732eff92de9cf07fcd92835e7d68bb9c" },
  { name: "libxkbcommon-1.13.2-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/extra/libxkbcommon-1.13.2-1-aarch64.pkg.tar.xz", sha256: "76b922d87d0af0011072b156464041ec5674f24ff1c78b7e85132fda72c9a7e7" },
  { name: "dbus-1.16.2-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/core/dbus-1.16.2-1-aarch64.pkg.tar.xz", sha256: "1aa0bc2be4fa083ec0cb678f05223f55b0c8e498351ad781dc4b7a0987b62bcc" },
  { name: "systemd-libs-261-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/core/systemd-libs-261-1-aarch64.pkg.tar.xz", sha256: "7e7c4c8c169caa36716c67868a1430cf0d346a7653588d4286f91621913ec452" },
  { name: "systemd-libs-261-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/s/systemd-libs/systemd-libs-261-1-x86_64.pkg.tar.zst", sha256: "78db8c587150fe1feb7ad7d3749e737c994a05c9cfcb597ca3d5cbb05fab014e" },
  { name: "util-linux-libs-2.42.2-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/u/util-linux-libs/util-linux-libs-2.42.2-1-x86_64.pkg.tar.zst", sha256: "50e5541bafc8e7013d1cfe7fe90cba2d5e96ac05acf3e8d1540f21daf24fb9c9" },
  { name: "vulkan-icd-loader-1.4.350.1-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/extra/vulkan-icd-loader-1.4.350.1-1-aarch64.pkg.tar.xz", sha256: "31142fb87d8c76233e35afc715afa17bcf89e4fe544dbb4d59c0b8e950640c3d" },
  { name: "vulkan-icd-loader-1.4.350.1-1-x86_64.pkg.tar.zst", url: "https://archive.archlinux.org/packages/v/vulkan-icd-loader/vulkan-icd-loader-1.4.350.1-1-x86_64.pkg.tar.zst", sha256: "e5a8c305ddb66582566d1974bae5f606478608cfe8986cc0ba1d46931d26d1f4" },
  { name: "libstdc++-16.1.1+r12+g301eb08fa2c5-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/core/libstdc%2B%2B-16.1.1%2Br12%2Bg301eb08fa2c5-1-aarch64.pkg.tar.xz", sha256: "19832a38b2c4820695d28289f1c4f371955586d39fb893d8cbc0d8dbb09a4383" },
  { name: "zlib-1:1.3.2-3-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/core/zlib-1%3A1.3.2-3-aarch64.pkg.tar.xz", sha256: "7e31b465e09e8e61375578b8e26ed883906e126e0c0ef1c75b7ccccfa95a58c4" },
  { name: "libdrm-2.4.134-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/extra/libdrm-2.4.134-1-aarch64.pkg.tar.xz", sha256: "d4173d7adc60d32d389e44e976447962c31bc23ff9a2153539bd8ad766e307b8" },
];
const EXPECTED_WINLATOR_PATCHES = [
  { path: "sysdeps/unix/sysv/linux/android_sysvshm.c", sha256: "3698e8e9cc8e00790f60ec16fbf88c88a78da933740e003677a6c37e509c71c2" },
  { path: "sysdeps/unix/sysv/linux/android_sysvshm.h", sha256: "031fa071ca44d1191dc22b4adbf351191bb292d718af0afd2caa9291aabf6550" },
  { path: "sysdeps/unix/sysv/linux/shmat.c", sha256: "e6b80913003e80ef4f900322398b682a76c1806dbd2f2da2a8f58a0759897f66" },
  { path: "sysdeps/unix/sysv/linux/shmctl.c", sha256: "fe72ee45e0c4cd6215f109c358bdbbfbeef22a6922ea0f91758226a671bc7d4b" },
  { path: "sysdeps/unix/sysv/linux/shmdt.c", sha256: "4e4362296e2e572e36e48617dce286639b3d4c2998afc7b8737a201b1e91069e" },
  { path: "sysdeps/unix/sysv/linux/shmget.c", sha256: "e59d1b5574e05cd6683202a6e0f61addea2eccca61ebad66682703a9cd477299" },
];
const REQUIRED_RUNTIME_PATHS = [
  "bin/hello",
  "bin/audio-tone",
  "bin/shadps4",
  "bin/sdl-window",
  "bin/vulkan-info",
  "etc/ssl/certs/ca-certificates.crt",
  "host/box64",
  "host/fexcore-smoke",
  "host/fexcore-guest-harness",
  "host/shadps4-arm64-fex",
  "host/ld-linux-aarch64.so.1",
  "host/libvulkan.so.1",
  "host/lib/libvulkan_vortek.so",
  "host/vulkan/icd.d/vortek.json",
  "usr/share/bachata/vortek/LICENSE",
  "usr/share/bachata/vortek/SOURCE.txt",
  "host/libc.so.6",
  "host/libdl.so.2",
  "host/libgcc_s.so.1",
  "host/libm.so.6",
  "host/libpthread.so.0",
  "host/libresolv.so.2",
  "host/libX11.so",
  "host/libX11.so.6",
  "host/libX11-xcb.so.1",
  "host/libXcursor.so.1",
  "host/libXext.so.6",
  "host/libXfixes.so.3",
  "host/libXi.so.6",
  "host/libXrandr.so.2",
  "host/libXss.so.1",
  "host/libxkbcommon.so.0",
  "host/libdbus-1.so.3",
  "host/libsystemd.so.0",
  "host/libXrender.so.1",
  "host/libXau.so.6",
  "host/libXdmcp.so.6",
  "host/libxcb.so",
  "host/libxcb.so.1",
  "lib/x86_64-linux-gnu/libc.so.6",
  "lib/x86_64-linux-gnu/libgcc_s.so.1",
  "lib/x86_64-linux-gnu/libm.so.6",
  "lib/x86_64-linux-gnu/libstdc++.so.6",
  "lib/x86_64-linux-gnu/libudev.so.1",
  "lib/x86_64-linux-gnu/libuuid.so.1",
  "lib/x86_64-linux-gnu/libSDL2-2.0.so.0",
  "lib/x86_64-linux-gnu/libX11.so.6",
  "lib/x86_64-linux-gnu/libX11-xcb.so.1",
  "lib/x86_64-linux-gnu/libXcursor.so.1",
  "lib/x86_64-linux-gnu/libXext.so.6",
  "lib/x86_64-linux-gnu/libXfixes.so.3",
  "lib/x86_64-linux-gnu/libXi.so.6",
  "lib/x86_64-linux-gnu/libXrandr.so.2",
  "lib/x86_64-linux-gnu/libXrender.so.1",
  "lib/x86_64-linux-gnu/libXss.so.1",
  "lib/x86_64-linux-gnu/libxcb.so.1",
  "lib/x86_64-linux-gnu/libXau.so.6",
  "lib/x86_64-linux-gnu/libXdmcp.so.6",
  "lib/x86_64-linux-gnu/libxkbcommon.so.0",
  "lib64/ld-linux-x86-64.so.2",
  "usr/share/bachata/shadps4-needed.txt",
  "usr/share/bachata/shadps4-arm64-fex-needed.txt",
];

function fail(message) {
  throw new Error(message);
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function countArm64SetRobustListCalls(bytes) {
  let count = 0;
  for (let offset = 0; offset + 4 <= bytes.length; offset += 4) {
    if (bytes.readUInt32LE(offset) !== 0xd2800c68) continue;
    for (let syscallOffset = offset + 4; syscallOffset <= offset + 32; syscallOffset += 4) {
      if (bytes.readUInt32LE(syscallOffset) === 0xd4000001) {
        count++;
        break;
      }
    }
  }
  return count;
}

function countArm64Clone3Calls(bytes) {
  let count = 0;
  for (let offset = 0; offset + 8 <= bytes.length; offset += 4) {
    if (bytes.readUInt32LE(offset) === 0xd2803668 && bytes.readUInt32LE(offset + 4) === 0xd4000001) count++;
  }
  return count;
}

function countArm64Faccessat2Calls(bytes) {
  let count = 0;
  for (let offset = 0; offset + 8 <= bytes.length; offset += 4) {
    if (bytes.readUInt32LE(offset) === 0xd28036e8 && bytes.readUInt32LE(offset + 4) === 0xd4000001) count++;
  }
  return count;
}

function parseStoredZip(bytes) {
  const eocdSignature = 0x06054b50;
  let eocd = -1;
  for (let offset = bytes.length - 22; offset >= Math.max(0, bytes.length - 65_557); offset--) {
    if (bytes.readUInt32LE(offset) === eocdSignature) {
      eocd = offset;
      break;
    }
  }
  if (eocd < 0) fail("ZIP end record missing");
  const entryCount = bytes.readUInt16LE(eocd + 10);
  const centralOffset = bytes.readUInt32LE(eocd + 16);
  const entries = [];
  let offset = centralOffset;
  for (let index = 0; index < entryCount; index++) {
    if (bytes.readUInt32LE(offset) !== 0x02014b50) fail("Invalid ZIP central directory");
    const method = bytes.readUInt16LE(offset + 10);
    const time = bytes.readUInt16LE(offset + 12);
    const date = bytes.readUInt16LE(offset + 14);
    const compressedSize = bytes.readUInt32LE(offset + 20);
    const size = bytes.readUInt32LE(offset + 24);
    const nameLength = bytes.readUInt16LE(offset + 28);
    const extraLength = bytes.readUInt16LE(offset + 30);
    const commentLength = bytes.readUInt16LE(offset + 32);
    const localOffset = bytes.readUInt32LE(offset + 42);
    const path = bytes.subarray(offset + 46, offset + 46 + nameLength).toString("utf8");
    if (method !== 0 || compressedSize !== size) fail(`ZIP entry must be stored: ${path}`);
    if (time !== 0 || date !== 0) fail(`ZIP timestamp is not zero: ${path}`);
    if (bytes.readUInt32LE(localOffset) !== 0x04034b50) fail(`Invalid local header: ${path}`);
    const localNameLength = bytes.readUInt16LE(localOffset + 26);
    const localExtraLength = bytes.readUInt16LE(localOffset + 28);
    const dataOffset = localOffset + 30 + localNameLength + localExtraLength;
    entries.push({ path, bytes: bytes.subarray(dataOffset, dataOffset + size) });
    offset += 46 + nameLength + extraLength + commentLength;
  }
  return entries;
}

const scriptDir = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(scriptDir, "../..");
const locksOnly = process.argv[2] === "--locks-only";
const lockPath = resolve(locksOnly ? resolve(projectRoot, "runtime/locks/components.lock.json") : (process.argv[2] ?? resolve(projectRoot, "runtime/locks/components.lock.json")));
const inputLockPath = resolve(projectRoot, "runtime/locks/runtime-inputs.lock.json");
const zipPath = resolve(process.argv[3] ?? resolve(projectRoot, "android/BachataS4/app/src/main/assets/runtime/runtime.zip"));
const manifestPath = resolve(process.argv[4] ?? resolve(projectRoot, "android/BachataS4/app/src/main/assets/runtime/manifest.json"));
const xServerOverridePath = resolve(projectRoot, "runtime/vendor-overrides/com/winlator/xserver/XServer.java");
const box64BuildScriptPath = resolve(projectRoot, "runtime/scripts/build-box64.sh");
const box64HostBuildScriptPath = resolve(projectRoot, "runtime/scripts/build-box64-host.sh");
const shadPs4BuildScriptPath = resolve(projectRoot, "runtime/scripts/build-shadps4-x86_64.sh");
const box64EntrypointPatchPath = resolve(projectRoot, "runtime/patches/box64-winlator-glibc-entrypoint.patch");
const box64QuickExitPatchPath = resolve(projectRoot, "runtime/patches/box64-cxa-quick-exit.patch");
const box64NativeWriteOpcodePatchPath = resolve(projectRoot, "runtime/patches/box64-native-write-opcode.patch");
const box64BachataThreadAffinityPatchPath = resolve(projectRoot, "runtime/patches/box64-bachata-thread-affinity.patch");
const nativeHostLoaderPath = resolve(projectRoot, "android/BachataS4/core/runtime/src/main/jniLibs/arm64-v8a/libbachata_host_loader.so");
const nativeHostBox64Path = resolve(projectRoot, "android/BachataS4/core/runtime/src/main/jniLibs/arm64-v8a/libbachata_host_box64.so");
const playStoreRuntimeDir = resolve(projectRoot, "android/BachataS4/app/src/playstore/assets/runtime");

if (existsSync(playStoreRuntimeDir)) {
  fail("Play Store flavor must use the generated main runtime assets, not a stale flavor override");
}

const lock = JSON.parse(readFileSync(lockPath, "utf8"));
if (readFileSync(xServerOverridePath, "utf8").includes("GLXExtension")) {
  fail("Winlator X server enables GLX without the gladiorenderer native library");
}
if (!readFileSync(box64BuildScriptPath, "utf8").includes("-DWINLATOR_GLIBC=ON")) {
  fail("Box64 must be built with Winlator glibc ABI wrappers");
}
if (readFileSync(shadPs4BuildScriptPath, "utf8").includes("-DENABLE_USERFAULTFD=ON")) {
  fail("shadPS4 managed runtime must keep mprotect write tracking");
}
if (!readFileSync(shadPs4BuildScriptPath, "utf8").includes("-DENABLE_USERFAULTFD=OFF")) {
  fail("shadPS4 build must override cached userfaultfd mode with OFF");
}
if (!readFileSync(box64HostBuildScriptPath, "utf8").includes("aarch64-linux-gnu-gcc")) {
  fail("Host Box64 must use the pinned AArch64 glibc cross-build path");
}
if (!readFileSync(box64HostBuildScriptPath, "utf8").includes("box64-cxa-quick-exit.patch")) {
  fail("Host Box64 build does not apply the quick-exit compatibility patch");
}
if (readFileSync(box64HostBuildScriptPath, "utf8").includes("box64-backing-dmem-clean-code.patch")) {
  fail("Host Box64 build must preserve shared-mapping invalidation semantics");
}
if (!readFileSync(box64HostBuildScriptPath, "utf8").includes("box64-bachata-thread-affinity.patch")) {
  fail("Host Box64 build does not apply the opt-in Bachata thread-affinity patch");
}
if (!readFileSync(box64QuickExitPatchPath, "utf8").includes("GOM(__cxa_at_quick_exit, iFEpp)")) {
  fail("Box64 quick-exit compatibility patch is invalid");
}
if (!readFileSync(box64EntrypointPatchPath, "utf8").includes("!defined(WINLATOR_GLIBC)")) {
  fail("Box64 Android entrypoint patch does not select glibc startup");
}
const nativeWriteOpcodePatch = readFileSync(box64NativeWriteOpcodePatchPath, "utf8");
for (const marker of [
  "0xd50b7a20u",
  "0xd50b7e20u",
  '"dc cvac, x7"',
  '"dc civac, x19"',
  '"dc isw, x0"',
]) {
  if (!nativeWriteOpcodePatch.includes(marker)) {
    fail(`Box64 native write classifier patch is missing ${marker}`);
  }
}
const bachataThreadAffinityPatch = readFileSync(box64BachataThreadAffinityPatchPath, "utf8");
for (const marker of [
  "BOX64_BACHATA_PRIME_THREAD",
  '"NexusRevolution"',
  "box64_bachata_highest_allowed_cpu",
  "test_bachata_thread_affinity.c",
]) {
  if (!bachataThreadAffinityPatch.includes(marker)) {
    fail(`Box64 Bachata thread-affinity patch is missing ${marker}`);
  }
}
if (lock.schemaVersion !== 1) fail("Lock schemaVersion must be 1");
if (JSON.stringify(lock.components) !== JSON.stringify(EXPECTED_COMPONENTS)) fail("Locked components differ from approved upstreams");
for (const component of lock.components) {
  if (!/^[0-9a-f]{40}$/.test(component.revision)) fail(`Invalid revision: ${component.name}`);
  if (!/^https:\/\//.test(component.url)) fail(`Invalid URL: ${component.name}`);
  if (!component.license) fail(`Missing license: ${component.name}`);
}
const inputLock = JSON.parse(readFileSync(inputLockPath, "utf8"));
if (inputLock.schemaVersion !== 1) fail("Input lock schemaVersion must be 1");
if (JSON.stringify(inputLock.inputs) !== JSON.stringify(EXPECTED_INPUTS)) fail("Runtime inputs differ from approved artifacts");
if (inputLock.winlatorRevision !== EXPECTED_COMPONENTS[2].revision) fail("Winlator patch revision mismatch");
if (JSON.stringify(inputLock.winlatorGlibcPatches) !== JSON.stringify(EXPECTED_WINLATOR_PATCHES)) fail("Winlator patch hashes differ from approved revision");
if (locksOnly) {
  console.log(`runtime locks verified: components=${lock.components.length} inputs=${inputLock.inputs.length}`);
  process.exit(0);
}

const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
if (manifest.schemaVersion !== 2 || manifest.protocolVersion !== 1 || manifest.distribution !== "debian") {
  fail("Invalid Debian runtime manifest schema/protocol");
}
if (!manifest.runtimeVersion) fail("Missing runtimeVersion");
const componentProvenance = lock.components.map(({ name, revision }) => ({ name, revision }));
if (JSON.stringify(manifest.components) !== JSON.stringify(componentProvenance)) fail("Manifest component provenance mismatch");
const zipEntries = parseStoredZip(readFileSync(zipPath));
const paths = zipEntries.map((entry) => entry.path);
if (new Set(paths).size !== paths.length) fail("Duplicate ZIP paths");
if (JSON.stringify(paths) !== JSON.stringify([...paths].sort())) fail("ZIP paths are not lexical");
for (const required of REQUIRED_RUNTIME_PATHS) {
  if (!paths.includes(required)) fail(`Required runtime file missing: ${required}`);
}
if (!Array.isArray(manifest.files) || manifest.files.length !== zipEntries.length) fail("Manifest file count mismatch");
for (let index = 0; index < zipEntries.length; index++) {
  const entry = zipEntries[index];
  const declared = manifest.files[index];
  if (declared.path !== entry.path) fail(`Manifest order/path mismatch: ${entry.path}`);
  if (declared.size !== entry.bytes.length) fail(`Manifest size mismatch: ${entry.path}`);
  if (declared.sha256 !== sha256(entry.bytes)) fail(`Manifest SHA-256 mismatch: ${entry.path}`);
}
const hostLoader = zipEntries.find((entry) => entry.path === "host/ld-linux-aarch64.so.1").bytes;
const hostLibc = zipEntries.find((entry) => entry.path === "host/libc.so.6").bytes;
const hostBox64 = zipEntries.find((entry) => entry.path === "host/box64").bytes;
const hostFexcoreSmoke = zipEntries.find((entry) => entry.path === "host/fexcore-smoke").bytes;
const hostFexcoreGuestHarness = zipEntries.find((entry) => entry.path === "host/fexcore-guest-harness").bytes;
const hostFexShadPs4 = zipEntries.find((entry) => entry.path === "host/shadps4-arm64-fex").bytes;
if (countArm64SetRobustListCalls(hostLoader) !== 0 || countArm64SetRobustListCalls(hostLibc) !== 0) {
  fail("Host glibc retains set_robust_list calls blocked by Android app seccomp");
}
if (countArm64Clone3Calls(hostLibc) !== 0) fail("Host glibc retains clone3 calls blocked by Android app seccomp");
if (countArm64Faccessat2Calls(hostLibc) !== 0) fail("Host glibc retains faccessat2 calls blocked by Android app seccomp");
if (!hostBox64.includes(Buffer.from("__cxa_at_quick_exit"))) fail("Host Box64 lacks __cxa_at_quick_exit wrapper");
if (hostFexcoreSmoke.length < 20 || hostFexcoreSmoke[0] !== 0x7f || hostFexcoreSmoke.subarray(1, 4).toString() !== "ELF") {
  fail("FEXCore smoke runner is not ELF");
}
if (hostFexcoreSmoke.readUInt16LE(18) !== 183) fail("FEXCore smoke runner is not AArch64 ELF");
for (const marker of [FEX_REVISION, "gpr=ok", "stack=ok", "fp=ok"]) {
  if (!hostFexcoreSmoke.includes(Buffer.from(marker))) fail(`FEXCore smoke runner lacks ${marker}`);
}
if (hostFexcoreGuestHarness.length < 20 || hostFexcoreGuestHarness[0] !== 0x7f || hostFexcoreGuestHarness.subarray(1, 4).toString() !== "ELF") {
  fail("FEXCore guest harness is not ELF");
}
if (hostFexcoreGuestHarness.readUInt16LE(18) !== 183) fail("FEXCore guest harness is not AArch64 ELF");
if (hostFexShadPs4.length < 20 || hostFexShadPs4[0] !== 0x7f ||
    hostFexShadPs4.subarray(1, 4).toString() !== "ELF" || hostFexShadPs4.readUInt16LE(18) !== 183) {
  fail("FEX shadPS4 is not AArch64 ELF");
}
for (const marker of [
  FEX_REVISION,
  "FEXCORE_GUEST_ENGINE_OK",
  "bridge=ok",
  "teardown=ok",
  "FEXCORE_GUEST_CPU_OK",
  "caller_mapping=ok",
  "thread_lifetime=ok",
  "thread_isolation=ok",
  "overlap_rejected=ok",
]) {
  if (!hostFexcoreGuestHarness.includes(Buffer.from(marker))) fail(`FEXCore guest harness lacks ${marker}`);
}
if (sha256(readFileSync(nativeHostLoaderPath)) !== sha256(hostLoader)) fail("Native host loader differs from runtime host loader");
if (sha256(readFileSync(nativeHostBox64Path)) !== sha256(hostBox64)) fail("Native host Box64 differs from runtime host Box64");
const hello = zipEntries.find((entry) => entry.path === "bin/hello").bytes;
if (hello.length < 20 || hello[0] !== 0x7f || hello.subarray(1, 4).toString() !== "ELF") fail("Probe is not ELF");
if (hello.readUInt16LE(18) !== 62) fail("Probe is not x86_64 ELF");
const shadps4 = zipEntries.find((entry) => entry.path === "bin/shadps4").bytes;
if (shadps4.length < 20 || shadps4[0] !== 0x7f || shadps4.subarray(1, 4).toString() !== "ELF") fail("shadPS4 is not ELF");
if (shadps4.readUInt16LE(18) !== 62) fail("shadPS4 is not x86_64 ELF");

console.log(`runtime verified: ${zipEntries.length} files, sha256=${sha256(readFileSync(zipPath))}`);
