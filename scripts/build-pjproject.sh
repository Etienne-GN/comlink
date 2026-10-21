#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PJ="$ROOT/third_party/pjproject"
PREFIX="$ROOT/third_party/prefix-debian"

[ -f "$PJ/configure" ] || { echo "pjproject submodule not checked out" >&2; exit 1; }

cp "$ROOT/scripts/config_site_debian.h" "$PJ/pjlib/include/pj/config_site.h"

cd "$PJ"
# -fPIC is mandatory, not an optimisation: pjproject installs static
# archives, and both the Debian shared library and the Android JNI .so
# link them into a shared object. Without it the link fails with
# "relocation against ... in read-only section".
export CFLAGS="${CFLAGS:-} -fPIC"
export CXXFLAGS="${CXXFLAGS:-} -fPIC"
export LDFLAGS="${LDFLAGS:-}"

# SSL, Opus and libsrtp are autodetected and on by default; --disable-*
# would be needed to exclude them. Video is explicitly off.
./configure --prefix="$PREFIX" --disable-video "$@"
make dep
make -j"$(nproc)"
make install

echo "pjproject installed to $PREFIX"
