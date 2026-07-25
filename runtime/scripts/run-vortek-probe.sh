#!/usr/bin/env bash
# Run the Vortek ICD probe.
# - Production path: aarch64 ICD under host glibc loader (device / qemu-aarch64).
# - Desktop fallback without qemu: host-arch ICD built only for probe (not packaged).
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
stage_dir="${VORTEK_STAGE_DIR:-$project_root/runtime/build/vortek-client-stage}"
rootfs="${VORTEK_ROOTFS:-$project_root/runtime/build/rootfs}"
probe_build="$project_root/runtime/build/vortek-probe"
socket_path="${BACHATA_VORTEK_SOCKET:-/tmp/bachata-vortek-probe.sock}"
host_dir="$rootfs/host"

if [[ ! -f "$stage_dir/host/lib/libvulkan_vortek.so" ]]; then
  echo "Vortek client stage missing; run runtime/scripts/build-vortek-client.sh first" >&2
  exit 1
fi

mkdir -p "$probe_build"
rm -f "$socket_path"
export BACHATA_VORTEK_SOCKET="$socket_path"
export BACHATA_VORTEK_LOG_LEVEL="${BACHATA_VORTEK_LOG_LEVEL:-1}"
export BACHATA_VORTEK_HANDSHAKE="${BACHATA_VORTEK_HANDSHAKE:-1}"
export VK_LOADER_DEBUG="${VK_LOADER_DEBUG:-error}"
unset MESA_VK_DEVICE_SELECT || true

build_probe() {
  local cc=$1
  local out=$2
  local vulkan_include=/usr/include
  if [[ ! -f "$vulkan_include/vulkan/vulkan.h" ]]; then
    vulkan_include="$project_root/runtime/sources/mesa/include"
  fi
  "$cc" -O2 -fno-ident \
    -I"$vulkan_include" \
    "$project_root/runtime/tests/vortek_probe/vortek_probe.c" \
    -o "$out" \
    -ldl
}

run_with_env() {
  local status
  set +e
  "$@"
  status=$?
  set -e
  echo "[Vortek.Probe] exit_code=$status"
  return "$status"
}

echo "[Vortek.Probe] configured_socket=$socket_path"

# Preferred: aarch64 production ICD via qemu or native aarch64.
if [[ "$(uname -m)" == "aarch64" ]]; then
  if [[ ! -f "$host_dir/libvulkan.so.1" || ! -f "$host_dir/ld-linux-aarch64.so.1" ]]; then
    echo "host rootfs incomplete under $host_dir" >&2
    exit 1
  fi
  build_probe aarch64-linux-gnu-gcc "$probe_build/vortek_probe_aarch64"
  export VK_ICD_FILENAMES="$stage_dir/host/vulkan/icd.d/vortek.json"
  echo "[Vortek.Probe] icd=$VK_ICD_FILENAMES"
  run_with_env "$host_dir/ld-linux-aarch64.so.1" \
    --library-path "$stage_dir/host/lib:$host_dir" \
    "$probe_build/vortek_probe_aarch64"
  exit $?
fi

if command -v qemu-aarch64 >/dev/null && [[ -f "$host_dir/ld-linux-aarch64.so.1" ]]; then
  build_probe aarch64-linux-gnu-gcc "$probe_build/vortek_probe_aarch64"
  export VK_ICD_FILENAMES="$stage_dir/host/vulkan/icd.d/vortek.json"
  echo "[Vortek.Probe] icd=$VK_ICD_FILENAMES mode=qemu-aarch64"
  run_with_env qemu-aarch64 "$host_dir/ld-linux-aarch64.so.1" \
    --library-path "$stage_dir/host/lib:$host_dir" \
    "$probe_build/vortek_probe_aarch64"
  exit $?
fi

# Desktop fallback: host-arch ICD (not packaged) + system Vulkan loader.
host_probe_lib="$stage_dir/host-probe/lib/libvulkan_vortek.so"
host_probe_icd="$stage_dir/host-probe/vulkan/icd.d/vortek.json"
if [[ ! -f "$host_probe_lib" || ! -f "$host_probe_icd" ]]; then
  echo "host-probe ICD missing and qemu-aarch64 unavailable; rebuild with build-vortek-client.sh" >&2
  exit 1
fi
build_probe gcc "$probe_build/vortek_probe_host"
export VK_ICD_FILENAMES="$host_probe_icd"
export LD_LIBRARY_PATH="$stage_dir/host-probe/lib:${LD_LIBRARY_PATH:-}"
echo "[Vortek.Probe] icd=$VK_ICD_FILENAMES mode=host-arch-fallback"
echo "[Vortek.Probe] note=production package still uses aarch64-glibc libvulkan_vortek.so"
run_with_env "$probe_build/vortek_probe_host"
exit $?
