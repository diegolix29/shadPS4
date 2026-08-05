#!/usr/bin/env bash
set -euo pipefail

# Reproducibility test for the Box64 host binary.
#
# Builds the exact same box64 source twice at different absolute paths and
# wall-clock times: tree A keeps usable nested git metadata, tree B has broken
# nested git metadata (the condition that used to empty the embedded GITREV).
# Both use the same SOURCE_DATE_EPOCH with ccache disabled and clean build
# dirs. The resulting binaries must be byte-identical.
#
# Run inside the build container (needs aarch64-linux-gnu-gcc, cmake, ninja):
#   bash runtime/tests/verify-box64-reproducible.sh
#
# Overridable via environment: REPRO_A_ROOT, REPRO_B_ROOT, REPRO_REPORT.

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
script_path="$project_root/runtime/scripts/build-box64-host.sh"
box64_source="$project_root/runtime/sources/box64"
expected_revision=50c8b90b09b433ab0767de44af2d0731cb0748b7

a_root=${REPRO_A_ROOT:-/tmp/bachata-repro-a}
b_root=${REPRO_B_ROOT:-/var/tmp/bachata-repro-b}
report=${REPRO_REPORT:-/tmp/bachata-repro-report.txt}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

require_tools() {
  local tool
  for tool in "$@"; do
    command -v "$tool" >/dev/null || fail "$tool is required"
  done
}

require_tools git cp cmp sha256sum diff date

[[ -f "$script_path" ]] || fail "build script not found: $script_path"
[[ -e "$box64_source/.git" ]] || fail "box64 source has no git metadata; cannot build tree A"
git -C "$box64_source" rev-parse HEAD >/dev/null 2>&1 || fail "box64 source git metadata is unusable; cannot build tree A"
[[ "$(git -C "$box64_source" rev-parse HEAD)" == "$expected_revision" ]] ||
  fail "box64 source is not at the pinned revision $expected_revision"

source_date_epoch=$(git -C "$project_root" show -s --format=%ct HEAD)
[[ "$source_date_epoch" =~ ^[0-9]+$ ]] || fail "cannot derive SOURCE_DATE_EPOCH from $project_root"
echo "source_date_epoch=$source_date_epoch ($(date -u -d "@$source_date_epoch" '+%Y-%m-%d %H:%M:%S UTC'))"

setup_tree() {
  local tree=$1
  local break_git=$2
  rm -rf "$tree"
  mkdir -p "$tree/runtime/scripts" "$tree/runtime/patches"
  git clone --quiet --no-checkout "$box64_source" "$tree/runtime/sources/box64"
  git -C "$tree/runtime/sources/box64" checkout --quiet "$expected_revision"
  cp "$script_path" "$tree/runtime/scripts/"
  chmod +x "$tree/runtime/scripts/build-box64-host.sh"
  cp "$project_root"/runtime/patches/box64-*.patch "$tree/runtime/patches/"
  if [[ "$break_git" == 1 ]]; then
    printf 'ref: refs/heads/missing\n' >"$tree/runtime/sources/box64/.git/HEAD"
    if git -C "$tree/runtime/sources/box64" rev-parse HEAD >/dev/null 2>&1; then
      fail "tree $tree still has usable git metadata"
    fi
  else
    git -C "$tree/runtime/sources/box64" rev-parse HEAD >/dev/null 2>&1 || fail "tree $tree lost its git metadata"
  fi
}

echo "==> preparing tree A (git metadata intact): $a_root"
setup_tree "$a_root" 0
echo "==> preparing tree B (broken nested git metadata): $b_root"
setup_tree "$b_root" 1

diff -r --exclude=.git "$a_root/runtime/sources/box64" "$b_root/runtime/sources/box64" \
  || fail "box64 source trees differ before building"

build_tree() {
  local tree=$1
  local allow_no_git=$2
  (cd "$tree" && CCACHE_DISABLE=1 SOURCE_DATE_EPOCH="$source_date_epoch" \
    BOX64_ALLOW_NO_GIT="$allow_no_git" runtime/scripts/build-box64-host.sh)
}

echo "==> building tree A"
a_output=$(build_tree "$a_root" 0) || fail "tree A build failed"
echo "==> building tree B"
b_output=$(build_tree "$b_root" 1) || fail "tree B build failed"

a_binary=$(sed -n 's/^box64_host=//p' <<<"$a_output")
b_binary=$(sed -n 's/^box64_host=//p' <<<"$b_output")
[[ -f "$a_binary" ]] || fail "tree A binary missing: $a_binary"
[[ -f "$b_binary" ]] || fail "tree B binary missing: $b_binary"

a_sha256=$(sha256sum "$a_binary" | cut -d' ' -f1)
b_sha256=$(sha256sum "$b_binary" | cut -d' ' -f1)

echo "box64_host_a=$a_binary"
echo "box64_host_b=$b_binary"
echo "sha256_a=$a_sha256"
echo "sha256_b=$b_sha256"

if [[ "$a_sha256" != "$b_sha256" ]]; then
  cmp -l "$a_binary" "$b_binary" | head -n 20 || true
  if command -v diffoscope >/dev/null 2>&1; then
    diffoscope --text "$report" "$a_binary" "$b_binary" || true
    echo "diffoscope report written to $report"
  else
    echo "diffoscope is not installed; install it for a detailed report" >&2
  fi
  fail "box64 host binaries differ (tree A $a_sha256 vs tree B $b_sha256)"
fi

echo "PASS: box64 host binary is byte-identical across git metadata states (sha256=$a_sha256)"
