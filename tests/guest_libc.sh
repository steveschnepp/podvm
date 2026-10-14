#!/bin/sh
# guest_libc.sh - The guest init must build under BOTH musl and glibc
#
# The initramfs is a static binary, and podvm's dev tree builds it with
# musl-gcc when present (54KB) and falls back to gcc/glibc (714KB) when
# not. Both are supported shipping configurations, but the suite used to
# build it only ONE way -- whichever the host had -- so the other libc
# could silently rot (a glibc-only call creeping in breaks the musl build
# nobody runs, or vice versa).
#
# This builds the init under each libc in turn, with -Wall -Wextra, and
# fails if either breaks. An implicit declaration (the classic way a
# libc-specific function slips in) is a -Wall warning, so warning-clean
# under both IS the portability guarantee.
#
# Ordering: musl is built LAST, because it is the smaller shipping
# config and the initramfs step that follows packs whatever `guest/init`
# is on disk.
#
# Usage: ./guest_libc.sh
# Exit: 0 = both libcs built (or musl absent + glibc ok), 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

fail() {
    echo "GUEST-LIBC FAIL: $1"
    exit 1
}

command -v gcc > /dev/null 2>&1 || fail "gcc not available"

# --- glibc (always available) ---
make -s -C guest clean > /dev/null 2>&1
make -s -C guest LIBC=glibc > /dev/null 2>&1 \
    || fail "glibc build (gcc -static) failed"
[ -f guest/init ] || fail "glibc build produced no guest/init"
echo "glibc: ok ($(size -B guest/init 2>/dev/null | awk 'NR==2{print $1}' || echo '?') bytes text)"

# --- musl (skipped when musl-gcc is absent) ---
if command -v musl-gcc > /dev/null 2>&1; then
    make -s -C guest clean > /dev/null 2>&1
    make -s -C guest LIBC=musl > /dev/null 2>&1 \
        || fail "musl build (musl-gcc -static) failed"
    [ -f guest/init ] || fail "musl build produced no guest/init"

    # A static musl init must not drag in libc NSS: getpwnam() would
    # dlopen nss modules that do not exist in the initramfs. userdb.c
    # exists precisely to avoid that -- assert the trap stays closed.
    if nm guest/init 2>/dev/null | grep -q "getpwnam\|getgrnam"; then
        fail "static init references NSS lookups (getpwnam/getgrnam)"
    fi
    echo "musl:  ok ($(size -B guest/init 2>/dev/null | awk 'NR==2{print $1}' || echo '?') bytes text); no NSS refs"
else
    echo "musl:  SKIPPED (musl-gcc not installed)"
fi

# Leave the musl binary in place when we built it (the smaller config
# the initramfs step should pack); with musl absent, the glibc build
# above is already the final artifact.
exit 0
