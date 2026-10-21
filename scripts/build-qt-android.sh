#!/usr/bin/env bash
set -euo pipefail

# Cross-builds Qt 6 for Android, so one QML tree renders on Debian and
# on a handset.
#
# From source, not from Qt's installer. Qt stopped publishing Android
# binaries publicly after 6.6.3 -- download.qt.io's android/ tree ends
# there while desktop/ goes to 6.12 -- so the binary route needs a Qt
# account. The source tarballs are public, and building them is the
# same thing this project already does for pjproject: a dependency
# whose provenance is a URL and a version, not "whatever was
# installed".
#
# The version must match the host Qt exactly, because the host
# supplies moc, rcc, qmlcachegen and androiddeployqt. Debian ships
# 6.11.2 including androiddeployqt, which is the whole reason this is
# straightforward.

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

QT_VERSION="6.11.2"
QT_SERIES="6.11"
# One ABI per build: "-android-abis ... Only one ABI can be
# specified" is what Qt 6.10's configure says, so each ABI gets its
# own prefix rather than sharing one.
ABI="${1:-x86_64}"
# 28, not the core's 23. Qt 6.10 does not build below it -- qlogging
# wants pthread_getname_np, which bionic gained at API 26 -- and Qt
# 6.9 onwards states Android 9 as its minimum anyway. The core's own
# libraries stay at 23 and run happily on 28; what this sets is the
# floor for the application, so the APK's minSdk becomes 28.
API="${COMLINK_ANDROID_API:-28}"

NDK_VERSION="27.3.13750724"
NDK="${ANDROID_NDK_ROOT:-/usr/lib/android-sdk/ndk/${NDK_VERSION}}"
SDK="${ANDROID_SDK_ROOT:-$HOME/android-sdk}"
HOST_QT="${COMLINK_QT_HOST:-/usr}"
# Qt's cross-build looks for the host Qt's CMake packages at
# <host>/lib/cmake. Debian is multiarch and puts them under
# lib/<triplet>/cmake, so the cross-build cannot find Qt6HostInfo and
# stops before it compiles anything. This is the documented override
# for exactly that layout.
HOST_QT_CMAKE="${COMLINK_QT_HOST_CMAKE:-/usr/lib/x86_64-linux-gnu/cmake}"

SRC="$ROOT/third_party/qt-android-src"
PREFIX="$ROOT/third_party/prefix-qt-android-$ABI"
# The core's dependencies, for OpenSSL's headers: build-deps-android.sh
# runs first.
DEPS="$ROOT/third_party/prefix-android-$ABI"
[ -f "$DEPS/include/openssl/ssl.h" ] || {
    echo "no OpenSSL at $DEPS: run scripts/build-deps-android.sh $ABI" >&2
    exit 1
}

have="$("$HOST_QT/lib/qt6/bin/qmake6" -query QT_VERSION 2>/dev/null \
        || qmake6 -query QT_VERSION 2>/dev/null || true)"
[ "$have" = "$QT_VERSION" ] || {
    echo "host Qt is '$have', this build needs '$QT_VERSION'." >&2
    echo "The host supplies moc, rcc, qmlcachegen and androiddeployqt;" >&2
    echo "a mismatch produces an APK that will not start." >&2
    exit 1
}
[ -d "$HOST_QT_CMAKE/Qt6HostInfo" ] || {
    echo "no Qt6HostInfo under $HOST_QT_CMAKE" >&2
    echo "Set COMLINK_QT_HOST_CMAKE to the host Qt's cmake directory." >&2
    exit 1
}
[ -x "$HOST_QT/lib/qt6/bin/androiddeployqt" ] || {
    echo "no androiddeployqt in the host Qt at $HOST_QT." >&2
    exit 1
}
[ -d "$NDK" ] || { echo "no NDK at $NDK" >&2; exit 1; }
[ -d "$SDK/platforms" ] || {
    echo "no Android platform in $SDK -- see docs/android-emulator.md" >&2
    exit 1
}
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}"

base="https://download.qt.io/official_releases/qt/$QT_SERIES/$QT_VERSION/submodules"
mkdir -p "$SRC"

# qtshadertools before qtdeclarative: Quick's shaders are baked at
# build time and the module does not configure without it.
for m in qtbase qtshadertools qtdeclarative; do
    tarball="$SRC/$m-everywhere-src-$QT_VERSION.tar.xz"
    if [ ! -d "$SRC/$m-everywhere-src-$QT_VERSION" ]; then
        [ -f "$tarball" ] || {
            echo "=== fetching $m"
            curl -fSL --retry 3 -o "$tarball" \
                 "$base/$m-everywhere-src-$QT_VERSION.tar.xz"
        }
        echo "=== unpacking $m"
        tar -xf "$tarball" -C "$SRC"
    fi
done

echo "=== qtbase for $ABI, API $API, NDK $NDK_VERSION"
cd "$SRC/qtbase-everywhere-src-$QT_VERSION"
if [ ! -f "build-$ABI/CMakeCache.txt" ]; then
    ./configure -prefix "$PREFIX" \
        -platform android-clang \
        -android-ndk "$NDK" \
        -android-sdk "$SDK" \
        -android-abis "$ABI" \
        -android-ndk-platform "android-$API" \
        -qt-host-path "$HOST_QT" \
        -nomake examples -nomake tests \
        -opensource -confirm-license \
        -openssl-runtime \
        -- -B "build-$ABI" \
           -DQT_HOST_PATH_CMAKE_DIR="$HOST_QT_CMAKE" \
           -DOPENSSL_ROOT_DIR="$DEPS"
fi
# HTTPS needs a TLS backend, and without OpenSSL qtbase builds only the
# certificate-only one: every https:// request fails, the speech model
# download included. Runtime-loaded, from the libssl_3.so and
# libcrypto_3.so build-deps-android.sh puts in the prefix and the APK
# ships. Set on the cache too, so a tree configured before this
# gains it on the next run instead of needing a fresh build. The header
# checks are cleared with it: a tree configured with no OpenSSL to find
# cached them as failed, and Qt then turns the feature back off.
grep -q '^FEATURE_openssl_runtime:BOOL=ON' "build-$ABI/CMakeCache.txt" ||
    sp_nice cmake -B "build-$ABI" -U 'TEST_openssl*' \
                  -DFEATURE_openssl_runtime=ON \
                  -DOPENSSL_ROOT_DIR="$DEPS" >/dev/null
grep -q '^FEATURE_openssl_runtime:BOOL=ON' "build-$ABI/CMakeCache.txt" || {
    echo "qtbase still has no OpenSSL backend: see its configure output" >&2
    exit 1
}
sp_nice cmake --build "build-$ABI" --parallel "$(sp_jobs)"
cmake --install "build-$ABI"

for m in qtshadertools qtdeclarative; do
    echo "=== $m"
    cd "$SRC/$m-everywhere-src-$QT_VERSION"
    # Guarded on the generated build system, not on CMakeCache.txt: a
    # configure that failed leaves the cache behind without ever
    # emitting a Makefile or build.ninja, and a guard on the cache
    # then skips the retry and hands make a directory it cannot
    # build. Which is exactly what happened the first time.
    if [ ! -f "build-$ABI/build.ninja" ] && [ ! -f "build-$ABI/Makefile" ]; then
        rm -rf "build-$ABI"
        "$PREFIX/bin/qt-configure-module" . -- -B "build-$ABI"
    fi
    sp_nice cmake --build "build-$ABI" --parallel "$(sp_jobs)"
    cmake --install "build-$ABI"
done

echo
echo "=== Qt $QT_VERSION for Android/$ABI installed to $PREFIX"
