#!/usr/bin/env bash
set -euo pipefail

# Cross-builds whisper.cpp into the Android prefix, so the handset
# transcribes calls the way the desktop does. Debian links its own
# libwhisper-dev; there is no such package here.
#
# Its own script rather than a stanza in build-deps-android.sh, which
# calls it: that script rebuilds every dependency from scratch, and
# whisper is the one most likely to be rebuilt on its own. Run alone
# after build-deps-android.sh has built the prefix.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Same defaults and override as build-deps-android.sh: a build nobody
# is waiting on does not get the operator's cores.
sp_jobs() {
    local n
    n="$(nproc)"
    local want=3
    [ "$n" -lt "$want" ] && want="$n"
    echo "${COMLINK_JOBS:-$want}"
}
sp_nice() { nice -n 19 ionice -c 3 "$@"; }

ABI="${1:-arm64-v8a}"
API="${COMLINK_ANDROID_API:-23}"
NDK_VERSION="27.3.13750724"
NDK="${ANDROID_NDK_ROOT:-/usr/lib/android-sdk/ndk/${NDK_VERSION}}"
PREFIX="$ROOT/third_party/prefix-android-$ABI"
SRC="$ROOT/third_party/whisper.cpp"
BUILD="$ROOT/build-android-whisper-$ABI"

[ -f "$SRC/CMakeLists.txt" ] || {
    echo "whisper.cpp submodule not checked out" >&2
    exit 1
}
[ -d "$NDK" ] || { echo "NDK not found at $NDK" >&2; exit 1; }

echo "=== whisper.cpp ($ABI)"
# Static and CPU only. No OpenMP: the NDK's libomp is a shared library
# the APK would have to carry, and ggml's own thread pool does the same
# job. No native-CPU tuning: the build machine is not the handset.
sp_nice cmake -S "$SRC" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF -DGGML_OPENMP=OFF -DGGML_NATIVE=OFF \
    -DWHISPER_BUILD_EXAMPLES=OFF -DWHISPER_BUILD_TESTS=OFF \
    -DWHISPER_BUILD_SERVER=OFF >/dev/null
sp_nice cmake --build "$BUILD" --parallel "$(sp_jobs)" >/dev/null
cmake --install "$BUILD" >/dev/null
echo "=== whisper.cpp installed into $PREFIX"
