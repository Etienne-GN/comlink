#!/usr/bin/env bash
set -euo pipefail

# Boots the x86_64 emulator the core's tests run on, headless, and
# waits until Android says it is up.
#
# The SDK lives in $HOME/android-sdk rather than the distribution's
# /usr/lib/android-sdk, which is root-owned and therefore unwritable
# by sdkmanager. Only the NDK comes from the Debian package; the
# emulator, the system image and the current cmdline-tools come from
# Google's repository. See docs/android-emulator.md for the one-time
# setup, which this script deliberately does not perform: downloading
# 2.5 GB and accepting Google's licences is the operator's call.

SDK="${ANDROID_SDK_ROOT:-$HOME/android-sdk}"
# avdmanager is a Java program and the default JDK here is 11, which
# it exits from without a word -- so the AVD looks absent when it is
# simply unreadable.
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}"
AVD="${COMLINK_AVD:-comlink-x86_64}"

[ -x "$SDK/emulator/emulator" ] || {
    echo "no emulator at $SDK/emulator -- see docs/android-emulator.md" >&2
    exit 1
}
"$SDK/cmdline-tools/latest/bin/avdmanager" list avd 2>/dev/null \
    | grep -q "Name: $AVD" || {
    echo "no AVD named '$AVD' -- see docs/android-emulator.md" >&2
    exit 1
}

ADB="$SDK/platform-tools/adb"

if "$ADB" devices | grep -q "^emulator-.*device$"; then
    echo "an emulator is already running"
    exit 0
fi

# swiftshader_indirect: software GL. The host GPU is not worth
# involving for a headless run, and a host driver that the emulator
# dislikes fails in ways that look like Android problems.
ANDROID_SDK_ROOT="$SDK" nohup "$SDK/emulator/emulator" -avd "$AVD" \
    -no-window -no-audio -no-boot-anim -no-snapshot \
    -gpu swangle_indirect > "${TMPDIR:-/var/tmp}/comlink-emulator.log" 2>&1 &

echo "waiting for boot"
timeout 300 "$ADB" wait-for-device shell \
    'while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 2; done' || {
    echo "emulator did not finish booting in five minutes" >&2
    exit 1
}
echo "up: $("$ADB" shell getprop ro.product.cpu.abi | tr -d '\r')," \
     "API $("$ADB" shell getprop ro.build.version.sdk | tr -d '\r')"
