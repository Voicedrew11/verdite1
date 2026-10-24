#!/bin/sh
# Builds the AppImage inside the builder image (scripts/package-linux.sh runs it):
#   REPO   this repository, read-only, at its own path (git finds a worktree's and the
#          submodule's repositories by absolute path)
#   /out   the build directory and the finished AppImage
set -eu
repo=$1
version=$2
build=/out/build
appdir=/out/AppDir
name="Verdite1-v$version-x86_64.AppImage"

# Release: line tables only, so a crash report names the stack. The C++ runtime is linked in, so
# the game needs only glibc and the libraries every desktop has. No English delta is embedded:
# builds that carry it are never distributed.
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/deps \
  -DKF_ENGLISH_PATCH=/nonexistent -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc -fuse-ld=lld-21" \
  -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib'
cmake --build "$build" --target verdite1

rm -rf "$appdir"
cmake --install "$build" --prefix "$appdir/usr"
mkdir -p "$appdir/usr/lib" "$appdir/usr/share/doc/verdite1/licenses"
cp -L /opt/deps/lib/libSDL3.so.0 "$appdir/usr/lib/"
cp /opt/deps/bin/bsdtar "$appdir/usr/bin/"
# The example mods are copied from beside the executable on the first launch.
cp -r "$repo/mods" "$appdir/usr/bin/mods"
cp "$repo/packaging/appimage/AppRun" "$appdir/AppRun"
cp "$repo/packaging/appimage/verdite1.desktop" "$repo/packaging/appimage/verdite1.svg" "$appdir/"
doc="$appdir/usr/share/doc/verdite1"
cp "$repo/docs/release/README.txt" "$repo/docs/MODDING.md" "$doc/"
cp "$repo/LICENSE" "$doc/licenses/Verdite1-CC0.txt"
cp "$repo/third_party/imgui/LICENSE.txt" "$doc/licenses/DearImGui.txt"
cp "$repo/third_party/lua/LICENSE" "$doc/licenses/Lua.txt"
cp "$repo/third_party/cjson/LICENSE" "$doc/licenses/cJSON.txt"
cp "$repo/third_party/libchdr/LICENSE.txt" "$doc/licenses/libchdr.txt"
cp "$repo/third_party/libchdr/deps/zstd-1.5.7/LICENSE" "$doc/licenses/zstd.txt"
cp /opt/deps/share/licenses/SDL3/LICENSE.txt "$doc/licenses/SDL3.txt"
chmod +x "$appdir/AppRun"

# Nothing from the build image but glibc and the system's own libraries may be needed.
if LD_LIBRARY_PATH= ldd "$appdir/usr/bin/verdite1" | grep -E 'not found|/opt/deps|libstdc\+\+|libgcc_s'; then
  echo "verdite1 links a library the AppImage does not carry" >&2
  exit 1
fi

appimagetool --no-appstream --runtime-file /opt/appimage-runtime "$appdir" "/out/$name"
echo "Created $name"
