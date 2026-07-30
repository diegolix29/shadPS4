#!/usr/bin/env python3
"""Validate the Bachata S4 compatibility and release JSON using only Python stdlib."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import datetime
from pathlib import Path
from typing import Any

STATUSES = {"playable", "ingame", "menus", "boots", "nothing"}
BACKENDS = {"fex", "box64", "native-arm64", "unknown"}
DRIVER_TYPES = {"system", "turnip", "custom"}
FRAME_PACING = {"stable", "mostly-stable", "stuttery", "severe-stutter", "unknown"}
SERIAL_RE = re.compile(r"^CUSA\d{5}$")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
COMMIT_RE = re.compile(r"^[0-9a-fA-F]{7,40}$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")
ROOT_KEYS = {"schemaVersion", "lastUpdated", "project", "games"}
PROJECT_KEYS = {"name", "repository", "platform", "releaseSource"}
GAME_KEYS = {"id", "title", "serial", "region", "publisher", "tests"}
TEST_KEYS = {
    "testedAt", "status", "gameVersion", "releaseTag", "emulatorVersion", "commit",
    "guestBackend", "summary", "notes", "issues", "device", "renderer",
    "performance", "settings", "screenshots", "logs", "videoUrl", "reportUrl", "tester",
}
DEVICE_KEYS = {"label", "manufacturer", "model", "soc", "gpu", "androidVersion", "ramGb"}
RENDERER_KEYS = {
    "driverType", "driver", "driverVersion", "turnipVersion", "turnipBuild",
    "turnipSource", "resolutionScale",
}
PERFORMANCE_KEYS = {"averageFps", "minFps", "maxFps", "framePacing", "testDurationSeconds"}
SCREENSHOT_KEYS = {"path", "caption"}
LOG_KEYS = {"path", "label", "sha256"}
RELEASE_ROOT_KEYS = {"schemaVersion", "generatedAt", "repository", "releases"}
RELEASE_KEYS = {"tag", "name", "url", "publishedAt", "prerelease", "latest"}


def parse_iso8601(value: Any, path: str, errors: list[str]) -> datetime | None:
    if not isinstance(value, str) or not value.strip():
        errors.append(f"{path}: expected a non-empty ISO-8601 string")
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        errors.append(f"{path}: invalid ISO-8601 date-time: {value!r}")
        return None


def require_string(obj: dict[str, Any], key: str, path: str, errors: list[str]) -> str:
    value = obj.get(key)
    if not isinstance(value, str) or not value.strip():
        errors.append(f"{path}.{key}: expected a non-empty string")
        return ""
    return value.strip()


def reject_unknown_keys(obj: dict[str, Any], allowed: set[str], path: str, errors: list[str]) -> None:
    for key in sorted(set(obj) - allowed):
        errors.append(f"{path}.{key}: unknown property")


def optional_number(obj: dict[str, Any], key: str, path: str, errors: list[str], *, integer: bool = False) -> None:
    if key not in obj:
        return
    value = obj[key]
    expected = int if integer else (int, float)
    if isinstance(value, bool) or not isinstance(value, expected) or value < 0:
        kind = "non-negative integer" if integer else "non-negative number"
        errors.append(f"{path}.{key}: expected a {kind}")


def validate_asset_path(value: Any, path: str, expected_prefix: str, site_root: Path, errors: list[str]) -> Path | None:
    if not isinstance(value, str) or not value.strip():
        errors.append(f"{path}: expected a non-empty path")
        return None
    normalized = value.replace("\\", "/")
    if normalized.startswith("/") or ".." in Path(normalized).parts:
        errors.append(f"{path}: path must be relative and cannot contain '..': {value!r}")
        return None
    if not normalized.startswith(expected_prefix):
        errors.append(f"{path}: expected a repository path under {expected_prefix!r}, got {value!r}")
        return None
    target = site_root / normalized
    try:
        resolved = target.resolve(strict=True)
        resolved.relative_to(site_root.resolve())
    except FileNotFoundError:
        errors.append(f"{path}: referenced file does not exist: {target}")
        return None
    except ValueError:
        errors.append(f"{path}: referenced file resolves outside the website: {target}")
        return None
    if not resolved.is_file():
        errors.append(f"{path}: referenced asset is not a file: {target}")
        return None
    return resolved


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_releases(path: Path) -> tuple[list[str], set[str]]:
    errors: list[str] = []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return [f"release index not found: {path}"], set()
    except json.JSONDecodeError as exc:
        return [f"invalid releases JSON at line {exc.lineno}, column {exc.colno}: {exc.msg}"], set()
    if not isinstance(data, dict):
        return ["releases root: expected an object"], set()
    reject_unknown_keys(data, RELEASE_ROOT_KEYS, "releases", errors)
    if data.get("schemaVersion") != 1:
        errors.append("releases.schemaVersion: expected integer 1")
    parse_iso8601(data.get("generatedAt"), "releases.generatedAt", errors)
    require_string(data, "repository", "releases", errors)
    releases = data.get("releases")
    if not isinstance(releases, list):
        errors.append("releases.releases: expected an array")
        return errors, set()
    tags: set[str] = set()
    latest_count = 0
    dates: list[datetime] = []
    for index, release in enumerate(releases):
        item_path = f"releases.releases[{index}]"
        if not isinstance(release, dict):
            errors.append(f"{item_path}: expected an object")
            continue
        reject_unknown_keys(release, RELEASE_KEYS, item_path, errors)
        tag = require_string(release, "tag", item_path, errors)
        require_string(release, "name", item_path, errors)
        require_string(release, "url", item_path, errors)
        published = parse_iso8601(release.get("publishedAt"), f"{item_path}.publishedAt", errors)
        if published is not None:
            dates.append(published)
        if tag in tags:
            errors.append(f"{item_path}.tag: duplicate tag {tag!r}")
        tags.add(tag)
        if not isinstance(release.get("prerelease"), bool):
            errors.append(f"{item_path}.prerelease: expected a boolean")
        if not isinstance(release.get("latest"), bool):
            errors.append(f"{item_path}.latest: expected a boolean")
        elif release["latest"]:
            latest_count += 1
            if release.get("prerelease"):
                errors.append(f"{item_path}: latest release cannot be a prerelease")
    if releases and latest_count != 1:
        errors.append(f"releases: expected exactly one latest stable release, found {latest_count}")
    if dates and dates != sorted(dates, reverse=True):
        errors.append("releases: entries must be sorted newest first")
    return errors, tags


def validate_test(test: Any, path: str, site_root: Path, release_tags: set[str], errors: list[str]) -> datetime | None:
    if not isinstance(test, dict):
        errors.append(f"{path}: expected an object")
        return None
    reject_unknown_keys(test, TEST_KEYS, path, errors)
    tested_at = parse_iso8601(test.get("testedAt"), f"{path}.testedAt", errors)
    status = require_string(test, "status", path, errors)
    if status and status not in STATUSES:
        errors.append(f"{path}.status: must be one of {sorted(STATUSES)}, got {status!r}")
    require_string(test, "gameVersion", path, errors)
    release_tag = require_string(test, "releaseTag", path, errors)
    if release_tag and release_tag not in release_tags:
        errors.append(f"{path}.releaseTag: {release_tag!r} is not present in data/releases.json")
    require_string(test, "emulatorVersion", path, errors)
    commit = require_string(test, "commit", path, errors)
    if commit and not COMMIT_RE.fullmatch(commit):
        errors.append(f"{path}.commit: expected 7-40 hexadecimal characters")
    require_string(test, "notes", path, errors)
    backend = test.get("guestBackend")
    if backend is not None and backend not in BACKENDS:
        errors.append(f"{path}.guestBackend: must be one of {sorted(BACKENDS)}")

    device = test.get("device")
    if not isinstance(device, dict):
        errors.append(f"{path}.device: expected an object")
    else:
        reject_unknown_keys(device, DEVICE_KEYS, f"{path}.device", errors)
        for key in ("label", "manufacturer", "model", "soc", "gpu", "androidVersion"):
            require_string(device, key, f"{path}.device", errors)
        if "ramGb" in device:
            value = device["ramGb"]
            if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
                errors.append(f"{path}.device.ramGb: expected a positive number")

    renderer = test.get("renderer")
    if not isinstance(renderer, dict):
        errors.append(f"{path}.renderer: expected an object")
    else:
        reject_unknown_keys(renderer, RENDERER_KEYS, f"{path}.renderer", errors)
        driver_type = require_string(renderer, "driverType", f"{path}.renderer", errors)
        if driver_type and driver_type not in DRIVER_TYPES:
            errors.append(f"{path}.renderer.driverType: must be one of {sorted(DRIVER_TYPES)}")
        require_string(renderer, "driver", f"{path}.renderer", errors)
        if driver_type == "turnip":
            require_string(renderer, "turnipVersion", f"{path}.renderer", errors)
        if "resolutionScale" in renderer:
            value = renderer["resolutionScale"]
            if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
                errors.append(f"{path}.renderer.resolutionScale: expected a positive number")

    performance = test.get("performance")
    if performance is not None:
        if not isinstance(performance, dict):
            errors.append(f"{path}.performance: expected an object")
        else:
            reject_unknown_keys(performance, PERFORMANCE_KEYS, f"{path}.performance", errors)
            for key in ("averageFps", "minFps", "maxFps"):
                optional_number(performance, key, f"{path}.performance", errors)
            optional_number(performance, "testDurationSeconds", f"{path}.performance", errors, integer=True)
            pacing = performance.get("framePacing")
            if pacing is not None and pacing not in FRAME_PACING:
                errors.append(f"{path}.performance.framePacing: must be one of {sorted(FRAME_PACING)}")
            values = (performance.get("minFps"), performance.get("averageFps"), performance.get("maxFps"))
            if all(isinstance(value, (int, float)) and not isinstance(value, bool) for value in values):
                if not values[0] <= values[1] <= values[2]:
                    errors.append(f"{path}.performance: expected minFps <= averageFps <= maxFps")

    issues = test.get("issues", [])
    if not isinstance(issues, list) or any(not isinstance(issue, str) or not issue.strip() for issue in issues):
        errors.append(f"{path}.issues: expected an array of non-empty strings")
    elif len(set(issues)) != len(issues):
        errors.append(f"{path}.issues: duplicate entries are not allowed")

    screenshots = test.get("screenshots", [])
    if not isinstance(screenshots, list) or not screenshots:
        errors.append(f"{path}.screenshots: expected at least one screenshot")
    else:
        for index, screenshot in enumerate(screenshots):
            if isinstance(screenshot, dict):
                reject_unknown_keys(screenshot, SCREENSHOT_KEYS, f"{path}.screenshots[{index}]", errors)
                if "caption" in screenshot and not isinstance(screenshot["caption"], str):
                    errors.append(f"{path}.screenshots[{index}].caption: expected a string")
            value = screenshot.get("path") if isinstance(screenshot, dict) else screenshot
            validate_asset_path(value, f"{path}.screenshots[{index}]", "assets/screenshots/", site_root, errors)

    logs = test.get("logs", [])
    if not isinstance(logs, list) or not logs:
        errors.append(f"{path}.logs: expected at least one log")
    else:
        for index, log in enumerate(logs):
            if isinstance(log, dict):
                reject_unknown_keys(log, LOG_KEYS, f"{path}.logs[{index}]", errors)
                if "label" in log and not isinstance(log["label"], str):
                    errors.append(f"{path}.logs[{index}].label: expected a string")
            value = log.get("path") if isinstance(log, dict) else log
            log_file = validate_asset_path(value, f"{path}.logs[{index}]", "assets/logs/", site_root, errors)
            if isinstance(log, dict) and "sha256" in log:
                digest = log["sha256"]
                if not isinstance(digest, str) or not SHA_RE.fullmatch(digest):
                    errors.append(f"{path}.logs[{index}].sha256: expected lowercase SHA-256")
                elif log_file is not None and sha256(log_file) != digest:
                    errors.append(f"{path}.logs[{index}].sha256: does not match {log_file}")
    return tested_at


def validate_database(database_path: Path, releases_path: Path | None = None) -> list[str]:
    errors: list[str] = []
    releases_path = releases_path or database_path.with_name("releases.json")
    release_errors, release_tags = validate_releases(releases_path)
    errors.extend(release_errors)
    try:
        data = json.loads(database_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return errors + [f"database not found: {database_path}"]
    except json.JSONDecodeError as exc:
        return errors + [f"invalid JSON at line {exc.lineno}, column {exc.colno}: {exc.msg}"]
    if not isinstance(data, dict):
        return errors + ["root: expected an object"]
    reject_unknown_keys(data, ROOT_KEYS, "root", errors)
    if data.get("schemaVersion") != 2:
        errors.append("schemaVersion: expected integer 2")
    last_updated = parse_iso8601(data.get("lastUpdated"), "lastUpdated", errors)
    project = data.get("project")
    if not isinstance(project, dict):
        errors.append("project: expected an object")
    else:
        reject_unknown_keys(project, PROJECT_KEYS, "project", errors)
        for key in ("name", "repository", "platform", "releaseSource"):
            require_string(project, key, "project", errors)
    games = data.get("games")
    if not isinstance(games, list):
        errors.append("games: expected an array")
        return errors

    seen_ids: set[str] = set()
    seen_serials: set[str] = set()
    all_dates: list[datetime] = []
    site_root = database_path.parent.parent
    for game_index, game in enumerate(games):
        path = f"games[{game_index}]"
        if not isinstance(game, dict):
            errors.append(f"{path}: expected an object")
            continue
        reject_unknown_keys(game, GAME_KEYS, path, errors)
        game_id = require_string(game, "id", path, errors)
        title = require_string(game, "title", path, errors)
        serial = require_string(game, "serial", path, errors).upper()
        if game_id and not ID_RE.fullmatch(game_id):
            errors.append(f"{path}.id: must match {ID_RE.pattern}")
        if game_id in seen_ids:
            errors.append(f"{path}.id: duplicate id {game_id!r}")
        seen_ids.add(game_id)
        if serial and not SERIAL_RE.fullmatch(serial):
            errors.append(f"{path}.serial: expected CUSA followed by five digits")
        if serial and game_id and game_id != serial.lower():
            errors.append(f"{path}.id: expected {serial.lower()!r} so IDs remain stable")
        if serial in seen_serials:
            errors.append(f"{path}.serial: duplicate serial {serial!r}")
        seen_serials.add(serial)
        if not title:
            continue
        tests = game.get("tests")
        if not isinstance(tests, list) or not tests:
            errors.append(f"{path}.tests: expected at least one test")
            continue
        dates: list[datetime] = []
        signatures: set[tuple[str, str, str, str]] = set()
        for test_index, test in enumerate(tests):
            test_path = f"{path}.tests[{test_index}]"
            parsed = validate_test(test, test_path, site_root, release_tags, errors)
            if parsed is not None:
                dates.append(parsed)
                all_dates.append(parsed)
            if isinstance(test, dict):
                device_label = str(test.get("device", {}).get("label", "")) if isinstance(test.get("device"), dict) else ""
                signature = (
                    str(test.get("releaseTag", "")), str(test.get("testedAt", "")),
                    str(test.get("commit", "")), device_label,
                )
                if signature in signatures:
                    errors.append(f"{test_path}: duplicate release/testedAt/commit/device report")
                signatures.add(signature)
        if dates and dates != sorted(dates, reverse=True):
            errors.append(f"{path}.tests: reports must be sorted newest first")
    if last_updated is not None and all_dates and last_updated < max(all_dates):
        errors.append("lastUpdated: cannot be older than the newest test")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", nargs="?", type=Path, default=Path("compatibility-site/data/games.json"))
    parser.add_argument("--releases", type=Path, default=None)
    args = parser.parse_args()
    database = args.database.resolve()
    releases = args.releases.resolve() if args.releases else database.with_name("releases.json")
    errors = validate_database(database, releases)
    if errors:
        print(f"Compatibility database validation failed with {len(errors)} error(s):", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    data = json.loads(database.read_text(encoding="utf-8"))
    reports = sum(len(game.get("tests", [])) for game in data.get("games", []))
    release_count = len(json.loads(releases.read_text(encoding="utf-8")).get("releases", []))
    print(f"OK: {len(data.get('games', []))} game(s), {reports} report(s), {release_count} release(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
