# In-app diagnostic reports (Release 1)

Ordinary Play Store users can create, review, save, and share a privacy-sanitized
diagnostic ZIP when a game stops unexpectedly, without ADB.

## What a report contains

Each ZIP (`bachata-diagnostic-<report-id>.zip`) may include:

| Entry | Purpose |
|---|---|
| `report.json` | Versioned metadata (app, game CUSA, device, driver, checkpoints, termination) |
| `manifest.json` | Per-file size, SHA-256, truncation, redaction version |
| `application.log` | Bachata session log |
| `shadps4.log` | Backend process output |
| `shadps4-internal.log` | Optional internal shadPS4 log when present |
| `runtime.log` | Optional runtime log when present |
| `process-exit.json` | Structured process termination (honest about unknown signals) |
| `settings.json` | Sanitized effective launch settings snapshot |
| `screenshot.webp` | Optional, **off by default** |

## Privacy and consent

- Report creation is **optional**.
- Release 1 **never uploads** a report automatically.
- The user chooses the destination through Android Sharesheet or Save (Storage Access Framework).
- Screenshots are included only when the user enables the toggle.
- Technical paths, emails, tokens, and similar secrets are redacted where possible.
- Original on-device session logs are not modified during export.

Reports may still contain technical evidence useful for debugging (CUSA IDs, Vulkan names,
driver versions, symbols, addresses, error codes).

## How to send a report to maintainers

1. Reproduce the unexpected stop.
2. Tap **Create diagnostic report**.
3. Review the summary and privacy notice.
4. Optionally add a short description.
5. Tap **Create and share**, then pick email, chat, or another app; or **Save locally**.
6. Attach the ZIP when filing a GitHub issue or contacting maintainers.

Do not commit copyrighted game data or private logs into the repository.

## Permissions

Release 1 does **not** add:

- `READ_LOGS`
- `MANAGE_EXTERNAL_STORAGE` / legacy storage permissions
- `READ_MEDIA_IMAGES`
- `QUERY_ALL_PACKAGES`

Sharing uses a narrowly scoped FileProvider authority `${applicationId}.diagnostics`
that only exposes `cacheDir/diagnostic-reports/`.

## Process exit honesty

Java/`Process.exitValue()` only provides a flattened exit value. Exit value `133` is **not**
automatically labeled `SIGTRAP`. Signal fields appear only when genuine native wait status
is available (not yet captured on the Java process boundary in Release 1).
