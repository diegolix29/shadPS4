# Bachata S4 compatibility site

A dependency-free GitHub Pages frontend for release-specific Android game compatibility.
The page joins two checked-in JSON files:

- `data/games.json`: games, historical test reports, screenshots, and logs;
- `data/releases.json`: official Bachata S4 GitHub releases.

The latest stable release is selected by default. Users can filter by GitHub release,
selected physical device, exact Turnip version/driver, GPU family, and compatibility status.
A game can therefore have different results across releases, devices, and drivers without
one test overwriting another.

## Local preview

```bash
python3 -m http.server 8080 --directory compatibility-site
```

## Refresh official releases

```bash
python3 scripts/compatibility/sync_releases.py
```

The Pages workflow also refreshes the index whenever a GitHub release is published,
edited, or deleted.

## Add a report

```bash
python3 scripts/compatibility/add_report.py \
  --title "Example Game" \
  --serial CUSA00001 \
  --region US \
  --status ingame \
  --game-version "1.00" \
  --release-tag v0.1.5 \
  --commit 407a5ae \
  --guest-backend fex \
  --notes "Reached controllable gameplay; rendering breaks after the intro." \
  --issue "Missing character textures" \
  --device-json .git/compatibility-work/cusa00001/<capture>/device.json \
  --driver-type turnip \
  --renderer-driver "Mesa Turnip" \
  --turnip-driver-version "26.1.0" \
  --turnip-driver-build "release build" \
  --turnip-driver-source "bundled Bachata S4 driver" \
  --average-fps 24 --min-fps 18 --max-fps 30 \
  --frame-pacing stuttery \
  --screenshot ".git/compatibility-work/cusa00001/<capture>/screenshots/gameplay.png::Gameplay" \
  --log ".git/compatibility-work/cusa00001/<capture>/session-logs/<session>/shadps4.log::shadPS4 log"
```

For the Android system driver, use `--driver-type system`, omit all Turnip arguments,
and record the observed system driver version with `--driver-version`.

Validate before committing:

```bash
python3 scripts/compatibility/validate_database.py
```

## Status definitions

- `playable`: full completion verified without major game-breaking issues
- `ingame`: controllable gameplay reached, but major issues remain
- `menus`: functional menus reached, not gameplay
- `boots`: output appears before menus
- `nothing`: crash, hang, or black screen
