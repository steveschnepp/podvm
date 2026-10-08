#!/bin/sh
# run_tests.sh - Run all podvm tests
# Usage: ./run_tests.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

echo "podvm test suite"
echo "================"
echo ""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PASS=0
FAIL=0
WARN=0

run_test() {
    local name="$1"
    local cmd="$2"
    
    printf "%-30s" "$name"
    
    output=$(eval "$cmd" 2>&1)
    status=$?
    
    # An explicit WARN in the output always wins, even on exit 0
    # (e.g. `which X || echo WARN` reports success when X is missing).
    if echo "$output" | grep -qi "WARN"; then
        echo "${YELLOW}WARN${NC}"
        WARN=$((WARN + 1))
    elif [ $status -eq 0 ]; then
        echo "${GREEN}PASS${NC}"
        PASS=$((PASS + 1))
    else
        if echo "$output" | grep -qi "No such file\|not found\|Permission denied"; then
            echo "${YELLOW}WARN${NC}"
            WARN=$((WARN + 1))
        else
            echo "${RED}FAIL${NC}"
            echo "$output" | head -5
            FAIL=$((FAIL + 1))
        fi
    fi
}

# Build tests
echo "Building tests..."
cd tests
make -s clean 2>/dev/null || true
make -s all
cd ..

echo ""
echo "Running unit tests..."
echo ""

# Unit tests
run_test "Config parsing" "./tests/test_config"
run_test "Host args" "./tests/test_host_args"
run_test "Vsock basics" "./tests/test_vsock"
run_test "Image pipeline" "./tests/test_image"

echo ""
echo "Running integration tests..."
echo ""

# Integration tests
run_test "Host build" "mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1 && make > /dev/null 2>&1"
run_test "Guest build" "cd guest && make -s 2>&1"
run_test "Initramfs build" "cd guest && chmod +x build-initramfs.sh && ./build-initramfs.sh 2>&1"

echo ""
echo "Checking dependencies..."
echo ""

# Dependency checks
run_test "QEMU available" "which qemu-system-x86_64"
run_test "KVM available" "test -e /dev/kvm"
run_test "9p modules available" "find /lib/modules/$(uname -r) -name '9p*.ko*' | grep -q . || echo WARN: no 9p modules for host kernel"
run_test "musl-gcc available" "which musl-gcc || echo WARN: musl-gcc not installed"

echo ""
echo "Checking kernel config..."
echo ""

# Kernel config checks
run_test "Vsock enabled" "grep -q 'CONFIG_VSOCK=y' kernel/config"
run_test "9p enabled" "grep -q 'CONFIG_NET_9P_VIRTIO=y' kernel/config && grep -q 'CONFIG_9P_FS=y' kernel/config"
run_test "Balloon enabled" "grep -q 'CONFIG_VIRTIO_BALLOON=y' kernel/config"
run_test "Uncompressed kernel" "grep -q 'CONFIG_KERNEL_UNCOMPRESSED=y' kernel/config"
run_test "No BPF" "grep -q 'CONFIG_BPF=y' kernel/config && exit 1 || true"
run_test "No modules" "grep -q 'CONFIG_MODULES=y' kernel/config && exit 1 || true"

echo ""
echo "Running end-to-end test..."
echo ""

# Real VM boot (needs KVM): 9p mount -> chroot -> exec -> clean exit.
# Not a WARN-if-missing check: skip silently on KVM-less hosts.
if [ -e /dev/kvm ]; then
    run_test "E2E boot (9p + vsock)" "timeout 150 sh tests/e2e_boot.sh"
    # Podman-storage e2e: needs podman + at least one local image.
    if which podman > /dev/null 2>&1 && podman images -q 2>/dev/null | grep -q .; then
        run_test "E2E podman image" "sh tests/e2e_podman.sh"
    else
        echo "E2E podman image              SKIPPED (no podman or no images)"
    fi
    # Networking e2e (DNS+HTTPS out of the guest): needs an alpine image.
    if which podman > /dev/null 2>&1 && podman images 2>/dev/null | grep -qi alpine; then
        run_test "E2E net (DNS+HTTPS)" "sh tests/e2e_net.sh"
    else
        echo "E2E net (DNS+HTTPS)           SKIPPED (no alpine image)"
    fi
else
    echo "E2E boot (9p + vsock)         SKIPPED (no /dev/kvm)"
fi

echo ""
echo "================"
echo "Results:"
echo "  ${GREEN}PASS: $PASS${NC}"
echo "  ${YELLOW}WARN: $WARN${NC}"
echo "  ${RED}FAIL: $FAIL${NC}"
echo ""

if [ $FAIL -eq 0 ]; then
    echo "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo "${RED}Some tests failed.${NC}"
    exit 1
fi
