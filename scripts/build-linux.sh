#!/bin/sh
# Verified path: Linux core, command-line tool, and tests.
# This does not produce a Windows, macOS, or Android install package.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
"$root/scripts/fetch-7zip.sh"
"$root/scripts/build-7zip-linux.sh"
cmake -S "$root" -B "$root/build" -DSEVENZIP_SRC="$root/third_party/7zip/src" -DCMAKE_CXX_COMPILER=g++
cmake --build "$root/build" -j"$(nproc)"
cp -f "$root/third_party/7zip/src/CPP/7zip/Bundles/Format7zF/b/g/7z.so" "$root/build/7z.so"
"$root/build/test_path_safety"
python3 "$root/tests/test_extract.py" "$root/build/archiver-cli" "$root/build/libarchiver.so" "$root/build/7z.so"
