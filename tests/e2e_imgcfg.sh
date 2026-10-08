#!/bin/sh
# e2e_imgcfg.sh - Prove an image's own config runs with no CLI command
#
# Builds a throwaway image whose config carries a known default command
# (CMD /bin/env) and a known Env var, then boots it with NO command on the
# podvm command line. Asserts the guest execs the image's Cmd and that the
# image's Env reached the entrypoint -- i.e. the OCI promise that an image
# carries its own default command (ADR-005) now holds end to end.
#
# Uses a space-free Cmd: the podvm.*= config channel is space-delimited, so
# a Cmd containing spaces (e.g. `sh -c "..."`) cannot travel over it yet.
#
# Requires: podman + KVM. Skips if podman is absent.
#
# Usage: ./e2e_imgcfg.sh
# Exit: 0 = image config honored, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-imgcfg.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

IMG="podvm-e2e-cfg:test"

fail() {
    echo "E2E-IMGCFG FAIL: $1"
    [ -f "$WORK/serial.log" ] && tail -25 "$WORK/serial.log"
    exit 1
}

command -v podman > /dev/null 2>&1 || { echo "E2E-IMGCFG SKIP: no podman"; exit 0; }

# Build a throwaway image: Cmd=/usr/bin/env (space-free), Env, and
# WorkingDir. env with no args prints the whole environment, so the
# marker proves the image's Env reached the entrypoint.
cat > "$WORK/Dockerfile" <<'EOF'
FROM docker.io/library/alpine:latest
ENV PODVM_E2E_MARKER=ENV-MARKER-OK
WORKDIR /app
CMD ["/usr/bin/env"]
EOF
podman build -q -t "$IMG" "$WORK" > /dev/null 2>&1 \
    || { echo "E2E-IMGCFG SKIP: cannot build test image (no alpine?)"; exit 0; }

# Build guest init + initramfs
make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"

# Run with NO command: podvm must exec the image's own Cmd.
timeout "$TIMEOUT_SECS" build/podvm -r "$WORK/initramfs.cpio" \
    "$IMG" > "$WORK/serial.log" 2>&1
RC=$?

grep -q "PODVM_E2E_MARKER=ENV-MARKER-OK" "$WORK/serial.log" \
    || fail "image Env did not reach the entrypoint (rc=$RC)"
grep -qiE "no command given|exec failed|entrypoint not set" "$WORK/serial.log" \
    && fail "podvm did not use the image's default command (rc=$RC)"

echo "E2E-IMGCFG PASS: image Cmd + Env honored with no CLI command"
exit 0
