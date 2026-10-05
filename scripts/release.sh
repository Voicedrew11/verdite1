#!/usr/bin/env bash
# Cut a port's release: bump VERSION, commit it, tag it.
#
# Run through the game's scripts/release.sh; the game is the checkout this subtree
# sits in (tools/verdite-core), or $VERDITE_GAME_ROOT, and its packaging/package.env
# names it for the tag's message.
#
# The version is one number in one file and everything else derives from it --
# the launcher's assembly version, the AppImage's name, the zip's, the
# installer's, and the tag the release workflow checks itself against. So a
# release is: change that number, tag the commit that changed it, push the tag.
# This script is that sequence, with the mistakes that are easy to make on the
# command line refused rather than committed.
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

OLD="$(tr -d '[:space:]' < VERSION)"
[ "$NEW" != "$OLD" ] || { echo "VERSION is already $NEW"; exit 1; }

# A tag is permanent in a way a commit is not, so refuse to make one that names
# a tree nobody else can reconstruct.
[ -z "$(git status --porcelain)" ] || { echo "working tree is dirty"; exit 1; }

git rev-parse -q --verify "refs/tags/v$NEW" >/dev/null \
    && { echo "tag v$NEW already exists"; exit 1; } || true

printf '%s\n' "$NEW" > VERSION
git add VERSION
git commit -m "Release v$NEW"
git tag -a "v$NEW" -m "$NAME v$NEW"

echo
echo "v$OLD -> v$NEW, committed and tagged."
echo "Publish it with:  git push origin HEAD && git push origin v$NEW"
echo "The release workflow builds both platforms and opens a DRAFT release."
