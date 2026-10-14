# podvm tests
# Run: make test

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lzstd

# Test binaries
TESTS = test_config test_vsock test_kernel_config test_image test_sha256 test_imgcfg test_userdb test_container test_cli test_build test_json test_store test_vsockalloc test_tarwrite test_store_native test_subuid rf

.PHONY: all test clean

all: $(TESTS)

# Config parsing tests
test_config: test_config.c ../src/strparse.c
	$(CC) $(CFLAGS) -o $@ $< ../src/strparse.c -I../src

# Vsock tests
test_vsock: test_vsock.c
	$(CC) $(CFLAGS) -o $@ $< -lpthread

# Kernel config tests
test_kernel_config: test_kernel_config.c
	$(CC) $(CFLAGS) -o $@ $<

# Image pipeline tests (includes src/image.c; needs curl + zlib + zstd)
test_image: test_image.c ../src/image.c ../src/image.h ../src/json.c ../src/json.h ../src/sha256.c ../src/sha256.h ../src/tar.c ../src/tar.h ../src/store_native.c ../src/store_native.h ../src/subuid.c ../src/subuid.h
	$(CC) $(CFLAGS) -o $@ test_image.c ../src/json.c ../src/sha256.c ../src/tar.c ../src/store_native.c ../src/subuid.c -I../src -lcurl -lz -lzstd

# SHA-256 known-answer tests (no deps)
test_sha256: test_sha256.c ../src/sha256.c ../src/sha256.h
	$(CC) $(CFLAGS) -o $@ test_sha256.c ../src/sha256.c -I../src

# Image-config fold tests (pure logic, no deps)
test_imgcfg: test_imgcfg.c ../src/imgcfg.c ../src/imgcfg.h ../src/image.h ../src/strparse.c ../src/strparse.h
	$(CC) $(CFLAGS) -o $@ test_imgcfg.c ../src/imgcfg.c ../src/strparse.c -I../src

# User resolution tests (pure logic, no deps)
test_userdb: test_userdb.c ../src/userdb.c ../src/userdb.h
	$(CC) $(CFLAGS) -o $@ test_userdb.c ../src/userdb.c -I../src

# Container state tests (name validation, state dir, rm -rf; no deps)
test_container: test_container.c ../src/container.c ../src/container.h ../src/subuid.c ../src/subuid.h
	$(CC) $(CFLAGS) -o $@ test_container.c ../src/container.c ../src/subuid.c -I../src

# Run-CLI parser tests (links the real src/cli.c; docker grammar)
test_cli: test_cli.c ../src/cli.c ../src/cli.h ../src/container.c ../src/strparse.c ../src/subuid.c ../src/subuid.h
	$(CC) $(CFLAGS) -o $@ test_cli.c ../src/cli.c ../src/container.c ../src/strparse.c ../src/subuid.c -I../src

# Build parser tests (pure logic, no deps)
test_build: test_build.c ../src/build.c ../src/build.h ../src/subuid.c ../src/subuid.h
	$(CC) $(CFLAGS) -o $@ test_build.c ../src/build.c ../src/subuid.c -I../src

# JSON parser tests (no deps)
test_json: test_json.c ../src/json.c ../src/json.h
	$(CC) $(CFLAGS) -o $@ test_json.c ../src/json.c -I../src

# Podman storage reader tests (fixture tree via XDG_DATA_HOME; image.c
# pulls in curl+zlib+zstd)
test_store: test_store.c ../src/store.c ../src/store.h ../src/image.c ../src/image.h ../src/json.c ../src/sha256.c ../src/tar.c ../src/tar.h ../src/store_native.c ../src/store_native.h ../src/subuid.c ../src/subuid.h
	$(CC) $(CFLAGS) -o $@ test_store.c ../src/store.c ../src/image.c ../src/json.c ../src/sha256.c ../src/tar.c ../src/store_native.c ../src/subuid.c -I../src -lcurl -lz -lzstd

# Vsock CID/port allocation tests (flock reservation; no deps)
test_vsockalloc: test_vsockalloc.c ../src/vsockalloc.c ../src/vsockalloc.h
	$(CC) $(CFLAGS) -o $@ test_vsockalloc.c ../src/vsockalloc.c -I../src

# Tar pack/extract round-trip (no deps)
test_tarwrite: test_tarwrite.c ../src/tar.c ../src/tar.h ../src/subuid.c ../src/subuid.h
	$(CC) $(CFLAGS) -o $@ test_tarwrite.c ../src/tar.c ../src/subuid.c -I../src

# Native layered store (image.c pulls in curl+zlib+zstd)
test_store_native: test_store_native.c ../src/store_native.c ../src/store_native.h ../src/tar.c ../src/tar.h ../src/sha256.c ../src/json.c ../src/image.c ../src/image.h ../src/subuid.c ../src/subuid.h
	$(CC) $(CFLAGS) -o $@ test_store_native.c ../src/store_native.c ../src/tar.c ../src/sha256.c ../src/json.c ../src/image.c ../src/strparse.c ../src/subuid.c -I../src -lcurl -lz -lzstd

# Container<->host ownership translation (pure arithmetic; no VM/newuidmap)
test_subuid: test_subuid.c ../src/subuid.c ../src/subuid.h
	$(CC) $(CFLAGS) -o $@ test_subuid.c ../src/subuid.c -I../src

# e2e-trap helper: rm -rf a tree that may hold subuid-owned files.
# Reuses the product's subuid_rm_rf; the traps call it because the shell's
# rm cannot unlink through subuid-owned directories.
rf: rf.c ../src/subuid.c ../src/subuid.h
	$(CC) $(CFLAGS) -o $@ rf.c ../src/subuid.c -I../src

# Run all tests
test: $(TESTS)
	@echo "Running tests..."
	@echo ""
	@echo "=== Config Tests ==="
	./test_config || true
	@echo ""
	@echo "=== Vsock Tests ==="
	./test_vsock || true
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
	@echo "=== Container State Tests ==="
	./test_container || true
	@echo ""
	@echo "=== CLI Parser Tests ==="
	./test_cli || true
	@echo ""
	@echo "=== Build Parser Tests ==="
	./test_build || true
	@echo ""
	@echo "=== JSON Parser Tests ==="
	./test_json || true
	@echo ""
	@echo "=== Store Tests ==="
	./test_store || true
	@echo ""
	@echo "=== Vsock Alloc Tests ==="
	./test_vsockalloc || true
	@echo ""
	@echo "=== Tar Write Tests ==="
	./test_tarwrite || true
	@echo ""
	@echo "=== Native Store Tests ==="
	./test_store_native || true
	@echo ""
	@echo "All tests complete!"

# Clean
clean:
	rm -f $(TESTS)
