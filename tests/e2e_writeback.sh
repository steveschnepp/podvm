#!/bin/sh
# e2e_writeback.sh - prove named-container overlay write-back
#
# The podman-storage path mounts the image as an overlay whose upper layer
# used to be a throwaway tmpfs, so anything the container wrote vanished
# when the VM exited. With --name the upper lives in a persistent host
# state dir, so writes survive the run (ADR-026).
#
# This runs a named container that writes a file, exits, then runs the
# SAME name again and asserts the file is still there -- proving the
# writable layer persisted across VM lifetimes. It also runs without a
# name to prove the default remains throwaway (writes discarded).
#
# Requires: podman + KVM + at least one local image. Skips if absent.
#
# Usage: ./e2e_writeback.sh
# Exit: 0 = writes persisted for a named container and were discarded
#          without a name, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-writeback.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

# A named container's state must be isolated to this test run.
export XDG_STATE_HOME="$WORK/state"
NAME="podvm-wb-e2e"
MARKER="podvm-wb-marker-$$"

fail() {
    echo "E2E-WRITEBACK FAIL: $1"
    [ -f "$WORK/r2.log" ] && tail -20 "$WORK/r2.log"
    exit 1
}

command -v podman > /dev/null 2>&1 || { echo "E2E-WRITEBACK SKIP: no podman"; exit 0; }
IMG=$(podman images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null \
      | grep -iE "alpine|debian" | head -1)
[ -n "$IMG" ] || { echo "E2E-WRITEBACK SKIP: no alpine/debian image"; exit 0; }

# Build guest init + initramfs
make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"

# Run 1 (named): write a marker file into the container rootfs.
timeout "$TIMEOUT_SECS" build/podvm -r "$WORK/initramfs.cpio" -n "$NAME" \
    "$IMG" /bin/sh -c "echo $MARKER > /wb.txt" > "$WORK/r1.log" 2>&1
grep -q "writable layer:" "$WORK/r1.log" \
    || fail "podvm did not report a persistent writable layer"

# The upper dir must already hold the file after the VM exited.
UFILE="$XDG_STATE_HOME/podvm/containers/$NAME/u/wb.txt"
[ -f "$UFILE" ] || fail "write did not persist to the host upper ($UFILE)"
grep -q "$MARKER" "$UFILE" || fail "persisted file has the wrong content"

# Run 2 (same name): the file must still be there.
timeout "$TIMEOUT_SECS" build/podvm -r "$WORK/initramfs.cpio" -n "$NAME" \
    "$IMG" /bin/cat /wb.txt > "$WORK/r2.log" 2>&1
grep -q "$MARKER" "$WORK/r2.log" \
    || fail "second run did not see the first run's write (no persistence)"

# Run 3 (no name): writes must be discarded (throwaway upper).
timeout "$TIMEOUT_SECS" build/podvm -r "$WORK/initramfs.cpio" \
    "$IMG" /bin/cat /wb.txt > "$WORK/r3.log" 2>&1
if grep -q "$MARKER" "$WORK/r3.log"; then
    fail "un-named run saw a named container's write (upper not isolated)"
fi

echo "E2E-WRITEBACK PASS: $IMG writes persisted for --name $NAME, discarded without"
exit 0
