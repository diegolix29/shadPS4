#!/usr/bin/env bash
# Build aarch64-glibc libvulkan_vortek.so for Bachata host runtime (not Android/Bionic NDK).
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
component_lock="$project_root/runtime/locks/components.lock.json"
source_dir="$project_root/runtime/sources/vortek-client"
server_dir="$project_root/runtime/sources/winlator-app/app/src/main/cpp/vortekrenderer"
build_dir="$project_root/runtime/build/vortek-client"
stage_dir="$project_root/runtime/build/vortek-client-stage"
overlay_main="$project_root/runtime/patches/vortek/bachata_main.c"
protocol_header="$project_root/runtime/vortek-protocol/bachata_vortek_protocol.h"
# Truthful after Task 8 Gates A–D (core 1.2/1.3 entry points + shad capability probe).
approved_api_version="1.3.0"

client_revision=$(
  node -e '
    const lock = require(process.argv[1]);
    const c = lock.components.find((x) => x.name === "vortek-client");
    if (!c) process.exit(1);
    process.stdout.write(c.revision);
  ' "$component_lock"
)
server_revision=$(
  node -e '
    const lock = require(process.argv[1]);
    const c = lock.components.find((x) => x.name === "vortek-server");
    if (!c) process.exit(1);
    process.stdout.write(c.revision);
  ' "$component_lock"
)

if [[ ! "$client_revision" =~ ^[0-9a-f]{40}$ ]]; then
  echo "invalid vortek-client revision in lock" >&2
  exit 1
fi

if [[ ! -d "$source_dir/.git" ]]; then
  echo "vortek-client not vendored; run runtime/scripts/vendor-vortek.sh first" >&2
  exit 1
fi
if [[ "$(git -C "$source_dir" rev-parse HEAD)" != "$client_revision" ]]; then
  echo "vortek-client checkout mismatch" >&2
  exit 1
fi
if [[ ! -d "$server_dir" ]]; then
  echo "missing server sources at $server_dir" >&2
  exit 1
fi

client_request_hash=$(sha256sum "$source_dir/include/request_codes.h" | cut -d' ' -f1)
server_request_hash=$(sha256sum "$server_dir/include/request_codes.h" | cut -d' ' -f1)
if [[ "$client_request_hash" != "$server_request_hash" ]]; then
  echo "Vortek protocol mismatch: client request_codes.h does not match pinned Winlator server." >&2
  exit 1
fi
client_serializer_hash=$(sha256sum "$source_dir/include/vortek_serializer.h" | cut -d' ' -f1)
server_serializer_hash=$(sha256sum "$server_dir/include/vortek_serializer.h" | cut -d' ' -f1)
if [[ "$client_serializer_hash" != "$server_serializer_hash" ]]; then
  echo "Vortek protocol mismatch: client vortek_serializer.h does not match pinned Winlator server." >&2
  exit 1
fi

command -v aarch64-linux-gnu-gcc >/dev/null || {
  echo "aarch64-linux-gnu-gcc required for glibc Vortek client" >&2
  exit 1
}

vulkan_include=""
for candidate in \
  /usr/include \
  "$project_root/runtime/sources/mesa/include" \
  /usr/aarch64-linux-gnu/include
do
  if [[ -f "$candidate/vulkan/vulkan.h" && -f "$candidate/vulkan/vk_icd.h" ]]; then
    vulkan_include=$candidate
    break
  fi
done
if [[ -z "$vulkan_include" ]]; then
  echo "Vulkan headers (vulkan/vulkan.h and vulkan/vk_icd.h) not found" >&2
  exit 1
fi

rm -rf "$build_dir" "$stage_dir"
mkdir -p "$build_dir/src" "$build_dir/include" \
  "$stage_dir/host/lib" "$stage_dir/host/vulkan/icd.d" \
  "$stage_dir/usr/share/bachata/vortek" \
  "$stage_dir/host-probe/lib" "$stage_dir/host-probe/vulkan/icd.d"

# Stage pure upstream sources then overlay Bachata client entrypoint + protocol header.
cp -a "$source_dir/src/." "$build_dir/src/"
cp -a "$source_dir/include/." "$build_dir/include/"
cp "$overlay_main" "$build_dir/src/main.c"
cp "$protocol_header" "$build_dir/include/bachata_vortek_protocol.h"
# Task 4 reuses the same protocol header from runtime/vortek-protocol/.

cflags_common=(
  -O2
  -fPIC
  -Wall
  -Wno-discarded-qualifiers
  -Wno-unused-function
  -Wno-stringop-truncation
  -DVK_USE_PLATFORM_XLIB_KHR
  -DBACHATA_VORTEK_CLIENT_BUILD_ID="\"$client_revision\""
  -I"$build_dir/include"
  -I"$vulkan_include"
)

sources=(
  src/main.c
  src/vulkan_calls.c
  src/vk_object.c
  src/vk_object_pool.c
  src/arrays.c
  src/descriptor_update_template.c
  src/ring_buffer.c
)

