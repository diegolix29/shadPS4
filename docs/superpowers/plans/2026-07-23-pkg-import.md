# PKG Import Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Import PS4 `.pkg` files into the Android library via native stream-extract (JNI), with progress stages in UI + notification, passcode dialog, and minimal KeyDB auto-save.

**Architecture:** Library picks Folder (existing SAF tree) or PKG (single document). `ImportService` opens a `ParcelFileDescriptor`, calls `PkgExtractor` (JNI → `libbachata_pkg.so`) to stream-decrypt/extract into `files/games/.import-<uuid>`, then finalizes with SFO metadata + atomic rename + `GameRepository`. Key resolution mirrors Pkg-Editor: zero passcode → saved passcode → embedded EKPFS → user passcode. No full package copy into app storage.

**Tech Stack:** Kotlin, Jetpack Compose, Hilt, Android Service/SAF, NDK C++17, CMake (existing `core/runtime` NDK), OpenSSL or Android `libcrypto` for AES if available — otherwise portable AES implementation ported from LibOrbisPkg logic, JUnit4.

**Spec:** `docs/superpowers/specs/2026-07-23-pkg-import-design.md`  
**Reference:** `/home/jica/repo/Pkg-Editor-2023` (`LibOrbisPkg`, `PkgEditor/Views/PkgView.cs` key order)

## Global Constraints

- Never copy the entire `.pkg` into app storage; read only via source FD.
- Free space preflight ≈ extracted size only when PFS/package size known.
- Folder import path remains functional and default-compatible.
- Notification channel stays `import` / `IMPORTANCE_LOW` (no importance setting).
- Do not log passcodes.
- On fail/cancel: delete staging under `files/games/.import-*`.
- ABI: `arm64-v8a` only (match `core/runtime` `ndk.abiFilters`).
- KeyDB v1 stores passcodes only at `filesDir/pkg_keydb.json`.
- Replace/update existing `games/<id>` is out of scope (fail “already imported”).
- Work in current worktree; commit per task.

## File map

| Path | Role |
|------|------|
| `android/.../data/ImportManager.kt` | Progress states, intent extras (`MODE`, `PASSCODE`) |
| `android/.../data/PkgKeyStore.kt` | JSON passcode vault |
| `android/.../data/ContentImporter.kt` | `finalizeStagingTree` for native extract output |
| `android/.../data/DataModule.kt` | Provide `PkgKeyStore` |
| `android/.../runtime/.../pkg/PkgExtractor.kt` | JNI facade |
| `android/.../runtime/src/main/cpp/pkg/*` | Native extract (header, crypto, PFS, JNI) |
| `android/.../runtime/src/main/cpp/CMakeLists.txt` | Build `bachata_pkg` |
| `android/.../service/ImportService.kt` | PKG branch + stages + notif text |
| `android/.../feature/library/LibraryScreen.kt` | Folder/PKG chooser, passcode dialog, new stages UI |
| `android/.../feature/settings/SettingsScreen.kt` | Clear saved PKG keys |
| Tests under matching `src/test` trees | Unit coverage |

---

### Task 1: Extend `ImportProgress` + intent constants

**Files:**
- Modify: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/ImportManager.kt`
- Modify: `android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/ImportManagerTest.kt`

**Interfaces:**
- Produces:
  - `ImportProgress.Extracting(bytesCopied, totalBytes, currentFile, gameTitle)`
  - `ImportProgress.Finalizing(title: String)`
  - `ImportProgress.NeedPasscode(contentId: String, titleHint: String?)`
  - `ImportManager.EXTRA_MODE` (`"folder"` | `"pkg"`), `ImportManager.MODE_FOLDER`, `ImportManager.MODE_PKG`
  - `ImportManager.EXTRA_PASSCODE`
  - `ImportManager.ACTION_SUBMIT_PASSCODE`
  - `isBusy` true for `Extracting`, `Finalizing`, `NeedPasscode`

- [ ] **Step 1: Write failing tests for new busy states**

Add to `ImportManagerTest.kt`:

```kotlin
@Test
fun extractingAndFinalizingAreBusy() {
    ImportManager.update(
        ImportProgress.Extracting(0L, 100L, "eboot.bin", "Game"),
    )
    assertTrue(ImportManager.isBusy())
    assertFalse(ImportManager.tryBeginImport())

    ImportManager.update(ImportProgress.Finalizing("Game"))
    assertTrue(ImportManager.isBusy())
    assertFalse(ImportManager.tryBeginImport())
}

