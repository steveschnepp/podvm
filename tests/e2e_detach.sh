#!/bin/sh
# e2e_detach.sh - podvm -d / stop / logs: the detached lifecycle
#
# A named detached run must: return immediately, keep its locks (via
# the VM), write state, serve its serial through `podvm logs`, refuse a
# second run of the same name, and stop cleanly via `podvm stop` --
# after which rm works and the state persists (no --rm involved).
#
# Requires: podman + KVM. Skips if absent.
#
# Usage: ./e2e_detach.sh
# Exit: 0 = pass, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-detach.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

export XDG_STATE_HOME="$WORK/state"
CONTAINERS="$XDG_STATE_HOME/podvm/containers"
PODVM=build/podvm
NAME="podvm-detach-e2e"

fail() {
    echo "E2E-DETACH FAIL: $1"
    [ -f "$WORK/last.log" ] && tail -20 "$WORK/last.log"
    exit 1
}

command -v podman > /dev/null 2>&1 || { echo "E2E-DETACH SKIP: no podman"; exit 0; }
[ -e /dev/kvm ] || { echo "E2E-DETACH SKIP: no /dev/kvm"; exit 0; }
IMG=$(podman images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null \
      | grep -iE "alpine|debian" | head -1)
[ -n "$IMG" ] || { echo "E2E-DETACH SKIP: no alpine/debian image"; exit 0; }

make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"
IR="$WORK/initramfs.cpio"

# ---- -d returns immediately and the VM keeps running ----

"$PODVM" -d -r "$IR" -n "$NAME" "$IMG" \
    /bin/sh -c 'echo detached-marker > /d.txt; sleep 30' > "$WORK/d1.log" 2>&1 \
    || { cp "$WORK/d1.log" "$WORK/last.log"; fail "--detach run failed"; }
grep -q "running in the background" "$WORK/d1.log" || fail "no detach confirmation"
[ -f "$CONTAINERS/$NAME/pid" ] || fail "no pidfile"
PID=$(cat "$CONTAINERS/$NAME/pid")
kill -0 "$PID" 2>/dev/null || fail "the VM pid is not alive"

# The guest actually ran: the marker appears in the persistent upper.
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    [ -f "$CONTAINERS/$NAME/u/d.txt" ] && break
    sleep 1
done
grep -q "detached-marker" "$CONTAINERS/$NAME/u/d.txt" 2>/dev/null \
    || { cp "$WORK/d1.log" "$WORK/last.log"; fail "the detached guest never ran"; }

# ---- logs ----

"$PODVM" logs "$NAME" > "$WORK/l1.log" 2>&1 \
    || { cp "$WORK/l1.log" "$WORK/last.log"; fail "logs exited non-zero"; }
grep -q "podvm-init: exec /bin/sh" "$WORK/l1.log" \
    || fail "logs does not show the guest's init output"

# ---- the live-name guard holds across detach ----

"$PODVM" -r "$IR" -n "$NAME" "$IMG" /bin/echo second > "$WORK/d2.log" 2>&1 \
    && fail "a second run of a live detached name was allowed"
grep -q "already running" "$WORK/d2.log" || fail "no already-running diagnosis"

# ---- stop ----

"$PODVM" stop "$NAME" > "$WORK/s1.log" 2>&1 \
    || { cp "$WORK/s1.log" "$WORK/last.log"; fail "stop failed"; }
grep -q "stopped $NAME" "$WORK/s1.log" || fail "stop did not report success"
kill -0 "$PID" 2>/dev/null && fail "the VM pid is still alive after stop"
[ ! -f "$CONTAINERS/$NAME/pid" ] || fail "pidfile survived stop"

# State persists after stop (no --rm): the marker is still on the host.
grep -q "detached-marker" "$CONTAINERS/$NAME/u/d.txt" 2>/dev/null \
    || fail "state did not persist after stop"

# Stopping again: not running.
"$PODVM" stop "$NAME" > "$WORK/s2.log" 2>&1 && fail "stopping twice succeeded"
grep -q "not running" "$WORK/s2.log" || fail "second stop did not diagnose"

# ---- rm works once stopped; validations hold ----

"$PODVM" rm "$NAME" > "$WORK/rm1.log" 2>&1 \
    || { cp "$WORK/rm1.log" "$WORK/last.log"; fail "rm after stop failed"; }
[ ! -e "$CONTAINERS/$NAME" ] || fail "rm left state behind"

"$PODVM" logs "$NAME" > /dev/null 2>&1 && fail "logs of a removed container succeeded"

echo "E2E-DETACH PASS: -d hands the VM over; logs/stop/rm close the lifecycle"
exit 0
