#!/usr/bin/env bash
# Builds the Linux release AppImage in a container (packaging/appimage/), so it runs on any
# desktop with glibc 2.35 or later, unlike the nix build.
#   scripts/package-linux.sh [VERSION]      -> dist/Verdite1-VERSION-x86_64.AppImage
# VERSION defaults to what scripts/version.sh says.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:-$("$root/scripts/version.sh")}"
engine="$(command -v podman || command -v docker)" || { echo "Needs podman or docker." >&2; exit 1; }
out="$root/build/appimage"
mkdir -p "$out" "$root/dist"
image="$("$root/scripts/builder-image.sh" appimage)"
# The compiler cache outlives the build directory; CI keeps it between runs.
ccache="${KF_CCACHE_DIR:-$root/build/ccache/appimage}"
mkdir -p "$ccache"
# The repository and git's own directory at their real paths: a worktree's and the submodule's
# .git files name them that way.
common="$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)"
"$engine" run --rm --security-opt label=disable -e "VERDITE_VERSION=$version" \
  -v "$root:$root:ro" -v "$common:$common:ro" -v "$out:/out" \
  -v "$ccache:/ccache" -e CCACHE_DIR=/ccache -e CCACHE_MAXSIZE \
  "$image" sh "$root/packaging/appimage/build.sh" "$root" "$version"
mv "$out/Verdite1-$version-x86_64.AppImage" "$root/dist/"
echo "dist/Verdite1-$version-x86_64.AppImage"
