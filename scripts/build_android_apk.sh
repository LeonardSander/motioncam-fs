#!/usr/bin/env bash
set -euo pipefail

# Build a sideloadable, debug-key-signed arm64 APK with optimized native code.
source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Local machines can retain their toolchain paths without changing CI's env.
if [[ -f "${source_dir}/android-build.env" ]]; then
  source "${source_dir}/android-build.env"
fi
: "${ANDROID_SDK_ROOT:?Set ANDROID_SDK_ROOT to the Android SDK directory}"
: "${ANDROID_NDK_ROOT:?Set ANDROID_NDK_ROOT to the Android NDK directory}"
: "${VCPKG_ROOT:?Set VCPKG_ROOT to the pinned vcpkg checkout}"
: "${QT_ANDROID_ROOT:?Set QT_ANDROID_ROOT to the Qt android_arm64_v8a directory}"
: "${QT_HOST_PATH:?Set QT_HOST_PATH to the matching Qt Linux desktop directory}"
: "${JAVA_HOME:?Set JAVA_HOME to a full JDK (21 recommended)}"
export ANDROID_SDK_ROOT ANDROID_NDK_ROOT VCPKG_ROOT QT_ANDROID_ROOT QT_HOST_PATH JAVA_HOME
export ANDROID_HOME="${ANDROID_SDK_ROOT}"
export ANDROID_NDK_HOME="${ANDROID_NDK_ROOT}"
export PATH="${JAVA_HOME}/bin:${ANDROID_SDK_ROOT}/platform-tools:${PATH}"
export VCPKG_MAX_CONCURRENCY="${VCPKG_MAX_CONCURRENCY:-4}"

if [[ ! -x "${JAVA_HOME}/bin/javac" ]]; then
  echo "JAVA_HOME must point to a full JDK: ${JAVA_HOME}" >&2
  exit 1
fi

build_dir="${1:-${source_dir}/build-android-arm64}"

"${QT_ANDROID_ROOT}/bin/qt-cmake" -S "${source_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DMOTIONCAM_REQUIRE_VULKAN=ON \
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

cmake --build "${build_dir}" --target apk --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
apk="${build_dir}/android-build/build/outputs/apk/debug/android-build-debug.apk"
if [[ ! -f "${apk}" ]]; then
  echo "APK target completed without an APK in ${build_dir}" >&2
  exit 1
fi
echo "${apk}"