@Test
fun needPasscodeIsBusyAndBlocksNewImport() {
    ImportManager.update(ImportProgress.NeedPasscode("EP0001-CUSA00000_00-TEST000000000000", "Hint"))
    assertTrue(ImportManager.isBusy())
    assertFalse(ImportManager.tryBeginImport())
}
```

- [ ] **Step 2: Run tests — expect compile/fail**

```bash
cd android/BachataS4 && ./gradlew :core:data:testDebugUnitTest --tests 'com.bachatas4.android.data.ImportManagerTest'
```

Expected: compile error (types missing) or FAIL.

- [ ] **Step 3: Implement progress + constants**

Update `ImportManager.kt` sealed hierarchy and constants:

```kotlin
sealed interface ImportProgress {
    data object Idle : ImportProgress
    data object Preparing : ImportProgress
    data class Scanning(val folderName: String) : ImportProgress
    data class Extracting(
        val bytesCopied: Long,
        val totalBytes: Long,
        val currentFile: String,
        val gameTitle: String,
    ) : ImportProgress
    data class Copying(
        val bytesCopied: Long,
        val totalBytes: Long,
        val currentFile: String,
        val gameTitle: String,
    ) : ImportProgress
    data class Finalizing(val title: String) : ImportProgress
    data class NeedPasscode(val contentId: String, val titleHint: String?) : ImportProgress
    data class Success(val gameId: String, val title: String) : ImportProgress
    data class Failed(val message: String) : ImportProgress
}

object ImportManager {
    const val ACTION_IMPORT = "com.bachatas4.android.action.IMPORT_GAME"
    const val ACTION_CANCEL = "com.bachatas4.android.action.CANCEL_IMPORT"
    const val ACTION_SUBMIT_PASSCODE = "com.bachatas4.android.action.SUBMIT_PASSCODE"
    const val EXTRA_URI = "source_uri"
    const val EXTRA_MODE = "import_mode"
    const val EXTRA_PASSCODE = "passcode"
    const val MODE_FOLDER = "folder"
    const val MODE_PKG = "pkg"
    const val SERVICE_CLASS = "com.bachatas4.android.service.ImportService"
    // ...
    fun isBusy(state: ImportProgress = _progress.value): Boolean =
        state is ImportProgress.Preparing ||
            state is ImportProgress.Scanning ||
            state is ImportProgress.Extracting ||
            state is ImportProgress.Copying ||
            state is ImportProgress.Finalizing ||
            state is ImportProgress.NeedPasscode
}
```

Keep existing `tryBeginImport` / `update` / `reset`.

- [ ] **Step 4: Re-run tests — expect PASS**

```bash
cd android/BachataS4 && ./gradlew :core:data:testDebugUnitTest --tests 'com.bachatas4.android.data.ImportManagerTest'
```

- [ ] **Step 5: Commit**

```bash
git add android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/ImportManager.kt \
  android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/ImportManagerTest.kt
git commit -m "feat(import): extend progress states for PKG extract"
```

---

### Task 2: `PkgKeyStore` (passcode vault)

**Files:**
- Create: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/PkgKeyStore.kt`
- Create: `android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/PkgKeyStoreTest.kt`
- Modify: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/DataModule.kt`

**Interfaces:**
- Produces:

```kotlin
class PkgKeyStore(private val filesDir: File) {
    fun getPasscode(contentId: String): String?
    fun putPasscode(contentId: String, passcode: String)
    fun clear()
}
```

- File: `File(filesDir, "pkg_keydb.json")`
- JSON: `{"passcodes":{"<content_id>":"<32 chars>"}}` via `org.json.JSONObject` (no new deps) or kotlinx.serialization if already on classpath in `core:data`.

- [ ] **Step 1: Failing tests**

```kotlin
class PkgKeyStoreTest {
    private lateinit var dir: File
    private lateinit var store: PkgKeyStore

