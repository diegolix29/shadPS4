# Bachata S4 release-based compatibility website

Extract this additive package into the root of `JICA98/Bachata-S4`. It does not
replace emulator source files or the existing documentation.

## What changed in schema v2

Compatibility is now scoped to a published Bachata S4 GitHub release. Every test
records:

- `releaseTag`, matched against `compatibility-site/data/releases.json`;
- a public selected-device label plus manufacturer/model/SoC/GPU/Android/RAM;
- driver type (`system`, `turnip`, or `custom`);
- exact `turnipVersion` when Turnip is selected;
- screenshots, immutable compressed logs, commit, game version, status, and notes.

The website defaults to the latest stable GitHub release and includes selectors for
release, device, graphics driver/Turnip version, GPU, and status. Older results remain
visible by switching releases.

## Included paths

- `compatibility-site/` — dependency-free GitHub Pages frontend
- `compatibility-site/data/games.json` — compatibility source of truth
- `compatibility-site/data/releases.json` — cached official GitHub release index
- `scripts/compatibility/sync_releases.py` — refresh release index from GitHub
- `scripts/compatibility/capture_android_report.sh` — select device, launch game, capture evidence
- `scripts/compatibility/add_report.py` — release-scoped JSON/evidence importer
- `scripts/compatibility/validate_database.py` — strict stdlib validator
- `.agents/skills/bachata-compatibility/SKILL.md` — complete agent testing workflow
- `.github/workflows/compatibility-pages.yml` — validation, release sync, and Pages deployment

## Install and preview

```bash
unzip bachata-s4-compatibility-website-v2.zip -d .
python3 scripts/compatibility/validate_database.py
python3 -m http.server 8080 --directory compatibility-site
```

Open `http://localhost:8080`. Commit and push the files, then set **Settings → Pages →
Source** to **GitHub Actions**. The expected URL is:

```text
https://jica98.github.io/Bachata-S4/
```

## Sync releases

Run this after publishing a release, or let the included release-triggered workflow do it:

```bash
python3 scripts/compatibility/sync_releases.py
```

A report cannot reference a tag absent from `data/releases.json`; this prevents results
from being accidentally attributed to a local development build.

## Capture a test

```bash
export SERIAL=<adb-device-serial>
scripts/compatibility/capture_android_report.sh CUSA00900 \
  --release-tag v0.1.5 \
  --device-label "OnePlus 12 · Snapdragon 8 Gen 3" \
  --driver-type turnip \
  --driver-name "Mesa Turnip" \
  --turnip-version "26.1.0" \
  --delay 60 --count 2 --interval 30
```

The selected ADB serial remains only in the private `.git/compatibility-work` capture
metadata. It is never copied to the public JSON.
