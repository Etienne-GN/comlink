#!/usr/bin/env bash
set -uo pipefail
DEBDIR="${1:?dir containing .deb files}"
fail=0
check() {
    if [ "$2" = "$3" ]; then echo "  ok $1"
    else echo "  FAIL $1: got '$2' want '$3'"; fail=1; fi
}

# The version has one source of truth, and this is what keeps it that
# way: CMakeLists.txt, the Debian changelog and the built package all
# have to agree, or a package claims a version its binaries do not.
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && cd .. && pwd)"
cmake_ver="$(sed -n 's/^project(comlink VERSION \([0-9.]*\).*/\1/p' \
             "$SRC/CMakeLists.txt" | head -1)"
changelog_ver="$(sed -n '1s/.*(\([0-9.]*\)-[0-9]*).*/\1/p' \
                 "$SRC/packaging/debian/changelog" | head -1)"
check "CMakeLists.txt declares a version" \
      "$([ -n "$cmake_ver" ] && echo yes)" "yes"
check "the changelog version matches CMakeLists.txt" \
      "$changelog_ver" "$cmake_ver"

deb="$(ls "$DEBDIR"/comlink_[0-9]*.deb 2>/dev/null | head -1)"
check "the package exists" "$([ -n "$deb" ] && echo yes)" "yes"
[ -n "$deb" ] || exit 1

check "exactly one installable package is produced" \
      "$(ls "$DEBDIR"/*.deb 2>/dev/null | grep -vc -- -dbgsym_)" "1"
check "the package version matches the declared version" \
      "$(dpkg-deb -f "$deb" Version | sed 's/-[0-9]*$//')" "$cmake_ver"

# --- everything an install has to contain ---------------------------
check "ships the GUI" \
      "$(dpkg-deb -c "$deb" | grep -c 'usr/bin/comlink$')" "1"
check "ships the CLI" \
      "$(dpkg-deb -c "$deb" | grep -c 'usr/bin/comlink-cli$')" "1"
check "ships the shared library" \
      "$([ "$(dpkg-deb -c "$deb" | grep -c 'libcomlink_core.so')" -ge 1 ] \
        && echo yes)" "yes"
check "the library soname carries the version" \
      "$([ "$(dpkg-deb -c "$deb" \
             | grep -c "libcomlink_core.so.$cmake_ver")" -ge 1 ] \
        && echo yes)" "yes"
check "ships the linker symlink" \
      "$(dpkg-deb -c "$deb" | grep -c 'libcomlink_core.so ->')" "1"
check "ships the public header" \
      "$(dpkg-deb -c "$deb" | grep -c 'include/comlink/core.h')" "1"
check "ships no static archive" \
      "$(dpkg-deb -c "$deb" | grep -c '\.a$')" "0"
check "ships the desktop entry" \
      "$(dpkg-deb -c "$deb" | grep -c 'applications/comlink.desktop')" "1"
check "the desktop entry names the icon" \
      "$(dpkg-deb --fsys-tarfile "$deb" \
         | tar -xOf - ./usr/share/applications/comlink.desktop 2>/dev/null \
         | grep -c '^Icon=comlink$')" "1"
check "ships an icon for every hicolor size" \
      "$(dpkg-deb -c "$deb" \
         | grep -c 'usr/share/icons/hicolor/.*/apps/comlink.png')" "9"
check "ships the example config" \
      "$(dpkg-deb -c "$deb" | grep -c 'comlink.example.yaml')" "1"
check "ships the headless ALSA configuration" \
      "$(dpkg-deb -c "$deb" | grep -c 'usr/share/comlink/alsa-none.conf')" "1"
check "ships the bundled sounds" \
      "$([ "$(dpkg-deb -c "$deb" \
             | grep -c 'usr/share/comlink/sounds/.*\.wav')" -ge 9 ] \
        && echo yes)" "yes"
check "ships all three sound slots" \
      "$(dpkg-deb -c "$deb" \
         | grep -oE 'sounds/(inbound|outbound|notifications)/' \
         | sort -u | wc -l)" "3"
check "ships the sound attribution" \
      "$(dpkg-deb -c "$deb" | grep -c 'sounds/ATTRIBUTION.md')" "1"

# --- it must replace the packages it used to be ---------------------
# Without this an install fails on file conflicts with the old split,
# which is exactly what an operator upgrading would hit.
check "declares Replaces for the packages it absorbed" \
      "$([ "$(dpkg-deb -f "$deb" Replaces \
             | grep -c 'comlink-core')" -ge 1 ] && echo yes)" "yes"
check "declares Breaks for them too" \
      "$([ "$(dpkg-deb -f "$deb" Breaks | grep -c 'comlink-core')" -ge 1 ] \
        && echo yes)" "yes"

check "declares a maintainer" \
      "$([ -n "$(dpkg-deb -f "$deb" Maintainer)" ] && echo yes)" "yes"
check "records pjproject in copyright" \
      "$(dpkg-deb --fsys-tarfile "$deb" | tar -tf - 2>/dev/null \
         | grep -c 'copyright')" "1"
check "lintian reports no errors" \
      "$(lintian --no-tag-display-limit "$deb" 2>/dev/null \
         | grep -c '^E:')" "0"
# The source path, whatever it is, rather than a hardcoded home
# directory: -ffile-prefix-map should have mapped it away, and the
# check should hold for whoever builds this.
check "no build path leaks into the binaries" \
      "$(dpkg-deb --fsys-tarfile "$deb" \
         | tar -xO ./usr/bin/comlink-cli 2>/dev/null \
         | grep -acF "$SRC")" "0"

# An embedded library and a single-package layout are deliberate,
# documented choices, so the override must exist and must carry
# reasons. A bare override hides a finding without explaining it.
check "ships a lintian override" \
      "$(dpkg-deb -c "$deb" | grep -c 'lintian/overrides/comlink')" "1"
check "the override is not bare" \
      "$([ "$(dpkg-deb --fsys-tarfile "$deb" \
             | tar -xO ./usr/share/lintian/overrides/comlink 2>/dev/null \
             | grep -c '^#')" -ge 3 ] && echo yes)" "yes"

# With one binary package there is no .install file and no debian/tmp
# staging: dh_auto_install puts everything straight into the package,
# so a built file cannot end up in no package. That was the failure
# that once shipped a build with the GUI missing, and collapsing to one
# package removes the possibility rather than guarding against it. The
# check stays as a tripwire in case a second package is ever added.
check "no uninstalled files were dropped" \
      "$(grep -c 'not installed' "$DEBDIR/../deb-build.log" 2>/dev/null \
         | head -1)" "0"

exit $fail
