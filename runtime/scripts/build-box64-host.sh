#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
script_path="$project_root/runtime/scripts/build-box64-host.sh"
source_dir="$project_root/runtime/sources/box64"
build_dir="$project_root/runtime/build/box64-host"
stage_dir="$project_root/runtime/build/box64-host-stage"
cache_manifest="$stage_dir/cache.manifest"
expected_revision=50c8b90b09b433ab0767de44af2d0731cb0748b7
quick_exit_patch="$project_root/runtime/patches/box64-cxa-quick-exit.patch"
vulkan_qcom_patch="$project_root/runtime/patches/box64-vulkan-dispatch-tile-qcom.patch"
vex_write_opcode_patch="$project_root/runtime/patches/box64-vex-write-opcode.patch"
native_write_opcode_patch="$project_root/runtime/patches/box64-native-write-opcode.patch"
bachata_thread_affinity_patch="$project_root/runtime/patches/box64-bachata-thread-affinity.patch"
patches=(
  "$quick_exit_patch"
  "$vulkan_qcom_patch"
  "$vex_write_opcode_patch"
  "$native_write_opcode_patch"
  "$bachata_thread_affinity_patch"
)

fail() {
  echo "$*" >&2
  exit 1
}

require_tools() {
  local tool
  for tool in "$@"; do
    command -v "$tool" >/dev/null || fail "$tool is required"
  done
}

apply_patch_once() {
  local patch=$1
  if ! git -C "$source_dir" apply --reverse --check "$patch" 2>/dev/null; then
    git -C "$source_dir" apply --check "$patch"
    git -C "$source_dir" apply "$patch"
  fi
}

compute_inputs_sha256() {
  {
    printf 'revision=%s\n' "$expected_revision"
    printf 'script_sha256=%s\n' "$(sha256sum "$script_path" | cut -d' ' -f1)"
    local patch
    for patch in "${patches[@]}"; do
      printf 'patch=%s sha256=%s\n' "${patch#"$project_root/"}" \
        "$(sha256sum "$patch" | cut -d' ' -f1)"
    done
    git -C "$source_dir" diff --binary --no-ext-diff HEAD
    while IFS= read -r -d '' path; do
      printf 'untracked=%s sha256=%s\n' "$path" \
        "$(sha256sum "$source_dir/$path" | cut -d' ' -f1)"
    done < <(git -C "$source_dir" ls-files --others --exclude-standard -z | LC_ALL=C sort -z)
  } | sha256sum | cut -d' ' -f1
}