    @Before
    fun setUp() {
        dir = createTempDir(prefix = "pkgkey")
        store = PkgKeyStore(dir)
    }

    @After
    fun tearDown() {
        dir.deleteRecursively()
    }

    @Test
    fun putAndGetRoundTrip() {
        assertNull(store.getPasscode("EP0001-CUSA00000_00-TEST000000000000"))
        store.putPasscode("EP0001-CUSA00000_00-TEST000000000000", "0123456789abcdef0123456789abcdef")
        assertEquals(
            "0123456789abcdef0123456789abcdef",
            store.getPasscode("EP0001-CUSA00000_00-TEST000000000000"),
        )
        // reload from disk
        val reloaded = PkgKeyStore(dir)
        assertEquals(
            "0123456789abcdef0123456789abcdef",
            reloaded.getPasscode("EP0001-CUSA00000_00-TEST000000000000"),
        )
    }

    @Test
    fun clearWipesFile() {
        store.putPasscode("EP0001-CUSA00000_00-TEST000000000000", "0123456789abcdef0123456789abcdef")
        store.clear()
        assertNull(store.getPasscode("EP0001-CUSA00000_00-TEST000000000000"))
        assertFalse(File(dir, "pkg_keydb.json").exists())
    }
}
```

- [ ] **Step 2: Run — expect fail**

```bash
cd android/BachataS4 && ./gradlew :core:data:testDebugUnitTest --tests 'com.bachatas4.android.data.PkgKeyStoreTest'
```

- [ ] **Step 3: Implement `PkgKeyStore` + Hilt provide**

```kotlin
// PkgKeyStore.kt — synchronized file R/W, create parent, atomic write via temp+rename
@Provides @Singleton
fun pkgKeyStore(@ApplicationContext context: Context): PkgKeyStore =
    PkgKeyStore(context.filesDir)
```

Reject empty `contentId`. Do not validate passcode length here (native/service validates).

- [ ] **Step 4: Tests PASS**

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(import): add PkgKeyStore passcode vault"
```

---

### Task 3: Finalize staging tree after native extract

**Files:**
- Modify: `android/BachataS4/core/data/src/main/kotlin/com/bachatas4/android/data/ContentImporter.kt`
- Modify: `android/BachataS4/core/data/src/test/kotlin/com/bachatas4/android/data/ContentImporterTest.kt`

**Interfaces:**
- Produces:

```kotlin
suspend fun finalizeStagingTree(
    request: ContentImportRequest,
    stagingDir: File,
): ContentImportResult
```

Behavior:
1. `validateGameId(request.id)`
2. `destination = files/games/<id>`; fail if exists
3. Require `stagingDir` inside `files/games` and is a directory
4. Require `File(stagingDir, "sce_sys/param.sfo").isFile` (hard fail if missing)
5. Prefer also requiring `eboot.bin` at staging root **or** document if some titles use only `sce_sys` first — match folder import expectations: at least `param.sfo`; if `eboot.bin` missing, still fail with `"invalid game layout: missing eboot.bin"` unless existing folder import allows absence (it copies whatever tree has — for PKG, require `eboot.bin` OR any `*.bin` at root used by runtime — **require `eboot.bin`** for v1)
6. Space check: if staging size known, ensure destination FS has room for rename (usually same FS → free)
7. `moveAtomically(stagingDir, destination)`
8. Return `ContentImportResult` with `bytesCopied = staging size sum`, `sha256 = ""` or hash of `param.sfo` only (folder path already computes full tree digest — for finalize, compute digest of all files if cheap enough; for 30GB skip full hash: use `sha256 = "pkg-extract"` sentinel **or** hash `param.sfo` only and set `bytesCopied` from walk). Spec does not require content hash for PKG. Use:

