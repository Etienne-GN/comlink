#!/usr/bin/env bash
# The package, installed where nothing else is: a clean Debian 13,
# with apt left to resolve every dependency, and then the window and
# each secondary window opened with QML warnings made fatal.
#
# 0.9.0-1 passed every test and would not start on a colleague's
# machine: the QML modules the window loads at run time were not
# dependencies, and the build machine had them anyway through the Qt
# development packages. Only an install on a machine that has nothing
# shows that, so this is that machine.
set -uo pipefail
DEBDIR="${1:?dir containing the .deb}"
IMAGE="${COMLINK_BUILD_IMAGE:-docker.io/library/debian:13}"
ENGINE="${COMLINK_CONTAINER_ENGINE:-podman}"

deb="$(ls "$DEBDIR"/comlink_[0-9]*.deb 2>/dev/null | head -1)"
[ -n "$deb" ] || { echo "no comlink_*.deb in $DEBDIR" >&2; exit 1; }
command -v "$ENGINE" >/dev/null 2>&1 || {
    echo "$ENGINE not found; set COMLINK_CONTAINER_ENGINE" >&2
    exit 1
}

"$ENGINE" run --rm \
    --cpus "${COMLINK_BUILD_CPUS:-3}" \
    -v "$(cd "$DEBDIR" && pwd):/deb:ro" \
    -e DEB="/deb/$(basename "$deb")" \
    "$IMAGE" \
    bash -uo pipefail -c '
        fail=0
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq >/dev/null
        if apt-get install -y -qq "$DEB" >/tmp/install.log 2>&1; then
            echo "  ok installs with apt resolving its dependencies"
        else
            echo "  FAIL install"; tail -5 /tmp/install.log; exit 1
        fi

        export QT_QPA_PLATFORM=offscreen LC_ALL=C.UTF-8 HOME=/root
        export XDG_CONFIG_HOME=/root/.config COMLINK_QUICK_STRICT=1

        if comlink-cli --version >/dev/null 2>&1; then
            echo "  ok comlink-cli runs"
        else
            echo "  FAIL comlink-cli --version"; fail=1
        fi

        shot() {
            local name="$1"; shift
            if env "$@" COMLINK_QUICK_SHOT=/tmp/shot.png \
                    timeout 30 comlink >/tmp/run.log 2>&1 \
                    && [ -s /tmp/shot.png ]; then
                echo "  ok $name opens"
            else
                echo "  FAIL $name"; grep -v "^PROBE" /tmp/run.log | head -5
                fail=1
            fi
            rm -f /tmp/shot.png
        }
        shot "the window"
        for d in settings accounts logs quality about headers; do
            shot "$d" COMLINK_QUICK_DIALOG=1 COMLINK_QUICK_OVERLAY="$d"
        done
        exit $fail
    '
