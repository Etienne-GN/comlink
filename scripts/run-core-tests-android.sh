#!/usr/bin/env bash
set -uo pipefail

# Runs the core's own test executables on a connected Android device or
# emulator. This is the only thing that turns "it cross-compiles" into
# "it runs": everything scripts/build-core-android.sh proves stops at
# the linker.
#
# Needs a booted emulator (or a handset with adb) for the same ABI the
# build targeted. scripts/start-emulator-android.sh brings one up.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ABI="${1:-x86_64}"
BUILD="$ROOT/build-android-$ABI"
DEV=/data/local/tmp/comlink

NDK_VERSION="27.3.13750724"
NDK="${ANDROID_NDK_ROOT:-/usr/lib/android-sdk/ndk/${NDK_VERSION}}"
SDK="${ANDROID_SDK_ROOT:-$HOME/android-sdk}"
ADB="${ADB:-$SDK/platform-tools/adb}"

case "$ABI" in
    arm64-v8a) SYSLIB=aarch64-linux-android ;;
    x86_64)    SYSLIB=x86_64-linux-android ;;
    *) echo "unsupported ABI '$ABI'" >&2; exit 1 ;;
esac
CXXLIB="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$SYSLIB/libc++_shared.so"

[ -x "$ADB" ] || { echo "no adb at $ADB" >&2; exit 1; }
[ -f "$BUILD/core/libcomlink_core.so" ] || {
    echo "nothing built for $ABI -- run scripts/build-core-android.sh $ABI" >&2
    exit 1
}

# An ABI mismatch produces "No such file or directory" from the loader,
# which reads like a missing file rather than a wrong architecture.
have="$("$ADB" shell getprop ro.product.cpu.abi | tr -d '\r')"
[ "$have" = "$ABI" ] || {
    echo "device reports ABI '$have', these binaries are '$ABI'" >&2
    exit 1
}

echo "=== pushing to $DEV"
"$ADB" shell "rm -rf $DEV && mkdir -p $DEV/tmp" || exit 1
"$ADB" push "$BUILD/core/libcomlink_core.so" "$CXXLIB" "$DEV/" > /dev/null
"$ADB" push "$ROOT/packaging/sounds" "$DEV/" > /dev/null
for t in "$BUILD"/core/test_*; do
    "$ADB" push "$t" "$DEV/" > /dev/null
done
"$ADB" shell "chmod +x $DEV/test_*"

env="LD_LIBRARY_PATH=$DEV SP_TEST_TMP=$DEV/tmp COMLINK_SOUND_DIR=$DEV/sounds"
pass=0; fail=0; failed=""
for t in "$BUILD"/core/test_*; do
    n="$(basename "$t")"
    # Each test gets a clean scratch directory: they were written for
    # a build tree that ctest empties, not a device that keeps state.
    "$ADB" shell "rm -rf $DEV/tmp && mkdir -p $DEV/tmp"
    out="$("$ADB" shell "cd $DEV && $env ./$n > /dev/null 2>&1; echo \$?")"
    if [ "$(echo "$out" | tr -d '\r')" = "0" ]; then
        echo "  ok   $n"; pass=$((pass + 1))
    else
        echo "  FAIL $n"; fail=$((fail + 1)); failed="$failed $n"
    fi
done

echo
echo "=== $pass passed, $fail failed, on $ABI / API $("$ADB" shell getprop ro.build.version.sdk | tr -d '\r')"
[ "$fail" -eq 0 ] || { echo "failed:$failed"; exit 1; }
