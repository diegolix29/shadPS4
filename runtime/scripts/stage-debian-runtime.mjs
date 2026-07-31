#!/usr/bin/env node

import { execFileSync } from "node:child_process";
import { chmodSync, copyFileSync, existsSync, lstatSync, mkdirSync, readlinkSync, realpathSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { basename, dirname, extname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

function fail(message) { throw new Error(message); }
function output(command, args) {
  return execFileSync(command, args, { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }).trim();
}
function run(cmd, args) { execFileSync(cmd, args, { stdio: "inherit" }); }

const scriptDir = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(scriptDir, "../..");
const rootfs = resolve(projectRoot, "runtime/build/rootfs");
const shadps4Stage = resolve(projectRoot, "runtime/build/shadps4-stage");
const box64Stage = resolve(projectRoot, "runtime/build/box64-host-stage");
const fexcoreSmokeStage = resolve(projectRoot, "runtime/build/fexcore-smoke-stage/bin/fexcore-smoke");
const fexcoreGuestHarnessStage = resolve(projectRoot, "runtime/build/fexcore-smoke-stage/bin/fexcore-guest-harness");

function isElf(file) {
  try { const fd = execFileSync("dd", ["if="+file, "bs=4", "count=1"], {stdio:["ignore","pipe","pipe"]}); return fd[0]==0x7f&&fd[1]==0x45&&fd[2]==0x4c&&fd[3]==0x46; } catch { return false; }
}
function elfNeeded(file) {
  try { return output("readelf", ["-d", realpathSync(file)]).split("\n").filter(l=>l.includes("Shared library:")).map(l=>l.match(/\[([^\]]+)\]/)?.[1]).filter(Boolean); } catch { return []; }
}
function elfInterp(file) {
  try { const raw = output("readelf", ["-l", realpathSync(file)]); const m=raw.match(/program interpreter:\s*([^\]]*)\]/); return m?basename(m[1]):null; } catch { return null; }
}
function findLib(soname, multiPaths) {
  for (const mp of multiPaths) { const p=join(mp,soname); try { return realpathSync(p); } catch {} }
  return null;
}
function pkgFor(file) {
  try {
    const raw = output("dpkg-query", ["-S", realpathSync(file)]).trim();
    // "libc6:arm64: /usr/lib/aarch64-linux-gnu/libc.so.6"
    const parts = raw.split(": ");
    if (parts.length >= 2) {
      // Rejoin package name with architecture
      const pkgPath = parts.slice(0, -1).join(": ");
      const lastPart = parts[parts.length-1];
      return pkgPath;
    }
    return raw.split(":")[0].trim();
  } catch { return null; }
}
function pkgBasename(pkgFull) {
  // "libc6:arm64" -> "libc6"
  return pkgFull.replace(/:.*$/, "");
}
function queryFormat(fmt, pkg) {
  try { return output("dpkg-query", ["-W", "-f", fmt, pkg]); } catch { return ""; }
}

const amd64Paths = ["/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu"];
const arm64Paths = ["/usr/lib/aarch64-linux-gnu", "/lib/aarch64-linux-gnu"];

function resolveClosure(roots, multiPaths) {
  const resolved = new Map();
  const queue = roots.map(f => realpathSync(f));
  const seen = new Set();
  while (queue.length) {
    const f = queue.shift();
    if (seen.has(f) || !isElf(f)) continue;
    seen.add(f);
    for (const s of elfNeeded(f)) {
      if (s==="linux-vdso.so.1") continue;
      if (resolved.has(s)) continue;
      const lib = findLib(s, multiPaths);
      if (!lib) { console.error(`WARN: unresolved ${s} for ${f}`); continue; }
      resolved.set(s, { path: lib, pkg: pkgFor(lib) });
      queue.push(lib);
    }
    const ip = elfInterp(f);
    if (ip && !resolved.has(ip)) {
      const lib = findLib(ip, multiPaths);
      if (lib) { resolved.set(ip, { path: lib, pkg: pkgFor(lib) }); queue.push(lib); }
    }
  }
  // Recurse to find transitive deps of resolved libs
  let changed = true;
  while (changed) {
    changed = false;
    for (const [soname, info] of resolved) {
      for (const dep of elfNeeded(info.path)) {
        if (dep==="linux-vdso.so.1") continue;
        if (resolved.has(dep)) continue;
        const lib = findLib(dep, multiPaths);
        if (!lib) { console.error(`WARN: unresolved transitive ${dep} for ${soname}`); continue; }
        resolved.set(dep, { path: lib, pkg: pkgFor(lib) });
        changed = true;
      }
    }
  }
  return resolved;
}

