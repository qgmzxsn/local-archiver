#!/bin/sh
# Build upstream 7z.so. Our programs dlopen it. They do not link it in.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
src="$root/third_party/7zip/src"
if [ ! -f "$src/CPP/7zip/Bundles/Format7zF/makefile.gcc" ]; then
  echo "7-Zip source is missing. Run scripts/fetch-7zip.sh" >&2
  exit 1
fi
make -C "$src/CPP/7zip/Bundles/Format7zF" -j"$(nproc)" -f ../../cmpl_gcc.mak LFLAGS_STRIP=
out="$src/CPP/7zip/Bundles/Format7zF/b/g/7z.so"
test -f "$out"
echo "$out"
