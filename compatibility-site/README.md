# Bachata S4 compatibility site

A static, dependency-free GitHub Pages frontend for Android game compatibility data.
The website loads `data/games.json` at runtime and supports:

- title, CUSA, device, SoC, GPU, and notes search
- status and GPU filtering
- sorting by recent test, title, compatibility, or average FPS
- responsive game cards and shareable `?game=<id>` details
- screenshots, device/driver/build data, performance, known issues, logs, and test history
- light/dark theme and accessible keyboard/dialog behavior

## Local preview

A web server is required because browsers normally block `fetch()` from `file://` URLs.

```bash
python3 -m http.server 8080 --directory compatibility-site
```

Then open `http://localhost:8080`.

## Database design

`data/games.json` is the source of truth. Each game has a stable `id`, a PS4 title ID,
and an ordered `tests` array. New tests are appended rather than overwriting old ones,
so compatibility changes remain auditable.

The latest `testedAt` entry determines the status shown on the main grid. The complete
history is visible in the game detail dialog.

## Adding reports

Do not edit paths or calculate hashes manually. Use:

```bash
python3 scripts/compatibility/add_report.py --help
```

Example:

```bash
python3 scripts/compatibility/add_report.py \
  --title "Example Game" \
  --serial CUSA00001 \
  --region US \
  --status ingame \
  --game-version "1.00" \
  --guest-backend fex \
  --notes "Reached controllable gameplay; major rendering issue after the intro." \
  --issue "Missing character textures" \
  --device-json .git/compatibility-work/cusa00001/20260730-120000/device.json \
  --renderer-driver "System Vulkan" \
  --driver-version "Adreno proprietary" \
  --average-fps 24 \
  --min-fps 18 \
  --max-fps 30 \
  --frame-pacing stuttery \
  --screenshot ".git/compatibility-work/cusa00001/20260730-120000/screenshots/cusa00001-01.png::Gameplay" \
  --log ".git/compatibility-work/cusa00001/20260730-120000/session-logs/latest/shadps4.log::shadPS4 session log"
```

Fields that were not measured must be omitted rather than estimated. Validate before committing:

```bash
python3 scripts/compatibility/validate_database.py
```

## Status definitions

- `playable`: full-game completion is verified with playable performance and no major game-breaking issues
- `ingame`: controllable gameplay is reached, but major issues remain
- `menus`: menus are reached, but gameplay is not
- `boots`: visual or audio output occurs before the menu
- `nothing`: launch crashes, hangs, or remains on a black screen

A title screen alone is not `ingame`, and a few minutes of gameplay are not enough to
claim `playable` for a long game.
