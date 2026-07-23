# PKG Import Design

## Goal

Allow library import of PS4 `.pkg` files in addition to existing game folders. Extract via native C++/JNI (stream from source FD; no full package copy). Surface extract/register progress in the in-app import card and status-bar notification. Support full key resolution order from Pkg-Editor / LibOrbisPkg, with minimal KeyDB UI (passcode dialog, auto-save, Settings clear-all).

## Background

Today import is folder-only:

1. Library uses `OpenDocumentTree`.
2. `ImportService` walks the SAF tree, reads `sce_sys/param.sfo`, resolves metadata.
3. `ContentImporter.importGameTree` copies into `files/games/<id>`.
4. `ImportManager` + notification channel `import` (`IMPORTANCE_LOW`) report progress.

Reference implementation for PKG read/decrypt/extract: `/home/jica/repo/Pkg-Editor-2023` (`LibOrbisPkg` + `PkgView` key resolution). Algorithms are the source of truth; the C# UI is not ported.

## Decisions (locked)

| Topic | Choice |
|-------|--------|
| Crypto / key path | Full order: zero passcode → saved KeyDB passcode → embedded EKPFS → user passcode |
| KeyDB UI | Passcode dialog on need; auto-save on success; Settings “Clear saved PKG keys” only |
| Notification | Stage labels in notif + UI only; no importance setting |
| Extract host | Native C++/JNI (30GB+ titles) |
| Architecture | Stream-extract from `ParcelFileDescriptor` into staging under `files/games/`; no double-write through folder re-copy |

## Pipeline

```
Library UI
  ├─ Import folder → OpenDocumentTree → existing ImportService folder path
  └─ Import PKG    → OpenDocument (.pkg) → ImportService PKG path
         │
         ▼
  claim ImportManager slot
  open ParcelFileDescriptor (read-only)
         │
         ▼
  JNI probe(fd) → content_id, sizes, title hint?, auth status
         │
  NEED_PASSCODE ──► UI passcode dialog ──► restart extract with passcode
         │
         ▼
  staging = files/games/.import-<uuid>
  JNI extract(fd, staging, passcode?, cancel, progress_cb)
         │
         ▼
  read staging/sce_sys/param.sfo
  GameMetadataResolver
  atomic rename staging → files/games/<id>
  GameRepository register
  KeyDB auto-save passcode when used successfully
  Success / Failed
```

### Storage and free space

- Never copy the entire `.pkg` into app storage.
- Read only via the source FD (SAF / MediaStore / file URI → `ParcelFileDescriptor`).
- Free space required ≈ extracted game size (+ small staging overhead), not package + extract.
- When package/PFS size is known from the header, preflight available space (`StatFs` on games dir) and fail early with a clear error.
- On any failure or cancel: delete staging directory.

### Folder import

Unchanged behavior and entry points, aside from shared progress/UI chrome if needed for the dual import affordance.

## Progress model

Extend `ImportProgress` (and map each state to notification text):

| State | Meaning | UI / notification |
|-------|---------|-------------------|
| `Idle` | No active import | — |
| `Preparing` | Slot claimed, opening source | Preparing import… |
| `Scanning(name)` | Folder tree scan or PKG probe | Identifying… |
| `Extracting(bytesCopied, totalBytes?, gameTitle, currentFile?)` | Native PFS write | Extracting PKG · size · file |
| `Copying(...)` | Folder tree copy only | Importing… (existing) |
| `Finalizing(title)` | SFO resolve, rename, DB | Registering game… |
| `NeedPasscode(contentId, titleHint?)` | Auth required | Passcode dialog; notif “Passcode required” |
| `Success(gameId, title)` | Done | Existing success treatment |
| `Failed(message)` | Terminal error | Existing failure treatment |

Notification channel remains `import` at `IMPORTANCE_LOW`. No user-facing importance control in v1.

Cancel: existing cancel path stops the service job; native extract polls a cancel flag between chunks/files and aborts with staging cleanup.

## UI

### Library

- Import affordance offers **Folder** or **PKG** (bottom sheet or equivalent dual action on the import card).
- PKG picker: `ActivityResultContracts.OpenDocument` (type `*/*` or application/octet-stream), validate `.pkg` suffix / magic after pick.
- On `NeedPasscode`: modal dialog — 32-character passcode field, Submit / Cancel. Cancel fails/resets import cleanly.
- Keep optional `POST_NOTIFICATIONS` request behavior already used for import progress.

### Settings

