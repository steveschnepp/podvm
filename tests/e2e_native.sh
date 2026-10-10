#!/bin/sh
# e2e_native.sh - Prove podvm's own registry pull end to end
#
# The native OCI client (src/image.c) was fixed and gained digest
# verification, but the test suite only ever exercised the podman-storage
# path -- because that's checked first, so the native pull was unreachable
# whenever podman had the image. This forces it via PODVM_RESOLVE=native,
# clears the cache so a real pull happens, and boots the result.
#
# Proves the whole native pipeline against a live registry: manifest fetch
# -> bearer auth -> platform manifest -> blob download -> sha256 digest
# verification -> gunzip -> untar -> 9p serve -> guest mount -> chroot ->
# exec. Also checks the image's own Cmd (from the config blob) runs with
# no command typed.
#
# Requires: KVM + network. Skips cleanly if the registry is unreachable.
#
# Usage: ./e2e_native.sh
# Exit: 0 = native pull + boot worked, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_NATIVE_TIMEOUT:-180}"
WORK=$(mktemp -d /tmp/podvm-e2e-native.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

# Small image, and one the native client resolves as docker.io/library/.
IMG="${PODVM_E2E_NATIVE_IMG:-busybox:latest}"

fail() {
    echo "E2E-NATIVE FAIL: $1"
    [ -f "$WORK/serial.log" ] && tail -25 "$WORK/serial.log"
    exit 1
}

# Skip if the registry is not reachable (offline host).
if ! curl -sS -o /dev/null -m 10 https://registry-1.docker.io/v2/ 2>/dev/null; then
    echo "E2E-NATIVE SKIP: registry unreachable (offline?)"
    exit 0
fi

# Build guest init + initramfs
make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"

# Clear podvm's native cache for this image so the run does a real pull.
# The cache layout is FLAT since image 87a370a: repos are mangled with
# '+' and tags are subdirs, so busybox:latest lives at
# <cache>/docker.io/library+busybox/latest. Clearing the old nested
# path is a no-op, and the run then resolves from cache -- no pull, and
# busybox's own default Cmd (sh) sits at a prompt until the timeout.
rm -rf /tmp/podvm/images/docker.io/library+busybox

# Run with the native backend forced. No command given: the image's own
# Cmd must be used, which also proves the config blob was cached.
PODVM_RESOLVE=native timeout "$TIMEOUT_SECS" \
    build/podvm -r "$WORK/initramfs.cpio" "$IMG" > "$WORK/serial.log" 2>&1
RC=$?

grep -q "podvm: using podman storage" "$WORK/serial.log" \
    && fail "native pull not engaged (fell through to podman storage)"

grep -qE "podvm: image .* ready at /tmp/podvm/images" "$WORK/serial.log" \
    || fail "native pull did not complete (rc=$RC)"
grep -qiE "digest mismatch|manifest fetch failed|gunzip failed|untar failed|failed to resolve" \
    "$WORK/serial.log" \
    && fail "native pull reported an error (rc=$RC)"

# The image ran in the VM. busybox with no args prints usage and exits
# non-zero, but it must at least have exec'd -- look for the init trace.
grep -q "podvm-init: exec" "$WORK/serial.log" \
    || fail "image did not exec in the guest (rc=$RC)"
grep -qiE "exec failed|Kernel panic" "$WORK/serial.log" \
    && fail "guest failed to exec the image's default command (rc=$RC)"

echo "E2E-NATIVE PASS: $IMG pulled via native client, digest-verified, booted"
exit 0
