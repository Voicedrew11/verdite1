#!/usr/bin/env bash
# Builds the signed release APK in the android nix shell.
#   KF_ANDROID_KEYSTORE=key.jks KF_ANDROID_KEYSTORE_PASSWORD=... scripts/package-android.sh [VERSION]
#     -> dist/Verdite1-VERSION-android-arm64.apk
# arm64 only, in a build directory of its own, so it takes nothing from a development build; no
# English delta is embedded: builds that carry it are never distributed. The key is the one
# scripts/android-release-key.sh made, and a phone refuses an update signed with any other.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
version="${1:-$(scripts/version.sh)}"
: "${KF_ANDROID_KEYSTORE:?the release key: see scripts/android-release-key.sh}"
: "${KF_ANDROID_KEYSTORE_PASSWORD:?the password of the release key}"
export VERDITE_VERSION="$version" KF_ANDROID_JNI_DIR="$root/build/android-release/jni"
export KF_ANDROID_ASSETS_DIR="$root/build/android-release/assets"
nix develop .#android -c bash -euc '
  cmake --preset android-arm64 -B build/android-release -DCMAKE_BUILD_TYPE=Release -DKF_RELEASE=ON -DKF_ENGLISH_PATCH=/nonexistent
  cmake --build build/android-release --target verdite1
  licenses="$KF_ANDROID_ASSETS_DIR/licenses"
  mkdir -p "$licenses"
  cp LICENSE "$licenses/Verdite1-MIT.txt"
  cp third_party/imgui/LICENSE.txt "$licenses/DearImGui.txt"
  cp third_party/lua/LICENSE "$licenses/Lua.txt"
  cp third_party/cjson/LICENSE "$licenses/cJSON.txt"
  cp third_party/libchdr/LICENSE.txt "$licenses/libchdr.txt"
  cp third_party/libchdr/deps/zstd-1.5.7/LICENSE "$licenses/zstd.txt"
  cp "$KF_SDL3_SOURCE/LICENSE.txt" "$licenses/SDL3.txt"
  cp "$KF_LIBARCHIVE_SOURCE/COPYING" "$licenses/libarchive.txt"
  cp "$KF_LIBDATACHANNEL_SOURCE/LICENSE" "$licenses/libdatachannel.txt"
  cp "$KF_LIBDATACHANNEL_SOURCE/deps/libjuice/LICENSE" "$licenses/libjuice.txt"
  cp "$KF_LIBDATACHANNEL_SOURCE/deps/usrsctp/LICENSE.md" "$licenses/usrsctp.txt"
  cp "$KF_LIBDATACHANNEL_SOURCE/deps/plog/LICENSE" "$licenses/plog.txt"
  cp build/android-release/openssl/source/LICENSE.txt "$licenses/OpenSSL.txt"
  chmod -R u+w "$KF_ANDROID_ASSETS_DIR"
  gradle -p android --no-daemon assembleRelease
'
mkdir -p dist
cp android/app/build/outputs/apk/release/app-release.apk "dist/Verdite1-$version-android-arm64.apk"
echo "dist/Verdite1-$version-android-arm64.apk"