- Single action: **Clear saved PKG keys** — deletes KeyDB file and confirms via toast/snackbar.
- No key list, edit, import/export of desktop `keydb.json` in v1.

## KeyDB

- Path: app-private `filesDir/pkg_keydb.json`.
- Schema v1:

```json
{
  "passcodes": {
    "<content_id>": "<32-char passcode>"
  }
}
```

- Resolution order (aligned with Pkg-Editor `PkgView`):
  1. Try zero passcode (`0000…0`).
  2. Try saved passcode for `content_id`.
  3. Try embedded EKPFS from package (`GetEkpfs` equivalent).
  4. Surface `NeedPasscode` to UI.
- On successful extract that used a non-zero user or saved passcode: write/update `passcodes[content_id]`.
- EKPFS/XTS maps are not user-editable in v1; native may still use embedded package material without persisting it unless needed later.

## Native module

New NDK library (e.g. `bachata_pkg`) linked into the Android app:

```text
probe(fd) -> {
  contentId,
  packageSize?,
  pfsSize?,
  titleHint?,
  auth: OK | NEED_PASSCODE | ERROR
}

extract(
  fd,
  outPath,
  passcodeNullable,
  cancelFlag,
  progressCb(bytesDone, totalHint?, currentFile?)
) -> OK | NEED_PASSCODE | CANCELLED | ERROR(message)
```

### Implementation notes

- Port extract path from LibOrbisPkg: package header/entries, outer PFS, key derivation (`ComputeKeys`), AES-XTS, PFS directory walk, PFSC decompression when present.
- Use large sequential buffers; prefer `pread`/mapped views on the FD where safe for multi-GB files.
- Write files directly under staging with path traversal checks (`..`, absolute paths rejected).
- Thin JNI: Kotlin `PkgExtractor` facade only; crypto and FS extract stay in C++.
- Host-side unit tests for crypto vectors / minimal fixtures where practical.

## Kotlin / service layer

| Component | Responsibility |
|-----------|----------------|
| `ImportService` | `EXTRA_MODE=folder\|pkg`; PKG branch owns PFD + JNI + finalize; folder branch unchanged |
| `ImportManager` | Extended progress states; single-flight slot |
| `PkgExtractor` | JNI wrapper |
| `PkgKeyStore` | Load/save/clear `pkg_keydb.json` |
| `ContentImporter` / helper | Shared atomic finalize: staging → `games/<id>` + validation that required layout exists (`param.sfo`, `eboot.bin` or project-equivalent) |
| `GameRepository` | Same registration path as successful folder import |
| `GameMetadataResolver` / `ParamSfoReader` | Unchanged consumers of extracted SFO |

Passcode redelivery: UI sends passcode via service intent extra (or bounded in-memory handoff scoped to the import job). Do not log passcodes.

## Errors

| Case | User-facing result |
|------|--------------------|
| Not a PKG / bad header | Failed: invalid package |
| Auth failure | NeedPasscode or Failed: wrong passcode |
| User cancel | Idle; staging deleted |
| Insufficient space | Failed: not enough storage |
| Missing `param.sfo` / critical payload after extract | Failed: invalid game layout |
| Destination `games/<id>` already exists | Failed: already imported |
| Permission / FD lost | Failed: source permission lost |

## Testing

- **Native:** key-derivation / XTS vectors; optional minimal synthetic PKG if buildable from LibOrbisPkg fixtures.
- **Kotlin unit:** `PkgKeyStore` round-trip and clear; progress state helpers; mode routing with mocks.
- **Manual:** small fake/homebrew PKG; one large title smoke (progress, background notif, cancel, passcode retry).

## Out of scope (v1)

- Notification importance / “notification level” preference
- Key list editor, desktop `keydb.json` import/export, manual EKPFS/XTS entry
- Replace / update existing installed game id
- PKG creation, GP4 projects
- Promoting import to a foreground service (retain current non-FG tradeoff)
- Non-PKG archives (zipped game folders, etc.)

## Success criteria

1. User can import a `.pkg` from the library without first extracting on a desktop.
2. User can still import a game folder as today.
3. 30GB+ packages do not require a full second copy of the package payload in app storage.
4. Progress stages appear in both the library card and the import notification during extract and finalize.
5. Passcode-protected packages work with dialog + auto-save; Settings can wipe saved keys.
6. Failed and cancelled imports leave no orphan staging trees under `files/games/`.

## Reference

- Pkg-Editor / LibOrbisPkg: `/home/jica/repo/Pkg-Editor-2023`
- Existing Android import: `ImportService`, `ImportManager`, `ContentImporter`, `LibraryScreen`
