#!/bin/sh
# e2e_net.sh - Prove guest networking: DNS + HTTPS out of the VM
#
# Boots a VM with slirp networking (ADR-023), brings up eth0 (static
# config in guest init), and runs an HTTPS request from inside the guest
# using the image's own dynamic wget (alpine busybox -- no static link
# needed, since the image ships its own libc + loader).
#
# A 401 from registry-1.docker.io/v2/ is a SUCCESS (unauthenticated
# /v2/ correctly returns 401); what matters is that DNS resolved, TCP
# connected, TLS handshook, and an HTTP round-trip came back. We hit an
# endpoint that returns 200 for a clean signal.
#
# Requires: podman + an alpine image (busybox wget). Skips if absent.
#
# Usage: ./e2e_net.sh
# Exit: 0 = DNS+HTTPS worked, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-net.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "E2E-NET FAIL: $1"
    [ -f "$WORK/serial.log" ] && tail -25 "$WORK/serial.log"
    exit 1
}

# Need an alpine image (provides busybox wget + its own libc)
IMG=$(podman images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null \
      | grep -i alpine | head -1)
if [ -z "$IMG" ]; then
    echo "E2E-NET SKIP: no alpine image (podman pull alpine:latest)"
    exit 0
fi

# Build guest init + initramfs (bundles virtio_net + 9p + vsock modules)
make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"

# HTTPS GET to an endpoint that returns 200. busybox wget exits non-zero
# on HTTP error but we only care that a 200 comes back cleanly.
URL="https://auth.docker.io/token?service=registry.docker.io"
timeout "$TIMEOUT_SECS" build/podvm -r "$WORK/initramfs.cpio" \
    "$IMG" /bin/busybox wget -q -O /dev/null "$URL" \
    > "$WORK/serial.log" 2>&1
RC=$?

grep -q "eth0 up 10.0.2.15" "$WORK/serial.log" \
    || fail "guest did not configure eth0"
grep -qiE "could not resolve|bad address|connection refused|network is unreachable" \
    "$WORK/serial.log" \
    && fail "network error in guest (dns/route/tls)"
[ "$RC" -eq 0 ] || fail "wget failed rc=$RC (expected 0 for HTTP 200)"

echo "E2E-NET PASS: $IMG resolved DNS + completed HTTPS from inside the VM"
exit 0
