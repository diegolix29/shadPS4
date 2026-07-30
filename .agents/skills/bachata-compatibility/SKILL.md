---
name: bachata-compatibility
description: Use when testing a PS4 game in Bachata S4 on Android and publishing or updating its compatibility entry with screenshots, device/build data, performance observations, and unmodified session logs.
---

# Bachata S4 compatibility reporting

Use this workflow to produce an evidence-backed entry in
`compatibility-site/data/games.json`. The website is static: the JSON file and
referenced assets are the complete compatibility database.

Resolve the repository root first:

```bash
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
```

## Non-negotiable rules

1. Test only a legally owned, unmodified game dump. Never copy games, keys, firmware,
   licenses, account data, or device identifiers into the repository.
2. Do not infer compatibility from an emulator window, title screen, or a successful
   process launch. Classify only the furthest state actually observed.
3. Never invent FPS, game version, driver version, device details, or emulator build.
   Omit optional fields when the evidence is unavailable.
4. Preserve published logs byte-for-byte. Gzip compression is allowed; editing or
   trimming the log is not. Scan for credentials or personal paths before publishing.
   If unsafe data is present, omit that log rather than silently altering it.
5. A screenshot must show the state supporting the classification. For `ingame` or
   `playable`, capture a complete gameplay frame after control is available.
6. Add a new test to the existing game instead of replacing historical results.
7. Do not publish a report until the JSON validator succeeds and every referenced
   screenshot/log exists in the repository.

## Status classification

Use exactly one of these lowercase values:

| Status | Required evidence |
|---|---|
| `playable` | Full-game completion has been verified with playable performance and no major game-breaking issue. Do not promote a short gameplay sample to `playable`. |
| `ingame` | Controllable gameplay is reached, but crashes, hangs, severe graphical/audio problems, or other game-breaking issues remain. |
| `menus` | A functional menu is reached, but gameplay cannot be entered. |
| `boots` | Visual or audio output appears, but the main menu is not reached. |
| `nothing` | The title crashes on launch, hangs before output, or remains on a black screen. |

When uncertain, choose the lower status. Record the exact blocker in `notes` and
`issues`.

## Phase 1 — Prepare the build and device

Read `AGENTS.md` and `documents/android-building.md` before rebuilding. The Gradle
build packages existing runtime assets; it does not regenerate them. Follow the
repository's runtime build and verification steps before installing a changed APK.

The direct game launcher exists only in debug builds. Install the appropriate debug
APK, then select adb by evidence:

```bash
ADB="${ADB:-adb}"
"$ADB" devices -l
# On WSL, use the full adb.exe path when Linux adb cannot see the device.
```

If more than one device is attached, export the exact serial:

```bash
export SERIAL=<device-serial>
```

Do not put that serial in the compatibility database.

Record the source state before testing:

```bash
git status --short
git rev-parse HEAD
git describe --tags --always --dirty
```

A dirty tree is allowed for development testing, but the report must identify the
actual commit and must not claim an official release unless that exact release was
installed.

## Phase 2 — Launch and capture evidence

Use the helper, which launches
`com.bachatas4.android/.DirectLaunchActivity --es game_id <CUSA>`, captures device
metadata and screenshots, flushes the app, and pulls the matching app-private session
logs from the same explicitly selected adb device:

```bash
scripts/compatibility/capture_android_report.sh CUSAxxxxx \
  --delay 60 \
  --count 2 \
  --interval 30
```

The helper prints the evidence directory. In a normal clone it is under
`.git/compatibility-work/<cusa>/<timestamp>/`, so raw captures cannot be committed by accident.
Assign that printed absolute path to `WORK`.

The fixed timer is only a convenience. Interact with the game or drive the UI so the
capture occurs after the relevant boundary. For a long intro or manual test, launch
without forcing a premature classification and capture additional frames directly:

```bash
"$ADB" -s "$SERIAL" exec-out screencap -p > \
  "$WORK/screenshots/gameplay-extra.png"
```

Before claiming `ingame` or `playable`, verify all three:

- screenshot shows a complete frame after the target boundary;
- the emulation process remains alive while the frame is captured;
- the newest pulled session log reaches the same or a later milestone and does not
  terminate before the screenshot state.

For crashes, black screens, exit codes, GPU stalls, or unexplained teardown, invoke
and follow `.agents/skills/diagnose-bachata/SKILL.md`. Compatibility reporting records
behavior; it must not substitute a guess for diagnosis.

## Phase 3 — Inspect evidence without destroying it

Locate the newest pulled session directory:

