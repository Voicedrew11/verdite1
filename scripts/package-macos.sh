#!/usr/bin/env bash
# Builds the macOS release app on a Mac with Homebrew's libraries (docs/PACKAGING.md), ad-hoc signed
# and zipped: it is not notarized, so the player opens it the first time with right-click > Open.
#   scripts/package-macos.sh [VERSION]      -> dist/Verdite1-VERSION-macos-arm64.zip
# VERSION defaults to what scripts/version.sh says.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
export VERDITE_VERSION="${1:-$("$root/scripts/version.sh")}"
build="$root/build/macos-release"
cd "$root"
cmake --preset macos -B "$build" -DCMAKE_BUILD_TYPE=Release -DKF_RELEASE=ON -DKF_ENGLISH_PATCH=/nonexistent
cmake --build "$build" --target verdite1
python3 scripts/package_macos.py --build "$build" --output dist/Verdite1.app
zip="dist/Verdite1-$VERDITE_VERSION-macos-arm64.zip"
rm -f "$zip"
# ditto keeps the bundle's symlinks and signature, which zip would not.
ditto -c -k --keepParent dist/Verdite1.app "$zip"
echo "$zip"
