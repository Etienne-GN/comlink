#!/usr/bin/env bash
set -euo pipefail

# Cross-builds libcomlink_core.so for one Android ABI.
#
# Run scripts/build-deps-android.sh for the same ABI first: this script
# builds nothing but the core and expects the dependency prefix to be
# populated already.
#
# The core target and its source list come from core/CMakeLists.txt,
# the same one the desktop uses. The two platforms differ by a prefix
# and a toolchain file, not by a second list that would drift.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Build parallelism, and niceness, so a long build does not make the
# machine unusable while it runs. nproc on this box is the number of
# cores the operator also needs to work with; taking all of them for
# forty minutes is not a reasonable default for a build nobody is
# waiting on. COMLINK_JOBS overrides.
sp_jobs() {
    local n
    n="$(nproc)"
    # Three, measured on the operator's laptop: three is comfortable
    # while he works, six stutters his music. A build nobody is
    # waiting on does not get the other cores.
    local want=3
    [ "$n" -lt "$want" ] && want="$n"
    echo "${COMLINK_JOBS:-$want}"
}
# nice for CPU, ionice for the disk: a parallel compile starves
# interactive work through the IO scheduler as readily as the CPU one.
sp_nice() { nice -n 19 ionice -c 3 "$@"; }

ABI="${1:-arm64-v8a}"
API="${COMLINK_ANDROID_API:-23}"

NDK_VERSION="27.3.13750724"
NDK="${ANDROID_NDK_ROOT:-/usr/lib/android-sdk/ndk/${NDK_VERSION}}"
have="$(sed -n 's/^Pkg.Revision *= *//p' "$NDK/source.properties" 2>/dev/null)"
[ "$have" = "$NDK_VERSION" ] || {
    echo "NDK version mismatch: $NDK has '$have', expected '$NDK_VERSION'." >&2
    echo "The dependencies in the prefix were built with that one." >&2
    exit 1
}

PREFIX="$ROOT/third_party/prefix-android-$ABI"
BUILD="$ROOT/build-android-$ABI"

[ -f "$PREFIX/lib/pkgconfig/libpjproject.pc" ] || {
    echo "no dependency prefix at $PREFIX" >&2
    echo "Run: ./scripts/build-deps-android.sh $ABI" >&2
    exit 1
}

# LIBDIR rather than PATH, because PATH only prepends: with the
# system directories still in the search list, a missing Android
# library is answered by the host's copy and the build cheerfully
# links x86-64 objects into an aarch64 target. Emptying LIBDIR to
# exactly one directory is what makes a missing dependency fail.
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
# The .pc files in that prefix hold absolute paths that are already
# correct, including one into the NDK sysroot for libc++_shared. Left
# to itself CMake sets this to CMAKE_SYSROOT and every path comes back
# with the sysroot glued in front of it.
export PKG_CONFIG_SYSROOT_DIR=""

cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCOMLINK_CORE_ONLY=ON \
    -DCOMLINK_DEPS_PREFIX="$PREFIX"

sp_nice cmake --build "$BUILD" --target comlink_core -j"$(sp_jobs)"

echo
echo "=== built $BUILD/core/libcomlink_core.so"
