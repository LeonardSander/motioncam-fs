#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-universal-vcpkg"

if [[ -z "${VCPKG_ROOT:-}" ]]; then
  if [[ -d "${ROOT_DIR}/vcpkg" ]]; then
    VCPKG_ROOT="${ROOT_DIR}/vcpkg"
  else
    echo "VCPKG_ROOT is not set. Export VCPKG_ROOT or place vcpkg at ${ROOT_DIR}/vcpkg." >&2
    exit 1
  fi
fi

host_arch="$(uname -m)"
if [[ "$host_arch" == x86_64 ]]; then host_arch=x64; fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DRAWPREAMP_MACOS_UNIVERSAL=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_TRIPLETS="${ROOT_DIR}/scripts/vcpkg-triplets" \
  -DVCPKG_HOST_TRIPLET="${host_arch}-osx-release" \
  -DVCPKG_TARGET_TRIPLET=universal-osx

cmake --build "${BUILD_DIR}" --config Release

APP_PATH="${BUILD_DIR}/RawPreamp.app"
if [[ -d "$APP_PATH" ]]; then
  "${ROOT_DIR}/scripts/package_macos_app.sh" "$APP_PATH"
fi

echo "Universal build output:"
echo "  ${BUILD_DIR}/RawPreamp.app"
