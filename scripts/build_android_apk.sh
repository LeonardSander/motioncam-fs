#!/usr/bin/env bash
set -euo pipefail

# Build a sideloadable, debug-key-signed arm64 APK with optimized native code.
: "${ANDROID_SDK_ROOT:?Set ANDROID_SDK_ROOT to the Android SDK directory}"
: "${ANDROID_NDK_ROOT:?Set ANDROID_NDK_ROOT to the Android NDK directory}"
: "${VCPKG_ROOT:?Set VCPKG_ROOT to the pinned vcpkg checkout}"
: "${QT_ANDROID_ROOT:?Set QT_ANDROID_ROOT to the Qt android_arm64_v8a directory}"
: "${QT_HOST_PATH:?Set QT_HOST_PATH to the matching Qt Linux desktop directory}"
export ANDROID_NDK_HOME="${ANDROID_NDK_ROOT}"

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${source_dir}/build-android-arm64}"

"${QT_ANDROID_ROOT}/bin/qt-cmake" -S "${source_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DANDROID_SDK_ROOT="${ANDROID_SDK_ROOT}" \
  -DANDROID_NDK_ROOT="${ANDROID_NDK_ROOT}" \
  -DANDROID_PLATFORM=android-28 \
  -DQT_HOST_PATH="${QT_HOST_PATH}" \
  -DQT_ANDROID_ABIS=arm64-v8a \
  -DQT_CHAINLOAD_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-android \
  -DVCPKG_OVERLAY_PORTS="${source_dir}/ports" \
  -DQT_ANDROID_DEPLOYMENT_TYPE=Debug

cmake --build "${build_dir}" --target apk --parallel
apk="$(find "${build_dir}" -type f -name '*.apk' -path '*/outputs/apk/*' -print -quit)"
if [[ -z "${apk}" ]]; then
  echo "APK target completed without an APK in ${build_dir}" >&2
  exit 1
fi
echo "${apk}"
