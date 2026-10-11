#!/bin/sh
# e2e_images.sh - podvm images / rmi against the real cache
#
# The cache (default /tmp/podvm/images) holds native pulls AND podvm
# build output; run/images/rmi all see one cache. This proves the
# listing, removal, and the in-use guard: a run serving a cached
# rootfs holds a shared flock, so rmi cannot delete the image out from
# under a live VM.
#
# This test removes alpine from the cache ON PURPOSE -- it must be the
# last e2e that touches the native cache.
#
# Requires: podman + KVM + network for the native pull. Skips if absent.
#
# Usage: ./e2e_images.sh
# Exit: 0 = pass, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-images.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

export XDG_STATE_HOME="$WORK/state"
PODVM=build/podvm
IMGREF="docker.io/library/alpine:latest"

fail() {
    echo "E2E-IMAGES FAIL: $1"
    [ -f "$WORK/last.log" ] && tail -20 "$WORK/last.log"
    exit 1
}

command -v podman > /dev/null 2>&1 || { echo "E2E-IMAGES SKIP: no podman"; exit 0; }
[ -e /dev/kvm ] || { echo "E2E-IMAGES SKIP: no /dev/kvm"; exit 0; }

make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"

# Isolate the native layered store so the test is hermetic (and does not
# touch the user's real store).
export PODVM_STORE_DIR="$WORK/store"

# Warm the native cache through podvm's own pull path (skipped when the
# cache already has it).
PODVM_RESOLVE=native timeout "$TIMEOUT_SECS" "$PODVM" -r "$WORK/initramfs.cpio" \
    "$IMGREF" /bin/echo warmed > "$WORK/warm.log" 2>&1 \
    || { cp "$WORK/warm.log" "$WORK/last.log"; fail "cannot warm the native cache"; }

# ---- listing ----

"$PODVM" images > "$WORK/i1.log" 2>&1 || fail "images exited non-zero"
grep -Eq '^alpine +latest' "$WORK/i1.log" || fail "alpine:latest not listed (normalization?)"

# ---- the in-use guard ----

# A live native run holds the shared image lock for the VM's life.
setsid env PODVM_RESOLVE=native timeout 60 "$PODVM" -r "$WORK/initramfs.cpio" \
    "$IMGREF" /bin/sh -c 'sleep 30' > "$WORK/live.log" 2>&1 &
LPID=$!
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    grep -q "exec /bin/sh" "$WORK/live.log" 2>/dev/null && break
    sleep 1
done
grep -q "exec /bin/sh" "$WORK/live.log" 2>/dev/null \
    || { cp "$WORK/live.log" "$WORK/last.log"; fail "the live run never started"; }

"$PODVM" rmi alpine > "$WORK/rmi1.log" 2>&1 \
    && fail "rmi of an in-use image succeeded"
grep -q "in use" "$WORK/rmi1.log" || fail "rmi did not diagnose the in-use image"

# ---- removal after the VM is gone ----

kill -TERM -- "-$LPID" 2>/dev/null
wait "$LPID" 2>/dev/null
GOT=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    if "$PODVM" rmi alpine > "$WORK/rmi2.log" 2>&1; then GOT=1; break; fi
    sleep 1
done
[ -n "$GOT" ] || { cp "$WORK/rmi2.log" "$WORK/last.log"; fail "rmi after the VM stopped failed"; }

"$PODVM" images > "$WORK/i2.log" 2>&1
if grep -q "alpine" "$WORK/i2.log"; then
    fail "images still lists a removed image"
fi
"$PODVM" rmi alpine > "$WORK/rmi3.log" 2>&1 && fail "rmi of a removed image succeeded"
grep -q "no such image" "$WORK/rmi3.log" || fail "missing-image error not reported"

echo "E2E-IMAGES PASS: images lists, rmi removes, in-use images are refused"
exit 0