function addExtra(resolved, sonames, multiPaths) {
  for (const s of sonames) {
    if (!resolved.has(s)) {
      const lib = findLib(s, multiPaths);
      if (lib) resolved.set(s, { path: lib, pkg: pkgFor(lib) });
    }
  }
}

// -- Stage a library preserving symlink chain --
function stageWithSymlinks(source, destDir) {
  // source is the real file. We need to discover all symlinks in the chain.
  // Walk from each multiarch root through symlinks to the real file.
  const realFile = realpathSync(source);

  // Find the SONAME-level entry (the lib*.so.MAJOR symlink)
  // and the unversioned .so entry from multiarch paths
  const filesToCopy = [{ path: realFile, targetName: basename(realFile) }];

  // Find all symlinks pointing to this real file or its derivatives
  const allArm64 = execFileSync("find", arm64Paths.concat(["-maxdepth","1","-name","lib*.so*"]), {encoding:"utf8"}).split("\n").filter(Boolean);
  for (const link of allArm64) {
    if (!lstatSync(link).isSymbolicLink()) continue;
    const linkTarget = readlinkSync(link);
    let resolved;
    try { resolved = realpathSync(link); } catch { continue; }
    if (resolved === realFile) {
      filesToCopy.push({ path: link, targetName: basename(link), isSymlink: true, linkTarget });
    }
  }

  // Also search for symlinks in parent that point to sub-file
  const dirName = dirname(realFile);
  try {
    for (const entry of execFileSync("ls", ["-1", dirName], {encoding:"utf8"}).split("\n").filter(Boolean)) {
      const fullPath = join(dirName, entry);
      if (fullPath === realFile) continue;
      try {
        if (lstatSync(fullPath).isSymbolicLink()) {
          let resolved;
          try { resolved = realpathSync(fullPath); } catch { continue; }
          if (resolved === realFile) {
            filesToCopy.push({ path: fullPath, targetName: entry, isSymlink: true, linkTarget: readlinkSync(fullPath) });
          }
        }
      } catch {}
    }
  } catch {}

  // Deduplicate by targetName
  const seen = new Set();
  const unique = [];
  for (const f of filesToCopy) {
    if (!seen.has(f.targetName)) { seen.add(f.targetName); unique.push(f); }
  }

  // Stage: copy the real file, create relative symlinks
  const realTarget = join(destDir, basename(realFile));
  if (!existsSync(realTarget)) {
    copyFileSync(realFile, realTarget);
  }

  for (const f of unique) {
    if (f.isSymlink) {
      const linkPath = join(destDir, f.targetName);
      if (!existsSync(linkPath)) {
        try { symlinkSync(f.linkTarget, linkPath); } catch {}
      }
    }
  }
}

function copyTo(source, destDir, name) {
  const target = join(destDir, name);
  copyFileSync(realpathSync(source), target);
  return target;
}

// -- MAIN --
rmSync(rootfs, { recursive: true, force: true });

const guestBin = join(rootfs, "bin");
const guestLib = join(rootfs, "lib/x86_64-linux-gnu");
const guestLib64 = join(rootfs, "lib64");
const hostDir = join(rootfs, "host");
const provDir = join(rootfs, "usr/share/bachata");
for (const d of [guestBin, guestLib, guestLib64, hostDir, provDir]) mkdirSync(d, { recursive: true });