```kotlin
bytesCopied = walkFileSize(stagingDir)
sha256 = "pkg-extract"
```

- [ ] **Step 1: Failing test** — create temp games dir layout with fake `param.sfo` + `eboot.bin`, call finalize, assert destination exists and staging gone.

- [ ] **Step 2: Implement `finalizeStagingTree` reusing `requireInside`, `moveAtomically`, `validateGameId`.**

- [ ] **Step 3: Test PASS + commit**

```bash
git commit -m "feat(import): finalize native PKG staging into games dir"
```

---

### Task 4: Native library scaffold + Kotlin `PkgExtractor` stubs

**Files:**
- Create: `android/BachataS4/core/runtime/src/main/cpp/pkg/CMakeLists.txt`
- Create: `android/BachataS4/core/runtime/src/main/cpp/pkg/bachata_pkg_jni.cpp`
- Create: `android/BachataS4/core/runtime/src/main/cpp/pkg/pkg_extractor.h`
- Create: `android/BachataS4/core/runtime/src/main/cpp/pkg/pkg_extractor.cpp` (stubs returning errors)
- Modify: `android/BachataS4/core/runtime/src/main/cpp/CMakeLists.txt` — `add_subdirectory(pkg)`
- Create: `android/BachataS4/core/runtime/src/main/kotlin/com/bachatas4/android/runtime/pkg/PkgExtractor.kt`
- Create: `android/BachataS4/core/runtime/src/test/kotlin/com/bachatas4/android/runtime/pkg/PkgExtractorContractTest.kt` (pure Kotlin data class / result enum tests if native not loadable on JVM)

**Interfaces:**

```kotlin
// PkgExtractor.kt
package com.bachatas4.android.runtime.pkg

data class PkgProbeResult(
    val contentId: String,
    val packageSize: Long,
    val pfsImageSize: Long,
    val titleHint: String?,
    val status: PkgStatus,
    val message: String? = null,
)

enum class PkgStatus {
    OK,
    NEED_PASSCODE,
    CANCELLED,
    ERROR,
}

data class PkgExtractResult(
    val status: PkgStatus,
    val message: String? = null,
    val contentId: String? = null,
)

fun interface PkgProgressListener {
    fun onProgress(bytesDone: Long, totalHint: Long, currentFile: String)
}

object PkgExtractor {
    init {
        System.loadLibrary("bachata_pkg")
    }

    external fun nativeProbe(fd: Int): PkgProbeResult
    external fun nativeExtract(
        fd: Int,
        outPath: String,
        passcode: String?,
        listener: PkgProgressListener?,
    ): PkgExtractResult
    external fun nativeCancel()
}
```

C API (internal):

```cpp
// pkg_extractor.h
struct BachataPkgProbe {
  char content_id[0x30];
  uint64_t package_size;
  uint64_t pfs_image_size;
  char title_hint[0x80];
  int status; // 0 OK, 1 NEED_PASSCODE, 2 CANCELLED, 3 ERROR
  char message[256];
};

int bachata_pkg_probe(int fd, BachataPkgProbe* out);
int bachata_pkg_extract(
    int fd,
    const char* out_path,
    const char* passcode_or_null,
    void (*progress)(void* ctx, uint64_t done, uint64_t total, const char* file),
    void* progress_ctx);
void bachata_pkg_cancel(void);
```

JNI maps `fd` from `ParcelFileDescriptor.detachFd()` **or** pass `pfd.fd` without detach if service keeps PFD open for lifetime of call (prefer keep PFD open; pass `pfd.getFd()` and do not close in native).

- [ ] **Step 1: Wire CMake**

```cmake
# cpp/CMakeLists.txt add:
add_subdirectory(pkg)

# pkg/CMakeLists.txt
add_library(bachata_pkg SHARED
    bachata_pkg_jni.cpp
    pkg_extractor.cpp
    # later: pkg_crypto.cpp pkg_reader.cpp pfs_reader.cpp
)
target_include_directories(bachata_pkg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bachata_pkg android log)
target_compile_features(bachata_pkg PRIVATE cxx_std_17)
```

