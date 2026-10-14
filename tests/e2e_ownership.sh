#!/bin/sh
# e2e_ownership.sh - prove cross-uid chown works (ADR-028, path B)
#
# The single-uid userns map made cross-uid chown structurally impossible:
# the guest's chown becomes a 9p request the host executes inside a ns
# that only knows uid 0, so chown to any other uid is EINVAL. That breaks
# `RUN chown -R app:app /app` before `USER app` in ordinary Dockerfiles.
#
# Path B maps a subuid RANGE instead: ns 0 -> invoking user, ns 1..N ->
# the user's /etc/subuid allocation. Now the guest's chown(uid) lands on a
# real subuid-owned file on the host, and stat maps it back. This test runs
# a NAMED container (upper on the persistent host state dir) that chowns a
# file to uid 1000, then asserts the upper file on the HOST is owned by a
# uid in the subuid range -- not the invoking user (the old failure), and
# not root.
#
# Requires: podman + KVM + a local image + a subuid allocation. Skips if
# any is absent (a host without subuid keeps the single-uid map, which is a
# supported configuration, just without cross-uid chown).
#
# Usage: ./e2e_ownership.sh
# Exit: 0 = guest chown landed on a subuid-owned host file, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-own.XXXXXX)
trap '"$SCRIPT_DIR/rf" "$WORK" 2>/dev/null || rm -rf "$WORK"' EXIT

export XDG_STATE_HOME="$WORK/state"
NAME="podvm-own-e2e"

fail() {
    echo "E2E-OWNERSHIP FAIL: $1"
    [ -f "$WORK/run.log" ] && tail -20 "$WORK/run.log"
    exit 1
}

# --- skip conditions: podman, an image, and a subuid allocation ---
command -v podman > /dev/null 2>&1 || { echo "E2E-OWNERSHIP SKIP: no podman"; exit 0; }
IMG=$(podman images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null \
      | grep -iE "alpine|debian" | head -1)
[ -n "$IMG" ] || { echo "E2E-OWNERSHIP SKIP: no alpine/debian image"; exit 0; }
command -v newuidmap > /dev/null 2>&1 || { echo "E2E-OWNERSHIP SKIP: no newuidmap"; exit 0; }
SUBUID_LINE=$(grep -E "^$(id -un):" /etc/subuid 2>/dev/null || true)
[ -n "$SUBUID_LINE" ] || { echo "E2E-OWNERSHIP SKIP: no subuid allocation"; exit 0; }
SUBUID_BASE=$(echo "$SUBUID_LINE" | cut -d: -f2)

# Build guest init + initramfs
make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"

# Run a NAMED container that chowns a file to uid 1000:1000. The upper is
# the persistent host state dir, so the ownership lands on the real fs.
timeout "$TIMEOUT_SECS" build/podvm -r "$WORK/initramfs.cpio" -n "$NAME" \
    "$IMG" /bin/sh -c "echo owned > /own.txt && chown 1000:1000 /own.txt" \
    > "$WORK/run.log" 2>&1

UFILE="$XDG_STATE_HOME/podvm/containers/$NAME/u/own.txt"
[ -f "$UFILE" ] || fail "chowned file did not reach the host upper ($UFILE)"

# --- the actual assertion: the host-side owner must be a subuid ---
# `stat -c %u` gives the raw numeric uid the kernel stored.
HOST_UID=$(stat -c %u "$UFILE")
HOST_GID=$(stat -c %g "$UFILE")

# It must be inside the subuid range [base, base+count). ns-uid 1000 maps
# to base+999 (ns 0 -> invoking user, ns 1.. -> base..), but we accept any
# uid in the range rather than assume the exact offset.
SUBUID_COUNT=$(echo "$SUBUID_LINE" | cut -d: -f3)
SUBUID_END=$((SUBUID_BASE + SUBUID_COUNT))

if [ "$HOST_UID" -lt "$SUBUID_BASE" ] || [ "$HOST_UID" -ge "$SUBUID_END" ]; then
    fail "chown did NOT use the subuid range: host uid=$HOST_UID (invoking=$(id -u), subuid range $SUBUID_BASE..$SUBUID_END). Guest chown was not translated."
fi
if [ "$HOST_GID" -lt "$SUBUID_BASE" ] || [ "$HOST_GID" -ge "$SUBUID_END" ]; then
    fail "chown gid did NOT use the subgid range: host gid=$HOST_GID"
fi
# Sanity: not the invoking user (the pre-B failure mode) and not root.
if [ "$HOST_UID" = "$(id -u)" ]; then
    fail "host uid is still the invoking user ($HOST_UID) -- single-uid map, B not in effect"
fi

echo "E2E-OWNERSHIP PASS: guest chown 1000 -> host uid $HOST_UID (subuid range $SUBUID_BASE..$SUBUID_END)"
exit 0