const provenance = {};
function recordPkg(pkgFull, arch, soname) {
  const key = pkgBasename(pkgFull);
  if (!provenance[key]) {
    provenance[key] = {
      binaryPackage: pkgBasename(pkgFull),
      architecture: arch,
      sourcePackage: queryFormat("${source:Package}", pkgBasename(pkgFull)),
      version: queryFormat("${Version}", pkgBasename(pkgFull)),
      files: [],
    };
  }
  if (!provenance[key].files.includes(soname)) provenance[key].files.push(soname);
}

// ---- amd64 guest ----
const shadps4Bin = join(shadps4Stage, "bin/shadps4");
if (!existsSync(shadps4Bin)) fail("shadPS4 not built. Run build-shadps4-x86_64.sh first.");
const amd64Resolved = resolveClosure([shadps4Bin], amd64Paths);
addExtra(amd64Resolved, [
  "libgcc_s.so.1", "libstdc++.so.6", "libm.so.6",
  "libSDL2-2.0.so.0", "libXss.so.1", "libxkbcommon.so.0",
  "libX11.so.6", "libXcursor.so.1", "libXext.so.6", "libXfixes.so.3",
  "libXi.so.6", "libXrandr.so.2", "libXrender.so.1", "libXau.so.6", "libXdmcp.so.6",
  "libxcb.so.1", "libX11-xcb.so.1", "libvulkan.so.1", "libudev.so.1", "libuuid.so.1",
], amd64Paths);

for (const [soname, info] of amd64Resolved) {
  const pkg = info.pkg || "unknown";
  if (soname === "ld-linux-x86-64.so.2") {
    copyTo(info.path, guestLib64, soname);
  } else {
    copyTo(info.path, guestLib, soname);
    stageWithSymlinks(info.path, guestLib);
  }
  recordPkg(pkg, "amd64", soname);
}

// ---- arm64 host ----
const box64Bin = join(box64Stage, "box64");
if (!existsSync(box64Bin)) fail("box64 not built. Run build-box64-host.sh first.");
if (!existsSync(fexcoreSmokeStage)) fail("FEXCore smoke not built. Run build-fexcore-smoke-aarch64.sh first.");
if (!existsSync(fexcoreGuestHarnessStage)) fail("FEXCore guest harness not built. Run build-fexcore-smoke-aarch64.sh first.");
const fexcoreSmokeTarget = join(hostDir, "fexcore-smoke");
copyFileSync(fexcoreSmokeStage, fexcoreSmokeTarget);
chmodSync(fexcoreSmokeTarget, 0o755);
const fexcoreGuestHarnessTarget = join(hostDir, "fexcore-guest-harness");
copyFileSync(fexcoreGuestHarnessStage, fexcoreGuestHarnessTarget);
chmodSync(fexcoreGuestHarnessTarget, 0o755);

// Resolve closure from box64 host binary
const arm64Resolved = resolveClosure([box64Bin], arm64Paths);

// Explicit parity libs — must be added regardless of box64 NEEDED
const arm64Explicit = [
  // Core C/C++ runtime parity
  "libgcc_s.so.1",
  "libstdc++.so.6",
  "libdl.so.2",
  "libpthread.so.0",
  "librt.so.1",
  "libutil.so.1",
  "libresolv.so.2",
  // X11/Vulkan host deps
  "libXss.so.1", "libxkbcommon.so.0", "libdbus-1.so.3", "libsystemd.so.0",
  "libX11.so.6", "libX11-xcb.so.1", "libXcursor.so.1", "libXext.so.6",
  "libXfixes.so.3", "libXi.so.6", "libXrandr.so.2", "libXrender.so.1",
  "libxcb.so.1", "libxcb-dri3.so.0", "libxcb-present.so.0", "libxcb-randr.so.0",
  "libxcb-render.so.0", "libxcb-shm.so.0", "libxcb-sync.so.1",
  "libXau.so.6", "libXdmcp.so.6", "libvulkan.so.1", "libz.so.1", "libdrm.so.2",
  "libudev.so.1", "libuuid.so.1", "libcap.so.2",
  // Data (not libs but needed)
  "libc.so.6", "libm.so.6",
];

