#!/usr/bin/env bash
# Builds the Linux release AppImage in a container (packaging/appimage/), so it runs on any
# desktop with glibc 2.35 or later, unlike the nix build.
#   scripts/package-linux.sh 0.1.0-pre.1      -> dist/KingsFieldEnhanced-v0.1.0-pre.1-x86_64.AppImage
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:?usage: scripts/package-linux.sh VERSION}"
engine="$(command -v podman || command -v docker)" || { echo "Needs podman or docker." >&2; exit 1; }
out="$root/build/appimage"
mkdir -p "$out" "$root/dist"
"$engine" build -t kf-appimage-builder -f "$root/packaging/appimage/Containerfile" "$root/packaging/appimage"
# The repository and git's own directory at their real paths: a worktree's and the submodule's
# .git files name them that way.
common="$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)"
"$engine" run --rm --security-opt label=disable -v "$root:$root:ro" -v "$common:$common:ro" -v "$out:/out" \
  kf-appimage-builder sh "$root/packaging/appimage/build.sh" "$root" "$version"
mv "$out/KingsFieldEnhanced-v$version-x86_64.AppImage" "$root/dist/"
echo "dist/KingsFieldEnhanced-v$version-x86_64.AppImage"
