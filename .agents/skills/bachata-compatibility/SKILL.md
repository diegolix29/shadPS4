---
name: bachata-compatibility
description: Create or reuse a canonical game issue in JICA98/Bachata-S4, create a dedicated compatibility-repository worktree, test a legally owned PS4 game on one published Bachata S4 release/device/driver, capture evidence, stage an immutable report, and publish it only after explicit user confirmation.
---

# Bachata S4 compatibility report workflow

The emulator/frontend repository is `JICA98/Bachata-S4`. Compatibility metadata, one-file-
per-test JSON, screenshots, and logs belong exclusively to
`JICA98/Bachata-S4-Compatibility`.

## Absolute rules

1. Test only content the tester legally owns. Never publish game files, firmware, keys,
   licenses, accounts, private device identifiers, or the ADB serial.
2. Create the compatibility Git worktree **before** creating or changing report files.
3. Search for or create the canonical GitHub issue in **JICA98/Bachata-S4** before launching the game. Reuse one issue per CUSA. Never create one issue per device or release. Compatibility report files and evidence still belong only to JICA98/Bachata-S4-Compatibility.
4. Every report belongs to one published Bachata S4 release, one selected physical device,
   and one selected Vulkan driver. Exact Turnip version is mandatory for Turnip.
5. Existing report JSON and evidence are immutable. Add a new superseding report.
6. Do not commit, push, open a pull request, change the final issue status, or comment the
   result until the user explicitly confirms the prepared report.
7. Status is the furthest state actually observed: `playable`, `ingame`, `menus`, `boots`,
   or `nothing`. When uncertain, choose the lower status.
8. The canonical issue has exactly one `status:*` label representing the best confirmed
   result across all reports. Per-report status remains in JSON.
9. Every confirmed issue update must display at least one safe, representative gameplay
   screenshot inline. Use screenshots committed with the report and immutable commit URLs;
   never expose a local path, ADB serial, notification, account name, or other private data.

## 1. Resolve repositories and prerequisites

```bash
MAIN_ROOT="$(git rev-parse --show-toplevel)"
COMPAT_REPO="${BACHATA_COMPAT_REPO:-$(dirname "$MAIN_ROOT")/Bachata-S4-Compatibility}"
COMPAT_REMOTE="https://github.com/JICA98/Bachata-S4-Compatibility.git"

command -v gh adb git python3 >/dev/null
gh auth status

if [[ ! -d "$COMPAT_REPO/.git" ]]; then
  git clone "$COMPAT_REMOTE" "$COMPAT_REPO"
fi
git -C "$COMPAT_REPO" remote set-url origin "$COMPAT_REMOTE"
git -C "$COMPAT_REPO" fetch origin main --prune
git -C "$MAIN_ROOT" fetch origin --tags --prune
```

Set concrete identifiers. CUSA must be uppercase:

```bash
export CUSA=CUSAxxxxx
export GAME_TITLE="Exact game title"
export BACHATA_RELEASE=v0.1.6
export DEVICE_LABEL="OnePlus 13 · Snapdragon 8 Elite"
```

Verify the release exists and obtain its commit:

```bash
gh release view "$BACHATA_RELEASE" --repo JICA98/Bachata-S4 \
  --json tagName,name,isPrerelease,publishedAt,targetCommitish,url
RELEASE_COMMIT="$(git -C "$MAIN_ROOT" rev-list -n1 "$BACHATA_RELEASE")"
```

Do not describe a dirty/local build as an official release. Install the APK from the
selected release or prove the APK was built from the exact release commit.

## 2. Create the compatibility worktree first

Use a unique branch and a sibling worktree. Never stage reports directly in the main
compatibility clone:

```bash
STAMP="$(date -u +%Y%m%d-%H%M%S)"
REPORT_BRANCH="compat/${CUSA,,}-$STAMP"
WORKTREE_ROOT="${BACHATA_WORKTREE_ROOT:-$(dirname "$MAIN_ROOT")/.worktrees}"
COMPAT_WORKTREE="$WORKTREE_ROOT/bachata-compat-${CUSA,,}-$STAMP"
mkdir -p "$WORKTREE_ROOT"

git -C "$COMPAT_REPO" worktree add -b "$REPORT_BRANCH" \
  "$COMPAT_WORKTREE" origin/main
```

All compatibility scripts after this point must run from `$COMPAT_WORKTREE`.

## 3. Find or create the canonical issue before testing

Ensure shared labels exist once:

```bash
"$COMPAT_WORKTREE/scripts/setup_labels.sh" JICA98/Bachata-S4
```

Search open and closed issues by exact CUSA:

```bash
ISSUE_NUMBER="$(gh issue list --repo JICA98/Bachata-S4 \
  --state all --search "\"$CUSA\" in:title" --json number,title \
  --jq ".[] | select(.title | contains(\"$CUSA\")) | .number" | head -n1)"
```

Create a game-specific label if needed, then create the issue only when none exists:

