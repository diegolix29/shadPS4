import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const mutexSource = readFileSync(
  resolve(root, "src/core/libraries/kernel/threads/mutex.cpp"),
  "utf8",
);

test("Pthread mutex destroyed slots are recreated on lock/try/timed-lock and remain strict EINVAL on unlock/destroy", () => {
  const checkMacroStart = mutexSource.indexOf("#define CHECK_AND_INIT_MUTEX");
  const checkMacroEnd = mutexSource.indexOf(
    "static constexpr PthreadMutexAttr PthreadMutexattrDefault",
  );
  assert.notEqual(checkMacroStart, -1, "CHECK_AND_INIT_MUTEX start marker found");
  assert.notEqual(checkMacroEnd, -1, "CHECK_AND_INIT_MUTEX end marker found");
  assert.ok(
    checkMacroStart < checkMacroEnd,
    "CHECK_AND_INIT_MUTEX start marker must precede end marker",
  );
  const checkMacro = mutexSource.slice(checkMacroStart, checkMacroEnd);

  const initStaticStart = mutexSource.indexOf("static s32 InitStatic(");
  const initStaticEnd = mutexSource.indexOf(
    "s32 PS4_SYSV_ABI posix_pthread_mutex_init",
  );
  assert.notEqual(initStaticStart, -1, "InitStatic start marker found");
  assert.notEqual(initStaticEnd, -1, "InitStatic end marker found");
  assert.ok(
    initStaticStart < initStaticEnd,
    "InitStatic start marker must precede end marker",
  );
  const initStaticFunc = mutexSource.slice(initStaticStart, initStaticEnd);

  const unlockStart = mutexSource.indexOf(
    "s32 PS4_SYSV_ABI posix_pthread_mutex_unlock(",
  );
  const unlockEnd = mutexSource.indexOf(
    "s32 PS4_SYSV_ABI posix_pthread_mutex_getspinloops_np",
  );
  assert.notEqual(unlockStart, -1, "posix_pthread_mutex_unlock start marker found");
  assert.notEqual(unlockEnd, -1, "posix_pthread_mutex_unlock end marker found");
  assert.ok(
    unlockStart < unlockEnd,
    "posix_pthread_mutex_unlock start marker must precede end marker",
  );
  const unlockFunc = mutexSource.slice(unlockStart, unlockEnd);

  const destroyStart = mutexSource.indexOf(
    "s32 PS4_SYSV_ABI posix_pthread_mutex_destroy(",
  );
  const destroyEnd = mutexSource.indexOf("s32 PthreadMutex::SelfTryLock()");
  assert.notEqual(destroyStart, -1, "posix_pthread_mutex_destroy start marker found");
  assert.notEqual(destroyEnd, -1, "posix_pthread_mutex_destroy end marker found");
  assert.ok(
    destroyStart < destroyEnd,
    "posix_pthread_mutex_destroy start marker must precede end marker",
  );
  const destroyFunc = mutexSource.slice(destroyStart, destroyEnd);

  // CHECK_AND_INIT_MUTEX must not immediately return POSIX_EINVAL for THR_MUTEX_DESTROYED
  assert.doesNotMatch(
    checkMacro,
    /m\s*==\s*THR_MUTEX_DESTROYED[\s\S]*?return\s+POSIX_EINVAL;/,
  );

  // InitStatic must handle THR_MUTEX_DESTROYED in the same branch as THR_MUTEX_INITIALIZER returning MutexInit
  assert.match(
    initStaticFunc,
    /(?:THR_MUTEX_INITIALIZER(?:(?!return)[\s\S])*?THR_MUTEX_DESTROYED|THR_MUTEX_DESTROYED(?:(?!return)[\s\S])*?THR_MUTEX_INITIALIZER)[\s\S]*?return\s+MutexInit\s*\(\s*mutex\s*,\s*&PthreadMutexattrDefault\s*,\s*nullptr\s*\);/,
  );

  // posix_pthread_mutex_unlock must remain strict EINVAL on THR_MUTEX_DESTROYED
  assert.match(
    unlockFunc,
    /mp\s*==\s*THR_MUTEX_DESTROYED[\s\S]*?return\s+POSIX_EINVAL;/,
  );

  // Repeated posix_pthread_mutex_destroy must remain strict EINVAL on THR_MUTEX_DESTROYED
  assert.match(
    destroyFunc,
    /m\s*==\s*THR_MUTEX_DESTROYED[\s\S]*?return\s+POSIX_EINVAL;/,
  );
});
