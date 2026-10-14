#!/bin/sh
# e2e_adr028.sh - ADR-028 acceptance: cross-uid chown + USER work end to end
#
# This is the EXACT Dockerfile shape that broke on 2026-10-12 and forced
# ADR-028: create a non-root user, `chown -R` a directory to it, then run
# as that user. Before ADR-028 the chown failed with "Invalid argument"
# (single-uid userns; the host-side chown was a cross-uid chown nobody had
# permission for), so the dogfood Dockerfile carried a `chown ... || true`
# workaround and files stayed root-owned.
#
# Acceptance: this shape WITHOUT the workaround must (a) build -- the chown
# step completing is itself proof cross-uid chown no longer aborts -- and
# (b) run as the non-root user, which must be able to WRITE to the chowned
# directory. If ownership did not take, the non-root process gets
# "Permission denied" writing to /app and APP-WRITE-OK never prints.
#
# Requires: podman + KVM + a local alpine + a subuid allocation. Skips if
# any is absent (no allocation -> single-uid map -> this is expected to fail,
# which is a supported configuration, just without cross-uid chown).
#
# Usage: ./e2e_adr028.sh
# Exit: 0 = chown+USER work end to end, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-adr028.XXXXXX)
trap '"$SCRIPT_DIR/rf" "$WORK" 2>/dev/null || rm -rf "$WORK"' EXIT

export XDG_STATE_HOME="$WORK/state"
export PODVM_STORE_DIR="$WORK/store"
PODVM=build/podvm
IMGREF="podvm-e2e-adr028:test"

fail() {
    echo "E2E-ADR028 FAIL: $1"
    [ -f "$WORK/last.log" ] && tail -25 "$WORK/last.log"
    exit 1
}

command -v podman > /dev/null 2>&1 || { echo "E2E-ADR028 SKIP: no podman"; exit 0; }
[ -e /dev/kvm ] || { echo "E2E-ADR028 SKIP: no /dev/kvm"; exit 0; }
command -v newuidmap > /dev/null 2>&1 || { echo "E2E-ADR028 SKIP: no newuidmap"; exit 0; }
grep -qE "^$(id -un):" /etc/subuid 2>/dev/null || { echo "E2E-ADR028 SKIP: no subuid allocation"; exit 0; }
# The Dockerfile below uses alpine/busybox user tools (adduser -D,
# addgroup -g), so this test needs an alpine base specifically -- not just
# any alpine|debian image (debian's adduser rejects -D and the step fails).
BASE=$(podman images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null \
       | grep -iE "alpine" | head -1)
[ -n "$BASE" ] || { echo "E2E-ADR028 SKIP: no alpine base image"; exit 0; }

make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"
IR="$WORK/initramfs.cpio"

"$PODVM" rmi "$IMGREF" > /dev/null 2>&1 || true

# The ADR-028 acceptance shape, with NO `|| true` workaround. `id` prints
# the real uid so we assert USER actually took, and the write to /app
# proves the non-root process owns the chowned dir. NB: in this unquoted
# heredoc, `\$(id -u)` (single backslash) escapes the host's command
# substitution so the guest shell -- not the test runner -- evaluates it.
mkdir -p "$WORK/ctx"
cat > "$WORK/ctx/Dockerfile" <<EOF
FROM $BASE
RUN addgroup -g 1000 app && adduser -D -u 1000 -G app app
RUN mkdir -p /app && echo hello > /app/f.txt
RUN chown -R app:app /app
USER app
CMD ["/bin/sh", "-c", "echo write > /app/new.txt && echo UID=\$(id -u) && echo APP-WRITE-OK"]
EOF

# (a) The build must SUCCEED. The chown step completing -- with no
#     `|| true` -- is itself the proof that cross-uid chown no longer
#     aborts the RUN step.
timeout $((TIMEOUT_SECS * 6)) "$PODVM" build -r "$IR" -m 512 -t "$IMGREF" "$WORK/ctx" \
    > "$WORK/b.log" 2>&1 \
    || { cp "$WORK/b.log" "$WORK/last.log"; fail "build failed (cross-uid chown aborting again?)"; }
grep -q "podvm: built $IMGREF" "$WORK/b.log" || fail "build did not report the image"

# (b) Run as USER app. It must write to the chowned /app and report uid 1000.
timeout "$TIMEOUT_SECS" "$PODVM" run -r "$IR" "$IMGREF" > "$WORK/r.log" 2>&1
grep -q "APP-WRITE-OK" "$WORK/r.log" \
    || { cp "$WORK/r.log" "$WORK/last.log"; fail "USER app could not write to the chowned /app (ownership did not take)"; }
grep -q "UID=1000" "$WORK/r.log" \
    || { cp "$WORK/r.log" "$WORK/last.log"; fail "USER app did not run as uid 1000"; }

echo "E2E-ADR028 PASS: cross-uid chown built without || true, USER app (1000) wrote to /app"
exit 0