```bash
gh label create "game:$CUSA" --repo JICA98/Bachata-S4 \
  --color 5319e7 --description "Reports for $CUSA" --force

if [[ -z "$ISSUE_NUMBER" ]]; then
  ISSUE_URL="$(gh issue create --repo JICA98/Bachata-S4 \
    --title "[$CUSA] $GAME_TITLE" \
    --label "game-report,game:$CUSA,status:testing,needs-confirmation" \
    --body "Canonical compatibility discussion for **$GAME_TITLE** ($CUSA). Individual release/device/driver tests will be submitted as immutable report pull requests after tester confirmation.")"
  ISSUE_NUMBER="${ISSUE_URL##*/}"
else
  gh issue edit "$ISSUE_NUMBER" --repo JICA98/Bachata-S4 \
    --add-label "game-report,game:$CUSA,status:testing,needs-confirmation"
fi
```

Read the issue and existing reports before testing so the new run addresses known blockers:

```bash
gh issue view "$ISSUE_NUMBER" --repo JICA98/Bachata-S4 --comments
find "$COMPAT_WORKTREE/games/$CUSA/reports" -maxdepth 1 -name '*.json' -print 2>/dev/null | sort
```

## 4. Select device and exact driver

```bash
adb devices -l
export SERIAL=<exact-adb-serial>
```

When multiple devices exist, `$SERIAL` is mandatory. The private capture file may contain
it, but report JSON must contain only the human-readable device label and hardware fields.

In Bachata S4, select the driver and record what is actually displayed or logged:

- Turnip: type, name, exact Mesa/Turnip version, optional build/revision and source.
- System: driver name and observed version.
- Custom: exact name and version.

Do not confuse Android version, Vulkan API version, GPU model, or bundle filename with the
Turnip version.

## 5. Launch and capture

Turnip example:

```bash
cd "$MAIN_ROOT"
scripts/compatibility/capture_android_report.sh "$CUSA" \
  --release-tag "$BACHATA_RELEASE" \
  --device-label "$DEVICE_LABEL" \
  --driver-type turnip \
  --driver-name "Mesa Turnip" \
  --turnip-version "26.3.0-devel" \
  --turnip-build "git-exactrevision" \
  --turnip-source "bundled/imported source label" \
  --delay 60 --count 2 --interval 30
```

The helper uses the selected ADB device, launches `DirectLaunchActivity` when available, takes screenshots,
force-stops the app to flush logs, and pulls the matching app-private session. If testing an official APK without the debug-only direct launcher, open the game manually and pass `--no-launch`. Assign the
printed evidence directory to `CAPTURE`.

```bash
export CAPTURE="<absolute evidence directory>"
cat "$CAPTURE/device.json"
cat "$CAPTURE/capture.json"   # private; never publish
find "$CAPTURE/session-logs" -type f -maxdepth 3 -print
```

Inspect screenshots and filtered log lines. Do not infer gameplay from launch success or a
single log line. Discard blank/private screenshots. Keep logs byte-for-byte; the importer
will gzip and hash them.

## 6. Stage one immutable report in the worktree

Use the canonical issue number and exact release commit. Add one `--screenshot` and one
`--log` argument per evidence file; `path::caption` and `path::label` are supported.

```bash
cd "$COMPAT_WORKTREE"
python3 scripts/add_report.py \
  --title "$GAME_TITLE" \
  --cusa "$CUSA" \
  --region US \
  --publisher "Publisher" \
  --issue-number "$ISSUE_NUMBER" \
  --issue-repository JICA98/Bachata-S4 \
  --status ingame \
  --game-version "01.00" \
  --release-tag "$BACHATA_RELEASE" \
  --commit "$RELEASE_COMMIT" \
  --emulator-version "${BACHATA_RELEASE#v}" \
  --guest-backend fex \
  --summary "Exact one-sentence observed result." \
  --notes "What was tested, how far it progressed, settings, and the blocker." \
  --issue "Major reproducible problem, when present" \
  --device-json "$CAPTURE/device.json" \
  --driver-type turnip \
  --driver-name "Mesa Turnip" \
  --driver-version "26.3.0-devel" \
  --driver-build "git-exactrevision" \
  --driver-source "bundled/imported source label" \
  --resolution-scale 1.0 \
  --average-fps 30 --min-fps 24 --max-fps 35 \
  --frame-pacing stuttery \
  --test-duration-seconds 300 \
  --screenshot "$CAPTURE/screenshots/first.png::What this proves" \
  --screenshot "$CAPTURE/screenshots/second.png::What this proves" \
  --log "$CAPTURE/session-logs/<session>/application.log::Bachata application log" \
  --log "$CAPTURE/session-logs/<session>/shadps4.log::shadPS4 session log" \
  --tester "$(gh api user --jq .login)"

python3 scripts/validate.py
python3 scripts/build_site_data.py --output generated
```

FPS values are optional unless measured by a real counter/trace. Do not invent values.

## 7. Preview and request explicit confirmation

Assemble a local preview using the frontend from `$MAIN_ROOT` and generated data from the
worktree. Copy only screenshots, not logs:

