#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir="$project_root/runtime/build/shadps4-arm64"
stage_dir="$project_root/runtime/build/shadps4-arm64-stage"
binary="$build_dir/shadps4"
host_tools_dir="$project_root/runtime/build/host-tools"
host_font_embed="$host_tools_dir/Dear_ImGui_FontEmbed"
font_embed_source="$project_root/externals/dear_imgui/misc/fonts/binary_to_compressed_c.cpp"

for tool in cmake ninja clang clang++ readelf aarch64-linux-gnu-strip; do
  command -v "$tool" >/dev/null || { echo "$tool is required" >&2; exit 1; }
done

llvm_ar=$(command -v llvm-ar-21 || command -v llvm-ar)
llvm_ranlib=$(command -v llvm-ranlib-21 || command -v llvm-ranlib)
mkdir -p "$host_tools_dir"
clang++ -std=c++23 "$font_embed_source" -o "$host_font_embed"
test -x "$host_font_embed"
sdl_patch="$project_root/runtime/patches/sdl3-winlator-x11.patch"
if git -C "$project_root/externals/sdl3" apply --check "$sdl_patch"; then
  git -C "$project_root/externals/sdl3" apply "$sdl_patch"
elif ! git -C "$project_root/externals/sdl3" apply --reverse --check "$sdl_patch"; then
  echo "SDL3 Winlator patch does not apply cleanly" >&2
  exit 1
fi

bash "$project_root/runtime/scripts/build-fexcore-smoke-aarch64.sh"

arm64_xext_lib=/usr/lib/aarch64-linux-gnu/libXext.so
if [[ ! -e "$arm64_xext_lib" ]]; then
  arm64_xext_lib=/usr/lib/aarch64-linux-gnu/libXext.so.6
fi
test -r "$arm64_xext_lib" || {
  echo "ARM64 libXext is required; install libxext-dev:arm64" >&2
  exit 1
}

# Debian runtime images can have runtime libraries without their development
# packages' unversioned linker names. Keep the cross build reproducible without
# modifying the host filesystem.
arm64_link_dir="$build_dir/cross-link"
mkdir -p "$arm64_link_dir"
for library in libuuid.so.1 libudev.so.1; do
  source_library="/usr/lib/aarch64-linux-gnu/$library"
  test -r "$source_library" || {
    echo "ARM64 $library is required" >&2
    exit 1
  }
  ln -sfn "$source_library" "$arm64_link_dir/${library%.1}"
done

cmake -S "$project_root" -B "$build_dir" -G Ninja \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu \
  -DCMAKE_CXX_COMPILER_TARGET=aarch64-linux-gnu \
  -DCMAKE_FIND_ROOT_PATH=/usr/aarch64-linux-gnu \
  -DCMAKE_EXE_LINKER_FLAGS:STRING="-L$arm64_link_dir" \
  -DX11_Xext_LIB:FILEPATH="$arm64_xext_lib" \
  -DXEXT_LIB:FILEPATH="$arm64_xext_lib" \
  -DIMGUI_FONT_EMBED_EXECUTABLE="$host_font_embed" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
  -DCMAKE_AR="$llvm_ar" \
  -DCMAKE_RANLIB="$llvm_ranlib" \
  -DCMAKE_C_COMPILER_AR="$llvm_ar" \
  -DCMAKE_C_COMPILER_RANLIB="$llvm_ranlib" \
  -DCMAKE_CXX_COMPILER_AR="$llvm_ar" \
  -DCMAKE_CXX_COMPILER_RANLIB="$llvm_ranlib" \
    -DENABLE_BACHATA_RUNTIME=ON \
    -DENABLE_FEX_GUEST_CPU=ON \
    -DFEXCORE_GUEST_CPU_SOURCE_DIR="$project_root/runtime/sources/fex" \
    -DFEXCORE_GUEST_CPU_BUILD_DIR="$project_root/runtime/build/fexcore-smoke-build" \
  -DENABLE_USERFAULTFD=OFF \
  -DENABLE_DISCORD_RPC=OFF \
  -DENABLE_UPDATER=OFF \
  -DENABLE_TESTS=OFF \
  -DSDL_X11_XCURSOR=OFF \
  -DSDL_X11_XDBE=OFF \
  -DSDL_X11_XINPUT=OFF \
  -DSDL_X11_XFIXES=OFF \
  -DSDL_X11_XRANDR=OFF \
  -DSDL_X11_XSCRNSAVER=OFF \
  -DSDL_X11_XSHAPE=OFF \
  -DSDL_X11_XSYNC=OFF \
  -DSDL_X11_XTEST=OFF \
  -DSDL_WAYLAND=OFF

sdl_config="$build_dir/externals/sdl3/include-config-release/build_config/SDL_build_config.h"
if ! grep -Fq '#define SDL_VIDEO_DRIVER_X11_DYNAMIC_XEXT "libXext.so.6"' "$sdl_config"; then
  echo "ARM64 SDL3 requires dynamic Xext support; install libx11-dev:arm64 and libxext-dev:arm64" >&2
  exit 1
fi

cmake --build "$build_dir" --target shadps4

test -f "$binary"
header=$(readelf -h "$binary")
grep -Eq 'Class:[[:space:]]+ELF64' <<<"$header"
grep -Eq 'Machine:[[:space:]]+AArch64' <<<"$header"
readelf -l "$binary" | grep -Fq '/lib/ld-linux-aarch64.so.1'

rm -rf "$stage_dir"
install -Dm755 "$binary" "$stage_dir/bin/shadps4-arm64"
readelf -d "$binary" \
  | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' \
  | LC_ALL=C sort -u >"$stage_dir/needed.txt"
test -s "$stage_dir/needed.txt"

printf 'shadps4=%s\nneeded=%s\n' "$stage_dir/bin/shadps4-arm64" "$stage_dir/needed.txt"
