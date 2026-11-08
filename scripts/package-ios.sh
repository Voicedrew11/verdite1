#!/usr/bin/env bash
# Builds the iOS release app on a Mac, unsigned, as an .ipa: no Apple account is needed to build
# it, and a sideloading tool (AltStore, Sideloadly) signs it with the player's own Apple ID.
#   scripts/package-ios.sh [VERSION]        -> dist/Verdite1-VERSION-ios-arm64.ipa
# VERSION defaults to what scripts/version.sh says. script/build_ios.sh is the signed device build.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
export VERDITE_VERSION="${1:-$("$root/scripts/version.sh")}"
build="$root/build/ios-release"
cd "$root"
cmake --preset ios-device -B "$build" -DKF_RELEASE=ON -DKF_ENGLISH_PATCH=/nonexistent
xcodebuild -project "$build/kings_field.xcodeproj" -scheme verdite1 -configuration Release \
  -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO -quiet
app="$build/Release-iphoneos/Verdite1.app"
test -d "$app" || { echo "no app at $app" >&2; exit 1; }
stage="$build/ipa"
rm -rf "$stage"
mkdir -p "$stage/Payload" dist
cp -R "$app" "$stage/Payload/"
ipa="$root/dist/Verdite1-$VERDITE_VERSION-ios-arm64.ipa"
rm -f "$ipa"
(cd "$stage" && zip -qry "$ipa" Payload)
echo "dist/$(basename "$ipa")"