```bash
WORK="<absolute evidence directory printed by capture_android_report.sh>"
find "$WORK/session-logs" -maxdepth 3 -type f -printf '%TY-%Tm-%Td %TT %p\n' | sort
```

Use filtered reads for very large logs. Do not open multi-million-line files in full:

```bash
/usr/bin/grep -E -i \
  'exitCode|guestBackend=|driver=|Critical|Error|Unhandled access|UNREACHABLE|SIG|fps|frame' \
  "$WORK"/session-logs/**/application.log \
  "$WORK"/session-logs/**/shadps4.log 2>/dev/null | tail -n 120
```

Determine and record:

- exact title and CUSA ID;
- game version shown by the game/app metadata or logs;
- furthest verified state and status;
- emulator version and commit;
- guest backend (`fex`, `box64`, `native-arm64`, or `unknown`);
- phone manufacturer/model, SoC, GPU, Android version, RAM;
- Vulkan driver and version from the session log or app settings;
- measured FPS range/average only when an actual counter or trace supports it;
- major issues, reproducible transitions, and any settings that materially affected
  behavior.

Do not treat the Android UI frame rate, screen refresh rate, or guessed visual smoothness
as game FPS.

Review every screenshot and discard captures that are blank, show private notifications,
show setup/account information, or fail to prove the selected state. Do not publish
`capture.json`, because it contains the adb device serial. If no app-private session folder
was created, use the unmodified `$WORK/logcat.txt` as the required log evidence and state
that the session log was unavailable.

## Phase 4 — Add or update the JSON report

Use the helper instead of editing asset paths or hashes manually. It copies screenshots,
compresses logs, computes SHA-256, appends a historical test, sorts the database, and
updates `lastUpdated`.

Example:

```bash
python3 scripts/compatibility/add_report.py \
  --title "Game title" \
  --serial CUSAxxxxx \
  --region US \
  --status ingame \
  --game-version "1.00" \
  --guest-backend fex \
  --summary "Reaches gameplay with a major rendering blocker." \
  --notes "Describe exactly what was tested, how far it progressed, and what failed." \
  --issue "First reproducible game-breaking issue" \
  --device-json "$WORK/device.json" \
  --renderer-driver "System Vulkan" \
  --driver-version "Observed driver version" \
  --resolution-scale 1.0 \
  --average-fps 24 \
  --min-fps 18 \
  --max-fps 30 \
  --frame-pacing stuttery \
  --test-duration-seconds 300 \
  --screenshot "$WORK/screenshots/menu.png::Main menu" \
  --screenshot "$WORK/screenshots/gameplay.png::Controllable gameplay" \
  --log "$WORK/session-logs/<session>/application.log::Bachata application log" \
  --log "$WORK/session-logs/<session>/shadps4.log::shadPS4 session log" \
  --tester "GitHub handle"
```

Remove arguments for facts that were not measured. Required device fields must remain
factual; correct `device.json` before running the command when the Android property is
generic or inaccurate. Use `--settings-json` for compatibility-relevant non-default
settings.

The database shape is documented by `compatibility-site/data/schema.json`. Each game
has one stable object and an ordered `tests` array. The website displays the newest
`testedAt` report while preserving prior results in history.

## Phase 5 — Validate and review the website

Run all checks:

```bash
python3 scripts/compatibility/validate_database.py
node --check compatibility-site/assets/js/app.js
python3 -m http.server 8080 --directory compatibility-site
```

Open the local page and verify:

- search finds the title and CUSA;
- status/GPU filters include the report;
- card screenshot loads and is not stretched or blank;
- details show the correct device, driver, build, performance, notes, and issues;
- every evidence link opens;
- the shareable `?game=<id>` URL opens the same report;
- mobile-width layout remains usable.

Inspect the exact diff:

```bash
git diff -- \
  compatibility-site/data/games.json \
  compatibility-site/assets/screenshots \
  compatibility-site/assets/logs
```

The raw work directory is inside Git metadata and cannot be committed. Commit only the database
and the accepted evidence copied into `compatibility-site/assets/`. A typical commit message is:

```text
compatibility: add CUSAxxxxx test on <device>
```

## Publication gate

The report is complete only when:

1. classification is supported by screenshot and log evidence;
2. no private or copyrighted game files were added;
3. optional measurements were omitted instead of guessed;
4. `validate_database.py` succeeds;
5. local website rendering and evidence links were checked;
6. the GitHub Pages workflow succeeds after push.

If any gate fails, leave the evidence in the printed raw work directory, do not publish the
entry, and state exactly what is still missing.
