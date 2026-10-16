#!/bin/sh
# Honest stub. The phase 1 environment is Linux and did not build a macOS app.
set -eu
os=$(uname -s 2>/dev/null || echo unknown)
if [ "$os" != "Darwin" ]; then
  echo "This system is ${os}. No macOS application was built." >&2
  echo "Windows and macOS share the Qt sources in apps/desktop. On a Mac, install Qt 6 and build 7z.so with 7-Zip's makefile.gcc. See README.md." >&2
  exit 1
fi
echo "Darwin was detected, but this script was not verified on macOS in phase 1." >&2
exit 1