```bash
PREVIEW="$(mktemp -d)"
cp -a "$MAIN_ROOT/compatibility-site/." "$PREVIEW/"
mkdir -p "$PREVIEW/data" "$PREVIEW/evidence"
cp "$COMPAT_WORKTREE/generated/site-index.json" "$PREVIEW/data/"
cp "$COMPAT_WORKTREE/generated/releases.json" "$PREVIEW/data/"
cp -a "$COMPAT_WORKTREE/generated/games" "$PREVIEW/data/"
(
  cd "$COMPAT_WORKTREE"
  find assets -type f -path '*/screenshots/*' -exec cp --parents '{}' "$PREVIEW/evidence/" \;
)
python3 -m http.server 8080 --directory "$PREVIEW"
```

Before publication, show the user:

- CUSA/title and canonical issue number;
- status and why it meets that boundary;
- Bachata release and commit;
- selected public device;
- selected driver and exact Turnip version/build;
- performance only when measured;
- screenshot thumbnails/paths and log names/hashes;
- the exact one to three screenshots that will be embedded in the canonical issue comment;
- `git -C "$COMPAT_WORKTREE" diff --stat` and validation result.

Ask for explicit confirmation to publish this exact report. Stop here until confirmation.
Do not treat silence, earlier general approval, or successful validation as confirmation.

## 8. Publish only after confirmation

Determine the best confirmed status from existing reports using this order:

```text
playable > ingame > menus > boots > nothing
```

Update the canonical issue to exactly one best `status:*` label; remove `status:testing`
and `needs-confirmation`. Then commit, push, and open the PR:

```bash
cd "$COMPAT_WORKTREE"
git add games assets

# Capture the newly staged, public screenshots before committing. Embed no more than three
# representative images in the issue so the conversation remains readable.
mapfile -t ISSUE_SCREENSHOTS < <(
  git diff --cached --name-only --diff-filter=A -- assets \
    | grep -Ei '/screenshots/.*\.(png|jpe?g|webp)$' \
    | head -n 3
)

if (( ${#ISSUE_SCREENSHOTS[@]} == 0 )); then
  echo "No safe staged screenshot found; refusing to publish an issue update without visual evidence." >&2
  exit 1
fi

git commit -m "compat($CUSA): add $BACHATA_RELEASE report"
REPORT_COMMIT="$(git rev-parse HEAD)"
git push -u origin "$REPORT_BRANCH"

PR_URL="$(gh pr create --repo JICA98/Bachata-S4-Compatibility \
  --base main --head "$REPORT_BRANCH" \
  --title "compat($CUSA): $GAME_TITLE on $BACHATA_RELEASE" \
  --body "Canonical discussion: https://github.com/JICA98/Bachata-S4/issues/$ISSUE_NUMBER\n\n- Status: <status>\n- Device: $DEVICE_LABEL\n- Driver: <exact driver/version>\n- Release: $BACHATA_RELEASE")"
```

Update labels carefully. Do not downgrade the best confirmed issue status because a newer
release/device regresses; add `regression` instead and retain the best-status label.

Comment on the same canonical issue only after confirmation. The comment must include the
report summary and one to three screenshots rendered inline. Build image URLs from the
**pushed report commit SHA**, not a local path or mutable branch name:

```bash
ISSUE_COMMENT="$(mktemp)"
cat > "$ISSUE_COMMENT" <<EOF
Confirmed compatibility report submitted: $PR_URL

- **Status:** <status>
- **Release:** $BACHATA_RELEASE
- **Device:** $DEVICE_LABEL
- **Driver:** <exact driver/version>
- **Evidence commit:** \`$REPORT_COMMIT\`

### Screenshots
EOF

for screenshot_path in "${ISSUE_SCREENSHOTS[@]}"; do
  # Report-generated asset paths are expected to be URL-safe. Encode spaces defensively.
  encoded_path="${screenshot_path// /%20}"
  printf '\n![%s — %s — %s](https://raw.githubusercontent.com/JICA98/Bachata-S4-Compatibility/%s/%s)\n' \
    "$GAME_TITLE" "$BACHATA_RELEASE" "<status>" \
    "$REPORT_COMMIT" "$encoded_path" >> "$ISSUE_COMMENT"
done

gh issue comment "$ISSUE_NUMBER" --repo JICA98/Bachata-S4 \
  --body-file "$ISSUE_COMMENT"
rm -f "$ISSUE_COMMENT"
```

Open the issue after commenting and verify that every selected image renders correctly. If
an image is broken, private, blank, or misleading, delete/edit the comment immediately and
replace it with a safe screenshot from the same report. Do not claim that a local pathname
or a plain hyperlink is an attached screenshot; the image must be visibly embedded in the
issue conversation.

The issue remains open as the long-lived communication thread. The PR is the auditable
report submission. The compatibility repository does not dispatch the website workflow. The site updates on the scheduled Pages rebuild or when the maintainer manually runs the Compatibility website workflow in JICA98/Bachata-S4.

## 9. Cleanup

After the PR is merged or abandoned:

```bash
git -C "$COMPAT_REPO" worktree remove "$COMPAT_WORKTREE"
git -C "$COMPAT_REPO" worktree prune
```

On user rejection, do not push. Remove the temporary worktree and local branch, and remove `status:testing`/`needs-confirmation` from the existing JICA98/Bachata-S4 issue if no test remains active.