- [ ] **Step 2: Stub extract returns ERROR `"not implemented"`; probe same.**

- [ ] **Step 3: Build native**

```bash
cd android/BachataS4 && ./gradlew :core:runtime:assembleDebug
```

Expected: `libbachata_pkg.so` in intermediates.

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(import): scaffold bachata_pkg JNI extractor"
```

---

### Task 5: Native crypto + PKG header probe

**Files:**
- Create/Modify under `android/BachataS4/core/runtime/src/main/cpp/pkg/`:
  - `pkg_crypto.cpp` / `.h` — port `LibOrbisPkg/Util/Crypto.cs` (`ComputeKeys`, PFS key expand) + AES-XTS from `XtsBlockTransform.cs`
  - `pkg_format.h` — header structs from `LibOrbisPkg/PKG/Pkg.cs` / `Enums.cs` (endian: little-endian fields as in format)
  - `pkg_reader.cpp` — read header at offset 0; content_id; `pfs_image_offset` / `pfs_image_size`; package size
- Reference only (do not vendor C#): `/home/jica/repo/Pkg-Editor-2023/LibOrbisPkg/...`

**Probe behavior (`bachata_pkg_probe`):**
1. `pread` magic / header; validate PKG magic.
2. Fill `content_id`, sizes.
3. Attempt key material without user passcode:
   - zero passcode → derive EKPFS → validate (digest check like `CheckPasscode`)
   - else try entry-based `GetEkpfs` if present in package entries
   - if neither works → `status = NEED_PASSCODE` (still return content_id/sizes)
   - if works → `status = OK`
4. Optional title hint from PARAM_SFO entry if readable without full PFS (entry table); else empty.

**Crypto tests (host optional):** If host gtest not wired, add a tiny self-check function `#ifdef BACHATA_PKG_SELFTEST` or document manual vector:

- Known zero-passcode fake PKG (add under `android/BachataS4/core/runtime/src/test/resources/` later in Task 6).

- [ ] **Step 1: Implement AES-ECB/XTS + `ComputeKeys(content_id, passcode, index)` matching LibOrbisPkg.**

- [ ] **Step 2: Implement header parse + probe.**

- [ ] **Step 3: Unit-test on device or with a checked-in minimal fixture when available; at minimum compile + call probe on invalid FD returns ERROR.**

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(import): native PKG probe and crypto"
```

---

### Task 6: Native PFS extract stream-to-directory

**Files:**
- Create: `pfs_reader.cpp` / `.h` — port essential path from `LibOrbisPkg/PFS/PfsReader.cs`, `XtsDecryptReader.cs`, `PFSCReader.cs` as needed
- Modify: `pkg_extractor.cpp` — full `bachata_pkg_extract`

**Extract algorithm (PkgView-aligned):**
1. Resolve keys: passcode arg → else zero → else fail NEED_PASSCODE (saved passcode applied in Kotlin before call).
2. Map outer PFS at `pfs_image_offset` for `pfs_image_size` via `pread` (do not load entire image into RAM).
3. Open outer PFS with EKPFS/XTS; locate inner encrypted PFS / image used for game files (same as LibOrbisPkg FileView extract of `pfs_image`).
4. Walk file inodes; for each file:
   - Build relative path; reject `..` and absolute
   - `mkdir -p` parent under `out_path`
   - stream decrypt/write in ≥1 MiB buffers
   - call `progress(done, total, relative_path)`
   - if cancel flag set → return CANCELLED and leave cleanup to Kotlin
5. Success only if at least `sce_sys/param.sfo` written.

**Cancel:** atomic flag set by `bachata_pkg_cancel`; check each file/chunk.

**Memory:** never `malloc(pfs_image_size)` for multi-GB images.

- [ ] **Step 1: Implement outer/inner PFS read + file extract.**

- [ ] **Step 2: Manual smoke with a small homebrew/fake PKG on device when available.**

- [ ] **Step 3: Commit**

```bash
git commit -m "feat(import): native PFS stream extract to staging"
```

---

### Task 7: `ImportService` PKG branch

**Files:**
- Modify: `android/BachataS4/app/src/main/kotlin/com/bachatas4/android/service/ImportService.kt`
- Ensure `app` depends on `core:runtime` (already true if runtime used elsewhere)

**Interfaces:**
- Consumes: `PkgExtractor`, `PkgKeyStore`, `ContentImporter.finalizeStagingTree`, `GameRepository`, `ParamSfoReader`, `GameMetadataResolver`
- Intent:
  - `ACTION_IMPORT` + `EXTRA_MODE=pkg` + `EXTRA_URI`
  - `ACTION_SUBMIT_PASSCODE` + `EXTRA_PASSCODE` (only valid while `NeedPasscode`)
  - `ACTION_CANCEL` → `PkgExtractor.nativeCancel()` + cancel job

**Flow (`runPkgImport`):**

```kotlin
// pseudocode-level steps to implement in full Kotlin
ImportManager already Preparing (tryBeginImport in onStartCommand)
takePersistableUriPermission if possible (OpenDocument may not always grant persistable — use FLAG_GRANT_READ_URI_PERMISSION; persistable optional)
open FileDescriptor via contentResolver.openFileDescriptor(uri, "r")
probe = PkgExtractor.nativeProbe(pfd.fd)
update Scanning(probe.contentId or display name)
// build passcode candidate list:
// 1) intent EXTRA_PASSCODE if present
// 2) zero
// 3) keyStore.getPasscode(contentId)
// try extract with each until OK or NEED_PASSCODE/ERROR
// on NEED_PASSCODE after all auto attempts:
//   ImportManager.update(NeedPasscode(...)); updateNotification("Passcode required"); return WITHOUT stopSelf if waiting
//   — OR stop and let SUBMIT_PASSCODE restart: simpler = keep service alive with job suspended on CompletableDeferred

