import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

/**
 * Extract function/method body by matching braces starting from signature match.
 * `label` is included in assertion messages for clear identification.
 */
function extractFunctionBody(code, signaturePattern, label) {
  const match = code.match(signaturePattern);
  assert.ok(
    match,
    `Could not find pattern for ${label}: ${signaturePattern}`
  );
  const matchStart = match.index;
  const openBrace = code.indexOf("{", matchStart + match[0].length - 1);
  assert.notEqual(
    openBrace,
    -1,
    `Could not find opening brace for ${label}: ${signaturePattern}`
  );

  let depth = 0;
  for (let i = openBrace; i < code.length; i++) {
    if (code[i] === "{") depth++;
    else if (code[i] === "}") {
      depth--;
      if (depth === 0) {
        return code.slice(openBrace, i + 1);
      }
    }
  }
  assert.fail(
    `Unmatched braces starting at index ${openBrace} for ${label}: ${signaturePattern}`
  );
}

/**
 * Extract the Linux `#else` branch text from a function body that uses
 * #ifdef _WIN64 / #elif defined(__APPLE__) / #else / #endif platform guards.
 * Returns the text between the last `#else` and the closing `#endif` so that
 * Windows and Apple code cannot satisfy Linux-specific assertions.
 */
function extractLinuxElseBranch(body, label) {
  // Find the `#else` that is NOT immediately followed by ` defined(` (i.e. not
  // `#elif defined`) and that precedes a `#endif`.
  const elseMatch = body.match(/#else(?!\s*defined\b)[\s\S]*?#endif/);
  assert.ok(
    elseMatch,
    `Could not isolate Linux #else branch in ${label} body`
  );
  return elseMatch[0];
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 – guarded forward-declaration: exact namespace Core::Fex block, no
//           broad OR/fallback, and no direct fex_guest_engine.h include.
// ─────────────────────────────────────────────────────────────────────────────
test("Semaphore header uses guarded forward declaration and no fex_guest_engine.h include", () => {
  const semaphoreHeader = read("src/core/libraries/kernel/sync/semaphore.h");

  assert.equal(
    semaphoreHeader.includes("fex_guest_engine.h"),
    false,
    "semaphore.h must not include fex_guest_engine.h directly"
  );

  // Exact match: namespace Core::Fex { void FlushPendingGuestOrbisSignal() noexcept; }
  // No broad OR/fallback — only this precise forward-declaration form is accepted.
  const hasExactForwardDecl =
    /namespace\s+Core::Fex\s*\{\s*void\s+FlushPendingGuestOrbisSignal\(\)\s*noexcept;\s*\}/.test(
      semaphoreHeader
    );

  assert.ok(
    hasExactForwardDecl,
    "semaphore.h must declare FlushPendingGuestOrbisSignal via exact " +
      "`namespace Core::Fex { void FlushPendingGuestOrbisSignal() noexcept; }` forward declaration"
  );
});

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 – Linux acquire path: bounded 25ms slices, at least two
//           FlushPendingGuestOrbisSignal calls (failed-slice + post-success).
//           Only the Linux #else branch is inspected — Win/Apple cannot satisfy.
// ─────────────────────────────────────────────────────────────────────────────
test("Linux Semaphore acquire implementation uses bounded 25ms try_acquire_for slices and flushes signals", () => {
  const semaphoreHeader = read("src/core/libraries/kernel/sync/semaphore.h");

  // Extract acquire() body, clearly labelled for error messages.
  const acquireBody = extractFunctionBody(
    semaphoreHeader,
    /void\s+acquire\s*\(\s*\)/,
    "Semaphore::acquire"
  );

  // Isolate Linux #else branch so Windows/Apple code cannot satisfy assertions.
  const linuxAcquire = extractLinuxElseBranch(acquireBody, "Semaphore::acquire");

  // Linux acquire must loop with bounded 25ms try_acquire_for slices.
  assert.match(
    linuxAcquire,
    /std::chrono::milliseconds\s*\{\s*25\s*\}/,
    "Semaphore::acquire Linux #else branch must use bounded std::chrono::milliseconds{25} try_acquire_for slices"
  );

  // Must call FlushPendingGuestOrbisSignal at least twice in the Linux path:
  // once after a failed slice (loop body) and once after success.
  const flushCount = (linuxAcquire.match(/FlushPendingGuestOrbisSignal/g) || []).length;
  assert.ok(
    flushCount >= 2,
    `Semaphore::acquire Linux #else branch must call FlushPendingGuestOrbisSignal at least twice ` +
      `(failed-slice and post-success), found ${flushCount}`
  );
});

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 – Linux try_acquire_for path: deadline/remaining/std::min bounded
//           slices, at least two FlushPendingGuestOrbisSignal call sites.
//           Only the Linux #else branch is inspected.
// ─────────────────────────────────────────────────────────────────────────────
test("Semaphore try_acquire_for independently sliced uses deadline/remaining/std::min bounded slices plus flush", () => {
  const semaphoreHeader = read("src/core/libraries/kernel/sync/semaphore.h");

  // Extract template try_acquire_for body, clearly labelled.
  const tryAcquireForBody = extractFunctionBody(
    semaphoreHeader,
    /template\s*<\s*class\s+Rep\s*,\s*class\s+Period\s*>\s*bool\s+try_acquire_for\s*\(/,
    "Semaphore::try_acquire_for"
  );

  // Isolate Linux #else branch.
  const linuxTry = extractLinuxElseBranch(tryAcquireForBody, "Semaphore::try_acquire_for");

  // 25ms bounded slice.
  assert.match(
    linuxTry,
    /std::chrono::milliseconds\s*\{\s*25\s*\}/,
    "try_acquire_for Linux #else branch must bound slice durations to std::chrono::milliseconds{25}"
  );

  // All three of deadline, remaining, std::min must appear — anchored tightly.
  assert.match(
    linuxTry,
    /\bdeadline\b/,
    "try_acquire_for Linux #else branch must compute a deadline"
  );
  assert.match(
    linuxTry,
    /\bremaining\b/,
    "try_acquire_for Linux #else branch must compute remaining time"
  );
  assert.match(
    linuxTry,
    /\bstd::min\b/,
    "try_acquire_for Linux #else branch must use std::min to bound each slice"
  );

  // At least two FlushPendingGuestOrbisSignal call sites in the timed FEX path.
  const flushCount = (linuxTry.match(/FlushPendingGuestOrbisSignal/g) || []).length;
  assert.ok(
    flushCount >= 2,
    `try_acquire_for Linux #else branch must have at least two FlushPendingGuestOrbisSignal ` +
      `call sites (failed-slice and post-success structural positions), found ${flushCount}`
  );
});

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 – BridgeSyscallHandler::FlushPendingOrbisSignal member body must not
//           contain SRA spilling or HostPC RIP restoration.
//           Anchored specifically to the class member, not any later free wrapper.
// ─────────────────────────────────────────────────────────────────────────────
test("BridgeSyscallHandler::FlushPendingOrbisSignal contains no SRA spilling or HostPC RIP restoration", () => {
  const fexEngine = read("src/core/fex/fex_guest_engine.cpp");

  // Anchor to the BridgeSyscallHandler class member definition.
  // The class body contains `void FlushPendingOrbisSignal()` — we match the
  // member specifically by requiring `BridgeSyscallHandler` to appear before it
  // in the source. We find the member by locating the class definition first,
  // then extracting FlushPendingOrbisSignal from within that class span.
  const classMatch = fexEngine.match(
    /class\s+BridgeSyscallHandler\s+final\s*:[^{]*\{/
  );
  assert.ok(
    classMatch,
    "fex_guest_engine.cpp must define class BridgeSyscallHandler"
  );

  // Extract from the class opening brace onwards for member search.
  const classStart = classMatch.index + classMatch[0].length - 1; // position of '{'
  // Now extract the BridgeSyscallHandler class body.
  const classBody = extractFunctionBody(
    fexEngine.slice(classStart),
    /\{/,
    "BridgeSyscallHandler class body"
  );

  // Within the class body, find the FlushPendingOrbisSignal member.
  const flushBody = extractFunctionBody(
    classBody,
    /void\s+FlushPendingOrbisSignal\s*\(\s*\)/,
    "BridgeSyscallHandler::FlushPendingOrbisSignal"
  );

  assert.equal(
    flushBody.includes("RestoreRIPFromHostPC"),
    false,
    "BridgeSyscallHandler::FlushPendingOrbisSignal must not contain RestoreRIPFromHostPC"
  );

  assert.equal(
    flushBody.includes("has_host"),
    false,
    "BridgeSyscallHandler::FlushPendingOrbisSignal must not contain has_host"
  );

  assert.equal(
    flushBody.includes("HasHostSnapshot"),
    false,
    "BridgeSyscallHandler::FlushPendingOrbisSignal must not contain HasHostSnapshot"
  );

  assert.equal(
    flushBody.includes("spill_sra"),
    false,
    "BridgeSyscallHandler::FlushPendingOrbisSignal must not contain spill_sra"
  );
});

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 – DeliverGuestOrbisSignal classifies rawContext with
//           IsAddressInCodeBuffer and logs host_pc, host_sp, in_jit tokens
//           without host-PC diversion or RestoreRIPFromHostPC.
// ─────────────────────────────────────────────────────────────────────────────
test("DeliverGuestOrbisSignal classifies rawContext with IsAddressInCodeBuffer and logs host_pc, host_sp, in_jit diagnostic tokens", () => {
  const fexEngine = read("src/core/fex/fex_guest_engine.cpp");

  const deliverBody = extractFunctionBody(
    fexEngine,
    /bool\s+DeliverGuestOrbisSignal\s*\(/,
    "DeliverGuestOrbisSignal"
  );

  assert.match(
    deliverBody,
    /IsAddressInCodeBuffer/,
    "DeliverGuestOrbisSignal must classify host_pc using IsAddressInCodeBuffer"
  );

  assert.match(
    deliverBody,
    /host_pc/,
    "DeliverGuestOrbisSignal must inspect host_pc from rawContext"
  );

  assert.match(
    deliverBody,
    /host_sp/,
    "DeliverGuestOrbisSignal must inspect host_sp from rawContext"
  );

  assert.match(
    deliverBody,
    /in_jit/,
    "DeliverGuestOrbisSignal must include in_jit classification"
  );

  assert.match(
    deliverBody,
    /BACHATA_FEX_SIGNAL defer.*host_pc=.*host_sp=.*in_jit=/,
    "DeliverGuestOrbisSignal log must format host_pc, host_sp, and in_jit in defer diagnostic"
  );

  assert.equal(
    deliverBody.includes("RestoreRIPFromHostPC"),
    false,
    "DeliverGuestOrbisSignal must not perform RestoreRIPFromHostPC diversion"
  );
});

