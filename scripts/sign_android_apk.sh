#!/usr/bin/env bash
set -euo pipefail

# Release APKs still need a signature for sideloading. Keep the existing local
# debug-key identity without making the Gradle package debuggable. A persistent
# distribution key can be supplied separately through these environment vars.
unsigned_apk="${1:?Usage: sign_android_apk.sh UNSIGNED_APK OUTPUT_APK}"
signed_apk="${2:?Usage: sign_android_apk.sh UNSIGNED_APK OUTPUT_APK}"
: "${ANDROID_SDK_ROOT:?Set ANDROID_SDK_ROOT}"
: "${JAVA_HOME:?Set JAVA_HOME}"
android_build_tools="${ANDROID_SDK_ROOT}/build-tools/36.0.0"
signing_keystore="${ANDROID_SIGNING_KEYSTORE:-${HOME}/.android/debug.keystore}"
export MOTIONCAM_APK_STORE_PASSWORD="${ANDROID_SIGNING_STORE_PASSWORD:-android}"
export MOTIONCAM_APK_KEY_PASSWORD="${ANDROID_SIGNING_KEY_PASSWORD:-android}"
signing_alias="${ANDROID_SIGNING_ALIAS:-androiddebugkey}"
if [[ ! -f "$signing_keystore" ]]; then
  if [[ -n "${ANDROID_SIGNING_KEYSTORE:-}" ]]; then
    echo "Signing keystore not found: $signing_keystore" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$signing_keystore")"
  "${JAVA_HOME}/bin/keytool" -genkeypair -noprompt -keystore "$signing_keystore" \
    -storepass:env MOTIONCAM_APK_STORE_PASSWORD -keypass:env MOTIONCAM_APK_KEY_PASSWORD \
    -alias "$signing_alias" -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android Debug,O=Android,C=US"
fi
mkdir -p "$(dirname "$signed_apk")"
aligned_apk="$(mktemp "${signed_apk}.aligned.XXXXXX.apk")"
trap 'rm -f "$aligned_apk"' EXIT
"${android_build_tools}/zipalign" -f -P 16 4 "$unsigned_apk" "$aligned_apk"
"${android_build_tools}/apksigner" sign --v4-signing-enabled false --ks "$signing_keystore" --ks-key-alias "$signing_alias" \
  --ks-pass env:MOTIONCAM_APK_STORE_PASSWORD --key-pass env:MOTIONCAM_APK_KEY_PASSWORD \
  --out "$signed_apk" "$aligned_apk"
"${android_build_tools}/apksigner" verify "$signed_apk"
