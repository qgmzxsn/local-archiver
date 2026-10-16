#!/bin/sh
# Honest stub. No Android SDK or NDK was available, and no APK was produced.
set -eu
echo "The Android app was not built." >&2
echo "Source only: apps/android (Kotlin shell) and the shared C++ core." >&2
echo "This environment had no Android SDK and no NDK, so no APK and no Android .so were produced." >&2
if [ -z "${ANDROID_NDK_HOME:-}${ANDROID_NDK_ROOT:-}" ]; then
  echo "ANDROID_NDK_HOME is not set." >&2
fi
exit 1
