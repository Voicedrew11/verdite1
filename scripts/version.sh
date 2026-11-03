#!/bin/sh
# Prints the version a build here calls itself. The tag is the version: VERDITE_VERSION when set
# (release.yml hands the tag it was pushed for), else the newest vMAJOR.MINOR.PATCH tag HEAD contains,
# else 0.0.0. Old tags like v0.1.0-pre.3 and the rolling `preview` are no versions.
root="$(cd "$(dirname "$0")/.." && pwd)"
if [ -n "${VERDITE_VERSION:-}" ]; then
    echo "$VERDITE_VERSION"
    exit 0
fi
tag="$(git -C "$root" describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*.[0-9]*' --exclude '*-*' 2>/dev/null)"
if [ -n "$tag" ]; then
    echo "${tag#v}"
else
    echo 0.0.0
fi
