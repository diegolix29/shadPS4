#!/usr/bin/env python3
"""Synchronize the Bachata S4 GitHub release index used by the compatibility site."""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def iso_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def fetch_releases(repository: str, token: str = "") -> list[dict[str, Any]]:
    url = f"https://api.github.com/repos/{repository}/releases?per_page=100"
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "bachata-s4-compatibility-index",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            payload = json.load(response)
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        raise SystemExit(f"Unable to fetch GitHub releases: {exc}") from exc
    if not isinstance(payload, list):
        raise SystemExit("GitHub releases API returned an unexpected payload")
    return [item for item in payload if isinstance(item, dict) and not item.get("draft")]


def normalize(repository: str, raw: list[dict[str, Any]]) -> dict[str, Any]:
    latest_tag = next((str(item.get("tag_name", "")).strip() for item in raw if not item.get("prerelease") and item.get("tag_name")), "")
    releases: list[dict[str, Any]] = []
    for item in raw:
        tag = str(item.get("tag_name", "")).strip()
        if not tag:
            continue
        releases.append({
            "tag": tag,
            "name": str(item.get("name") or tag).strip(),
            "url": str(item.get("html_url") or f"https://github.com/{repository}/releases/tag/{tag}"),
            "publishedAt": str(item.get("published_at") or item.get("created_at") or iso_now()),
            "prerelease": bool(item.get("prerelease")),
            "latest": tag == latest_tag,
        })
    releases.sort(key=lambda item: item["publishedAt"], reverse=True)
    return {
        "schemaVersion": 1,
        "generatedAt": iso_now(),
        "repository": repository,
        "releases": releases,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", default="JICA98/Bachata-S4")
    parser.add_argument("--output", type=Path, default=Path("compatibility-site/data/releases.json"))
    parser.add_argument("--check", action="store_true", help="Fail if the checked-in index differs from GitHub")
    args = parser.parse_args()

    data = normalize(args.repository, fetch_releases(args.repository, os.environ.get("GITHUB_TOKEN", "")))
    serialized = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
    output = args.output.resolve()
    if args.check:
        try:
            existing_data = json.loads(output.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            existing_data = {}
        comparable_existing = {key: existing_data.get(key) for key in ("schemaVersion", "repository", "releases")}
        comparable_remote = {key: data.get(key) for key in ("schemaVersion", "repository", "releases")}
        if comparable_existing != comparable_remote:
            print(f"Release index is stale: {output}", file=sys.stderr)
            return 1
        print(f"OK: release index is current ({len(data['releases'])} releases)")
        return 0
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(serialized, encoding="utf-8")
    print(f"Updated {output} with {len(data['releases'])} published release(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
