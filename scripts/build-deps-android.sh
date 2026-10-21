#!/usr/bin/env bash
set -euo pipefail

# Cross-builds everything comlink_core links, for one Android ABI.
#
# The Debian counterpart is scripts/build-pjproject.sh, and this one is
# deliberately shaped like it. There, three of the four dependencies
# come from apt and only pjproject is built; here nothing comes from
# apt, because there is no apt. libyaml and libogg are submodules
# pinned to the tags matching the system versions, and SQLite is its
# vendored amalgamation -- see third_party/sqlite/README.md for why
# that one is not a submodule.
#
# Output: third_party/prefix-android-<abi>/{lib,include}, which is what
# scripts/build-core-android.sh passes to CMake as COMLINK_DEPS_PREFIX.

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

# Pinned, not discovered. A binary toolchain from outside the
# distribution is not a build input anybody can reproduce if the
# version is "whatever happened to be installed" -- and an NDK major
# release routinely removes something an autotools project relied on.
NDK_VERSION="27.3.13750724"

# Matches third_party/sqlite/README.md; only the .pc file needs it.
SQLITE_VERSION="3.53.4"
NDK_DEFAULT="/usr/lib/android-sdk/ndk/${NDK_VERSION}"
NDK="${ANDROID_NDK_ROOT:-$NDK_DEFAULT}"

[ -d "$NDK" ] || {
    echo "NDK not found at $NDK" >&2
    echo "Install it, or set ANDROID_NDK_ROOT. This build is pinned to" >&2
    echo "NDK $NDK_VERSION (r27d)." >&2
    exit 1
}
have="$(sed -n 's/^Pkg.Revision *= *//p' "$NDK/source.properties" 2>/dev/null)"
[ "$have" = "$NDK_VERSION" ] || {
    echo "NDK version mismatch: $NDK has '$have', this build is pinned" >&2
    echo "to '$NDK_VERSION'. Point ANDROID_NDK_ROOT at that version, or" >&2
    echo "change NDK_VERSION here deliberately -- not by accident." >&2
    exit 1
}

case "$ABI" in
    arm64-v8a) TRIPLE=aarch64-linux-android; SSL_TARGET=android-arm64 ;;
    x86_64)    TRIPLE=x86_64-linux-android;  SSL_TARGET=android-x86_64 ;;
    *) echo "unsupported ABI '$ABI' -- this build targets arm64-v8a" >&2
       echo "(the handset) and x86_64 (the emulator). See the plan for" >&2
       echo "why 32-bit is left out." >&2
       exit 1 ;;
esac

TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
PREFIX="$ROOT/third_party/prefix-android-$ABI"
export CC="$TC/bin/${TRIPLE}${API}-clang"
export CXX="$TC/bin/${TRIPLE}${API}-clang++"
export AR="$TC/bin/llvm-ar"
export RANLIB="$TC/bin/llvm-ranlib"
export STRIP="$TC/bin/llvm-strip"

[ -x "$CC" ] || { echo "no compiler at $CC" >&2; exit 1; }

# -fPIC is mandatory, not an optimisation, for the same reason the
# Debian script says so: these static archives end up inside a shared
# object, and without it the link fails on a relocation in a read-only
# section.
export CFLAGS="${CFLAGS:-} -fPIC -O2"
export CXXFLAGS="${CXXFLAGS:-} -fPIC -O2"

echo "=== $ABI, API $API, NDK $NDK_VERSION"
echo "=== prefix: $PREFIX"

# Emptied, not topped up. pjproject compiles with -I$PREFIX/include on
# its command line -- --with-opus puts it there -- and installs its own
# headers into that same directory. A second run therefore reads the
# previous run's pjmedia-codec/config_auto.h in preference to the one
# configure just generated, and the build silently keeps whatever the
# last build decided. That is how enabling Opus produced a 744-byte
# opus.o: the stale header still said PJMEDIA_HAS_OPUS_CODEC 0.
rm -rf "$PREFIX"
mkdir -p "$PREFIX/lib/pkgconfig" "$PREFIX/include"

# --- libyaml ---------------------------------------------------------
echo "=== libyaml"
cd "$ROOT/third_party/libyaml"
[ -f configure ] || ./bootstrap
make distclean >/dev/null 2>&1 || true
./configure --host="$TRIPLE" --prefix="$PREFIX" \
            --enable-static --disable-shared >/dev/null
sp_nice make -j"$(sp_jobs)" >/dev/null
make install >/dev/null

# --- libogg ----------------------------------------------------------
echo "=== libogg"
cd "$ROOT/third_party/libogg"
[ -f configure ] || ./autogen.sh
make distclean >/dev/null 2>&1 || true
./configure --host="$TRIPLE" --prefix="$PREFIX" \
            --enable-static --disable-shared >/dev/null
sp_nice make -j"$(sp_jobs)" >/dev/null
make install >/dev/null

