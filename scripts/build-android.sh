#!/bin/sh
# Build the Android arm64 core .so and 7z.so with the NDK.
# Does not download the NDK into this repo and does not build an APK.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

if [ -n "${ANDROID_NDK_HOME:-}" ]; then
  ndk="$ANDROID_NDK_HOME"
elif [ -n "${ANDROID_NDK_ROOT:-}" ]; then
  ndk="$ANDROID_NDK_ROOT"
elif [ -x /tmp/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/clang ]; then
  ndk=/tmp/android-ndk-r27c
else
  echo "blocker: Android NDK was not found." >&2
  echo "Set ANDROID_NDK_HOME, or unpack android-ndk-r27c so clang is at" >&2
  echo "  /tmp/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" >&2
  echo "NDK zip: https://dl.google.com/android/repository/android-ndk-r27c-linux.zip" >&2
  exit 1
fi

pre="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
cc="$pre/aarch64-linux-android24-clang"
cxx="$pre/aarch64-linux-android24-clang++"
if [ ! -x "$cc" ] || [ ! -x "$cxx" ]; then
  echo "blocker: NDK clang for aarch64 android-24 is missing under $ndk" >&2
  exit 1
fi
if [ ! -f "$ndk/build/cmake/android.toolchain.cmake" ]; then
  echo "blocker: $ndk/build/cmake/android.toolchain.cmake is missing" >&2
  exit 1
fi

"$root/scripts/fetch-7zip.sh"
src="$root/third_party/7zip/src"
bundle="$src/CPP/7zip/Bundles/Format7zF"
make -C "$bundle" -j"$(nproc)" -f ../../cmpl_gcc.mak \
  CC="$cc" \
  CXX="$cxx" \
  O=b/android-arm64 \
  LFLAGS_STRIP= \
  CFLAGS_WARN= \
  CFLAGS_WARN_WALL="-Wall -Wextra" \
  LIB2=
seven="$bundle/b/android-arm64/7z.so"
test -f "$seven"

cmake -S "$root" -B "$root/build-android" \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DSEVENZIP_SRC="$src" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build-android" -j"$(nproc)"

out="$root/build-android/arm64-v8a"
mkdir -p "$out"
cp -f "$root/build-android/libarchiver.so" "$out/libarchiver.so"
cp -f "$root/build-android/libarchiver_jni.so" "$out/libarchiver_jni.so"
cp -f "$seven" "$out/7z.so"
cp -f "$seven" "$out/lib7z.so"
pkg="$root/build-android/jni-package/arm64-v8a"
mkdir -p "$pkg"
cp -f "$out/"*.so "$pkg/"

echo "Android core: $out/libarchiver.so"
echo "Android JNI:  $out/libarchiver_jni.so"
echo "Android 7-Zip library: $out/lib7z.so"
echo "No APK. This script does not install the Android SDK."
