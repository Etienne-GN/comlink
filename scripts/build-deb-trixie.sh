#!/usr/bin/env bash
set -euo pipefail

# A package's dependencies are whatever dpkg-shlibdeps finds linked at
# build time. Built on forky/sid, comlink demands glibc 2.43 and Qt
# 6.10, which Debian 13 cannot satisfy, and apt refuses it outright.
# The build therefore has to happen on the release being targeted
# rather than on whatever the developer happens to run.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${COMLINK_BUILD_IMAGE:-docker.io/library/debian:13}"
ENGINE="${COMLINK_CONTAINER_ENGINE:-podman}"
OUT="$ROOT/build/deb-trixie"

command -v "$ENGINE" >/dev/null 2>&1 || {
    echo "$ENGINE not found; set COMLINK_CONTAINER_ENGINE" >&2
    exit 1
}

rm -rf "$OUT"
mkdir -p "$OUT"

# The source is mounted read-only and copied inside: pjproject installs
# a prefix into the tree and dpkg-buildpackage writes to it, and none of
# that belongs in the developer's checkout. .git comes along because
# CMake reads the commit from it for --version.
# Three cores, like every other long build here: this is the
# operator's working machine and a container that takes all of them
# makes it unusable for as long as it runs.
"$ENGINE" run --rm \
    --cpus "${COMLINK_BUILD_CPUS:-3}" \
    -v "$ROOT:/src:ro" \
    -v "$OUT:/out" \
    "$IMAGE" \
    bash -euo pipefail -c '
        export DEBIAN_FRONTEND=noninteractive
        # whisper.cpp is not in Debian 13: build without transcripts.
        # Both apt-get build-dep and dpkg-buildpackage read this.
        export DEB_BUILD_PROFILES=pkg.comlink.nowhisper
        apt-get update -qq
        apt-get install -y --no-install-recommends \
            build-essential dpkg-dev git ca-certificates

        mkdir -p /work
        # The Android trees are left out with the desktop ones: Qt
        # for Android sources and builds run to gigabytes, and a Debian
        # package uses none of them.
        tar -C /src -cf - \
            --exclude=./build \
            --exclude=./build-rel \
            --exclude=./obj-x86_64-linux-gnu \
            --exclude=./third_party/prefix-debian \
            --exclude="./third_party/prefix-android-*" \
            --exclude="./third_party/prefix-qt-android-*" \
            --exclude=./third_party/qt-android-src \
            --exclude="./build-android-*" \
            --exclude="./build-quick-*" \
            --exclude=./run \
            --exclude=./.superpowers \
            . | tar -C /work -xf -

        cd /work

        # The build dependencies come from debian/control, not from a
        # list kept here: a second copy drifts, and the drift only
        # shows up as a build that works on the machine that has the
        # missing package installed anyway.
        cp -r packaging/debian debian
        apt-get build-dep -y ./
        rm -rf debian

        scripts/build-pjproject.sh
        scripts/build-deb.sh
        cp build/deb/*.deb /out/
    '

echo
echo "built in $IMAGE:"
for deb in "$OUT"/*.deb; do
    echo "  $(basename "$deb")"
done
echo
echo "dependencies of the installable package:"
dpkg-deb -f "$OUT"/comlink_[0-9]*.deb Depends | tr ',' '\n' | sed 's/^ */    /'

# A package that builds and passes its tests can still fail to start on
# a machine that has none of what this one has; 0.9.0-1 did. It is not
# a package until it has been installed somewhere clean and opened.
echo
echo "installing it on a clean $IMAGE:"
"$ROOT/tests/packaging/test_deb_install.sh" "$OUT"
