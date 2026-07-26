import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const source = readFileSync(resolve(root, "src/core/signals.cpp"), "utf8");

test("crash reporter is initialized before signal handlers can run", () => {
  const initCalls = source.match(/Common::InitCrashReporter\(\);/g) ?? [];

  assert.equal(initCalls.length, 1);
  assert.match(
    source,
    /SignalDispatch::SignalDispatch\(\)\s*\{\s*Common::InitCrashReporter\(\);/,
  );
  assert.doesNotMatch(source, /crashReporterOn/);
});