Preferred: use CompletableDeferred<String?> passcodeWaiter on service instance.
NeedPasscode → waiter; SUBMIT_PASSCODE completes waiter; CANCEL completes null.

staging = File(filesDir, "games/.import-${UUID}")
extract with progress → ImportProgress.Extracting + notification "Extracting PKG · …"
Finalizing → read param.sfo from staging, resolve metadata
if destination exists → fail already imported
finalizeStagingTree(...)
if passcode used (non-zero) → keyStore.putPasscode
addImportedGame
Success
```

Inject:

```kotlin
@Inject lateinit var pkgKeyStore: PkgKeyStore
// PkgExtractor is object with loadLibrary — no inject required
```

Folder branch: set default mode folder when `EXTRA_MODE` missing.

- [ ] **Step 1: Implement mode switch in `onStartCommand`.**

- [ ] **Step 2: Implement passcode waiter + PKG extract path + staging cleanup in `finally` if not completed.**

- [ ] **Step 3: Map all stages to `updateNotification` strings matching design.**

- [ ] **Step 4: Compile**

```bash
cd android/BachataS4 && ./gradlew :app:compileDebugKotlin
```

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(import): ImportService native PKG extract path"
```

---

### Task 8: Library UI — dual import + passcode + stages

**Files:**
- Modify: `android/BachataS4/feature/library/src/main/kotlin/com/bachatas4/android/feature/library/LibraryScreen.kt`

**Behavior:**
1. `requestImport` opens chooser (`AlertDialog` / bottom sheet): **Import folder** | **Import PKG**.
2. Folder → existing `OpenDocumentTree` launcher (add `EXTRA_MODE=folder`).
3. PKG → `OpenDocument` launcher `arrayOf("*/*")`; on result validate name ends with `.pkg` (case-insensitive) else Toast error.
4. Start service with `EXTRA_MODE=pkg` + URI + read permission.
5. `when (importProgress)` add branches:
   - `Extracting` — same card as Copying but title `Extracting ${gameTitle}`
   - `Finalizing` — indeterminate “Registering…”
   - `NeedPasscode` — show `AlertDialog` with `OutlinedTextField`, Submit sends `ACTION_SUBMIT_PASSCODE`, Cancel sends `ACTION_CANCEL`
