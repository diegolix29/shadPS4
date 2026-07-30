---
name: bachata-compatibility
description: Test a legally owned PS4 game on a specific published Bachata S4 GitHub release, selected Android device, and selected Vulkan/Turnip driver; capture screenshots and logs; then publish a validated compatibility JSON entry.
---

# Bachata S4 release compatibility reporting

Use this workflow to create evidence-backed records in
`compatibility-site/data/games.json`. Compatibility is not a single global state: every
report belongs to one published GitHub release, one selected physical Android device, and
one selected graphics driver. Never merge observations from different environments.

```bash
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
```

## Non-negotiable rules

1. Test only games, firmware, and content the tester is legally entitled to use. Never
   commit game files, firmware, keys, licenses, account data, or private device IDs.
2. Use a published tag listed by `gh release list --repo JICA98/Bachata-S4`. Do not label
   a dirty/local build as an official release.
3. Install and test the APK asset from the selected release, or prove that the installed
   APK was built from the exact release commit. Record both `releaseTag` and `commit`.
4. Explicitly select one ADB device. Publish only its human-readable device label and
   hardware details; never publish the ADB serial.
5. Record the driver actually selected in Bachata S4. When Turnip is selected, the exact
   Turnip version is mandatory. Do not use the Mesa/Turnip version from memory.
6. Classify only the furthest state actually observed. Never infer gameplay from a
   process launch, title screen, or log line alone.
7. Do not invent FPS, versions, hardware, settings, or milestones. Omit optional
   measurements that are not supported by evidence.
8. Preserve published logs byte-for-byte. Gzip is allowed; editing or trimming is not.
9. Append a historical test. Never overwrite results from an older release/device/driver.
10. Publish only after strict validation and local website review succeed.

## Status classification

| Status | Required evidence |
|---|---|
| `playable` | Full-game completion verified with playable performance and no major game-breaking issue. |
| `ingame` | Controllable gameplay reached, but crashes, hangs, severe rendering/audio defects, or other major issues remain. |
| `menus` | Functional menus reached, but gameplay cannot be entered. |
| `boots` | Visual or audio output occurs before the main menu. |
| `nothing` | Crash, hang, or black screen before useful output. |

When uncertain, use the lower status and describe the exact blocker.

## Phase 1 — Select and verify a GitHub release

Refresh the checked-in release index and inspect official releases:

```bash
python3 scripts/compatibility/sync_releases.py
gh release list --repo JICA98/Bachata-S4 --limit 20
```

Set the exact published tag:

```bash
export BACHATA_RELEASE=v0.1.5

gh release view "$BACHATA_RELEASE" \
  --repo JICA98/Bachata-S4 \
  --json tagName,name,isPrerelease,publishedAt,targetCommitish,url
```

Install the correct APK asset for that tag. After installation, verify the package version:

```bash
ADB="${ADB:-adb}"
"$ADB" shell dumpsys package com.bachatas4.android \
  | sed -nE 's/^[[:space:]]*versionName=//p' | head -n 1
```

The installed `versionName` should correspond to the selected tag after removing the
leading `v`. If it does not, stop and install the correct release.

## Phase 2 — Select the physical device and graphics driver

List devices:

```bash
"$ADB" devices -l
```

When multiple devices are attached, explicitly select one:

```bash
export SERIAL=<exact-adb-serial>
```

Choose a public label that distinguishes the hardware without exposing the serial, for
example:

```bash
export DEVICE_LABEL="OnePlus 12 · Snapdragon 8 Gen 3"
```

In Bachata S4, open the graphics-driver selector and note exactly what is selected:

- system driver: record driver name and observed version;
- Turnip: record exact Mesa/Turnip version, optional build/revision, and source;
- custom non-Turnip driver: record its displayed name and version.

Confirm driver evidence from app settings and/or session logs. Search likely fields:

```bash
grep -E -i 'turnip|mesa|driver|vulkan|gpu' <session-log> | tail -n 120
```

Do not confuse Android version, Vulkan API version, or GPU model with the Turnip version.

## Phase 3 — Launch and capture evidence

Example using Turnip:

```bash
scripts/compatibility/capture_android_report.sh CUSAxxxxx \
  --release-tag "$BACHATA_RELEASE" \
  --device-label "$DEVICE_LABEL" \
  --driver-type turnip \
  --driver-name "Mesa Turnip" \
  --turnip-version "26.1.0" \
  --turnip-build "exact displayed build, when available" \
  --turnip-source "bundled / imported source label" \
  --delay 60 \
  --count 2 \
  --interval 30
```

System-driver example:

```bash
scripts/compatibility/capture_android_report.sh CUSAxxxxx \
  --release-tag "$BACHATA_RELEASE" \
  --device-label "$DEVICE_LABEL" \
  --driver-type system \
  --driver-name "Qualcomm system Vulkan" \
  --driver-version "exact observed version" \
  --delay 60 --count 2 --interval 30
```

The helper launches the debug-only `DirectLaunchActivity`, captures screenshots and
hardware metadata from the selected device, force-stops the app to flush logs, and pulls
the newest matching app-private session logs. It prints `Evidence directory: ...`; assign
that absolute path to `WORK`.

Interact with the game until the target boundary is reached. For `ingame` or `playable`,
verify that the screenshot shows a complete controllable frame and that the session log
reaches the same or a later state without terminating first.

For crashes, black screens, exit codes, GPU stalls, or unexplained teardown, use
`.agents/skills/diagnose-bachata/SKILL.md` when available. Do not replace diagnosis with a
guess in the compatibility notes.

## Phase 4 — Inspect evidence

```bash
WORK="<absolute evidence directory>"
cat "$WORK/device.json"
cat "$WORK/capture.json"  # private work file; never publish it
find "$WORK/session-logs" -maxdepth 3 -type f -printf '%TY-%Tm-%Td %TT %p\n' | sort
```

Use filtered reads for large logs:

```bash
/usr/bin/grep -E -i \
  'exitCode|guestBackend=|turnip|mesa|driver=|Critical|Error|Unhandled access|SIG|fps|frame' \
  "$WORK"/session-logs/**/application.log \
  "$WORK"/session-logs/**/shadps4.log 2>/dev/null | tail -n 160
```

Determine:

- title, CUSA, region, and game version;
- exact release tag and release commit;
- selected device label and captured hardware fields;
- selected driver type/name/version;
- exact Turnip version/build/source when applicable;
- guest backend;
- furthest verified state and status;
- measured FPS only from an actual counter or trace;
- major issues and relevant non-default settings.

Discard screenshots that are blank, fail to prove the status, or expose notifications,
accounts, or private information. Never publish `capture.json`, because it contains the
ADB serial. If app-private logs are unavailable, use unmodified `logcat.txt` and state that
limitation in the notes.

## Phase 5 — Add the report

Turnip example:

```bash
python3 scripts/compatibility/add_report.py \
  --title "Game title" \
  --serial CUSAxxxxx \
  --region US \
  --status ingame \
  --game-version "1.00" \
  --release-tag "$BACHATA_RELEASE" \
  --commit "<exact release commit SHA>" \
  --guest-backend fex \
  --summary "Reaches gameplay with a major rendering blocker." \
  --notes "State exactly what was tested, how far it progressed, and what failed." \
  --issue "First reproducible game-breaking issue" \
  --device-json "$WORK/device.json" \
  --driver-type turnip \
  --renderer-driver "Mesa Turnip" \
  --turnip-driver-version "26.1.0" \
  --turnip-driver-build "exact displayed build" \
  --turnip-driver-source "bundled / imported source label" \
  --resolution-scale 1.0 \
  --average-fps 24 --min-fps 18 --max-fps 30 \
  --frame-pacing stuttery \
  --test-duration-seconds 300 \
  --screenshot "$WORK/screenshots/menu.png::Main menu" \
  --screenshot "$WORK/screenshots/gameplay.png::Controllable gameplay" \
  --log "$WORK/session-logs/<session>/application.log::Bachata application log" \
  --log "$WORK/session-logs/<session>/shadps4.log::shadPS4 session log" \
  --tester "GitHub handle"
```

For the system driver, use `--driver-type system`, set `--driver-version`, and omit all
`--turnip-*` arguments. The importer rejects an unknown release tag and requires a Turnip
version whenever the driver type is `turnip`.

## Phase 6 — Validate and review

```bash
python3 scripts/compatibility/validate_database.py
node --check compatibility-site/assets/js/app.js
bash -n scripts/compatibility/capture_android_report.sh
python3 -m http.server 8080 --directory compatibility-site
```

Verify locally:

- the selected release defaults/filters correctly;
- the game appears only for environments with a matching report;
- release, selected device, and exact Turnip/system driver are visible on the card;
- detail history preserves older release/device/driver reports;
- screenshots and immutable log links open;
- mobile layout remains usable.

Inspect the exact diff:

```bash
git diff -- \
  compatibility-site/data/games.json \
  compatibility-site/data/releases.json \
  compatibility-site/assets/screenshots \
  compatibility-site/assets/logs
```

Commit only accepted public evidence and JSON. A suitable message is:

```text
compatibility: add CUSAxxxxx on v0.1.5 / <device> / Turnip <version>
```

## Publication gate

Publish only when the release tag is official, installed version matches, selected device
and driver are factual, classification is supported by screenshot and log evidence, no
private/copyrighted content is present, and all validation and local rendering checks pass.
