#!/usr/bin/env bash
# Cross-builds the Windows release in a container (packaging/windows/), with llvm-mingw.
#   scripts/package-windows.sh [VERSION]    -> dist/Verdite1-VERSION-win-x64.zip
# The installer is made from the zip by packaging/windows/make-installer.ps1.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:-$("$root/scripts/version.sh")}"
engine="$(command -v podman || command -v docker)" || { echo "Needs podman or docker." >&2; exit 1; }
out="$root/build/windows-package"
mkdir -p "$out" "$root/dist"
image="$("$root/scripts/builder-image.sh" windows)"
# The compiler cache outlives the build directory; CI keeps it between runs.
ccache="${KF_CCACHE_DIR:-$root/build/ccache/windows}"
mkdir -p "$ccache"
common="$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)"
"$engine" run --rm --security-opt label=disable -e "VERDITE_VERSION=$version" \
  -v "$root:$root:ro" -v "$common:$common:ro" -v "$out:/out" \
  -v "$ccache:/ccache" -e CCACHE_DIR=/ccache -e CCACHE_MAXSIZE \
  "$image" sh "$root/packaging/windows/build.sh" "$root" "$version"
mv "$out/Verdite1-$version-win-x64.zip" "$root/dist/"
echo "dist/Verdite1-$version-win-x64.zip"
