# podvm - Top-level Makefile
# Orchestrates CMake build system
#
# Usage:
#   make            - Build podvm
#   make clean      - Clean build artifacts
#   make test       - Run tests
#   make install    - Install podvm
#   make help       - Show help

BUILD_DIR = build
BUILD_TYPE = Debug
CMAKE_FLAGS = -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

.PHONY: all clean test install help guest initramfs kernel

# Default target
all: $(BUILD_DIR)/podvm

# Build with CMake
$(BUILD_DIR)/podvm: src/main.c src/cli.c src/cli.h src/strparse.c src/image.c src/strparse.h src/image.h src/container.c src/container.h src/vsockalloc.c src/vsockalloc.h src/tar.c src/tar.h src/store_native.c src/store_native.h src/subuid.c src/subuid.h
	@mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake .. $(CMAKE_FLAGS) && make -j$(shell nproc)

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C tests clean
	rm -f guest/init guest/initramfs.cpio
	cd guest && make clean 2>/dev/null || true

# Run tests
test: $(BUILD_DIR)/podvm
	./tests/run_tests.sh

# Install
install: $(BUILD_DIR)/podvm
	cd $(BUILD_DIR) && sudo make install

# Build guest init
guest:
	cd guest && make

# Build initramfs (dev artifact lands in build/ next to the podvm
# binary; podvm auto-resolves it there without -r)
initramfs: guest
	cd guest && ./build-initramfs.sh
	@mkdir -p $(BUILD_DIR)
	cp guest/initramfs.cpio $(BUILD_DIR)/initramfs.cpio

# Build kernel (requires kernel source)
kernel:
	@echo "Kernel build requires kernel source:"
	@echo "  1. Download kernel source to ./linux-x86_64"
	@echo "  2. Run: make kernel-build"
	@echo ""
	@echo "Or manually:"
	@echo "  cd linux-x86_64"
	@echo "  make defconfig"
	@echo "  cp ../kernel/config .config"
	@echo "  make olddefconfig"
	@echo "  make -j\$$(nproc) bzImage"

# Help
help:
	@echo "podvm build system"
	@echo "=================="
	@echo ""
	@echo "Targets:"
	@echo "  make          - Build podvm (default)"
	@echo "  make clean    - Clean build artifacts"
	@echo "  make test     - Run tests"
	@echo "  make install  - Install podvm"
	@echo "  make guest    - Build guest init"
	@echo "  make initramfs- Build initramfs"
	@echo "  make kernel   - Show kernel build instructions"
	@echo "  make help     - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_TYPE=$(BUILD_TYPE)"
	@echo "  BUILD_DIR=$(BUILD_DIR)"
