#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

[ -d third_party/prefix-debian ] || {
    echo "pjproject not built; run scripts/build-pjproject.sh first" >&2
    exit 1
}

rm -rf debian
cp -r packaging/debian debian
trap 'rm -rf "$ROOT/debian"' EXIT

dpkg-buildpackage -us -uc -b

mkdir -p build/deb
mv ../comlink*_*.deb build/deb/ 2>/dev/null || true
rm -f ../comlink*_*.buildinfo ../comlink*_*.changes
ls -1 build/deb/