validate_box64() {
  local binary=$1
  local description=$2
  local header dynamic unsupported
  [[ -f "$binary" ]] || fail "$description not found: $binary"
  [[ -x "$binary" ]] || fail "$description is not executable: $binary"
  if ! header=$(readelf -h "$binary" 2>&1); then
    fail "$description has an unreadable ELF header: $binary"
  fi
  if ! grep -Eq 'Class:[[:space:]]+ELF64' <<<"$header" ||
      ! grep -Eq 'Machine:[[:space:]]+AArch64' <<<"$header"; then
    fail "$description is not ELF64 AArch64: $binary"
  fi
  if ! dynamic=$(readelf -d "$binary" 2>&1); then
    fail "$description has an unreadable dynamic section: $binary"
  fi
  unsupported=$(sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' <<<"$dynamic" \
    | grep -Ev '^(libc\.so\.6|libm\.so\.6|libresolv\.so\.2|ld-linux-aarch64\.so\.1)$' || true)
  [[ -z "$unsupported" ]] || fail "$description has unsupported dynamic dependencies: $unsupported"
}

require_tools git sha256sum readelf

skip_box64=${BACHATA_SKIP_BOX64_BUILD:-0}
[[ "$skip_box64" == 0 || "$skip_box64" == 1 ]] ||
  fail "BACHATA_SKIP_BOX64_BUILD must be unset, 0, or 1"

actual_revision=$(git -C "$source_dir" rev-parse HEAD)
[[ "$actual_revision" == "$expected_revision" ]] ||
  fail "Box64 revision mismatch: expected $expected_revision, found $actual_revision"
for patch in "${patches[@]}"; do
  apply_patch_once "$patch"
done
inputs_sha256=$(compute_inputs_sha256)

if [[ "$skip_box64" == 1 ]]; then
  binary="$stage_dir/box64"
  [[ -f "$binary" ]] || fail "Cached Box64 artifact not found: $binary"
  [[ -f "$cache_manifest" ]] || fail "Cached Box64 manifest not found: $cache_manifest; rebuild without BACHATA_SKIP_BOX64_BUILD"
  cache_format=$(sed -n 's/^format=//p' "$cache_manifest")
  cache_revision=$(sed -n 's/^revision=//p' "$cache_manifest")
  cache_inputs_sha256=$(sed -n 's/^inputs_sha256=//p' "$cache_manifest")
  cache_binary_sha256=$(sed -n 's/^binary_sha256=//p' "$cache_manifest")
  [[ "$cache_format" == 1 ]] || fail "Cached Box64 manifest has an unsupported format"
  [[ "$cache_revision" == "$expected_revision" ]] || fail "Cached Box64 revision mismatch"
  [[ "$cache_inputs_sha256" =~ ^[0-9a-f]{64}$ ]] || fail "Cached Box64 manifest has an invalid input hash"
  [[ "$cache_inputs_sha256" == "$inputs_sha256" ]] ||
    fail "Cached Box64 build inputs changed; rebuild without BACHATA_SKIP_BOX64_BUILD"
  [[ "$cache_binary_sha256" =~ ^[0-9a-f]{64}$ ]] || fail "Cached Box64 manifest has an invalid binary hash"
  actual_binary_sha256=$(sha256sum "$binary" | cut -d' ' -f1)
  [[ "$actual_binary_sha256" == "$cache_binary_sha256" ]] || fail "Cached Box64 artifact hash mismatch"
  validate_box64 "$binary" "Cached Box64 artifact"
  printf 'box64_host=%s\nrevision=%s\nbox64_cache=reused\n' \
    "$binary" "$expected_revision"
  exit 0
fi

require_tools cc cmake ninja aarch64-linux-gnu-gcc aarch64-linux-gnu-g++

mkdir -p "$build_dir"
mkdir -p "$source_dir/tests"
[[ -f "$source_dir/tests/box64-bash" ]] || touch "$source_dir/tests/box64-bash"
affinity_test="$build_dir/test_bachata_thread_affinity"
cc -Wall -Wextra -Werror "$source_dir/tests/test_bachata_thread_affinity.c" -o "$affinity_test"
"$affinity_test"

cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  -DARM64=ON \
  -DARM_DYNAREC=ON \
  -DBAD_SIGNAL=ON \
  -DNO_LIB_INSTALL=ON \
  -DNO_CONF_INSTALL=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target box64

binary="$build_dir/box64"
validate_box64 "$binary" "Box64 host binary"

stage_tmp="$stage_dir.tmp.$$"
trap 'rm -rf "$stage_tmp"' EXIT
rm -rf "$stage_tmp"
mkdir -p "$stage_tmp"
install -m755 "$binary" "$stage_tmp/box64"
binary_sha256=$(sha256sum "$stage_tmp/box64" | cut -d' ' -f1)
printf 'format=1\nrevision=%s\ninputs_sha256=%s\nbinary_sha256=%s\n' \
  "$expected_revision" "$inputs_sha256" "$binary_sha256" >"$stage_tmp/cache.manifest"
rm -rf "$stage_dir"
mv "$stage_tmp" "$stage_dir"
trap - EXIT
printf 'box64_host=%s\nrevision=%s\nbox64_cache=rebuilt\n' \
  "$stage_dir/box64" "$expected_revision"
