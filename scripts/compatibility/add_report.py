#!/usr/bin/env python3
"""Add or update a Bachata S4 game compatibility report and copy its evidence."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from validate_database import validate_database

SERIAL_RE = re.compile(r"^CUSA\d{5}$")
SAFE_RE = re.compile(r"[^a-zA-Z0-9._-]+")


def command_output(*args: str, cwd: Path) -> str:
    try:
        return subprocess.check_output(args, cwd=cwd, text=True, stderr=subprocess.DEVNULL).strip()
    except (subprocess.SubprocessError, FileNotFoundError):
        return ""


def repo_root() -> Path:
    script_root = Path(__file__).resolve().parents[2]
    discovered = command_output("git", "rev-parse", "--show-toplevel", cwd=script_root)
    return Path(discovered).resolve() if discovered else script_root


def iso_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def parse_iso(value: str) -> str:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid ISO-8601 date-time: {value}") from exc
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def clean_name(value: str) -> str:
    cleaned = SAFE_RE.sub("-", value.strip()).strip("-._").lower()
    return cleaned or "evidence"


def load_json_object(value: str | None, label: str) -> dict[str, Any]:
    if not value:
        return {}
    candidate = Path(value)
    try:
        raw = candidate.read_text(encoding="utf-8") if candidate.is_file() else value
        parsed = json.loads(raw)
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"Unable to parse {label} JSON: {exc}") from exc
    if not isinstance(parsed, dict):
        raise SystemExit(f"{label} JSON must be an object")
    return parsed


def split_evidence(value: str, fallback_label: str) -> tuple[Path, str]:
    source, separator, label = value.partition("::")
    return Path(source).expanduser().resolve(), (label.strip() if separator and label.strip() else fallback_label)


def ensure_under_site(path: Path, site_root: Path, prefix: str) -> str | None:
    try:
        relative = path.resolve().relative_to(site_root.resolve()).as_posix()
    except ValueError:
        return None
    return relative if relative.startswith(prefix) else None


def copy_screenshot(spec: str, site_root: Path, serial: str, stamp: str, index: int) -> tuple[dict[str, str], Path | None]:
    source, caption = split_evidence(spec, f"Screenshot {index}")
    if not source.is_file():
        raise SystemExit(f"Screenshot does not exist: {source}")
    existing = ensure_under_site(source, site_root, "assets/screenshots/")
    if existing:
        return {"path": existing, "caption": caption}, None
    extension = source.suffix.lower()
    if extension not in {".png", ".jpg", ".jpeg", ".webp"}:
        raise SystemExit(f"Unsupported screenshot format {extension!r}: {source}")
    destination_dir = site_root / "assets" / "screenshots" / serial.lower()
    destination_dir.mkdir(parents=True, exist_ok=True)
    destination = destination_dir / f"{stamp}-{index:02d}{extension}"
    if destination.exists():
        raise SystemExit(f"Screenshot destination already exists: {destination}")
    shutil.copy2(source, destination)
    return {"path": destination.relative_to(site_root).as_posix(), "caption": caption}, destination


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy_log(spec: str, site_root: Path, serial: str, stamp: str, index: int) -> tuple[dict[str, str], Path | None]:
    source, label = split_evidence(spec, f"Session log {index}")
    if not source.is_file():
        raise SystemExit(f"Log does not exist: {source}")
    existing = ensure_under_site(source, site_root, "assets/logs/")
    if existing:
        return {"path": existing, "label": label, "sha256": sha256(source)}, None

    destination_dir = site_root / "assets" / "logs" / serial.lower()
    destination_dir.mkdir(parents=True, exist_ok=True)
    base = clean_name(source.name.removesuffix(".gz").removesuffix(".log"))
    destination = destination_dir / f"{stamp}-{index:02d}-{base}.log.gz"
    if destination.exists():
        raise SystemExit(f"Log destination already exists: {destination}")
    if source.suffix.lower() == ".gz":
        shutil.copy2(source, destination)
    else:
        with source.open("rb") as input_handle, destination.open("wb") as raw_output:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw_output, compresslevel=9, mtime=0) as output_handle:
                shutil.copyfileobj(input_handle, output_handle, length=1024 * 1024)
    return {"path": destination.relative_to(site_root).as_posix(), "label": label, "sha256": sha256(destination)}, destination


def compact_dict(value: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, item in value.items():
        if item is None or item == "" or item == [] or item == {}:
            continue
        result[key] = compact_dict(item) if isinstance(item, dict) else item
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--title", required=True)
    parser.add_argument("--serial", required=True, help="PS4 title ID, for example CUSA00900")
    parser.add_argument("--region", default="")
    parser.add_argument("--publisher", default="")
    parser.add_argument("--status", required=True, choices=("playable", "ingame", "menus", "boots", "nothing"))
    parser.add_argument("--game-version", required=True)
    parser.add_argument("--emulator-version", default="")
    parser.add_argument("--commit", default="")
    parser.add_argument("--tested-at", type=parse_iso, default=iso_now())
    parser.add_argument("--guest-backend", choices=("fex", "box64", "native-arm64", "unknown"), default="unknown")
    parser.add_argument("--summary", default="")
    parser.add_argument("--notes", required=True)
    parser.add_argument("--issue", action="append", default=[], help="Repeat for each known issue")
    parser.add_argument("--tester", default="")

    parser.add_argument("--device-json", help="JSON file/object containing manufacturer, model, soc, gpu, androidVersion, ramGb")
    parser.add_argument("--manufacturer", default="")
    parser.add_argument("--model", default="")
    parser.add_argument("--soc", default="")
    parser.add_argument("--gpu", default="")
    parser.add_argument("--android-version", default="")
    parser.add_argument("--ram-gb", type=float)

    parser.add_argument("--renderer-driver", required=True)
    parser.add_argument("--driver-version", default="")
    parser.add_argument("--resolution-scale", type=float)
    parser.add_argument("--average-fps", type=float)
    parser.add_argument("--min-fps", type=float)
    parser.add_argument("--max-fps", type=float)
    parser.add_argument("--frame-pacing", choices=("stable", "mostly-stable", "stuttery", "severe-stutter", "unknown"))
    parser.add_argument("--test-duration-seconds", type=int)
    parser.add_argument("--settings-json", help="JSON file/object with compatibility-relevant settings")

    parser.add_argument("--screenshot", action="append", required=True, metavar="PATH[::CAPTION]", help="Repeat to copy screenshots")
    parser.add_argument("--log", action="append", required=True, metavar="PATH[::LABEL]", help="Repeat to gzip and copy logs")
    parser.add_argument("--video-url", default="")
    parser.add_argument("--report-url", default="")
    parser.add_argument("--database", type=Path, default=None)
    parser.add_argument("--dry-run", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    root = repo_root()
    site_root = root / "compatibility-site"
    database_path = args.database.resolve() if args.database else site_root / "data" / "games.json"
    serial = args.serial.strip().upper()
    if not SERIAL_RE.fullmatch(serial):
        raise SystemExit("--serial must match CUSA followed by five digits, e.g. CUSA00900")
    if not database_path.is_file():
        raise SystemExit(f"Database not found: {database_path}")

    commit = args.commit.strip() or command_output("git", "rev-parse", "HEAD", cwd=root)
    emulator_version = args.emulator_version.strip() or command_output("git", "describe", "--tags", "--always", "--dirty", cwd=root)
    if not commit:
        raise SystemExit("Unable to determine commit; provide --commit")
    if not emulator_version:
        raise SystemExit("Unable to determine emulator version; provide --emulator-version")

    device = load_json_object(args.device_json, "device")
    cli_device = {
        "manufacturer": args.manufacturer,
        "model": args.model,
        "soc": args.soc,
        "gpu": args.gpu,
        "androidVersion": args.android_version,
        "ramGb": args.ram_gb,
    }
    for key, value in cli_device.items():
        if value not in (None, ""):
            device[key] = value
    missing_device = [key for key in ("manufacturer", "model", "soc", "gpu", "androidVersion") if not str(device.get(key, "")).strip()]
    if missing_device:
        raise SystemExit(f"Missing device fields: {', '.join(missing_device)}. Use --device-json or individual flags.")

    stamp = args.tested_at.replace("-", "").replace(":", "").replace("T", "-").replace("Z", "")
    screenshot_results = [copy_screenshot(value, site_root, serial, stamp, index) for index, value in enumerate(args.screenshot, 1)]
    log_results = [copy_log(value, site_root, serial, stamp, index) for index, value in enumerate(args.log, 1)]
    screenshots = [entry for entry, _ in screenshot_results]
    logs = [entry for entry, _ in log_results]
    created_evidence = [path for _, path in screenshot_results + log_results if path is not None]

    renderer = compact_dict({
        "driver": args.renderer_driver,
        "driverVersion": args.driver_version,
        "resolutionScale": args.resolution_scale,
    })
    performance = compact_dict({
        "averageFps": args.average_fps,
        "minFps": args.min_fps,
        "maxFps": args.max_fps,
        "framePacing": args.frame_pacing,
        "testDurationSeconds": args.test_duration_seconds,
    })
    settings = load_json_object(args.settings_json, "settings")

    report = compact_dict({
        "testedAt": args.tested_at,
        "status": args.status,
        "gameVersion": args.game_version.strip(),
        "emulatorVersion": emulator_version,
        "commit": commit,
        "guestBackend": args.guest_backend,
        "summary": args.summary.strip(),
        "notes": args.notes.strip(),
        "issues": list(dict.fromkeys(issue.strip() for issue in args.issue if issue.strip())),
        "device": device,
        "renderer": renderer,
        "performance": performance,
        "settings": settings,
        "screenshots": screenshots,
        "logs": logs,
        "videoUrl": args.video_url.strip(),
        "reportUrl": args.report_url.strip(),
        "tester": args.tester.strip(),
    })

    database = json.loads(database_path.read_text(encoding="utf-8"))
    games = database.setdefault("games", [])
    game = next((item for item in games if str(item.get("serial", "")).upper() == serial), None)
    if game is None:
        game = {
            "id": serial.lower(),
            "title": args.title.strip(),
            "serial": serial,
            "tests": [],
        }
        games.append(game)
    else:
        game["title"] = args.title.strip()
    if args.region.strip():
        game["region"] = args.region.strip()
    if args.publisher.strip():
        game["publisher"] = args.publisher.strip()

    duplicate = next((item for item in game["tests"] if item.get("testedAt") == report["testedAt"] and item.get("commit") == report["commit"]), None)
    if duplicate:
        raise SystemExit(f"A report for {serial} already exists at {report['testedAt']} on commit {report['commit']}")
    game["tests"].append(report)
    game["tests"].sort(key=lambda item: item.get("testedAt", ""), reverse=True)
    games.sort(key=lambda item: str(item.get("title", "")).casefold())
    database["lastUpdated"] = iso_now()

    serialized = json.dumps(database, indent=2, ensure_ascii=False) + "\n"
    if args.dry_run:
        print(serialized)
        for path in created_evidence:
            path.unlink(missing_ok=True)
        return 0

    backup = database_path.with_suffix(database_path.suffix + ".bak")
    shutil.copy2(database_path, backup)
    database_path.write_text(serialized, encoding="utf-8")
    errors = validate_database(database_path)
    if errors:
        shutil.move(backup, database_path)
        for path in created_evidence:
            path.unlink(missing_ok=True)
        print("Report was rejected; database restored:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    backup.unlink(missing_ok=True)
    print(f"Added {args.status} report for {args.title} ({serial})")
    print(f"Database: {database_path.relative_to(root) if database_path.is_relative_to(root) else database_path}")
    for screenshot in screenshots:
        print(f"Screenshot: {screenshot['path']}")
    for log in logs:
        print(f"Log: {log['path']} ({log['sha256'][:12]}…)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
