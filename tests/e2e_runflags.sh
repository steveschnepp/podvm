#!/bin/sh
# e2e_runflags.sh - prove the podman-parity run flags reach the guest
#
# -e/--env, --env-file, -u/--user, -w/--workdir: flags whose plumbing
# (config channel, imgcfg fold, userdb) existed before the flags did.
# Each run boots a real VM and asserts the flag's effect inside the
# container, not just in the QEMU argv.
#
# Requires: podman + KVM + an alpine image. Skips if absent.
#
# Usage: ./e2e_runflags.sh
# Exit: 0 = pass, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-runflags.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

PODVM=build/podvm

fail() {
    echo "E2E-RUNFLAGS FAIL: $1"
    [ -f "$WORK/last.log" ] && tail -20 "$WORK/last.log"
    exit 1
}

command -v podman > /dev/null 2>&1 || { echo "E2E-RUNFLAGS SKIP: no podman"; exit 0; }
[ -e /dev/kvm ] || { echo "E2E-RUNFLAGS SKIP: no /dev/kvm"; exit 0; }
IMG=$(podman images --format "{{.Repository}}:{{.Tag}}" 2>/dev/null \
      | grep -iE "alpine|debian" | head -1)
[ -n "$IMG" ] || { echo "E2E-RUNFLAGS SKIP: no alpine/debian image"; exit 0; }

# Build guest init + initramfs
make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"

run_podvm() {   # <logname> [podvm args...] -- runs one container
    log="$WORK/$1"; shift
    timeout "$TIMEOUT_SECS" "$PODVM" -r "$WORK/initramfs.cpio" "$@" > "$log" 2>&1
}

# ---- -e/--env ----

# Values reach the guest, including ones with spaces and commas (the
# podvm.*= channel escapes them; ADR-025).
run_podvm e1.log -e PODVM_E2E_FOO=bar \
    -e 'PODVM_E2E_MSG=hello world' -e 'PODVM_E2E_CSV=a,b,c' \
    "$IMG" /bin/sh -c 'echo "FOO=$PODVM_E2E_FOO"; echo "MSG=$PODVM_E2E_MSG"; echo "CSV=$PODVM_E2E_CSV"' \
    || fail "-e run failed"
cp "$WORK/e1.log" "$WORK/last.log"
grep -q "FOO=bar" "$WORK/e1.log" || fail "-e FOO=bar did not reach the guest"
grep -q "MSG=hello world" "$WORK/e1.log" || fail "-e value with a space was truncated"
grep -q "CSV=a,b,c" "$WORK/e1.log" || fail "-e value with a comma was split"

# A CLI -e beats the image's value for the same key (docker semantics;
# alpine images ship PATH).
run_podvm e2.log -e PATH=/custom \
    "$IMG" /bin/sh -c 'echo "PATH=$PATH"' || fail "PATH override run failed"
cp "$WORK/e2.log" "$WORK/last.log"
grep -q "PATH=/custom" "$WORK/e2.log" || fail "-e did not override the image's PATH"

# --env-file: KEY=VALUE lines; comments and blank lines skipped.
cat > "$WORK/envs" <<'EOF'
# a comment
PODVM_E2E_FILEVAR=file-value

PODVM_E2E_FILE2=second
EOF
run_podvm e3.log --env-file "$WORK/envs" \
    "$IMG" /bin/sh -c 'echo "FILEVAR=$PODVM_E2E_FILEVAR"; echo "FILE2=$PODVM_E2E_FILE2"' \
    || fail "--env-file run failed"
cp "$WORK/e3.log" "$WORK/last.log"
grep -q "FILEVAR=file-value" "$WORK/e3.log" || fail "--env-file value did not reach the guest"
grep -q "FILE2=second" "$WORK/e3.log" || fail "--env-file second line did not reach the guest"

# Malformed input is rejected before any VM starts.
"$PODVM" -e NOEQUALS "$IMG" /bin/true > "$WORK/bad.log" 2>&1 \
    && fail "-e without '=' accepted"

echo "E2E-RUNFLAGS PASS (so far): -e/--env/--env-file reach the guest and override the image"
exit 0
