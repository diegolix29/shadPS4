#!/usr/bin/env bash
# Homebrew LLVM 23+ libclc no longer ships libclc.pc or spirv-mesa3d-.spv.
# KosmicKrisp/Mesa still needs those files, so install Karol Herbst's mesa-libclc
# fork (same source Homebrew's mesa formula uses).
set -euo pipefail

brew install llvm meson ninja pkg-config spirv-tools spirv-llvm-translator

python3 -m pip install --break-system-packages mako packaging pyyaml

LLVM_PREFIX="$(brew --prefix llvm)"
export PATH="${LLVM_PREFIX}/bin:${PATH}"

MESA_LIBCLC_VERSION="22.1.8.3"
PREFIX="${MESA_LIBCLC_PREFIX:-${GITHUB_WORKSPACE}/.deps/mesa-libclc}"

if [[ ! -f "${PREFIX}/share/pkgconfig/libclc.pc" ]]; then
  STAGING="${RUNNER_TEMP:-/tmp}/mesa-libclc-build"
  mkdir -p "${STAGING}" "${PREFIX}"
  ARCHIVE="${STAGING}/mesa-libclc.tar.bz2"
  curl -fsSL -o "${ARCHIVE}" \
    "https://gitlab.freedesktop.org/karolherbst/mesa-libclc/-/archive/${MESA_LIBCLC_VERSION}/mesa-libclc-${MESA_LIBCLC_VERSION}.tar.bz2"
  tar -xjf "${ARCHIVE}" -C "${STAGING}"
  cmake -S "${STAGING}/mesa-libclc-${MESA_LIBCLC_VERSION}" -B "${STAGING}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_PREFIX_PATH="${LLVM_PREFIX}" \
    -DLLVM_CONFIG="${LLVM_PREFIX}/bin/llvm-config"
  cmake --build "${STAGING}/build" --parallel "$(sysctl -n hw.ncpu)"
  cmake --install "${STAGING}/build"
fi

{
  echo "PATH=${LLVM_PREFIX}/bin:${PATH}"
  echo "PKG_CONFIG_PATH=${PREFIX}/share/pkgconfig:${LLVM_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
  echo "LLVM_CONFIG=${LLVM_PREFIX}/bin/llvm-config"
} >> "${GITHUB_ENV}"

export PKG_CONFIG_PATH="${PREFIX}/share/pkgconfig:${LLVM_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
pkg-config --exists libclc
pkg-config --modversion libclc
pkg-config --variable=libexecdir libclc
