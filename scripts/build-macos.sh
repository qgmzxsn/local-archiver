#!/bin/sh
# A Mac is required. This Linux environment cannot produce a macOS binary.
set -eu
os=$(uname -s 2>/dev/null || echo unknown)
echo "A Mac is required to build the macOS app. This system is ${os}." >&2
echo "No macOS binary was produced here, and none should be treated as done." >&2
if [ "$os" != "Darwin" ]; then
  echo "On a Mac: install Qt 6, run scripts/fetch-7zip.sh, then" >&2
  echo "  make -C third_party/7zip/src/CPP/7zip/Bundles/Format7zF -f ../../cmpl_gcc.mak" >&2
  echo "  cmake -S . -B build -DSEVENZIP_SRC=\$PWD/third_party/7zip/src" >&2
  echo "  cmake --build build" >&2
  exit 1
fi
echo "Darwin was detected, but this script was not verified on macOS." >&2
exit 1