6. `isImporting` / busy includes new states (uses `ImportManager.isBusy()`).

Example start PKG:

```kotlin
val intent = Intent(ImportManager.ACTION_IMPORT).apply {
    setClassName(context.packageName, ImportManager.SERVICE_CLASS)
    putExtra(ImportManager.EXTRA_URI, uri.toString())
    putExtra(ImportManager.EXTRA_MODE, ImportManager.MODE_PKG)
}
context.startService(intent)
```

- [ ] **Step 1: Wire launchers + dialog + progress UI.**

- [ ] **Step 2: Compile library/app modules.**

- [ ] **Step 3: Commit**

```bash
git commit -m "feat(import): library PKG picker and passcode UI"
```

---

### Task 9: Settings — clear saved PKG keys

**Files:**
- Modify: `android/BachataS4/feature/settings/src/main/kotlin/com/bachatas4/android/feature/settings/SettingsScreen.kt` (and ViewModel only if needed)
- Optionally inject `PkgKeyStore` via existing EntryPoint or Hilt VM

**UI:**
- Section “Game import” (or under existing storage-ish area): button **Clear saved PKG keys**
- On click: `pkgKeyStore.clear()` on IO; Toast “Saved PKG keys cleared”

If Settings cannot easily inject data module types, add a thin method on existing repository/EntryPoint used by Settings, or `@AndroidEntryPoint` already on host — use:

```kotlin
val store = remember { PkgKeyStore(context.filesDir) }
// acceptable if PkgKeyStore is simple; prefer Hilt if SettingsViewModel already Hilt
```

- [ ] **Step 1: Add button + clear action.**

- [ ] **Step 2: Commit**

```bash
git commit -m "feat(import): settings action to clear PKG keydb"
```

---

### Task 10: End-to-end verification

**Files:** none required unless fixes.

- [ ] **Step 1: Unit tests**

```bash
cd android/BachataS4 && ./gradlew :core:data:testDebugUnitTest :core:runtime:testDebugUnitTest
```

Expected: PASS (runtime tests may be sparse).

- [ ] **Step 2: Assemble debug app**

```bash
cd android/BachataS4 && ./gradlew :app:assembleDebug
```

Expected: SUCCESS; APK contains `lib/arm64-v8a/libbachata_pkg.so`:

```bash
unzip -l app/build/outputs/apk/debug/app-debug.apk | grep bachata_pkg
```

- [ ] **Step 3: Manual checklist (device)**
  1. Import folder still works.
  2. Import small `.pkg` (fake/homebrew) → appears in library.
  3. Progress shows Extracting → Registering → success; notification text matches stages.
  4. Cancel mid-extract → no `.import-*` left under `files/games`.
  5. Passcode PKG → dialog → success → second import no dialog (KeyDB).
  6. Settings clear keys → passcode asked again.

- [ ] **Step 4: Final commit only if verification fixes needed**

---

## Spec coverage checklist

| Spec requirement | Task |
|------------------|------|
| PKG import via native stream-extract | 4–7 |
| No full PKG copy | 6–7 |
| Folder import unchanged | 7–8 (mode switch) |
| Progress stages UI + notif | 1, 7, 8 |
| Key order zero → KeyDB → EKPFS → user | 5–7 |
| Passcode dialog + auto-save | 2, 7, 8 |
| Settings clear keys | 9 |
| Staging cleanup | 7 |
| Free space preflight | 7 (use probe sizes + `File.usableSpace`) |
| Already imported fail | 3, 7 |
| Out of scope items not implemented | — |

## Self-review notes

- No notification importance setting (explicitly out of scope).
- Type names consistent: `PkgStatus`, `PkgProbeResult`, `PkgExtractResult`, `PkgKeyStore`, `finalizeStagingTree`.
- Native port is large: Tasks 5–6 are the riskiest; keep buffers big and avoid full-image RAM maps.
- If OpenSSL is awkward in NDK, port AES from LibOrbisPkg C# logic to portable C++ (correctness first).
