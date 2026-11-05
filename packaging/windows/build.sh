#!/bin/sh
# Builds the Windows package inside the builder image (scripts/package-windows.sh runs it):
#   REPO   this repository, read-only, at its own path
#   /out   the build directory and the finished zip
set -eu
repo=$1
version=$2
build=/out/build
name="Verdite1-$version-win-x64"
stage="/out/$name"

# Release: line tables only, so a crash report names the stack. -static: the C++ runtime is in the
# exe and SDL3.dll is the one library beside it. No English delta is embedded: builds that carry it
# are never distributed.
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_TOOLCHAIN_FILE=/opt/toolchain.cmake \
  -DCMAKE_PREFIX_PATH=/opt/win-deps -DKF_RELEASE=ON -DPython3_EXECUTABLE=/usr/bin/python3.11 -DKF_ENGLISH_PATCH=/nonexistent \
  -DKF_HOST_CLANG=/opt/llvm-mingw/bin/x86_64-w64-mingw32-clang++ \
  -DOPENSSL_USE_STATIC_LIBS=ON -DOPENSSL_ROOT_DIR=/opt/win-deps \
  -DZLIB_LIBRARY=/opt/win-deps/lib/libzlibstatic.a -DZLIB_INCLUDE_DIR=/opt/win-deps/include \
  -DCMAKE_EXE_LINKER_FLAGS=-static
cmake --build "$build" --target verdite1 -- -k 0
ccache --show-stats

rm -rf "$stage"
mkdir -p "$stage/licenses"
cp "$build/verdite1.exe" /opt/win-deps/bin/SDL3.dll "$stage/"
cp "$repo/docs/release/README.txt" "$repo/docs/MODDING.md" "$stage/"
cp -r "$repo/mods" "$stage/mods"
cp "$repo/LICENSE" "$stage/licenses/Verdite1-MIT.txt"
cp "$repo/third_party/imgui/LICENSE.txt" "$stage/licenses/DearImGui.txt"
cp "$repo/third_party/lua/LICENSE" "$stage/licenses/Lua.txt"
cp "$repo/third_party/cjson/LICENSE" "$stage/licenses/cJSON.txt"
cp "$repo/third_party/libchdr/LICENSE.txt" "$stage/licenses/libchdr.txt"
cp "$repo/third_party/libchdr/deps/zstd-1.5.7/LICENSE" "$stage/licenses/zstd.txt"
cp /opt/win-deps/SDL3-LICENSE.txt "$stage/licenses/SDL3.txt"
cp /opt/libarchive/COPYING "$stage/licenses/libarchive.txt"
cp /opt/win-deps/zlib-LICENSE.txt "$stage/licenses/zlib.txt"
cp /opt/libdatachannel/LICENSE "$stage/licenses/libdatachannel.txt"
cp /opt/libdatachannel/deps/libjuice/LICENSE "$stage/licenses/libjuice.txt"
cp /opt/libdatachannel/deps/usrsctp/LICENSE.md "$stage/licenses/usrsctp.txt"
cp /opt/libdatachannel/deps/plog/LICENSE "$stage/licenses/plog.txt"

# Nothing but SDL3.dll and Windows' own libraries may be needed.
if x86_64-w64-mingw32-objdump -p "$stage/verdite1.exe" | grep 'DLL Name' \
    | grep -iE 'libc\+\+|libunwind|libwinpthread|libcrypto|libssl|libgcc|zlib|libarchive'; then
  echo "verdite1.exe links a library the package does not carry" >&2
  exit 1
fi
(cd /out && rm -f "$name.zip" && zip -qr "$name.zip" "$name")
echo "Created $name.zip"
