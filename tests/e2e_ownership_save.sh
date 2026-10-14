#!/bin/sh
# e2e_ownership_save.sh - ownership survives the COMMIT/SAVE path
#
# e2e_ownership.sh proves the RUN side (a guest chown lands on a subuid
# on the host). This proves the other half of the round-trip: a build
# step that chowns a file, then `podvm save`, must produce a layer tar
# carrying the CONTAINER uid (1000) -- not 0 (the old hdr_build hardcode)
# and not a raw subuid (an implementation detail that must never reach the
# portable format). That is what makes the image correct for podman interop
# and for a later `podvm load`.
#
# Requires: podman + KVM + a local base image + a subuid allocation. Skips
# if any is absent.
#
# Usage: ./e2e_ownership_save.sh
# Exit: 0 = saved layer tar carries container-uid 1000, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-ownsave.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

export XDG_STATE_HOME="$WORK/state"
# Hermetic store so the build's committed blobs live where we can find them.
export PODVM_STORE_DIR="$WORK/store"
PODVM=build/podvm
IMGREF="podvm-e2e-ownsave:test"

fail() {
    echo "E2E-OWNSAVE FAIL: $1"
    [ -f "$WORK/last.log" ] && tail -25 "$WORK/last.log"
    exit 1
}

command -v podman > /dev/null 2>&1 || { echo "E2E-OWNSAVE SKIP: no podman"; exit 0; }
[ -e /dev/kvm ] || { echo "E2E-OWNSAVE SKIP: no /dev/kvm"; exit 0; }
command -v newuidmap > /dev/null 2>&1 || { echo "E2E-OWNSAVE SKIP: no newuidmap"; exit 0; }
grep -qE "^$(id -un):" /etc/subuid 2>/dev/null || { echo "E2E-OWNSAVE SKIP: no subuid"; exit 0; }
BASE=$(podman images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null \
       | grep -iE "alpine|debian" | head -1)
[ -n "$BASE" ] || { echo "E2E-OWNSAVE SKIP: no alpine/debian base image"; exit 0; }

make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"
IR="$WORK/initramfs.cpio"

"$PODVM" rmi "$IMGREF" > /dev/null 2>&1 || true

# A RUN step that chowns a file to uid 1000 -- the ordinary Dockerfile
# idiom that ADR-028 exists to support. This runs in a real VM with the
# subuid-range userns, so the chown lands on a subuid on disk; the commit
# must translate it back to container-uid 1000 in the stored blob.
mkdir -p "$WORK/ctx"
cat > "$WORK/ctx/Dockerfile" <<EOF
FROM $BASE
RUN echo owned > /app.txt && chown 1000:1000 /app.txt
EOF

timeout $((TIMEOUT_SECS * 6)) "$PODVM" build -r "$IR" -t "$IMGREF" "$WORK/ctx" \
    > "$WORK/b.log" 2>&1 \
    || { cp "$WORK/b.log" "$WORK/last.log"; fail "build failed"; }
grep -q "podvm: built $IMGREF" "$WORK/b.log" || fail "build did not report the image"

# Save to a docker-archive and inspect the layer tar's ownership.
timeout "$TIMEOUT_SECS" "$PODVM" save -o "$WORK/out.tar" "$IMGREF" \
    > "$WORK/s.log" 2>&1 \
    || { cp "$WORK/s.log" "$WORK/last.log"; fail "save failed"; }
[ -f "$WORK/out.tar" ] || fail "save produced no archive"

# Extract just the layer tars from the docker-archive and check /app.txt.
mkdir -p "$WORK/x" || fail "extract dir"
tar -xf "$WORK/out.tar" -C "$WORK/x" || fail "cannot unpack the saved archive"

# Find /app.txt across the layer tars (layers are <hex>/layer.tar).
FOUND=0
OWNER=""
for lt in "$WORK/x"/*/layer.tar; do
    [ -f "$lt" ] || continue
    # --numeric-owner -tvf prints "mode owner/group size name"
    line=$(tar --numeric-owner -tvf "$lt" 2>/dev/null | grep -E ' app\.txt$' || true)
    if [ -n "$line" ]; then
        FOUND=1
        OWNER=$(echo "$line" | awk '{print $2}' | cut -d/ -f1)
        break
    fi
done
[ "$FOUND" = "1" ] || fail "app.txt not found in any saved layer tar"

if [ "$OWNER" != "1000" ]; then
    fail "saved layer tar carries uid $OWNER for /app.txt (want container 1000). Commit/save lost or leaked ownership."
fi

echo "E2E-OWNSAVE PASS: committed+saved /app.txt carries container-uid $OWNER"
exit 0
