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

# ---- -u/--user ----

# Numeric uid:gid from the CLI.
run_podvm u1.log -u 1000:1000 "$IMG" /usr/bin/id || fail "-u 1000:1000 run failed"
cp "$WORK/u1.log" "$WORK/last.log"
grep -q "uid=1000" "$WORK/u1.log" || fail "-u 1000 did not reach the guest"
grep -q "gid=1000" "$WORK/u1.log" || fail "-u gid 1000 did not reach the guest"

# A named user, resolved against the CONTAINER's /etc/passwd (userdb,
# after chroot). alpine ships 'nobody' (uid 65534).
run_podvm u2.log -u nobody "$IMG" /usr/bin/id || fail "-u nobody run failed"
cp "$WORK/u2.log" "$WORK/last.log"
grep -q "uid=65534" "$WORK/u2.log" || fail "-u nobody was not resolved in the container"

# A CLI -u beats the image's USER, and the image's USER still applies
# when no -u is given. Throwaway image: alpine + appuser(1234) + USER.
cat > "$WORK/Dockerfile.user" <<'EOF'
FROM docker.io/library/alpine:latest
RUN adduser -D -u 1234 appuser
USER appuser
CMD ["/usr/bin/id"]
EOF
podman build -q -t podvm-e2e-user:test -f "$WORK/Dockerfile.user" "$WORK" \
    > "$WORK/build.log" 2>&1 || fail "cannot build test image"

run_podvm u3.log -u 7:7 podvm-e2e-user:test /usr/bin/id || fail "-u override run failed"
cp "$WORK/u3.log" "$WORK/last.log"
grep -q "uid=7" "$WORK/u3.log" || fail "-u did not override the image's USER"

run_podvm u4.log podvm-e2e-user:test || fail "image-USER run failed"
cp "$WORK/u4.log" "$WORK/last.log"
grep -q "uid=1234" "$WORK/u4.log" || fail "image USER no longer applies without -u"

# ---- -w/--workdir ----

# An existing directory: the command runs there.
run_podvm w1.log -w /etc "$IMG" /bin/pwd || fail "-w /etc run failed"
cp "$WORK/w1.log" "$WORK/last.log"
grep -q "/etc" "$WORK/w1.log" || fail "-w /etc did not land the command in /etc"

# A missing directory: created, like docker's WORKDIR (not silently /).
run_podvm w2.log -w /newdir "$IMG" /bin/pwd || fail "-w /newdir run failed"
cp "$WORK/w2.log" "$WORK/last.log"
grep -q "/newdir" "$WORK/w2.log" || fail "-w did not create the missing directory"

# A relative path is rejected before any VM starts.
"$PODVM" -w relative "$IMG" /bin/true > "$WORK/bad2.log" 2>&1 \
    && fail "-w relative accepted"

echo "E2E-RUNFLAGS PASS: -e/--env/--env-file, -u/--user, -w/--workdir reach the guest"
exit 0