# --- libopus ---------------------------------------------------------
# Needed twice over. The core encodes recordings with it directly
# (oggopus_write.c includes <opus/opus.h>), and pjproject needs it for
# the SIP codec. On Debian both come from the system package; here
# neither does.
echo "=== libopus"
cd "$ROOT/third_party/libopus"
[ -f configure ] || ./autogen.sh
make distclean >/dev/null 2>&1 || true
./configure --host="$TRIPLE" --prefix="$PREFIX" \
            --enable-static --disable-shared \
            --disable-doc --disable-extra-programs >/dev/null
sp_nice make -j"$(sp_jobs)" >/dev/null
make install >/dev/null

# --- sqlite ----------------------------------------------------------
# One translation unit, so no build system is involved. The defines
# match what Debian's libsqlite3 enables for the features this project
# uses: FTS and JSON are not among them, so they are off rather than
# carried.
echo "=== sqlite"
cd "$ROOT/third_party/sqlite"
"$CC" $CFLAGS -c sqlite3.c -o "$PREFIX/sqlite3.o" \
      -DSQLITE_ENABLE_COLUMN_METADATA \
      -DSQLITE_THREADSAFE=1 \
      -DSQLITE_DEFAULT_FOREIGN_KEYS=1
"$AR" rcs "$PREFIX/lib/libsqlite3.a" "$PREFIX/sqlite3.o"
rm -f "$PREFIX/sqlite3.o"
cp sqlite3.h "$PREFIX/include/"

# The amalgamation has no build system, so nothing writes the
# pkg-config file that core/CMakeLists.txt looks SQLite up with. Kept
# field-for-field like Debian's own sqlite3.pc, minus the -lz that
# only the zipfile extension needs.
cat > "$PREFIX/lib/pkgconfig/sqlite3.pc" <<EOF
prefix=$PREFIX
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: SQLite
Description: SQL database engine
Version: $SQLITE_VERSION
Libs: -L\${libdir} -lsqlite3
Libs.private: -lm
Cflags: -I\${includedir}
EOF

# --- openssl ---------------------------------------------------------
# Without this pjproject has no TLS transport, which for a SIP client
# that registers over TLS is not a missing extra. Like Opus, it is not
# autodetected when cross-compiling -- see the pjproject stanza below.
#
# Shared, not static: Qt's TLS backend loads OpenSSL at run time, and
# one copy in the APK serves both it and pjproject. Two copies in one
# process -- a static one inside the core and a shared one for Qt --
# is two sets of the same global symbols. The names carry Qt's "_3"
# suffix (scripts/openssl-android.conf), and the static archives are
# removed so nothing can quietly link a second copy.
#
# Configure builds its own compiler command from ANDROID_NDK_ROOT and
# the toolchain on PATH, so the environment this script exports for
# the autotools packages is cleared for it rather than fought with.
echo "=== openssl"
cd "$ROOT/third_party/openssl"
(
    unset CC CXX AR RANLIB STRIP CFLAGS CXXFLAGS LDFLAGS
    export ANDROID_NDK_ROOT="$NDK"
    export PATH="$TC/bin:$PATH"
    make distclean >/dev/null 2>&1 || true
    ./Configure --config="$ROOT/scripts/openssl-android.conf" \
                "comlink-$SSL_TARGET" -D__ANDROID_API__="$API" -fPIC \
                shared no-tests no-docs \
                --prefix="$PREFIX" --libdir=lib \
                --openssldir="$PREFIX/ssl" >/dev/null
    sp_nice make -j"$(sp_jobs)" >/dev/null
    # install_sw: libraries, headers and pkg-config, without the man
    # pages and the sample openssl.cnf that install would also place.
    make install_sw >/dev/null
)
rm -f "$PREFIX/lib/libssl.a" "$PREFIX/lib/libcrypto.a"

# --- pjproject -------------------------------------------------------
# Last, because it links against libopus from the prefix above.
#
# configure-android reads ANDROID_NDK_ROOT and TARGET_ABI from the
# environment rather than taking them as flags.
#
# --with-opus is not an override here, it is the only way to get Opus:
# aconfigure.ac sets enable_opus=no whenever ac_cross_compile is set
# and --with-opus was not given, so autodetection is off by
# construction. Without this flag the build succeeds and quietly has
# no Opus codec. --with-ssl is the same rule and the same one-line
# giveaway in the configure output: "Checking if SSL support is
# disabled... yes".
# Call transcripts. Built by its own script so it can be rebuilt
# without everything above.
"$ROOT/scripts/build-whisper-android.sh" "$ABI"

echo "=== pjproject"
cd "$ROOT/third_party/pjproject"
cp "$ROOT/scripts/config_site_debian.h" pjlib/include/pj/config_site.h
make distclean >/dev/null 2>&1 || true
ANDROID_NDK_ROOT="$NDK" TARGET_ABI="$ABI" APP_PLATFORM="android-$API" \
CFLAGS="$CFLAGS -I$PREFIX/include" \
LDFLAGS="${LDFLAGS:-} -L$PREFIX/lib" \
    ./configure-android --use-ndk-cflags --prefix="$PREFIX" \
        --disable-video --with-opus="$PREFIX" --with-ssl="$PREFIX"
make dep >/dev/null
sp_nice make -j"$(sp_jobs)" >/dev/null
make install >/dev/null

echo
echo "=== built into $PREFIX"
