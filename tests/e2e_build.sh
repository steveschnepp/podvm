#!/bin/sh
# e2e_build.sh - podvm build: Dockerfile -> runnable cached image
#
# Builds a throwaway image exercising the whole pipeline -- FROM
# (podman chain), ENV/WORKDIR metadata, two RUN steps (each a real VM;
# the second deletes a base-layer file), two COPYs, a shell-form CMD --
# then runs it from the cache and asserts every piece survived,
# including the whiteout: a file removed in step 2 must not exist in
# the image. A second, failing Dockerfile must abort the build and
# leave no image and no stray step state.
#
# Requires: podman + KVM. Skips if absent.
#
# Usage: ./e2e_build.sh
# Exit: 0 = pass, 1 = failure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

TIMEOUT_SECS="${PODVM_E2E_TIMEOUT:-60}"
WORK=$(mktemp -d /tmp/podvm-e2e-build.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

export XDG_STATE_HOME="$WORK/state"
# Isolate the native layered store (the build now writes there) so the
# test is hermetic and does not touch the user's real store.
export PODVM_STORE_DIR="$WORK/store"
PODVM=build/podvm
IMGREF="podvm-e2e-build:test"

fail() {
    echo "E2E-BUILD FAIL: $1"
    [ -f "$WORK/last.log" ] && tail -25 "$WORK/last.log"
    exit 1
}

command -v podman > /dev/null 2>&1 || { echo "E2E-BUILD SKIP: no podman"; exit 0; }
[ -e /dev/kvm ] || { echo "E2E-BUILD SKIP: no /dev/kvm"; exit 0; }

make -s -C guest || fail "guest build"
guest/build-initramfs.sh "$WORK/initramfs.cpio" > /dev/null || fail "initramfs"
IR="$WORK/initramfs.cpio"

# Clean slate: an image left by an earlier run must not mask a failure
# (the first version of this test built the failing image "successfully").
"$PODVM" rmi podvm-e2e-build:test podvm-e2e-fail:test > /dev/null 2>&1 || true

# ---- the build ----

mkdir -p "$WORK/ctx/sub"
echo "cdata-content" > "$WORK/ctx/cdata.txt"
echo "sub-content" > "$WORK/ctx/sub/s.txt"
cat > "$WORK/ctx/Dockerfile" <<'EOF'
FROM docker.io/library/alpine:latest
ENV GREETING=hello
WORKDIR /app
RUN echo step1 > /f1
RUN rm /etc/hostname && cp /f1 /app/copied
COPY cdata.txt /app/cdata.txt
COPY sub /app/sub
CMD ["/bin/sh", "-c", "cat /app/copied /app/cdata.txt /app/sub/s.txt; echo $GREETING; pwd"]
EOF

timeout $((TIMEOUT_SECS * 6)) "$PODVM" build -r "$IR" -t "$IMGREF" "$WORK/ctx" \
    > "$WORK/b.log" 2>&1 \
    || { cp "$WORK/b.log" "$WORK/last.log"; fail "build failed"; }
grep -q "podvm: built podvm-e2e-build:test" "$WORK/b.log" \
    || fail "build did not report the image"
grep -q "step 3: RUN" "$WORK/b.log" || fail "RUN steps were not executed"

# ---- listing ----

"$PODVM" images > "$WORK/i1.log" 2>&1 || fail "images exited non-zero"
grep -Eq '^podvm-e2e-build +test' "$WORK/i1.log" \
    || fail "built image not listed by podvm images"

# ---- the image runs, with its config ----

timeout "$TIMEOUT_SECS" "$PODVM" -r "$IR" "$IMGREF" > "$WORK/r1.log" 2>&1 \
    || { cp "$WORK/r1.log" "$WORK/last.log"; fail "running the built image failed"; }
grep -q "step1" "$WORK/r1.log" || fail "RUN step 1 output missing from the image"
grep -q "cdata-content" "$WORK/r1.log" || fail "COPY file missing from the image"
grep -q "sub-content" "$WORK/r1.log" || fail "COPY dir missing from the image"
grep -q "hello" "$WORK/r1.log" || fail "ENV did not reach the built image"
grep -q "/app" "$WORK/r1.log" || fail "WORKDIR did not reach the built image"

# ---- the whiteout: /etc/hostname was deleted in step 2 ----

timeout "$TIMEOUT_SECS" "$PODVM" -r "$IR" "$IMGREF" \
    /bin/sh -c 'test -e /etc/hostname && echo STILL || echo GONE' \
    > "$WORK/r2.log" 2>&1 \
    || { cp "$WORK/r2.log" "$WORK/last.log"; fail "whiteout check run failed"; }
grep -q "GONE" "$WORK/r2.log" || fail "a file deleted in a RUN step survived the build"

# ---- multi-stage: FROM..AS + COPY --from ----

# Isolation invariants: only the copied file crosses stages; the
# builder's extra files AND its whiteouts must not. The checks ride on
# the exit code (a marker string would also appear in the serial log's
# kernel cmdline echo of the command itself).
mkdir -p "$WORK/ctx3"
cat > "$WORK/ctx3/Dockerfile" <<'EOF'
FROM docker.io/library/alpine:latest AS builder
RUN echo ms-artifact > /artifact.txt && touch /builder-only
RUN rm /etc/hostname
FROM docker.io/library/alpine:latest
COPY --from=builder /artifact.txt /final.txt
CMD ["/bin/sh", "-c", "cat /final.txt; test ! -e /builder-only -a -e /etc/hostname"]
EOF
timeout $((TIMEOUT_SECS * 6)) "$PODVM" build -r "$IR" -t podvm-e2e-ms:test "$WORK/ctx3" \
    > "$WORK/b3.log" 2>&1 \
    || { cp "$WORK/b3.log" "$WORK/last.log"; fail "multi-stage build failed"; }
grep -q "stage 2: FROM" "$WORK/b3.log" || fail "the second stage was not started"
grep -q "COPY --from=builder" "$WORK/b3.log" || fail "COPY --from step missing"
timeout "$TIMEOUT_SECS" "$PODVM" -r "$IR" podvm-e2e-ms:test > "$WORK/r3.log" 2>&1 \
    || { cp "$WORK/r3.log" "$WORK/last.log"; fail "running the multi-stage image failed"; }
grep -q "ms-artifact" "$WORK/r3.log" || fail "COPY --from did not carry the artifact"

"$PODVM" rmi podvm-e2e-ms:test > /dev/null 2>&1 || true

# ---- a failing RUN aborts the build and leaves nothing ----

mkdir -p "$WORK/ctx2"
cat > "$WORK/ctx2/Dockerfile" <<'EOF'
FROM docker.io/library/alpine:latest
RUN echo about-to-fail
RUN /bin/false
EOF
timeout "$TIMEOUT_SECS" "$PODVM" build -r "$IR" -t podvm-e2e-fail:test "$WORK/ctx2" \
    > "$WORK/b2.log" 2>&1 \
    && fail "a failing RUN did not fail the build"
"$PODVM" images > "$WORK/i2.log" 2>&1
grep -q "podvm-e2e-fail" "$WORK/i2.log" && fail "the failed build left an image behind"
if ls "$XDG_STATE_HOME/podvm/containers" 2>/dev/null | grep -q '^pb'; then
    fail "the failed build left step container state behind"
fi

# ---- rmi manages build output (one cache) ----

"$PODVM" rmi podvm-e2e-build:test > "$WORK/rmi.log" 2>&1 \
    || { cp "$WORK/rmi.log" "$WORK/last.log"; fail "rmi of the built image failed"; }
"$PODVM" images > "$WORK/i3.log" 2>&1
grep -Eq '^podvm-e2e-build +test' "$WORK/i3.log" && fail "built image still listed after rmi"

echo "E2E-BUILD PASS: Dockerfile -> runnable image; whiteouts, COPY, config, rmi all hold"
exit 0
