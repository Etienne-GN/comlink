#!/usr/bin/env bash
# Reproduce the intermittent teardown deadlock under concurrent load,
# and capture a backtrace from the wedged process rather than guessing.
#
# Usage: deadlock-hunt.sh <build-dir> [iterations] [timeout-seconds]
set -uo pipefail
BUILD="${1:?build dir}"
ITERS="${2:-40}"
BUDGET="${3:-45}"
OUT="$BUILD/stress"
mkdir -p "$OUT"
rm -f "$OUT"/wedged-*.txt

cat > "$OUT/noise.yaml" <<'YAML'
log_level: 0
sip_port: 0
audio_device: none
accounts: []
YAML

# Snapshot a wedged process and its threads, then let it die.
snapshot() {
    local pid="$1" tag="$2" f="$OUT/wedged-$tag-$pid.txt"
    {
        echo "=== $tag pid=$pid $(date -Is) ==="
        echo "--- threads: $(ls /proc/$pid/task 2>/dev/null | wc -l) ---"
        for t in /proc/$pid/task/*; do
            echo "tid $(basename "$t") wchan=$(cat "$t/wchan" 2>/dev/null)"
        done
        echo "--- gdb backtraces ---"
        timeout 60 gdb -p "$pid" -batch \
            -ex 'set pagination off' \
            -ex 'thread apply all bt' 2>&1
    } > "$f"
    echo "  WEDGED: captured $f"
}

hits=0
for i in $(seq 1 "$ITERS"); do
    # Background noise: interactive-style instances starting and stopping,
    # which is the condition the original wedge appeared under.
    for n in 1 2 3; do
        ( sleep 0.$((RANDOM % 9)) ;
          printf 'codecs\nrules\nquit\n' | \
            timeout 20 "$BUILD/frontend/cli/comlink-cli" \
              --config "$OUT/noise.yaml" --no-audio ) > /dev/null 2>&1 &
    done

    # The tests that exercise create/start/stop/destroy hardest, run
    # concurrently with each other.
    pids=()
    for t in test_lifecycle test_account test_call; do
        SP_TEST_TMP="$OUT" "$BUILD/core/$t" > "$OUT/$t.log" 2>&1 &
        pids+=("$!:$t")
    done

    # Watchdog on each test process.
    deadline=$((SECONDS + BUDGET))
    while :; do
        alive=0
        for entry in "${pids[@]}"; do
            pid="${entry%%:*}"; name="${entry##*:}"
            if kill -0 "$pid" 2>/dev/null; then
                alive=1
                if [ "$SECONDS" -ge "$deadline" ]; then
                    snapshot "$pid" "$name"
                    hits=$((hits + 1))
                    kill -9 "$pid" 2>/dev/null
                fi
            fi
        done
        [ "$alive" -eq 0 ] && break
        [ "$SECONDS" -ge "$((deadline + 5))" ] && break
        sleep 0.5
    done
    wait 2>/dev/null

    printf 'iter %2d/%s  wedges so far: %d\n' "$i" "$ITERS" "$hits"
    [ "$hits" -gt 0 ] && break
done

echo
echo "total wedges: $hits"
ls -1 "$OUT"/wedged-*.txt 2>/dev/null || echo "(none captured)"