build_shared() {
  local cc=$1
  local out_lib=$2
  local obj_root=$3
  local -a objects=()
  mkdir -p "$obj_root"
  for src in "${sources[@]}"; do
    local obj="$obj_root/${src%.c}.o"
    mkdir -p "$(dirname "$obj")"
    "$cc" "${cflags_common[@]}" -c "$build_dir/$src" -o "$obj"
    objects+=("$obj")
  done
  if ! "$cc" -shared -o "$out_lib" "${objects[@]}" \
      -Wl,-soname,libvulkan_vortek.so \
      -Wl,--version-script="$project_root/runtime/patches/vortek/libvulkan_vortek.map"; then
    "$cc" -shared -o "$out_lib" "${objects[@]}" -Wl,-soname,libvulkan_vortek.so
  fi
  if command -v "${cc%-gcc}-strip" >/dev/null 2>&1; then
    "${cc%-gcc}-strip" --strip-unneeded "$out_lib" 2>/dev/null || true
  elif command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "$out_lib" 2>/dev/null || true
  fi
}

output_lib="$stage_dir/host/lib/libvulkan_vortek.so"
build_shared aarch64-linux-gnu-gcc "$output_lib" "$build_dir/obj-aarch64"

# Desktop probe helper (not packaged): same sources for host arch when not aarch64.
host_probe_lib="$stage_dir/host-probe/lib/libvulkan_vortek.so"
if [[ "$(uname -m)" != "aarch64" ]] && command -v gcc >/dev/null; then
  build_shared gcc "$host_probe_lib" "$build_dir/obj-host"
  cat >"$stage_dir/host-probe/vulkan/icd.d/vortek.json" <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../lib/libvulkan_vortek.so",
        "api_version": "$approved_api_version"
    }
}
EOF
fi

# ICD uses a path relative to the JSON file: host/vulkan/icd.d -> host/lib
icd_path="$stage_dir/host/vulkan/icd.d/vortek.json"
cat >"$icd_path" <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../lib/libvulkan_vortek.so",
        "api_version": "$approved_api_version"
    }
}
EOF

# Guard: Task 8 approved ICD is exactly 1.3.0 (no silent 1.4+ over-advertisement).
if ! grep -q '"api_version": "1.3.0"' "$icd_path"; then
  echo "ICD manifest api_version must be 1.3.0 after Task 8 capability gates" >&2
  exit 1
fi
if grep -qE '"api_version": "1\.(4|5|6)\.' "$icd_path"; then
  echo "ICD must not over-advertise beyond approved 1.3.0" >&2
  exit 1
fi
if grep -qi 'com.winlator' "$icd_path"; then
  echo "ICD manifest must not contain Winlator paths" >&2
  exit 1
fi

cp "$source_dir/LICENSE" "$stage_dir/usr/share/bachata/vortek/LICENSE"
cat >"$stage_dir/usr/share/bachata/vortek/SOURCE.txt" <<EOF
name=vortek-client
url=https://github.com/brunodev85/vortek.git
revision=$client_revision
license=LGPL-2.1
server_url=https://github.com/brunodev85/winlator-app.git
server_revision=$server_revision
server_path=app/src/main/cpp/vortekrenderer
built_from_source=true
target=aarch64-glibc
api_version=$approved_api_version
EOF

# Basic binary checks
file_out=$(file -b "$output_lib")
echo "$file_out" | grep -qi 'aarch64' || {
  echo "output is not aarch64: $file_out" >&2
  exit 1
}
if echo "$file_out" | grep -qi 'android'; then
  echo "output must not be an Android/Bionic binary" >&2
  exit 1
fi
# glibc marker
if ! readelf -d "$output_lib" | grep -q 'libc.so.6'; then
  echo "output does not link glibc libc.so.6" >&2
  exit 1
fi
if readelf -d "$output_lib" | grep -qi 'linker64\|bionic'; then
  echo "output appears to use Android linker" >&2
  exit 1
fi

exports=$(nm -D --defined-only "$output_lib" 2>/dev/null | awk '{print $3}' || true)
for sym in vk_icdGetInstanceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion; do
  if ! echo "$exports" | grep -qx "$sym"; then
    # some toolchains hide via version script; fall back to readelf
    if ! readelf -Ws "$output_lib" | grep -q "$sym"; then
      echo "missing ICD export: $sym" >&2
      exit 1
    fi
  fi
done

printf '[Bachata.Vortek.Build] client_commit=%s\n' "$client_revision"
printf '[Bachata.Vortek.Build] server_commit=%s\n' "$server_revision"
printf '[Bachata.Vortek.Build] protocol_match=true\n'
printf '[Bachata.Vortek.Build] target=aarch64-glibc\n'
printf '[Bachata.Vortek.Build] output=%s\n' "$output_lib"
printf '[Bachata.Vortek.Build] icd_manifest=%s\n' "$icd_path"
printf '[Bachata.Vortek.Build] api_version=%s\n' "$approved_api_version"
