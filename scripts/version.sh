#!/usr/bin/env bash
# Print a port's version, MAJOR.MINOR.PATCH, by the rule every build reads it by
# (Launcher.targets and build-windows.ps1 follow the same one):
#
#   1. VERSION at the game's root, when the game keeps one (Verdite2);
#   2. else $VERDITE_VERSION, which a release build sets from the tag it was
#      pushed for;
#   3. else the newest vMAJOR.MINOR.PATCH tag reachable from HEAD;
#   4. else 0.0.0 (no tag yet, or not a checkout).
#
# A game with no VERSION file is versioned by its tags alone. The game is the
# checkout this subtree sits in (tools/verdite-core), or $VERDITE_GAME_ROOT.
set -euo pipefail

ROOT="${VERDITE_GAME_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"

if [ -f "$ROOT/VERSION" ]; then
    v="$(tr -d '[:space:]' < "$ROOT/VERSION")"
elif [ -n "${VERDITE_VERSION:-}" ]; then
    v="${VERDITE_VERSION#v}"
else
    v="$( { git -C "$ROOT" tag --merged HEAD --list 'v*' --sort=-v:refname 2>/dev/null || true; } \
        | sed -nE '/^v[0-9]+\.[0-9]+\.[0-9]+$/{s/^v//p;q;}')"
    v="${v:-0.0.0}"
fi

echo "$v" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo "not MAJOR.MINOR.PATCH: $v" >&2; exit 1; }
echo "$v"
