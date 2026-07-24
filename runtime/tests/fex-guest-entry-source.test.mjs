import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("ARM64 production guest entries use the shared FEX runtime", () => {
  const linkerHeader = read("src/core/linker.h");
  const linker = read("src/core/linker.cpp");
  const module = read("src/core/module.cpp");
  const pthread = read("src/core/libraries/kernel/threads/pthread.cpp");
  const pthreadClean = read("src/core/libraries/kernel/threads/pthread_clean.cpp");
  const pthreadSpecific = read("src/core/libraries/kernel/threads/pthread_spec.cpp");
  const guestCallback = read("src/core/guest_cpu/guest_callback.h");
  const guestCpu = read("src/core/guest_cpu/guest_cpu.h");
  const fexCpu = read("src/core/guest_cpu/fex_guest_cpu.cpp");
  const engine = read("src/core/fex/fex_guest_engine.cpp");
  const harness = read("runtime/probes/fexcore-guest-harness.cpp");

  assert.match(linkerHeader, /RunGuestFunction/);
  assert.match(linkerHeader, /RunGuestMain/);
  assert.match(linkerHeader, /FexGuestCpuBackend/);
  assert.match(linkerHeader, /HleGuestBridge/);
  assert.match(linker, /HleGuestBridge/);
  assert.match(linker, /FexGuestCpuBackend::Create/);
  assert.match(linker, /QueryProtection/);
  assert.match(linker, /GetExecutableRanges/);
  assert.match(linker, /FexExecutableQueryContext/);
  assert.match(linker, /QueryExecutableRange/);
  assert.match(linkerHeader, /FexExecutableQueryContext/);

  const mainEntry = linker.slice(linker.indexOf("RunMainEntry"), linker.indexOf("Linker::Linker"));
  assert.match(mainEntry, /RunGuestMain/);
  assert.doesNotMatch(mainEntry, /RunMainEntry unimplemented/);

  assert.match(module, /RunGuestFunction/);
  assert.match(pthread, /RunGuestFunctionOrAbort/);
  assert.match(pthread, /ThreadDtors/);
  assert.match(pthread, /init_routine/);
  assert.match(pthreadClean, /RunGuestFunctionOrAbort/);
  assert.match(pthreadSpecific, /RunGuestFunctionOrAbort/);
  assert.match(guestCallback, /IsGuestFunctionAddress/);
  assert.match(guestCallback, /std::abort/);

  assert.match(guestCpu, /FsBase/);
  assert.ok(
    [...linker.matchAll(/request\.FsBase\s*=\s*reinterpret_cast<std::uintptr_t>\(GetTcbBase\(\)\)/g)]
      .length >= 2,
    "main and detached guest calls must expose the PS4 TCB through FS",
  );
  assert.doesNotMatch(linker, /request\.GsBase\s*=\s*reinterpret_cast<std::uintptr_t>\(GetTcbBase\(\)\)/);
  assert.match(fexCpu, /CallGuest/);
  assert.match(engine, /HandleCallback/);
  assert.match(engine, /ActiveThread/);
  assert.match(engine, /FunctionReturn/);
  assert.match(engine, /0x0f/);
  assert.match(engine, /0x3e/);
  assert.match(harness, /NestedCallbackBridge/);
  assert.match(harness, /nested_callback=ok/);
});
