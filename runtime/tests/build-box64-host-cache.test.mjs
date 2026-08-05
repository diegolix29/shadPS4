import assert from "node:assert/strict";
import {
  chmodSync,
  copyFileSync,
  mkdtempSync,
  mkdirSync,
  readFileSync,
  rmSync,
  unlinkSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const sourceScript = resolve(root, "runtime/scripts/build-box64-host.sh");
const revision = "50c8b90b09b433ab0767de44af2d0731cb0748b7";
const embeddedRevision = revision.slice(0, 8);
const sourceDateEpoch = "1772668800";
const patchNames = [
  "box64-cxa-quick-exit.patch",
  "box64-vulkan-dispatch-tile-qcom.patch",
  "box64-vex-write-opcode.patch",
  "box64-native-write-opcode.patch",
  "box64-bachata-thread-affinity.patch",
  "box64-reproducible-build-info.patch",
];

function executable(path, contents) {
  writeFileSync(path, `#!/usr/bin/env bash\nset -euo pipefail\n${contents}\n`);
  chmodSync(path, 0o755);
}

function fixture(t) {
  const project = mkdtempSync(join(tmpdir(), "bachata-box64-cache-"));
  t.after(() => rmSync(project, { recursive: true, force: true }));
  const scripts = join(project, "runtime/scripts");
  const patches = join(project, "runtime/patches");
  const source = join(project, "runtime/sources/box64");
  const tools = join(project, "tools");
  mkdirSync(scripts, { recursive: true });
  mkdirSync(patches, { recursive: true });
  mkdirSync(join(source, "tests"), { recursive: true });
  mkdirSync(tools);
  copyFileSync(sourceScript, join(scripts, "build-box64-host.sh"));
  chmodSync(join(scripts, "build-box64-host.sh"), 0o755);
  for (const name of patchNames) writeFileSync(join(patches, name), `${name}\n`);
  writeFileSync(join(source, "tests/test_bachata_thread_affinity.c"), "int main(void) { return 0; }\n");

  executable(join(tools, "git"), `
if [[ "$*" == *"rev-parse HEAD"* ]]; then printf '%s\\n' "${revision}"; exit 0; fi
if [[ "$*" == *"apply --reverse --check"* ]]; then exit 0; fi
if [[ "$*" == *"diff"* ]]; then exit 0; fi
if [[ "$*" == *"ls-files --others"* ]]; then exit 0; fi
printf 'unexpected git invocation: %s\\n' "$*" >&2
exit 70`);
  executable(join(tools, "cc"), `
out=""
while (($#)); do if [[ "$1" == -o ]]; then out="$2"; shift 2; else shift; fi; done
printf '#!/usr/bin/env bash\\nexit 0\\n' >"$out"
chmod +x "$out"`);
  executable(join(tools, "cmake"), `
printf '%s\\n' "$*" >>"$TEST_CMAKE_LOG"
if [[ "\${FAIL_CMAKE:-0}" == 1 ]]; then echo 'cmake must not run' >&2; exit 71; fi
if [[ "\${1:-}" == --build ]]; then
  mkdir -p "$2"
  printf 'mock-aarch64-box64\\n' >"$2/box64"
  chmod +x "$2/box64"
fi`);
  executable(join(tools, "readelf"), `
if [[ "$1" == -h ]]; then
  printf '  Class:                             ELF64\\n'
  if [[ "\${TEST_BAD_ABI:-0}" == 1 ]]; then
    printf '  Machine:                           Advanced Micro Devices X86-64\\n'
  else
    printf '  Machine:                           AArch64\\n'
  fi
elif [[ "$1" == -d ]]; then
  printf ' 0x0000000000000001 (NEEDED) Shared library: [libc.so.6]\\n'
fi`);
  executable(join(tools, "strings"), `
date -u -d "@\${TEST_SOURCE_DATE_EPOCH:-0}" '+Box64 arm64 v0.4.3 50c8b90b with Dynarec built on %b %e %Y %H:%M:%S'
`);
  for (const tool of ["ninja", "aarch64-linux-gnu-gcc", "aarch64-linux-gnu-g++"])
    executable(join(tools, tool), "exit 0");

  const cmakeLog = join(project, "cmake.log");
  const env = {
    ...process.env,
    PATH: `${tools}:${process.env.PATH}`,
    TEST_CMAKE_LOG: cmakeLog,
    SOURCE_DATE_EPOCH: sourceDateEpoch,
    TEST_SOURCE_DATE_EPOCH: sourceDateEpoch,
  };
  const script = join(scripts, "build-box64-host.sh");
  return {
    project,
    cmakeLog,
    binary: join(project, "runtime/build/box64-host-stage/box64"),
    manifest: join(project, "runtime/build/box64-host-stage/cache.manifest"),
    run(extraEnv = {}) {
      return spawnSync(script, { cwd: project, env: { ...env, ...extraEnv }, encoding: "utf8" });
    },
  };
}

test("normal mode builds Box64 and writes a verifiable cache manifest", (t) => {
  const f = fixture(t);
  const result = f.run();
  assert.equal(result.status, 0, result.stderr);
  assert.match(readFileSync(f.cmakeLog, "utf8"), /--build .*box64-host --target box64/);
  assert.match(readFileSync(f.cmakeLog, "utf8"), /-DBOX64_GITREV=50c8b90b/);
  assert.match(readFileSync(f.cmakeLog, "utf8"), /-DUSE_CCACHE=OFF/);
  const manifest = readFileSync(f.manifest, "utf8");
  assert.match(manifest, /^format=1$/m);
  assert.match(manifest, new RegExp(`^revision=${revision}$`, "m"));
  assert.match(manifest, new RegExp(`^embedded_git_revision=${embeddedRevision}$`, "m"));
  assert.match(manifest, new RegExp(`^source_date_epoch=${sourceDateEpoch}$`, "m"));
  assert.match(manifest, /^inputs_sha256=[0-9a-f]{64}$/m);
  assert.match(manifest, /^binary_sha256=[0-9a-f]{64}$/m);
  assert.match(manifest, /^compiler_identity=.*$/m);
  assert.match(manifest, /^compiler_version=.*$/m);
  assert.match(result.stdout, /box64_cache=rebuilt/);
});

test("normal mode passes version-banner reproducibility verification", (t) => {
  const f = fixture(t);
  const result = f.run();
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /box64_cache=rebuilt/);
});

