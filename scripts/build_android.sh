#!/usr/bin/env bash
# Cross-compile the loader, shim, and example mods for Android arm64-v8a.
#
# NDK resolution order:
#   $MOCHA_NDK
#   Unity's bundled NDK for a 6000.x editor
#   $ANDROID_NDK_HOME / $ANDROID_NDK_ROOT
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

ndk="${MOCHA_NDK:-}"
if [[ -z "$ndk" ]]; then
    for candidate in "$HOME"/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer/NDK; do
        [[ -f "$candidate/build/cmake/android.toolchain.cmake" ]] && ndk="$candidate"
    done
fi
if [[ -z "$ndk" ]]; then
    ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
fi
if [[ -z "$ndk" || ! -f "$ndk/build/cmake/android.toolchain.cmake" ]]; then
    echo "error: Android NDK not found; set MOCHA_NDK" >&2
    exit 1
fi

build_dir="${BUILD_DIR:-build/android}"
api="${MOCHA_ANDROID_API:-26}"
abi="${MOCHA_ANDROID_ABI:-arm64-v8a}"

echo "NDK: $ndk"
echo "ABI: $abi, API: $api"

cmake -S . -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM="android-$api" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$build_dir"

echo
echo "artifacts:"
ls -l "$build_dir"/native/libmain.so "$build_dir"/native/libmocha.so "$build_dir"/examples/*.so