addExtra(arm64Resolved, arm64Explicit, arm64Paths);

for (const [soname, info] of arm64Resolved) {
  const pkg = info.pkg || "unknown";
  if (soname === "ld-linux-aarch64.so.1") {
    stageWithSymlinks(info.path, hostDir);
  } else {
    copyTo(info.path, hostDir, soname);
    stageWithSymlinks(info.path, hostDir);
  }
  recordPkg(pkg, "arm64", soname);
}

// ---- XKB data ----
if (existsSync("/usr/share/X11/xkb")) {
  const xkbDest = join(rootfs, "usr/share/X11/xkb");
  // Remove symlink if it exists from previous operations
  try { rmSync(xkbDest, { recursive: true, force: true }); } catch {}
  mkdirSync(dirname(xkbDest), { recursive: true });
  execFileSync("cp", ["-a", "--dereference", "/usr/share/X11/xkb", xkbDest], { stdio: "inherit" });
  recordPkg("xkb-data", "all", "usr/share/X11/xkb");
}

// ---- CA certs ----
const caSrc = "/etc/ssl/certs/ca-certificates.crt";
if (existsSync(caSrc)) {
  const d = join(rootfs, "etc/ssl/certs"); mkdirSync(d, { recursive: true });
  copyFileSync(caSrc, join(d, "ca-certificates.crt"));
}

// ---- Provenance ----
writeFileSync(join(provDir, "debian-provenance.json"),
  JSON.stringify({ schemaVersion: 1, packages: Object.values(provenance) }, null, 2) + "\n");

// ---- Verification ----
const checks = [
  ["host/libgcc_s.so.1", "arm64 libgcc_s"],
  ["host/libstdc++.so.6", "arm64 libstdc++"],
  ["host/fexcore-smoke", "FEXCore smoke runner"],
  ["host/fexcore-guest-harness", "FEXCore guest harness"],
  ["host/libdl.so.2", "arm64 libdl"],
  ["host/libpthread.so.0", "arm64 libpthread"],
  ["host/libresolv.so.2", "arm64 libresolv"],
  ["host/ld-linux-aarch64.so.1", "arm64 loader"],
  ["host/libc.so.6", "arm64 libc"],
  ["host/libX11.so.6", "arm64 X11"],
  ["host/libvulkan.so.1", "arm64 vulkan"],
  ["host/libxkbcommon.so.0", "arm64 xkbcommon"],
  ["lib64/ld-linux-x86-64.so.2", "x86_64 loader"],
  ["lib/x86_64-linux-gnu/libc.so.6", "x86_64 libc"],
  ["lib/x86_64-linux-gnu/libSDL2-2.0.so.0", "x86_64 SDL2"],
  ["usr/share/X11/xkb", "XKB data"],
  ["etc/ssl/certs/ca-certificates.crt", "CA certs"],
];
let missing = 0;
for (const [p, label] of checks) {
  if (!existsSync(join(rootfs, p))) { console.error(`MISSING: ${label} (${p})`); missing++; }
}
if (missing) fail(`${missing} required files missing`);

const hc = execFileSync("ls", ["-1", hostDir], { encoding: "utf8" }).split("\n").filter(Boolean).length;
const gc = execFileSync("ls", ["-1", guestLib], { encoding: "utf8" }).split("\n").filter(Boolean).length;
const gbc = existsSync(join(rootfs, "lib64/ld-linux-x86-64.so.2")) ? 1 : 0;
console.log(`Staged: host=${hc} arm64 libs, guest=${gc}+${gbc} amd64 libs, packages=${Object.keys(provenance).length}`);
