#!/usr/bin/env bash
set -euo pipefail

# Build a Release APK and sign it for sideloading (signing identity is separate
# from the native/Gradle build configuration).
source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Local defaults must not overwrite an explicit caller or CI toolchain.
declare -A supplied_toolchain=()
for name in ANDROID_SDK_ROOT ANDROID_NDK_ROOT ANDROID_NDK_VERSION VCPKG_ROOT VCPKG_COMMIT \
    QT_ANDROID_ROOT QT_HOST_PATH QT_VERSION QT_ANDROID_ABI ANDROID_VCPKG_TRIPLET JAVA_HOME \
    BUILD_TESTING CMAKE_BUILD_PARALLEL_LEVEL VCPKG_MAX_CONCURRENCY; do
  if [[ -v "$name" ]]; then supplied_toolchain[$name]="${!name}"; fi
done
if [[ "${CI:-false}" != true && -f "${source_dir}/android-build.env" ]]; then
  source "${source_dir}/android-build.env"
fi
for name in "${!supplied_toolchain[@]}"; do
  printf -v "$name" '%s' "${supplied_toolchain[$name]}"
done
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
phase="${2:-all}"
case "$phase" in
  all|configure|build|print-config) ;;
  *) echo "Usage: $0 [BUILD_DIRECTORY] [all|configure|build|print-config]" >&2; exit 2 ;;
esac

expected_vcpkg="${VCPKG_COMMIT:-b1c0e26466f37efd4112bf7c1f88f5a8e28c510e}"
actual_vcpkg="$(git -C "$VCPKG_ROOT" rev-parse HEAD)"
if [[ "$actual_vcpkg" != "$expected_vcpkg" ]]; then
  echo "VCPKG_ROOT must use CI revision $expected_vcpkg (found $actual_vcpkg)." >&2
  exit 2
fi
expected_ndk="${ANDROID_NDK_VERSION:-27.2.12479018}"
actual_ndk="$(awk '$1 == "Pkg.Revision" {print $3}' "$ANDROID_NDK_ROOT/source.properties")"
if [[ "$actual_ndk" != "$expected_ndk" ]]; then
  echo "ANDROID_NDK_ROOT must use Qt's NDK $expected_ndk (found $actual_ndk)." >&2
  exit 2
fi
if [[ ! -d "$ANDROID_SDK_ROOT/platforms/android-36" || ! -d "$ANDROID_SDK_ROOT/build-tools/36.0.0" ]]; then
  echo "ANDROID_SDK_ROOT must contain platforms/android-36 and build-tools/36.0.0, as in CI." >&2
  exit 2
fi

android_abi="${QT_ANDROID_ABI:-arm64-v8a}"
case "$android_abi" in
  arm64-v8a) android_triplet=arm64-android-release ;;
  x86_64) android_triplet=x64-android-release ;;
  *) echo "Unsupported Android ABI: $android_abi" >&2; exit 2 ;;
esac
if [[ -n "${ANDROID_VCPKG_TRIPLET:-}" && "$ANDROID_VCPKG_TRIPLET" != "$android_triplet" ]]; then
  echo "QT_ANDROID_ABI=$android_abi requires ANDROID_VCPKG_TRIPLET=$android_triplet" >&2
  exit 2
fi
kit_abi="$(awk -F '"' '/^set\(ANDROID_ABI / {print $2; exit}' "$QT_ANDROID_ROOT/lib/cmake/Qt6/qt.toolchain.cmake")"
if [[ "$kit_abi" != "$android_abi" ]]; then
  echo "QT_ANDROID_ROOT is a $kit_abi kit, but QT_ANDROID_ABI requests $android_abi." >&2
  exit 2
fi
expected_qt="${QT_VERSION:-6.12.0}"
for qt_root in "$QT_ANDROID_ROOT" "$QT_HOST_PATH"; do
  actual_qt="$(awk -F '"' '/^set\(PACKAGE_VERSION / {print $2; exit}' "$qt_root/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake")"
  if [[ "$actual_qt" != "$expected_qt" ]]; then
    echo "Qt kit $qt_root must use CI version $expected_qt (found $actual_qt)." >&2
    exit 2
  fi
done

if [[ "$phase" != build ]]; then
  configure=("${QT_ANDROID_ROOT}/bin/qt-cmake" -S "${source_dir}" -B "${build_dir}" -G Ninja
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING="${BUILD_TESTING:-OFF}" \
    -DMOTIONCAM_REQUIRE_VULKAN=ON \
    -DANDROID_SDK_ROOT="${ANDROID_SDK_ROOT}" \
    -DANDROID_NDK_ROOT="${ANDROID_NDK_ROOT}" \
    -DANDROID_PLATFORM=android-28 \
    -DANDROID_ABI="${android_abi}" \
    -DANDROID_STL=c++_shared \
    -DQT_HOST_PATH="${QT_HOST_PATH}" \
    -DQT_ANDROID_ABIS="${android_abi}" \
    -DQT_CHAINLOAD_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake" \
    -DVCPKG_TARGET_TRIPLET="${android_triplet}" \
    -DVCPKG_HOST_TRIPLET=x64-linux-release \
    -DVCPKG_OVERLAY_TRIPLETS="${source_dir}/scripts/vcpkg-triplets" \
    -DVCPKG_OVERLAY_PORTS="${source_dir}/ports" \
    -DVCPKG_MANIFEST_MODE=ON \
    -DQT_ANDROID_DEPLOYMENT_TYPE=Release)
  if [[ "$phase" == print-config ]]; then
    printf '%q ' "${configure[@]}"; printf '\n'
    exit 0
  fi
  "${configure[@]}"
fi
if [[ "$phase" == configure ]]; then exit 0; fi

cmake --build "${build_dir}" --target apk --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
apk="${build_dir}/android-build/build/outputs/apk/release/android-build-release-unsigned.apk"
if [[ ! -f "${apk}" ]]; then
  echo "APK target completed without an APK in ${build_dir}" >&2
  exit 1
fi
signed_apk="${build_dir}/android-build-release.apk"
bash "${source_dir}/scripts/sign_android_apk.sh" "${apk}" "${signed_apk}"
echo "${signed_apk}"
