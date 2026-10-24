#!/usr/bin/env bash
# Run the nix-built game on a non-NixOS host: nix's libglvnd cannot find the
# host's GPU driver unless pointed at its vendor files and libraries.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
export __EGL_VENDOR_LIBRARY_DIRS="${__EGL_VENDOR_LIBRARY_DIRS:-/usr/share/glvnd/egl_vendor.d}"
export LD_LIBRARY_PATH="/usr/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$root/build/linux/verdite1" --data "$root/data/${KF_LANGUAGE:-en}" "$@"
