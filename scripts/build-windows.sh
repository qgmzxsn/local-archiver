#!/bin/sh
# Cross-build the Windows CLI and 7z.dll on Linux with MinGW.
# The Qt shell is not part of this build: this machine has no Windows Qt kit.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
  echo "blocker: x86_64-w64-mingw32-g++ is not installed." >&2
  echo "On Debian/Ubuntu: sudo apt-get install g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64" >&2
  exit 1
fi
if ! command -v x86_64-w64-mingw32-windres >/dev/null 2>&1; then
  echo "blocker: x86_64-w64-mingw32-windres is not installed." >&2
  exit 1
fi

"$root/scripts/fetch-7zip.sh"
src="$root/third_party/7zip/src"
bundle="$src/CPP/7zip/Bundles/Format7zF"
make -C "$bundle" -j"$(nproc)" -f ../../cmpl_gcc.mak \
  CROSS_COMPILE=x86_64-w64-mingw32- \
  IS_MINGW=1 \
  RC=x86_64-w64-mingw32-windres \
  O=b/mingw64 \
  LFLAGS_STRIP=
dll="$bundle/b/mingw64/7z.dll"
test -f "$dll"

cmake -S "$root" -B "$root/build-windows" \
  -DCMAKE_TOOLCHAIN_FILE="$root/cmake/mingw-w64-x86_64.cmake" \
  -DSEVENZIP_SRC="$src" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build-windows" -j"$(nproc)"
cp -f "$dll" "$root/build-windows/7z.dll"

echo "Windows CLI: $root/build-windows/archiver-cli.exe"
echo "Windows 7-Zip library: $root/build-windows/7z.dll"
echo "The Qt shell was not linked. There is no Windows Qt kit on this machine."
