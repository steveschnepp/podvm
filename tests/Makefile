# podvm tests
# Run: make test

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS =

# Test binaries
TESTS = test_config test_host_args test_vsock test_kernel_config test_integration test_image

.PHONY: all test clean

all: $(TESTS)

# Config parsing tests
test_config: test_config.c ../src/strparse.c
	$(CC) $(CFLAGS) -o $@ $< ../src/strparse.c -I../src

# Host argument tests
test_host_args: test_host_args.c
	$(CC) $(CFLAGS) -o $@ $< -I../src

# Vsock tests
test_vsock: test_vsock.c
	$(CC) $(CFLAGS) -o $@ $< -lpthread

# Kernel config tests
test_kernel_config: test_kernel_config.c
	$(CC) $(CFLAGS) -o $@ $<

# Integration tests
test_integration: test_integration.c
	$(CC) $(CFLAGS) -o $@ $<

# Image pipeline tests (includes src/image.c; needs curl + zlib)
test_image: test_image.c ../src/image.c ../src/image.h
	$(CC) $(CFLAGS) -o $@ $< -I../src -lcurl -lz

# Run all tests
test: $(TESTS)
	@echo "Running tests..."
	@echo ""
	@echo "=== Config Tests ==="
	./test_config || true
	@echo ""
	@echo "=== Host Args Tests ==="
	./test_host_args || true
	@echo ""
	@echo "=== Vsock Tests ==="
	./test_vsock || true
	@echo ""
	@echo "=== Integration Tests ==="
	./test_integration || true
	@echo ""
	@echo "=== Image Tests ==="
	./test_image || true
	@echo ""
	@echo "All tests complete!"

# Clean
clean:
	rm -f $(TESTS)
