#!/bin/sh
# Honest stub. The phase 1 environment is Linux and did not build a Windows app.
set -eu
os=$(uname -s 2>/dev/null || echo unknown)
case "$os" in
  MINGW*|MSYS*|CYGWIN*)
    echo "A Windows shell was detected, but this script was not run or verified there." >&2
    echo "Install Qt 6, build 7z.dll from third_party/7zip/src/CPP/7zip/Bundles/Format7zF, then run cmake. See README.md." >&2
    exit 1
    ;;
esac
echo "This system is ${os}. No Windows executable was built." >&2
echo "Windows and macOS share the Qt sources in apps/desktop. Build that project on Windows." >&2
exit 1