test("skip mode reuses only a hash- and ABI-verified cached artifact", (t) => {
  const f = fixture(t);
  assert.equal(f.run().status, 0);
  writeFileSync(f.cmakeLog, "");
  const reused = f.run({ BACHATA_SKIP_BOX64_BUILD: "1", FAIL_CMAKE: "1" });
  assert.equal(reused.status, 0, reused.stderr);
  assert.equal(readFileSync(f.cmakeLog, "utf8"), "");
  assert.match(reused.stdout, /box64_cache=reused/);

  writeFileSync(f.binary, "corrupt\n");
  const corrupt = f.run({ BACHATA_SKIP_BOX64_BUILD: "1", FAIL_CMAKE: "1" });
  assert.notEqual(corrupt.status, 0);
  assert.match(corrupt.stderr, /cached Box64 artifact hash mismatch/i);
});

test("skip mode fails clearly for missing cache or invalid ABI", (t) => {
  const f = fixture(t);
  assert.equal(f.run().status, 0);
  const invalidAbi = f.run({ BACHATA_SKIP_BOX64_BUILD: "1", FAIL_CMAKE: "1", TEST_BAD_ABI: "1" });
  assert.notEqual(invalidAbi.status, 0);
  assert.match(invalidAbi.stderr, /cached Box64 artifact is not ELF64 AArch64/i);

  unlinkSync(f.binary);
  const missing = f.run({ BACHATA_SKIP_BOX64_BUILD: "1", FAIL_CMAKE: "1" });
  assert.notEqual(missing.status, 0);
  assert.match(missing.stderr, /cached Box64 artifact not found/i);
});

test("skip mode rejects a cached artifact built with a different SOURCE_DATE_EPOCH", (t) => {
  const f = fixture(t);
  assert.equal(f.run().status, 0);
  const stale = f.run({ BACHATA_SKIP_BOX64_BUILD: "1", FAIL_CMAKE: "1", SOURCE_DATE_EPOCH: "1" });
  assert.notEqual(stale.status, 0);
  assert.match(stale.stderr, /different SOURCE_DATE_EPOCH/i);
});

test("skip mode rejects a cached artifact with a stale embedded revision", (t) => {
  const f = fixture(t);
  assert.equal(f.run().status, 0);
  let manifest = readFileSync(f.manifest, "utf8");
  manifest = manifest.replace(/^embedded_git_revision=.*$/m, "embedded_git_revision=deadbeef");
  writeFileSync(f.manifest, manifest);
  const stale = f.run({ BACHATA_SKIP_BOX64_BUILD: "1", FAIL_CMAKE: "1" });
  assert.notEqual(stale.status, 0);
  assert.match(stale.stderr, /embedded revision mismatch/i);
});

test("runtime orchestrator continues ARM64, rootfs staging, and packaging after Box64", () => {
  const source = readFileSync(resolve(root, "runtime/scripts/build-runtime-debian.sh"), "utf8");
  const box64 = source.indexOf("runtime/scripts/build-box64-host.sh");
  const arm64 = source.indexOf("runtime/scripts/build-shadps4-arm64.sh");
  const rootfs = source.indexOf("runtime/scripts/stage-debian-runtime.mjs");
  const packageRuntime = source.indexOf("runtime/scripts/package-runtime.mjs");
  assert.ok(box64 >= 0 && box64 < arm64 && arm64 < rootfs && rootfs < packageRuntime);
});
