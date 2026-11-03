#!/usr/bin/env bash
# Cuts a release: tags HEAD vVERSION; the tag is the version. Commits nothing, pushes nothing.
#   scripts/release.sh 0.1.0
# Pushing the tag starts release.yml, which opens a draft release (docs/PACKAGING.md).
set -euo pipefail
cd "$(dirname "$0")/.."
version="${1:?usage: scripts/release.sh MAJOR.MINOR.PATCH}"
echo "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo "$version is not MAJOR.MINOR.PATCH" >&2; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "the working tree is dirty" >&2; exit 1; }
git rev-parse -q --verify "refs/tags/v$version" >/dev/null && { echo "v$version already exists" >&2; exit 1; }
# The update check never announces a lower number.
newest="$(git tag --list 'v[0-9]*.[0-9]*.[0-9]*' | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sed 's/^v//' | sort -V | tail -1 || true)"
if [ -n "$newest" ] && [ "$(printf '%s\n%s\n' "$newest" "$version" | sort -V | tail -1)" != "$version" ]; then
    echo "$version is not above the newest tag, v$newest" >&2
    exit 1
fi
git tag -a "v$version" -m "Verdite1 $version"
echo "Tagged v$version at $(git rev-parse --short=9 HEAD). To publish the draft:"
echo "  git push origin HEAD && git push origin v$version"
