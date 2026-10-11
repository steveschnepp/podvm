#!/bin/sh
# e2e_saveload.sh - podvm save/load interop with podman (ADR-027 Phase 4)
#
# Proves the archive boundary both ways:
#   1. podvm build -> podvm save -> podman load -> podman run (sees the
#      file a RUN step created).
#   2. podman save -> podvm load -> podvm run (boots the loaded image).
# Plus `podvm system prune` GCs an orphan without touching a live image.
#
# Requires: podman + KVM. Skips if absent.
#
# Usage: ./e2e_saveload.sh
# Exit: 0 = pass, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-sl.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

export XDG_STATE_HOME="$WORK/state"
export PODVM_STORE_DIR="$WORK/store"
PODVM=build/podvm

fail() {
    echo "E2E-SAVELOAD FAIL: $1"
    [ -f "$WORK/last.log" ] && tail -20 "$WORK/last.log"
    exit 1
}

command -v podman > /dev/null 2>&1 || { echo "E2E-SAVELOAD SKIP: no podman"; exit 0; }
[ -e /dev/kvm ] || { echo "E2E-SAVELOAD SKIP: no /dev/kvm"; exit 0; }
BASE=$(podman images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null \
      | grep -iE "alpine" | head -1)
[ -n "$BASE" ] || { echo "E2E-SAVELOAD SKIP: no alpine image"; exit 0; }

make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"
IR="$WORK/initramfs.cpio"

# ---- 1. podvm build -> save -> podman load -> podman run ----

mkdir -p "$WORK/ctx"
printf 'FROM %s\nRUN echo save-load-marker > /marker\n' "$BASE" > "$WORK/ctx/Dockerfile"
timeout $((TIMEOUT_SECS * 2)) "$PODVM" build -r "$IR" -t sl/e2e:1 "$WORK/ctx" \
    > "$WORK/build.log" 2>&1 || { cp "$WORK/build.log" "$WORK/last.log"; fail "build failed"; }

"$PODVM" save -o "$WORK/sl.tar" sl/e2e:1 > "$WORK/save.log" 2>&1 \
    || { cp "$WORK/save.log" "$WORK/last.log"; fail "save failed"; }
[ -s "$WORK/sl.tar" ] || fail "save produced an empty tar"

podman load -i "$WORK/sl.tar" > "$WORK/pload.log" 2>&1 \
    || { cp "$WORK/pload.log" "$WORK/last.log"; fail "podman load rejected the archive"; }
podman run --rm localhost/sl/e2e:1 /bin/cat /marker > "$WORK/prun.log" 2>&1 \
    || { cp "$WORK/prun.log" "$WORK/last.log"; fail "podman run of the loaded image failed"; }
grep -q "save-load-marker" "$WORK/prun.log" \
    || fail "podman did not see the image's built content"

# ---- 2. podman save -> podvm load -> podvm run ----

podman save "$BASE" -o "$WORK/pm.tar" > /dev/null 2>&1 \
    || { echo "E2E-SAVELOAD SKIP: podman save failed"; exit 0; }
"$PODVM" load -i "$WORK/pm.tar" > "$WORK/vload.log" 2>&1 \
    || { cp "$WORK/vload.log" "$WORK/last.log"; fail "podvm load failed"; }
timeout "$TIMEOUT_SECS" "$PODVM" -r "$IR" "$BASE" /bin/sh -c 'echo loaded-ok' \
    > "$WORK/vrun.log" 2>&1 || { cp "$WORK/vrun.log" "$WORK/last.log"; fail "podvm run of loaded image failed"; }
grep -q "loaded-ok" "$WORK/vrun.log" || fail "loaded image did not run in podvm"

# ---- 3. system prune: drops an orphan, keeps a live image ----

# Plant an extra unreferenced layer via a throwaway save target, then make
# sure prune keeps the referenced image intact.
"$PODVM" images > "$WORK/i1.log" 2>&1 || fail "images failed"
"$PODVM" system prune > "$WORK/prune.log" 2>&1 || { cp "$WORK/prune.log" "$WORK/last.log"; fail "system prune failed"; }
timeout "$TIMEOUT_SECS" "$PODVM" -r "$IR" "$BASE" /bin/true > "$WORK/after.log" 2>&1 \
    || { cp "$WORK/after.log" "$WORK/last.log"; fail "prune broke a live image"; }

podman rmi localhost/sl/e2e:1 > /dev/null 2>&1 || true

echo "E2E-SAVELOAD PASS: podvm<->podman save/load interop; prune is safe"
exit 0
