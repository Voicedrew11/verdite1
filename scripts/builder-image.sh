#!/usr/bin/env bash
# Prints the name of a package's builder image (packaging/NAME/Containerfile), building it if need be.
#   scripts/builder-image.sh appimage|windows
# Locally it is built here, as verdite1-NAME-builder. With KF_BUILDER_REGISTRY set (CI: ghcr.io/OWNER,
# logged in), it is NAME-builder:HASH there, HASH being the Containerfile's and the files it copies:
# pulled when it exists, else built and pushed, so a run builds it only when its recipe changed.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
name="${1:?usage: scripts/builder-image.sh appimage|windows}"
context="$root/packaging/$name"
[ -f "$context/Containerfile" ] || { echo "no packaging/$name/Containerfile" >&2; exit 1; }
engine="$(command -v podman || command -v docker)" || { echo "Needs podman or docker." >&2; exit 1; }

if [ -z "${KF_BUILDER_REGISTRY:-}" ]; then
  image="verdite1-$name-builder"
  "$engine" build -t "$image" -f "$context/Containerfile" "$context" >&2
  echo "$image"
  exit 0
fi

copied="$(sed -n 's/^COPY[[:space:]]\{1,\}\([^[:space:]]\{1,\}\).*/\1/p' "$context/Containerfile")"
hash="$(cd "$context" && cat Containerfile $copied | sha256sum | cut -c1-16)"
image="$KF_BUILDER_REGISTRY/verdite1-$name-builder:$hash"
if "$engine" pull -q "$image" >&2; then
  echo "pulled $image" >&2
else
  echo "building $image: its recipe changed" >&2
  "$engine" build -t "$image" -f "$context/Containerfile" "$context" >&2
  "$engine" push "$image" >&2
fi
echo "$image"
