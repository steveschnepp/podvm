#!/bin/sh
# cli_matrix.sh - drive the REAL binary's CLI surface, no VM needed
#
# Replaces the old test_host_args.c, which tested a hand-copied mock of
# main.c's parser that had drifted from it (it still knew -it, which the
# real getopt has never accepted). The one regression this exists to
# catch: a letter dropped from the getopt optstring, which no e2e can
# see because the e2es only ever pass flags correctly.
#
# Strategy: every run uses a nonexistent *path* positional, so it fails
# hermetically at image resolution ("directory not found") -- after
# getopt, before any podman/network/VM involvement. A run that fails
# with "invalid option" or "Usage:" instead means parsing broke.
#
# Requires: build/podvm. No KVM, no podman, no network.
# Exit: 0 = pass, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

PODVM=build/podvm
WORK=$(mktemp -d /tmp/podvm-cli-matrix.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

# State-touching subcommands (rm/stop/logs) must not see the real ~/.local.
export XDG_STATE_HOME="$WORK/state"

[ -x "$PODVM" ] || { echo "CLI-MATRIX SKIP: no $PODVM (build first)"; exit 0; }

PASS=0
FAIL=0
BOGUS=/nonexistent-podvm-cli-test

ok()   { PASS=$((PASS + 1)); }
bad()  { echo "CLI-MATRIX FAIL: $1"; shift; for f in "$@"; do echo "  --- $f"; cat "$f"; done; FAIL=$((FAIL + 1)); }

# expect_fail <name> <expected-msg-substring> <args...>
# Must exit non-zero AND print the expected message.
expect_fail() {
    name="$1"; want="$2"; shift 2
    out="$WORK/out"
    if "$PODVM" "$@" > "$out" 2>&1; then
        bad "$name: exited 0, wanted failure" "$out"
    elif ! grep -q -e "$want" "$out"; then
        bad "$name: missing message '$want'" "$out"
    else
        ok
    fi
}

# ---- help and parse-level errors ----

if "$PODVM" --help > "$WORK/out" 2>&1 && grep -q "Usage:" "$WORK/out"; then
    ok
else
    bad "--help must exit 0 with usage" "$WORK/out"
fi

expect_fail "no args"          "Usage:"      # no positional at all
expect_fail "unknown -x"       "invalid option"      -x "$BOGUS"
# -it used to be a "phantom" (mock-parser fossil); -t is now real
# (docker compat: the serial console is a tty) -- pinned below instead.
expect_fail "-y unknown"       "invalid option"      -y "$BOGUS"
expect_fail "-m missing value" "requires an argument" -m

# ---- the optstring matrix: THE regression guard ----
# Every short flag in main.c's optstring, in one invocation that parses
# cleanly and fails only at resolution. If any letter is dropped from
# the optstring, getopt reports "invalid option" and this fails.
printf 'CLIMTX_A=1\n' > "$WORK/envfile"
"$PODVM" -i -t -m 256 -c 2 -k "$WORK/kernel" -r "$WORK/initramfs" \
    -n climtx -e K=V -e 'MSG=a b,c' --env-file "$WORK/envfile" \
    -u root -w / -p 127.0.0.1:9999:80 --network slirp \
    -v "$WORK:/mnt:ro" --tmpfs /clitmpfs --hostname climtx --read-only \
    "$BOGUS" /bin/true > "$WORK/out" 2>&1
rc=$?
if [ $rc -eq 0 ]; then
    bad "optstring matrix: exited 0 on nonexistent path" "$WORK/out"
elif grep -q "invalid option\|requires an argument" "$WORK/out"; then
    bad "optstring matrix: a flag was not accepted (getopt drift)" "$WORK/out"
elif ! grep -q "directory not found" "$WORK/out"; then
    bad "optstring matrix: did not fail at resolution" "$WORK/out"
else
    ok
fi

# ---- per-flag host-side validation ----

expect_fail "relative -w"      "must be an absolute path" -w relative "$BOGUS"
expect_fail "-n .."            "invalid container name"   -n .. "$BOGUS"
expect_fail "-n with slash"    "invalid container name"   -n a/b "$BOGUS"
expect_fail "-e without ="     "must be KEY=VALUE"        -e NOEQUALS "$BOGUS"
expect_fail "--network host"   "slirp or none"            --network host "$BOGUS"
expect_fail "bad -p spec"      "invalid -p"               -p garbage "$BOGUS"
expect_fail "-v missing host"  "not a directory"          -v /nonexistent-h:/mnt "$BOGUS"
expect_fail "-p with none"     "requires networking"      -p 8080:80 --network none "$BOGUS"

# ---- detach rules ----

expect_fail "-d without name"  "requires --name"                    -d "$BOGUS"
expect_fail "-d with --rm"     "cannot be combined with --rm"       -d -n climtx --rm "$BOGUS"
expect_fail "-d with -i"       "cannot be combined with --interactive" -d -n climtx -i "$BOGUS"

# ---- subcommands (state isolated under $WORK) ----

expect_fail "rm no args"    "Usage:"                  rm
expect_fail "stop no args"  "Usage:"                  stop
expect_fail "logs no args"  "Usage:"                  logs
expect_fail "rmi no args"   "Usage:"                  rmi
expect_fail "build no -t"   "-t <name:tag> is required" build
expect_fail "stop unknown"  "no such container"       stop climtx-nope
expect_fail "logs unknown"  "no log for container"    logs climtx-nope
expect_fail "logs 2 names"  "takes a single container name" logs a b

for sub in rm stop logs rmi; do
    if "$PODVM" "$sub" --help > "$WORK/out" 2>&1; then
        ok
    else
        bad "$sub --help must exit 0" "$WORK/out"
    fi
done

if "$PODVM" images > "$WORK/out" 2>&1; then
    ok
else
    bad "images on an empty cache must exit 0" "$WORK/out"
fi

echo ""
echo "CLI matrix: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || exit 1
exit 0
