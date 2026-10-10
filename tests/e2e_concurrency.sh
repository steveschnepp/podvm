#!/bin/sh
# e2e_concurrency.sh - two overlapping runs get distinct vsock resources
#
# Before per-VM allocation the guest CID (3) and the exit-report port
# (5001) were hardcoded globals: a second concurrent run could not get a
# vhost-vsock device (kernel rejects a duplicate guest-cid) and its exit
# helper could not bind the port. This proves both are now allocated per
# VM. Run A stays up in the background while run B boots AND reports its
# exit code -- which needs B to hold a different CID (else its VM fails
# to start) and a different exit port (else its code is lost and podvm
# returns QEMU's 0 instead of 42).
#
# Requires: podman + KVM + vsock. Skips if absent.
#
# Usage: ./e2e_concurrency.sh
# Exit: 0 = pass, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-conc.XXXXXX)
APID=""
# Kill the background run A on ANY exit (success, fail, or interrupt),
# or a failed assertion would leak a VM that still holds a CID+port.
cleanup() {
    [ -n "$APID" ] && kill -TERM -- "-$APID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

export XDG_STATE_HOME="$WORK/state"
CONTAINERS="$XDG_STATE_HOME/podvm/containers"
PODVM=build/podvm
NAME_A="podvm-conc-e2e-a"
NAME_B="podvm-conc-e2e-b"

fail() {
    echo "E2E-CONC FAIL: $1"
    [ -f "$WORK/last.log" ] && tail -20 "$WORK/last.log"
    exit 1
}

command -v podman > /dev/null 2>&1 || { echo "E2E-CONC SKIP: no podman"; exit 0; }
[ -e /dev/kvm ] || { echo "E2E-CONC SKIP: no /dev/kvm"; exit 0; }
[ -e /dev/vhost-vsock ] || { echo "E2E-CONC SKIP: no /dev/vhost-vsock"; exit 0; }
IMG=$(podman images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null \
      | grep -iE "alpine|debian" | head -1)
[ -n "$IMG" ] || { echo "E2E-CONC SKIP: no alpine/debian image"; exit 0; }

make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"
IR="$WORK/initramfs.cpio"

# The vsock CID the run reported: start_qemu prints "vsock: yes (cid N)".
cid_of() {
    sed -n 's/.*vsock:.*cid \([0-9][0-9]*\).*/\1/p' "$1" | head -1
}

# ---- run A stays up in the background (holds a CID + an exit port) ----

setsid timeout 90 "$PODVM" -r "$IR" -n "$NAME_A" \
    "$IMG" /bin/sh -c 'sleep 45' > "$WORK/a.log" 2>&1 &
APID=$!

# Wait for A to be holding its resources: its writable layer appears
# once the QEMU child has mounted the overlay (CID + port are claimed
# even earlier, at startup).
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    [ -d "$CONTAINERS/$NAME_A/u" ] && break
    sleep 1
done
[ -d "$CONTAINERS/$NAME_A/u" ] || { cp "$WORK/a.log" "$WORK/last.log"; \
    fail "run A never came up"; }
# The host banner (with A's CID) is flushed before QEMU starts, so it is
# in the log by the time the state dir appears; retry briefly regardless.
A_CID=""
for _ in 1 2 3 4 5; do
    A_CID=$(cid_of "$WORK/a.log")
    [ -n "$A_CID" ] && break
    sleep 1
done
[ -n "$A_CID" ] || { cp "$WORK/a.log" "$WORK/last.log"; \
    fail "run A did not report a vsock cid"; }

# ---- run B, while A is live: must boot AND report its own exit code ----

timeout "$TIMEOUT_SECS" "$PODVM" -r "$IR" -n "$NAME_B" \
    "$IMG" /bin/sh -c 'exit 42' > "$WORK/b.log" 2>&1
RC=$?
[ "$RC" -eq 42 ] || { cp "$WORK/b.log" "$WORK/last.log"; \
    fail "concurrent run B returned $RC, expected its own exit code 42"; }

B_CID=$(cid_of "$WORK/b.log")
[ -n "$B_CID" ] || { cp "$WORK/b.log" "$WORK/last.log"; \
    fail "run B did not report a vsock cid"; }
[ "$B_CID" != "$A_CID" ] || fail "runs A and B share vsock cid $A_CID"

# ---- cleanup: stop A's whole process group, drop both states ----

kill -TERM -- "-$APID" 2>/dev/null
wait "$APID" 2>/dev/null
APID=""   # already reaped; the trap need not kill it again
"$PODVM" rm "$NAME_A" > /dev/null 2>&1
"$PODVM" rm "$NAME_B" > /dev/null 2>&1

echo "E2E-CONC PASS: overlapping runs got distinct CIDs ($A_CID,$B_CID); B reported exit 42"
exit 0
