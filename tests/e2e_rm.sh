#!/bin/sh
# e2e_rm.sh - prove `podvm rm` and `--rm` manage named-container state
#
# ADR-026 made a named container's writable layer persistent; this proves
# the two ways to remove it again:
#
#   podvm rm <name>   remove state on demand
#   podvm run --rm    remove state when a run ends
#
# Part 1 needs only the podvm binary (no VM): rm mechanics are checked
# against hand-made state dirs. Part 2 boots real VMs and skips without
# podman + KVM + a local image.
#
# Usage: ./e2e_rm.sh
# Exit: 0 = pass, 1 = fail

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-rm.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

# All container state for this test stays inside $WORK.
export XDG_STATE_HOME="$WORK/state"
CONTAINERS="$XDG_STATE_HOME/podvm/containers"
PODVM=build/podvm

fail() {
    echo "E2E-RM FAIL: $1"
    exit 1
}

[ -x "$PODVM" ] || fail "no $PODVM binary (build first)"
mkdir -p "$CONTAINERS"

fake_container() {   # <name>: a state dir shaped like the real thing
    mkdir -p "$CONTAINERS/$1/u" "$CONTAINERS/$1/w"
    echo marker > "$CONTAINERS/$1/u/f"
}

# ---- Part 1: rm mechanics (no VM) ----

fake_container keep
fake_container drop1
fake_container drop2
fake_container drop3

# rm removes the state dir and says so
"$PODVM" rm drop1 > "$WORK/rm1.log" 2>&1 || fail "rm of existing container exited nonzero"
[ ! -e "$CONTAINERS/drop1" ] || fail "state dir survived rm"
grep -q "removed container 'drop1'" "$WORK/rm1.log" || fail "rm did not report the removal"

# rm of a missing container is an error (docker semantics: hiding typos
# behind a zero exit is worse than being strict)
"$PODVM" rm drop1 > "$WORK/rm2.log" 2>&1 && fail "rm of a missing container succeeded"
grep -q "no such container" "$WORK/rm2.log" || fail "missing-container error not reported"

# one bulk rm tries every name and exits nonzero if any failed
"$PODVM" rm drop3 nope drop2 > "$WORK/rm3.log" 2>&1 && fail "bulk rm containing a bad name succeeded"
[ ! -e "$CONTAINERS/drop3" ] || fail "bulk rm skipped a valid name before the bad one"
[ ! -e "$CONTAINERS/drop2" ] || fail "bulk rm skipped a valid name after the bad one"
[ -e "$CONTAINERS/keep" ] || fail "bulk rm touched an unrelated container"

# names that could escape the state root are rejected, and touch nothing
"$PODVM" rm .. > "$WORK/rm4.log" 2>&1 && fail "'rm ..' accepted"
[ -e "$CONTAINERS" ] || fail "'rm ..' deleted the containers root"
[ -e "$CONTAINERS/keep" ] || fail "'rm ..' deleted a container"
"$PODVM" rm a/b > "$WORK/rm5.log" 2>&1 && fail "'rm a/b' accepted"
"$PODVM" rm > "$WORK/rm6.log" 2>&1 && fail "'rm' with no names accepted"

# the same validation guards --name on run (before any VM starts)
"$PODVM" -n .. /nonexistent /bin/true > "$WORK/name1.log" 2>&1 && fail "'--name ..' accepted"
grep -q "invalid container name" "$WORK/name1.log" || fail "--name rejection not reported"

echo "E2E-RM part 1 PASS: rm removes state, errors on missing, rejects escaping names"

# ---- Part 2: --rm lifecycle (real VMs) ----

command -v podman > /dev/null 2>&1 || { echo "E2E-RM part 2 SKIP: no podman"; exit 0; }
[ -e /dev/kvm ] || { echo "E2E-RM part 2 SKIP: no /dev/kvm"; exit 0; }
IMG=$(podman images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null \
      | grep -iE "alpine|debian" | head -1)
[ -n "$IMG" ] || { echo "E2E-RM part 2 SKIP: no alpine/debian image"; exit 0; }

# Build guest init + initramfs
make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"

NAME="podvm-rm-e2e"

# Run 1 (--rm): the command runs, and the state dir is gone when it ends.
timeout "$TIMEOUT_SECS" "$PODVM" -r "$WORK/initramfs.cpio" --rm -n "$NAME" \
    "$IMG" /bin/echo hello > "$WORK/r1.log" 2>&1 || fail "--rm run failed"
grep -q "hello" "$WORK/r1.log" || fail "--rm run did not execute the command"
[ ! -e "$CONTAINERS/$NAME" ] || fail "--rm left the state dir behind"

# Run 2 (--rm without --name): a no-op; the run itself still works.
timeout "$TIMEOUT_SECS" "$PODVM" -r "$WORK/initramfs.cpio" --rm \
    "$IMG" /bin/echo hello > "$WORK/r2.log" 2>&1 || fail "--rm without --name failed"
grep -q "hello" "$WORK/r2.log" || fail "--rm without --name did not execute"

# Run 3 (named, no --rm): state persists -- so run 1's cleanup was --rm's
# doing, not an accident of the harness.
timeout "$TIMEOUT_SECS" "$PODVM" -r "$WORK/initramfs.cpio" -n "$NAME" \
    "$IMG" /bin/sh -c "echo kept > /kept.txt" > "$WORK/r3.log" 2>&1 \
    || fail "named run failed"
[ -e "$CONTAINERS/$NAME/u/kept.txt" ] || fail "named run did not persist state"

# ...and `podvm rm` removes a real container's state, closing the loop.
"$PODVM" rm "$NAME" > "$WORK/rm7.log" 2>&1 || fail "rm of a real container failed"
[ ! -e "$CONTAINERS/$NAME" ] || fail "rm did not remove the real container's state"

echo "E2E-RM part 2 PASS: --rm discarded state on exit, rm removed a real container"
exit 0
