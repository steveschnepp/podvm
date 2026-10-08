#!/bin/sh
# e2e_boot.sh - End-to-end VM boot test over 9p
#
# Boots a real VM (host kernel + built initramfs), mounts a tiny test
# rootfs over 9p, chroots into it, and execs a static binary. Proves:
#   - QEMU 9p fsdev -> guest 9p mount (incl. bundled module loading)
#   - vsock config handshake (when /dev/vhost-vsock is present)
#   - guest init flow: pseudofs -> cmdline -> vsock -> mount -> chroot -> exec
#   - panic=-1 lifecycle: VM exits when the entrypoint exits
#
# Usage: ./e2e_boot.sh
# Exit: 0 = booted and entrypoint output seen, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "E2E FAIL: $1"
    [ -f "$WORK/serial.log" ] && tail -20 "$WORK/serial.log"
    exit 1
}

# --- 1. Build guest init + initramfs (with bundled 9p modules) ---
make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs build"

# --- 2. Build a tiny static rootfs ---
# The entrypoint must be static: the guest has no shared libraries.
mkdir -p "$WORK/rootfs/bin" "$WORK/rootfs/tmp"
cat > "$WORK/rootfs/hello.c" <<'EOF'
#include <stdio.h>
int main(void) {
    puts("E2E-ROOTFS-OK");
    return 0;
}
EOF
gcc -static -Os -o "$WORK/rootfs/bin/hello" "$WORK/rootfs/hello.c" \
    || fail "static entrypoint build"

# --- 3. Run podvm ---
build/podvm -r "$WORK/initramfs.cpio" "$WORK/rootfs" /bin/hello \
    > "$WORK/serial.log" 2>&1 &
PODVM_PID=$!

# Bounded wait: podvm must exit on its own (panic=-1 + -no-reboot).
WAITED=0
while kill -0 "$PODVM_PID" 2>/dev/null; do
    sleep 1
    WAITED=$((WAITED + 1))
    if [ "$WAITED" -ge "$TIMEOUT_SECS" ]; then
        kill -9 "$PODVM_PID" 2>/dev/null
        fail "podvm did not exit within ${TIMEOUT_SECS}s (hang)"
    fi
done
wait "$PODVM_PID"
RC=$?

# --- 4. Verify ---
grep -q "E2E-ROOTFS-OK" "$WORK/serial.log" \
    || fail "entrypoint output not seen (rc=$RC)"
grep -q "exec /bin/hello" "$WORK/serial.log" \
    || fail "guest init never reached exec"

echo "E2E PASS: booted, mounted 9p rootfs, chrooted, exec'd entrypoint"
exit 0
