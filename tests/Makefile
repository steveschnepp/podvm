# podvm tests
# Run: make test

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lzstd

# Test binaries
TESTS = test_config test_host_args test_vsock test_kernel_config test_integration test_image test_sha256 test_imgcfg test_userdb

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

# Image pipeline tests (includes src/image.c; needs curl + zlib + zstd)
test_image: test_image.c ../src/image.c ../src/image.h ../src/json.c ../src/json.h ../src/sha256.c ../src/sha256.h
	$(CC) $(CFLAGS) -o $@ test_image.c ../src/json.c ../src/sha256.c -I../src -lcurl -lz -lzstd

# SHA-256 known-answer tests (no deps)
test_sha256: test_sha256.c ../src/sha256.c ../src/sha256.h
	$(CC) $(CFLAGS) -o $@ test_sha256.c ../src/sha256.c -I../src

# Image-config fold tests (pure logic, no deps)
test_imgcfg: test_imgcfg.c ../src/imgcfg.c ../src/imgcfg.h ../src/image.h
	$(CC) $(CFLAGS) -o $@ test_imgcfg.c ../src/imgcfg.c -I../src

# User resolution tests (pure logic, no deps)
test_userdb: test_userdb.c ../src/userdb.c ../src/userdb.h
	$(CC) $(CFLAGS) -o $@ test_userdb.c ../src/userdb.c -I../src

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
	@echo "=== SHA-256 Tests ==="
	./test_sha256 || true
	@echo ""
	@echo "=== Image Config Fold Tests ==="
	./test_imgcfg || true
	@echo ""
	@echo "=== User DB Tests ==="
	./test_userdb || true
	@echo ""
	@echo "All tests complete!"

# Clean
clean:
	rm -f $(TESTS)
