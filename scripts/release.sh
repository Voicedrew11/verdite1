#!/usr/bin/env bash
# Cut a port's release: tag it, after bumping and committing VERSION when the
# game keeps one.
#
# Run through the game's scripts/release.sh; the game is the checkout this subtree
# sits in (tools/verdite-core), or $VERDITE_GAME_ROOT, and its packaging/package.env
# names it for the tag's message.
#
# The version is one number and everything else derives from it -- the
# launcher's assembly version, the AppImage's name, the zip's, the installer's.
# A game that keeps a VERSION file (Verdite2) writes it there, so a release is:
# change that number, tag the commit that changed it, push the tag. A game with
# none (Verdite3) is versioned by its tags alone (scripts/version.sh), so a
# release is the tag, on HEAD, and nothing is committed. This script is either
# sequence, with the mistakes that are easy to make on the command line refused
# rather than committed.
#
#   bash scripts/release.sh 0.2.0
#
# It does NOT push. Pushing the tag is what publishes a draft release, and that
# is a decision rather than a step.
set -euo pipefail

ROOT="${VERDITE_GAME_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
cd "$ROOT"
[ -f packaging/package.env ] || { echo "no packaging/package.env under $ROOT" >&2; exit 1; }
# shellcheck disable=SC1091
. packaging/package.env
: "${NAME:?package.env sets no NAME}"

NEW="${1:-}"
[ -n "$NEW" ] || { echo "usage: bash scripts/release.sh MAJOR.MINOR.PATCH"; exit 1; }

echo "$NEW" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || { echo "not MAJOR.MINOR.PATCH: $NEW"; exit 1; }

OLD="$(bash "$(dirname "${BASH_SOURCE[0]}")/version.sh")"
[ "$NEW" != "$OLD" ] || { echo "the version is already $NEW"; exit 1; }
# Without a file the newest tag is the version, and one below it would never be
# announced: the update check only reports a higher number.
if [ ! -f VERSION ] && [ "$(printf '%s\n%s\n' "$OLD" "$NEW" | sort -V | tail -1)" != "$NEW" ]; then
    echo "v$NEW is not above the newest tag, v$OLD"; exit 1
fi

# A tag is permanent in a way a commit is not, so refuse to make one that names
# a tree nobody else can reconstruct.
[ -z "$(git status --porcelain)" ] || { echo "working tree is dirty"; exit 1; }

git rev-parse -q --verify "refs/tags/v$NEW" >/dev/null \
    && { echo "tag v$NEW already exists"; exit 1; } || true

if [ -f VERSION ]; then
    printf '%s\n' "$NEW" > VERSION
    git add VERSION
    git commit -m "Release v$NEW"
fi
git tag -a "v$NEW" -m "$NAME v$NEW"

echo
echo "v$OLD -> v$NEW, $([ -f VERSION ] && echo "committed and tagged" || echo "tagged at $(git rev-parse --short=9 HEAD)")."
echo "Publish it with:  git push origin HEAD && git push origin v$NEW"
echo "The release workflow builds both platforms and opens a DRAFT release."
