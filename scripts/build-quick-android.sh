#!/usr/bin/env bash
set -euo pipefail

# Builds the QML frontend as an APK: the same Main.qml the desktop
# renders, packaged by androiddeployqt against the cross-built core.
#
# Needs both prefixes to exist already --
#   scripts/build-deps-android.sh <abi>   (pjproject, opus, ssl, ...)
#   scripts/build-qt-android.sh   <abi>   (Qt itself)
# -- because this script builds Comlink and nothing else.

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

ABI="${1:-x86_64}"
QT="$ROOT/third_party/prefix-qt-android-$ABI"
DEPS="$ROOT/third_party/prefix-android-$ABI"
BUILD="$ROOT/build-quick-$ABI"

export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/android-sdk}"
export ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-/usr/lib/android-sdk/ndk/27.3.13750724}"
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}"

[ -x "$QT/bin/qt-cmake" ] || {
    echo "no Qt for Android at $QT" >&2
    echo "Run: ./scripts/build-qt-android.sh $ABI" >&2
    exit 1
}
[ -f "$DEPS/lib/pkgconfig/libpjproject.pc" ] || {
    echo "no core dependencies at $DEPS" >&2
    echo "Run: ./scripts/build-deps-android.sh $ABI" >&2
    exit 1
}

# qt-cmake supplies the Android toolchain file and the Qt prefix, so
# this passes only what is ours: where the core's libraries are.
"$QT/bin/qt-cmake" -S "$ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCOMLINK_DEPS_PREFIX="$DEPS" \
    -DQT_ANDROID_ABIS="$ABI"

sp_nice cmake --build "$BUILD" --target comlink-quick -j"$(sp_jobs)"
# Qt names the packaging step after the target; it runs
# androiddeployqt with everything CMake recorded about the build.
sp_nice cmake --build "$BUILD" --target comlink-quick_make_apk

# Signed here rather than by hand afterwards, so what gets installed
# is reproducible and nobody has to remember the incantation. The key
# is Android's own debug key: it is the one every SDK install already
# has, it is not a secret, and a real key is a decision that has not
# been made yet.
UNSIGNED="$BUILD/frontend/quick/android-build/build/outputs/apk/release/android-build-release-unsigned.apk"
SIGNED="$BUILD/comlink-$ABI.apk"
APKSIGNER="$(ls -d "$ANDROID_SDK_ROOT"/build-tools/*/apksigner 2>/dev/null |
             sort -V | tail -1)"
# The SDK's, not Debian's: Debian ships build-tools 19.1.0, whose
# zipalign has no -p and cannot page-align the shared libraries.
ZIPALIGN="$(ls -d "$ANDROID_SDK_ROOT"/build-tools/*/zipalign 2>/dev/null |
            sort -V | tail -1)"

if [ -f "$UNSIGNED" ] && [ -n "$APKSIGNER" ] && [ -n "$ZIPALIGN" ] &&
   [ -f "$HOME/.android/debug.keystore" ]; then
    # zipalign before signing: apksigner refuses to align afterwards,
    # and an unaligned APK loads its resources through a copy instead
    # of mapping them.
    "$ZIPALIGN" -p -f 4 "$UNSIGNED" "$SIGNED"
    "$APKSIGNER" sign \
        --ks "$HOME/.android/debug.keystore" \
        --ks-pass pass:android --key-pass pass:android \
        --ks-key-alias androiddebugkey \
        "$SIGNED"
    "$APKSIGNER" verify --print-certs "$SIGNED" | head -3
    echo "=== signed: $SIGNED ($(stat -c %s "$SIGNED") bytes)"
else
    # A build that produced nothing installable has not succeeded,
    # whatever the compiler said.
    echo "not signed: no apksigner, no zipalign, or no keystore" >&2
    find "$BUILD" -name "*.apk" -printf "=== %p (%s bytes)\n"
    exit 1
fi

find "$BUILD" -name "*.apk" -printf "=== %p (%s bytes)\n"
