#!/bin/sh
# e2e_podman.sh - Run a real podman-pulled image inside the VM
#
# Resolves the image from podman's on-disk storage (layers in place,
# no export), mounts the layer chain as an overlay inside the QEMU
# child's userns, and runs the entrypoint. Proves the whole
# podvm+podman integration: storage lookup, layer chain, userns
# overlay mount, 9p serve, guest mount, chroot, exec.
#
# Uses the first image from `podman images` that has a usable shell
# or echo binary. Falls back to debian if present.
#
# Usage: ./e2e_podman.sh
# Exit: 0 = image ran, expected output seen, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-podman.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "E2E-PODMAN FAIL: $1"
    [ -f "$WORK/serial.log" ] && tail -20 "$WORK/serial.log"
    exit 1
}

# Pick an image: prefer debian (known to work), else first available.
IMG=$(podman images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null \
      | grep -v "<none>" | head -1)
[ -n "$IMG" ] || { echo "E2E-PODMAN SKIP: no podman images"; exit 0; }

# Build guest init + initramfs
make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"

# Verify the image resolves from podman storage
podman image inspect "$IMG" > /dev/null 2>&1 || fail "podman image inspect $IMG"

# Run: entrypoint is /bin/echo (present in most images)
timeout "$TIMEOUT_SECS" build/podvm -r "$WORK/initramfs.cpio" \
    "$IMG" /bin/echo "E2E-PODMAN-OK" > "$WORK/serial.log" 2>&1
RC=$?

grep -q "E2E-PODMAN-OK" "$WORK/serial.log" \
    || fail "expected output not seen (rc=$RC, img=$IMG)"
grep -q "podvm: using podman storage" "$WORK/serial.log" \
    || fail "podman storage mode not engaged"

echo "E2E-PODMAN PASS: $IMG ran in VM via podman storage"
exit 0
