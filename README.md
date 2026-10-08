# podvm

> VM-based container runtime with minimal attack surface.

---

## What is podvm?

podvm runs containers in lightweight VMs using KVM, providing hardware isolation without the overhead of full VMs.

**Key features:**
- **OCI compatible** - Use existing container images
- **Minimal kernel** - ~2MB synthetic kernel with minimal attack surface
- **Vsock IPC** - Host↔guest communication via virtio-vsock
- **Uncompressed initrd** - Faster boot, less kernel code
- **No guest runtime** - Container runs directly in VM

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  HOST                                                        │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  podvm CLI                                              │ │
│  └─────────────────────────────────────────────────────────┘ │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  virtiofsd                                              │ │
│  └─────────────────────────────────────────────────────────┘ │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  QEMU                                                   │ │
│  └─────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                             │
                    virtiofs │ vsock
                             │
┌─────────────────────────────┼───────────────────────────────┐
│  VM                         │                               │
│  ┌──────────────────────────▼─────────────────────────────┐ │
│  │  Synthetic Kernel (~2MB)                                │ │
│  └──────────────────────────┬─────────────────────────────┘ │
│  ┌──────────────────────────▼─────────────────────────────┐ │
│  │  /init (static)                                          │ │
│  └──────────────────────────┬─────────────────────────────┘ │
│  ┌──────────────────────────▼─────────────────────────────┐ │
│  │  Container Rootfs (via virtiofs)                       │ │
│  └─────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## Requirements

- Linux with KVM support
- QEMU (`qemu-system-x86_64`)
- virtiofsd (shares the container rootfs with the guest)
- libcurl + zlib (host build; registry pull + gzip layers)
- CMake 3.10+
- musl-gcc (optional) for the guest init; falls back to gcc when absent

Optional, for the full VM path:
- `/dev/vhost-vsock` on the host (enables the vsock config channel;
  podvm auto-detects it and falls back to the kernel cmdline otherwise)
- A built kernel + initramfs (see below); without them podvm uses the host
  kernel at `/boot/vmlinuz-$(uname -r)`

## Installation

### Build from source

```bash
# Clone repository
git clone https://github.com/your-org/podvm.git
cd podvm

# Build
make

# Install to system
sudo make install
```

### Install locations

```
/usr/local/bin/podvm              # CLI binary
/var/lib/podvm/kernel/            # Kernel + initramfs
/usr/local/lib/podvm/             # Shared libraries
```

### Build guest components

```bash
# Build guest init (musl-gcc if present, else gcc)
make guest

# Build initramfs
make initramfs
```

### Build kernel (optional)

```bash
# Download kernel source
git clone --depth 1 https://github.com/torvalds/linux.git linux-x86_64

# Build with podvm config
cd linux-x86_64
make defconfig
cp ../kernel/config .config
make olddefconfig
make -j$(nproc) bzImage
cp arch/x86/boot/bzImage ../kernel/bzImage
cd ..
```

## Usage

### Basic usage

```bash
# Run a command in a container
podvm /path/to/rootfs /bin/sh

# Interactive mode
podvm -i /path/to/rootfs /bin/sh

# With custom memory/CPU
podvm -m 256 -c 2 /path/to/rootfs /bin/sh
```

### Options

```
Usage: podvm [options] <image|rootfs-dir> [command] [args...]

Options:
  -i, --interactive   Interactive (attach stdin/stdout)
  -m, --memory <MB>   Memory in MB (default: 128)
  -c, --cpus <cpus>   CPUs (default: 1)
  -k, --kernel <path> Kernel path
  -r, --initramfs <path> Initramfs path
  -h, --help          Show help
```

The first positional argument may be a rootfs directory **or** an image
reference (e.g. `debian:12`, `registry.example.com:5000/foo/bar:v1`). Image
references are pulled from an OCI registry into a local cache
(`/tmp/podvm/images`) on first use and reused afterward.

### Examples

```bash
# Run a simple command
podvm /var/lib/containers/storage/overlay/abc123/merged /bin/echo "Hello"

# Interactive shell
podvm -i /var/lib/containers/storage/overlay/abc123/merged /bin/sh

# Run with arguments
podvm /rootfs /bin/ls -la /tmp

# Run as non-root user
podvm /rootfs /bin/id
```

## Development

### Project structure

```
podvm/
├── src/                    # Host-side code
│   ├── main.c             # CLI entry point
│   ├── strparse.c         # String parsing helpers
│   └── strparse.h
├── guest/                  # Guest-side code
│   ├── init.c             # Guest init
│   ├── Makefile           # Build guest init
│   └── build-initramfs.sh # Build initramfs
├── kernel/                 # Kernel config
│   └── config             # Kernel .config
├── tests/                  # Test suite
│   ├── Makefile
│   ├── run_tests.sh
│   └── test_*.c
├── docs/                   # Documentation
│   ├── architecture.md
│   ├── component-boundaries.md
│   └── ...
├── adrs/                   # Architecture Decision Records
├── Makefile                # Top-level build
├── CMakeLists.txt          # CMake build
└── SPEC.md                 # Full specification
```

### Building for development

```bash
# Build with debug symbols
make BUILD_TYPE=Debug

# Build with optimizations
make BUILD_TYPE=Release

# Clean and rebuild
make clean && make
```

### Running tests

```bash
# Run all tests
make test

# Run specific test
cd tests && make test_config && ./test_config
```

### Code style

- C99 standard
- `-Wall -Wextra` enabled
- Static linking for guest init
- Minimal dependencies

## Architecture Decisions

See [adrs/](adrs/) for detailed architecture decisions:

- **ADR001**: Use VMs for isolation
- **ADR002**: Host-side overlay assembly
- **ADR003**: Musl static guest init
- **ADR007**: Synthetic kernel
- **ADR016**: Vsock IPC
- **ADR019**: Virtiofs with QEMU

## Security

### Attack surface reduction

The kernel config minimizes attack surface:

**Included:**
- Virtio PCI, Console, FS, Balloon
- Vsock
- Namespaces (UTS/IPC/USER/PID/NET)
- Seccomp
- Capabilities
- Security hardening (KASLR, stack protector)

**Excluded:**
- BPF (VM inside kernel)
- Modules (all built-in)
- Tracing/debug subsystems
- Network stack (no guest networking)
- Block devices
- Filesystem drivers (overlay, ext4, etc.)

### Threat model

- **Trust boundary**: Host is trusted, container is untrusted
- **Isolation**: Hardware VM boundary (KVM)
- **IPC**: Vsock (not shared memory)
- **Filesystem**: Virtiofs (host-controlled)

## Documentation

- [SPEC.md](SPEC.md) - Full specification
- [docs/architecture.md](docs/architecture.md) - Architecture overview
- [docs/component-boundaries.md](docs/component-boundaries.md) - Host vs Guest
- [docs/kernel-attack-surface.md](docs/kernel-attack-surface.md) - Security analysis
- [docs/kata-inspiration.md](docs/kata-inspiration.md) - Kata Containers analysis
- [docs/tests.md](docs/tests.md) - Test suite

## Contributing

1. Fork the repository
2. Create a feature branch
3. Add tests for new features
4. Ensure all tests pass
5. Submit a pull request

## License

Apache 2.0

## See also

- [Kata Containers](https://katacontainers.io) - Similar project
- [Firecracker](https://firecracker-microvm.github.io) - AWS microVM
- [Cloud Hypervisor](https://github.com/cloud-hypervisor/cloud-hypervisor) - Rust VMM
- [OCI Runtime Spec](https://github.com/opencontainers/runtime-spec) - Container runtime spec
