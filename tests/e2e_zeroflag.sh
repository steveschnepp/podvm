#!/bin/sh
# e2e_zeroflag.sh - The canonical docker invocation, with zero config flags
#
# The user's smoke test that found TWO compat bugs in one command:
#   `podvm run --rm hello-world` -- the `run` keyword was treated as an
#   image name, and after that was fixed it still needed `-r out/initramfs.cpio`
#   because the compiled-in default only exists after `make install`.
#
# Both bugs lived on the path NO e2e covered: every other test passes
# `-r "$WORK/initramfs.cpio"` explicitly, so the default-resolution path
# (the one a new user hits first) was never executed in CI.
#
# This test exercises it hermetically: it copies the podvm binary into a
# temp dir NEXT TO a freshly built initramfs.cpio and runs it with the
# docker spelling (`run --rm`) and NO -r, NO env. resolve_initramfs()
# must find the artifact via /proc/self/exe (next-to-binary), which is
# the dev-tree loop `make initramfs` sets up.
#
# Requires: KVM. Uses a static rootfs directory as the "image" (no podman,
# no network), so it runs on any KVM host.
#
# Usage: ./e2e_zeroflag.sh
# Exit: 0 = ran with no -r and the guest exec'd, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-zeroflag.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "E2E-ZEROFLAG FAIL: $1"
    [ -f "$WORK/serial.log" ] && tail -25 "$WORK/serial.log"
    exit 1
}

# --- 1. Build guest init + initramfs ---
make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs build"

# --- 2. Stage an isolated "installed-looking" dir: binary + initramfs side by
#        side. Running the COPIED binary makes /proc/self/exe resolve next to
#        the temp initramfs, exercising next-to-binary lookup without touching
#        the real build/initramfs.cpio. ---
mkdir -p "$WORK/stage" || fail "stage dir"
cp build/podvm "$WORK/stage/podvm" || fail "copy podvm binary"
cp "$WORK/initramfs.cpio" "$WORK/stage/initramfs.cpio" || fail "stage initramfs"

# --- 3. Build a tiny static rootfs to run (the "image" argument) ---
mkdir -p "$WORK/rootfs/bin" "$WORK/rootfs/tmp" || fail "rootfs dirs"
cat > "$WORK/rootfs/hello.c" <<'EOF'
#include <stdio.h>
int main(void) {
    puts("E2E-ZEROFLAG-OK");
    return 0;
}
EOF
gcc -static -Os -o "$WORK/rootfs/bin/hello" "$WORK/rootfs/hello.c" \
    || fail "static entrypoint build"

# --- 4. Run with the DOCKER spelling and ZERO config flags: `run --rm`.
#        No -r, no env, no kernel -- every default must resolve on its own. ---
"$WORK/stage/podvm" run --rm "$WORK/rootfs" /bin/hello \
    > "$WORK/serial.log" 2>&1
RC=$?

# --- 5. Verify the run actually happened AND used the default resolution ---
grep -q "E2E-ZEROFLAG-OK" "$WORK/serial.log" \
    || fail "entrypoint output not seen (rc=$RC)"
grep -q "exec /bin/hello" "$WORK/serial.log" \
    || fail "guest init never reached exec"

# The initramfs in use must be the one NEXT TO the binary, not the
# compiled-in /var/lib/podvm default (absent in CI) -- proof the
# next-to-binary resolution ran. The launch banner prints the resolved path.
grep -q "initramfs: $WORK/stage/initramfs.cpio" "$WORK/serial.log" \
    || fail "initramfs did not resolve next-to-binary (got: $(grep initramfs "$WORK/serial.log" | head -1))"

# `--rm` parsed cleanly (no unknown-option / no leftover state). If the
# keyword or flag had misparsed, the image ref would have been wrong and
# we would never have reached exec -- but assert the parse stayed clean.
grep -qiE "unknown option|invalid argument|manifest fetch failed" "$WORK/serial.log" \
    && fail "podvm misparsed the docker spelling (rc=$RC)"

echo "E2E-ZEROFLAG PASS: 'run --rm' with no -r resolved next-to-binary and ran"
exit 0
