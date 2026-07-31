#!/usr/bin/env python3
"""Regenerate the Bachata S4 status summary table in the repo README.

The table is delimited by HTML comment markers so it can be replaced
safely without touching the rest of the README.

  <!-- compatibility-status-table -->
  ...
  <!-- compatibility-status-table -->

Run after adding a report to compatibility-site/data/games.json:
  python3 scripts/compatibility/update_readme.py
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

STATUS_ORDER = ("playable", "ingame", "menus", "boots", "nothing")
OPEN_MARKER = "<!-- compatibility-status-table -->"
CLOSE_MARKER = "<!-- compatibility-status-table -->"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def build_table(counts: Counter[str]) -> str:
    rows = [f"| Status | Reports |", f"|---|---|"]
    for status in STATUS_ORDER:
        rows.append(f"| `{status}` | {counts.get(status, 0)} |")
    return "\n".join(rows)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--readme", type=Path, default=None,
        help="Path to the repo README; defaults to <repo root>/README.md",
    )
    parser.add_argument(
        "--database", type=Path, default=None,
        help="Path to games.json; defaults to <repo root>/compatibility-site/data/games.json",
    )
    args = parser.parse_args()

    root = repo_root()
    readme_path = (args.readme or root / "README.md").resolve()
    database_path = (args.database or root / "compatibility-site" / "data" / "games.json").resolve()

    if not readme_path.is_file():
        print(f"README not found: {readme_path}", file=sys.stderr)
        return 1
    if not database_path.is_file():
        print(f"Database not found: {database_path}", file=sys.stderr)
        return 1

    database = json.loads(database_path.read_text(encoding="utf-8"))
    counts = Counter(t["status"] for game in database.get("games", []) for t in game.get("tests", []))

    readme = readme_path.read_text(encoding="utf-8")
    if OPEN_MARKER not in readme or CLOSE_MARKER not in readme:
        print("README status table markers not found; add a Compatibility section first.", file=sys.stderr)
        return 1
    start = readme.index(OPEN_MARKER) + len(OPEN_MARKER)
    end = readme.index(CLOSE_MARKER, start)
    if end < start:
        print("Malformed README status table markers.", file=sys.stderr)
        return 1

    block = readme[start:end]
    if not block.startswith("\n"):
        print("OPEN_MARKER must be immediately followed by a newline.", file=sys.stderr)
        return 1
    if not block.endswith("\n"):
        print("CLOSE_MARKER must be immediately preceded by a newline.", file=sys.stderr)
        return 1
    block = block[len("\n"):-len("\n")]

    table = build_table(counts)
    if block == table:
        print("Status summary already up to date.")
        return 0

    new_readme = readme[:start] + "\n" + table + "\n" + readme[end:]
    readme_path.write_text(new_readme, encoding="utf-8")
    print("Updated README status summary:")
    for status in STATUS_ORDER:
        print(f"  {status}: {counts.get(status, 0)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
