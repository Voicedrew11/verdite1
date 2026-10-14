#!/usr/bin/env bash
# Build a port's Linux AppImage.
#
# Run through the game's packaging/linux/build-appimage.sh, or directly; the game
# is the checkout this subtree sits in (tools/verdite-core), or $VERDITE_GAME_ROOT.
# Its packaging/package.env names it (NAME, APP_ID), and packaging/shared/ holds
# its $APP_ID.desktop and, if it ships a mark, $APP_ID.png.
#
# Needs the RecompOne subtree built (the game's scripts/setup_tools.sh) and the
# .NET 10 SDK. It does NOT need the disc: the launcher carries the inputs to a
# build and makes the game on the player's machine, which is what lets this run
# in CI at all.
set -euo pipefail

ROOT="${VERDITE_GAME_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)}"
[ -f "$ROOT/packaging/package.env" ] || { echo "no packaging/package.env under $ROOT" >&2; exit 1; }
# shellcheck disable=SC1091
. "$ROOT/packaging/package.env"
: "${NAME:?package.env sets no NAME}" "${APP_ID:?package.env sets no APP_ID}"

OUT="${OUT:-$ROOT/dist}"
APPDIR="$OUT/$NAME.AppDir"
# One rule for the number, for everything that names a build: the launcher's
# csproj resolves it the same way (VERSION, else $VERDITE_VERSION, else the newest
# version tag), so a package can never be named something other than what is
# inside it.
VERSION="${VERSION:-$(VERDITE_GAME_ROOT="$ROOT" bash "$(dirname "${BASH_SOURCE[0]}")/../../scripts/version.sh")}"

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

echo "==> publishing linux-x64"
dotnet publish "$ROOT/$NAME.Launcher/$NAME.Launcher.csproj" \
    -c Release -r linux-x64 --self-contained \
    -p:DebugType=none -p:DebugSymbols=false \
    -o "$APPDIR/usr/bin"

# AppRun, not a symlink to the binary: the working directory an AppImage starts
# in is wherever the user invoked it, and $APPDIR is a read-only mount, so the
# launcher's own data-directory handling has to be the thing that decides where
# files go. It does -- this just execs it.
cat > "$APPDIR/AppRun" <<RUN
#!/bin/sh
HERE="\$(dirname "\$(readlink -f "\$0")")"
exec "\$HERE/usr/bin/$NAME" "\$@"
RUN
chmod +x "$APPDIR/AppRun"

# X-AppImage-Version is what an AppImage reports about itself to the desktop and
# to update tooling; without it the number lives only in the file name, which a
# player renames. Added here rather than kept in the .desktop, so there is still
# one place the version is written.
for d in "$APPDIR/usr/share/applications/$APP_ID.desktop" "$APPDIR/$APP_ID.desktop"; do
    mkdir -p "$(dirname "$d")"
    { cat "$ROOT/packaging/shared/$APP_ID.desktop"; echo "X-AppImage-Version=$VERSION"; } > "$d"
done
# appimagetool refuses an AppDir without the icon its entry names. A port that
# ships no mark (its icon is the game's own, read off the player's disc and put
# into the icon theme by the game, which outranks this copy) gets a transparent
# square, so the build passes and nothing is drawn in its place.
ICON="$ROOT/packaging/shared/$APP_ID.png"
if [ ! -f "$ICON" ]; then
    ICON="$OUT/$APP_ID-blank.png"
    base64 -d > "$ICON" <<'PNG'
iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAYAAABccqhmAAABFUlEQVR42u3BMQEAAADCoPVP7WsIoAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAeAMBPAAB2ClDBAAAAABJRU5ErkJggg==
PNG
fi
cp "$ICON" "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_ID.png"
cp "$ICON" "$APPDIR/$APP_ID.png"

# Third-party licences the artifact is obliged to carry. Noto Sans is embedded in
# RecompOne.Runtime.dll (tools/RecompOne/patches/0033) and is SIL OFL 1.1, which
# requires its licence to travel with the font; the port's own MIT terms go beside
# it rather than only in the source tree.
mkdir -p "$APPDIR/usr/share/doc/$APP_ID"
cp "$ROOT/LICENSE" "$APPDIR/usr/share/doc/$APP_ID/LICENSE"
cp "$ROOT/tools/RecompOne/patches/assets/NotoSans-OFL.txt" "$APPDIR/usr/share/doc/$APP_ID/NotoSans-OFL.txt"

echo "==> appimagetool"
TOOL="${APPIMAGETOOL:-}"
if [ -z "$TOOL" ]; then
    TOOL="$OUT/appimagetool"
    if [ ! -x "$TOOL" ]; then
        curl -fsSL -o "$TOOL" \
            https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
        chmod +x "$TOOL"
    fi
fi

# ARCH is read from the environment by appimagetool and is not inferred.
ARCH=x86_64 "$TOOL" --no-appstream "$APPDIR" "$OUT/$NAME-$VERSION-x86_64.AppImage"

echo "done: $OUT/$NAME-$VERSION-x86_64.AppImage"
